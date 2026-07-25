<!-- neograph-i18n: source=benchmarks/dr_compare/README.md locale=ko source_sha256=54863904c664b90c0e13360fef870ad45a396a3150352de4af85f1084ffff9e6 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# dr_compare — NeoGraph 대 LangGraph 심층 연구 벤치


동일한 심층 연구 워크플로의 두 가지 구현(라우터 → 계획 →
보내기 → 합성)을 통해 엔진당 한 명씩 5명의 연구원을 팬아웃합니다. 같은
프롬프트, 동일한 모델, 동일한 Crawl4AI 검색, 동일한 Postgres 체크포인트
백엔드(또는 인메모리). 차이점은 엔진 + 해당 엔진에 격리되어 있습니다.
HTTP 전송.

## 파일

- `dr_neograph.py` — NeoGraph 실행자. Env 구동 손잡이(아래 참조)
- `dr_langgraph.py` — LangGraph와 동일합니다. `def` 노드 동기화 + 동기화
`dr_neograph.py`와의 패리티를 위한 `app.invoke()`입니다.
- `bench.py` — 실제 LLM 하네스. 워밍업 + 교대 측정 +
백분위 수.
- `bench_mock.py` — 모의 LLM가 포함된 엔진 처리량 하니스. 모듈
한 번 사전 로드되면 iter는 컴파일된 엔진을 재사용합니다.
- `mem_probe.py` — 작업자 확장 및 동시 팬아웃 RSS 비교.
- `mem_prod_stack.py` — 프로덕션 스택 메모리 비교.
- `sweep.sh` — `(FANOUT, LLM_MOCK_MS)`에서 `bench_mock.py`를 실행합니다.
변형.
- `_run_single.py` — 원샷 러너. wire/strace 프로브에 사용됩니다.

메모리 프로브에는 [psutil](https://github.com/giampaolo/psutil)가 필요합니다.
프로젝트에 문서화된 대로 설치되었습니다.

```sh
python -m pip install psutil
```

## 환경 손잡이

|바르|기본|목적|
|---|---|---|
|`LLM_MOCK_MS`|-1(실제)|LLM를 `time.sleep(MS)`로 바꾸세요. >=0이면 모의를 활성화합니다.|
|`MOCK_SEARCH`| "0" |Crawl4AI 건너뛰기; 미리 준비된 증거를 반환하세요.|
|`FANOUT`| 5 |연구원이 보낸 횟수.|
|`USE_INMEMORY_CP`| "0" |인메모리 체크포인트를 사용합니다(PG_DSN 무시).|
|`NG_TRANSPORT`|`ws-responses`|NG만 해당: `ws-responses`(WebSocket 응답) 또는 `http-chat`(`/v1/chat/completions`).|
|`NG_WORKER_COUNT`| "4" |NG 전용: 송신 팬아웃 병렬 처리를 위한 스레드 풀입니다.|

## 조사 결과 (2026-04-26)

1. **순수 엔진 처리량(모의 LLM, FANOUT=5)** — NeoGraph 1.0ms
중앙값 대 LangGraph 5.9ms. NG는 LLM 비용이 0일 때 **5.9배 더 빠릅니다**.
2. **실제 LLM 벤치** — 첫 번째 라운드에는 NG p50이 23.90초(sd 5.90), LG가 있었습니다.
21.95초(sd 1.23). LG는 ~10% 더 빨라 보였습니다.
3. **와이어 진단** - WSL2의 pcap 거짓말(BPF는 다음을 통해 대부분의 패킷을 삭제함)
HyperV vswitch). `strace -e trace=connect`는 21을 하는 NG를 보여주었습니다.
7-LLM 호출당 connect() 시스템 호출이 실행됩니다. 매번 새로운 TCP+TLS입니다.
4. **근본 원인** — `SchemaProvider::complete_async`는 무료
대신 `async::async_post()`(호출당 소켓 닫기)
이미 존재하는 `async::ConnPool`(HTTP/1.1 연결 유지).
`run_sync`의 호출별 폐기 io_context는 명백한 "풀"을 만들었습니다.
공급자 내부의 배선은 안전하지 않지만 오래 지속되는 배경
공급자가 소유한 io_context가 작동합니다.
5. **수정(6da4810 / bc2ab4f 커밋)** — SchemaProvider + OpenAIProvider
이제 자체 io_context + 작업자 스레드 + ConnPool을 보유합니다. 후에
수정, NG p90이 35.34초 → 25.28초(-10초) 감소, sd 5.90→1.28
(4.6배 더 안정적임). 중앙값 ~병렬 전송 팬아웃 때문에 변경되지 않음
여전히 HTTP/1.1에 N TCP 연결이 필요합니다.
6. **남은 공백** — LG의 httpx는 HTTP/2, 다중화 N을 지원합니다.
단일 TCP를 통한 병렬 스트림. 이 격차를 해소하려면 NG가 필요합니다.
HTTP/2 클라이언트 지원을 추가합니다(httplib는 HTTP/1.1에만 해당).
7. **작업자 풀 한도** — `set_worker_count(N)`는 Python 노드를 제한합니다.
팬아웃 동시성. 벤치 코드의 `set_worker_count(4)`는 실제였습니다.
천장; `NG_WORKER_COUNT=50`는 LG asyncio보다 NG 동기화를 뒤집습니다.
(FANOUT=50, LLM=100ms에서 307ms 대 711ms).

`feedback_schema_provider_no_pool.md` 및
전체 클로드 메모리의 `feedback_pybind_worker_ceiling.md`
이야기.

## 재현

실제 LLM 벤치:
```sh
set -a && source ../../.env && set +a
export NEOGRAPH_PG_DSN="postgresql://postgres:test@localhost:5433/neograph"
export CRAWL4AI_URL="http://localhost:11235"
export NG_TRANSPORT=http-chat   # apples-to-apples vs LG (both HTTP)
python bench.py --warmup 2 --iters 5
```

엔진 처리량 스윕:
```sh
./sweep.sh   # writes /tmp/sweep.log
```

와이어 진단(pcap에 대해 의심스러운 경우 strace가 Ground Truth입니다.):
```sh
strace -f -e trace=connect -o /tmp/ng.log \
    python _run_single.py neograph
grep "connect(" /tmp/ng.log | grep -oE 'sin_addr=inet_addr\("[^"]+"\)' \
    | sort | uniq -c
```
