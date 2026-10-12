<!-- neograph-i18n: source=tests/fixtures/identity_corpus/README.md locale=ko source_sha256=83accd0c1a18f7f267e987bd5317425e2b0b24cc05a9249464d08243fe4f72e9 -->
# 골든 identity 코퍼스

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`manifest.json`은 Program 소스 목록과, 이 소스들을 컴파일할 때 쓰는 registry를 담고 있습니다.
`golden.json`은 각 소스가 컴파일되어 나오는 결과를 고정(pin)해 둔 파일입니다. 고정하는 항목은
봉인된 Core 정의, 그 `definition_hash`, 컴파일된 plan의 identity, 봉인된 진단 결과, bundle id입니다.
모든 pin은 `tests/test_identity_golden.cpp`와 `bindings/python/tests/test_identity_golden.py`가
검사합니다.

새 Core 키를 쓰지 않는 program은 pin이 하나도 바뀌지 않아야 합니다. 이런 program의 pin이
바뀌면 호환성이 깨진 것입니다.

## 플랫폼 규칙

JavaScript 소스 envelope에는 `ProgramSource::JAVASCRIPT_QUICKJS_BUILD_OPTIONS`가 봉인되는데, 이
값은 MSVC 포트에서 다릅니다. 그래서 아래 pin은 `posix_quickjs` 아래에 두고, POSIX QuickJS 빌드
identity에서만 검사합니다.

- JavaScript의 `source_hash`와 `bundle_id`
- `main()`이 있는 program의 `canonical_program_hash`(이 값은 소스를 control source로 봉인합니다)

`all_platforms` 아래 항목은 모든 플랫폼에서 검사합니다. C++ builder program의 pin도 모두 여기에
포함됩니다.

## 재생성

의도한 identity 변경이 있은 뒤에만 실행하며, CI에서는 실행하지 않습니다.

```sh
NEOGRAPH_REGENERATE_IDENTITY_CORPUS=1 \
    <build>/tests/neograph_identity_golden_tests --gtest_filter='IdentityGolden.*'
```

`NEOGRAPH_BUILD_QUICKJS_CONTROL=ON`으로 구성한 POSIX 빌드가 필요합니다. `CI`나 `GITHUB_ACTIONS`가
설정되어 있으면 실행을 거부합니다. 출력은 결정적이라서, 파일을 손으로 고치면
`IdentityGolden.GoldenFileIsTheDeterministicRegeneration`이 실패합니다. 커밋하기 전에 diff를
검토합니다.
