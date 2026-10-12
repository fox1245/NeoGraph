<!-- neograph-i18n: source=docs/promo/README.md locale=ko source_sha256=30c867676721c71ac71439cd89b4b5ce8a34f8a0db32675233aa37d1cd21d390 -->
# NeoGraph 프로모 — Remotion 소스

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

15초짜리 프로모 영상의 소스입니다
(`docs/videos/neograph-promo-v3.mp4` + `docs/images/neograph-promo-v3.gif`).

장면을 수정하고 다시 렌더링할 수 있도록 소스를 커밋해 두었습니다.
생성된 영상은 Program 파이프라인을 보여 주는 그림일 뿐이며 API 레퍼런스나 벤치마크가 아닙니다.
미디어 파일명의 `v3`는 에셋 이름표이지 SDK 인터페이스 리비전이 아닙니다.

## 장면 (`src/scenes/`)

1. `Intro` — 워드마크와 금색 선 그리기
2. 그리드 공개 (고정 배경인 `GridBackground`가 아무것도 없는 구간 위로 서서히 나타남)
3. `ReactGraph` — 제안 → 컴파일 → 의미 검증 게이트 → 승인(admission)으로 이어지는 파이프라인.
   연결선은 박스 가장자리(오른쪽 가운데 → 왼쪽 가운데)에 붙고, ReAct 루프백 호는 `llm_call`
   윗변에 닿습니다. 레이아웃은 `ReactGraph.tsx`의 `NODES`를 고쳐서 바꿉니다. 너비와 간격이
   명시되어 있고 가운데 정렬되므로 겹치는 요소가 없습니다.
4. `CodeEditor` — QuickJS `define()`과 제너레이터 `main()`이 저절로 타이핑되듯 나타납니다
5. `FeatureOutro` — 현재 Program, Hook, 런타임 컨텍스트, 프로토콜 기능을 보여 주는 격자 → 아웃트로 패널

타이밍은 `src/theme.ts`의 `SCENES`와 `VIDEO`에서 정합니다.

## 다시 빌드하기

Node.js/npm과 FFmpeg를 설치합니다. `package-lock.json`이 Remotion/React 의존성 트리를
고정하며, `npm ci`는 여기 기록된 의존성을 그대로 설치합니다.

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

`node render.mjs stills 30,175,290,445`는 전체 인코딩 없이 눈으로 빠르게 비교할 수 있도록,
검증용 프레임을 하나씩 `out/`에 렌더링합니다.

헤드리스 환경이나 샌드박스 환경에서는 다음을 참고하세요. 렌더러는 로컬 HTTP 서버를 띄웁니다.
기본 포트 탐색이 막힐 수 있으므로 `render.mjs`는 서버를 `REMOTION_PORT`(기본값 45678)에
고정합니다. Chrome Headless Shell이 설치되어 있지 않으면 Remotion이 직접 내려받으며,
다운로드 크기는 일정하지 않습니다.

`node_modules/`와 `out/`는 gitignore에 등록되어 있습니다.
