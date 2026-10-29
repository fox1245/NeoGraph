#!/usr/bin/env python3
"""Contract tests for scripts/check_host_timers.py (standard library only).

The scan refuses a raw Asio timer in src/ and include/, leaves comments, literals and look-alike names alone, honours
the allowlist and rejects an allowlist that is wrong, and fails on a copy of the repository when one raw timer is
added back.
"""

from __future__ import annotations

import importlib.util
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "check_host_timers.py"
spec = importlib.util.spec_from_file_location("check_host_timers", SCRIPT)
guard = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = guard
spec.loader.exec_module(guard)

HELPER_TEXT = """\
#pragma once
// The one place that may use the raw timer.
class HostTimer {
    void arm_after(int d) { timer_.expires_after(d); note_deadline(timer_.get_executor(), timer_.expiry()); }
    asio::steady_timer timer_;
};
"""


def lines_for(text: str) -> list:
    return [line for line, _ in guard.scan_text(text)]


class ScanTest(unittest.TestCase):
    def test_raw_timers_are_found(self):
        for sample in [
            "asio::steady_timer timer(ex);",
            "std::shared_ptr<asio::steady_timer> timer;",
            "asio::system_timer timer(ex);",
            "asio::high_resolution_timer timer(ex);",
            "asio::basic_waitable_timer<std::chrono::steady_clock> timer(ex);",
            "asio::deadline_timer timer(io);",
            "#include <asio/steady_timer.hpp>",
            "timer.expires_after(std::chrono::milliseconds(5));",
            "timer->expires_at (deadline);",
            "timer.expires_from_now(delay);",
            "co_await asio::cancel_after(1s, asio::use_awaitable);",
            "asio::cancel_at(when, asio::use_awaitable);",
            "int n = 1'000; asio::steady_timer timer(ex);",
            "char quote = '\"'; asio::steady_timer timer(ex);",
        ]:
            with self.subTest(sample=sample):
                self.assertEqual(lines_for(sample), [1])

    def test_look_alikes_and_text_are_left_alone(self):
        for sample in [
            "// asio::steady_timer timer(ex); timer.expires_after(1s);",
            "/* asio::steady_timer timer(ex); */",
            "/* a\n   b steady_timer\n   c */",
            'const char* text = "asio::steady_timer";',
            'const char* text = "// steady_timer";',
            'auto json = R"x(expires_after( steady_timer )x";',
            "auto now = std::chrono::steady_clock::now();",
            "if (data.expires_at_ms <= now) return;",
            "task.lease_expires_at_unix_ms = 0;",
            "identity[\"expires_after\"] = request.expires_after ? json(*request.expires_after) : json(nullptr);",
            "auto cancel_attempt = true;",
            "asio::awaitable<void> cancel_after_timer(Token token);",
            "async::HostTimer timer(executor); timer.arm_after(std::chrono::milliseconds(5));",
            "timer->arm_at(wake_at);",
            "// a continued comment \\\n   steady_timer on its second line\nint x;",
        ]:
            with self.subTest(sample=sample):
                self.assertEqual(guard.scan_text(sample), [])

    def test_lines_are_counted_through_comments_and_strings(self):
        text = (
            "/* one\n"
            "   two */\n"
            'const char* s = "a\\\n'
            'b";\n'
            "auto raw = R\"x(\n"
            "steady_timer\n"
            ")x\";\n"
            "asio::steady_timer timer(ex);\n"
            "timer.expires_after(1s);\n"
        )
        self.assertEqual(lines_for(text), [8, 9])


class TreeTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.write("src/core/host_timer.h", HELPER_TEXT)
        self.write("src/core/clean.cpp", "#include \"host_timer.h\"\nvoid f() { HostTimer t; t.arm_after(1); }\n")
        self.write("include/neograph/clean.h", "// no timer here\n")
        self.write("scripts/host_timer_allowlist.txt", "# empty\n")

    def write(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def run_guard(self, *args):
        proc = subprocess.run(
            [sys.executable, str(SCRIPT), "--root", str(self.root), *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
        )
        return proc.returncode, proc.stdout, proc.stderr

    def test_a_clean_tree_passes_and_the_helper_may_use_the_raw_timer(self):
        self.assertEqual(self.run_guard()[0], 0)

    def test_a_raw_timer_in_src_fails_with_its_place(self):
        self.write("src/core/late.cpp", "void f() {\n    asio::steady_timer timer(ex);\n}\n")
        code, out, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("src/core/late.cpp:2:", out)
        self.assertIn("HostTimer", out)
        self.assertIn("host_timer_allowlist.txt", out)
        self.assertIn("1 finding", err)

    def test_a_raw_expiry_in_include_fails(self):
        self.write("include/neograph/late.h", "inline void f(T& t) {\n    t.expires_at(when);\n}\n")
        code, out, _ = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("include/neograph/late.h:2:", out)

    def test_a_direct_clock_read_in_src_program_fails_unless_allowlisted(self):
        self.write(
            "src/program/late.cpp",
            "void f() {\n"
            "    auto a = std::chrono::steady_clock::now();\n"
            "    auto b = std::chrono::system_clock::now();\n"
            "    auto c = std::chrono::high_resolution_clock::now();\n"
            "}\n",
        )
        code, out, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("src/program/late.cpp:2:", out)
        self.assertIn("src/program/late.cpp:3:", out)
        self.assertIn("src/program/late.cpp:4:", out)
        self.assertIn("RuntimeClock", out)
        self.assertIn("3 finding", err)
        # The timer allowlist does not excuse a clock read: the reads stay, and its entry is unused.
        self.write("scripts/host_timer_allowlist.txt", "src/program/late.cpp\tA reason.\n")
        code, out, _ = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("src/program/late.cpp:2:", out)
        self.assertIn("excuses no raw timer use", out)
        self.write("scripts/host_timer_allowlist.txt", "# empty\n")
        self.write("scripts/runtime_clock_allowlist.txt", "src/program/late.cpp\tA reason.\n")
        self.assertEqual(self.run_guard()[0], 0)

    def test_high_resolution_clock_now_is_refused(self):
        self.write(
            "src/program/hires.cpp",
            "void f() {\n    auto now = std::chrono::high_resolution_clock::now();\n}\n",
        )
        code, out, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("src/program/hires.cpp:2:", out)
        self.assertIn("high_resolution_clock::now", out)
        self.assertIn("1 finding", err)

    def test_clock_read_through_type_alias_is_refused_while_runtime_clock_alias_passes(self):
        self.write(
            "src/program/ok_alias.h",
            "struct S {\n"
            "    using clock = detail::RuntimeClock;\n"
            "    using steady = std::chrono::steady_clock;\n"
            "    using time_point = steady::time_point;\n"
            "    static time_point f() { return clock::now(); }\n"
            "};\n",
        )
        self.assertEqual(self.run_guard()[0], 0)

        self.write(
            "src/program/bad_alias.cpp",
            "using clock = std::chrono::steady_clock;\n"
            "typedef std::chrono::system_clock sys_clock;\n"
            "using hires = std::chrono::high_resolution_clock;\n"
            "void f() {\n"
            "    auto a = clock::now();\n"
            "    auto b = sys_clock::now();\n"
            "    auto c = hires::now();\n"
            "    auto d = steady::now();\n"
            "}\n",
        )
        code, out, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("src/program/bad_alias.cpp:5:", out)
        self.assertIn("src/program/bad_alias.cpp:6:", out)
        self.assertIn("src/program/bad_alias.cpp:7:", out)
        self.assertIn("src/program/bad_alias.cpp:8:", out)
        self.assertIn("4 finding", err)

    def test_include_neograph_program_and_core_clock_files_are_scanned(self):
        self.write(
            "include/neograph/program/header.h",
            "inline auto f() { return std::chrono::steady_clock::now(); }\n",
        )
        self.write(
            "src/core/host_admission.cpp",
            "void g() { auto t = std::chrono::steady_clock::now(); }\n",
        )
        self.write(
            "src/core/hook_runtime.cpp",
            "void h() { auto t = std::chrono::system_clock::now(); }\n",
        )
        code, out, err = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("include/neograph/program/header.h:1:", out)
        self.assertIn("src/core/host_admission.cpp:1:", out)
        self.assertIn("src/core/hook_runtime.cpp:1:", out)
        self.assertIn("3 finding", err)

    def test_explicit_missing_clock_allowlist_fails_with_exit_2(self):
        missing = self.root / "does_not_exist.txt"
        code, _, err = self.run_guard("--clock-allowlist", str(missing))
        self.assertEqual(code, 2)
        self.assertIn("cannot read the allowlist", err)

    def test_the_clock_allowlist_does_not_excuse_a_raw_timer_and_goes_stale_like_the_other(self):
        self.write("src/program/late.cpp", "void f() {\n    asio::steady_timer timer(ex);\n}\n")
        self.write("scripts/runtime_clock_allowlist.txt", "src/program/late.cpp\tA reason.\n")
        code, out, _ = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("src/program/late.cpp:2:", out)
        self.assertIn("runtime_clock_allowlist.txt:1:", out)
        self.assertIn("excuses no direct clock read", out)
        clock = self.root / "elsewhere.txt"
        clock.write_text("# empty\n", encoding="utf-8")
        code, out, _ = self.run_guard("--clock-allowlist", str(clock))
        self.assertEqual(code, 1)
        self.assertNotIn("runtime_clock_allowlist.txt", out)

    def test_an_allowlisted_file_passes(self):
        self.write("src/async/client.cpp", "asio::steady_timer timer(ex);\ntimer.expires_after(1s);\n")
        self.assertEqual(self.run_guard()[0], 1)
        self.write("scripts/host_timer_allowlist.txt", "src/async/*\tThe component is not built for a host.\n")
        self.assertEqual(self.run_guard()[0], 0)

    def test_an_allowlist_entry_that_excuses_nothing_fails(self):
        self.write("scripts/host_timer_allowlist.txt", "src/async/*\tThe component is not built for a host.\n")
        code, out, _ = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("host_timer_allowlist.txt:1:", out)
        self.assertIn("src/async/*", out)
        self.write("src/async/client.cpp", "void f() {}\n")  # matches a file, but the file no longer uses a timer
        self.assertEqual(self.run_guard()[0], 1)

    def test_an_allowlist_entry_needs_a_reason(self):
        self.write("src/async/client.cpp", "asio::steady_timer timer(ex);\n")
        for text in ("src/async/*\n", "src/async/*\t\n", "src/async/*\t   \n", "\treason\n"):
            with self.subTest(text=text):
                self.write("scripts/host_timer_allowlist.txt", text)
                code, _, err = self.run_guard()
                self.assertEqual(code, 2)
                self.assertIn("the reason is required", err)

    def test_the_helper_must_exist_and_must_tell_the_loop(self):
        self.write("src/core/host_timer.h", HELPER_TEXT.replace("note_deadline(", "forget_deadline("))
        code, out, _ = self.run_guard()
        self.assertEqual(code, 1)
        self.assertIn("src/core/host_timer.h:1:", out)
        self.assertIn("note_deadline", out)
        (self.root / "src/core/host_timer.h").unlink()
        code, _, err = self.run_guard()
        self.assertEqual(code, 2)
        self.assertIn("helper", err)

    def test_a_note_in_a_comment_does_not_count_for_the_helper(self):
        self.write("src/core/host_timer.h", "// note_deadline(executor, at);\nasio::steady_timer timer_;\n")
        self.assertEqual(self.run_guard()[0], 1)


class RealTreeTest(unittest.TestCase):
    """Adding one raw timer back to the engine's sources makes the scan of the repository fail."""

    def test_one_raw_timer_added_back_fails(self):
        allowlist = ROOT / "scripts" / "host_timer_allowlist.txt"
        with tempfile.TemporaryDirectory() as temp:
            copy = Path(temp)
            for name in ("src", "include"):
                shutil.copytree(ROOT / name, copy / name)
            (copy / "scripts").mkdir()
            shutil.copy(allowlist, copy / "scripts")
            before = guard.check(copy, copy / "scripts" / allowlist.name)
            target = copy / "src" / "core" / "artifact_provider.cpp"
            target.write_text(
                target.read_text(encoding="utf-8") + "\nasio::steady_timer raw_timer(executor);\n", encoding="utf-8"
            )
            after = guard.check(copy, copy / "scripts" / allowlist.name)
            added = [finding for finding in after if finding not in before]
            self.assertEqual([finding.path for finding in added], ["src/core/artifact_provider.cpp"])

    def test_one_direct_clock_read_added_back_to_program_fails(self):
        allowlist = ROOT / "scripts" / "host_timer_allowlist.txt"
        with tempfile.TemporaryDirectory() as temp:
            copy = Path(temp)
            for name in ("src", "include"):
                shutil.copytree(ROOT / name, copy / name)
            (copy / "scripts").mkdir()
            shutil.copy(allowlist, copy / "scripts")
            shutil.copy(ROOT / "scripts" / guard.CLOCK_ALLOWLIST, copy / "scripts")
            before = guard.check(copy, copy / "scripts" / allowlist.name)
            target = copy / "src" / "program" / "runtime.cpp"
            target.write_text(
                target.read_text(encoding="utf-8") + "\nauto raw_now = std::chrono::steady_clock::now();\n",
                encoding="utf-8",
            )
            after = guard.check(copy, copy / "scripts" / allowlist.name)
            added = [finding for finding in after if finding not in before]
            self.assertEqual([finding.path for finding in added], ["src/program/runtime.cpp"])


if __name__ == "__main__":
    unittest.main()
