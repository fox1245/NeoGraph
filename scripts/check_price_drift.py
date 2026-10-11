#!/usr/bin/env python3
"""Compare the pricing catalog with live price sources and report drift.

NeoGraph never fetches prices at run time: Program budgets are priced from the
versioned catalog in data/pricing/catalog.json. This script is the only place
that reads price sources, so a person can review a change before it reaches
the catalog. It reads, compares and reports; it never edits the catalog.

Sources (HTTP GET only, standard library only):

  openrouter  https://openrouter.ai/api/v1/models, and the per-model
              /endpoints listing for entries with a route ceiling. Prices are
              USD per token. Authoritative for the "openrouter" origin. No key.
  litellm     BerriAI/litellm model_prices_and_context_window.json. Community
              maintained, so it is a signal to check against the official
              page, not the truth. No key.
  gcp         Cloud Billing Catalog API SKUs of the Vertex AI service, for
              entries that map rate fields to SKUs (drift.gcp_skus). Needs an
              API key in GCP_BILLING_API_KEY, sent as a header; without it the
              source is skipped.

A rate that differs from its source by more than the tolerance is drift, in
either direction. A rate the source lists but the catalog leaves to a
fallback is drift only when the source rate is higher than the fallback (the
catalog would under-price it). An entry may list "source:field" pairs in
drift.ignore after a person has checked the official page; those findings are
reported as ignored.

Exit status: 0 no drift, 1 drift, 2 the catalog is invalid or a source could
not be read.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import asdict, dataclass, field
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any, Callable

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_CATALOG = ROOT / "data" / "pricing" / "catalog.json"
OPENROUTER_API = "https://openrouter.ai/api/v1"
LITELLM_URL = (
    "https://raw.githubusercontent.com/BerriAI/litellm/main/"
    "model_prices_and_context_window.json"
)
GCP_BILLING_API = "https://cloudbilling.googleapis.com/v1"
GCP_KEY_ENV = "GCP_BILLING_API_KEY"
MAX_RESPONSE_BYTES = 32 * 1024 * 1024
MILLION = Decimal(1_000_000)

RATE_FIELDS = ("input", "output", "cache_read", "cache_write", "cache_write_1h", "output_image")
RATE_RE = re.compile(r"^(0|[1-9][0-9]{0,4})(\.[0-9]{1,6})?$")
DATE_RE = re.compile(r"^[0-9]{4}-(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])$")

OPENROUTER_FIELDS = {
    "prompt": "input",
    "completion": "output",
    "input_cache_read": "cache_read",
    "input_cache_write": "cache_write",
}
LITELLM_FIELDS = {
    "input_cost_per_token": "input",
    "output_cost_per_token": "output",
    "cache_read_input_token_cost": "cache_read",
    "cache_creation_input_token_cost": "cache_write",
    "cache_creation_input_token_cost_above_1hr": "cache_write_1h",
    "output_cost_per_image_token": "output_image",
}
# LiteLLM key prefix per catalog origin; an entry may override it with drift.litellm_key.
LITELLM_PREFIX = {
    "openai": "",
    "anthropic": "",
    "google-ai-studio": "gemini/",
    "google-vertex-global": "vertex_ai/",
    "openrouter": "openrouter/",
}


class CatalogInvalid(ValueError):
    """The catalog does not have the shape the runtime loader accepts."""


class SourceError(RuntimeError):
    """A price source could not be read. Messages never include credentials."""


@dataclass
class Finding:
    origin: str
    model: str
    source: str
    field: str
    status: str  # ok | drift | info | ignored
    catalog: str | None = None
    live: str | None = None
    detail: str = ""


@dataclass
class Report:
    catalog: str
    catalog_version: str
    tolerance: str
    sources: dict[str, str] = field(default_factory=dict)
    findings: list[Finding] = field(default_factory=list)

    @property
    def drift(self) -> bool:
        return any(finding.status == "drift" for finding in self.findings)

    @property
    def source_failed(self) -> bool:
        return any(state.startswith("error") for state in self.sources.values())

    def summary(self) -> dict[str, int]:
        counts = {"ok": 0, "drift": 0, "info": 0, "ignored": 0}
        for finding in self.findings:
            counts[finding.status] = counts.get(finding.status, 0) + 1
        return counts

    def to_json(self) -> dict[str, Any]:
        return {
            "catalog": self.catalog,
            "catalog_version": self.catalog_version,
            "tolerance": self.tolerance,
            "sources": self.sources,
            "summary": self.summary(),
            "drift": self.drift,
            "findings": [asdict(finding) for finding in self.findings],
        }


# ---------------------------------------------------------------- numbers


def plain(value: Decimal | None) -> str | None:
    if value is None:
        return None
    text = format(value.normalize(), "f")
    return text


def per_million(per_token: Any) -> Decimal:
    """USD per token (string or float from a source) to USD per million tokens, exactly."""
    if isinstance(per_token, bool) or per_token is None:
        raise ValueError(f"not a price: {per_token!r}")
    try:
        # repr() of a float is its shortest round-trip decimal, so 2.5e-07 stays 2.5E-7.
        value = Decimal(per_token if isinstance(per_token, str) else repr(per_token))
    except InvalidOperation as error:
        raise ValueError(f"not a price: {per_token!r}") from error
    if not value.is_finite() or value < 0:
        raise ValueError(f"not a price: {per_token!r}")
    return value * MILLION


def catalog_rate(text: Any, where: str) -> Decimal:
    if not isinstance(text, str) or not RATE_RE.match(text) or Decimal(text) > 10_000:
        raise CatalogInvalid(f"{where}: must be a decimal string in USD per million tokens")
    return Decimal(text)


def relative_delta(catalog: Decimal, live: Decimal) -> Decimal | None:
    if catalog == live:
        return Decimal(0)
    if catalog == 0:
        return None  # infinite
    return abs(live - catalog) / catalog


def within(catalog: Decimal, live: Decimal, tolerance: Decimal) -> bool:
    delta = relative_delta(catalog, live)
    return delta is not None and delta <= tolerance


# ---------------------------------------------------------------- catalog


def effective(rates: dict[str, Decimal], name: str) -> Decimal:
    """The rate a band bills at, following the runtime's fallbacks."""
    if name in rates:
        return rates[name]
    if name in ("cache_read", "cache_write"):
        return rates["input"]
    if name == "cache_write_1h":
        return effective(rates, "cache_write")
    if name == "output_image":
        return rates["output"]
    raise KeyError(name)


def parse_rates(obj: dict, where: str) -> dict[str, Decimal]:
    rates = {}
    for name in RATE_FIELDS:
        if name in obj:
            rates[name] = catalog_rate(obj[name], f"{where}.{name}")
    for required in ("input", "output"):
        if required not in rates:
            raise CatalogInvalid(f"{where}: missing field '{required}'")
    return rates


def load_catalog(path: Path) -> dict:
    """Load the catalog and check the fields this script relies on.

    The C++ loader (src/program/pricing.cpp) is the full validator; this keeps
    the script from comparing against a catalog the runtime would refuse.
    """
    try:
        document = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CatalogInvalid(f"{path}: {error}") from error
    if not isinstance(document, dict):
        raise CatalogInvalid("catalog: must be an object")
    expected = {
        "schema": "neograph.pricing.catalog",
        "schema_version": 1,
        "currency": "USD",
        "rate_unit": "usd_per_million_tokens",
    }
    for key, value in expected.items():
        if document.get(key) != value:
            raise CatalogInvalid(f"catalog.{key}: must be {value!r}")
    if not isinstance(document.get("version"), str) or not document["version"]:
        raise CatalogInvalid("catalog.version: must be a non-empty string")
    origins = document.get("origins")
    if not isinstance(origins, dict) or not origins:
        raise CatalogInvalid("catalog.origins: must be a non-empty object")
    entries = document.get("entries")
    if not isinstance(entries, list) or not entries:
        raise CatalogInvalid("catalog.entries: must be a non-empty array")
    for index, entry in enumerate(entries):
        where = f"entries[{index}]"
        if not isinstance(entry, dict):
            raise CatalogInvalid(f"{where}: must be an object")
        if entry.get("origin") not in origins:
            raise CatalogInvalid(f"{where}.origin: names no declared origin")
        if not isinstance(entry.get("model"), str) or not entry["model"]:
            raise CatalogInvalid(f"{where}.model: must be a non-empty string")
        url = entry.get("source_url")
        if not isinstance(url, str) or not url.startswith("https://"):
            raise CatalogInvalid(f"{where}.source_url: must be an https:// URL")
        if not isinstance(entry.get("retrieved"), str) or not DATE_RE.match(entry["retrieved"]):
            raise CatalogInvalid(f"{where}.retrieved: must be a YYYY-MM-DD date")
        tiers = entry.get("tiers")
        if not isinstance(tiers, list) or not tiers:
            raise CatalogInvalid(f"{where}.tiers: must be a non-empty array")
        entry["_tiers"] = [parse_rates(tier, f"{where}.tiers[{i}]") for i, tier in enumerate(tiers)]
        ceiling = entry.get("route_ceiling")
        if ceiling is not None:
            entry["_ceiling"] = parse_rates(ceiling, f"{where}.route_ceiling")
        drift = entry.get("drift", {})
        if not isinstance(drift, dict):
            raise CatalogInvalid(f"{where}.drift: must be an object")
        ignore = drift.get("ignore", [])
        if not isinstance(ignore, list) or not all(isinstance(item, str) for item in ignore):
            raise CatalogInvalid(f"{where}.drift.ignore: must be an array of 'source:field' strings")
    return document


# ---------------------------------------------------------------- fetching


def fetch_json(url: str, *, headers: dict[str, str] | None = None, timeout: float = 30) -> Any:
    """GET a JSON document. Error messages carry the URL without its query string."""
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": "neograph-price-drift-check/1",
            **(headers or {}),
        },
    )
    shown = urllib.parse.urlsplit(url)._replace(query="", fragment="").geturl()
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            body = response.read(MAX_RESPONSE_BYTES + 1)
    except urllib.error.HTTPError as error:
        raise SourceError(f"GET {shown}: HTTP {error.code}") from None
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        reason = getattr(error, "reason", error)
        raise SourceError(f"GET {shown}: {reason}") from None
    if len(body) > MAX_RESPONSE_BYTES:
        raise SourceError(f"GET {shown}: response larger than {MAX_RESPONSE_BYTES} bytes")
    try:
        return json.loads(body)
    except json.JSONDecodeError as error:
        raise SourceError(f"GET {shown}: invalid JSON ({error.msg})") from None


Fetch = Callable[..., Any]


# ---------------------------------------------------------------- comparison


class Comparer:
    def __init__(self, report: Report, tolerance: Decimal):
        self.report = report
        self.tolerance = tolerance

    def add(self, entry: dict, source: str, field_name: str, status: str,
            catalog: Decimal | None = None, live: Decimal | None = None, detail: str = "") -> None:
        ignored = entry.get("drift", {}).get("ignore", [])
        if status == "drift" and f"{source}:{field_name}" in ignored:
            status = "ignored"
            detail = (detail + "; " if detail else "") + "listed in drift.ignore"
        self.report.findings.append(Finding(
            origin=entry["origin"], model=entry["model"], source=source, field=field_name,
            status=status, catalog=plain(catalog), live=plain(live), detail=detail))

    def rates(self, entry: dict, source: str, catalog: dict[str, Decimal],
              live: dict[str, Decimal], label: str = "", direction_matters: bool = False,
              only: set[str] | None = None) -> None:
        """Compare one set of catalog rates with the source's rates for the same thing."""
        for name in RATE_FIELDS:
            if only is not None and name not in only:
                continue
            field_name = f"{label}{name}"
            if name in catalog and name in live:
                if within(catalog[name], live[name], self.tolerance):
                    self.add(entry, source, field_name, "ok", catalog[name], live[name])
                    continue
                detail = "differs beyond tolerance"
                if direction_matters:
                    detail = ("below the live maximum: reservations can under-reserve"
                              if live[name] > catalog[name] else
                              "above the live maximum: reservations over-reserve")
                self.add(entry, source, field_name, "drift", catalog[name], live[name], detail)
            elif name in live:
                fallback = effective(catalog, name)
                if live[name] > fallback and not within(fallback, live[name], self.tolerance):
                    self.add(entry, source, field_name, "drift", None, live[name],
                             f"source lists it; the catalog bills it at {plain(fallback)}")
                else:
                    self.add(entry, source, field_name, "info", None, live[name],
                             f"source lists it; the catalog bills it at {plain(fallback)}")
            elif name in catalog:
                self.add(entry, source, field_name, "info", catalog[name], None,
                         "source does not list it")


def openrouter_rates(pricing: dict) -> dict[str, Decimal]:
    rates = {}
    for key, name in OPENROUTER_FIELDS.items():
        value = pricing.get(key)
        if value is not None:
            rates[name] = per_million(value)
    return rates


def unmodeled(entry: dict, comparer: Comparer, pricing: dict) -> None:
    """Price components the catalog cannot represent. Per-call ones are drift."""
    for key, value in pricing.items():
        if key in OPENROUTER_FIELDS or key in ("overrides", "discount"):
            continue
        try:
            amount = per_million(value) if isinstance(value, (str, int, float)) else None
        except ValueError:
            amount = None
        if amount is None or amount == 0:
            continue
        if key == "internal_reasoning":
            if within(per_million(pricing.get("completion", "0")), amount, comparer.tolerance):
                continue
            comparer.add(entry, "openrouter", key, "drift", live=amount,
                         detail="reasoning tokens priced apart from completion; not modeled")
        elif key == "request":
            comparer.add(entry, "openrouter", key, "drift", live=amount / MILLION,
                         detail="per-request fee in USD; not modeled")
        else:
            comparer.add(entry, "openrouter", key, "info", live=amount,
                         detail="priced component the catalog does not model")


def openrouter_peak(pricings: list[dict]) -> dict[str, Decimal]:
    """Highest rate per field over endpoint pricings and their time-of-day overrides."""
    peak: dict[str, Decimal] = {}
    for pricing in pricings:
        for layer in [pricing, *pricing.get("overrides", [])]:
            for name, value in openrouter_rates(layer).items():
                if name not in peak or value > peak[name]:
                    peak[name] = value
    return peak


def check_openrouter(catalog: dict, comparer: Comparer, fetch: Fetch, api: str) -> None:
    entries = [entry for entry in catalog["entries"]
               if catalog["origins"][entry["origin"]].get("routing") == "aggregator"
               and entry["origin"] == "openrouter"]
    if not entries:
        return
    listing = fetch(f"{api}/models")
    models = listing.get("data") if isinstance(listing, dict) else None
    if not isinstance(models, list):
        raise SourceError("GET /models: no data array")
    by_id: dict[str, dict] = {}
    for model in models:
        if isinstance(model, dict):
            for key in (model.get("id"), model.get("canonical_slug")):
                if isinstance(key, str):
                    by_id.setdefault(key, model)
    for entry in entries:
        model = by_id.get(entry["model"])
        if model is None:
            comparer.add(entry, "openrouter", "model", "drift", detail="not listed by /models")
            continue
        pricing = model.get("pricing") or {}
        comparer.rates(entry, "openrouter", entry["_tiers"][0], openrouter_rates(pricing))
        unmodeled(entry, comparer, pricing)
        if pricing.get("overrides"):
            peak = openrouter_peak([pricing])
            comparer.add(entry, "openrouter", "pricing", "info",
                         detail="time-of-day overrides; peak " + ", ".join(
                             f"{name} {plain(value)}" for name, value in sorted(peak.items())))
        if "_ceiling" not in entry:
            continue
        slug = entry.get("drift", {}).get("openrouter_endpoints", entry["model"])
        endpoints_doc = fetch(f"{api}/models/{urllib.parse.quote(slug, safe='/~')}/endpoints")
        data = endpoints_doc.get("data") if isinstance(endpoints_doc, dict) else None
        endpoints = data.get("endpoints") if isinstance(data, dict) else None
        if not isinstance(endpoints, list) or not endpoints:
            comparer.add(entry, "openrouter-endpoints", "route_ceiling", "drift",
                         detail="the endpoints listing is empty")
            continue
        peak = openrouter_peak([e.get("pricing") or {} for e in endpoints if isinstance(e, dict)])
        comparer.rates(entry, "openrouter-endpoints", entry["_ceiling"], peak,
                       label="route_ceiling.", direction_matters=True)


def litellm_rates(record: dict, suffix: str = "") -> dict[str, Decimal]:
    """LiteLLM rates; a tier suffix such as "_above_200k_tokens" selects a higher tier."""
    rates = {}
    for key, name in LITELLM_FIELDS.items():
        value = record.get(f"{key}{suffix}")
        if value is not None:
            rates[name] = per_million(value)
    return rates


def check_litellm(catalog: dict, comparer: Comparer, fetch: Fetch, url: str) -> None:
    prices = fetch(url)
    if not isinstance(prices, dict):
        raise SourceError("LiteLLM price file is not an object")
    for entry in catalog["entries"]:
        drift = entry.get("drift", {})
        key = drift.get("litellm_key")
        if key is None:
            prefix = LITELLM_PREFIX.get(entry["origin"])
            if prefix is None:
                comparer.add(entry, "litellm", "model", "info", detail="no LiteLLM key for this origin")
                continue
            key = prefix + entry["model"]
        record = prices.get(key)
        if not isinstance(record, dict):
            comparer.add(entry, "litellm", "model", "info", detail=f"no LiteLLM entry {key!r}")
            continue
        tiers = entry["_tiers"]
        comparer.rates(entry, "litellm", tiers[0], litellm_rates(record))
        for index in range(1, len(tiers)):
            threshold = entry["tiers"][index - 1].get("up_to_input_tokens")
            if not isinstance(threshold, int) or threshold % 1000:
                continue
            suffix = f"_above_{threshold // 1000}k_tokens"
            live = litellm_rates(record, suffix)
            if not live:
                comparer.add(entry, "litellm", f"tiers[{index}]", "info",
                             detail=f"LiteLLM has no {suffix} rates")
                continue
            comparer.rates(entry, "litellm", tiers[index], live, label=f"tiers[{index}].")


def sku_per_million(sku: dict) -> Decimal:
    """USD per million base units (tokens) from a Cloud Billing SKU's first price tier."""
    try:
        expression = sku["pricingInfo"][0]["pricingExpression"]
        tiers = sorted(expression["tieredRates"],
                       key=lambda rate: Decimal(str(rate.get("startUsageAmount", 0))))
        unit_price = tiers[0]["unitPrice"]
        if unit_price.get("currencyCode", "USD") != "USD":
            raise ValueError("not priced in USD")
        price = Decimal(str(unit_price.get("units", "0"))) + Decimal(unit_price.get("nanos", 0)) / Decimal(10**9)
        factor = Decimal(str(expression.get("baseUnitConversionFactor", 1)))
        if factor <= 0:
            raise ValueError("bad baseUnitConversionFactor")
    except (KeyError, IndexError, TypeError, InvalidOperation) as error:
        raise ValueError(f"unexpected SKU pricing shape: {error}") from None
    return price / factor * MILLION


def paginate(fetch: Fetch, url: str, key: str, item: str) -> list[dict]:
    items: list[dict] = []
    token = None
    for _ in range(100):
        page_url = url + ("&" if "?" in url else "?") + "pageSize=5000"
        if token:
            page_url += "&pageToken=" + urllib.parse.quote(token)
        page = fetch(page_url, headers={"X-Goog-Api-Key": key})
        items.extend(page.get(item, []) if isinstance(page, dict) else [])
        token = page.get("nextPageToken") if isinstance(page, dict) else None
        if not token:
            return items
    raise SourceError(f"{item}: more than 100 pages")


def check_gcp(catalog: dict, comparer: Comparer, fetch: Fetch, key: str) -> None:
    entries = [entry for entry in catalog["entries"] if entry["origin"].startswith("google-vertex")]
    if not entries:
        return
    services = paginate(fetch, f"{GCP_BILLING_API}/services", key, "services")
    service = next((s for s in services if s.get("displayName") == "Vertex AI"), None)
    if service is None:
        raise SourceError("Cloud Billing lists no 'Vertex AI' service")
    skus = paginate(fetch, f"{GCP_BILLING_API}/{service['name']}/skus?currencyCode=USD", key, "skus")
    by_key = {}
    for sku in skus:
        by_key.setdefault(sku.get("skuId"), sku)
        by_key.setdefault(sku.get("description"), sku)
    for entry in entries:
        mapping = entry.get("drift", {}).get("gcp_skus")
        if not mapping:
            words = [w for w in re.split(r"[^a-z0-9.]+", entry["model"].lower()) if w]
            candidates = sorted(sku.get("description", "") for sku in skus
                                if all(w in sku.get("description", "").lower() for w in words))
            comparer.add(entry, "gcp", "model", "info",
                         detail="no drift.gcp_skus mapping; candidate SKUs: "
                                + ("; ".join(candidates[:8]) or "none"))
            continue
        live: dict[str, Decimal] = {}
        for name, sku_key in mapping.items():
            sku = by_key.get(sku_key)
            if sku is None:
                comparer.add(entry, "gcp", name, "drift", detail=f"SKU {sku_key!r} not found")
                continue
            try:
                live[name] = sku_per_million(sku)
            except ValueError as error:
                comparer.add(entry, "gcp", name, "info", detail=str(error))
        comparer.rates(entry, "gcp", entry["_tiers"][0], live, only=set(mapping))


# ---------------------------------------------------------------- driver


def run(catalog_path: Path, *, sources: list[str], tolerance: Decimal, fetch: Fetch,
        openrouter_api: str = OPENROUTER_API, litellm_url: str = LITELLM_URL,
        gcp_key: str | None = None) -> Report:
    catalog = load_catalog(catalog_path)
    report = Report(catalog=str(catalog_path), catalog_version=catalog["version"],
                    tolerance=plain(tolerance) or "0")
    comparer = Comparer(report, tolerance)
    checks = {
        "openrouter": lambda: check_openrouter(catalog, comparer, fetch, openrouter_api),
        "litellm": lambda: check_litellm(catalog, comparer, fetch, litellm_url),
        "gcp": lambda: check_gcp(catalog, comparer, fetch, gcp_key or ""),
    }
    for name in sources:
        if name == "gcp" and not gcp_key:
            report.sources[name] = f"skipped: {GCP_KEY_ENV} is not set"
            continue
        try:
            checks[name]()
        except (SourceError, ValueError) as error:
            report.sources[name] = f"error: {error}"
        else:
            report.sources[name] = "ok"
    return report


def render_table(report: Report) -> str:
    header = ("STATUS", "ORIGIN", "MODEL", "SOURCE", "FIELD", "CATALOG", "LIVE", "DETAIL")
    rows = [header] + [
        (f.status, f.origin, f.model, f.source, f.field, f.catalog or "-", f.live or "-", f.detail)
        for f in report.findings
    ]
    widths = [max(len(row[i]) for row in rows) for i in range(len(header) - 1)]
    lines = [
        f"Price drift check: catalog {report.catalog_version}, tolerance {report.tolerance}, "
        "rates in USD per million tokens",
    ]
    for name, state in report.sources.items():
        lines.append(f"  source {name}: {state}")
    lines.append("")
    for row in rows:
        lines.append("  ".join(cell.ljust(width) for cell, width in zip(row, widths)) + "  " + row[-1])
    summary = report.summary()
    lines.append("")
    lines.append(", ".join(f"{count} {status}" for status, count in summary.items()))
    return "\n".join(line.rstrip() for line in lines)


def main(argv: list[str] | None = None, *, fetch: Fetch | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--catalog", type=Path, default=DEFAULT_CATALOG)
    parser.add_argument("--sources", default="openrouter,litellm,gcp",
                        help="comma-separated subset of openrouter,litellm,gcp")
    parser.add_argument("--tolerance", default="0.005",
                        help="relative difference accepted before a rate is drift (default 0.005)")
    parser.add_argument("--json", action="store_true", help="print a JSON report instead of a table")
    parser.add_argument("--json-out", type=Path, help="also write the JSON report to this file")
    parser.add_argument("--timeout", type=float, default=30.0, help="seconds per request")
    parser.add_argument("--openrouter-api", default=OPENROUTER_API)
    parser.add_argument("--litellm-url", default=LITELLM_URL)
    args = parser.parse_args(argv)

    sources = [name.strip() for name in args.sources.split(",") if name.strip()]
    unknown = sorted(set(sources) - {"openrouter", "litellm", "gcp"})
    if unknown:
        parser.error(f"unknown source(s): {', '.join(unknown)}")
    try:
        tolerance = Decimal(args.tolerance)
    except InvalidOperation:
        parser.error("--tolerance must be a number")
    if not tolerance.is_finite() or tolerance < 0:
        parser.error("--tolerance must be a non-negative number")

    if fetch is None:
        def fetch(url: str, headers: dict[str, str] | None = None) -> Any:
            return fetch_json(url, headers=headers, timeout=args.timeout)

    try:
        report = run(args.catalog, sources=sources, tolerance=tolerance, fetch=fetch,
                     openrouter_api=args.openrouter_api.rstrip("/"), litellm_url=args.litellm_url,
                     gcp_key=os.environ.get(GCP_KEY_ENV) or None)
    except CatalogInvalid as error:
        print(f"invalid catalog: {error}", file=sys.stderr)
        return 2

    if args.json_out:
        args.json_out.write_text(json.dumps(report.to_json(), indent=2) + "\n", encoding="utf-8")
    if args.json:
        print(json.dumps(report.to_json(), indent=2))
    else:
        print(render_table(report))
    if report.source_failed:
        return 2
    return 1 if report.drift else 0


if __name__ == "__main__":
    sys.exit(main())
