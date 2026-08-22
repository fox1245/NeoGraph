<!-- neograph-i18n: source=examples/cookbook/jarvis/README.md locale=ja source_sha256=e52a150fd89075b66a0022d867def85dca59b234e1fc2e664a953c21f6625b10 -->
# JARVIS — 音声駆動型メタ・オーケストレーター

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

> クラウド依存なし、単体のRaspberry Piで動作。
> マイクはTony、NeoGraphはJARVIS、ツール/エキスパートはJARVISの部下。

このクックブックは **「音声TTSの例」ではありません**。NeoGraphのマルチエージェント・プリミティブ — MCPツール、双方向A2A、非同期パラレル、Store memory、ReActサブグラフ — を **たった一行の音声で織り上げた** デモンストレーションです。

## なぜこれが JARVIS か

映画の中のJARVISは、音声TTSを備えた単なるチャットボットではありません。JARVISは同時に5つのことを行います：

1. Tonyが話し終える前に意図を捉える — **高速intent分類**
2. できれば直接答え、できなければ部下に委任 delegate — **4方ルーティング**
3. 同時に複数の情報を収集する — **並列 fan-out**
4. 昨日の会話を思い出す — **長期的な記憶**
5. 他のJARVIS/システムから呼び出し可能 — **双方向A2A**

このクックブックの核心は**グラフの形状**であって、音声ではありません。音声は単なる入出力のシェルにすぎず、「JARVIS感」を生み出すのはNeoGraphのオーケストレーションエンジンです。

## フルグラフ⟦697284dfddf4⟧

```
                          ┌────────────────────────┐
                          │ Background triggers    │
                          │ (timer / external events)│ ── A2A server for
                          └───────────┬────────────┘     JARVIS calls go here
                                      │
 [Microphone]──[VAD]──[whisper.cpp STT]──[memory_lookup]──[intent_router]
    miniaudio                          ▲                   │
                                       │ Store             │
                                       │ (conversation accumulation)│
                                       │                   │ Router makes 4-way decision
                                       │                   │ (chat goes directly to synthesizer)
                                       │                   │
                           ┌───────────┴───────────────────┴───────────────┐
                           │                                                │
                   [direct_branch]        [delegate_branch]        [parallel_branch]
                        │                       │                       │
               MCP tool single call        Delegate to expert entirely       Send / fan-out
               (time, weather, memo, etc.)    (coder, researcher, ...)     to multiple tools simultaneously
                        │                       │                       │
                        └───────────────────────┼───────────────────────┘
                                                │
                                        [response_synth]
                                        (synthesize natural response with large LLM)
                                                │
                                                ↓
                                   [supertonic TTS] ──→ [Speaker]
                                   (in detected language)     miniaudio
```

## 二つのCatalog JSONファイル — JARVISの「できること」

JARVIS起動時、2つのファイルを読み取り、機能リストを構築します。**これは、コードを再コンパイルすることなく機能を追加・削除できることを意味します。**

### `config/mcp_catalog.json` — ツール

JARVISが直接呼び出せる関数型ツールのリスト。各エントリは1つのMCPサーバー(HTTPまたはstdio)に対応する。

```json
{
  "tools": [
    {
      "name": "time_weather",
      "transport": "http",
      "url": "http://127.0.0.1:8000",
      "description": "Short, immediate-answer information like current time, weather, exchange rates",
      "enabled": true
    },
    {
      "name": "personal_memo",
      "transport": "stdio",
      "command": ["python3", "examples/demo_mcp_stdio_server.py"],
      "description": "Tony's personal memo storage/retrieval",
      "enabled": true
    }
  ]
}
```

起動時に、各MCPサーバーに対して`get_tools()`を呼び出し、ツール定義をマージして、ルーターのシステムプロンプトに「利用可能なツール」として注入する。

### `config/agent_registry.json` — エキスパート(A2A)

JARVISがタスク全体を委任できるサブエージェント。各エージェントはA2Aエンドポイントとして別のプロセス/マシンで実行される。

```json
{
  "agents": [
    {
      "name": "coder",
      "url": "http://127.0.0.1:8210",
      "expertise": "Code writing, review, debugging",
      "fetch_card_on_start": true
    },
    {
      "name": "researcher",
      "url": "http://127.0.0.1:8211",
      "expertise": "Web search + summarization, academic paper organization",
      "fetch_card_on_start": true
    }
  ]
}
```

起動時に、各URLから`AgentCard`を要求し、応答したものだけを起動する。**重要な仕掛け**: A2A標準に従う外部エージェントであれば、他人が作ったPython A2Aボットでも、別のNeoGraphインスタンスでも、このJSONにURLを追加するだけでJARVISの配下になる。

## ルーター(意図分類) — JARVISの頭脳

ピン留めされたDeepSeekモデル(`~deepseek/deepseek-v4-flash-latest`)への単一呼び出しで次の結果が返る:

```json
{
  "mode": "chat" | "direct" | "delegate" | "parallel",
  "tool_calls": [{"tool": "time_weather.now", "args": {}}],
  "delegate_to": null,
  "skip_synthesis": false
}
```

- `chat` — ツールや委任なし。シンセサイザーは自身の知識と会話メモリだけを使って直接回答する。挨拶、自己紹介、雑談、「さっき何て言った？」のような会話の想起。ルーターがカタログにないツールやエージェントをでっち上げた場合、検証段階でこのモードに降格する。
- `direct` — 単一ツール呼び出し。結果が単純な場合（`"3:30 PM"`）、`skip_synthesis=true` による合成をスキップし、直接TTSへ進む。**高速。**
- `delegate` — `delegate_to`が指すA2Aエンドポイントに完全に委任する。結果を取得後、音声用に1行の要約だけを合成する。
- `parallel` — 複数の `tool_calls`。NeoGraph の `make_parallel_group` を使って同時実行し、リデューサーが結果を統合してシンセサイザーに渡す。

### なぜルーターとシンセサイザーを分けるのか

すべてをReActで1つの大きなLLMで実行すると、毎ターン1〜3秒かかり、JARVISの雰囲気が損なわれる。
- ルーター: 小さなモデル、約200ms、単一のJSON
- シンセサイザー: 大きなモデル、約800〜1500ms、単一の自然言語応答
- ツールが即座に回答を提供すれば、シンセサイザーをスキップ→応答は約500msで開始される

映画の中のJARVISの素早い応答タイミングは、この分離に由来している。

## メモリ (`Store`)

各ターンの開始時に、`memory_lookup`ノードは、最後のNターンとユーザー設定（`tony.prefers.language=ko`、`tony.last_topic=...`）をNeoGraph `Store`から取得します。

各ターンの終了時に、JARVIS はレスポンスと Tony の発話、使用したツールを Store にプッシュする。次のターンのルーターは「さっき言ったあのこと」のような参照を解決できる。`JsonFileStore` はファイルに永続化される。再起動をまたいで記憶される。空のターン（STT失敗・ノイズ）は、メモリの汚染を防ぐためコミットから除外される。`prefs.native_lang` は推定される母語を維持する（言語の一貫性）。

## 双方向A2A — JARVIS が呼び出し、呼び出される

- **呼び出し**: 専門家に委任する `A2AClient` から `agent_registry.json`.
- **呼び出し先**: JARVIS自体が`A2AServer`（ポート8200）を公開しています。
  - 外部システムは`POST /v1/messages`を介してJARVISにテキストメッセージを送信できます。
  - モバイルアプリ、他のNeoGraphインスタンス、さらには別のJARVISも呼び出すことができます。
  - テキスト入力はマイク/STTステージをスキップし、ルーターに直接送信されます。

**JARVIS間通信デモ**: ホームJARVIS（8200）↔ オフィスJARVIS（8201）。「今日の会議議事録をオフィスJARVISから取得」→ ホームJARVISがA2A経由でオフィスJARVISを呼び出す→ 応答が音声でトニーに配信されます。

## バックグラウンドトリガー（プロアクティブ）

別の非同期グラフがバックグラウンドで実行されます：
- タイマー（5分ごとにカレンダーをチェック）
- 外部イベント（ホームセンサー、メール受信）
- 外部A2A呼び出し

イベントが発生すると、JARVISのメイングラフにメッセージが注入される → Tonyが尋ねる前にJARVISが話す。（「Sir、あと10分で会議です」）

NeoGraphの `27_async_concurrent_runs.cpp` パターンを完全に使用する。

## ディレクトリ構造

```
jarvis/
├── README.md                      ← This document
├── CMakeLists.txt                 External dependencies (whisper/onnxruntime/miniaudio) gated
├── config/                        Default config (graph · catalog · registry · persona)
├── config-demo/                   Execution preset (real-tools / mock)
├── config-bench*/                 Benchmark config
├── src/
│   ├── main.cpp                   Entry point (node registration · graph compilation · main loop)
│   ├── audio/                     miniaudio capture (+Silero VAD) · playback, supertonic TTS
│   ├── stt/                       whisper_node (multi-language · language consistency) + moonshine_node (edge)
│   ├── orchestrator/              Router, MCP catalog loader, A2A dispatcher
│   └── memory/                    Store-based conversation memory (JsonFileStore persistence)
├── specialists/                   coder / researcher (separate A2A servers)
├── bench/                         NeoGraph vs LangGraph benchmark (twin · driver · Docker)
├── assets/download.sh             Download whisper/supertonic/moonshine/silero models
├── scripts/
│   ├── run_jarvis.sh              Execution wrapper (LD_LIBRARY_PATH · ROCm · dxg auto)
│   ├── jarvis_repl.py             Korean readline REPL (text/wav input)
│   ├── build_whisper_hip.sh       Build whisper.cpp ROCm/HIP GPU
│   └── demo_mcp_server.py         Demo MCP server (time/weather/calc)
└── docs/architecture.md          Detailed node-by-node graph explanation
```

## ビルド / 実行

```bash
# 1. Download models (whisper-large-v3-turbo ~1.6GB + supertonic + silero VAD)
#    Lightweight: JARVIS_WHISPER=small bash assets/download.sh  (Raspberry Pi / CPU)
bash examples/cookbook/jarvis/assets/download.sh

# 2. Build — onnxruntime, whisper.cpp, miniaudio found on system (or mock if missing)
cmake -B build-jarvis -DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON
cmake --build build-jarvis --target cookbook_jarvis -j

# 3a. Run — text/wav input (Korean line-edit REPL recommended)
cd examples/cookbook/jarvis
python3 scripts/jarvis_repl.py                 # Automatically loads OPENROUTER_API_KEY from .env
#   Tony ▸ Hello?                                # Text
#   Tony ▸ wav:/path/to/audio.wav                # Audio file → STT

# 3b. Run — live microphone (miniaudio capture + Silero VAD)
JARVIS_MIC=1 bash scripts/run_jarvis.sh config-demo/real-tools
#   "Online" appears → speak → voice end detection → STT → response → TTS

# (Demo MCP server for tools — separate terminal)
python3 scripts/demo_mcp_server.py 8888        # Time/weather/calc
```

ライブプロバイダーは、ピン留めされたDeepSeekモデルと`OPENROUTER_API_KEY`を`.env`で持つOpenRouterに固定されています。キーがない場合、MockProvider（エコー）でオフライン実行されます。

## 音声スタックの詳細

### ライブマイク (miniaudio + Silero VAD)
`JARVIS_MIC=1` または config `use_microphone:true`。キャプチャワーカースレッドは512サンプルウィンドウ上でSilero VADを実行し、音声の開始/終了（200ms プリロール、500ms サイレンス終了）を検出する。**バックプレッシャー**: 推論中はキャプチャを破棄して、TTS エコー、古い発話、発話開始ノイズをブロックする。デバイス故障 (WSL2 マイク切断など) は自動的に stdin へフォールバックする。チューニング: `JARVIS_VAD_THRESHOLD` (デフォルト 0.5), 監視: `JARVIS_MIC_DEBUG=1`.

### STT — 2つの選択肢(`stt.type` による設定切り替え)
- **`whisper_stt`** (デフォルト): whisper.cpp。`language:"auto"` は99言語を自動検出 → **話者の言語での応答とTTS**。**言語一貫性**: store.prefs でネイティブ言語を維持するため、短い発話が外国語として誤認識されても突然切り替わらない（切り替わるには一貫した誤認識が必要）。
- **`moonshine_stt`**: Moonshine-tiny ONNX（27M、supertonic と ORT を共有）。エッジ、低レイテンシー、韓国語向け。言語固有モデルのため、言語は固定。

### GPU高速化 (whisper.cpp ROCm/HIP)
同梱の whisper.cpp は CPU 専用です — 大規模モデルは CPU で約32秒かかります（11秒のクリップ）。AMD GPU（gfx1201=R9700、ROCm≥7.2）では、GGML_HIP ビルド用に `bash scripts/build_whisper_hip.sh` を実行してください → **約7秒（4.5倍）**。run_jarvis.sh は ROCm ランタイムと WSL dxg を自動的に読み込みます。

## ベンチマーク — NeoGraph 対 LangGraph（`bench/`）

LangGraph（Python ツイン`langgraph_twin.py`）において同一のトポロジー（mic→stt→merge→memory→router→4-way→synth/skip→commit→tts）をミラーリングし、同一の制約（`--cpus=2 --memory=2g`）コンテナ内で計測する。

```bash
OPENROUTER_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + OpenRouter 20 turns × both
```

## 実装ステータス

**完全機能** — 実機（OpenRouter DeepSeek）でのライブ音声シングルターン実行を検証済み。マイク→VAD→STT→ルーター→4方向→シンセシス→TTSフルチェーン+

既知の制限事項 / 次バージョン:
- **Barge-in非対応** — TTS再生中の発話はグブレッシャーで破棄されます (v2でcancel tokenを追加予定).
- **ストリーミングSTTは未適用** — 発話完了後のバッチ転写. Moonshine v2のエルゴード encoderチャンク毎のストリーム化転写が次の候補です.
- **複数話者・長期メモリ圧縮** — 単一話者前提、24ターン上限。
- 背景トリガー (プロアクティブ機能) — 設計済みだが未実装.

## **License / 外部依存関係**

| : * ライブラリ | License | ロール |
|---|---|---|
| [supertonic](https://github.com/supertone-inc/supertonic) | MIT | TTS（99M、ONNX、31言語） |
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) | MIT | STT（99言語自動検出、CPU/ROCm） |
| [Moonshine](https://github.com/moonshine-ai/moonshine) | MIT | Edge STTオプション (27M ONNX) |
| [miniaudio](https://github.com/mackron/miniaudio) | MIT-0 / パブリックドメイン | マイクキャプチャ＋スピーカー再生 |
| [Silero VAD](https://github.com/snakers4/silero-vad) | MIT | 音声開始・終了検出 (ONNX) |
| ONNX Runtime | MIT | supertonic・moonshine・VAD推論 |
