"""Golden identity corpus through the Python binding (Program API v2, WP-G).

tests/test_identity_golden.cpp pins, for every program in
tests/fixtures/identity_corpus/manifest.json, the sealed definition, its
definition_hash, the compiled-plan identity and the bundle id. This test
compiles the same programs from Python, with the registry a Python host builds
for the same names, and checks the same pins. Both languages must agree, and
neither may drift while later work packages add Core keys.

Platform rule (see the C++ test): JavaScript source hashes and bundle ids, and
canonical_program_hash for programs with main(), seal the QuickJS build options
and are checked only for the POSIX QuickJS build identity. Everything else is
checked on every platform. golden.json is regenerated only by the C++ test.
"""

import json
import pathlib
import sys

import pytest

import neograph_engine as ng


_CORPUS = pathlib.Path(__file__).resolve().parents[3] / "tests" / "fixtures" / "identity_corpus"


class _Node(ng.GraphNode):
    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, _input):
        return []


def _load(name):
    with open(_CORPUS / name, encoding="utf-8") as handle:
        return json.load(handle)


@pytest.fixture(scope="module")
def corpus():
    if not (_CORPUS / "manifest.json").exists():
        pytest.skip("identity corpus fixtures are not available in this checkout")
    return _load("manifest.json"), _load("golden.json")


def _registry(manifest):
    registry = manifest["registry"]
    assert registry["attestation"] == "python-registered-executable"
    builder = ng.ProgramRegistryBuilder()
    for node in registry["nodes"]:
        ng.NodeFactory.register_type(
            node["name"], lambda name, _config, _ctx: _Node(name)
        )
        builder.add_registered_node(
            node["name"], node["semantic_version"], node["implementation_digest"]
        )
    for reducer in registry["reducers"]:
        if reducer["name"] not in ("append", "overwrite"):
            ng.ReducerRegistry.register_reducer(
                reducer["name"], lambda _current, incoming: incoming
            )
        builder.add_registered_reducer(
            reducer["name"], reducer["semantic_version"], reducer["implementation_digest"]
        )
    for condition in registry["conditions"]:
        ng.ConditionRegistry.register_condition(condition["name"], lambda _state: "unused")
        builder.add_registered_condition(
            condition["name"], condition["semantic_version"], condition["implementation_digest"]
        )
    return builder.build()


def _source(entry, pinned):
    path = _CORPUS / entry["source"]
    if entry["frontend"] == "javascript":
        return ng.ProgramSource.from_javascript(
            "identity-corpus/" + entry["id"] + ".js", path.read_text(encoding="utf-8")
        )
    # Python has no C++ builder frontend; a stored source envelope is the same
    # value, and parse() recomputes its source_hash and rejects a mismatch.
    document = json.loads(path.read_text(encoding="utf-8"))
    stored = {
        "format": "neograph-program-source",
        "storage_schema_version": 1,
        "kind": "cpp_builder",
        "source_id": "identity-corpus/" + entry["id"] + ".json",
        "program_schema_version": document["program_schema_version"],
        "document": document,
        "source_hash": pinned["all_platforms"]["source_hash"],
        "imports": [],
        "source_map": [],
    }
    return ng.ProgramSource.parse(json.dumps(stored, ensure_ascii=False))


def _identity(bundle):
    stored = json.loads(bundle.serialize_canonical())
    definitions = bundle.sealed_core_definitions
    assert len(definitions) == 1
    assert len(stored["core_plan_identities"]) == 1
    return {
        "definition": definitions[0]["definition"],
        "definition_name": definitions[0]["name"],
        "definition_hash": definitions[0]["definition_hash"],
        "compiled_plan_identity": stored["core_plan_identities"][0]["compiled_plan_identity"],
        "control_source": "control_source" in stored,
        "sealed_diagnostics": [
            {"code": item["code"], "json_pointer": item["json_pointer"]}
            for item in bundle.diagnostics
        ],
        "source_hash": bundle.source_hash,
        "bundle_id": bundle.id,
        "canonical_program_hash": stored["canonical_program_hash"],
    }


def _posix_quickjs(golden):
    probe = ng.ProgramSource.from_javascript("identity-corpus/probe.js", "")
    return probe.document["quickjs_build_options"] == golden["posix_quickjs_build_options"]


def test_python_builds_the_pinned_registry(corpus):
    manifest, golden = corpus
    assert _registry(manifest).fingerprint == golden["registry_fingerprint"]


def test_posix_builds_carry_the_pinned_quickjs_identity(corpus):
    _manifest, golden = corpus
    # The MSVC port seals different QuickJS build options; every other build must
    # match, so the POSIX pins are never skipped by accident.
    assert _posix_quickjs(golden) == (sys.platform != "win32")


def test_python_compiles_the_pinned_identities(corpus):
    manifest, golden = corpus
    compiler = ng.ProgramCompiler(_registry(manifest), manifest["compiler_build_id"])
    posix = _posix_quickjs(golden)
    pins = {entry["id"]: entry for entry in golden["entries"]}
    assert [entry["id"] for entry in manifest["entries"]] == [
        entry["id"] for entry in golden["entries"]
    ]
    for entry in manifest["entries"]:
        pinned = pins[entry["id"]]
        actual = _identity(compiler.compile(_source(entry, pinned)))
        assert actual["definition"] == pinned["definition"], entry["id"]
        for key, value in pinned["all_platforms"].items():
            assert actual[key] == value, (entry["id"], key)
        if posix:
            for key, value in pinned.get("posix_quickjs", {}).items():
                assert actual[key] == value, (entry["id"], key)
