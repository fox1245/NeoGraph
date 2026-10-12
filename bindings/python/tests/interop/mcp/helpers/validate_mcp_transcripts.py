"""Validate captured MCP wire messages against the official schema.json.

    python -I validate_mcp_transcripts.py SCHEMA_DIR TRANSCRIPT_DIR [OUT_JSON]

SCHEMA_DIR holds <revision>/schema.json. Each transcript is checked against the
schema of the revision its initialize result negotiated (the 2025-11-25 schema
when that revision's is missing, which the output notes). Every NeoGraph-emitted
message (s2c) is validated; client messages (c2s) are validated for transcripts
produced by official SDK clients (py-*, ts-*), which shows what NeoGraph
accepted. Responses are matched to their request by id to select the exact
Result definition (InitializeResult, ListToolsResult, ...). A JSON-RPC batch
(an array) is checked message by message; only 2025-03-26 defines batches, and
NeoGraph also answers them for 2024-11-05 peers. Each Tool's inputSchema and
outputSchema is also checked against the JSON Schema 2020-12 meta-schema.
"""

import json
import sys
from collections import Counter
from pathlib import Path

from jsonschema import Draft202012Validator

NEWEST = "2025-11-25"
BATCH_REVISIONS = ("2025-03-26", "2024-11-05")


class Schema:
    """One revision's schema.json and the definitions the checks use."""

    def __init__(self, path: Path):
        schema = json.loads(path.read_text())
        self.defs = schema.get("$defs") or schema.get("definitions")
        self.ref_root = "#/$defs/" if "$defs" in schema else "#/definitions/"
        self._validators: dict = {}
        self.client_req = self._method_defs("ClientRequest")
        self.client_note = self._method_defs("ClientNotification")
        self.result_for = {m: (n[:-len("Request")] + "Result") for m, n in self.client_req.items()}
        self.result_for["ping"] = "EmptyResult" if "EmptyResult" in self.defs else "Result"
        self.result_def = ("JSONRPCResultResponse" if "JSONRPCResultResponse" in self.defs
                           else "JSONRPCResponse")
        self.error_def = ("JSONRPCErrorResponse" if "JSONRPCErrorResponse" in self.defs
                          else "JSONRPCError")

    def _method_defs(self, kind: str) -> dict:
        out = {}
        for ref in self.defs[kind]["anyOf"]:
            name = ref["$ref"].split("/")[-1]
            const = self.defs[name].get("properties", {}).get("method", {}).get("const")
            if const:
                out[const] = name
        return out

    def errors(self, name: str, msg) -> list[str]:
        if name not in self._validators:
            self._validators[name] = Draft202012Validator(
                {"$ref": self.ref_root + name, "$defs": self.defs, "definitions": self.defs})
        return [f"{'/'.join(map(str, e.absolute_path)) or '$'}: {e.message[:200]}"
                for e in self._validators[name].iter_errors(msg)]


_schemas: dict = {}


def schema_for(schema_dir: Path, revision: str, counts: Counter) -> Schema:
    path = schema_dir / revision / "schema.json"
    if not path.exists():
        counts[f"checked with the {NEWEST} schema (no {revision} schema)"] += 1
        path = schema_dir / NEWEST / "schema.json"
    if path not in _schemas:
        _schemas[path] = Schema(path)
    return _schemas[path]


def records(path: Path) -> list[tuple[str, object, str]]:
    out = []
    for line in path.read_text().splitlines():
        rec = json.loads(line)
        raw = rec.get("raw") or ""
        if not raw.strip():
            continue
        try:
            out.append((rec["dir"], json.loads(raw), raw))
        except json.JSONDecodeError:
            out.append((rec["dir"], None, raw))
    return out


def negotiated(recs) -> str:
    for direction, msg, _ in recs:
        if direction == "s2c" and isinstance(msg, dict):
            version = (msg.get("result") or {}).get("protocolVersion")
            if isinstance(version, str):
                return version
    return NEWEST


def check(path: Path, schema_dir: Path):
    findings, counts = [], Counter()
    sdk = path.name.startswith(("py-", "ts-"))
    recs = records(path)
    revision = negotiated(recs)
    schema = schema_for(schema_dir, revision, counts)
    counts[f"revision {revision}"] += 1
    pending: dict = {}

    def client_message(msg, raw):
        if not isinstance(msg, dict):
            return
        if "method" in msg and "id" in msg:
            pending[json.dumps(msg["id"])] = msg["method"]
        if not sdk:
            return
        m = msg.get("method")
        name = schema.client_req.get(m) if "id" in msg else schema.client_note.get(m)
        if m and name is None:
            counts[f"c2s method outside {revision}"] += 1
            return  # e.g. server/discover (2026-07-28 probe), validated elsewhere
        name = name or (schema.result_def if "result" in msg else schema.error_def)
        errs = schema.errors(name, msg)
        counts["c2s checked"] += 1
        if errs:
            findings.append(("c2s", name, raw[:300], errs))

    def server_message(msg, raw):
        if not isinstance(msg, dict):
            findings.append(("s2c", "non-object", raw[:300], ["message is not an object"]))
            return
        counts["s2c checked"] += 1
        if "error" in msg:
            errs = schema.errors(schema.error_def, msg)
            label = f"{schema.error_def} (code {msg['error'].get('code')})"
        elif "result" in msg:
            method = pending.get(json.dumps(msg.get("id")))
            rname = schema.result_for.get(method, "Result")
            errs = schema.errors(schema.result_def, msg) + schema.errors(rname, msg["result"])
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
            errs = schema.errors(name, msg)
            label = name
        if errs:
            findings.append(("s2c", f"{label} [{revision}]", raw[:300], errs))

    for direction, msg, raw in recs:
        if msg is None:
            if direction == "s2c":
                findings.append(("s2c", "not JSON", raw[:120], []))
            continue
        if direction == "c2s":
            handle = client_message
        elif direction == "s2c":
            handle = server_message
        else:
            continue
        if isinstance(msg, list):
            if direction == "s2c":
                counts["s2c batches"] += 1
                if revision not in BATCH_REVISIONS:
                    findings.append(("s2c", f"batch [{revision}]", raw[:300],
                                     [f"{revision} has no JSON-RPC batches"]))
                if not msg:
                    findings.append(("s2c", "empty batch", raw[:300], ["an empty array"]))
            for item in msg:
                handle(item, json.dumps(item))
            continue
        handle(msg, raw)
    return findings, counts


def main():
    schema_dir = Path(sys.argv[1])
    out = {"schemas": str(schema_dir), "files": {}}
    total = Counter()
    for path in sorted(Path(sys.argv[2]).glob("*.jsonl")):
        findings, counts = check(path, schema_dir)
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
    revisions = ", ".join(f"{k[len('revision '):]}={v}" for k, v in sorted(total.items())
                          if k.startswith("revision "))
    print(f"checked s2c={total['s2c checked']} c2s={total['c2s checked']} "
          f"batches={total['s2c batches']} invalid={total['invalid']} ({revisions})")
    for key, value in sorted(total.items()):
        if key.startswith("checked with"):
            print(f"  NOTE x{value} {key}")
    for d in out["distinct"]:
        print(f"  INVALID x{d['occurrences']:<3} {d['dir']} {d['def']}: {d['first_error']}")
    return 1 if total["invalid"] else 0


if __name__ == "__main__":
    sys.exit(main())
