#!/usr/bin/env python3
"""Run the live end-to-end suite with real provider keys, or verify a committed report.

A release is not ready until this suite has passed against the release commit. Local unit and integration tests
cannot show that a provider still accepts what NeoGraph sends, or that NeoGraph still accepts what it answers.

  run      Runs the Python live suite and the C++ live test binary with the keys from an env file and writes
           tests/live/report.json. Refuses a working tree with uncommitted changes, because the report must
           describe a commit.
  verify   Checks a committed report: it describes this tree (every tracked file but the report), this version,
           every case in tests/live/manifest.json has the expected result, and no case failed. CI runs it on
           release pull requests. It needs no key.
  manifest Rewrites tests/live/manifest.json from the cases the suite collects, keeping the known gaps.

Keys are read from the env file into the children's environment only. They are never printed or written, and
any key value that appears in a failure message is replaced before the report is written.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ElementTree
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPORT = Path("tests/live/report.json")
MANIFEST = Path("tests/live/manifest.json")
PYTHON_SUITE = ["bindings/python/tests/live", "bindings/python/tests/test_live_provider.py"]
KEY_VARIABLES = ("OPENAI_API_KEY", "GEMINI_API_KEY", "ANTHROPIC_API_KEY", "OPENROUTER_API_KEY")
FORMAT = "neograph.live-e2e/v1"


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout


def tree_hash(report=REPORT):
    """SHA-256 of the committed tree listing without the report itself."""
    lines = [line for line in git("ls-tree", "-r", "--full-tree", "HEAD").splitlines()
             if line.split("\t", 1)[1] != report.as_posix()]
    return hashlib.sha256("\n".join(lines).encode()).hexdigest()


def project_version():
    # CMakeLists.txt takes the version from pyproject.toml, as the Python distribution does.
    text = (ROOT / "pyproject.toml").read_text(encoding="utf-8")
    return re.search(r'^version\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"', text, re.M).group(1)


def schemaprovider_pin():
    text = (ROOT / "cmake/NeoGraphSchemaProvider.cmake").read_text(encoding="utf-8")
    match = re.search(r"\b([0-9a-f]{40})\b", text)
    return match.group(1) if match else None


def read_keys(env_file):
    keys = {}
    for line in Path(env_file).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if "=" not in line or line.startswith("#"):
            continue
        name, value = line.split("=", 1)
        name = name.strip().removeprefix("export ").strip()
        if name in KEY_VARIABLES or name.startswith("NG_LIVE_"):
            keys[name] = value.strip().strip('"').strip("'")
    return keys


def scrub(text, secrets):
    for secret in secrets:
        if len(secret) >= 8:
            text = text.replace(secret, "<redacted>")
    return text[:500]


def python_cases(junit, secrets):
    cases = {}
    for test in ElementTree.parse(junit).getroot().iter("testcase"):
        module = test.get("classname", "").rsplit(".", 1)[-1]
        case_id = f"py:{module}::{test.get('name')}"
        status, message = "passed", ""
        for child in test:
            if child.tag in ("failure", "error"):
                status, message = "failed", child.get("message") or (child.text or "")
            elif child.tag == "skipped":
                kind = child.get("type", "")
                status = "xfailed" if kind == "pytest.xfail" else "skipped"
                message = child.get("message", "")
        cases[case_id] = {"status": status, "seconds": round(float(test.get("time", 0)), 2),
                          "message": scrub(message, secrets)}
    return cases


def cxx_cases(report_json, secrets):
    cases = {}
    for suite in json.loads(Path(report_json).read_text(encoding="utf-8")).get("testsuites", []):
        for test in suite.get("testsuite", []):
            case_id = f"cpp:{suite['name']}.{test['name']}"
            failures = test.get("failures", [])
            status = "failed" if failures else "skipped" if test.get("result") == "SKIPPED" else "passed"
            message = failures[0].get("failure", "") if failures else ""
            seconds = float(str(test.get("time", "0s")).rstrip("s") or 0)
            cases[case_id] = {"status": status, "seconds": round(seconds, 2), "message": scrub(message, secrets)}
    return cases


def run(args):
    dirty = [line for line in git("status", "--porcelain").splitlines() if line[3:] != REPORT.as_posix()]
    if dirty and not args.allow_dirty:
        raise SystemExit("the working tree has uncommitted changes; commit them first (the report names a commit)")
    keys = read_keys(args.env_file)
    missing = [name for name in KEY_VARIABLES if not keys.get(name)]
    if missing and not args.allow_missing_keys:
        raise SystemExit(f"the env file has no {', '.join(missing)}")
    env = dict(os.environ, NEOGRAPH_E2E_LIVE="1", **keys)
    if args.pythonpath:
        env["PYTHONPATH"] = os.pathsep.join([args.pythonpath, env.get("PYTHONPATH", "")]).rstrip(os.pathsep)
    secrets = list(keys.values())
    version = subprocess.run([args.python, "-c", "import neograph_engine; print(neograph_engine.__version__)"],
                             env=env, check=True, capture_output=True, text=True).stdout.strip()
    if version != project_version():
        raise SystemExit(f"neograph_engine {version} is not this tree's version {project_version()}")
    started = time.time()
    cases = {}
    with tempfile.TemporaryDirectory() as scratch:
        junit = Path(scratch, "python.xml")
        command = [args.python, "-m", "pytest", "-q", "-p", "no:cacheprovider", "-rN", f"--junitxml={junit}",
                   *PYTHON_SUITE]
        if args.select:
            command += ["-k", args.select]
        subprocess.run(command, cwd=ROOT, env=env, stdout=sys.stdout, stderr=sys.stderr)
        cases.update(python_cases(junit, secrets))
        if args.cxx:
            gtest = Path(scratch, "cxx.json")
            command = [args.cxx, f"--gtest_output=json:{gtest}"]
            if args.cxx_filter:
                command.append(f"--gtest_filter={args.cxx_filter}")
            subprocess.run(command, cwd=ROOT, env=env, stdout=sys.stdout, stderr=sys.stderr)
            cases.update(cxx_cases(gtest, secrets))
    report = {
        "format": FORMAT,
        "neograph": project_version(),
        "python_module": version,
        "schemaprovider": schemaprovider_pin(),
        "commit": git("rev-parse", "HEAD").strip(),
        "tree": tree_hash(),
        "dirty": bool(dirty),
        "finished": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "seconds": round(time.time() - started, 1),
        "cases": dict(sorted(cases.items())),
    }
    (ROOT / REPORT).write_text(json.dumps(report, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    counts = {}
    for value in cases.values():
        counts[value["status"]] = counts.get(value["status"], 0) + 1
    print(f"wrote {REPORT}: " + ", ".join(f"{n} {s}" for s, n in sorted(counts.items())))
    problems = check(report)
    for problem in problems:
        print("NOT READY:", problem)
    return 1 if problems else 0


def check(report, verify_tree=False):
    """Every problem that keeps the report from approving a release."""
    problems = []
    if report.get("format") != FORMAT:
        return [f"unknown report format {report.get('format')!r}"]
    if verify_tree and report.get("tree") != tree_hash():
        problems.append("the report does not describe this tree; rerun scripts/live_e2e.py run on this commit")
    if report.get("dirty"):
        problems.append("the report was made from a working tree with uncommitted changes")
    if report.get("neograph") != project_version():
        problems.append(f"the report is for NeoGraph {report.get('neograph')}, this tree is {project_version()}")
    manifest = json.loads((ROOT / MANIFEST).read_text(encoding="utf-8"))
    gaps = manifest.get("known_gaps", {})
    cases = report.get("cases", {})
    for case_id in manifest["cases"]:
        expected = "xfailed" if case_id in gaps else "passed"
        status = cases.get(case_id, {}).get("status", "missing")
        if status != expected:
            problems.append(f"{case_id}: {status}, expected {expected}")
    for case_id, value in cases.items():
        if value["status"] == "failed" and case_id not in manifest["cases"]:
            problems.append(f"{case_id}: failed")
    return problems


def verify(args):
    path = ROOT / (args.report or REPORT)
    if not path.exists():
        print(f"NOT READY: {path.relative_to(ROOT)} is missing; run scripts/live_e2e.py run and commit the report")
        return 1
    report = json.loads(path.read_text(encoding="utf-8"))
    problems = check(report, verify_tree=True)
    passed = sum(1 for v in report.get("cases", {}).values() if v["status"] in ("passed", "xfailed"))
    print(f"live e2e report: NeoGraph {report.get('neograph')}, commit {report.get('commit', '?')[:12]}, "
          f"{passed}/{len(report.get('cases', {}))} cases passed or failed as known gaps")
    for problem in problems:
        print("NOT READY:", problem)
    if not problems:
        print("live e2e report verified")
    return 1 if problems else 0


def manifest(args):
    env = dict(os.environ, **({"PYTHONPATH": args.pythonpath} if args.pythonpath else {}))
    listing = subprocess.run([args.python, "-m", "pytest", "-q", "--collect-only", "-p", "no:cacheprovider",
                              *PYTHON_SUITE], cwd=ROOT, check=True, capture_output=True, text=True, env=env)
    cases = []
    for line in listing.stdout.splitlines():
        if "::" in line:
            path, name = line.split("::", 1)
            cases.append(f"py:{Path(path).stem}::{name}")
    if args.cxx:
        listing = subprocess.run([args.cxx, "--gtest_list_tests"], check=True, capture_output=True, text=True).stdout
        suite = None
        for line in listing.splitlines():
            if not line.startswith(" "):
                suite = line.strip().rstrip(".")
            elif suite:
                cases.append(f"cpp:{suite}.{line.split('#')[0].strip()}")
    path = ROOT / MANIFEST
    old = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    document = {"format": "neograph.live-e2e-manifest/v1",
                "known_gaps": {k: v for k, v in old.get("known_gaps", {}).items() if k in cases},
                "cases": sorted(cases)}
    path.write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8")
    print(f"wrote {MANIFEST}: {len(cases)} cases, {len(document['known_gaps'])} known gaps")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("run", "manifest"):
        sub = commands.add_parser(name)
        sub.add_argument("--python", default=sys.executable, help="interpreter that imports this tree's neograph_engine")
        sub.add_argument("--pythonpath", help="prepended to PYTHONPATH, e.g. a build directory")
        sub.add_argument("--cxx", help="the neograph_live_e2e_tests binary of this tree's build")
        if name == "run":
            sub.add_argument("--env-file", required=True, help="file with the provider key variables")
            sub.add_argument("--select", help="pytest -k expression (a partial run never approves a release)")
            sub.add_argument("--cxx-filter", help="gtest filter (a partial run never approves a release)")
            sub.add_argument("--allow-dirty", action="store_true", help="run anyway; the report is marked dirty")
            sub.add_argument("--allow-missing-keys", action="store_true")
    sub = commands.add_parser("verify")
    sub.add_argument("--report", help=f"report path relative to the repository (default {REPORT})")
    args = parser.parse_args()
    return {"run": run, "verify": verify, "manifest": manifest}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
