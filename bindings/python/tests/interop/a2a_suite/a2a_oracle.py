"""Machine-readable A2A oracles for wire messages.

1.0 (ProtoJSON, `lf.a2a.v1`):
  * strict ProtoJSON parse into the generated descriptor shipped with a2a-sdk
    (unknown fields and bad enum names are errors),
  * google.api.field_behavior REQUIRED walk (spec section 5.7: a required array
    must have at least one element). The REQUIRED fields listed in
    EMPTY_MEANS_NONE may hold their empty value when the key is on the wire:
    their proto comments define it ("empty if there are no more results"),
    and the reference SDK servers emit it (always_print_fields_with_no_presence),
  * the published JSON Schema bundle (spec/a2a-v1.schema.json).
0.3 (JSON-RPC dialect): the normative draft-07 JSON Schema (spec/a2a-v0.3.0.schema.json)
and the SDK's pydantic models generated from it.

Every validator returns a list of human-readable problems (empty == valid).
"""

from __future__ import annotations

import copy
import json
import os
from functools import lru_cache
from pathlib import Path
from typing import Any

import jsonschema
from google.api import field_behavior_pb2
from google.protobuf import json_format
from google.protobuf.descriptor import FieldDescriptor

from a2a.types import a2a_pb2

SPEC_DIR = Path(os.environ.get("A2A_SPEC_DIR", Path(__file__).resolve().parent / "spec"))

_REQUIRED = field_behavior_pb2.REQUIRED
# REQUIRED fields whose empty value the proto defines (a last or empty page).
EMPTY_MEANS_NONE = {
    "lf.a2a.v1.ListTasksResponse.tasks",
    "lf.a2a.v1.ListTasksResponse.next_page_token",
    "lf.a2a.v1.ListTasksResponse.total_size",
}
_WELL_KNOWN = {"google.protobuf.Struct", "google.protobuf.Value", "google.protobuf.ListValue",
               "google.protobuf.Timestamp"}


@lru_cache(maxsize=None)
def _schema(name: str) -> dict[str, Any]:
    return json.loads((SPEC_DIR / name).read_text())


def _required_problems(msg, path: str, wire: Any = None) -> list[str]:
    out: list[str] = []
    desc = msg.DESCRIPTOR
    if desc.full_name in _WELL_KNOWN:
        return out
    wire = wire if isinstance(wire, dict) else {}
    for f in desc.fields:
        req = _REQUIRED in list(f.GetOptions().Extensions[field_behavior_pb2.field_behavior])
        fpath = f"{path}.{f.json_name}" if path else f.json_name
        on_wire = f.json_name in wire or f.name in wire
        sub = wire.get(f.json_name, wire.get(f.name))
        is_map = f.message_type is not None and f.message_type.GetOptions().map_entry
        repeated = f.is_repeated if hasattr(f, "is_repeated") else (
            f.label == FieldDescriptor.LABEL_REPEATED)
        if repeated:
            values = getattr(msg, f.name)
            empty_ok = f.full_name in EMPTY_MEANS_NONE and on_wire
            if req and len(values) == 0 and not empty_ok:
                out.append(f"{fpath}: REQUIRED repeated field is missing or empty")
            if is_map:
                vdesc = f.message_type.fields_by_name["value"]
                if vdesc.message_type is not None:
                    for k, v in values.items():
                        out += _required_problems(
                            v, f"{fpath}[{k!r}]", (sub or {}).get(k) if isinstance(sub, dict) else None)
            elif f.message_type is not None:
                items = sub if isinstance(sub, list) else []
                for i, v in enumerate(values):
                    out += _required_problems(v, f"{fpath}[{i}]", items[i] if i < len(items) else None)
            continue
        if f.message_type is not None:
            if msg.HasField(f.name):
                out += _required_problems(getattr(msg, f.name), fpath, sub)
            elif req:
                out.append(f"{fpath}: REQUIRED field is missing")
            continue
        if req:
            value = getattr(msg, f.name)
            if f.has_presence:
                present = msg.HasField(f.name)
            else:
                present = value != f.default_value or (
                    on_wire and f.full_name in EMPTY_MEANS_NONE)
            if not present:
                out.append(f"{fpath}: REQUIRED field is missing or empty")
    return out


def validate_v1(type_name: str, obj: Any) -> list[str]:
    """Validate a 1.0 ProtoJSON object against lf.a2a.v1.<type_name>."""
    problems: list[str] = []
    cls = getattr(a2a_pb2, type_name)
    msg = cls()
    try:
        json_format.ParseDict(copy.deepcopy(obj), msg, ignore_unknown_fields=False)
    except json_format.ParseError as e:
        problems.append(f"ProtoJSON({type_name}): {e}")
        # keep going with a lenient parse so REQUIRED problems are still reported
        msg = cls()
        try:
            json_format.ParseDict(copy.deepcopy(obj), msg, ignore_unknown_fields=True)
        except json_format.ParseError:
            return problems
    problems += [f"REQUIRED({type_name}): {p}" for p in _required_problems(msg, "", obj)]
    root = _schema("a2a-v1.schema.json")
    defs = root.get("$defs") or root.get("definitions")
    if type_name in defs:
        schema = {"$schema": root["$schema"], "$defs": defs, "$ref": f"#/$defs/{type_name}"}
        v = jsonschema.Draft202012Validator(schema)
        for err in sorted(v.iter_errors(obj), key=lambda e: list(e.absolute_path)):
            loc = "/".join(str(x) for x in err.absolute_path) or "<root>"
            problems.append(f"JSONSchema({type_name}) at {loc}: {err.message[:300]}")
    return problems


def validate_v03(def_name: str, obj: Any) -> list[str]:
    """Validate a 0.3 object against the normative draft-07 schema definition."""
    root = _schema("a2a-v0.3.0.schema.json")
    schema = {"$schema": root["$schema"], "definitions": root["definitions"],
              "$ref": f"#/definitions/{def_name}"}
    v = jsonschema.Draft7Validator(schema)
    problems = []
    for err in sorted(v.iter_errors(obj), key=lambda e: list(e.absolute_path)):
        loc = "/".join(str(x) for x in err.absolute_path) or "<root>"
        msg = err.message
        if err.context:
            best = min(err.context, key=lambda c: len(list(c.absolute_path)))
            msg += f" (best match: {best.message[:200]})"
        problems.append(f"JSONSchema0.3({def_name}) at {loc}: {msg[:400]}")
    try:
        from a2a.compat.v0_3 import types as t03  # pydantic models generated from the 0.3 schema
        model = getattr(t03, def_name, None)
        if model is not None:
            model.model_validate(obj)
    except Exception as e:  # pydantic.ValidationError
        problems.append(f"pydantic0.3({def_name}): {str(e)[:400]}")
    return problems


def v1_result_type(method: str) -> str:
    return {
        "SendMessage": "SendMessageResponse",
        "SendStreamingMessage": "StreamResponse",
        "SubscribeToTask": "StreamResponse",
        "GetTask": "Task",
        "CancelTask": "Task",
        "ListTasks": "ListTasksResponse",
        "GetExtendedAgentCard": "AgentCard",
    }[method]


def v1_request_type(method: str) -> str:
    return {
        "SendMessage": "SendMessageRequest",
        "SendStreamingMessage": "SendMessageRequest",
        "GetTask": "GetTaskRequest",
        "CancelTask": "CancelTaskRequest",
        "ListTasks": "ListTasksRequest",
        "SubscribeToTask": "SubscribeToTaskRequest",
        "GetExtendedAgentCard": "GetExtendedAgentCardRequest",
    }[method]


V03_RESPONSE_DEF = {
    "message/send": "SendMessageResponse",
    "message/stream": "SendStreamingMessageResponse",
    "tasks/get": "GetTaskResponse",
    "tasks/cancel": "CancelTaskResponse",
}
V03_REQUEST_DEF = {
    "message/send": "SendMessageRequest",
    "message/stream": "SendStreamingMessageRequest",
    "tasks/get": "GetTaskRequest",
    "tasks/cancel": "CancelTaskRequest",
    "tasks/resubscribe": "TaskResubscriptionRequest",
}

# JSON-RPC error codes derived from the spec (section 5.4 / 9.5); 0.3 uses the
# same numbers for its first six A2A errors.
A2A_ERROR_CODES = {
    "TaskNotFoundError": -32001,
    "TaskNotCancelableError": -32002,
    "PushNotificationNotSupportedError": -32003,
    "UnsupportedOperationError": -32004,
    "ContentTypeNotSupportedError": -32005,
    "InvalidAgentResponseError": -32006,
    "ExtendedAgentCardNotConfiguredError": -32007,
    "ExtensionSupportRequiredError": -32008,
    "VersionNotSupportedError": -32009,
}
JSONRPC_CODES = {"ParseError": -32700, "InvalidRequest": -32600, "MethodNotFound": -32601,
                 "InvalidParams": -32602, "InternalError": -32603}


def v1_method_set() -> list[str]:
    """JSON-RPC method names = A2AService rpc names in the normative proto."""
    return [m.name for m in a2a_pb2.DESCRIPTOR.services_by_name["A2AService"].methods]
