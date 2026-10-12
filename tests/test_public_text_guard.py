#!/usr/bin/env python3
"""Contract tests for scripts/check_public_text.py (standard library only).

Every rule fires on a sample and stays quiet on its allowed cases; --diff
reports added lines only; --tree honours the scope switch and the allowlist.
Fake secrets are assembled at run time, so this file never holds a token-shaped
string.
"""

from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "check_public_text.py"
spec = importlib.util.spec_from_file_location("check_public_text", SCRIPT)
guard = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = guard  # dataclasses resolve annotations through sys.modules
spec.loader.exec_module(guard)

HAVE_GIT = shutil.which("git") is not None


def rules_for(line: str, path: str = "src/sample.cpp") -> set:
    return {finding.rule for finding in guard.scan_text(path, line)}


class RuleTest(unittest.TestCase):
    def assertFires(self, rule: str, samples) -> None:
        for sample in samples:
            with self.subTest(rule=rule, sample=sample):
                self.assertIn(rule, rules_for(sample))

    def assertQuiet(self, rule: str, samples) -> None:
        for sample in samples:
            with self.subTest(rule=rule, sample=sample):
                self.assertNotIn(rule, rules_for(sample))

    def test_predecessor_name(self):
        self.assertFires("predecessor-name", [
            "// ported from NexaGraph", "nexagraph", "Nexa-Graph", "NEXA_GRAPH", "see Nexa Graph docs",
            "src/nexagraph/grpc_client.cpp",
        ])
        self.assertQuiet("predecessor-name", ["NeoGraph", "Alexa Graph", "Nexa alone", "graph nexa"])

    def test_private_repo_link(self):
        self.assertFires("private-repo-link", [
            "https://github.com/fox1245/re-agent",
            "see github.com/fox1245/NeoGraph-v1-redesign-backup for history",
            "git@github.com:fox1245/re-agent.git",
            "https://raw.githubusercontent.com/fox1245/re-agent/main/x.py",
            "https://github.com/fox1245/not-a-listed-repo",
            "https://GitHub.com/Fox1245/RE-Agent",
        ])
        self.assertQuiet("private-repo-link", [
            "https://github.com/fox1245/NeoGraph",
            "https://github.com/fox1245/NeoGraph/issues/12",
            "https://github.com/fox1245/NeoGraph.git",
            "https://github.com/fox1245/SchemaProvider",
            "https://github.com/fox1245/neoclaw#readme",
            "https://github.com/fox1245/cppdotenv",
            "https://github.com/fox1245/NeoProtocol",
            "(https://github.com/fox1245/NeoGraph).",
            "https://github.com/fox1245",
            "https://github.com/someone-else/re-agent",
            "handle fox1245 owns the organisation",
        ])

    def test_internal_process_wording(self):
        self.assertFires("internal-process-wording", [
            "// owner-local metadata", "// Owner-Local metadata", "the owner's examples",
            "The Owner's examples", "// the owner\u2019s call", "owner's decision for hardened sessions",
            "Following the owner, we", "an owner decision (D5)", "the AgentX peer", "a WorkOrder was run",
            "Analysis Graph provenance",
        ])
        self.assertQuiet("internal-process-wording", [
            "owner_scope", "owner_id", "OwnerScope owner_scope_", "owner-qualified id",
            "agentx", "work order", "workorder", "the owners of a program", "program owner",
            "std::string owner", "AgentXY",
        ])

    def test_home_path(self):
        self.assertFires("home-path", [
            "cd /home/alice/project", "/Users/bob/Library", r"C:\Users\carol\source",
            r"C:\\Users\\carol\\source", "C:/Users/dave/source", 'path = "/home/erin"',
            "file:///home/frank/x",
        ])
        self.assertQuiet("home-path", [
            "/home/user/proj", "/home/me/x", "/home/example/x", "/home/<name>/x", "$HOME/x",
            "/Users/<name>/x", r"C:\Users\<name>\x", "/home/${USER}/x", "/Users/%USER%/x",
            "/mnt/home/alice/x", "~/x", "/home/runner/work/repo", "/home/", "/home/your-name/x",
            r"C:\Users\Public\x", "/Users/Shared/x", "/home/user", "/etc/home/x",
        ])

    def test_external_ip(self):
        self.assertFires("external-ip", [
            "dns 8.8.8.8", "connect(\"1.1.1.1:53\")", "http://93.184.216.34/index.html",
            "host=8.8.4.4", "<code>9.9.9.9</code>", "2606:4700:4700::1111",
            "[2001:4860:4860::8888]:53", "2001:4860:4860:0:0:0:0:8888",
        ])
        self.assertQuiet("external-ip", [
            "10.0.0.1", "127.0.0.1", "0.0.0.0", "192.168.1.20", "172.16.0.5", "172.31.255.1",
            "169.254.10.1", "100.64.0.1", "224.0.0.251", "255.255.255.255",
            "192.0.2.1", "198.51.100.7", "203.0.113.9",
            "version 1.2.3.4", "Version: 1.2.3.4", "**Version:** 1.2.3.4", "v1.2.3.4", "pkg==1.2.3.4",
            "pkg>=1.2.3.4", "pkg~=1.2.3.4", "tag 8.8.8.8",
            "Linux 6.6.87.2-microsoft-standard-WSL2", "libfoo-1.2.3.4.tar.gz", "1.2.3.4.5",
            "300.1.2.3", "1.2.3", "::1", "fe80::1", "fd00::1", "2001:db8::1", "3fff::1",
            "std::string", "Face::bad", "12:30:45", "aa:bb:cc:dd:ee:ff", "00:11:22:33:44:55:66:77",
            "Foo::Bar::baz", "ff02::1",
        ])

    def test_secret_shaped(self):
        body = "A1b2C3d4" * 6
        self.assertFires("secret-shaped", [
            "key = sk-" + body, "sk-ant-api03-" + body, "sk-proj-" + body[:30],
            "ghp_" + body[:36], "gho_" + body[:36], "github_pat_" + body[:40],
            "AKIA" + "ABCD1234EFGH5678", "hf_" + body[:34], "AIza" + body[:35],
            "-----BEGIN " + "RSA PRIVATE KEY-----", "-----BEGIN " + "PRIVATE KEY-----",
            "-----BEGIN " + "OPENSSH PRIVATE KEY-----", "xoxb-" + "1234567890-abcdefghij",
        ])
        self.assertQuiet("secret-shaped", [
            "sk-test-fake-0000", "sk-or-v1-abcdef", "task-abcdefghijklmnopqrstuvwxyz",
            "risk-assessment-and-management-plan", "ask-the-user-for-a-confirmation-now",
            "sk-short1", "ghp_short", "AKIA-not-a-key", "hf_tokenizer", "hf_" + "a" * 10,
            "MAKIAABCD1234EFGH5678", "-----BEGIN " + "CERTIFICATE-----", "-----BEGIN " + "PUBLIC KEY-----",
        ])

    def test_secret_excerpt_is_masked(self):
        secret = "sk-" + "Q9x7Zk2m" * 6
        findings = guard.scan_text("src/a.cpp", 'auto key = "' + secret + '";')
        self.assertEqual([f.rule for f in findings], ["secret-shaped"])
        self.assertNotIn(secret, findings[0].excerpt)
        self.assertNotIn(secret[8:], findings[0].excerpt)
        self.assertIn("masked", findings[0].excerpt)
        self.assertNotIn(secret[:12], guard.Finding("a", 1, "secret-shaped", findings[0].excerpt).render())

    def test_private_project_name(self):
        self.assertFires("private-project-name", ["the Capstone project", "CAPSTONE-2026", "(Capstone)"])
        self.assertQuiet("private-project-name", ["capstone", "Capstones", "CapstoneX", "keystone"])

    def test_allowed_on_purpose(self):
        line = (
            "Contact someone@example.com. Tested on a Raspberry Pi 5, a Jetson Orin NX and an "
            "Apple M2. Gateway 192.168.0.1, lab 10.20.30.40, loopback 127.0.0.1. "
            "Source: https://github.com/fox1245/NeoGraph"
        )
        self.assertEqual(rules_for(line), set())

    def test_one_finding_per_rule_and_line(self):
        findings = guard.scan_text("a.cpp", "NexaGraph and nexagraph again\nclean\nNexaGraph")
        self.assertEqual([(f.line, f.rule) for f in findings], [(1, "predecessor-name"), (3, "predecessor-name")])

    def test_long_line_excerpt_keeps_the_match_visible(self):
        line = "x" * 400 + " NexaGraph " + "y" * 400
        (finding,) = guard.scan_text("a.json", line)
        self.assertIn("NexaGraph", finding.excerpt)
        self.assertLess(len(finding.excerpt), 200)

    def test_rule_table_is_stable(self):
        self.assertEqual(
            guard.RULE_NAMES,
            ("predecessor-name", "private-repo-link", "internal-process-wording", "home-path",
             "external-ip", "secret-shaped", "private-project-name"),
        )
        for rule in guard.RULES:
            self.assertTrue(rule.summary)
            self.assertTrue(rule.kinds <= guard.ALL_KINDS)


class FileSelectionTest(unittest.TestCase):
    def test_out_of_scope(self):
        for path in (
            "deps/quickjs/quickjs.c", "THIRD_PARTY_LICENSES", "THIRD_PARTY_LICENSES.md",
            "docs/THIRD_PARTY_LICENSES.ko.md", "web/node_modules/x/index.js", "node_modules/x.js",
            "package-lock.json", "ui/pnpm-lock.yaml", "Cargo.lock", "poetry.lock", "x/y.lock",
            "bindings/python/tests/interop/a2a_suite/spec/agent.schema.json",
            "bindings/python/tests/interop/acp_suite/spec/acp.proto", "docs/diagram.png",
            "wasm/neograph.wasm",
        ):
            with self.subTest(path=path):
                self.assertTrue(guard.is_out_of_scope(path))

    def test_in_scope(self):
        for path in (
            "proto/neograph.proto", "schemas/task-graph-proposal-v1.schema.json",
            "bindings/python/tests/interop/a2a_suite/test_a2a_interop_wire.py",
            "bindings/python/tests/interop/a2a_suite/other.schema.json", "dependencies/x.cpp",
            "src/deps/x.cpp",
        ):
            with self.subTest(path=path):
                self.assertFalse(guard.is_out_of_scope(path))

    def test_file_kind(self):
        expected = {
            "README.md": "doc", "benchmarks/README.ko.md": "doc", "CHANGELOG.md": "doc",
            "CHANGELOG.ja.md": "doc", "ROADMAP_v1.md": "doc", "docs/diagrams/a/source.tex": "doc",
            "docs/promo/page.html": "doc", "src/core/a.cpp": "code", "CMakeLists.txt": "code",
            "requirements.txt": "data", ".gitignore": "data",
            "data/pricing/catalog.json": "data", ".github/workflows/ci.yml": "data",
            "scripts/check_public_text.py": "code", "include/neograph/a.h": "code",
            "proto/neograph.proto": "code", "tests/model/BarrierJoin.tla": "code",
        }
        for path, kind in expected.items():
            with self.subTest(path=path):
                self.assertEqual(guard.file_kind(path), kind)


class AllowlistTest(unittest.TestCase):
    def test_parse_and_cover(self):
        entries = guard.parse_allowlist(
            "# comment\n\n"
            "tests/fixtures/*\tsecret-shaped\tfake keys for redaction tests\n"
            "scripts/check_public_text.py\t*\trule definitions\n"
        )
        self.assertEqual(len(entries), 2)
        self.assertTrue(entries[0].covers("tests/fixtures/a/b.json", "secret-shaped"))
        self.assertFalse(entries[0].covers("tests/fixtures/a/b.json", "home-path"))
        self.assertFalse(entries[0].covers("tests/other/b.json", "secret-shaped"))
        self.assertTrue(entries[1].covers("scripts/check_public_text.py", "home-path"))

    def test_malformed_lines_are_rejected(self):
        for bad in (
            "only-a-path\n", "path\tsecret-shaped\n", "path\tsecret-shaped\t \n",
            "path\tno-such-rule\treason\n",
        ):
            with self.subTest(bad=bad):
                with self.assertRaises(guard.AllowlistError):
                    guard.parse_allowlist(bad)

    def test_allowlist_silences_only_its_rule_and_path(self):
        text = "NexaGraph at /home/alice/x\n"
        entries = guard.parse_allowlist("src/a.cpp\thome-path\tsample path in a test\n")
        self.assertEqual({f.rule for f in guard.scan_text("src/a.cpp", text, entries)}, {"predecessor-name"})
        self.assertEqual(
            {f.rule for f in guard.scan_text("src/b.cpp", text, entries)}, {"predecessor-name", "home-path"}
        )

    def test_the_committed_allowlist_is_well_formed(self):
        entries = guard.load_allowlist(guard.DEFAULT_ALLOWLIST)
        for entry in entries:
            self.assertGreater(len(entry.reason), 10, entry)


class DiffParserTest(unittest.TestCase):
    def test_added_lines_with_numbers(self):
        diff = (
            "diff --git a/src/a.cpp b/src/a.cpp\n"
            "index 111..222 100644\n"
            "--- a/src/a.cpp\n"
            "+++ b/src/a.cpp\n"
            "@@ -3,2 +3,3 @@ void f()\n"
            "-old one\n"
            "-old two\n"
            "+new one\n"
            "+++ looks like a header but is content\n"
            "+new three\n"
            "@@ -20,0 +30,1 @@\n"
            "+tail\n"
            "diff --git a/gone.txt b/gone.txt\n"
            "--- a/gone.txt\n"
            "+++ /dev/null\n"
            "@@ -1,2 +0,0 @@\n"
            "-bye\n"
            "-bye\n"
        )
        self.assertEqual(
            list(guard.parse_added_lines(diff)),
            [
                ("src/a.cpp", 3, "new one"),
                ("src/a.cpp", 4, "++ looks like a header but is content"),
                ("src/a.cpp", 5, "new three"),
                ("src/a.cpp", 30, "tail"),
            ],
        )


@unittest.skipUnless(HAVE_GIT, "git is not available")
class RepositoryModeTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name) / "repo"
        self.repo.mkdir()
        self.env = dict(os.environ)
        self.env.update(
            GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1", HOME=self._tmp.name,
            GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.com",
            GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.com",
        )
        self.git("init", "-q")

    def git(self, *args: str) -> None:
        subprocess.run(["git", "-c", "commit.gpgsign=false", *args], cwd=self.repo, env=self.env,
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def write(self, name: str, text: str, mode: str = "w") -> None:
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if "b" in mode:
            path.write_bytes(text.encode("latin-1"))
        else:
            path.write_text(text, encoding="utf-8", newline="\n")

    def commit(self, message: str = "c") -> None:
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)

    def run_guard(self, *args: str):
        proc = subprocess.run(
            [sys.executable, str(SCRIPT), "--root", str(self.repo),
             "--allowlist", str(self.repo / "allow.txt"), *args],
            env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        return proc.returncode, proc.stdout.decode("utf-8"), proc.stderr.decode("utf-8")

    # --tree ---------------------------------------------------------------

    def test_tree_reports_path_line_rule_excerpt(self):
        self.write("src/a.cpp", "int ok;\n// ported from NexaGraph\n")
        self.commit()
        code, out, err = self.run_guard("--tree")
        self.assertEqual(code, 1, err)
        self.assertEqual(out.splitlines(), ["src/a.cpp:2: predecessor-name: // ported from NexaGraph"])
        self.assertIn("predecessor-name=1", err)

    def test_tree_clean_exits_zero(self):
        self.write("src/a.cpp", "int ok;\n")
        self.commit()
        code, out, err = self.run_guard("--tree")
        self.assertEqual((code, out), (0, ""), err)

    def test_tree_skips_third_party_binary_and_lock_files(self):
        bad = "// NexaGraph at /home/alice/x\n"
        self.write("deps/lib/a.c", bad)
        self.write("node_modules/p/i.js", bad)
        self.write("THIRD_PARTY_LICENSES", bad)
        self.write("package-lock.json", bad)
        self.write("bindings/python/tests/interop/s/spec/x.schema.json", bad)
        self.write("bindings/python/tests/interop/s/spec/x.proto", bad)
        self.write("blob.dat", "NexaGraph\0\x01\x02", mode="wb")
        self.write("src/ok.cpp", "int ok;\n")
        self.commit()
        code, out, err = self.run_guard("--tree")
        self.assertEqual((code, out), (0, ""), err)

    def test_scope_code_skips_documents_only(self):
        bad = "NexaGraph\n"
        for name in ("README.md", "docs/guide.md", "docs/page.html", "CHANGELOG.ko.md", "notes.md"):
            self.write(name, bad)
        self.write("src/a.cpp", bad)
        self.write("CMakeLists.txt", bad)
        self.commit()
        _, out_all, _ = self.run_guard("--tree")
        _, out_code, _ = self.run_guard("--tree", "--scope", "code")
        self.assertEqual(
            sorted(line.split(":")[0] for line in out_all.splitlines()),
            ["CHANGELOG.ko.md", "CMakeLists.txt", "README.md", "docs/guide.md", "docs/page.html",
             "notes.md", "src/a.cpp"],
        )
        self.assertEqual(
            sorted(line.split(":")[0] for line in out_code.splitlines()), ["CMakeLists.txt", "src/a.cpp"]
        )

    def test_allowlist_is_applied_by_path_and_rule(self):
        key = "sk-" + "Z8y7X6w5" * 5
        self.write("tests/fixtures/keys.json", '{"k": "' + key + '"}\n// NexaGraph\n')
        self.write("allow.txt", "tests/fixtures/*\tsecret-shaped\tfake key for the redaction test\n")
        self.commit()
        code, out, err = self.run_guard("--tree")
        self.assertEqual(code, 1, err)
        self.assertEqual([line.split(": ")[1] for line in out.splitlines()], ["predecessor-name"])

    def test_secret_is_masked_in_cli_output(self):
        key = "sk-" + "Z8y7X6w5" * 5
        self.write("src/a.cpp", 'const char* k = "' + key + '";\n')
        self.commit()
        code, out, _ = self.run_guard("--tree")
        self.assertEqual(code, 1)
        self.assertIn("secret-shaped", out)
        self.assertNotIn(key[6:], out)

    def test_bad_allowlist_is_a_usage_error(self):
        self.write("src/a.cpp", "int ok;\n")
        self.write("allow.txt", "src/a.cpp\tsecret-shaped\n")
        self.commit()
        code, _, err = self.run_guard("--tree")
        self.assertEqual(code, 2)
        self.assertIn("allow.txt:1", err)

    def test_non_git_tree_is_walked(self):
        plain = Path(self._tmp.name) / "plain"
        (plain / "src").mkdir(parents=True)
        (plain / "src" / "a.cpp").write_text("// NexaGraph\n", encoding="utf-8")
        (plain / "build-x").mkdir()
        (plain / "build-x" / "gen.cpp").write_text("// NexaGraph\n", encoding="utf-8")
        proc = subprocess.run(
            [sys.executable, str(SCRIPT), "--root", str(plain), "--allowlist", str(plain / "none.txt"), "--tree"],
            env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(proc.stdout.decode().splitlines(), ["src/a.cpp:1: predecessor-name: // NexaGraph"])

    # --diff ---------------------------------------------------------------

    def start_topic(self):
        """A base commit tagged 'base' that already holds a violation, then new work."""
        self.write("src/old.cpp", "int a;\n// legacy NexaGraph note\nint b;\n")
        self.write("src/doc.cpp", "int x;\n")
        self.commit("base")
        self.git("tag", "base")

    def test_diff_reports_only_added_lines(self):
        self.start_topic()
        self.write("src/old.cpp", "int a;\n// legacy NexaGraph note\nint b;\n// fine\n// see /home/alice/src\n")
        self.commit("topic")
        code, out, err = self.run_guard("--diff", "base")
        self.assertEqual(code, 1, err)
        self.assertEqual(out.splitlines(), ["src/old.cpp:5: home-path: // see /home/alice/src"])
        # the whole tree still has the old violation
        _, tree_out, _ = self.run_guard("--tree")
        self.assertIn("src/old.cpp:2: predecessor-name", tree_out)

    def test_diff_ignores_removed_and_unchanged_violations(self):
        self.start_topic()
        self.write("src/old.cpp", "int a;\nint b;\n")  # the violating line is deleted
        self.commit("topic")
        code, out, err = self.run_guard("--diff", "base")
        self.assertEqual((code, out), (0, ""), err)

    def test_diff_sees_uncommitted_staged_and_untracked_additions(self):
        self.start_topic()
        self.write("src/doc.cpp", "int x;\n// owner-local note\n")  # unstaged edit
        self.write("src/new.cpp", "// ported from NexaGraph\n")  # untracked
        self.write("src/staged.cpp", "// Capstone work\n")
        self.git("add", "src/staged.cpp")
        code, out, err = self.run_guard("--diff", "base")
        self.assertEqual(code, 1, err)
        self.assertEqual(
            sorted(out.splitlines()),
            [
                "src/doc.cpp:2: internal-process-wording: // owner-local note",
                "src/new.cpp:1: predecessor-name: // ported from NexaGraph",
                "src/staged.cpp:1: private-project-name: // Capstone work",
            ],
        )

    def test_diff_is_relative_to_the_merge_base(self):
        self.start_topic()
        self.git("checkout", "-q", "-b", "topic")
        self.write("src/doc.cpp", "int x;\n// topic line\n")
        self.commit("topic")
        self.git("checkout", "-q", "-")
        # the base branch moves on: it rewrites the legacy line and adds a violation of its own;
        # the topic still has the old line and must not be blamed for either
        self.write("src/old.cpp", "int a;\n// reworded\nint b;\n")
        self.write("src/other.cpp", "// NexaGraph on the base branch\n")
        self.commit("base moves")
        self.git("tag", "base2")
        self.git("checkout", "-q", "topic")
        code, out, err = self.run_guard("--diff", "base2")
        self.assertEqual((code, out), (0, ""), err)

    def test_diff_skips_out_of_scope_and_honours_scope_and_allowlist(self):
        self.start_topic()
        self.write("deps/x/a.c", "// NexaGraph\n")
        self.write("README.md", "NexaGraph\n")
        self.write("src/fix.cpp", "// NexaGraph\n// Capstone\n")
        self.write("allow.txt", "src/fix.cpp\tprivate-project-name\tsample for the guard test\n")
        self.commit("topic")
        _, out_all, _ = self.run_guard("--diff", "base")
        _, out_code, _ = self.run_guard("--diff", "base", "--scope", "code")
        self.assertEqual(sorted(l.split(": ")[0] for l in out_all.splitlines()), ["README.md:1", "src/fix.cpp:1"])
        self.assertEqual([l.split(": ")[0] for l in out_code.splitlines()], ["src/fix.cpp:1"])

    def test_diff_with_unknown_base_is_a_usage_error(self):
        self.start_topic()
        code, _, err = self.run_guard("--diff", "no-such-ref")
        self.assertEqual(code, 2)
        self.assertIn("no-such-ref", err)

    def test_diff_default_base_is_origin_master(self):
        self.assertEqual(guard.DEFAULT_BASE, "origin/master")
        self.start_topic()
        code, _, err = self.run_guard("--diff")  # no origin/master here
        self.assertEqual(code, 2)
        self.assertIn("origin/master", err)


class CommandLineTest(unittest.TestCase):
    def test_list_rules(self):
        proc = subprocess.run([sys.executable, str(SCRIPT), "--list-rules"], stdout=subprocess.PIPE)
        self.assertEqual(proc.returncode, 0)
        names = [line.split("\t")[0] for line in proc.stdout.decode().splitlines()]
        self.assertEqual(tuple(names), guard.RULE_NAMES)


if __name__ == "__main__":
    unittest.main()
