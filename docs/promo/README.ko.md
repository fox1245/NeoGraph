<!-- neograph-i18n: source=docs/promo/README.md locale=ko source_sha256=9321b772f9d90caa27addc8f166b403e9874ce911798bfd16ed8b703811197a4 -->
# NeoGraph 프로모 — Remotion 소스

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

리포지토리 README 상단에 표시되는 15초 프로모션의 소스입니다(`docs/videos/neograph-promo-v3.mp4` + `docs/images/neograph-promo-v3.gif`).

**이것은 의도적으로 커밋된 것입니다.** 원본 프로모션은 한 번 렌더링되었고 소스는 체크인된 적이 없어서, ReAct-그래프 장면의 끊어진 커넥터를 처음부터 다시 빌드하지 않고는 수정할 수 없었습니다. 소스를 여기에 유지하세요.

## 장면 (`src/scenes/`)

1. `Intro` — 워드마크 + 금색 선 그리기
2. 그리드 공개(지속적인 `GridBackground`가 빈 비트 위에 페이드 인)
3. `ReactGraph` — 제안 → 컴파일 → 의미 검증 → 승인(admission) 파이프라인. 커넥터는 박스 가장자리(오른쪽 중간 → 왼쪽 중간)에 고정되고 ReAct 루프백 호는 `llm_call`의 상단 가장자리에 닿습니다. `NODES`를 `ReactGraph.tsx`에서 편집해 레이아웃을 구성하십시오. 너비와 간격이 명시적이고 중앙 정렬되므로 아무것도 겹치지 않습니다.
4. `CodeEditor` — QuickJS `define()` + 생성기 `main()`이 스스로 입력되어 나타납니다
5. `FeatureOutro` — 현재 Program, Hook, 런타임 컨텍스트, 프로토콜 기능 그리드 → 아웃트로 패널

타이밍은 `src/theme.ts`(`SCENES`, `VIDEO`)에 있습니다.

## 재빌드

```bash
cd docs/promo
npm install
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

헤드리스/샌드박스 환경 참고 사항: 렌더러는 로컬 HTTP 서버를 바인딩합니다. 기본 포트 스캔이 차단될 수 있으므로 `render.mjs`는 포트를 `REMOTION_PORT`(기본값 45678)로 고정합니다. Remotion은 첫 실행 시 자체 Chrome Headless Shell을 다운로드합니다(~108 MB).

`node_modules/`와 `out/`는 gitignore에 포함됩니다.
