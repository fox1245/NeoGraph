"""Validate captured MCP wire messages against the official schema.json.

    python -I validate_transcripts.py SCHEMA_JSON TRANSCRIPT_DIR [OUT_JSON]

Every NeoGraph-emitted message (s2c) is validated; client messages (c2s) are
validated for transcripts produced by official SDK clients (py-*, ts-*), which
shows what NeoGraph accepted. Responses are matched to their request by id to
select the exact Result definition (InitializeResult, ListToolsResult, ...).
Each Tool's inputSchema/outputSchema is also checked against the JSON Schema
2020-12 meta-schema (the dialect MCP 2025-11-25 mandates by default).
"""

import json
import sys
from collections import Counter
from pathlib import Path

from jsonschema import Draft202012Validator

schema = json.loads(Path(sys.argv[1]).read_text())
defs = schema.get("$defs") or schema.get("definitions")
ref_root = "#/$defs/" if "$defs" in schema else "#/definitions/"
_validators: dict = {}


def validator(name: str) -> Draft202012Validator:
    if name not in _validators:
        _validators[name] = Draft202012Validator({"$ref": ref_root + name, "$defs": defs,
                                                  "definitions": defs})
    return _validators[name]


def method_defs(kind: str) -> dict:
    out = {}
    for ref in defs[kind]["anyOf"]:
        name = ref["$ref"].split("/")[-1]
        const = defs[name].get("properties", {}).get("method", {}).get("const")
        if const:
            out[const] = name
    return out


CLIENT_REQ = method_defs("ClientRequest")
CLIENT_NOTE = method_defs("ClientNotification")
RESULT_FOR = {m: (n[:-len("Request")] + "Result") for m, n in CLIENT_REQ.items()}
RESULT_FOR["ping"] = "EmptyResult" if "EmptyResult" in defs else "Result"


def errors(name: str, msg) -> list[str]:
    return [f"{'/'.join(map(str, e.absolute_path)) or '$'}: {e.message[:200]}"
            for e in validator(name).iter_errors(msg)]


def check(path: Path):
    findings, counts = [], Counter()
    sdk = path.name.startswith(("py-", "ts-"))
    pending: dict = {}
    for line in path.read_text().splitlines():
        rec = json.loads(line)
        raw = rec.get("raw") or ""
        if not raw.strip():
            continue
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            if rec["dir"] == "s2c":
                findings.append(("s2c", "not JSON", raw[:120], []))
            continue
        if rec["dir"] == "c2s":
            if isinstance(msg, dict) and "method" in msg and "id" in msg:
                pending[json.dumps(msg["id"])] = msg["method"]
            if not sdk or not isinstance(msg, dict):
                continue
            m = msg.get("method")
            name = CLIENT_REQ.get(m) if "id" in msg else CLIENT_NOTE.get(m)
            if m and name is None:
                counts["c2s non-2025-11-25 method"] += 1
                continue  # e.g. server/discover (2026-07-28 probe), validated elsewhere
            name = name or ("JSONRPCResultResponse" if "result" in msg else "JSONRPCErrorResponse")
            errs = errors(name, msg)
            counts["c2s checked"] += 1
            if errs:
                findings.append(("c2s", name, raw[:300], errs))
            continue
        if rec["dir"] != "s2c":
            continue
        if not isinstance(msg, dict):
            findings.append(("s2c", "non-object", raw[:300], ["top-level value is not an object"]))
            continue
        counts["s2c checked"] += 1
        if "error" in msg:
            errs = errors("JSONRPCErrorResponse", msg)
            label = f"JSONRPCErrorResponse (code {msg['error'].get('code')})"
        elif "result" in msg:
            method = pending.get(json.dumps(msg.get("id")))
            rname = RESULT_FOR.get(method, "Result")
            errs = errors("JSONRPCResultResponse", msg) + errors(rname, msg["result"])
            label = f"{rname} ({method})"
            if rname == "ListToolsResult":
                for tool in msg["result"].get("tools", []):
                    for key in ("inputSchema", "outputSchema"):
                        if key in tool:
                            for e in Draft202012Validator(
                                    Draft202012Validator.META_SCHEMA).iter_errors(tool[key]):
                                errs.append(f"tools[{tool.get('name')}].{key} meta-schema: "
                                            f"{e.message[:160]}")
        else:
            name = "JSONRPCRequest" if "id" in msg else "JSONRPCNotification"
            errs = errors(name, msg)
            label = name
        if errs:
            findings.append(("s2c", label, raw[:300], errs))
    return findings, counts


def main():
    out = {"schema": sys.argv[1], "files": {}}
    total = Counter()
    for path in sorted(Path(sys.argv[2]).glob("*.jsonl")):
        findings, counts = check(path)
        total.update(counts)
        out["files"][path.name] = {"counts": counts, "findings": [
            {"dir": d, "def": n, "message": m, "errors": e} for d, n, m, e in findings]}
        total["invalid"] += len(findings)
    out["total"] = total
    distinct = Counter()
    for f in out["files"].values():
        for x in f["findings"]:
            distinct[(x["dir"], x["def"], x["errors"][0] if x["errors"] else "")] += 1
    out["distinct"] = [{"dir": k[0], "def": k[1], "first_error": k[2], "occurrences": v}
                       for k, v in distinct.most_common()]
    text = json.dumps(out, indent=1, default=dict)
    if len(sys.argv) > 3:
        Path(sys.argv[3]).write_text(text)
    print(f"checked s2c={total['s2c checked']} c2s={total['c2s checked']} "
          f"invalid={total['invalid']}")
    for d in out["distinct"]:
        print(f"  INVALID x{d['occurrences']:<3} {d['dir']} {d['def']}: {d['first_error']}")
    return 1 if total["invalid"] else 0


if __name__ == "__main__":
    sys.exit(main())
