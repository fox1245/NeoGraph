<!-- neograph-i18n: source=tests/fixtures/identity_corpus/README.md locale=ko source_sha256=83accd0c1a18f7f267e987bd5317425e2b0b24cc05a9249464d08243fe4f72e9 -->
# 골든 identity 코퍼스

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`manifest.json`은 Program 소스와, 각 소스를 컴파일할 때 기준이 되는 registry를 나열합니다.
`golden.json`은 각 소스의 컴파일 결과를 pin(고정값)으로 기록합니다. 대상은 봉인된(sealed)
Core definition, 그 `definition_hash`, 컴파일된 plan의 identity, 봉인된 diagnostics, bundle id입니다.
`tests/test_identity_golden.cpp`와 `bindings/python/tests/test_identity_golden.py`가 모든 pin을 검사합니다.

새 Core 키를 사용하지 않는 program은 모든 pin을 그대로 유지해야 합니다. 그런 program의 pin이
바뀌었다면 호환성이 깨진 것입니다.

## 플랫폼 규칙

JavaScript 소스 envelope은 `ProgramSource::JAVASCRIPT_QUICKJS_BUILD_OPTIONS`를 봉인하며,
이 값은 MSVC 포트에서 다릅니다. 따라서 다음 pin은 `posix_quickjs` 아래에 두고
POSIX QuickJS 빌드 identity에서만 검사합니다.

- JavaScript `source_hash`와 `bundle_id`
- program에 `main()`이 있을 때의 `canonical_program_hash`(이 값은 소스를 control source로
  봉인합니다)

`all_platforms` 아래의 모든 항목은 C++ builder program의 모든 pin을 포함해 모든 플랫폼에서 검사합니다.

## 재생성

의도한 identity 변경 뒤에만 실행하며, CI에서는 절대 실행하지 않습니다.

```sh
NEOGRAPH_REGENERATE_IDENTITY_CORPUS=1 \
    <build>/tests/neograph_identity_golden_tests --gtest_filter='IdentityGolden.*'
```

`NEOGRAPH_BUILD_QUICKJS_CONTROL=ON`으로 구성한 POSIX 빌드가 필요합니다. `CI` 또는
`GITHUB_ACTIONS`가 설정되어 있으면 실행을 거부합니다. 출력이 결정적(deterministic)이므로
파일을 손으로 편집하면 `IdentityGolden.GoldenFileIsTheDeterministicRegeneration`이 실패합니다.
커밋하기 전에 diff를 검토하세요.
