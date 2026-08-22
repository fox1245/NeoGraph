#!/usr/bin/env python3
"""Check that every owned English Markdown document has synchronized twins."""

from __future__ import annotations

import argparse
import hashlib
import json
import posixpath
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / "docs" / "i18n-manifest.json"
TRANSLATION_SUFFIXES = (".ko.md", ".ja.md", ".zh-CN.md")
LANGUAGE_LABELS = {
    "en": "English",
    "ko": "한국어",
    "ja": "日本語",
    "zh-CN": "简体中文",
}
HEADER_RE = re.compile(
    r"^<!-- neograph-i18n: source=(\S+) locale=(\S+) source_sha256=([0-9a-f]{64}) -->$"
)
HEADING_RE = re.compile(r"^(#{1,6})\s+", re.MULTILINE)
FENCE_RE = re.compile(r"^```([^\n]*)\n(.*?)^```\s*$", re.MULTILINE | re.DOTALL)
TABLE_DIVIDER_RE = re.compile(r"^\s*\|?(?:\s*:?-+:?\s*\|)+\s*$", re.MULTILINE)
LINK_TARGET_RE = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)")
HTML_TARGET_RE = re.compile(r"\b(?:href|src)=[\"']([^\"']+)[\"']")
EXPLICIT_ANCHOR_RE = re.compile(r"<a\s+(?:id|name)=[\"']([^\"']+)[\"']\s*></a>")
MARKDOWN_HEADING_RE = re.compile(r"^(#{1,6})\s+(.+?)\s*$")


def tracked_markdown() -> list[str]:
    result = subprocess.run(
        [
            "git",
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "--",
            "*.md",
        ],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    return [line for line in result.stdout.splitlines() if line]


def load_manifest() -> dict:
    with MANIFEST_PATH.open(encoding="utf-8") as handle:
        manifest = json.load(handle)
    if manifest.get("schema_version") != 1:
        raise ValueError("docs/i18n-manifest.json must use schema_version 1")
    return manifest


def default_translation_path(source: str, locale: str) -> str:
    path = PurePosixPath(source)
    return str(path.with_name(f"{path.stem}.{locale}{path.suffix}"))


def translation_paths(source: str, manifest: dict) -> dict[str, str]:
    overrides = manifest.get("path_overrides", {}).get(source, {})
    return {
        locale: overrides.get(locale, default_translation_path(source, locale))
        for locale in manifest["translation_locales"]
    }


def navigation_line(source: str, current: str, manifest: dict) -> str:
    paths = {"en": source, **translation_paths(source, manifest)}
    parent = str(PurePosixPath(current).parent)
    if parent == ".":
        parent = ""
    links = []
    for locale in ("en", *manifest["translation_locales"]):
        target = posixpath.relpath(paths[locale], start=parent or ".")
        links.append(f"[{LANGUAGE_LABELS[locale]}]({target})")
    return "**Languages:** " + " | ".join(links)


def add_navigation(text: str, navigation: str) -> str:
    if navigation in text:
        return text
    lines = text.splitlines()
    html_heading = next(
        (index for index, line in enumerate(lines) if "<h1" in line),
        None,
    )
    heading = html_heading
    if heading is not None:
        heading = next(
            (
                index
                for index in range(heading, len(lines))
                if lines[index].strip() == "</p>"
            ),
            heading,
        )
    else:
        in_fence = False
        for index, line in enumerate(lines):
            if line.startswith("```"):
                in_fence = not in_fence
            elif not in_fence and line.startswith("# "):
                heading = index
                break
    if heading is None:
        raise ValueError("document has no level-1 Markdown or HTML heading")
    tail = heading + 1
    if tail < len(lines) and not lines[tail]:
        tail += 1
    lines[tail:tail] = [navigation, ""]
    rendered = "\n".join(lines)
    return rendered + ("\n" if text.endswith("\n") else "")


def normalize_navigation(text: str, navigation: str) -> str:
    """Replace translated/stale language labels while preserving link targets."""
    targets = re.findall(r"\(([^)]+)\)", navigation)
    lines = text.splitlines()
    matches = [
        index
        for index, line in enumerate(lines)
        if targets and all(f"({target})" in line for target in targets)
    ]
    if not matches:
        return add_navigation(text, navigation)
    lines[matches[0]] = navigation
    for index in reversed(matches[1:]):
        del lines[index]
    rendered = "\n".join(lines)
    return rendered + ("\n" if text.endswith("\n") else "")


def synchronize_metadata(
    source: str, locale: str, translated: str, manifest: dict
) -> None:
    source_text = (ROOT / source).read_text(encoding="utf-8")
    path = ROOT / translated
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    if lines and HEADER_RE.match(lines[0]):
        lines = lines[1:]
        if lines and not lines[0]:
            lines = lines[1:]
        text = "\n".join(lines) + ("\n" if text.endswith("\n") else "")
    text = normalize_navigation(text, navigation_line(source, translated, manifest))
    header = (
        f"<!-- neograph-i18n: source={source} locale={locale} "
        f"source_sha256={sha256(source_text)} -->"
    )
    path.write_text(f"{header}\n{text}", encoding="utf-8")


def markdown_headings(text: str) -> list[tuple[int, int, str]]:
    headings = []
    in_fence = False
    for index, line in enumerate(text.splitlines()):
        if line.startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        match = MARKDOWN_HEADING_RE.match(line)
        if match:
            headings.append((index, len(match.group(1)), match.group(2)))
    return headings


def link_targets(text: str) -> list[str]:
    lines = []
    in_fence = False
    for line in text.splitlines():
        if line.startswith("```"):
            in_fence = not in_fence
            continue
        if not in_fence:
            lines.append(line)
    prose = "\n".join(lines)
    return LINK_TARGET_RE.findall(prose) + HTML_TARGET_RE.findall(prose)


def github_heading_slugs(text: str) -> list[str]:
    # GitHub preserves underscores, removes punctuation, and suffixes duplicates.
    # https://github.com/Flet/github-slugger/blob/master/index.js (2026-07-25)
    counts: dict[str, int] = {}
    slugs = []
    for _, _, heading in markdown_headings(text):
        heading = re.sub(r"<[^>]+>", "", heading)
        heading = re.sub(r"!?\[([^\]]+)\]\([^)]+\)", r"\1", heading)
        heading = heading.replace("`", "").replace("*", "")
        heading = re.sub(r"[^\w\s-]", "", heading.lower(), flags=re.UNICODE)
        base = re.sub(r"\s", "-", heading).strip("-")
        suffix = counts.get(base, 0)
        counts[base] = suffix + 1
        slugs.append(base if suffix == 0 else f"{base}-{suffix}")
    return slugs


def add_source_heading_anchors(source_text: str, translated_text: str) -> str:
    source_slugs = github_heading_slugs(source_text)
    translated_headings = markdown_headings(translated_text)
    translated_slugs = github_heading_slugs(translated_text)
    if len(source_slugs) != len(translated_headings):
        raise ValueError("source and translation heading counts differ")

    explicit = set(EXPLICIT_ANCHOR_RE.findall(translated_text))
    needed = {
        target[1:]
        for target in link_targets(translated_text)
        if target.startswith("#")
    }
    lines = translated_text.splitlines()
    insertions = []
    for source_slug, translated_slug, (line, _, _) in zip(
        source_slugs, translated_slugs, translated_headings
    ):
        if (
            source_slug
            and source_slug in needed
            and source_slug != translated_slug
            and source_slug not in explicit
        ):
            insertions.append((line, f'<a id="{source_slug}"></a>'))
            explicit.add(source_slug)
    for line, anchor in reversed(insertions):
        lines.insert(line, anchor)
    rendered = "\n".join(lines)
    return rendered + ("\n" if translated_text.endswith("\n") else "")


def canonical_sources(files: list[str], manifest: dict) -> list[str]:
    excluded = set(manifest.get("excluded", {}))
    overridden_translations = {
        path
        for translations in manifest.get("path_overrides", {}).values()
        for path in translations.values()
    }
    sources = []
    for path in files:
        if path in excluded or path in overridden_translations:
            continue
        if path.endswith(TRANSLATION_SUFFIXES):
            continue
        sources.append(path)
    return sources


def sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def structure(text: str) -> dict:
    return {
        "heading_levels": [len(match.group(1)) for match in HEADING_RE.finditer(text)],
        "code_blocks": [
            (match.group(1).strip(), match.group(2)) for match in FENCE_RE.finditer(text)
        ],
        "table_count": len(TABLE_DIVIDER_RE.findall(text)),
        "link_targets": link_targets(text),
    }


def unresolved_internal_links(
    document: str, text: str, link_targets: list[str]
) -> list[str]:
    unresolved = set()
    cached_text = {document: text}
    cached_anchors = {}

    for target in link_targets:
        parsed = urlsplit(target)
        if parsed.scheme or parsed.netloc or parsed.path.startswith("/"):
            continue
        if not parsed.fragment:
            continue
        if parsed.path and PurePosixPath(unquote(parsed.path)).suffix.lower() not in {
            ".md",
            ".markdown",
        }:
            continue

        target_path = document
        if parsed.path:
            parent = str(PurePosixPath(document).parent)
            target_path = posixpath.normpath(
                posixpath.join(parent, unquote(parsed.path))
            )

        path = (ROOT / target_path).resolve()
        try:
            path.relative_to(ROOT)
        except ValueError:
            continue
        if not path.exists():
            unresolved.add(target)
            continue
        if not path.is_file():
            continue

        if target_path not in cached_anchors:
            target_text = cached_text.get(target_path)
            if target_text is None:
                target_text = path.read_text(encoding="utf-8")
                cached_text[target_path] = target_text
            anchors = set(github_heading_slugs(target_text))
            anchors.update(EXPLICIT_ANCHOR_RE.findall(target_text))
            cached_anchors[target_path] = anchors
        if unquote(parsed.fragment) not in cached_anchors[target_path]:
            unresolved.add(target)

    return sorted(unresolved)


def validate_translation(
    source: str, locale: str, translated: str, manifest: dict
) -> list[str]:
    errors = []
    source_text = (ROOT / source).read_text(encoding="utf-8")
    translated_text = (ROOT / translated).read_text(encoding="utf-8")
    first_line = translated_text.splitlines()[0] if translated_text else ""
    header = HEADER_RE.match(first_line)
    expected_hash = sha256(source_text)
    if not header:
        errors.append(f"{translated}: missing neograph-i18n metadata header")
    elif (header.group(1), header.group(2), header.group(3)) != (
        source,
        locale,
        expected_hash,
    ):
        errors.append(f"{translated}: source path, locale, or SHA256 is stale")

    source_shape = structure(source_text)
    translated_shape = structure(translated_text)
    if source_shape["heading_levels"] != translated_shape["heading_levels"]:
        errors.append(f"{translated}: heading level sequence differs from {source}")
    if source_shape["code_blocks"] != translated_shape["code_blocks"]:
        errors.append(f"{translated}: fenced code blocks differ from {source}")
    if source_shape["table_count"] != translated_shape["table_count"]:
        errors.append(f"{translated}: table count differs from {source}")
    if source_shape["link_targets"] != translated_shape["link_targets"]:
        errors.append(f"{translated}: ordered link targets differ from {source}")
    navigation = navigation_line(source, translated, manifest)
    if navigation not in translated_text:
        errors.append(f"{translated}: missing or incorrect language navigation")
    unresolved = unresolved_internal_links(
        translated, translated_text, translated_shape["link_targets"]
    )
    if unresolved:
        errors.append(
            f"{translated}: unresolved internal links: {', '.join(unresolved)}"
        )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--strict",
        action="store_true",
        help="fail when any translation is missing or stale, regardless of manifest mode",
    )
    parser.add_argument(
        "--write-navigation",
        action="store_true",
        help="insert the required language navigation into English sources",
    )
    parser.add_argument(
        "--write-heading-anchors",
        action="store_true",
        help="insert source-heading anchors into existing translations",
    )
    parser.add_argument(
        "--write-metadata",
        action="store_true",
        help="refresh translation SHA metadata and normalize language navigation",
    )
    parser.add_argument(
        "--source",
        action="append",
        default=[],
        help="limit write operations to this canonical source (repeatable)",
    )
    args = parser.parse_args()

    manifest = load_manifest()
    files = tracked_markdown()
    sources = canonical_sources(files, manifest)
    requested_sources = set(args.source)
    unknown_sources = sorted(requested_sources - set(sources))
    if unknown_sources:
        parser.error(f"unknown canonical sources: {', '.join(unknown_sources)}")
    write_sources = [source for source in sources if not requested_sources or source in requested_sources]
    strict = args.strict or manifest.get("strict", False)
    tracked = set(files)
    missing = []
    errors = []

    if args.write_navigation:
        for source in write_sources:
            path = ROOT / source
            text = path.read_text(encoding="utf-8")
            updated = add_navigation(text, navigation_line(source, source, manifest))
            if updated != text:
                path.write_text(updated, encoding="utf-8")

    if args.write_heading_anchors:
        for source in write_sources:
            source_text = (ROOT / source).read_text(encoding="utf-8")
            for translated in translation_paths(source, manifest).values():
                path = ROOT / translated
                if not path.exists():
                    continue
                text = path.read_text(encoding="utf-8")
                updated = add_source_heading_anchors(source_text, text)
                if updated != text:
                    path.write_text(updated, encoding="utf-8")

    if args.write_metadata:
        for source in write_sources:
            for locale, translated in translation_paths(source, manifest).items():
                path = ROOT / translated
                if path.exists():
                    synchronize_metadata(source, locale, translated, manifest)

    expected_translations = set()
    for source in sources:
        source_text = (ROOT / source).read_text(encoding="utf-8")
        if navigation_line(source, source, manifest) not in source_text:
            errors.append(f"{source}: missing or incorrect language navigation")
        unresolved = unresolved_internal_links(
            source, source_text, structure(source_text)["link_targets"]
        )
        if unresolved:
            errors.append(
                f"{source}: unresolved internal links: {', '.join(unresolved)}"
            )
        for locale, translated in translation_paths(source, manifest).items():
            expected_translations.add(translated)
            if translated not in tracked:
                missing.append(translated)
                continue
            errors.extend(validate_translation(source, locale, translated, manifest))

    known_translations = {
        path
        for path in files
        if path.endswith(TRANSLATION_SUFFIXES)
        or path in {
            value
            for translations in manifest.get("path_overrides", {}).values()
            for value in translations.values()
        }
    }
    orphaned = sorted(known_translations - expected_translations)
    if orphaned:
        errors.extend(f"{path}: translation has no canonical source" for path in orphaned)

    print(
        f"i18n inventory: {len(sources)} English sources, "
        f"{len(expected_translations) - len(missing)}/{len(expected_translations)} translations present"
    )
    if missing:
        print("missing translations:")
        for path in sorted(missing):
            print(f"  {path}")
    if errors:
        print("i18n errors:")
        for error in errors:
            print(f"  {error}")

    if strict and (errors or missing):
        return 1
    if missing:
        print("manifest strict mode is disabled; missing translations are migration work")
    return 0


if __name__ == "__main__":
    sys.exit(main())
