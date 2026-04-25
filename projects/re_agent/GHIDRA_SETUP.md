# Ghidra + GhidraMCP 셋업 가이드

이 문서가 RE agent를 처음 돌리기 위한 **단 하나의 진입점**입니다. Ghidra
의 plugin enable 절차는 직관적이지 않아서 (4단계로 나뉘고 중간에 restart
가 필요함) 이 순서를 그대로 따라가는 걸 권장합니다.

## 0. 사전조건

체크리스트:

- [ ] Docker Desktop (또는 WSL2 native docker engine) 실행 중 — `docker version` 확인
- [ ] WSLg 데스크탑 사용 가능 — `echo $DISPLAY` 가 `:0` 출력
- [ ] `.env` 파일에 `OPENAI_API_KEY=sk-...` 설정 (NeoGraph repo root)
- [ ] `targets/crackme01` 빌드됨 — 없으면 `./targets/build.sh`
- [ ] NeoGraph `example_re_agent` 빌드됨 — 없으면:
  ```
  cmake -S . -B build-release -DNEOGRAPH_BUILD_MCP=ON -DNEOGRAPH_BUILD_EXAMPLES=ON
  cmake --build build-release --target example_re_agent -j
  ```

## 1. 컨테이너 시작

```
cd projects/re_agent/docker
docker compose up -d
```

이 한 줄이 다 합니다:
- `blacktop/ghidra:11.4.3` 위에 GhidraMCP 플러그인 (11.4.3로 manifest 패치된 버전) 을 시스템 Extensions에 미리 복사한 이미지를 빌드/실행
- WSLg X11 socket 마운트 → Ghidra GUI가 호스트 데스크탑에 표시
- 호스트 `127.0.0.1:18080` ↔ 컨테이너 `:8080` 매핑 (호스트 8080은 다른
  컨테이너가 자주 쓰니 18080으로 비켜놓음)
- `targets/`를 `/samples`에 read-only 마운트
- 사용자 홈 dir (project, prefs, plugin enablement 상태)을
  `neograph-re-ghidra-home` named volume에 영속화

검증: 1분쯤 기다렸다가
```
docker compose logs ghidra | grep "startup complete"
```
`Ghidra startup complete (xxxx ms)` 가 보이면 GUI도 떠 있음 (WSLg 데스크탑
에서 Ghidra 창 확인).

## 2. Ghidra 첫 셋업 (1회성)

WSLg 데스크탑의 **Ghidra Project Manager** 창에서:

1. **File → New Project** → Non-Shared → 이름 `NeoGraphTest` (또는 자유)
2. **File → Import File** → `/samples/crackme01` 선택
   - "Open File" 다이얼로그에서 좌측 트리로 `/samples`까지 내려가면 됨
3. import된 `crackme01`을 **더블클릭** → CodeBrowser 창이 열림
4. "Analyze 'crackme01'? Yes/No" 묻으면 **Yes** → 모든 분석기 기본값 OK
5. Analysis 끝날 때까지 (보통 5~30초) 좌하단 progress bar 대기

## 3. Plugin install (1회성, restart 필요) ⚠️

이게 사람들이 가장 많이 막히는 단계입니다. **CodeBrowser의 File 메뉴가
아니라 Project Manager 창의 File 메뉴**를 써야 함.

1. **Project Manager** 창으로 가기 (작업표시줄에서 "Ghidra: NeoGraphTest")
2. **File → Install Extensions...**
3. 목록에서 **GhidraMCP** (Version 11.4.3) 체크박스 켜기
4. OK
5. "Restart Ghidra now?" → Yes
   - 또는 호스트에서 `docker compose restart ghidra` 후 1분 대기

> **GhidraMCP가 목록에 안 보이면**: docker 이미지에서 plugin이 시스템
> Extensions로 복사 안 된 것. `docker compose exec ghidra ls /ghidra/Extensions/Ghidra/GhidraMCP/` 로
> 확인하고, 비어 있으면 `docker compose build --no-cache && up -d`.

## 4. Plugin enable (1회성, restart 직후) ⚠️

restart 후 Project Manager 다시 열린 상태에서:

1. crackme01 다시 더블클릭 → CodeBrowser 다시 열림
2. CodeBrowser 메뉴 **File → Configure**
3. 패키지 카테고리 6개가 보임 (Ghidra Core, BSim, Debugger, Miscellaneous,
   **Developer**, Experimental)
4. ★ **Developer 옆 체크박스를 켭니다** ★
   - 이걸 안 켜면 GhidraMCPPlugin이 어디에도 안 보입니다. plugin이
     `packageName="Developer"` 로 등록돼있기 때문.
5. **Developer** 줄의 **Configure** 링크 클릭
6. Filter에 `GhidraMCP` → 검색 결과에 **GhidraMCPPlugin** 보임 → 체크 → OK
7. **Close** 로 Configure Tool 닫기

이 시점에 plugin이 자동으로 8080 (컨테이너 내부) HTTP 서버를 띄웁니다.

## 5. 검증

호스트에서:
```
curl -s http://127.0.0.1:18080/methods | head
```
함수 이름 리스트가 나오면 성공:
```
_DT_INIT
FUN_00101020
__cxa_finalize
puts
...
FUN_001012ea
_DT_FINI
```

(이미 RE agent를 한 번 돌렸다면 `FUN_xxxxxx` 대신 `xor_encrypt`,
`compute_checksum` 같은 의미있는 이름이 보임 — Ghidra DB에 영속됨.)

응답이 안 오면:
- `docker compose logs ghidra | grep -i ghidramcp` — `HTTP server started on port 8080` 라인 있어야 함
- 없으면 4단계 plugin enable이 안 된 것 → 다시 확인

## 6. RE agent 실행

NeoGraph repo root에서:
```
./build-release/example_re_agent > /tmp/agent_out.json 2> /tmp/agent_trace.log
```

- stderr (`agent_trace.log`): tool 발견 + ReAct trace
- stdout (`agent_out.json`): 최종 JSON 결과

기대 결과: 6개 user 함수가 모두 의미있는 이름으로 rename되고, 각 함수에
한 줄 summary comment 추가됨. Ghidra GUI의 Symbol Tree → Functions에서
시각 확인 가능.

## 트러블슈팅

| 증상 | 원인/해결 |
|---|---|
| GUI 창 안 뜸 | `echo $DISPLAY` 가 `:0` 인지 확인. `ls /tmp/.X11-unix/` 에 X0 socket 있어야 함. WSLg 안 쓰면 xhost 또는 Xauth 별도 작업 필요. |
| Install Extensions 목록에 GhidraMCP 없음 | 이미지 빌드 문제. `docker compose build --no-cache ghidra` 다시. |
| Configure에 GhidraMCPPlugin 안 보임 | **Developer 패키지 체크박스 안 켰음** (4-4단계). |
| `:18080` connection refused | CodeBrowser 안 열렸거나 plugin 미enable. `docker compose logs ghidra \| grep -i ghidramcp` 로 plugin 로드 여부 확인. |
| `Ghidra startup complete` 안 나옴 | Java OOM 가능성. compose.yaml의 `mem_limit` / `MAXMEM` 늘려보기. |
| Ghidra 창은 떴는데 한글 깨짐/폰트 이상 | WSLg 폰트 issue, NeoGraph agent 동작에는 무영향. |

## 참고: native (non-docker) 셋업

docker 부담을 피하고 싶으면 호스트에 직접:

1. Ghidra 11.0.3 설치 → `/root/ghidra_11.0.3_PUBLIC/ghidraRun`
2. `/root/mcp-servers/GhidraMCP/release/GhidraMCP-11.0.3.zip` 을 Install Extensions로 등록
3. 위 3~6단계는 동일
4. agent에 native bridge 경로 알려주기:
   ```
   GHIDRA_SERVER_URL=http://127.0.0.1:8080/ ./build-release/example_re_agent
   ```

(메모리 reference: `/root/mcp-servers/GhidraMCP/bridge_mcp_ghidra.py` + 플러그인 zip)
