<!-- neograph-i18n: source=benchmarks/stress/README.md locale=ko source_sha256=dc3aa60d06ab4b26c3b084af5651245fcbc52e56a724212845d2e0351f68a69a -->
# NeoGraph 지속 동시 실행 스트레스 벤치마크

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

이 러너는 노드 세 개짜리 카운터 그래프를 정해진 실제 시간 동안 반복 실행합니다. 측정 대상은 로컬 엔진의 반복 실행(churn)이며, provider 호출, 영속성, 프로덕션 준비 상태는 측정하지 않습니다.

## 측정 방식과 종료 상태

`bench_sustained_concurrent`의 기본값은 `--concurrency 1000`, `--duration-s 60`, `--sample-s 5`, `--warmup-s 5`, `--rss-tolerance-pct 25`입니다. 목표 실행 수만큼 스레드를 가진 호출자 풀을 만들고, 실행이 끝나면 다음 실행을 큐에 넣습니다. 샘플은 평균 지연과 최대 지연을 보고하며 P99는 보고하지 않습니다. 시간 측정은 호출자 워커 안에서 시작하므로 큐 대기 시간은 포함되지 않습니다. `ok_total`은 예외 없이 반환된 호출의 수일 뿐, 반환된 그래프 상태를 검증한 수가 아닙니다.

종료 코드 1은 최종 현재 RSS가 기록된 워밍업 기준값보다 허용치 이상 커졌다는 뜻입니다. 종료 코드 0이라고 해서 누수가 없다거나 실행 오류가 없다는 것이 증명되지는 않으므로 `err_total`을 따로 확인해야 합니다. 최종 RSS는 풀을 멈추고 join한 뒤에 읽기 때문에 스레드 해제가 드리프트에 영향을 줍니다. 워밍업 기준값은 첫 샘플이 `warmup-s`에 이미 도달했을 때만 첫 샘플에서 기록되므로, 워밍업 시간은 첫 샘플 간격보다 길게 두지 않아야 합니다. 기준값이 없으면 드리프트가 0으로 나오며, 이 값으로는 메모리 검사를 통과했다고 볼 수 없습니다.

Windows에서는 프로세스의 working-set 카운터를, Linux에서는 `/proc/self/status`를 사용합니다. 그 밖의 플랫폼은 이런 카운터가 없어서 0을 보고할 수 있습니다. 최대 RSS는 최고점 기록이라 줄어들지 않습니다. 메모리 증가를 조사할 때는 현재 RSS와 기준값이 유효한지를 확인해야 합니다.

## 빌드와 실행

외부 SchemaProvider SDK를 설치하고 prefix를 지정합니다. LLM과 NeoGraph의 선택적 libcurl 백엔드를 꺼도 Core에는 `SchemaProvider::runtime`이 필요합니다. prefix 대신 `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR`로 SDK 소스 체크아웃을 직접 지정할 수도 있습니다. 이 경우 SDK 빌드에는 C++20, Python, libcurl 7.88 이상, OpenSSL Crypto가 필요합니다. 의존성과 플랫폼 제약은 [빌드 가이드](../../README.md)를 참고하세요. 아래 명령은 네트워크 다운로드와 쓰지 않는 NeoGraph 통합 기능을 끕니다.
NeoGraph `0.13.1`에는 알파 SDK `0.1.1`, 인터페이스 리비전/공유 라이브러리 세대 4를 쓰고, 맞는 헤더와 라이브러리로 다시 빌드해야 합니다.

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

날짜가 기록되지 않은 이전 Ryzen 7 5800X 실행은 동시성 100, 실행 시간 15초 조건에서 15.3 M회 실행(초당 약 1.0 M회), 평균 지연 약 55 µs, 워밍업 후 RSS 9.3 MB, 최종 RSS 7.4 MB(약 −20%, 종료 코드 0)였습니다. 날짜와 SDK 리비전은 기록되어 있지 않습니다. 이 값은 과거 관측으로만 남겨 두며, 처리량 보장이 아닙니다. 아래 출력 발췌는 이 관측에 속하며, 줄임표(…)는 JSON이 아닙니다.

```json
{"sample":1,"elapsed_s":5,"window_ok":5012514,"err_total":0,"inflight":100,
 "mean_us":55.95,"max_us_window":189607,"rss_kb":9344,"peak_rss_kb":9472}
…
{"summary":true,"concurrency":100,"duration_s":15,"ok_total":15334628,
 "err_total":0,"rss_warm_kb":9344,"rss_final_kb":7448,"rss_peak_kb":9600,
 "rss_drift_pct":-20.29,"rss_tolerance_pct":25,"leak_suspect":false}
```

## 할당 압박 실험

`prlimit`은 Linux의 가상 주소 공간을 제한합니다. 호출자 슬롯마다 스레드를 하나씩 두기 때문에, 스택과 풀 생성만으로 그래프를 실행하기도 전에 이 한도가 바닥날 수 있습니다. 실행별 catch는 `engine->run`에서 발생한 예외를 기록하지만 풀 생성은 이 catch 바깥입니다. 할당 압박 상황에서 프로세스가 깔끔하게 종료되는지는 측정해야 할 합격 기준이지, 이 스크립트가 보장하는 동작이 아닙니다.

```bash
# Linux: cap virtual address space, not resident memory.
prlimit --as=$((256*1024*1024)) \
  ./build-stress/bench_sustained_concurrent \
  --concurrency 200 --duration-s 30
```

## 추가 실험

24시간 실행이나 cgroup 메모리 제한을 쓰는 실험은 환경과 결과를 따로 기록해야 합니다. 정상 상태 구간별로 현재 RSS를 비교하고, `err_total`과 종료 시그널을 기록합니다. cgroup이 상주 메모리를 제한하는 것과 `prlimit`이 주소 공간을 제한하는 것도 구분해야 합니다. 이 문서는 그런 실행의 결과를 보고하지 않습니다.
