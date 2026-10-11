"""Model-free regressions for Jarvis benchmark input/result admission."""

import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import driver


class DriverAdmissionTests(unittest.TestCase):
    def test_empty_marker_is_not_a_successful_reply(self):
        for line in ("[jarvis:tts]", "[jarvis:tts] \n", "[jarvis:tts][ko] \t\n"):
            self.assertEqual(driver.tts_payload(line), "")

    def test_language_marker_is_not_part_of_the_spoken_payload(self):
        self.assertEqual(driver.tts_payload("[jarvis:tts][ko] 안녕하세요\n"), "안녕하세요")
        self.assertEqual(driver.tts_payload("[jarvis:tts][en] [note] Hello\n"), "[note] Hello")
        self.assertEqual(driver.tts_payload("[jarvis:tts][ko] (생성: audio.wav, 1.0초)\n"),
                         "(생성: audio.wav, 1.0초)")
        self.assertEqual(driver.tts_payload("[jarvis:ttft]\n"), "")

    def test_empty_inputs_and_invalid_turn_limit_never_spawn_a_child(self):
        with tempfile.TemporaryDirectory() as directory:
            turns = Path(directory) / "turns.txt"
            turns.write_text(" \n\t\n", encoding="utf-8")
            for extra in ([], ["--max-turns", "0"], ["--max-turns", "-1"]):
                argv = ["driver", "--cmd", "must-not-run", "--turns", str(turns),
                        "--out", str(Path(directory) / "out.jsonl"), "--label", "fixture", *extra]
                with patch.object(sys, "argv", argv), patch.object(driver.subprocess, "Popen") as spawn, \
                        contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                    driver.main()
                self.assertEqual(error.exception.code, 2)
                spawn.assert_not_called()


if __name__ == "__main__":
    unittest.main()
