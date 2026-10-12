"""Validate captured ACP traffic against the official ACP JSON Schema.

Used inline by the interop tests (every transcript a test produces is checked)
and as a CLI over saved transcripts:

    python -I acp_schema_check.py transcripts/*.jsonl

Each transcript line is {"dir": "c2a"|"a2c", "raw": "<json-rpc message>"}
(written by acp_stdio_tap.py). For every message the schema definition is
picked from the schema's own annotations (`x-method`, `x-side`):

* request params    -> <X>Request       (x-side = receiving side)
* notification      -> <X>Notification
* success result    -> <X>Response      (method found by correlating the id)
* error             -> Error, and the code must be one ACP names

Agent-emitted messages (a2c) are the subject; client-emitted ones (c2a) are
checked too so a broken probe shows up.
"""

from __future__ import annotations

import collections
import json
import sys
from functools import lru_cache
from pathlib import Path

from jsonschema import Draft202012Validator

SPEC_DIR = Path(__file__).resolve().parent / "spec"


@lru_cache(maxsize=None)
def _schema():
    schema = json.loads((SPEC_DIR / "schema.json").read_text(encoding="utf-8"))
    meta = json.loads((SPEC_DIR / "meta.json").read_text(encoding="utf-8"))
    defs = schema["$defs"]
    index = {}
    for name, body in defs.items():
        method = body.get("x-method")
        side = body.get("x-side")
        if not method:
            continue
        kind = ("request" if name.endswith("Request") else
                "response" if name.endswith("Response") else
                "notification" if name.endswith("Notification") else None)
        if kind:
            index[(method, side, kind)] = name
    known_codes = {c.get("const") for c in defs["ErrorCode"]["anyOf"] if "const" in c}
    return defs, index, known_codes, meta


_validators: dict[str, Draft202012Validator] = {}


def _errors_of(defs, name, instance):
    if name not in _validators:
        _validators[name] = Draft202012Validator({"$defs": defs, "$ref": f"#/$defs/{name}"})
    out = []
    for err in sorted(_validators[name].iter_errors(instance), key=lambda e: list(e.absolute_path)):
        path = "/".join(str(p) for p in err.absolute_path) or "<root>"
        out.append(f"{path}: {err.message[:300]}")
    return out


def check_transcript(path) -> list[dict]:
    """One result per message: {where, dir, schema, ok, problems, policy, raw}."""
    defs, index, known_codes, meta = _schema()
    agent_methods = set(meta["agentMethods"].values())
    client_methods = set(meta["clientMethods"].values())
    protocol_methods = set(meta.get("protocolMethods", {}).values())
    results = []
    pending = {"c2a": {}, "a2c": {}}  # requests sent in dir -> id -> method
    for lineno, line in enumerate(Path(path).read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        entry = json.loads(line)
        direction = entry["dir"]
        sender = "agent" if direction == "a2c" else "client"
        receiver_side = "client" if sender == "agent" else "agent"
        problems, policy, target = [], [], None
        try:
            msg = json.loads(entry["raw"])
        except ValueError:
            msg = None
            problems.append("not JSON (stdout hygiene / framing)")
        if entry.get("invalid_utf8"):
            problems.append("not valid UTF-8")
        if msg is not None and not isinstance(msg, dict):
            problems.append("JSON-RPC batch/array or non-object message")
            msg = None
        if msg is not None:
            if msg.get("jsonrpc") != "2.0":
                problems.append('missing/incorrect "jsonrpc": "2.0"')
            has_id = "id" in msg
            if has_id and not (msg["id"] is None or isinstance(msg["id"], (str, int))
                               and not isinstance(msg["id"], bool)):
                problems.append(f"id has invalid type: {msg['id']!r}")
            if "method" in msg:
                method = msg["method"]
                kind = "request" if has_id else "notification"
                params = msg.get("params", {})
                if has_id:
                    pending[direction][json.dumps(msg["id"])] = method
                allowed = agent_methods if receiver_side == "agent" else client_methods
                if method.startswith("_"):
                    target = "(extension method)"
                elif method in protocol_methods:
                    target = index.get((method, "protocol", "notification"))
                    if target:
                        problems += _errors_of(defs, target, params)
                elif method not in allowed:
                    problems.append(f"method {method!r} is not a {receiver_side} method in meta.json")
                else:
                    target = index.get((method, receiver_side, kind))
                    if target is None:
                        problems.append(f"no schema definition for {kind} {method!r}")
                    else:
                        problems += _errors_of(defs, target, params)
            else:
                other = "c2a" if direction == "a2c" else "a2c"
                method = pending[other].pop(json.dumps(msg.get("id")), None)
                has_result, has_error = "result" in msg, "error" in msg
                if has_result == has_error:
                    problems.append("response must carry exactly one of result/error")
                if has_error:
                    target = "Error"
                    problems += _errors_of(defs, "Error", msg["error"])
                    code = msg["error"].get("code") if isinstance(msg["error"], dict) else None
                    if code not in known_codes:
                        policy.append(f"error code {code} is valid only via ErrorCode 'Other'")
                elif has_result:
                    if method is None:
                        problems.append("response id matches no outstanding request")
                    else:
                        # a2c responses answer client->agent requests (x-side agent);
                        # c2a responses answer agent->client requests (x-side client).
                        side = "agent" if direction == "a2c" else "client"
                        target = index.get((method, side, "response"))
                        if target:
                            problems += _errors_of(defs, target, msg["result"])
                        elif not method.startswith("_"):
                            problems.append(f"no schema definition for response to {method!r}")
        results.append({"where": f"{Path(path).name}:{lineno}", "dir": direction,
                        "schema": target, "ok": not problems, "problems": problems,
                        "policy": policy, "raw": entry["raw"][:600]})
    return results


def agent_problems(path) -> list[str]:
    """Agent-emitted messages that fail the schema or use an unnamed error code."""
    out = []
    for r in check_transcript(path):
        if r["dir"] != "a2c":
            continue
        for p in r["problems"] + r["policy"]:
            out.append(f"{r['where']} {r['schema']}: {p}\n      wire: {r['raw']}")
    return out


def main(argv) -> int:
    failed = 0
    total = 0
    counts = collections.Counter()
    for path in argv:
        for r in check_transcript(path):
            if r["dir"] != "a2c":
                continue
            total += 1
            counts[r["schema"]] += 1
            if r["problems"] or r["policy"]:
                failed += 1
                print(f"FAIL {r['where']} {r['schema']}")
                for p in r["problems"] + r["policy"]:
                    print(f"      - {p}")
                print(f"      wire: {r['raw']}")
    print(f"agent-emitted messages: {total}, failing schema or using an unnamed code: {failed}")
    for name, n in sorted(counts.items(), key=lambda kv: str(kv[0])):
        print(f"  {n:4d}  {name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
