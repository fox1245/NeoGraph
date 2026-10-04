<!-- neograph-i18n: source=examples/cookbook/jarvis/README.md locale=ja source_sha256=4de91aa4e04dc5f5a30c8878f37fa3dbf2c16671b0545387dd6ee20465a280ee -->
# JARVIS — 音声駆動型メタ・オーケストレーター

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## 型付きプロバイダー移行 — 範囲限定実行状況

C++ルーター・合成器・専門家フィクスチャは型付き `ProviderRequest`、`sp::Message`、`sp::Event` と完全な不変 `sp::Outcome` (`sp::runtime::Result`) を使用し、旧文字列応答APIではない。`src/provider_support.h` のJarvis/coder/researcher mockは固定ルーターJSON、ユーザーテキストecho、明示的な架空の研究応答を返す。キーやネットワーク提供者は不要だが、本物の研究・推論ではない。以下の既存設定/プロファイルのパスを、この文書変更で作成・修正することはない。

ローカル音声は任意で、選択したwhisper/Moonshineモデル、ONNX Runtime/Supertonic資産、miniaudio、利用可能なマイク・スピーカーが必要。テキスト/mock動作は音声動作の証拠ではない。クラウド不要はローカル/mockのみ。ライブには承認された `OPENROUTER_API_KEY`、ネットワーク・提供者容量が必要で、プロンプト・会話メモリ・添付ツール/委譲結果をOpenRouterへ送信する。モデルは固定され、ネイティブ要求のZDRは地域内常駐保証ではない。キーをログ・リポジトリへ入れない。nullableトークン使用量は請求額ではなく、費用には現行のエンドポイント/モデル価格と実際の請求対象使用量が必要。

`[jarvis:ttft]` は最初の非空 `sp::PartDelta` の `PartKind::Text`・`DeltaChannel::Content` で発生し、使用量・推論・ヘッダーイベントでは発生しない。最初の合成テキストであり、実際のTTS音声開始ではない。Python REPL driver は protocol client です。pybind benchmark は移行済み型付き binding を使い、別の実行証拠が必要です。現在の実行証拠は実際のCLI挨拶、永続化した合成メモリturn、正常なEOF終了に限定される。マイク入力・ASR・TTS・pybindベンチやvendor推論の検証ではない。以下の時間・音声/live実行主張は過去の記録であり、現在の移行qualificationではない。

上の CLI 証拠は interface 3 で記録しました。保持された A2A 1.0 wire 更新と SDK interface-4 制御は
ソース契約で、新しい runtime pass ではありません。

> ローカル/mockはクラウド提供者不要。任意の音声にはローカル資産・機器が必要。
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

## フルグラフ

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

起動時に JARVIS は設定済み AgentCard を fetch します。呼出しには互換 JSON-RPC 0.x/1.0 interface が必要で、discovery 応答だけでは互換性を証明しません。client は設定済み endpoint を変えず card dialect を選びます。card-selected 呼出しは他 dialect に fallback せず、配信済み SSE event は再送しません。外部 Python agent と NeoGraph instance も card と wire 動作が一致すればこの契約を使えます。

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

この JSON file は speech/conversation projection を保存し、認証された native provider replay custody ではありません。

## 双方向A2A — JARVIS が呼び出し、呼び出される

- **呼び出し**: 専門家に委任する `A2AClient` から `agent_registry.json`.
- **呼び出し先**: JARVIS自体が`A2AServer`（ポート8200）を公開しています。
  - 外部システムは `POST /` に JSON-RPC を送ります。0.x は `message/send`、1.0 は `A2A-Version: 1.0` と `SendMessage` を使い、header が応答 dialect を選びます。
  - モバイルアプリ、他のNeoGraphインスタンス、さらには別のJARVISも呼び出すことができます。
  - テキスト入力はマイク/STTステージをスキップし、ルーターに直接送信されます。

**JARVIS間通信デモ**: ホームJARVIS（8200）↔ オフィスJARVIS（8201）。「今日の会議議事録をオフィスJARVISから取得」→ ホームJARVISがA2A経由でオフィスJARVISを呼び出す→ 応答が音声でトニーに配信されます。

## バックグラウンドトリガー（プロアクティブ）

background timer/event trigger は設計であり実装済み component ではありません。host は `27_async_concurrent_runs.cpp` pattern で calendar/sensor event の text を queue に入れられます。A2A server は別に実装済みで proactive trigger の検証ではありません。

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

# 2. Build — install SchemaProvider and optional voice dependencies first
export SCHEMAPROVIDER_PREFIX="/absolute/path/to/installed/schemaprovider"
cmake -S . -B build-jarvis -DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON \
  -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX"
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

**過去の音声実行証拠** — 以前の実機での live 単一 turn 実行

既知の制限事項 / 次バージョン:
- **Barge-in非対応** — TTS再生中の発話はグブレッシャーで破棄されます (v2でcancel tokenを追加予定).
- **ストリーミングSTTは未適用** — 発話完了後のバッチ転写. Moonshine v2のエルゴード encoderチャンク毎のストリーム化転写が次の候補です.
- **Multi-speaker · long-memory compression** — 一話者を想定します。lookup は既定で最近六 turn、commit は最新 24 turn を保持し、古い履歴は要約しません。
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
