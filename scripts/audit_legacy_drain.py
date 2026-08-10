#!/usr/bin/env python3
"""Produce a deterministic, fail-closed proof for QuickJS legacy-runtime removal.

The audit consumes either a frozen, explicitly enumerated storage snapshot or
an explicit operator attestation that no pre-release or production deployment
ever existed. It never opens a database writable and does not infer legacy
status from a JSON shape: known Program/Harness records are classified from
their persisted source and frontend receipts; every discovered legacy record
needs an explicit terminal classification in the inventory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sqlite3
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path, PurePosixPath
from typing import Any, Iterable
from urllib.parse import quote


INVENTORY_FORMAT = "neograph-legacy-drain-inventory"
PROOF_FORMAT = "neograph-legacy-drain-proof"
SCHEMA_VERSION = 1
SHA256_PREFIX = "sha256:"
NO_DEPLOYMENT_STATEMENT = "no_pre_release_or_production_deployment_has_ever_existed"
NO_DEPLOYMENT_SCOPE = "all_neograph_program_and_harness_durable_state"



CURRENT_SOURCE_KINDS = {"javascript", "cpp_builder"}
LEGACY_SOURCE_KIND = "canonical_json"
LEGACY_KINDS = {"legacy_core_definition", "legacy_program_version"}
CLASSIFICATIONS = {"translated", "drain_only", "rejected"}
TERMINAL_RUN_STATUSES = {
    "completed",
    "cancelled",
    "budget_exhausted",
    "timed_out",
    "failed",
    "ambiguous_effect",
    "checkpoint_incompatible",
}
HARNESS_TERMINAL_RUN_STATUSES = {
    "completed",
    "failed",
    "cancelled",
    "timeout",
    "expired",
    "max_steps_exhausted",
}
STORE_KINDS = {
    "program_sqlite",
    "program_postgres_dump",
    "program_transitions_sqlite",
    "harness_sqlite",
    "harness_file",
}
SQLITE_STORE_KINDS = {
    "program_sqlite",
    "program_transitions_sqlite",
    "harness_sqlite",
}
SQLITE_LIVE_SIDECAR_SUFFIXES = ("-journal", "-shm", "-wal")


class AuditError(ValueError):
    """The supplied snapshot or inventory is not a valid audit input."""


@dataclass
class Blocker:
    code: str
    message: str
    artifact_id: str | None = None
    run_id: str | None = None
    store_id: str | None = None

    def as_json(self) -> dict[str, Any]:
        value: dict[str, Any] = {"code": self.code, "message": self.message}
        if self.artifact_id is not None:
            value["artifact_id"] = self.artifact_id
        if self.run_id is not None:
            value["run_id"] = self.run_id
        if self.store_id is not None:
            value["store_id"] = self.store_id
        return value


@dataclass
class LegacyArtifact:
    artifact_id: str
    kind: str
    source_kind: str
    store_id: str
    bundle_id: str | None = None
    active_runs: list[str] = field(default_factory=list)
    classification: dict[str, Any] | None = None
    active_activation_scopes: list[str] = field(default_factory=list)

    @property
    def active_or_recoverable(self) -> bool:
        return bool(self.active_runs or self.active_activation_scopes)

    def as_json(self) -> dict[str, Any]:
        value: dict[str, Any] = {
            "artifact_id": self.artifact_id,
            "kind": self.kind,
            "source_kind": self.source_kind,
            "store_id": self.store_id,
            "active_or_recoverable": self.active_or_recoverable,
            "active_run_ids": sorted(self.active_runs),
            "active_activation_scopes": sorted(self.active_activation_scopes),
        }
        if self.bundle_id is not None:
            value["bundle_id"] = self.bundle_id
        if self.classification is not None:
            value["classification"] = self.classification["classification"]
            for key in (
                "reason",
                "replacement_artifact_id",
                "equivalence_proof",
                "legacy_runtime_identity",
            ):
                if key in self.classification:
                    value[key] = self.classification[key]
        return value


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def sha256_bytes(value: bytes) -> str:
    return SHA256_PREFIX + hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return SHA256_PREFIX + digest.hexdigest()


def sha256_regular_file(path: Path, context: str) -> str:
    if not path.is_file() or path.is_symlink():
        raise AuditError(f"{context} is not a non-symlink regular file: {path}")
    return sha256_file(path)


def sqlite_sidecar_suffixes(path: Path) -> tuple[str, ...]:
    return tuple(
        suffix
        for suffix in SQLITE_LIVE_SIDECAR_SUFFIXES
        if (candidate := path.with_name(path.name + suffix)).exists() or candidate.is_symlink()
    )


def sha256_directory(path: Path) -> str:
    if not path.is_dir() or path.is_symlink():
        raise AuditError(f"snapshot directory is not a non-symlink directory: {path}")
    entries: list[dict[str, str]] = []
    for candidate in sorted(path.rglob("*")):
        if candidate.is_symlink():
            raise AuditError(f"snapshot directory contains a symlink: {candidate}")
        if candidate.is_dir():
            continue
        if not candidate.is_file():
            raise AuditError(f"snapshot directory contains a non-regular file: {candidate}")
        entries.append(
            {
                "path": candidate.relative_to(path).as_posix(),
                "content_identity": sha256_file(candidate),
            }
        )
    return sha256_bytes(canonical_json(entries))


def require_object(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise AuditError(f"{context} must be an object")
    return value


def require_string(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise AuditError(f"{context} must be a non-empty string")
    return value


def require_bool(value: Any, context: str) -> bool:
    if not isinstance(value, bool):
        raise AuditError(f"{context} must be a boolean")
    return value


def require_positive_integer(value: Any, context: str) -> int:
    if isinstance(value, str):
        if not value.isascii() or not value.isdecimal() or value.startswith("0"):
            raise AuditError(f"{context} must be a positive integer")
        value = int(value)
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise AuditError(f"{context} must be a positive integer")
    return value


def require_exact_keys(value: dict[str, Any], context: str, keys: set[str]) -> None:
    actual = set(value)
    missing = keys - actual
    extra = actual - keys
    if missing or extra:
        raise AuditError(f"{context} keys mismatch: missing={sorted(missing)} extra={sorted(extra)}")


def is_sha256_identity(value: Any) -> bool:
    if not isinstance(value, str) or len(value) != 71 or not value.startswith(SHA256_PREFIX):
        return False
    return all(character in "0123456789abcdef" for character in value[len(SHA256_PREFIX) :])


def parse_timestamp(value: Any, context: str) -> str:
    text = require_string(value, context)
    try:
        parsed = datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError as error:
        raise AuditError(f"{context} must be an RFC 3339 timestamp") from error
    if parsed.tzinfo is None:
        raise AuditError(f"{context} must include a timezone")
    return text

def validate_no_deployment_attestation(value: Any) -> dict[str, Any]:
    attestation = require_object(value, "inventory.no_deployment_attestation")
    require_exact_keys(
        attestation,
        "inventory.no_deployment_attestation",
        {"attestation_id", "attested_by", "statement", "scope"},
    )
    require_string(attestation["attestation_id"], "inventory.no_deployment_attestation.attestation_id")
    require_string(attestation["attested_by"], "inventory.no_deployment_attestation.attested_by")
    if attestation["statement"] != NO_DEPLOYMENT_STATEMENT:
        raise AuditError("inventory.no_deployment_attestation.statement is unsupported")
    if attestation["scope"] != NO_DEPLOYMENT_SCOPE:
        raise AuditError("inventory.no_deployment_attestation.scope is unsupported")
    return attestation




def resolve_snapshot_path(root: Path, relative_path: Any, context: str) -> Path:
    text = require_string(relative_path, context)
    pure = PurePosixPath(text)
    if pure.is_absolute() or any(part in {"", ".", ".."} for part in pure.parts):
        raise AuditError(f"{context} must be a normalized relative path")
    candidate = root
    for part in pure.parts:
        candidate = candidate / part
        if candidate.is_symlink():
            raise AuditError(f"{context} traverses a symlink: {candidate}")
    resolved = candidate.resolve()
    if not resolved.is_relative_to(root):
        raise AuditError(f"{context} escapes the snapshot root")
    return resolved


def parse_inventory(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AuditError(f"cannot read inventory {path}: {error}") from error
    document = require_object(document, "inventory")
    base_keys = {
        "format",
        "schema_version",
        "cutover_id",
        "captured_at",
        "inventory_complete",
        "stores",
        "legacy_artifacts",
    }
    has_no_deployment_attestation = "no_deployment_attestation" in document
    require_exact_keys(
        document,
        "inventory",
        base_keys | ({"no_deployment_attestation"} if has_no_deployment_attestation else set()),
    )
    if document["format"] != INVENTORY_FORMAT:
        raise AuditError("inventory format is unsupported")
    if document["schema_version"] != SCHEMA_VERSION:
        raise AuditError("inventory schema_version is unsupported")
    require_string(document["cutover_id"], "inventory.cutover_id")
    parse_timestamp(document["captured_at"], "inventory.captured_at")
    require_bool(document["inventory_complete"], "inventory.inventory_complete")

    if has_no_deployment_attestation:
        validate_no_deployment_attestation(document["no_deployment_attestation"])

    stores = document["stores"]
    if not isinstance(stores, list):
        raise AuditError("inventory.stores must be an array")
    if not stores and not has_no_deployment_attestation:
        raise AuditError("inventory.stores must be a non-empty array without no_deployment_attestation")
    if stores and has_no_deployment_attestation:
        raise AuditError("inventory.no_deployment_attestation requires inventory.stores to be empty")
    seen_store_ids: set[str] = set()
    for index, store in enumerate(stores):
        store = require_object(store, f"inventory.stores[{index}]")
        require_exact_keys(store, f"inventory.stores[{index}]", {"id", "kind", "path"})
        store_id = require_string(store["id"], f"inventory.stores[{index}].id")
        if store_id in seen_store_ids:
            raise AuditError(f"inventory.stores has duplicate id {store_id!r}")
        seen_store_ids.add(store_id)
        if store["kind"] not in STORE_KINDS:
            raise AuditError(f"inventory.stores[{index}].kind is unsupported")
        require_string(store["path"], f"inventory.stores[{index}].path")

    entries = document["legacy_artifacts"]
    if not isinstance(entries, list):
        raise AuditError("inventory.legacy_artifacts must be an array")
    if has_no_deployment_attestation and entries:
        raise AuditError("inventory.no_deployment_attestation requires legacy_artifacts to be empty")
    seen_artifact_ids: set[str] = set()
    for index, entry in enumerate(entries):
        validate_classification_entry(entry, f"inventory.legacy_artifacts[{index}]")
        artifact_id = entry["artifact_id"]
        if artifact_id in seen_artifact_ids:
            raise AuditError(f"inventory.legacy_artifacts has duplicate artifact_id {artifact_id!r}")
        seen_artifact_ids.add(artifact_id)
    return document


def validate_classification_entry(value: Any, context: str) -> dict[str, Any]:
    entry = require_object(value, context)
    base = {"artifact_id", "kind", "classification"}
    if not base.issubset(entry):
        raise AuditError(f"{context} is missing one of {sorted(base)}")
    artifact_id = require_string(entry["artifact_id"], f"{context}.artifact_id")
    del artifact_id
    kind = require_string(entry["kind"], f"{context}.kind")
    if kind not in LEGACY_KINDS:
        raise AuditError(f"{context}.kind is unsupported")
    classification = require_string(entry["classification"], f"{context}.classification")
    if classification not in CLASSIFICATIONS:
        raise AuditError(f"{context}.classification is unsupported")

    if classification == "translated":
        require_exact_keys(
            entry,
            context,
            {"artifact_id", "kind", "classification", "replacement_artifact_id", "equivalence_proof"},
        )
        replacement = require_string(entry["replacement_artifact_id"], f"{context}.replacement_artifact_id")
        if replacement == entry["artifact_id"]:
            raise AuditError(f"{context}.replacement_artifact_id must differ from artifact_id")
        if not is_sha256_identity(entry["equivalence_proof"]):
            raise AuditError(f"{context}.equivalence_proof must be a sha256 identity")
    elif classification == "drain_only":
        require_exact_keys(
            entry,
            context,
            {"artifact_id", "kind", "classification", "legacy_runtime_identity"},
        )
        require_string(entry["legacy_runtime_identity"], f"{context}.legacy_runtime_identity")
    else:
        require_exact_keys(entry, context, {"artifact_id", "kind", "classification", "reason"})
        require_string(entry["reason"], f"{context}.reason")
    return entry


def sqlite_connection(path: Path) -> sqlite3.Connection:
    if not path.is_file() or path.is_symlink():
        raise AuditError(f"SQLite snapshot is not a non-symlink regular file: {path}")
    try:
        return sqlite3.connect(path.as_uri() + "?mode=ro&immutable=1", uri=True)
    except sqlite3.Error as error:
        raise AuditError(f"cannot open SQLite snapshot read-only {path}: {error}") from error


def sqlite_tables(connection: sqlite3.Connection) -> set[str]:
    try:
        return {row[0] for row in connection.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    except sqlite3.Error as error:
        raise AuditError(f"cannot inspect SQLite schema: {error}") from error


def require_sqlite_tables(connection: sqlite3.Connection, store_id: str, expected: set[str]) -> None:
    actual = sqlite_tables(connection)
    missing = expected - actual
    if missing:
        raise AuditError(f"store {store_id!r} is missing expected SQLite tables {sorted(missing)}")


def decode_stored_json(value: Any, context: str) -> dict[str, Any]:
    if isinstance(value, bytes):
        try:
            text = value.decode("utf-8")
        except UnicodeDecodeError as error:
            raise AuditError(f"{context} is not UTF-8 JSON") from error
    elif isinstance(value, str):
        text = value
    else:
        raise AuditError(f"{context} is not a JSON text/blob")
    try:
        return require_object(json.loads(text), context)
    except json.JSONDecodeError as error:
        raise AuditError(f"{context} is invalid JSON") from error


def source_kind_from_bundle(bundle: dict[str, Any], context: str) -> str:
    require_string(bundle.get("id"), f"{context}.id")
    source_kind = require_string(bundle.get("source_kind"), f"{context}.source_kind")
    if source_kind not in CURRENT_SOURCE_KINDS | {LEGACY_SOURCE_KIND}:
        raise AuditError(f"{context}.source_kind is unknown: {source_kind!r}")
    return source_kind


def classify_program_records(
    bundle_rows: Iterable[tuple[Any, Any]],
    version_rows: Iterable[tuple[Any, Any, Any, Any]],
    activation_rows: Iterable[tuple[Any, Any, Any, Any, Any]],
    store_id: str,
    artifacts: dict[str, LegacyArtifact],
    legacy_version_candidates: dict[tuple[str, str], list[str]],
    legacy_bundle_ids: set[str],
    known_bundle_ids: set[str],
    known_artifact_ids: set[str],
) -> None:
    bundles: dict[str, tuple[dict[str, Any], str]] = {}
    for stored_id, canonical_bytes in bundle_rows:
        bundle_id = require_string(stored_id, f"program store {store_id} bundle row id")
        bundle = decode_stored_json(canonical_bytes, f"program store {store_id} bundle {bundle_id}")
        if bundle.get("id") != bundle_id:
            raise AuditError(f"program store {store_id} bundle {bundle_id!r} identity mismatch")
        if bundle_id in bundles:
            raise AuditError(f"program store {store_id} has duplicate bundle {bundle_id!r}")
        bundles[bundle_id] = (bundle, source_kind_from_bundle(bundle, f"program store {store_id} bundle {bundle_id}"))
        known_bundle_ids.add(bundle_id)

    versions_by_bundle: dict[str, list[str]] = {bundle_id: [] for bundle_id in bundles}
    versions_by_id: dict[str, tuple[str, str]] = {}
    for stored_id, stored_bundle_id, owner_scope, canonical_bytes in version_rows:
        version_id = require_string(stored_id, f"program store {store_id} version row id")
        bundle_id = require_string(stored_bundle_id, f"program store {store_id} version {version_id}.bundle_id")
        owner_scope = require_string(owner_scope, f"program store {store_id} version {version_id}.owner_scope")
        version = decode_stored_json(canonical_bytes, f"program store {store_id} version {version_id}")
        if version.get("id") != version_id or version.get("bundle_id") != bundle_id:
            raise AuditError(f"program store {store_id} version {version_id!r} identity mismatch")
        if bundle_id not in bundles:
            raise AuditError(
                f"program store {store_id} version {version_id!r} references missing bundle {bundle_id!r}"
            )
        if version_id in versions_by_id:
            raise AuditError(f"program store {store_id} has duplicate version {version_id!r}")
        versions_by_bundle[bundle_id].append(version_id)
        versions_by_id[version_id] = (bundle_id, owner_scope)
        known_artifact_ids.add(f"{store_id}/version/{version_id}")

    for bundle_id, (_, source_kind) in bundles.items():
        if source_kind != LEGACY_SOURCE_KIND:
            continue
        legacy_bundle_ids.add(bundle_id)
        version_ids = versions_by_bundle[bundle_id]
        if version_ids:
            for version_id in version_ids:
                artifact_id = f"{store_id}/version/{version_id}"
                artifacts[artifact_id] = LegacyArtifact(
                    artifact_id=artifact_id,
                    kind="legacy_program_version",
                    source_kind=source_kind,
                    store_id=store_id,
                    bundle_id=bundle_id,
                )
                legacy_version_candidates.setdefault((bundle_id, version_id), []).append(artifact_id)
        else:
            artifact_id = f"{store_id}/bundle/{bundle_id}"
            artifacts[artifact_id] = LegacyArtifact(
                artifact_id=artifact_id,
                kind="legacy_core_definition",
                source_kind=source_kind,
                store_id=store_id,
                bundle_id=bundle_id,
            )
            known_artifact_ids.add(artifact_id)

    for stored_owner_scope, stored_generation, stored_active_version_id, stored_policy_hash, canonical_bytes in activation_rows:
        owner_scope = require_string(stored_owner_scope, f"program store {store_id} activation row owner_scope")
        generation = require_positive_integer(stored_generation, f"program store {store_id} activation {owner_scope}.generation")
        active_version_id = require_string(
            stored_active_version_id, f"program store {store_id} activation {owner_scope}.active_version_id"
        )
        policy_hash = require_string(
            stored_policy_hash, f"program store {store_id} activation {owner_scope}.policy_snapshot_hash"
        )
        activation = decode_stored_json(canonical_bytes, f"program store {store_id} activation {owner_scope}")
        if (
            activation.get("owner_scope") != owner_scope
            or activation.get("generation") != generation
            or activation.get("active_version_id") != active_version_id
            or activation.get("policy_snapshot_hash") != policy_hash
        ):
            raise AuditError(f"program store {store_id} activation {owner_scope!r} identity mismatch")
        version = versions_by_id.get(active_version_id)
        if version is None:
            raise AuditError(
                f"program store {store_id} activation {owner_scope!r} references missing version {active_version_id!r}"
            )
        bundle_id, version_owner_scope = version
        if version_owner_scope != owner_scope:
            raise AuditError(
                f"program store {store_id} activation {owner_scope!r} targets a version owned by {version_owner_scope!r}"
            )
        if bundles[bundle_id][1] != LEGACY_SOURCE_KIND:
            continue
        artifact_id = f"{store_id}/version/{active_version_id}"
        artifacts[artifact_id].active_activation_scopes.append(owner_scope)


def classify_program_store(
    connection: sqlite3.Connection,
    store_id: str,
    artifacts: dict[str, LegacyArtifact],
    legacy_version_candidates: dict[tuple[str, str], list[str]],
    legacy_bundle_ids: set[str],
    known_bundle_ids: set[str],
    known_artifact_ids: set[str],
) -> None:
    require_sqlite_tables(connection, store_id, {"program_bundles", "program_versions", "program_activations"})
    try:
        bundle_rows = list(connection.execute("SELECT id, canonical_bytes FROM program_bundles ORDER BY id"))
        version_rows = list(
            connection.execute(
                "SELECT id, bundle_id, owner_scope, canonical_bytes FROM program_versions ORDER BY id"
            )
        )
        activation_rows = list(
            connection.execute(
                "SELECT owner_scope, generation, active_version_id, policy_snapshot_hash, canonical_bytes "
                "FROM program_activations ORDER BY owner_scope"
            )
        )
    except sqlite3.Error as error:
        raise AuditError(f"cannot query Program SQLite store {store_id!r}: {error}") from error
    classify_program_records(
        bundle_rows,
        version_rows,
        activation_rows,
        store_id,
        artifacts,
        legacy_version_candidates,
        legacy_bundle_ids,
        known_bundle_ids,
        known_artifact_ids,
    )


POSTGRES_PROGRAM_TABLES = (
    ("neograph_program_bundles", ("id", "canonical_bytes")),
    ("neograph_program_versions", ("id", "bundle_id", "owner_scope", "canonical_bytes")),
    (
        "neograph_program_activations",
        ("owner_scope", "generation", "active_version_id", "policy_snapshot_hash", "canonical_bytes"),
    ),
)


def decode_postgres_copy_field(value: str, context: str) -> str | None:
    if value == r"\N":
        return None
    decoded: list[str] = []
    index = 0
    escaped_characters = {
        "b": "\b",
        "f": "\f",
        "n": "\n",
        "r": "\r",
        "t": "\t",
        "v": "\v",
    }
    while index < len(value):
        character = value[index]
        if character != "\\":
            decoded.append(character)
            index += 1
            continue
        index += 1
        if index == len(value):
            raise AuditError(f"{context} has an unterminated PostgreSQL COPY escape")
        escaped = value[index]
        if escaped in "01234567x":
            raise AuditError(f"{context} uses an unsupported PostgreSQL COPY numeric escape")
        decoded.append(escaped_characters.get(escaped, escaped))
        index += 1
    return "".join(decoded)


def parse_postgres_copy_rows(
    output: str,
    store_id: str,
    table: str,
    expected_columns: tuple[str, ...],
) -> list[tuple[str, ...]]:
    rows: list[tuple[str, ...]] = []
    copy_blocks = 0
    lines = output.splitlines()
    index = 0
    while index < len(lines):
        line = lines[index]
        if not line.startswith("COPY "):
            index += 1
            continue
        copy_blocks += 1
        if copy_blocks > 1:
            raise AuditError(f"PostgreSQL dump store {store_id!r} table {table!r} emitted multiple COPY blocks")
        suffix = " FROM stdin;"
        if not line.endswith(suffix):
            raise AuditError(f"PostgreSQL dump store {store_id!r} table {table!r} emitted an invalid COPY header")
        try:
            relation, encoded_columns = line[len("COPY ") : -len(suffix)].split(" (", 1)
        except ValueError as error:
            raise AuditError(
                f"PostgreSQL dump store {store_id!r} table {table!r} emitted an invalid COPY target"
            ) from error
        if relation != f"public.{table}" or not encoded_columns.endswith(")"):
            raise AuditError(f"PostgreSQL dump store {store_id!r} table {table!r} emitted an unexpected COPY target")
        columns = tuple(column.strip() for column in encoded_columns[:-1].split(","))
        if columns != expected_columns:
            raise AuditError(f"PostgreSQL dump store {store_id!r} table {table!r} has an unexpected COPY column layout")
        index += 1
        while index < len(lines):
            row = lines[index]
            index += 1
            if row == r"\.":
                break
            fields = row.split("\t")
            if len(fields) != len(expected_columns):
                raise AuditError(
                    f"PostgreSQL dump store {store_id!r} table {table!r} has a COPY row with the wrong field count"
                )
            decoded = [
                decode_postgres_copy_field(
                    field,
                    f"PostgreSQL dump store {store_id!r} table {table!r} COPY row {len(rows) + 1}",
                )
                for field in fields
            ]
            if any(field is None for field in decoded):
                raise AuditError(f"PostgreSQL dump store {store_id!r} table {table!r} has a null required field")
            rows.append(tuple(field for field in decoded if field is not None))
        else:
            raise AuditError(f"PostgreSQL dump store {store_id!r} table {table!r} has an unterminated COPY block")
    if copy_blocks != 1:
        raise AuditError(f"PostgreSQL dump store {store_id!r} table {table!r} has no COPY data block")
    return rows


def postgres_dump_table_rows(
    path: Path,
    store_id: str,
    table: str,
    expected_columns: tuple[str, ...],
) -> list[tuple[str, ...]]:
    pg_restore = shutil.which("pg_restore")
    if pg_restore is None:
        raise AuditError("pg_restore is required to scan a program_postgres_dump snapshot")
    # pg_restore 18 docs, fetched 2026-08-10:
    # --file=- emits a script instead of restoring, and --strict-names rejects
    # an archive that lacks an expected table.
    # https://www.postgresql.org/docs/current/app-pgrestore.html
    try:
        result = subprocess.run(
            [
                pg_restore,
                "--data-only",
                "--strict-names",
                f"--table=public.{table}",
                "--file=-",
                str(path),
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        raise AuditError(f"cannot execute pg_restore for PostgreSQL dump store {store_id!r}: {error}") from error
    if result.returncode:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise AuditError(
            f"pg_restore cannot extract PostgreSQL dump store {store_id!r} table {table!r}: "
            f"{detail or f'exit status {result.returncode}'}"
        )
    try:
        output = result.stdout.decode("utf-8")
    except UnicodeDecodeError as error:
        raise AuditError(
            f"pg_restore emitted non-UTF-8 data for PostgreSQL dump store {store_id!r} table {table!r}"
        ) from error
    return parse_postgres_copy_rows(output, store_id, table, expected_columns)


def scan_postgres_program_dump(
    path: Path,
    store_id: str,
    artifacts: dict[str, LegacyArtifact],
    legacy_version_candidates: dict[tuple[str, str], list[str]],
    legacy_bundle_ids: set[str],
    known_bundle_ids: set[str],
    known_artifact_ids: set[str],
) -> None:
    extracted = {
        table: postgres_dump_table_rows(path, store_id, table, columns)
        for table, columns in POSTGRES_PROGRAM_TABLES
    }
    classify_program_records(
        extracted["neograph_program_bundles"],
        extracted["neograph_program_versions"],
        extracted["neograph_program_activations"],
        store_id,
        artifacts,
        legacy_version_candidates,
        legacy_bundle_ids,
        known_bundle_ids,
        known_artifact_ids,
    )

def program_run_is_recoverable(run: dict[str, Any], context: str) -> bool:
    terminal_result = run.get("terminal_result")
    if terminal_result is None:
        return True
    terminal_result = require_object(terminal_result, f"{context}.terminal_result")
    status = require_string(terminal_result.get("status"), f"{context}.terminal_result.status")
    if status == "interrupted":
        return True
    if status in TERMINAL_RUN_STATUSES:
        return False
    raise AuditError(f"{context}.terminal_result.status is unknown: {status!r}")


def classify_transition_store(
    connection: sqlite3.Connection,
    store_id: str,
    artifacts: dict[str, LegacyArtifact],
    legacy_version_candidates: dict[tuple[str, str], list[str]],
    legacy_bundle_ids: set[str],
    known_bundle_ids: set[str],
) -> list[Blocker]:
    require_sqlite_tables(connection, store_id, {"program_transition_run_heads_v2"})
    try:
        rows = list(
            connection.execute(
                "SELECT owner_scope, run_id, run_record_bytes "
                "FROM program_transition_run_heads_v2 ORDER BY owner_scope, run_id"
            )
        )
    except sqlite3.Error as error:
        raise AuditError(f"cannot query Program transition store {store_id!r}: {error}") from error

    blockers: list[Blocker] = []
    for owner_scope, stored_run_id, run_record_bytes in rows:
        run_id = require_string(stored_run_id, f"transition store {store_id} run row id")
        run = decode_stored_json(run_record_bytes, f"transition store {store_id} run {run_id}")
        if run.get("run_id") != run_id or run.get("owner_scope") != owner_scope:
            raise AuditError(f"transition store {store_id} run {run_id!r} identity mismatch")
        bundle_id = require_string(run.get("bundle_id"), f"transition store {store_id} run {run_id}.bundle_id")
        version_id = require_string(
            run.get("program_version_id"), f"transition store {store_id} run {run_id}.program_version_id"
        )
        if bundle_id not in known_bundle_ids:
            blockers.append(
                Blocker(
                    "TRANSITION_REFERENCES_UNSCANNED_BUNDLE",
                    "A persisted Program run references a bundle absent from every scanned Program store",
                    run_id=run_id,
                    store_id=store_id,
                )
            )
            continue
        if bundle_id not in legacy_bundle_ids:
            continue
        candidate_ids = legacy_version_candidates.get((bundle_id, version_id))
        if not candidate_ids:
            blockers.append(
                Blocker(
                    "TRANSITION_REFERENCES_UNSCANNED_LEGACY_VERSION",
                    "A persisted legacy Program run does not resolve to a scanned legacy version",
                    run_id=run_id,
                    store_id=store_id,
                )
            )
            continue
        if not program_run_is_recoverable(run, f"transition store {store_id} run {run_id}"):
            continue
        for artifact_id in candidate_ids:
            artifacts[artifact_id].active_runs.append(f"{store_id}/{run_id}")
    return blockers


def harness_run_is_recoverable(run: dict[str, Any], context: str) -> bool:
    status = run.get("status")
    if not isinstance(status, str) or not status:
        return True
    return status not in HARNESS_TERMINAL_RUN_STATUSES


def classify_harness_records(
    store_id: str,
    artifact_records: Iterable[tuple[str, dict[str, Any]]],
    run_records: Iterable[tuple[str, dict[str, Any]]],
    artifacts: dict[str, LegacyArtifact],
    known_artifact_ids: set[str],
) -> list[Blocker]:
    candidate_ids_by_harness_id: dict[str, list[str]] = {}
    all_harness_artifact_ids: set[str] = set()

    for stored_id, record in artifact_records:
        artifact_id = require_string(stored_id, f"harness store {store_id} artifact row id")
        all_harness_artifact_ids.add(artifact_id)
        candidate_id: str | None = None
        source_kind = "unknown"
        kind = "legacy_core_definition"

        if record.get("format") == "neograph-harness-program-adapter-artifact":
            if record.get("storage_schema_version") != 2:
                raise AuditError(f"harness store {store_id} artifact {artifact_id!r} has an unsupported schema")
            if record.get("artifact_id") != artifact_id:
                raise AuditError(f"harness store {store_id} artifact {artifact_id!r} identity mismatch")
            bundle_text = require_string(record.get("bundle"), f"harness store {store_id} artifact {artifact_id}.bundle")
            bundle = decode_stored_json(bundle_text, f"harness store {store_id} artifact {artifact_id}.bundle")
            source_kind = source_kind_from_bundle(bundle, f"harness store {store_id} artifact {artifact_id}.bundle")
            projection = require_object(record.get("projection"), f"harness store {store_id} artifact {artifact_id}.projection")
            frontend = projection.get("authoring_frontend")
            if source_kind == "javascript" and frontend == "javascript":
                known_artifact_ids.add(f"{store_id}/artifact/{artifact_id}")
                continue
            if source_kind == "cpp_builder" and frontend == "trusted_cpp":
                known_artifact_ids.add(f"{store_id}/artifact/{artifact_id}")
                continue
            kind = "legacy_program_version"
        else:
            # A pre-Program Harness record has no sealed bundle/source receipt.  It is
            # legacy Core source until a migration record proves otherwise.
            source_kind = "unknown"
            kind = "legacy_core_definition"

        candidate_id = f"{store_id}/artifact/{artifact_id}"
        artifacts[candidate_id] = LegacyArtifact(
            artifact_id=candidate_id,
            kind=kind,
            source_kind=source_kind,
            store_id=store_id,
        )
        candidate_ids_by_harness_id.setdefault(artifact_id, []).append(candidate_id)
        known_artifact_ids.add(candidate_id)

    blockers: list[Blocker] = []
    for stored_run_id, record in run_records:
        run_id = require_string(stored_run_id, f"harness store {store_id} run row id")
        if record.get("run_id") not in {None, run_id}:
            raise AuditError(f"harness store {store_id} run {run_id!r} identity mismatch")
        harness_artifact_id = require_string(
            record.get("artifact_id"), f"harness store {store_id} run {run_id}.artifact_id"
        )
        if harness_artifact_id not in all_harness_artifact_ids:
            blockers.append(
                Blocker(
                    "HARNESS_RUN_REFERENCES_UNSCANNED_ARTIFACT",
                    "A persisted Harness run references an artifact absent from the scanned snapshot",
                    run_id=run_id,
                    store_id=store_id,
                )
            )
            continue
        if not harness_run_is_recoverable(record, f"harness store {store_id} run {run_id}"):
            continue
        for candidate_id in candidate_ids_by_harness_id.get(harness_artifact_id, []):
            artifacts[candidate_id].active_runs.append(f"{store_id}/{run_id}")
    return blockers


def json_files(directory: Path, context: str) -> list[Path]:
    if not directory.is_dir() or directory.is_symlink():
        raise AuditError(f"{context} must be a non-symlink directory")
    paths: list[Path] = []
    for candidate in sorted(directory.iterdir()):
        if candidate.is_symlink() or not candidate.is_file() or candidate.suffix != ".json":
            raise AuditError(f"{context} contains an unrecognized entry: {candidate.name}")
        paths.append(candidate)
    return paths


def read_json_record(path: Path, context: str) -> dict[str, Any]:
    try:
        return require_object(json.loads(path.read_text(encoding="utf-8")), context)
    except (OSError, json.JSONDecodeError) as error:
        raise AuditError(f"cannot read {context}: {error}") from error


def scan_harness_file(
    path: Path,
    store_id: str,
    artifacts: dict[str, LegacyArtifact],
    known_artifact_ids: set[str],
) -> list[Blocker]:
    artifact_directory = path / "artifacts"
    run_directory = path / "runs"
    artifact_records = [
        (record_path.stem, read_json_record(record_path, f"harness store {store_id} artifact {record_path.name}"))
        for record_path in json_files(artifact_directory, f"harness store {store_id} artifacts")
    ]
    run_records = [
        (record_path.stem, read_json_record(record_path, f"harness store {store_id} run {record_path.name}"))
        for record_path in json_files(run_directory, f"harness store {store_id} runs")
    ]
    return classify_harness_records(store_id, artifact_records, run_records, artifacts, known_artifact_ids)


def scan_harness_sqlite(
    connection: sqlite3.Connection,
    store_id: str,
    artifacts: dict[str, LegacyArtifact],
    known_artifact_ids: set[str],
) -> list[Blocker]:
    require_sqlite_tables(connection, store_id, {"neograph_harness_artifacts", "neograph_harness_runs"})
    try:
        artifact_rows = list(
            connection.execute(
                "SELECT artifact_id, record_json FROM neograph_harness_artifacts ORDER BY artifact_id"
            )
        )
        run_rows = list(connection.execute("SELECT run_id, record_json FROM neograph_harness_runs ORDER BY run_id"))
    except sqlite3.Error as error:
        raise AuditError(f"cannot query Harness SQLite store {store_id!r}: {error}") from error
    artifact_records = [
        (require_string(stored_id, f"harness store {store_id} artifact row id"), decode_stored_json(record, f"harness store {store_id} artifact {stored_id}"))
        for stored_id, record in artifact_rows
    ]
    run_records = [
        (require_string(stored_id, f"harness store {store_id} run row id"), decode_stored_json(record, f"harness store {store_id} run {stored_id}"))
        for stored_id, record in run_rows
    ]
    return classify_harness_records(store_id, artifact_records, run_records, artifacts, known_artifact_ids)


def attach_classifications(
    artifacts: dict[str, LegacyArtifact],
    declarations: list[dict[str, Any]],
    known_artifact_ids: set[str],
) -> list[Blocker]:
    blockers: list[Blocker] = []
    declared_by_id = {entry["artifact_id"]: entry for entry in declarations}
    for artifact_id, declaration in declared_by_id.items():
        if artifact_id not in artifacts:
            blockers.append(
                Blocker(
                    "DECLARED_ARTIFACT_NOT_FOUND",
                    "The inventory classifies an artifact absent from the scanned snapshot",
                    artifact_id=artifact_id,
                )
            )
            continue
        artifact = artifacts[artifact_id]
        if declaration["kind"] != artifact.kind:
            blockers.append(
                Blocker(
                    "LEGACY_ARTIFACT_KIND_MISMATCH",
                    "The declared legacy artifact kind differs from the discovered persisted source",
                    artifact_id=artifact_id,
                    store_id=artifact.store_id,
                )
            )
            continue
        artifact.classification = declaration

    for artifact in artifacts.values():
        if artifact.classification is None:
            blockers.append(
                Blocker(
                    "UNCLASSIFIED_LEGACY_ARTIFACT",
                    "Every discovered legacy source must have an explicit terminal migration classification",
                    artifact_id=artifact.artifact_id,
                    store_id=artifact.store_id,
                )
            )
            continue
        classification = artifact.classification["classification"]
        if classification == "translated":
            replacement = artifact.classification["replacement_artifact_id"]
            if replacement not in known_artifact_ids:
                blockers.append(
                    Blocker(
                        "TRANSLATION_REPLACEMENT_NOT_FOUND",
                        "A translated legacy artifact references a replacement absent from the scanned snapshot",
                        artifact_id=artifact.artifact_id,
                        store_id=artifact.store_id,
                    )
                )
        if classification == "drain_only":
            blockers.append(
                Blocker(
                    "DRAIN_ONLY_ARTIFACT_REMAINS",
                    "A drain-only artifact still requires the legacy runtime and must be purged before Q7 removal",
                    artifact_id=artifact.artifact_id,
                    store_id=artifact.store_id,
                )
            )
        if artifact.active_runs:
            blockers.append(
                Blocker(
                    "ACTIVE_OR_RECOVERABLE_LEGACY_RUN",
                    "A legacy source still has an active or recoverable persisted run",
                    artifact_id=artifact.artifact_id,
                    run_id=",".join(sorted(artifact.active_runs)),
                    store_id=artifact.store_id,
                )
            )
        if artifact.active_activation_scopes:
            blockers.append(
                Blocker(
                    "ACTIVATED_LEGACY_PROGRAM_VERSION",
                    "A legacy Program version remains active for owner scopes: "
                    + ", ".join(sorted(artifact.active_activation_scopes)),
                    artifact_id=artifact.artifact_id,
                    store_id=artifact.store_id,
                )
            )
    return blockers


def count_artifacts(artifacts: Iterable[LegacyArtifact]) -> dict[str, int]:
    values = list(artifacts)
    active_runs = {run_id for value in values for run_id in value.active_runs}
    return {
        "legacy_artifacts": len(values),
        "translated_legacy_artifacts": sum(
            value.classification is not None and value.classification["classification"] == "translated"
            for value in values
        ),
        "drain_only_legacy_artifacts": sum(
            value.classification is not None and value.classification["classification"] == "drain_only"
            for value in values
        ),
        "rejected_legacy_artifacts": sum(
            value.classification is not None and value.classification["classification"] == "rejected"
            for value in values
        ),
        "unclassified_legacy_artifacts": sum(value.classification is None for value in values),
        "active_or_recoverable_legacy_runs": len(active_runs),
        "active_legacy_program_activations": sum(
            len(set(value.active_activation_scopes)) for value in values
        ),
    }


def validate_output_path(output: Path, store_paths: list[Path]) -> None:
    resolved = output.resolve()
    for store_path in store_paths:
        if resolved == store_path or (store_path.is_dir() and resolved.is_relative_to(store_path)):
            raise AuditError("output path must not be inside or equal to a scanned storage target")


def write_json_atomically(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(canonical_json(value) + b"\n")
    temporary.replace(path)


def audit(inventory: dict[str, Any], root: Path) -> dict[str, Any]:
    artifacts: dict[str, LegacyArtifact] = {}
    legacy_version_candidates: dict[tuple[str, str], list[str]] = {}
    legacy_bundle_ids: set[str] = set()
    known_bundle_ids: set[str] = set()
    known_artifact_ids: set[str] = set()
    blockers: list[Blocker] = []
    resolved_stores: list[tuple[dict[str, Any], Path, str, tuple[str, ...]]] = []

    # Capture every identity before parsing any target. The scan order below is
    # deliberately semantic rather than inventory order: transition records
    # need the complete Program bundle/version index, not merely a preceding
    # declaration in an operator-authored JSON array.
    for store in inventory["stores"]:
        store_id = store["id"]
        store_kind = store["kind"]
        path = resolve_snapshot_path(root, store["path"], f"inventory store {store_id!r} path")
        sidecars = sqlite_sidecar_suffixes(path) if store_kind in SQLITE_STORE_KINDS else ()
        content_identity = (
            sha256_directory(path)
            if store_kind == "harness_file"
            else sha256_regular_file(path, f"inventory store {store_id!r} snapshot")
        )
        resolved_stores.append((store, path, content_identity, sidecars))
        if sidecars:
            blockers.append(
                Blocker(
                    "SQLITE_SNAPSHOT_HAS_LIVE_SIDECAR",
                    "SQLite snapshot has live sidecar files: " + ", ".join(sidecars),
                    store_id=store_id,
                )
            )

    for expected_kind in (
        "program_sqlite",
        "program_postgres_dump",
        "program_transitions_sqlite",
        "harness_sqlite",
        "harness_file",
    ):
        for store, path, _, sidecars in resolved_stores:
            if store["kind"] != expected_kind:
                continue
            if sidecars:
                continue
            store_id = store["id"]
            try:
                if expected_kind == "program_sqlite":
                    with sqlite_connection(path) as connection:
                        classify_program_store(
                            connection,
                            store_id,
                            artifacts,
                            legacy_version_candidates,
                            legacy_bundle_ids,
                            known_bundle_ids,
                            known_artifact_ids,
                        )
                elif expected_kind == "program_postgres_dump":
                    scan_postgres_program_dump(
                        path,
                        store_id,
                        artifacts,
                        legacy_version_candidates,
                        legacy_bundle_ids,
                        known_bundle_ids,
                        known_artifact_ids,
                    )
                elif expected_kind == "program_transitions_sqlite":
                    with sqlite_connection(path) as connection:
                        blockers.extend(
                            classify_transition_store(
                                connection,
                                store_id,
                                artifacts,
                                legacy_version_candidates,
                                legacy_bundle_ids,
                                known_bundle_ids,
                            )
                        )
                elif expected_kind == "harness_sqlite":
                    with sqlite_connection(path) as connection:
                        blockers.extend(
                            scan_harness_sqlite(connection, store_id, artifacts, known_artifact_ids)
                        )
                else:
                    blockers.extend(scan_harness_file(path, store_id, artifacts, known_artifact_ids))
            except AuditError as error:
                blockers.append(Blocker("STORE_SCAN_ERROR", str(error), store_id=store_id))

    scanned_stores: list[dict[str, Any]] = []
    for store, path, before, before_sidecars in resolved_stores:
        store_id = store["id"]
        store_kind = store["kind"]
        after = (
            sha256_directory(path)
            if store_kind == "harness_file"
            else sha256_regular_file(path, f"inventory store {store_id!r} snapshot")
        )
        after_sidecars = sqlite_sidecar_suffixes(path) if store_kind in SQLITE_STORE_KINDS else ()
        if before != after:
            blockers.append(
                Blocker(
                    "SNAPSHOT_MUTATED_DURING_SCAN",
                    "A storage snapshot changed while it was being audited",
                    store_id=store_id,
                )
            )
        if after_sidecars != before_sidecars:
            blockers.append(
                Blocker(
                    "SQLITE_SNAPSHOT_SIDECARS_CHANGED_DURING_SCAN",
                    "SQLite snapshot sidecar set changed while it was being audited",
                    store_id=store_id,
                )
            )
        scanned_stores.append(
            {"id": store_id, "kind": store_kind, "path": store["path"], "content_identity": before}
        )

    if not inventory["inventory_complete"]:
        blockers.append(
            Blocker(
                "INCOMPLETE_INVENTORY",
                "Final legacy removal requires an operator-attested complete storage inventory",
            )
        )

    blockers.extend(attach_classifications(artifacts, inventory["legacy_artifacts"], known_artifact_ids))
    sorted_blockers = sorted(
        (blocker.as_json() for blocker in blockers),
        key=lambda value: (
            value["code"],
            value.get("store_id", ""),
            value.get("artifact_id", ""),
            value.get("run_id", ""),
            value["message"],
        ),
    )
    no_deployment_attestation = inventory.get("no_deployment_attestation")
    body: dict[str, Any] = {
        "format": PROOF_FORMAT,
        "schema_version": SCHEMA_VERSION,
        "cutover_id": inventory["cutover_id"],
        "captured_at": inventory["captured_at"],
        "inventory_identity": sha256_bytes(canonical_json(inventory)),
        "evidence_mode": "no_deployment_attestation"
        if no_deployment_attestation is not None
        else "storage_snapshot",
        "scanned_stores": sorted(scanned_stores, key=lambda value: value["id"]),
        "legacy_artifacts": sorted(
            (artifact.as_json() for artifact in artifacts.values()), key=lambda value: value["artifact_id"]
        ),
        "counts": count_artifacts(artifacts.values()),
        "final_drain": {"passed": not sorted_blockers, "blockers": sorted_blockers},
    }
    if no_deployment_attestation is not None:
        body["no_deployment_attestation"] = no_deployment_attestation
    body["proof_identity"] = sha256_bytes(canonical_json(body))
    return body


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", required=True, type=Path, help="strict inventory JSON")
    parser.add_argument("--root", required=True, type=Path, help="frozen snapshot root")
    parser.add_argument("--output", required=True, type=Path, help="proof JSON to write")
    parser.add_argument(
        "--require-final",
        action="store_true",
        help="exit 1 when a final legacy-removal gate blocker remains",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    arguments = parse_arguments(argv)
    try:
        root = arguments.root.resolve()
        if not root.is_dir() or root.is_symlink():
            raise AuditError("--root must be a non-symlink directory")
        inventory_path = arguments.inventory.resolve()
        if not inventory_path.is_file():
            raise AuditError("--inventory must be a readable file")
        inventory = parse_inventory(inventory_path)
        store_paths = [
            resolve_snapshot_path(root, store["path"], f"inventory store {store['id']!r} path")
            for store in inventory["stores"]
        ]
        validate_output_path(arguments.output, store_paths)
        proof = audit(inventory, root)
        write_json_atomically(arguments.output, proof)
    except AuditError as error:
        print(f"legacy drain audit configuration error: {error}", file=sys.stderr)
        return 2

    print(proof["proof_identity"])
    if arguments.require_final and not proof["final_drain"]["passed"]:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
