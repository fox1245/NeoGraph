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


def create_program_store(
    path: Path,
    bundles: list[dict[str, Any]],
    versions: list[dict[str, Any]],
    activations: list[dict[str, Any]] | None = None,
) -> None:
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
            CREATE TABLE program_activations (
                owner_scope TEXT PRIMARY KEY NOT NULL,
                generation INTEGER NOT NULL,
                active_version_id TEXT NOT NULL,
                policy_snapshot_hash TEXT NOT NULL,
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
        for activation in activations or []:
            connection.execute(
                "INSERT INTO program_activations("
                "owner_scope, generation, active_version_id, policy_snapshot_hash, canonical_bytes"
                ") VALUES (?, ?, ?, ?, ?)",
                (
                    activation["owner_scope"],
                    activation["generation"],
                    activation["active_version_id"],
                    activation["policy_snapshot_hash"],
                    json.dumps(activation, sort_keys=True, separators=(",", ":")),
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

def postgres_copy_escape(value: str) -> str:
    return (
        value.replace("\\", r"\\")
        .replace("\b", r"\b")
        .replace("\f", r"\f")
        .replace("\n", r"\n")
        .replace("\r", r"\r")
        .replace("\t", r"\t")
        .replace("\v", r"\v")
    )


def create_pg_restore_stub(path: Path, tables: dict[str, dict[str, Any]]) -> None:
    encoded_tables = {
        table: {
            "columns": entry["columns"],
            "rows": ["\t".join(postgres_copy_escape(value) for value in row) for row in entry["rows"]],
        }
        for table, entry in tables.items()
    }
    executable = path / "pg_restore"
    payload = json.dumps(encoded_tables, sort_keys=True, separators=(",", ":"))
    executable.write_text(
        f"""#!{sys.executable}
import json
import sys

tables = json.loads({payload!r})
arguments = set(sys.argv[1:])
requested_table = next(
    (argument.removeprefix("--table=") for argument in sys.argv[1:] if argument.startswith("--table=")),
    None,
)
table = requested_table.removeprefix("public.") if requested_table is not None else None
if (
    not {{"--data-only", "--strict-names", "--file=-"}}.issubset(arguments)
    or requested_table is None
    or requested_table != "public." + table
    or table not in tables
):
    print("pg_restore stub rejected the invocation", file=sys.stderr)
    raise SystemExit(2)
entry = tables[table]
print("-- PostgreSQL database dump")
print("COPY public." + table + " (" + ", ".join(entry["columns"]) + ") FROM stdin;")
for row in entry["rows"]:
    print(row)
print(r"\\.")
""",
        encoding="utf-8",
    )
    executable.chmod(0o755)


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

def no_deployment_inventory() -> dict[str, Any]:
    document = inventory([])
    document["no_deployment_attestation"] = {
        "attestation_id": "operator:quickjs-control-no-deployment",
        "attested_by": "release-operator",
        "statement": "no_pre_release_or_production_deployment_has_ever_existed",
        "scope": "all_neograph_program_and_harness_durable_state",
    }
    return document




class LegacyDrainAuditTest(unittest.TestCase):
    maxDiff = None

    def invoke_audit(
        self,
        snapshot: Path,
        document: dict[str, Any],
        environment: dict[str, str] | None = None,
    ) -> tuple[subprocess.CompletedProcess[str], Path]:
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
            env=environment,
        )
        return result, proof_path

    def run_audit(
        self,
        snapshot: Path,
        document: dict[str, Any],
        environment: dict[str, str] | None = None,
    ) -> tuple[subprocess.CompletedProcess[str], dict[str, Any]]:
        result, proof_path = self.invoke_audit(snapshot, document, environment)
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

    def test_postgres_dump_scans_legacy_source_without_restoring(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = Path(temporary)
            (snapshot / "program.dump").write_bytes(b"synthetic PostgreSQL archive")
            command_directory = snapshot / "commands"
            command_directory.mkdir()
            legacy_bundle = json.dumps(
                {"id": "legacy-bundle", "note": "line one\nline two", "source_kind": "canonical_json"},
                sort_keys=True,
                separators=(",", ":"),
            )
            javascript_bundle = json.dumps(
                {"id": "javascript-bundle", "source_kind": "javascript"},
                sort_keys=True,
                separators=(",", ":"),
            )
            legacy_version = json.dumps(
                {"bundle_id": "legacy-bundle", "id": "legacy-version"},
                sort_keys=True,
                separators=(",", ":"),
            )
            javascript_version = json.dumps(
                {"bundle_id": "javascript-bundle", "id": "javascript-version"},
                sort_keys=True,
                separators=(",", ":"),
            )
            create_pg_restore_stub(
                command_directory,
                {
                    "neograph_program_bundles": {
                        "columns": ["id", "canonical_bytes"],
                        "rows": [
                            ["legacy-bundle", legacy_bundle],
                            ["javascript-bundle", javascript_bundle],
                        ],
                    },
                    "neograph_program_versions": {
                        "columns": ["id", "bundle_id", "owner_scope", "canonical_bytes"],
                        "rows": [
                            ["legacy-version", "legacy-bundle", "tenant:test", legacy_version],
                            ["javascript-version", "javascript-bundle", "tenant:test", javascript_version],
                        ],
                    },
                    "neograph_program_activations": {
                        "columns": [
                            "owner_scope",
                            "generation",
                            "active_version_id",
                            "policy_snapshot_hash",
                            "canonical_bytes",
                        ],
                        "rows": [],
                    },
                },
            )
            result, proof = self.run_audit(
                snapshot,
                inventory(
                    [{"id": "program", "kind": "program_postgres_dump", "path": "program.dump"}],
                    [
                        {
                            "artifact_id": "program/version/legacy-version",
                            "kind": "legacy_program_version",
                            "classification": "rejected",
                            "reason": "pre-release state is intentionally discarded",
                        }
                    ],
                ),
                {"PATH": str(command_directory)},
            )

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertTrue(proof["final_drain"]["passed"])
            self.assertEqual(proof["counts"]["rejected_legacy_artifacts"], 1)


    def test_postgres_dump_activated_legacy_version_blocks_final_removal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = Path(temporary)
            (snapshot / "program.dump").write_bytes(b"synthetic PostgreSQL archive")
            command_directory = snapshot / "commands"
            command_directory.mkdir()
            bundle = {"id": "legacy-bundle", "source_kind": "canonical_json"}
            version = {"bundle_id": "legacy-bundle", "id": "legacy-version"}
            activation = {
                "active_version_id": "legacy-version",
                "generation": 1,
                "owner_scope": "tenant:test",
                "policy_snapshot_hash": "sha256:" + "b" * 64,
            }
            create_pg_restore_stub(
                command_directory,
                {
                    "neograph_program_bundles": {
                        "columns": ["id", "canonical_bytes"],
                        "rows": [["legacy-bundle", json.dumps(bundle, sort_keys=True, separators=(",", ":"))]],
                    },
                    "neograph_program_versions": {
                        "columns": ["id", "bundle_id", "owner_scope", "canonical_bytes"],
                        "rows": [
                            [
                                "legacy-version",
                                "legacy-bundle",
                                "tenant:test",
                                json.dumps(version, sort_keys=True, separators=(",", ":")),
                            ]
                        ],
                    },
                    "neograph_program_activations": {
                        "columns": [
                            "owner_scope",
                            "generation",
                            "active_version_id",
                            "policy_snapshot_hash",
                            "canonical_bytes",
                        ],
                        "rows": [
                            [
                                "tenant:test",
                                "1",
                                "legacy-version",
                                activation["policy_snapshot_hash"],
                                json.dumps(activation, sort_keys=True, separators=(",", ":")),
                            ]
                        ],
                    },
                },
            )
            result, proof = self.run_audit(
                snapshot,
                inventory(
                    [{"id": "program", "kind": "program_postgres_dump", "path": "program.dump"}],
                    [
                        {
                            "artifact_id": "program/version/legacy-version",
                            "kind": "legacy_program_version",
                            "classification": "rejected",
                            "reason": "pre-release state is intentionally discarded",
                        }
                    ],
                ),
                {"PATH": str(command_directory)},
            )

            self.assertEqual(result.returncode, 1, msg=result.stderr)
            self.assertEqual(proof["counts"]["active_legacy_program_activations"], 1)
            self.assertTrue(
                any(
                    blocker["code"] == "ACTIVATED_LEGACY_PROGRAM_VERSION"
                    for blocker in proof["final_drain"]["blockers"]
                )
            )
            artifact = proof["legacy_artifacts"][0]
            self.assertTrue(artifact["active_or_recoverable"])
            self.assertEqual(artifact["active_activation_scopes"], ["tenant:test"])

    def test_activated_legacy_program_version_blocks_final_removal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = Path(temporary)
            create_program_store(
                snapshot / "program.db",
                [{"id": "legacy-bundle", "source_kind": "canonical_json"}],
                [{"id": "legacy-version", "bundle_id": "legacy-bundle"}],
                [
                    {
                        "owner_scope": "tenant:test",
                        "generation": 1,
                        "active_version_id": "legacy-version",
                        "policy_snapshot_hash": "sha256:" + "a" * 64,
                    }
                ],
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

            self.assertEqual(result.returncode, 1, msg=result.stderr)
            self.assertFalse(proof["final_drain"]["passed"])
            self.assertTrue(
                any(
                    blocker["code"] == "ACTIVATED_LEGACY_PROGRAM_VERSION"
                    for blocker in proof["final_drain"]["blockers"]
                )
            )

    def test_no_deployment_attestation_produces_a_distinct_final_proof(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            snapshot = Path(temporary)
            document = no_deployment_inventory()
            result, proof = self.run_audit(snapshot, document)

            self.assertEqual(result.returncode, 0, msg=result.stderr)
            self.assertTrue(proof["final_drain"]["passed"])
            self.assertEqual(proof["evidence_mode"], "no_deployment_attestation")
            self.assertEqual(proof["no_deployment_attestation"], document["no_deployment_attestation"])
            self.assertEqual(proof["scanned_stores"], [])
            self.assertEqual(proof["counts"]["legacy_artifacts"], 0)

    def test_no_deployment_attestation_rejects_any_snapshot_or_legacy_artifact(self) -> None:
        invalid_documents = [inventory([])]

        mixed_snapshot = no_deployment_inventory()
        mixed_snapshot["stores"] = [{"id": "program", "kind": "program_sqlite", "path": "program.db"}]
        invalid_documents.append(mixed_snapshot)

        declared_legacy = no_deployment_inventory()
        declared_legacy["legacy_artifacts"] = [
            {
                "artifact_id": "program/version/legacy-version",
                "kind": "legacy_program_version",
                "classification": "rejected",
                "reason": "pre-release state is intentionally discarded",
            }
        ]
        invalid_documents.append(declared_legacy)

        for document in invalid_documents:
            with self.subTest(document=document):
                with tempfile.TemporaryDirectory() as temporary:
                    result, proof_path = self.invoke_audit(Path(temporary), document)
                    self.assertEqual(result.returncode, 2, msg=result.stderr)
                    self.assertFalse(proof_path.exists())
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
