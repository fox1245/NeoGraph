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


def tracked_markdown() -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "*.md"],
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
    }


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
    navigation = navigation_line(source, translated, manifest)
    if navigation not in translated_text:
        errors.append(f"{translated}: missing or incorrect language navigation")
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
    args = parser.parse_args()

    manifest = load_manifest()
    files = tracked_markdown()
    sources = canonical_sources(files, manifest)
    strict = args.strict or manifest.get("strict", False)
    tracked = set(files)
    missing = []
    errors = []

    if args.write_navigation:
        for source in sources:
            path = ROOT / source
            text = path.read_text(encoding="utf-8")
            updated = add_navigation(text, navigation_line(source, source, manifest))
            if updated != text:
                path.write_text(updated, encoding="utf-8")

    expected_translations = set()
    for source in sources:
        source_text = (ROOT / source).read_text(encoding="utf-8")
        if navigation_line(source, source, manifest) not in source_text:
            errors.append(f"{source}: missing or incorrect language navigation")
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
        print("translation errors:")
        for error in errors:
            print(f"  {error}")

    if strict and (errors or missing):
        return 1
    if missing:
        print("manifest strict mode is disabled; missing translations are migration work")
    return 0


if __name__ == "__main__":
    sys.exit(main())
