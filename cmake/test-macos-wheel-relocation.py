#!/usr/bin/env python3
"""Post-repair, installed-wheel gate; append AFTER the unchanged full pytest run.

CI invocation (the wheel's CPython interpreter, once per cp39--cp313 wheel):
    python -I "{package}/cmake/test-macos-wheel-relocation.py"

Requires an actual arm64 macOS 14 runner and /usr/bin/sandbox-exec. No build
stage is sandboxed, and no global files or permissions are changed. A fresh
exec'd interpreter imports the original installed distribution, not a copied
wheel. Its Seatbelt profile denies reads of Homebrew opt/Cellar trees. An owned
readable canary proves enforcement; existing LLVM runtime aliases and resolved
Cellar files additionally prove denial at the real dependency paths.

Assumptions and primary/source documentation:
* sandbox-exec(1), -f executes the command inside the specified profile:
  https://manp.gs/mac/1/sandbox-exec (deprecated, so availability is a hard gate).
* SBPL subpath covers the path and descendants; Chromium's implementation docs:
  https://chromium.googlesource.com/chromium/src/+/main/sandbox/mac/seatbelt_sandbox_design.md
* Apple's image enumeration API (not thread-safe). Snapshots are taken before
  native work and after all calls finish; no concurrent dlopen is requested:
  https://github.com/apple-oss-distributions/dyld/blob/main/include/mach-o/dyld.h
* Apple's Mach-O structures and minimum-version encoding:
  https://github.com/apple-oss-distributions/xnu/blob/main/EXTERNAL_HEADERS/mach-o/loader.h

Mapped-image and load-command checks supplement concrete graph, SDK request /
completion / streaming / failure, and QuickJS Program output oracles. They do
not substitute for behavior, full pytest, or proof on macOS itself.
"""

import base64
import ctypes
import errno
import importlib.metadata
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import sysconfig
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer


DENIED_ROOTS = ("/opt/homebrew/opt", "/opt/homebrew/Cellar",
                "/usr/local/opt", "/usr/local/Cellar")
SDK_COMPONENTS = ("core", "json", "descriptor", "codecs", "transport", "runtime")
MARKER = "NEOGRAPH_MACOS_RELOCATION_RESULT="
TEXT = "local arithmetic: 42"
MODEL = "relocation-local-model"
OPERATIONS = ("buffered:6*7", "stream:6*7", "reject:6*7", "graph:6*7")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def platform_gate():
    require(sys.platform == "darwin", "macOS runtime proof requires Darwin; no Linux soft pass")
    require(platform.mac_ver()[0].split(".")[0] == "14", "run this floor gate on macOS 14")
    require(platform.machine() == "arm64", "run this wheel gate natively on arm64")
    require(sys.implementation.name == "cpython" and (3, 9) <= sys.version_info[:2] <= (3, 13),
            "the release gate requires CPython 3.9 through 3.13")


def images():
    dyld = ctypes.CDLL(None)
    count = dyld._dyld_image_count
    count.argtypes = []
    count.restype = ctypes.c_uint32
    name = dyld._dyld_get_image_name
    name.argtypes = [ctypes.c_uint32]
    name.restype = ctypes.c_char_p
    result = set()
    for index in range(count()):
        value = name(index)
        require(value is not None, "dyld image disappeared during snapshot")
        result.add(str(Path(os.fsdecode(value)).resolve()))
    return result


def system_image(path):
    return path.startswith(("/usr/lib/", "/System/Library/"))


def macho(path):
    """Read only headers/load commands, selecting arm64 in a universal binary."""
    with path.open("rb") as stream:
        first = stream.read(8)
        require(len(first) == 8, "truncated Mach-O: " + str(path))
        offset = 0
        if first[:4] in (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):
            wide = first[:4] == b"\xca\xfe\xba\xbf"
            count = struct.unpack(">I", first[4:])[0]
            require(0 < count <= 32, "invalid universal slice count")
            size = 32 if wide else 20
            slices = stream.read(count * size)
            require(len(slices) == count * size, "truncated universal header")
            choices = []
            for index in range(count):
                row = slices[index * size:(index + 1) * size]
                cpu = struct.unpack_from(">I", row)[0]
                if cpu == 0x0100000C:
                    choices.append(struct.unpack_from(">Q" if wide else ">I", row, 8)[0])
            require(len(choices) == 1, "expected one arm64 Mach-O slice: " + str(path))
            offset = choices[0]
        stream.seek(offset)
        header = stream.read(32)
        require(len(header) == 32 and header[:4] == b"\xcf\xfa\xed\xfe",
                "expected little-endian 64-bit Mach-O: " + str(path))
        fields = struct.unpack("<8I", header)
        require(fields[1] == 0x0100000C, "packaged native binary lacks arm64: " + str(path))
        count, length = fields[4:6]
        require(count <= 65536 and length <= 16 * 1024 * 1024, "invalid Mach-O command bounds")
        commands = stream.read(length)
        require(len(commands) == length, "truncated Mach-O commands")
    result = {"id": None, "dependencies": [], "rpaths": [], "minimum_macos": []}
    cursor = 0
    for _ in range(count):
        require(cursor + 8 <= length, "truncated load command header")
        cmd, size = struct.unpack_from("<II", commands, cursor)
        require(size >= 8 and size % 8 == 0 and cursor + size <= length,
                "invalid Mach-O load command size")
        row = commands[cursor:cursor + size]
        if cmd in (0xC, 0xD, 0x80000018, 0x8000001F, 0x20, 0x80000023, 0x8000001C):
            require(size >= (12 if cmd == 0x8000001C else 24), "short dylib/rpath command")
            string_offset = struct.unpack_from("<I", row, 8)[0]
            require(12 <= string_offset < size and b"\0" in row[string_offset:],
                    "invalid dylib/rpath string")
            value = row[string_offset:].split(b"\0", 1)[0].decode("utf-8")
            if cmd == 0xD:
                result["id"] = value
            else:
                key = "rpaths" if cmd == 0x8000001C else "dependencies"
                result[key].append(value)
                require(system_image(value) or value.startswith(("@loader_path/", "@rpath/", "@executable_path/"))
                        or (key == "rpaths" and value == "@loader_path"),
                        "unrepaired build-host dependency/search path in %s: %s" % (path, value))
        elif cmd == 0x32:
            require(size >= 24, "short LC_BUILD_VERSION")
            target, minimum = struct.unpack_from("<II", row, 8)
            require(target == 1, "packaged native binary targets a non-macOS platform")
            result["minimum_macos"].append(minimum)
        elif cmd == 0x24:
            require(size >= 16, "short LC_VERSION_MIN_MACOSX")
            result["minimum_macos"].append(struct.unpack_from("<I", row, 8)[0])
        cursor += size
    require(cursor == length, "Mach-O command count/size mismatch")
    require(result["minimum_macos"] and all(0 < v <= (14 << 16) for v in result["minimum_macos"]),
            "packaged binary is missing floor metadata or requires newer than macOS 14.0: " + str(path))
    result["minimum_macos"] = ["%d.%d.%d" % (v >> 16, (v >> 8) & 255, v & 255)
                                for v in result["minimum_macos"]]
    return result


def installed_inventory():
    dist = importlib.metadata.distribution("neograph-engine")
    require(dist.version == "0.13.0", "expected the installed NeoGraph 0.13.0 wheel")
    direct = dist.read_text("direct_url.json")
    require(not direct or not json.loads(direct).get("dir_info", {}).get("editable", False),
            "editable/source installs are not wheel proof")
    require(dist.files is not None, "installed wheel has no RECORD file inventory")
    recorded = {str(item): Path(dist.locate_file(item)).resolve() for item in dist.files}
    require("neograph_engine/__init__.py" in recorded, "wheel RECORD lacks package entry point")
    native = {path for path in recorded.values() if path.name.endswith((".so", ".dylib"))}
    require(native, "installed wheel contains no native binaries")
    return recorded, {str(path): macho(path) for path in sorted(native)}


def python_dependency_inventory(recorded):
    """Pydantic's declared native dependency is not a NeoGraph wheel member."""
    dist = importlib.metadata.distribution("pydantic-core")
    require(dist.files is not None, "pydantic-core has no RECORD inventory")
    site = recorded["neograph_engine/__init__.py"].parent.parent
    native = {}
    for item in dist.files:
        if not str(item).startswith("pydantic_core/") or not str(item).endswith(".so"):
            continue
        path = Path(dist.locate_file(item)).resolve()
        require(path.parent == site / "pydantic_core" and path.is_file() and
                re.fullmatch(r"_pydantic_core(?:\.[^/]+)?\.so", path.name),
                "pydantic-core extension is outside the installed dependency")
        require(item.hash is not None and item.hash.mode == "sha256",
                "pydantic-core extension lacks a SHA-256 RECORD digest")
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(128 * 1024), b""):
                digest.update(block)
        actual = base64.urlsafe_b64encode(digest.digest()).rstrip(b"=").decode("ascii")
        require(actual == item.hash.value, "pydantic-core extension differs from RECORD")
        native[str(path)] = macho(path)
    require(len(native) == 1, "expected one RECORD-verified pydantic-core extension")
    return native


def behavior(ng, port):
    class Doubler(ng.GraphNode):
        def __init__(self, name):
            super().__init__()
            self._name = name

        def get_name(self):
            return self._name

        def run(self, incoming):
            return [ng.ChannelWrite("doubled", incoming.state.get("seed") * 2)]

    ng.NodeFactory.register_type("relocation_double", lambda name, _config, _ctx: Doubler(name))
    graph = {"name": "relocation-arithmetic", "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
             "channels": {"seed": {"reducer": "overwrite"}, "doubled": {"reducer": "overwrite"}},
             "nodes": {"double": {"type": "relocation_double"}},
             "edges": [{"from": ng.START_NODE, "to": "double"},
                       {"from": "double", "to": ng.END_NODE}]}
    engine = ng.GraphEngine.compile(graph, ng.NodeContext())
    for seed, expected in ((21, 42), (-7, -14)):
        output = engine.run(ng.RunConfig(thread_id="relocation-%d" % seed, input={"seed": seed})).output
        require(output["channels"]["seed"]["value"] == seed and
                output["channels"]["doubled"]["value"] == expected, "graph state/reducer arithmetic failed")

    import neograph_engine.llm as nglm
    descriptor = {"descriptor_version": 1, "revision": 1, "id": "mac-relocation-local-chat",
                  "family": "openai.chat", "connection": {"base_url": "http://127.0.0.1:%d" % port,
                  "paths": {"buffered": "/v1/chat/completions", "streaming": "/v1/chat/completions"}},
                  "bindings": {"model": "model", "messages": "messages", "stream": "stream",
                               "max_output_tokens": "max_tokens", "usage": ["usage"]},
                  "stop_reasons": {"stop": "EndTurn", "length": "MaxTokens", "tool_calls": "ToolUse"}}
    options = ng.ProviderRuntimeOptions(api_key="", default_timeout_ms=5000, workers=1)
    options.http_version = ng.ProviderHttpVersion.Http1_1
    provider = nglm.SchemaProvider(ng.load_provider_descriptor(json.dumps(descriptor)), options)

    def request(operation, mode=ng.ProviderMode.Collect):
        value = ng.make_provider_request(provider, MODEL,
                [ng.ProviderMessage(ng.ProviderRole.User, [ng.Text(operation)])], mode=mode)
        value.timeout_ms = 5000
        retry = ng.ProviderRetryPolicy()
        retry.enabled = False
        value.retry = retry
        return value

    def completion(outcome):
        require(outcome.failure is None and outcome.completion is not None, "local native completion failed")
        value = outcome.completion
        require(outcome.text == TEXT and len(value.messages) == 1 and
                value.messages[0].parts[0].value == TEXT, "native text projection differs from local wire response")
        require(value.stop.kind == ng.ProviderStopKind.EndTurn and value.stop.raw == "stop",
                "native stop reason projection failed")
        require((value.usage.input_total.value, value.usage.output_total.value, value.usage.total.value)
                == (8, 3, 11), "native usage accounting differs from reported counters")
        require(value.messages[0].native is not None, "native provider history owner was dropped")
        return value

    prepared = provider.prepare(request(OPERATIONS[0]))
    buffered = completion(provider.dispatch(prepared))
    raw = [event.payload for event in buffered.raw_events if event.type == "chat.completion"]
    require(len(raw) == 1 and raw[0]["id"] == "relocation-buffered" and
            raw[0]["choices"][0]["message"]["content"] == TEXT, "owned buffered wire evidence was lost")
    events = []
    streaming_request = request(OPERATIONS[1], ng.ProviderMode.Stream)
    streaming_request.on_event = events.append
    completion(provider.invoke(streaming_request))
    require([event.value.bytes for event in events if event.kind == "PartDelta"]
            == ["local ", "arithmetic: ", "42"], "SSE deltas were lost, reordered, or duplicated")
    failure = provider.invoke(request(OPERATIONS[2]))
    require(failure.completion is None and failure.failure is not None and
            failure.failure.error.http_status == 429 and failure.failure.partial.usage.total is None,
            "native HTTP refusal did not produce the owned 429 failure")

    llm_graph = {"name": "relocation-native-provider", "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
                 "channels": {"messages": {"reducer": "append"}}, "nodes": {"llm": {"type": "llm_call"}},
                 "edges": [{"from": ng.START_NODE, "to": "llm"}, {"from": "llm", "to": ng.END_NODE}]}
    llm_engine = ng.GraphEngine.compile(llm_graph, ng.NodeContext(provider=provider, model=MODEL))
    result = llm_engine.run(ng.RunConfig(thread_id="relocation-native",
                           input={"messages": [{"role": "user", "content": OPERATIONS[3]}]}))
    require(result.output["channels"]["messages"]["value"][-1]["content"] == TEXT and
            [outcome.text for outcome in result.provider_outcomes] == [TEXT] and
            result.native_messages[-1].parts[0].value == TEXT, "native provider graph output/history failed")
    completion(result.provider_outcomes[0])

    registry = ng.ProgramRegistryBuilder()
    registry.add_registered_node("relocation_double", "1.0.0", "sha256:" + "1" * 64)
    registry.add_registered_reducer("overwrite", "1.0.0", "sha256:" + "2" * 64)
    budget = ng.ProgramRunBudget()
    for name, value in (("wall_time_ms", 10000), ("model_tokens", 1000), ("monetary_microunits", 1000),
                        ("max_concurrency", 2), ("max_program_operations", 32), ("max_core_steps", 20),
                        ("max_dynamic_compiles", 0), ("max_child_depth", 1), ("max_total_children", 4)):
        setattr(budget, name, value)
    ceiling = ng.ProgramRunBudget()
    for name in ("wall_time_ms", "model_tokens", "monetary_microunits", "max_concurrency",
                 "max_program_operations", "max_core_steps", "max_child_depth", "max_total_children"):
        setattr(ceiling, name, getattr(budget, name))
    ceiling.max_dynamic_compiles = 2
    source = ng.ProgramSource.from_javascript("mac-relocation.js", """
export function define() {
  const graph = ng.graph("main");
  graph.channel("seed", {reducer: "overwrite", initial: 0});
  graph.channel("doubled", {reducer: "overwrite", initial: 0});
  graph.node("double", {type: "relocation_double"});
  graph.entry("double"); graph.exit("double");
  return graph;
}
export function* main(input) { return yield ng.callCore("main", input, "relocation:main"); }
""")
    host = ng.LocalProgramHost(registry.build(), "mac-relocation-owner", ceiling, "mac-relocation/v1")
    version = host.compile_admit(source, budget)
    program = host.run(version, {"seed": 9}, budget, "mac-relocation-program")
    require(program.status == ng.ProgramTerminalStatus.Completed and
            program.program_version_id == version.id and program.execution_trace == ["double"] and
            program.output["channels"]["seed"]["value"] == 9 and
            program.output["channels"]["doubled"]["value"] == 18, "QuickJS Program execution/output failed")
    return {"graph": {"21": 42, "-7": -14}, "provider_text": TEXT, "provider_usage": [8, 3, 11],
            "provider_http_failure": 429, "stream_deltas": ["local ", "arithmetic: ", "42"],
            "program": {"seed": 9, "doubled": 18, "trace": ["double"]}}


def worker(config_path):
    platform_gate()
    config = json.loads(Path(config_path).read_text())
    denied = [config["canary"]] + config["runtime_witnesses"]
    for path in denied:
        try:
            with open(path, "rb") as stream:
                stream.read(1)
        except OSError as error:
            require(error.errno in (errno.EACCES, errno.EPERM),
                    "sandbox denial must be permission refusal, not absence: " + path)
        else:
            raise RuntimeError("sandbox read denial is ineffective: " + path)
    before = images()
    recorded, native = installed_inventory()
    dependencies = python_dependency_inventory(recorded)
    import neograph_engine as ng
    import neograph_engine._neograph as extension
    require(Path(ng.__file__).resolve() == recorded["neograph_engine/__init__.py"] and
            str(Path(extension.__file__).resolve()) in native and ng.__version__ == "0.13.0",
            "import did not resolve to the original installed wheel RECORD")
    outcomes = behavior(ng, config["port"])
    after = images()
    stdlib_native = Path(sysconfig.get_config_var("DESTSHARED") or "__missing_stdlib__").resolve()
    for path in after:
        require(not path.startswith(("/opt/homebrew/", "/usr/local/opt/", "/usr/local/Cellar/")),
                "dyld mapped a Homebrew/build-host library: " + path)
        if path not in before and path not in native and path not in dependencies and not system_image(path):
            require(Path(path).parent == stdlib_native and path.endswith(".so"),
                    "new dyld image is neither RECORD-owned nor an OS/Python stdlib image: " + path)
    wheel_images = sorted(after.intersection(native))
    for component in SDK_COMPONENTS:
        matches = [path for path in wheel_images if re.fullmatch(
                   r"libsp_" + component + r"\.4(?:\.[0-9a-f]{6,})?\.dylib", Path(path).name)]
        require(len(matches) == 1, "SDK interface4 component not uniquely mapped from wheel: " + component)
        require(native[matches[0]]["id"] is not None and re.fullmatch(
                r"libsp_" + component + r"\.4(?:\.[0-9a-f]{6,})?\.dylib",
                Path(native[matches[0]]["id"]).name), "SDK component install identity is not interface4")
    for library in ("libc++", "libc++abi", "libunwind"):
        matches = [path for path in wheel_images if re.fullmatch(
                   re.escape(library) + r"\.1(?:\.[0-9]+)*(?:\.[0-9a-f]{6,})?\.dylib", Path(path).name)]
        require(len(matches) == 1, "matching LLVM runtime is not uniquely packaged AND mapped: " + library)
    for component in ("core", "llm", "program"):
        require(sum(bool(re.fullmatch(r"libneograph_" + component + r"\.0(?:\.[0-9]+)*\.dylib",
                                      Path(path).name)) for path in wheel_images) == 1,
                "NeoGraph component not uniquely mapped from wheel: " + component)
    require(str(Path(extension.__file__).resolve()) in wheel_images, "dyld did not map the wheel extension")
    return {"status": "passed", "platform": platform.mac_ver()[0], "python": platform.python_version(),
            "denied_reads": denied, "outcomes": outcomes, "dyld_wheel_images": wheel_images,
            "dyld_images": sorted(after), "packaged_macho": native,
            "python_dependency_macho": dependencies}


class LocalPeer(HTTPServer):
    def __init__(self):
        self.operations = []
        self.errors = []
        super().__init__(("127.0.0.1", 0), PeerHandler)

    def get_request(self):
        connection, address = super().get_request()
        connection.settimeout(5)
        return connection, address


class PeerHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_POST(self):
        try:
            require(self.path == "/v1/chat/completions", "unexpected native provider route")
            require(self.headers.get("Authorization") is None, "local provider sent credentials")
            length = int(self.headers.get("Content-Length", "0"))
            require(0 < length <= 65536, "invalid local request body length")
            body = json.loads(self.rfile.read(length))
            require(body["model"] == MODEL and len(body["messages"]) == 1, "native request model/history differs")
            message = body["messages"][0]
            require(message["role"] == "user", "native request role differs")
            content = message["content"]
            if isinstance(content, list):
                require(all(part.get("type") == "text" for part in content), "unexpected content block")
                content = "".join(part["text"] for part in content)
            index = len(self.server.operations)
            require(index < len(OPERATIONS) and content == OPERATIONS[index], "native request content/order differs")
            streaming = content == OPERATIONS[1]
            require(bool(body.get("stream", False)) == streaming, "native stream mode differs")
            self.server.operations.append(content)
            status = 429 if content == OPERATIONS[2] else 200
            if status == 429:
                payload = {"error": {"message": "owned local refusal", "type": "rate_limit_error",
                                      "code": "rate_limit_exceeded"}}
            else:
                payload = {"id": "relocation-buffered", "object": "chat.completion", "created": 1,
                           "model": MODEL, "choices": [{"index": 0, "finish_reason": "stop",
                           "message": {"role": "assistant", "content": TEXT}}],
                           "usage": {"prompt_tokens": 8, "completion_tokens": 3, "total_tokens": 11}}
            content_type = "application/json"
            if streaming:
                frames = []
                for fragment in ("local ", "arithmetic: ", "42"):
                    frames.append({"id": "relocation-stream", "object": "chat.completion.chunk", "created": 1,
                                   "model": MODEL, "choices": [{"index": 0, "finish_reason": None,
                                                               "delta": {"content": fragment}}]})
                frames.append({"id": "relocation-stream", "object": "chat.completion.chunk", "created": 1,
                               "model": MODEL, "choices": [{"index": 0, "finish_reason": "stop", "delta": {}}]})
                frames.append({"id": "relocation-stream", "object": "chat.completion.chunk", "created": 1,
                               "model": MODEL, "choices": [], "usage": payload["usage"]})
                encoded = ("".join("data: " + json.dumps(frame) + "\n\n" for frame in frames)
                           + "data: [DONE]\n\n").encode()
                content_type = "text/event-stream"
            else:
                encoded = json.dumps(payload).encode()
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(encoded)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(encoded)
            self.close_connection = True
        except Exception as error:
            self.server.errors.append(str(error))
            self.close_connection = True
            self.send_error(400, "local protocol oracle rejected request")


def main():
    platform_gate()
    require(Path("/usr/bin/sandbox-exec").is_file(), "sandbox-exec unavailable: cold-loader gate cannot run")
    witnesses = []
    for library in ("lib/c++/libc++.1.dylib", "lib/c++/libc++abi.1.dylib", "lib/unwind/libunwind.1.dylib"):
        alias = Path("/opt/homebrew/opt/llvm@20") / library
        if alias.is_file():
            for path in (str(alias), str(alias.resolve())):
                if path not in witnesses:
                    with open(path, "rb") as stream:
                        require(len(stream.read(4)) == 4, "Homebrew runtime witness not readable before sandbox")
                    witnesses.append(path)
    peer = LocalPeer()
    thread = threading.Thread(target=peer.serve_forever, kwargs={"poll_interval": 0.1}, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="neograph-mac-cold-loader-") as directory:
            work = Path(directory).resolve()
            canary = work / "denied-canary"
            canary.write_bytes(b"owned readable denial witness")
            require(canary.read_bytes() == b"owned readable denial witness", "owned denial witness setup failed")
            script = work / "probe.py"
            shutil.copyfile(Path(__file__).resolve(), script)
            config = work / "config.json"
            config.write_text(json.dumps({"port": peer.server_port, "runtime_witnesses": witnesses,
                                          "canary": str(canary)}))
            profile = work / "profile.sb"
            roots = list(DENIED_ROOTS) + [str(canary)]
            profile.write_text("(version 1)\n(allow default)\n(deny file-read*\n" +
                               "\n".join("  (subpath %s)" % json.dumps(root) for root in roots) + ")\n")
            # A whitelist prevents provider credentials/proxies, Python source overlays,
            # or DYLD injection from crossing into the cold interpreter. This is not the
            # isolation proof: the independently verified Seatbelt deny rules are.
            environment = {"HOME": str(work), "TMPDIR": str(work), "PATH": "/usr/bin:/bin",
                           "LANG": "en_US.UTF-8"}
            child = subprocess.Popen(["/usr/bin/sandbox-exec", "-f", str(profile), sys.executable,
                                      "-I", "-B", str(script), "--worker", str(config)],
                                     cwd=work, env=environment, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, start_new_session=True)
            try:
                stdout, stderr = child.communicate(timeout=60)
            finally:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.communicate(timeout=5)
            require(child.returncode == 0, "sandboxed wheel probe failed (%s):\n%s\n%s" %
                    (child.returncode, stdout, stderr))
            reports = [line[len(MARKER):] for line in stdout.splitlines() if line.startswith(MARKER)]
            require(len(reports) == 1, "sandboxed wheel probe did not emit exactly one result")
            report = json.loads(reports[0])
            require(report["status"] == "passed" and not peer.errors and peer.operations == list(OPERATIONS),
                    "local native HTTP request oracles failed: %s / %s" % (peer.errors, peer.operations))
            report["peer_operations"] = peer.operations
            report["homebrew_runtime_witnesses_present"] = bool(witnesses)
            print(json.dumps(report, indent=2, sort_keys=True))
    finally:
        peer.shutdown()
        peer.server_close()
        thread.join(timeout=7)
        require(not thread.is_alive(), "owned local peer did not stop within cleanup deadline")


if __name__ == "__main__":
    try:
        if len(sys.argv) == 3 and sys.argv[1] == "--worker":
            print(MARKER + json.dumps(worker(sys.argv[2]), sort_keys=True))
        else:
            require(len(sys.argv) == 1, "usage: python -I test-macos-wheel-relocation.py")
            main()
    except Exception as error:
        print("macOS wheel cold-loader gate FAILED: " + str(error), file=sys.stderr)
        sys.exit(1)
