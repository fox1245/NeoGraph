#!/usr/bin/env python3
"""Regenerate the model catalogues of the shipped typed-client configurations
from the providers' own model listings.

The typed clients admit exactly the models their configuration lists, so a new
provider model needs only a configuration change. This script makes that change
from the listings instead of by hand:

* schemas/typed-decisions-client.json from OpenRouter's public models listing
  (no key needed): every model whose output modalities include "decisions".
* schemas/typed-veo-client.json from the Gemini models listing, only when
  GEMINI_API_KEY is set: every model that supports predictLongRunning. Limits
  already declared for a model that is still listed are kept.

With --check it writes nothing and exits 1 when a catalogue differs from the
listing, printing the difference. A configured default model that the listing
no longer has is an error either way; the script never picks a new default.
"""
import argparse
import difflib
import json
import os
import sys
import urllib.request
from decimal import Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OPENROUTER_DECISIONS = "https://openrouter.ai/api/v1/models?output_modalities=decisions"
GEMINI_MODELS = "https://generativelanguage.googleapis.com/v1beta/models?pageSize=1000"


def fetch(url, headers=None):
    request = urllib.request.Request(url, headers={"User-Agent": "neograph-catalogue-sync", **(headers or {})})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.loads(response.read())


def per_million(price):
    value = Decimal(str(price or "0")) * 1000000
    return int(value) if value == value.to_integral_value() else float(value)


def decisions_models(listing):
    models = []
    for entry in listing["data"]:
        if "decisions" not in (entry.get("architecture") or {}).get("output_modalities", []):
            continue
        pricing = entry.get("pricing") or {}
        models.append({
            "id": entry["id"],
            "prompt_usd_per_million_tokens": per_million(pricing.get("prompt")),
            "output_usd_per_million_tokens": per_million(pricing.get("completion")),
            "implicit_caching": "input_cache_read" in pricing,
        })
    return sorted(models, key=lambda model: model["id"])


def veo_models(pages, current):
    declared = {entry["id"]: entry for entry in current if isinstance(entry, dict)}
    names = sorted(model["name"].removeprefix("models/") for page in pages for model in page.get("models", [])
                   if "predictLongRunning" in model.get("supportedGenerationMethods", []))
    return [declared.get(name, name) for name in names]


def gemini_pages(key):
    pages, token = [], ""
    while True:
        page = fetch(GEMINI_MODELS + (f"&pageToken={token}" if token else ""), {"x-goog-api-key": key})
        pages.append(page)
        token = page.get("nextPageToken")
        if not token:
            return pages


def model_id(entry):
    return entry["id"] if isinstance(entry, dict) else entry


def update(path, document, models, default_path):
    if not models:
        raise SystemExit(f"{path.relative_to(ROOT)}: the listing has no models; refusing to write an empty catalogue")
    default = document
    for key in default_path[:-1]:
        default = default[key]
    if default[default_path[-1]] not in {model_id(model) for model in models}:
        raise SystemExit(f"{path.relative_to(ROOT)}: default model {default[default_path[-1]]!r} is no longer listed; "
                         "choose a new default by hand")
    document["models"] = models
    return json.dumps(document, indent=2, ensure_ascii=False) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="write nothing; exit 1 when a catalogue differs")
    args = parser.parse_args()

    targets = []
    decisions = ROOT / "schemas/typed-decisions-client.json"
    document = json.loads(decisions.read_text(encoding="utf-8"))
    targets.append((decisions, update(decisions, document, decisions_models(fetch(OPENROUTER_DECISIONS)),
                                      ["default_model"])))
    key = os.environ.get("GEMINI_API_KEY")
    if key:
        veo = ROOT / "schemas/typed-veo-client.json"
        document = json.loads(veo.read_text(encoding="utf-8"))
        targets.append((veo, update(veo, document, veo_models(gemini_pages(key), document["models"]),
                                    ["defaults", "model"])))
    else:
        print("GEMINI_API_KEY is not set; skipping the Veo catalogue")

    drift = False
    for path, text in targets:
        old = path.read_text(encoding="utf-8")
        if old == text:
            print(f"{path.relative_to(ROOT)}: up to date")
            continue
        drift = True
        name = str(path.relative_to(ROOT))
        sys.stdout.writelines(difflib.unified_diff(old.splitlines(True), text.splitlines(True), name, name))
        if not args.check:
            path.write_text(text, encoding="utf-8")
            print(f"{name}: updated")
    return 1 if drift and args.check else 0


if __name__ == "__main__":
    sys.exit(main())
