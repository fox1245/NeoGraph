<!-- neograph-i18n: source=docs/promo/README.md locale=ko source_sha256=eda23a0fd62ee1ba5a3eb5f26292eac3aa3ac273903f5fd8c5714eaa49627b54 -->
# NeoGraph 프로모 — Remotion 소스

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

리포지토리 README 상단에 표시되는 15초 프로모션의 소스입니다(`docs/videos/neograph-promo-v3.mp4` + `docs/images/neograph-promo-v3.gif`).

장면을 다시 편집하고 렌더링할 수 있도록 소스를 커밋했습니다.
생성 video는 Program pipeline을 설명하며 API reference, benchmark, qualification 기록이 아닙니다.
미디어 파일명의 `v3`는 asset 이름이지 SDK interface revision이 아닙니다. NeoGraph `0.13.1`은 alpha SDK `0.1.1`, interface/shared generation 4를 사용합니다. 기존 render는 해당 runtime이나 대기 중인 검증을 입증하지 않습니다.

## 장면 (`src/scenes/`)

1. `Intro` — 워드마크 + 금색 선 그리기
2. 그리드 공개(지속적인 `GridBackground`가 빈 비트 위에 페이드 인)
3. `ReactGraph` — 제안 → 컴파일 → 의미 검증 → 승인(admission) 파이프라인. 커넥터는 박스 가장자리(오른쪽 중간 → 왼쪽 중간)에 고정되고 ReAct 루프백 호는 `llm_call`의 상단 가장자리에 닿습니다. `NODES`를 `ReactGraph.tsx`에서 편집해 레이아웃을 구성하십시오. 너비와 간격이 명시적이고 중앙 정렬되므로 아무것도 겹치지 않습니다.
4. `CodeEditor` — QuickJS `define()` + 생성기 `main()`이 스스로 입력되어 나타납니다
5. `FeatureOutro` — 현재 Program, Hook, 런타임 컨텍스트, 프로토콜 기능 그리드 → 아웃트로 패널

타이밍은 `src/theme.ts`(`SCENES`, `VIDEO`)에 있습니다.

## 재빌드

Node.js/npm과 FFmpeg를 설치하세요. `package-lock.json`이 Remotion/React 의존성 트리를 고정하고
`npm ci`는 기록된 의존성을 설치합니다.

```bash
cd docs/promo
npm ci
node render.mjs media          # → out/promo.mp4 (1920x1080, 15s)

# compress + GIF (what ships in docs/):
ffmpeg -i out/promo.mp4 -c:v libx264 -crf 27 -preset slow \
  -pix_fmt yuv420p -movflags +faststart -an ../videos/neograph-promo-v3.mp4 -y
ffmpeg -i out/promo.mp4 -vf "fps=14,scale=960:540:flags=lanczos,palettegen=max_colors=128:stats_mode=diff" -y /tmp/pal.png
ffmpeg -i out/promo.mp4 -i /tmp/pal.png \
  -lavfi "fps=14,scale=960:540:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=5" \
  -y ../images/neograph-promo-v3.gif
```

`node render.mjs stills 30,175,290,445`는 전체 인코딩 없이 빠른 시각적 diff를 위해 개별 검증 프레임을 `out/`로 렌더링합니다.

헤드리스/샌드박스 환경에서 렌더러는 로컬 HTTP 서버를 바인딩합니다. 기본 포트 검색이 차단될 수 있어 `render.mjs`는 `REMOTION_PORT`(기본 45678)를 사용합니다. Chrome Headless Shell이 설치되지 않았다면 Remotion이 다운로드하며 크기는 달라질 수 있습니다.

`node_modules/`와 `out/`는 gitignore에 포함됩니다.
