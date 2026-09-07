"""Black-box qualification: real server + Program runtime + SQLite/PostgreSQL.

Usage: python3 test_program_chat.py /path/to/cookbook_program_chatbot
Set NEOGRAPH_CHAT_POSTGRES_URL to run the same scenarios on a disposable database.
No packages or external LLM credentials required. The live-provider case uses a
local HTTP fixture to exercise the existing OpenAIProvider protocol adapter.
"""
import concurrent.futures
import contextlib
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
import uuid

EXECUTABLE = str(Path(sys.argv.pop(1)).resolve())


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, directory, extra=(), env=None):
        self.directory = Path(directory)
        self.port = free_port()
        self.session = "test-" + uuid.uuid4().hex
        self.extra = list(extra)
        if "--env-file" not in self.extra:
            self.extra.append("--no-env")
        self.env = dict(os.environ)
        self.env.pop("OPENROUTER_API_KEY", None)
        self.env.pop("OPENROUTER_MODEL", None)
        self.env.pop("NEOGRAPH_CHAT_BASE_URL", None)
        self.env.update(env or {})
        self.process = None
        self.log = (self.directory / (self.session + ".log")).open("w+")

    def start(self):
        self.process = subprocess.Popen(
            [EXECUTABLE, "--db", str(self.directory / (self.session + ".sqlite")),
             "--session", self.session, "--port", str(self.port), *self.extra],
            stdout=self.log, stderr=self.log, env=self.env)
        for _ in range(450):
            if self.process.poll() is not None:
                self.log.seek(0)
                raise RuntimeError(self.log.read())
            try:
                self.state("alice")
                return self
            except (OSError, urllib.error.URLError):
                time.sleep(0.1)
        raise RuntimeError("Server startup timeout")

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.kill()  # Deliberate process loss, preserves durable Running checkpoints.
            self.process.wait(timeout=10)
        self.process = None

    def request(self, path, body=None, tenant="alice", token=None):
        req = urllib.request.Request(
            f"http://127.0.0.1:{self.port}{path}",
            data=None if body is None else json.dumps(body).encode(),
            headers={"Authorization": "Bearer " + (token or tenant + "-demo"),
                     "Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=170) as response:
            return json.load(response)

    def state(self, tenant):
        return self.request("/api/state", tenant=tenant)

    def turn(self, tenant, request_id, message, force=False):
        return self.request("/api/turn", {"request_id": request_id, "message": message,
                                         "force_swap": force}, tenant)

    def close(self):
        self.stop()
        self.log.close()


class ChatTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="neograph-chat-test-")
        self.servers = []

    def tearDown(self):
        for server in self.servers:
            if any(test is self for test, _ in self._outcome.result.errors + self._outcome.result.failures):
                server.log.flush()
                server.log.seek(0)
                print(server.log.read()[-8000:], file=sys.stderr)
            server.close()
        self.temp.cleanup()

    def server(self, extra=(), env=None):
        server = Server(self.temp.name, extra, env)
        self.servers.append(server)
        return server.start()

    @staticmethod
    def assistant(state):
        return next(a for a in state["agents"] if a["depth"] == 1)

    def test_recursive_swap_isolation_idempotency_and_process_restart(self):
        s = self.server()
        with concurrent.futures.ThreadPoolExecutor(2) as pool:
            results = [pool.submit(s.turn, "alice", "a1", "review ALICE_SECRET", True),
                       pool.submit(s.turn, "bob", "b1", "hello BOB_SECRET")]
            for result in results:
                self.assertEqual(result.result()["status"], "completed")
        alice, bob = s.state("alice"), s.state("bob")
        self.assertEqual(self.assistant(alice)["generation"], 2)
        self.assertEqual(self.assistant(bob)["generation"], 1)
        self.assertNotIn("BOB_SECRET", json.dumps(alice))
        self.assertNotIn("ALICE_SECRET", json.dumps(bob))
        self.assertEqual(alice["evolutions"][0]["status"], "published")
        before = self.assistant(alice)
        s.turn("alice", "a2", "review again")
        alice = s.state("alice")
        self.assertEqual(alice["evolutions"][-1]["status"], "kept")
        self.assertEqual([a["depth"] for a in alice["agents"]], [0, 1, 2])
        self.assertEqual(self.assistant(alice)["logical_id"], before["logical_id"])
        self.assertLess(self.assistant(alice)["remaining"]["max_dynamic_compiles"], before["remaining"]["max_dynamic_compiles"])
        calls = alice["usage"]["calls"]
        s.turn("alice", "a2", "review again")
        self.assertEqual(s.state("alice")["usage"]["calls"], calls)
        with self.assertRaises(urllib.error.HTTPError) as conflict:
            s.turn("alice", "a2", "different request")
        self.assertEqual(conflict.exception.code, 400)
        with self.assertRaises(urllib.error.HTTPError) as unauthorized:
            s.request("/api/state", token="invalid")
        self.assertEqual(unauthorized.exception.code, 401)
        saved = s.state("alice")
        s.stop()
        s.start()
        self.assertEqual(s.state("alice")["messages"], saved["messages"])
        self.assertEqual(s.state("alice")["usage"], saved["usage"])
        s.turn("alice", "a3", "review after restart", True)
        after = s.state("alice")
        self.assertEqual(self.assistant(after)["logical_id"], before["logical_id"])
        self.assertEqual(self.assistant(after)["generation"], 3)
        self.assertEqual(len([a for a in after["agents"] if a["depth"] == 2]), 2)
        self.assertEqual(after["turn"], 3)
        self.assertEqual(s.state("bob")["usage"], bob["usage"])
        self.assertGreater(after["usage"]["tokens"], saved["usage"]["tokens"])

    def test_budget_exhaustion_does_not_redispatch_or_reset(self):
        s = self.server(["--max-calls", "1"])
        with self.assertRaises(urllib.error.HTTPError):
            s.turn("alice", "a1", "hello")
        self.assertEqual(s.state("alice")["usage"]["calls"], 1)
        for _ in range(2):
            with self.assertRaises(urllib.error.HTTPError):
                s.turn("alice", "a1", "hello")
        self.assertEqual(s.state("alice")["usage"]["calls"], 1)
        s.stop()
        s.start()
        self.assertEqual(s.state("alice")["limits"]["calls"], 1)
        self.assertEqual(s.state("alice")["usage"]["calls"], 1)

    def test_dotenv_environment_and_model_override_precedence(self):
        path = Path(self.temp.name) / "settings.env"
        path.write_text("OPENROUTER_API_KEY=fixture-env-secret\nOPENROUTER_MODEL=dotenv-model\n", encoding="utf-8")
        from_file = self.server(["--env-file", str(path)])
        self.assertEqual(from_file.state("alice")["settings"]["model"], "dotenv-model")
        self.assertNotIn("fixture-env-secret", json.dumps(from_file.state("alice")))
        from_env = self.server(["--env-file", str(path)], {"OPENROUTER_MODEL": "environment-model"})
        self.assertEqual(from_env.state("alice")["settings"]["model"], "environment-model")
        from_cli = self.server(["--env-file", str(path), "--model", "z-ai/glm-5.3-flash"],
                               {"OPENROUTER_MODEL": "environment-model"})
        self.assertEqual(from_cli.state("alice")["settings"]["model"], "z-ai/glm-5.3-flash")
        self.assertTrue(from_cli.state("alice")["settings"]["skill_sha256"].startswith("sha256:"))

    def test_provider_adapter_rejects_invalid_proposal_and_retains_unknown_usage(self):
        seen = []

        class Provider(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            def log_message(self, *_):
                pass

            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                seen.append(request)
                proposal = "Return only JSON" in request["messages"][0]["content"]
                empty = "empty proposal" in request["messages"][1]["content"]
                content = ("" if empty else "{\"plan\":\"execute_shell\",\"reason\":\"untrusted\",\"confidence\":1}") if proposal else "Fixture answer"
                response = {"id": "fixture", "object": "chat.completion",
                    "choices": [{"index": 0, "message": {"role": "assistant", "content": content},
                                 "finish_reason": "length" if empty and proposal else "stop"}]}
                if empty:
                    response["usage"] = {"prompt_tokens":7,"completion_tokens":2,"total_tokens":9}
                body = json.dumps(response).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider) as provider:
            thread = threading.Thread(target=provider.serve_forever, daemon=True)
            thread.start()
            try:
                s = self.server(["--live", "--allow-loopback-provider", "--reasoning-effort", "low"], {
                    "OPENROUTER_API_KEY": "local-fixture-only", "OPENROUTER_MODEL": "fixture-model",
                    "NEOGRAPH_CHAT_BASE_URL": f"http://127.0.0.1:{provider.server_port}/v1"})
                s.turn("alice", "a1", "hello from protocol test")
                state = s.state("alice")
                self.assertEqual(len(seen), 2)
                self.assertEqual(state["evolutions"][0]["status"], "rejected")
                self.assertEqual(self.assistant(state)["generation"], 1)
                self.assertGreater(state["usage"]["tokens"], 0)
                self.assertEqual(state["usage"]["prompt_tokens"], 0)
                self.assertEqual(seen[0]["model"], "fixture-model")
                self.assertNotIn("response_format",seen[0])
                self.assertEqual(seen[0]["reasoning_effort"],"low")
                self.assertEqual(seen[1]["response_format"],{"type":"json_object"})
                self.assertGreater(seen[0].get("max_completion_tokens", seen[0].get("max_tokens", 0)), 0)
                skill = (Path(__file__).resolve().parents[3] / "skills/neograph-harness-authoring/SKILL.md").read_text(encoding="utf-8")
                self.assertIn(skill, seen[1]["messages"][0]["content"].replace("\r\n", "\n"))
                before=state["usage"]["tokens"]
                result=s.turn("alice","a2","empty proposal")
                after=s.state("alice")
                self.assertEqual(result["status"],"completed")
                self.assertEqual(result["answer"],"Fixture answer")
                self.assertEqual(after["usage"]["tokens"],before+18)
                self.assertEqual(after["usage"]["uncertain_calls"],0)
                self.assertEqual(after["evolutions"][-1]["status"],"rejected")
                self.assertEqual(after["evolutions"][-1]["proposal"]["stop_reason"],"max_tokens")
            finally:
                provider.shutdown()
                thread.join()

    def test_process_loss_during_provider_call_never_redispatches(self):
        dispatched = threading.Event()
        release = threading.Event()
        seen = []

        class Provider(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_):
                pass

            def do_POST(self):
                seen.append(self.rfile.read(int(self.headers["Content-Length"])))
                dispatched.set()
                release.wait(60)
                self.close_connection = True

        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider) as provider:
            thread = threading.Thread(target=provider.serve_forever, daemon=True)
            thread.start()
            try:
                s = self.server(["--live", "--allow-loopback-provider"], {
                    "OPENROUTER_API_KEY": "local-fixture-only", "OPENROUTER_MODEL": "fixture-model",
                    "NEOGRAPH_CHAT_BASE_URL": f"http://127.0.0.1:{provider.server_port}/v1"})
                with concurrent.futures.ThreadPoolExecutor(1) as pool:
                    request = pool.submit(s.turn, "alice", "a1", "uncertain effect")
                    self.assertTrue(dispatched.wait(30))
                    before = s.state("alice")
                    self.assertEqual(before["usage"]["calls"], 1)
                    self.assertGreater(before["usage"]["tokens"], 0)
                    s.stop()
                    release.set()
                    with contextlib.suppress(Exception):
                        request.result()
                s.start()
                recovered = s.state("alice")
                self.assertEqual(recovered["usage"]["tokens"], before["usage"]["tokens"])
                self.assertEqual(recovered["usage"]["uncertain_calls"], 1)
                with self.assertRaises(urllib.error.HTTPError):
                    s.turn("alice", "a1", "uncertain effect")
                self.assertEqual(len(seen), 1)
                self.assertEqual(s.state("alice")["usage"]["calls"], 1)
            finally:
                release.set()
                provider.shutdown()
                thread.join()


if __name__ == "__main__":
    unittest.main(verbosity=2)
