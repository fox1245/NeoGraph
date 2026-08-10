#!/usr/bin/env python3
"""Black-box contract tests for the final legacy-drain audit."""

from __future__ import annotations

import json
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
AUDIT = ROOT / "scripts" / "audit_legacy_drain.py"


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, sort_keys=True, separators=(",", ":")), encoding="utf-8")


def create_program_store(path: Path, bundles: list[dict[str, Any]], versions: list[dict[str, Any]]) -> None:
    connection = sqlite3.connect(path)
    try:
        connection.executescript(
            """
            CREATE TABLE program_bundles (
                id TEXT PRIMARY KEY NOT NULL,
                canonical_bytes BLOB NOT NULL
            );
            CREATE TABLE program_versions (
                id TEXT PRIMARY KEY NOT NULL,
                bundle_id TEXT NOT NULL,
                owner_scope TEXT NOT NULL,
                canonical_bytes BLOB NOT NULL
            );
            """
        )
        for bundle in bundles:
            connection.execute(
                "INSERT INTO program_bundles(id, canonical_bytes) VALUES (?, ?)",
                (bundle["id"], json.dumps(bundle, sort_keys=True, separators=(",", ":"))),
            )
        for version in versions:
            connection.execute(
                "INSERT INTO program_versions(id, bundle_id, owner_scope, canonical_bytes) "
                "VALUES (?, ?, ?, ?)",
                (
                    version["id"],
                    version["bundle_id"],
                    version.get("owner_scope", "tenant:test"),
                    json.dumps(version, sort_keys=True, separators=(",", ":")),
                ),
            )
        connection.commit()
    finally:
        connection.close()


def create_transition_store(path: Path, runs: list[dict[str, Any]]) -> None:
    connection = sqlite3.connect(path)
    try:
        connection.executescript(
            """
            CREATE TABLE program_transition_run_heads_v2 (
                owner_scope TEXT NOT NULL,
                run_id TEXT NOT NULL,
                run_record_bytes BLOB NOT NULL,
                journal_record_bytes BLOB NOT NULL,
                migration_plan_bytes BLOB,
                last_publication_bytes BLOB NOT NULL,
                PRIMARY KEY(owner_scope, run_id)
            );
            """
        )
        for run in runs:
            encoded = json.dumps(run, sort_keys=True, separators=(",", ":"))
            connection.execute(
                "INSERT INTO program_transition_run_heads_v2 "
                "(owner_scope, run_id, run_record_bytes, journal_record_bytes, "
                "migration_plan_bytes, last_publication_bytes) VALUES (?, ?, ?, ?, NULL, ?)",
                (run.get("owner_scope", "tenant:test"), run["run_id"], encoded, "{}", "{}"),
            )
        connection.commit()
    finally:
        connection.close()

def create_harness_store(
    path: Path, artifacts: list[tuple[str, dict[str, Any]]], runs: list[tuple[str, dict[str, Any]]]
) -> None:
    connection = sqlite3.connect(path)
    try:
        connection.executescript(
            """
            CREATE TABLE neograph_harness_artifacts (
                artifact_id TEXT PRIMARY KEY,
                record_json TEXT NOT NULL
            );
            CREATE TABLE neograph_harness_runs (
                run_id TEXT PRIMARY KEY,
                record_json TEXT NOT NULL
            );
            """
        )
        connection.executemany(
            "INSERT INTO neograph_harness_artifacts(artifact_id, record_json) VALUES (?, ?)",
            [
                (artifact_id, json.dumps(record, sort_keys=True, separators=(",", ":")))
                for artifact_id, record in artifacts
            ],
        )
        connection.executemany(
            "INSERT INTO neograph_harness_runs(run_id, record_json) VALUES (?, ?)",
            [
                (run_id, json.dumps(record, sort_keys=True, separators=(",", ":")))
                for run_id, record in runs
            ],
        )
        connection.commit()
    finally:
        connection.close()


def inventory(stores: list[dict[str, Any]], legacy_artifacts: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    return {
        "format": "neograph-legacy-drain-inventory",
        "schema_version": 1,
        "cutover_id": "quickjs-control-test-cutover",
        "captured_at": "2026-08-10T00:00:00Z",
        "inventory_complete": True,
        "stores": stores,
        "legacy_artifacts": legacy_artifacts or [],
    }


class LegacyDrainAuditTest(unittest.TestCase):
    maxDiff = None

    def run_audit(self, snapshot: Path, document: dict[str, Any]) -> tuple[subprocess.CompletedProcess[str], dict[str, Any]]:
        inventory_path = snapshot / "inventory.json"
        proof_path = snapshot / "proof.json"
        write_json(inventory_path, document)
        result = subprocess.run(
            [
                sys.executable,
                str(AUDIT),
                "--inventory",
                str(inventory_path),
                "--root",
                str(snapshot),
                "--output",
                str(proof_path),
                "--require-final",
            ],
            capture_output=True,
            check=False,
            text=True,
        )
        self.assertTrue(proof_path.is_file(), msg=result.stderr)
        return result, json.loads(proof_path.read_text(encoding="utf-8"))

    def test_transition_store_is_order_independent_and_final_proof_is_reproducible(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = Path(temporary)
            create_program_store(
                snapshot / "program.db",
                [{"id": "bundle-js", "source_kind": "javascript"}],
                [{"id": "version-js", "bundle_id": "bundle-js"}],
            )
            create_transition_store(
                snapshot / "transitions.db",
                [
                    {
                        "owner_scope": "tenant:test",
                        "run_id": "current-recoverable-run",
                        "bundle_id": "bundle-js",
                        "program_version_id": "version-js",
                        "terminal_result": None,
                    }
                ],
            )
            document = inventory(
                [
                    {
                        "id": "transitions",
                        "kind": "program_transitions_sqlite",
                        "path": "transitions.db",
                    },
                    {"id": "program", "kind": "program_sqlite", "path": "program.db"},
                ]
            )

            first, first_proof = self.run_audit(snapshot, document)
            self.assertEqual(first.returncode, 0, msg=first.stderr)
            self.assertTrue(first_proof["final_drain"]["passed"])
            self.assertEqual(first_proof["counts"]["legacy_artifacts"], 0)
            self.assertTrue(first_proof["scanned_stores"][0]["content_identity"].startswith("sha256:"))
            first_identity = first_proof["proof_identity"]

            second, second_proof = self.run_audit(snapshot, document)
            self.assertEqual(second.returncode, 0, msg=second.stderr)
            self.assertEqual(second_proof["proof_identity"], first_identity)
            self.assertEqual(second_proof, first_proof)

    def test_live_sqlite_sidecar_blocks_final_proof(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = Path(temporary)
            (snapshot / "program.db").write_bytes(b"not a SQLite database")
            (snapshot / "program.db-wal").write_bytes(b"simulated live WAL")

            result, proof = self.run_audit(
                snapshot,
                inventory([{"id": "program", "kind": "program_sqlite", "path": "program.db"}]),
            )

            self.assertEqual(result.returncode, 1, msg=result.stderr)
            self.assertFalse(proof["final_drain"]["passed"])
            self.assertTrue(
                any(
                    blocker["code"] == "SQLITE_SNAPSHOT_HAS_LIVE_SIDECAR"
                    for blocker in proof["final_drain"]["blockers"]
                )
            )
            self.assertFalse(
                any(blocker["code"] == "STORE_SCAN_ERROR" for blocker in proof["final_drain"]["blockers"])
            )

    def test_current_harness_sqlite_artifact_needs_no_legacy_classification(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = Path(temporary)
            create_harness_store(
                snapshot / "harness.db",
                [
                    (
                        "javascript-artifact",
                        {
                            "format": "neograph-harness-program-adapter-artifact",
                            "storage_schema_version": 2,
                            "artifact_id": "javascript-artifact",
                            "bundle": json.dumps(
                                {"id": "javascript-bundle", "source_kind": "javascript"},
                                sort_keys=True,
                                separators=(",", ":"),
                            ),
                            "projection": {"authoring_frontend": "javascript"},
                        },
                    )
                ],
                [],
            )

            result, proof = self.run_audit(
                snapshot,
                inventory([{"id": "harness", "kind": "harness_sqlite", "path": "harness.db"}]),
            )

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertTrue(proof["final_drain"]["passed"])
            self.assertEqual(proof["counts"]["legacy_artifacts"], 0)

    def test_unclassified_legacy_version_blocks_final_removal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = Path(temporary)
            create_program_store(
                snapshot / "program.db",
                [{"id": "legacy-bundle", "source_kind": "canonical_json"}],
                [{"id": "legacy-version", "bundle_id": "legacy-bundle"}],
            )
            result, proof = self.run_audit(
                snapshot,
                inventory([{"id": "program", "kind": "program_sqlite", "path": "program.db"}]),
            )

            self.assertEqual(result.returncode, 1, msg=result.stderr)
            self.assertFalse(proof["final_drain"]["passed"])
            self.assertEqual(proof["counts"]["unclassified_legacy_artifacts"], 1)
            self.assertTrue(
                any(blocker["code"] == "UNCLASSIFIED_LEGACY_ARTIFACT" for blocker in proof["final_drain"]["blockers"])
            )

    def test_rejected_inactive_legacy_version_is_not_a_runtime_drain_blocker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = Path(temporary)
            create_program_store(
                snapshot / "program.db",
                [{"id": "legacy-bundle", "source_kind": "canonical_json"}],
                [{"id": "legacy-version", "bundle_id": "legacy-bundle"}],
            )
            result, proof = self.run_audit(
                snapshot,
                inventory(
                    [{"id": "program", "kind": "program_sqlite", "path": "program.db"}],
                    [
                        {
                            "artifact_id": "program/version/legacy-version",
                            "kind": "legacy_program_version",
                            "classification": "rejected",
                            "reason": "pre-release state is intentionally discarded",
                        }
                    ],
                ),
            )

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertTrue(proof["final_drain"]["passed"])
            self.assertEqual(proof["counts"]["rejected_legacy_artifacts"], 1)

    def test_recoverable_legacy_run_blocks_even_when_source_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = Path(temporary)
            create_program_store(
                snapshot / "program.db",
                [{"id": "legacy-bundle", "source_kind": "canonical_json"}],
                [{"id": "legacy-version", "bundle_id": "legacy-bundle"}],
            )
            create_transition_store(
                snapshot / "transitions.db",
                [
                    {
                        "owner_scope": "tenant:test",
                        "run_id": "recoverable-run",
                        "bundle_id": "legacy-bundle",
                        "program_version_id": "legacy-version",
                        "terminal_result": None,
                    }
                ],
            )
            result, proof = self.run_audit(
                snapshot,
                inventory(
                    [
                        {"id": "program", "kind": "program_sqlite", "path": "program.db"},
                        {
                            "id": "transitions",
                            "kind": "program_transitions_sqlite",
                            "path": "transitions.db",
                        },
                    ],
                    [
                        {
                            "artifact_id": "program/version/legacy-version",
                            "kind": "legacy_program_version",
                            "classification": "rejected",
                            "reason": "pre-release state is intentionally discarded",
                        }
                    ],
                ),
            )

            self.assertEqual(result.returncode, 1, msg=result.stderr)
            self.assertFalse(proof["final_drain"]["passed"])
            self.assertEqual(proof["counts"]["active_or_recoverable_legacy_runs"], 1)
            self.assertTrue(
                any(blocker["code"] == "ACTIVE_OR_RECOVERABLE_LEGACY_RUN" for blocker in proof["final_drain"]["blockers"])
            )

    def test_transition_run_is_bound_to_its_exact_legacy_version(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = Path(temporary)
            create_program_store(
                snapshot / "program.db",
                [{"id": "legacy-bundle", "source_kind": "canonical_json"}],
                [
                    {"id": "legacy-version-a", "bundle_id": "legacy-bundle"},
                    {"id": "legacy-version-b", "bundle_id": "legacy-bundle"},
                ],
            )
            create_transition_store(
                snapshot / "transitions.db",
                [
                    {
                        "owner_scope": "tenant:test",
                        "run_id": "recoverable-run",
                        "bundle_id": "legacy-bundle",
                        "program_version_id": "legacy-version-a",
                        "terminal_result": None,
                    }
                ],
            )
            result, proof = self.run_audit(
                snapshot,
                inventory(
                    [
                        {"id": "program", "kind": "program_sqlite", "path": "program.db"},
                        {
                            "id": "transitions",
                            "kind": "program_transitions_sqlite",
                            "path": "transitions.db",
                        },
                    ],
                    [
                        {
                            "artifact_id": "program/version/legacy-version-a",
                            "kind": "legacy_program_version",
                            "classification": "rejected",
                            "reason": "pre-release state is intentionally discarded",
                        },
                        {
                            "artifact_id": "program/version/legacy-version-b",
                            "kind": "legacy_program_version",
                            "classification": "rejected",
                            "reason": "pre-release state is intentionally discarded",
                        },
                    ],
                ),
            )

            self.assertEqual(result.returncode, 1, msg=result.stderr)
            self.assertEqual(proof["counts"]["active_or_recoverable_legacy_runs"], 1)
            artifacts = {entry["artifact_id"]: entry for entry in proof["legacy_artifacts"]}
            self.assertTrue(artifacts["program/version/legacy-version-a"]["active_or_recoverable"])
            self.assertFalse(artifacts["program/version/legacy-version-b"]["active_or_recoverable"])

    def test_unknown_artifact_frontend_is_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = Path(temporary)
            artifacts = snapshot / "harness" / "artifacts"
            runs = snapshot / "harness" / "runs"
            artifacts.mkdir(parents=True)
            runs.mkdir()
            write_json(
                artifacts / "legacy.json",
                {
                    "format": "neograph-harness-program-adapter-artifact",
                    "storage_schema_version": 2,
                    "artifact_id": "legacy",
                    "bundle": json.dumps({"id": "legacy-bundle", "source_kind": "cpp_builder"}),
                    "projection": {},
                },
            )
            result, proof = self.run_audit(
                snapshot,
                inventory([{"id": "harness", "kind": "harness_file", "path": "harness"}]),
            )

            self.assertEqual(result.returncode, 1, msg=result.stderr)
            self.assertFalse(proof["final_drain"]["passed"])
            self.assertEqual(proof["counts"]["unclassified_legacy_artifacts"], 1)
            self.assertTrue(
                any(blocker["code"] == "UNCLASSIFIED_LEGACY_ARTIFACT" for blocker in proof["final_drain"]["blockers"])
            )


if __name__ == "__main__":
    unittest.main()
