<!-- neograph-i18n: source=benchmarks/stress/README.md locale=ko source_sha256=87cc091ee71ab14f86ff9642a147a3d80e1452c56020c0073aba63e125258190 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# NeoGraph 스트레스 하니스


엔진 오버헤드를 보완하는 운영 준비 게이트
단일 샷 동시 벤치마크. `benchmarks/bench_neograph` 어디에
**호출당 비용**을 측정하고 `benchmarks/concurrent/...`는
**단일 10k 버스트**, 이 디렉토리는 **시간 경과에 따라** NeoGraph를 실행합니다.

## 여기에 무엇이 있습니까?

### `bench_sustained_concurrent`

M 벽시계 초 동안 N 그래프 실행을 유지합니다. 새로운 제출
완료되자마자 실행되므로 기내에서 대상에 머물게 됩니다. 샘플
RSS 및 `--sample-s`초마다 창당 대기 시간; RSS인 경우 1을 종료합니다.
따뜻한 온도 사이에서 `--rss-tolerance-pct` 이상으로 상승합니다.
기준선(예열 후) 및 최종 샘플.

버스트 벤치가 포착할 수 없는 세 가지 실패 모드를 포착합니다.

- **정상 상태 누출** — 코루틴/대기 중인 쓰기/캐시됨
무한히 성장하는 상태. 드리프트 게이트는 최선의 노력입니다
(Valgrind/LSan은 권위 있는 도구로 유지됩니다. ASan/TSan CI 참조)
그러나 60초 동안 25% RSS 상승은 강력한 "이것 좀 보세요" 신호입니다.
- **지연 시간 드리프트** — 창당 평균/최대값은
수영장이 따뜻해집니다. 종종 스레드 풀 부족 또는 스케줄러를 지적합니다.
t=0 파열 테스트에서는 나타나지 않는 배압.
- **이탈로 인한 풀 소진** — 완료가 새로운 완료와 겹칩니다.
제출하므로 작업자 풀은 혼합된 기내 패턴을 보게 됩니다.
버스트의 모든 배수가 아닙니다.

#### 용법

```bash
cmake -B build-stress -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DNEOGRAPH_BUILD_BENCHMARKS=ON \
    -DNEOGRAPH_BUILD_TESTS=OFF \
    -DNEOGRAPH_BUILD_EXAMPLES=OFF
cmake --build build-stress -j$(nproc) --target bench_sustained_concurrent

./build-stress/bench_sustained_concurrent \
    --concurrency        1000 \
    --duration-s         60   \
    --sample-s           5    \
    --warmup-s           5    \
    --rss-tolerance-pct  25
```

연기 결과(동시성=100, 기간=15초, Ryzen 7 5800X에서):
- 15.3M 그래프 실행/15초 ≒ **1.0M runs/s** 지속
- 실행당 평균 대기 시간: ~55 µs
- RSS 웜: 9.3MB → 최종: 7.4MB(드리프트 ‑20%, 종료 0)

#### 출력 형태

샘플당 JSON 라인 1개, 최종 요약 라인 1개:

```json
{"sample":1,"elapsed_s":5,"window_ok":5012514,"err_total":0,"inflight":100,
 "mean_us":55.95,"max_us_window":189607,"rss_kb":9344,"peak_rss_kb":9472}
…
{"summary":true,"concurrency":100,"duration_s":15,"ok_total":15334628,
 "err_total":0,"rss_warm_kb":9344,"rss_final_kb":7448,"rss_peak_kb":9600,
 "rss_drift_pct":-20.29,"rss_tolerance_pct":25,"leak_suspect":false}
```

### `prlimit` 아래의 `bench_sustained_concurrent`(메모리 캡 테스트)

NeoGraph 핸들을 증명하기 위해 하네스를 가상 메모리 캡으로 감싸십시오.
깔끔하게 할당 압력:

```bash
# Cap address space at 256 MB. Allocations beyond this fail with
# std::bad_alloc — NeoGraph's audit-Round-5 typed catch in
# graph_executor (commit ead703e) rethrows bad_alloc instead of
# silently retrying, so the workload should error out instead of
# crashing.
prlimit --as=$((256*1024*1024)) \
    ./build-stress/bench_sustained_concurrent \
        --concurrency 200 --duration-s 30
```

통과 기준: 프로세스가 완전히 종료됩니다(반환 코드 0 또는 1, SIGABRT 아님).
/ SIGSEGV), `err_total`는 0이 아닐 수 있습니다(이것은 bad_alloc입니다).
실패한 실행으로 표면을 다시 발생시킵니다.)

## 아직 여기에 없습니다

- **24시간 흡수** — 동일한 하네스, 더 긴 벽 창. 그것을 실행
전용 호스트; 단조롭고 감소하지 않는 경우 `peak_rss_kb`를 시청하세요.
시간 경과에 따른 추세.
- **cgroup 경계 실행** — `systemd-run --scope -p MemoryMax=512M`
`prlimit`(커널 측)보다 더 엄격한 리소스 제한을 위해
할당 시간 확인뿐만 아니라 시행). WSL2 시스템화
지원이 제한됩니다. 실제 Linux 호스트에서 테스트해 보세요.
