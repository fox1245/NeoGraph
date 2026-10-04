<!-- neograph-i18n: source=benchmarks/stress/README.md locale=ko source_sha256=245bd9f1555c8f45feba5119b20683c54700e0b74468f8857b4707267680b859 -->
# NeoGraph 지속 동시 실행 스트레스 벤치마크

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

세 노드 카운터 그래프를 일정 시간 반복합니다. 로컬 엔진의 반복 실행을 측정하며 공급자 호출, 영속성, 운영 준비 상태를 측정하지 않습니다.

## 측정과 종료 상태

`bench_sustained_concurrent` 기본값은 `--concurrency 1000`, `--duration-s 60`, `--sample-s 5`, `--warmup-s 5`, `--rss-tolerance-pct 25`입니다. 목표 실행 수만큼 호출자 스레드를 만들고 완료 시 다음 실행을 제출합니다. 평균과 최대 지연을 출력하며 P99는 없습니다. 작업자 내부에서 측정을 시작하므로 큐 대기를 제외합니다. `ok_total`은 예외 없이 반환된 호출 수이며 반환 상태의 검증 수가 아닙니다.

종료 1은 최종 현재 RSS가 예열 기준보다 허용치 이상 증가했다는 뜻입니다. 종료 0은 누수 없음이나 실행 오류 없음을 입증하지 않습니다. `err_total`도 확인하세요. 최종 RSS는 풀 중지 및 join 후 측정하므로 스레드 종료가 영향을 줍니다. 예열 기준은 첫 샘플이 `warmup-s`에 도달했을 때만 잡습니다. 예열을 첫 샘플 간격보다 길게 두지 마세요. 기준 누락 시 드리프트 0은 메모리 검사 통과 근거가 아닙니다.

Windows는 프로세스 working-set 카운터, Linux는 `/proc/self/status`를 사용합니다. 다른 플랫폼의 0은 측정 불가일 수 있습니다. 최대 RSS는 감소하지 않으므로 증가 조사에는 현재 RSS와 기준 유효성을 확인하세요.

## 빌드와 실행

외부 SchemaProvider SDK를 설치하고 prefix를 설정하세요. LLM과 NeoGraph의 선택적 libcurl 백엔드를 꺼도 Core에는 `SchemaProvider::runtime`이 필요합니다. prefix 대신 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`로 명시적 소스를 지정할 수 있으며 소스 빌드에는 C++20, Python, libcurl ≥7.88, OpenSSL Crypto가 필요합니다. 의존성과 플랫폼 제약은 [빌드 안내](../../README.md)를 참고하세요. 아래 명령은 네트워크 다운로드와 미사용 NeoGraph 통합을 끕니다.
NeoGraph `0.13.0`에는 alpha SDK `0.1.0`, interface revision/shared generation 4를 사용하고 일치하는 header/library로 재빌드하세요. 현재 통합 검증은 대기 중입니다.

```bash
# Set SCHEMAPROVIDER_PREFIX to the installed SDK prefix.
cmake -B build-stress -S . \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
  -DNEOGRAPH_FETCH_SCHEMAPROVIDER=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON \
  -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_EXAMPLES=OFF \
  -DNEOGRAPH_BUILD_PROGRAM=OFF -DNEOGRAPH_BUILD_LLM=OFF \
  -DNEOGRAPH_BUILD_ASYNC=OFF -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF \
  -DNEOGRAPH_BUILD_UTIL=OFF -DNEOGRAPH_BUILD_POSTGRES=OFF \
  -DNEOGRAPH_BUILD_SQLITE=OFF -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-stress --parallel --target bench_sustained_concurrent

./build-stress/bench_sustained_concurrent \
  --concurrency 1000 --duration-s 60 --sample-s 5 \
  --warmup-s 5 --rss-tolerance-pct 25
```

## 보존한 과거 관측

이전 README의 날짜 없는 Ryzen 7 5800X 기록은 동시성 100, 15초 동안 15.3 M 실행(약 1.0 M runs/s), 평균 약 55 µs, 예열 RSS 9.3 MB에서 최종 7.4 MB(약 −20%, 종료 0)였습니다. 날짜와 SDK 리비전은 기록되지 않았습니다. 새 cutover 검증이나 처리량 보장이 아닌 과거 증거입니다. 아래 출력은 해당 관측의 발췌이며 생략 기호는 JSON이 아닙니다.

```json
{"sample":1,"elapsed_s":5,"window_ok":5012514,"err_total":0,"inflight":100,
 "mean_us":55.95,"max_us_window":189607,"rss_kb":9344,"peak_rss_kb":9472}
…
{"summary":true,"concurrency":100,"duration_s":15,"ok_total":15334628,
 "err_total":0,"rss_warm_kb":9344,"rss_final_kb":7448,"rss_peak_kb":9600,
 "rss_drift_pct":-20.29,"rss_tolerance_pct":25,"leak_suspect":false}
```

## 할당 압박 실험

`prlimit`은 Linux 가상 주소 공간을 제한합니다. 호출 슬롯마다 스레드가 있으므로 스택과 풀 생성이 그래프 실행 전에 한도를 소진할 수 있습니다. 실행별 catch는 `engine->run` 예외를 기록하지만 풀 생성은 catch 밖입니다. 할당 압박에서 정상 종료는 측정할 합격 기준이지 스크립트의 보장이 아닙니다.

```bash
# Linux: cap virtual address space, not resident memory.
prlimit --as=$((256*1024*1024)) \
  ./build-stress/bench_sustained_concurrent \
  --concurrency 200 --duration-s 30
```

## 추가 실험

24시간 실행이나 cgroup 제한은 환경과 결과를 따로 기록해야 합니다. 정상 상태 구간의 현재 RSS를 비교하고 `err_total`과 종료 시그널을 기록하세요. cgroup의 상주 메모리 제한과 `prlimit` 주소 공간 제한을 구분하세요. 이 문서는 해당 실행 결과를 보고하지 않습니다.
