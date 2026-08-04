#!/usr/bin/env python3
"""Check the fail-closed cross-repository compatibility declaration.

This is intentionally a metadata/source-contract check.  It reads the two
explicitly named JSON files and verifies their references plus current markers
in the exposed NeoGraph headers; it does not clone, execute, or inspect
NeoCode/NeoProtocol, and it never grants compatibility based on a repository
name alone.
"""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
from typing import Any


class CompatibilityCheckError(ValueError):
    """A compatibility metadata invariant failed."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise CompatibilityCheckError(message)


def _nonempty(value: Any, field: str) -> None:
    _require(isinstance(value, str) and bool(value), f"{field} must be non-empty")


def _unique(values: list[Any], field: str) -> None:
    _require(len(values) == len(set(values)), f"{field} contains duplicate IDs")


def _check_source(root: Path, relative_path: str, field: str) -> None:
    if not isinstance(relative_path, str):
        raise CompatibilityCheckError(f"{field} must be a path")
    path = (root / relative_path).resolve()
    _require(path.is_relative_to(root.resolve()), f"{field} escapes repository root")
    _require(path.is_file(), f"{field} does not exist: {relative_path}")


def _check_source_marker(root: Path, relative_path: str, marker: str, field: str) -> None:
    """Require a current-contract marker in an exposed source surface.

    Compatibility metadata is deliberately checked against the public source
    that it names.  A path existing is not enough: a copied historical header
    or schema must not be able to satisfy a current conformance declaration.
    Whitespace is normalized so formatting-only edits do not change the gate.
    """

    _check_source(root, relative_path, field)
    path = (root / relative_path).resolve()
    try:
        content = " ".join(path.read_text(encoding="utf-8").split())
    except OSError as error:
        raise CompatibilityCheckError(f"{field} cannot be read: {relative_path}: {error}") from error
    normalized_marker = " ".join(marker.split())
    _require(
        normalized_marker in content,
        f"{field} is missing current contract marker {marker!r}",
    )


_CURRENT_SURFACE_SCHEMA_VERSIONS = {
    "program_version": [1],
    "run_invocation": [1],
    # CollaborationLink was extended with the explicit message-kind allowlist
    # in schema v2.  The metadata previously accepted a stale [1] declaration
    # because it only checked that versions were positive integers.
    "a2a_collaboration": [2],
}


_CURRENT_SURFACE_MARKERS = {
    "a2a_collaboration": (
        (
            "include/neograph/a2a/collaboration.h",
            "std::uint32_t schema_version = 2",
        ),
        (
            "include/neograph/a2a/collaboration.h",
            "static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 2",
        ),
        (
            "include/neograph/a2a/collaboration.h",
            "std::vector<std::string> message_kind_allowlist",
        ),
    ),
}


def _validate_current_surface_contract(surface: dict[str, Any], root: Path, field: str) -> None:
    surface_id = surface.get("id")
    expected_versions = _CURRENT_SURFACE_SCHEMA_VERSIONS.get(surface_id)
    if expected_versions is not None:
        _require(
            surface.get("schema_versions") == expected_versions,
            f"{field}.schema_versions does not match the current exposed contract",
        )
    for marker_path, marker in _CURRENT_SURFACE_MARKERS.get(surface_id, ()):
        declared_sources = surface.get("source_paths")
        _require(
            isinstance(declared_sources, list) and marker_path in declared_sources,
            f"{field}.source_paths omits the marker-backed public header {marker_path}",
        )
        _check_source_marker(root, marker_path, marker, f"{field}.{marker_path}")


def _validate(metadata: dict[str, Any], root: Path) -> None:
    expected_format = "neograph-cross-repository-compatibility-v1"
    _require(metadata.get("format") == expected_format, "unexpected metadata format")
    _require(metadata.get("schema_version") == 1, "unsupported metadata schema version")
    _require(metadata.get("issue") == 7, "metadata must remain bound to issue #7")
    _require(
        metadata.get("status") in {"rebase_required", "current"},
        "status must be rebase_required or current",
    )
    _nonempty(metadata.get("contract_revision"), "contract_revision")
    _nonempty(metadata.get("authority"), "authority")

    policy = metadata.get("policy")
    _require(isinstance(policy, dict), "policy must be an object")
    required_fields = policy.get("required_current_metadata")
    _require(
        policy.get("historical_reference_status") == "historical_only",
        "historical references must be historical_only",
    )
    _require(policy.get("historical_conformance") is False, "historical conformance must be false")
    _require(policy.get("current_consumer_status") == "rebased_current", "current status must be rebased_current")
    _require(policy.get("reject_without_explicit_rebase") is True, "rebase rejection must be enabled")
    _require(isinstance(required_fields, list) and required_fields, "required current metadata is empty")
    _unique(required_fields, "required_current_metadata")

    surfaces = metadata.get("current_surfaces")
    _require(isinstance(surfaces, list) and surfaces, "current_surfaces must be non-empty")
    surface_ids = [surface.get("id") for surface in surfaces if isinstance(surface, dict)]
    _unique(surface_ids, "current_surfaces")
    _require(
        {"program_version", "run_invocation", "a2a_collaboration"}.issubset(surface_ids),
        "ProgramVersion, RunInvocation, and A2A surfaces are required",
    )
    for index, surface in enumerate(surfaces):
        prefix = f"current_surfaces[{index}]"
        _require(isinstance(surface, dict), f"{prefix} must be an object")
        for field in ("id", "api", "owner", "contract_revision", "implementation_state"):
            _nonempty(surface.get(field), f"{prefix}.{field}")
        _require(surface.get("current_conformance") is True, f"{prefix} must be current NeoGraph conformance")
        schema_versions = surface.get("schema_versions")
        _require(
            isinstance(schema_versions, list) and schema_versions and all(isinstance(version, int) and version >= 1 for version in schema_versions),
            f"{prefix}.schema_versions must contain positive integers",
        )
        _validate_current_surface_contract(surface, root, prefix)
        source_paths = surface.get("source_paths")
        _require(isinstance(source_paths, list) and source_paths, f"{prefix}.source_paths is empty")
        _unique(source_paths, f"{prefix}.source_paths")
        for path_index, source_path in enumerate(source_paths):
            _check_source(root, source_path, f"{prefix}.source_paths[{path_index}]")
        comparisons = surface.get("comparison_to_historical")
        _require(isinstance(comparisons, list) and comparisons, f"{prefix}.comparison_to_historical is empty")
        for comparison_index, comparison in enumerate(comparisons):
            comparison_prefix = f"{prefix}.comparison_to_historical[{comparison_index}]"
            _require(isinstance(comparison, dict), f"{comparison_prefix} must be an object")
            _nonempty(comparison.get("reference_id"), f"{comparison_prefix}.reference_id")
            _require(
                comparison.get("classification") in {"not_equivalent", "requires_adapter", "target_contract"},
                f"{comparison_prefix}.classification is invalid",
            )
            _nonempty(comparison.get("reason"), f"{comparison_prefix}.reason")

    references = metadata.get("historical_references")
    _require(isinstance(references, list) and references, "historical_references must be non-empty")
    reference_ids = [reference.get("id") for reference in references if isinstance(reference, dict)]
    _unique(reference_ids, "historical_references")
    _require(
        {"neocode_harness_sidecar", "neoprotocol_federated_acp"}.issubset(reference_ids),
        "NeoCode and NeoProtocol historical references are required",
    )
    for index, reference in enumerate(references):
        prefix = f"historical_references[{index}]"
        _require(isinstance(reference, dict), f"{prefix} must be an object")
        for field in ("id", "repository", "reference_kind", "legacy_surface"):
            _nonempty(reference.get(field), f"{prefix}.{field}")
        _require(reference.get("status") == "historical_only", f"{prefix}.status is not historical_only")
        _require(reference.get("current_conformance") is False, f"{prefix} cannot claim current conformance")
        _require(
            reference.get("admission_disposition") == "blocked_until_rebased",
            f"{prefix} must be blocked until rebased",
        )
        source = reference.get("source")
        _require(isinstance(source, dict), f"{prefix}.source must be an object")
        _check_source(root, source.get("document"), f"{prefix}.source.document")
        _nonempty(source.get("anchor"), f"{prefix}.source.anchor")
        mapped_surfaces = reference.get("current_surface_ids")
        _require(isinstance(mapped_surfaces, list) and mapped_surfaces, f"{prefix}.current_surface_ids is empty")
        _unique(mapped_surfaces, f"{prefix}.current_surface_ids")
        _require(set(mapped_surfaces).issubset(surface_ids), f"{prefix} maps to an unknown current surface")
        rebase_evidence = reference.get("required_rebase_evidence")
        _require(
            isinstance(rebase_evidence, list) and rebase_evidence,
            f"{prefix}.required_rebase_evidence is empty",
        )
        _require(
            set(required_fields).issubset(rebase_evidence),
            f"{prefix} omits required explicit rebase metadata",
        )
        evidence = reference.get("evidence")
        _require(isinstance(evidence, list) and evidence, f"{prefix}.evidence is empty")

    for surface in surfaces:
        for comparison in surface["comparison_to_historical"]:
            _require(
                comparison["reference_id"] in reference_ids,
                f"surface comparison references unknown historical ID {comparison['reference_id']}",
            )

    consumers = metadata.get("consumers")
    _require(isinstance(consumers, list) and consumers, "consumers must be non-empty")
    consumer_ids = [consumer.get("id") for consumer in consumers if isinstance(consumer, dict)]
    _unique(consumer_ids, "consumers")
    _require(set(consumer_ids) == {"neocode", "neoprotocol"}, "consumer declarations must cover both historical repositories")
    for index, consumer in enumerate(consumers):
        prefix = f"consumers[{index}]"
        _require(isinstance(consumer, dict), f"{prefix} must be an object")
        _nonempty(consumer.get("id"), f"{prefix}.id")
        _require(consumer.get("reference_id") in reference_ids, f"{prefix}.reference_id is unknown")
        status = consumer.get("status")
        _require(status in {"historical_reference", "rebased_current"}, f"{prefix}.status is invalid")
        if status == "historical_reference":
            _require(consumer.get("current_conformance") is False, f"{prefix} cannot be current")
            for field in (
                "declared_contract_revision",
                "program_schema_version",
                "run_invocation_contract_revision",
                "a2a_contract_revision",
                "rebase_commit",
            ):
                _require(consumer.get(field) is None, f"{prefix}.{field} must be null until rebase")
            _require(consumer.get("conformance_evidence") == [], f"{prefix}.conformance_evidence must be empty")
        else:
            _require(consumer.get("current_conformance") is True, f"{prefix} must explicitly claim current conformance")
            for field in required_fields:
                value = consumer.get(field)
                _require(value not in (None, "", []), f"{prefix}.{field} is required for current conformance")
            _require(
                consumer.get("declared_contract_revision") == metadata["contract_revision"],
                f"{prefix}.declared_contract_revision does not match the gate revision",
            )
            _require(consumer.get("program_schema_version") == 1, f"{prefix}.program_schema_version must be 1")
            _require(
                consumer.get("run_invocation_contract_revision") == "neograph-run-invocation-v1",
                f"{prefix}.run_invocation_contract_revision is not current",
            )
            _require(
                consumer.get("a2a_contract_revision") == "neograph-a2a-collaboration-v1",
                f"{prefix}.a2a_contract_revision is not current",
            )
            evidence = consumer.get("conformance_evidence")
            _require(isinstance(evidence, list) and evidence, f"{prefix}.conformance_evidence is empty")
            _require(all(item.get("status") == "verified" for item in evidence), f"{prefix} has unverified evidence")

    if metadata["status"] == "current":
        _require(
            all(consumer["status"] == "rebased_current" for consumer in consumers),
            "metadata cannot be current while a consumer remains historical",
        )


def _parse_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CompatibilityCheckError(f"cannot parse {path}: {error}") from error
    _require(isinstance(value, dict), f"{path} must contain a JSON object")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    repository_root = Path(__file__).resolve().parents[1]
    parser.add_argument(
        "--metadata",
        type=Path,
        default=repository_root / "spec/cross-repository-compatibility-v1.json",
    )
    parser.add_argument(
        "--schema",
        type=Path,
        default=repository_root / "spec/cross-repository-compatibility-v1.schema.json",
    )
    args = parser.parse_args()
    root = repository_root
    metadata_path = args.metadata.resolve()
    schema_path = args.schema.resolve()
    metadata = _parse_json(metadata_path)
    schema = _parse_json(schema_path)

    _require(
        schema.get("properties", {}).get("format", {}).get("const") == metadata.get("format"),
        "metadata format does not match its JSON schema",
    )
    _validate(metadata, root)

    # A focused negative proof: merely changing a historical consumer's label
    # to "rebased_current" cannot pass.  Current conformance requires every
    # explicit revision and verified evidence field above.
    candidate = copy.deepcopy(metadata)
    candidate["consumers"][0]["status"] = "rebased_current"
    candidate["consumers"][0]["current_conformance"] = True
    try:
        _validate(candidate, root)
    except CompatibilityCheckError:
        pass
    else:
        raise CompatibilityCheckError("historical consumer was silently accepted as current")

    # A focused drift proof: a historical v1 A2A schema declaration must not
    # pass merely because the source paths still exist.  The current public
    # CollaborationLink contract is v2 and is checked against its header above.
    candidate = copy.deepcopy(metadata)
    a2a_surface = next(
        surface for surface in candidate["current_surfaces"] if surface["id"] == "a2a_collaboration"
    )
    a2a_surface["schema_versions"] = [1]
    try:
        _validate(candidate, root)
    except CompatibilityCheckError:
        pass
    else:
        raise CompatibilityCheckError("stale A2A schema declaration was silently accepted")

    print(
        "cross-repository compatibility metadata: PASS "
        f"({len(metadata['current_surfaces'])} current surfaces, "
        f"{len(metadata['historical_references'])} historical references, "
        f"{len(metadata['consumers'])} guarded consumers; "
        "negative current-claim and stale-schema checks rejected)"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CompatibilityCheckError as error:
        print(f"cross-repository compatibility metadata: FAIL: {error}")
        raise SystemExit(1)
