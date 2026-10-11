# Golden identity corpus

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`manifest.json` lists Program sources and the registry they compile against.
`golden.json` pins what each one compiles to: the sealed Core definition, its
`definition_hash`, the compiled-plan identity, the sealed diagnostics and the
bundle id. `tests/test_identity_golden.cpp` and
`bindings/python/tests/test_identity_golden.py` check every pin.

A program that does not use a new Core key must keep all of its pins. A changed
pin for such a program is a compatibility break.

## Platform rule

A JavaScript source envelope seals `ProgramSource::JAVASCRIPT_QUICKJS_BUILD_OPTIONS`,
which differs on the MSVC port. These pins are therefore kept under
`posix_quickjs` and checked only for the POSIX QuickJS build identity:

- the JavaScript `source_hash` and `bundle_id`;
- `canonical_program_hash` when the program has `main()` (it seals the source as
  its control source).

Everything under `all_platforms` is checked on every platform, including every
pin of a C++ builder program.

## Regenerating

Only after an intended identity change, and never in CI:

```sh
NEOGRAPH_REGENERATE_IDENTITY_CORPUS=1 \
    <build>/tests/neograph_identity_golden_tests --gtest_filter='IdentityGolden.*'
```

It needs a POSIX build with `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON`. It refuses to
run when `CI` or `GITHUB_ACTIONS` is set. The output is deterministic, so
`IdentityGolden.GoldenFileIsTheDeterministicRegeneration` fails if the file was
edited by hand. Review the diff before committing it.
