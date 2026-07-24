<!-- neograph-i18n: source=docs/promo/README.md locale=ko source_sha256=49b4d504ffeb1f20069738e75640caada9b3aeb0ed51c7290f3204b560543051 -->
**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

# NeoGraph 프로모션 — 원격 소스


저장소 README 상단에 표시된 15초 프로모션 소스
(`docs/videos/neograph-promo-v2.mp4` + `docs/images/neograph-promo-v2.gif`).

**이는 의도적으로 커밋되었습니다.** 원본 프로모션은 한 번 렌더링되었습니다.
소스가 체크인되지 않았으므로
ReAct 그래프 장면은 처음부터 다시 작성하지 않으면 수정할 수 없습니다.
출처는 여기에 남겨두세요.

## 장면(`src/scenes/`)

1. `Intro` — 워드마크 + 금색 자 그리기
2. 그리드 공개(지속적인 `GridBackground`가 빈 비트 위로 페이드 인됨)
3. `ReactGraph` — START → llm_call → tool_dispatch → END 파이프라인.
커넥터는 상자 가장자리(오른쪽 중앙 → 왼쪽 중앙)에 고정되어 있으며
ReAct 루프백 아크는 `llm_call`의 상단 가장자리에 도달합니다. 그것을 밖으로 배치
`ReactGraph.tsx` — widths/gaps에서 `NODES`를 편집하면 명시적이고
중앙에 위치하므로 겹치는 부분이 없습니다.
4. `CodeEditor` — `react_agent.py`는 자신을 입력합니다.
5. `FeatureOutro` — 기능 칩 그리드 → 아웃트로 패널(하나의 연속 장면)

타이밍은 `src/theme.ts`(`SCENES`, `VIDEO`)에 있습니다.

## 재구축

```bash
cd docs/promo
npm install
node render.mjs media          # → out/promo.mp4 (1920x1080, 15s)

# compress + GIF (what ships in docs/):
ffmpeg -i out/promo.mp4 -c:v libx264 -crf 27 -preset slow \
  -pix_fmt yuv420p -movflags +faststart -an ../videos/neograph-promo-v2.mp4 -y
ffmpeg -i out/promo.mp4 -vf "fps=14,scale=960:540:flags=lanczos,palettegen=max_colors=128:stats_mode=diff" -y /tmp/pal.png
ffmpeg -i out/promo.mp4 -i /tmp/pal.png \
  -lavfi "fps=14,scale=960:540:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=5" \
  -y ../images/neograph-promo-v2.gif
```

`node render.mjs stills 30,175,290,445`는 단일 검증을 렌더링합니다.
전체 인코딩 없이 빠른 시각적 비교를 위해 프레임을 `out/`로 변환합니다.

headless/sandboxed 상자에 대한 참고 사항: 렌더러는 로컬 HTTP를 바인딩합니다.
서버 — `render.mjs`는 이를 `REMOTION_PORT`에 고정합니다(기본값 45678).
기본 포트 스캔이 차단될 수 있기 때문입니다. 원격 다운로드
처음 실행 시 Chrome Headless Shell 소유(~108MB)

`node_modules/` 및 `out/`는 무시됩니다.
