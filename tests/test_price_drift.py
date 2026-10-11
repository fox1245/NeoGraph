"""Price drift checker on recorded sources (stdlib only, no network).

tests/fixtures/pricing holds responses recorded on 2026-10-11, trimmed to the
models under test: OpenRouter /models and /endpoints, and LiteLLM's price
file. The Cloud Billing fixtures are synthetic (no key was used to record
them) and follow the documented SKU shape. catalog.json there is the catalog
as written that day, so it must agree with the recorded sources.
"""
import contextlib
import copy
import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
from decimal import Decimal
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests" / "fixtures" / "pricing"
spec = importlib.util.spec_from_file_location("check_price_drift", ROOT / "scripts/check_price_drift.py")
drift = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = drift  # dataclasses resolve annotations through sys.modules
spec.loader.exec_module(drift)

OPENROUTER = drift.OPENROUTER_API
RECORDED = {
    f"{OPENROUTER}/models": "openrouter_models.json",
    f"{OPENROUTER}/models/deepseek/deepseek-v4-flash-0731/endpoints":
        "openrouter_endpoints_deepseek-v4-flash-0731.json",
    f"{OPENROUTER}/models/deepseek/deepseek-v4-flash-vision-exp/endpoints":
        "openrouter_endpoints_deepseek-v4-flash-vision-exp.json",
    drift.LITELLM_URL: "litellm.json",
    f"{drift.GCP_BILLING_API}/services?pageSize=5000": "gcp_services_synthetic.json",
    f"{drift.GCP_BILLING_API}/services/0000-SYNTHETIC-0002/skus?currencyCode=USD&pageSize=5000":
        "gcp_skus_synthetic_page1.json",
    f"{drift.GCP_BILLING_API}/services/0000-SYNTHETIC-0002/skus?currencyCode=USD&pageSize=5000"
    "&pageToken=page-2": "gcp_skus_synthetic_page2.json",
}


class Recorded:
    """A fetch() replacement that serves fixtures and records what was asked."""

    def __init__(self, overrides=None, fail=()):
        self.overrides = overrides or {}
        self.fail = set(fail)
        self.requests = []

    def __call__(self, url, headers=None):
        self.requests.append((url, dict(headers or {})))
        for prefix in self.fail:
            if url.startswith(prefix):
                raise drift.SourceError(f"GET {prefix}: HTTP 503")
        if url in self.overrides:
            return copy.deepcopy(self.overrides[url])
        if url not in RECORDED:
            raise AssertionError(f"unexpected request {url}")
        return json.loads((FIXTURES / RECORDED[url]).read_text(encoding="utf-8"))


def recorded(name):
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


class DriftCheck(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        env = mock.patch.dict(os.environ, {}, clear=False)
        env.start()
        self.addCleanup(env.stop)
        os.environ.pop(drift.GCP_KEY_ENV, None)

    def catalog(self, edit=None):
        document = recorded("catalog.json")
        if edit:
            edit(document)
        path = Path(self.tmp.name) / "catalog.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def run_main(self, args, fetch):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = drift.main([*args], fetch=fetch)
        return status, out.getvalue()

    def report(self, path, fetch=None, tolerance="0.005", sources=("openrouter", "litellm", "gcp"),
               key=None):
        return drift.run(path, sources=list(sources), tolerance=Decimal(tolerance),
                         fetch=fetch or Recorded(), gcp_key=key)

    def findings(self, report, status=None, field=None, source=None):
        return [f for f in report.findings
                if (status is None or f.status == status) and (field is None or f.field == field)
                and (source is None or f.source == source)]

    @staticmethod
    def entry(document, origin, model):
        return next(e for e in document["entries"] if e["origin"] == origin and e["model"] == model)

    # --------------------------------------------------------------- baseline

    def test_catalog_agrees_with_the_sources_recorded_when_it_was_written(self):
        report = self.report(self.catalog())
        self.assertEqual(report.sources["openrouter"], "ok")
        self.assertEqual(report.sources["litellm"], "ok")
        self.assertTrue(report.sources["gcp"].startswith("skipped"))
        self.assertFalse(report.drift, [f for f in report.findings if f.status == "drift"])
        ceilings = self.findings(report, source="openrouter-endpoints")
        self.assertEqual({f.field for f in ceilings},
                         {"route_ceiling.input", "route_ceiling.output", "route_ceiling.cache_read"})
        status, text = self.run_main(["--catalog", str(self.catalog())], Recorded())
        self.assertEqual(status, 0)
        self.assertIn("0 drift", text)
        self.assertIn("route_ceiling.cache_read", text)

    def test_shipped_catalog_has_the_shape_the_runtime_accepts(self):
        document = drift.load_catalog(drift.DEFAULT_CATALOG)
        for entry in document["entries"]:
            self.assertTrue(entry["source_url"].startswith("https://"), entry["model"])
            self.assertRegex(entry["retrieved"], r"^\d{4}-\d{2}-\d{2}$")
            self.assertIn(entry["origin"], document["origins"])

    def test_time_varying_openrouter_prices_are_reported(self):
        # deepseek-v4-pro-0813 has weekday peak prices; it is not in the catalog,
        # so add an entry for it and check the peak is surfaced.
        def add(document):
            document["entries"].append({
                "origin": "openrouter", "model": "deepseek/deepseek-v4-pro-0813",
                "source_url": "https://openrouter.ai/api/v1/models", "retrieved": "2026-10-11",
                "tiers": [{"input": "0.66", "cache_read": "0.022", "output": "1.98"}]})
        report = self.report(self.catalog(add), sources=["openrouter"])
        notes = self.findings(report, field="pricing")
        self.assertEqual(len(notes), 1)
        self.assertEqual(notes[0].status, "info")
        self.assertIn("input 1.32", notes[0].detail)
        self.assertFalse(report.drift)

    # ------------------------------------------------------------------ drift

    def test_changed_list_price_is_drift_in_either_direction(self):
        for value in ("0.11", "0.09"):
            with self.subTest(value=value):
                def edit(document, value=value):
                    self.entry(document, "openai", "gpt-4.1-nano")["tiers"][0]["input"] = value
                report = self.report(self.catalog(edit))
                drifted = self.findings(report, status="drift")
                self.assertEqual([(f.model, f.source, f.field) for f in drifted],
                                 [("gpt-4.1-nano", "litellm", "input")])
                self.assertEqual(drifted[0].live, "0.1")
                status, _ = self.run_main(["--catalog", str(self.catalog(edit))], Recorded())
                self.assertEqual(status, 1)

    def test_tolerance_is_relative(self):
        def edit(document):
            self.entry(document, "anthropic", "claude-opus-5")["tiers"][0]["output"] = "25.1"
        path = self.catalog(edit)
        self.assertFalse(self.report(path, tolerance="0.005").drift)  # 0.4 % apart
        self.assertTrue(self.report(path, tolerance="0.001").drift)

    def test_route_ceiling_below_an_endpoint_is_drift(self):
        def edit(document):
            entry = self.entry(document, "openrouter", "deepseek/deepseek-v4-flash-0731")
            entry["route_ceiling"]["cache_read"] = "0.10"
        report = self.report(self.catalog(edit), sources=["openrouter"])
        drifted = self.findings(report, status="drift")
        self.assertEqual(len(drifted), 1)
        self.assertEqual(drifted[0].field, "route_ceiling.cache_read")
        self.assertEqual(drifted[0].live, "0.12")
        self.assertIn("under-reserve", drifted[0].detail)

    def test_a_dearer_endpoint_appearing_is_drift(self):
        endpoints = recorded("openrouter_endpoints_deepseek-v4-flash-vision-exp.json")
        endpoints["data"]["endpoints"].append({
            "provider_name": "Example", "tag": "example",
            "pricing": {"prompt": "0.0000005", "completion": "0.00000132"},
            "status": 0})
        url = f"{OPENROUTER}/models/deepseek/deepseek-v4-flash-vision-exp/endpoints"
        report = self.report(self.catalog(), fetch=Recorded({url: endpoints}), sources=["openrouter"])
        self.assertEqual([(f.field, f.live) for f in self.findings(report, status="drift")],
                         [("route_ceiling.input", "0.5")])

    def test_time_of_day_override_counts_toward_the_ceiling(self):
        endpoints = recorded("openrouter_endpoints_deepseek-v4-flash-vision-exp.json")
        endpoints["data"]["endpoints"][0]["pricing"]["overrides"] = [
            {"utc_start": 0, "utc_end": 600, "prompt": "0.00000045", "completion": "0.00000132"}]
        url = f"{OPENROUTER}/models/deepseek/deepseek-v4-flash-vision-exp/endpoints"
        report = self.report(self.catalog(), fetch=Recorded({url: endpoints}), sources=["openrouter"])
        self.assertEqual([(f.field, f.live) for f in self.findings(report, status="drift")],
                         [("route_ceiling.input", "0.45")])

    def test_model_missing_from_openrouter_is_drift(self):
        models = recorded("openrouter_models.json")
        models["data"] = [m for m in models["data"] if m["id"] != "deepseek/deepseek-v4-flash-vision-exp"]
        report = self.report(self.catalog(), fetch=Recorded({f"{OPENROUTER}/models": models}),
                             sources=["openrouter"])
        drifted = self.findings(report, status="drift")
        self.assertEqual([(f.model, f.field) for f in drifted],
                         [("deepseek/deepseek-v4-flash-vision-exp", "model")])

    def test_unmodeled_openrouter_charges(self):
        models = recorded("openrouter_models.json")
        target = next(m for m in models["data"] if m["id"] == "deepseek/deepseek-v4-flash-0731")
        target["pricing"]["internal_reasoning"] = "0.000001"
        target["pricing"]["request"] = "0.001"
        target["pricing"]["web_search"] = "0.004"
        report = self.report(self.catalog(), fetch=Recorded({f"{OPENROUTER}/models": models}),
                             sources=["openrouter"])
        by_field = {f.field: f for f in report.findings if f.model == target["id"]}
        self.assertEqual(by_field["internal_reasoning"].status, "drift")
        self.assertEqual(by_field["request"].status, "drift")
        self.assertEqual(by_field["request"].live, "0.001")
        self.assertEqual(by_field["web_search"].status, "info")

    def test_source_rates_the_catalog_leaves_to_fallbacks(self):
        # Vertex lists a cheaper cache read than the AI Studio entry's input fallback: info.
        lite = recorded("litellm.json")
        lite["gemini/gemini-3.1-flash-lite-image"]["cache_read_input_token_cost"] = 2.5e-08
        # A dearer band the catalog does not price separately would be under-priced: drift.
        lite["gpt-4.1-mini"]["output_cost_per_image_token"] = 4e-05
        report = self.report(self.catalog(), fetch=Recorded({drift.LITELLM_URL: lite}),
                             sources=["litellm"])
        studio = [f for f in report.findings if f.origin == "google-ai-studio" and f.field == "cache_read"]
        self.assertEqual(studio[0].status, "info")
        mini = [f for f in report.findings if f.model == "gpt-4.1-mini" and f.field == "output_image"]
        self.assertEqual(mini[0].status, "drift")

    def test_tiered_rates_compare_with_litellm_above_tiers(self):
        def edit(document):
            document["origins"]["example"] = {"url": "https://api.example.com", "routing": "direct"}
            document["entries"].append({
                "origin": "example", "model": "tiered", "source_url": "https://example.com",
                "retrieved": "2026-10-11", "drift": {"litellm_key": "vertex_ai/claude-sonnet-4-5"},
                "tiers": [
                    {"up_to_input_tokens": 200000, "input": "3", "cache_read": "0.30",
                     "cache_write": "3.75", "cache_write_1h": "6", "output": "15"},
                    {"up_to_input_tokens": None, "input": "6", "cache_read": "0.60",
                     "cache_write": "7.50", "cache_write_1h": "12", "output": "22.50"}]})
        report = self.report(self.catalog(edit), sources=["litellm"])
        tier = [f for f in report.findings if f.model == "tiered" and f.field.startswith("tiers[1].")]
        self.assertEqual({f.field for f in tier},
                         {"tiers[1].input", "tiers[1].output", "tiers[1].cache_read",
                          "tiers[1].cache_write", "tiers[1].cache_write_1h"})
        self.assertTrue(all(f.status == "ok" for f in tier), tier)

    def test_ignore_list_acknowledges_a_checked_difference(self):
        def edit(document):
            entry = self.entry(document, "openai", "gpt-4.1-mini")
            entry["tiers"][0]["cache_read"] = "0.11"
            entry["drift"] = {"ignore": ["litellm:cache_read"]}
        status, text = self.run_main(["--catalog", str(self.catalog(edit)), "--sources", "litellm"],
                                     Recorded())
        self.assertEqual(status, 0)
        self.assertIn("listed in drift.ignore", text)

    # ---------------------------------------------------------------- outputs

    def test_json_report(self):
        saved = Path(self.tmp.name) / "report.json"
        status, text = self.run_main(["--catalog", str(self.catalog()), "--json", "--json-out", str(saved)],
                                     Recorded())
        self.assertEqual(status, 0)
        report = json.loads(text)
        self.assertEqual(json.loads(saved.read_text(encoding="utf-8")), report)
        self.assertEqual(set(report), {"catalog", "catalog_version", "tolerance", "sources", "summary",
                                       "drift", "findings"})
        self.assertEqual(report["catalog_version"], "2026-10-11.1")
        self.assertFalse(report["drift"])
        self.assertEqual(report["summary"]["drift"], 0)
        first = report["findings"][0]
        self.assertEqual(set(first), {"origin", "model", "source", "field", "status", "catalog", "live",
                                      "detail"})

    def test_unreadable_source_exits_2(self):
        fetch = Recorded(fail=[drift.LITELLM_URL])
        status, text = self.run_main(["--catalog", str(self.catalog())], fetch)
        self.assertEqual(status, 2)
        self.assertIn("source litellm: error", text)
        self.assertIn("source openrouter: ok", text)

    def test_invalid_catalog_exits_2(self):
        def edit(document):
            document["entries"][0]["tiers"][0]["input"] = "0.0000001"
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            status, _ = self.run_main(["--catalog", str(self.catalog(edit))], Recorded())
        self.assertEqual(status, 2)
        self.assertIn("entries[0].tiers[0].input", err.getvalue())

    # -------------------------------------------------------------------- gcp

    def test_gcp_is_skipped_cleanly_without_a_key(self):
        fetch = Recorded()
        status, text = self.run_main(["--catalog", str(self.catalog())], fetch)
        self.assertEqual(status, 0)
        self.assertIn("source gcp: skipped: GCP_BILLING_API_KEY is not set", text)
        self.assertFalse(any("cloudbilling" in url for url, _ in fetch.requests))

    def test_gcp_compares_mapped_skus_and_keeps_the_key_out_of_urls(self):
        def edit(document):
            self.entry(document, "google-vertex-global", "gemini-3.1-flash-lite-image")["drift"] = {
                "gcp_skus": {"input": "SYN-0001", "cache_read": "SYN-0002", "output": "SYN-0003",
                             "output_image": "Synthetic: Gemini 3.1 Flash Lite Image image output "
                                             "tokens (global)"}}
        fetch = Recorded()
        os.environ[drift.GCP_KEY_ENV] = "test-key-not-a-secret"
        status, text = self.run_main(["--catalog", str(self.catalog(edit)), "--sources", "gcp"], fetch)
        self.assertEqual(status, 0, text)
        self.assertNotIn("test-key-not-a-secret", text)
        for url, headers in fetch.requests:
            self.assertNotIn("test-key", url)
            self.assertEqual(headers.get("X-Goog-Api-Key"), "test-key-not-a-secret")
        self.assertEqual(len(fetch.requests), 3)  # services, then two SKU pages
        report = self.report(self.catalog(edit), fetch=Recorded(), sources=["gcp"],
                             key="test-key-not-a-secret")
        self.assertEqual({(f.field, f.status, f.live) for f in report.findings},
                         {("input", "ok", "0.25"), ("cache_read", "ok", "0.025"),
                          ("output", "ok", "1.5"), ("output_image", "ok", "30")})

    def test_gcp_without_a_mapping_lists_candidate_skus(self):
        report = self.report(self.catalog(), sources=["gcp"], key="test-key-not-a-secret")
        self.assertEqual(report.sources["gcp"], "ok")
        notes = [f for f in report.findings if f.source == "gcp"]
        self.assertEqual(len(notes), 1)
        self.assertEqual(notes[0].status, "info")
        self.assertIn("no drift.gcp_skus mapping", notes[0].detail)

    # ---------------------------------------------------------------- numbers

    def test_prices_convert_exactly(self):
        self.assertEqual(drift.per_million("0.0000000028"), Decimal("0.0028"))
        self.assertEqual(drift.per_million(2.5e-07), Decimal("0.25"))
        self.assertEqual(drift.per_million(1.1e-06), Decimal("1.1"))
        self.assertEqual(drift.plain(drift.per_million("0.0000002156")), "0.2156")
        for bad in ("-1", "abc", None, True, float("nan")):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                drift.per_million(bad)

    def test_catalog_rate_grammar_matches_the_runtime(self):
        for good, value in (("0.10", "0.10"), ("30", "30"), ("0.00686", "0.00686"), ("10000", "10000")):
            self.assertEqual(drift.catalog_rate(good, "x"), Decimal(value))
        for bad in ("", "0.0000001", "-1", "+1", "1e3", ".5", "1.", "01", "123456", "10000.000001", 0.1):
            with self.subTest(bad=bad), self.assertRaises(drift.CatalogInvalid):
                drift.catalog_rate(bad, "x")


if __name__ == "__main__":
    unittest.main()
