<!-- neograph-i18n: source=examples/cookbook/jarvis/README.md locale=ja source_sha256=19709bb07c36265ac28bd757009fd525505ef7bcd930e8d1b5ee6b763c4ff454 -->
# JARVIS — 音声主導型メタオーケストレーター

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

> クラウドゼロの依存関係は、単一の Raspberry Pi 上で実行されます。
> マイクはTony、NeoGraphはJARVIS、ツール/専門家はJARVISの部下です。

このクックブックは「音声 TTS サンプル」ではありません**。 NeoGraph のデモンストレーションです
マルチエージェント プリミティブ — MCP ツール、双方向 A2A、非同期並列、ストア メモリ、
ReAct サブグラフ — **単一の音声行で織り込まれた**。

## なぜこれがジャービスなのか

映画の中の JARVIS は、音声 TTS を備えた単なるチャットボットではありません。 JARVIS は 5 つのことを同時に実行します。

1. トニーが話し終わる前に意図を把握します — **高速な意図分類**
2. 可能であれば直接回答し、そうでない場合は部下に委任します — **4 方向のルーティング**
3. 複数の情報を一度に収集 — **並列ファンアウト**
4. 昨日の会話を覚えています — **長期記憶**
5. 他の JARVIS/システムから呼び出すことができます — **双方向 A2A**

したがって、このクックブックの核心は音声ではなく、**グラフの形状**です。声はまさ​​に、
入出力シェル。 「JARVIS 感」を生み出すのは、NeoGraph のオーケストレーション エンジンです。

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

## 2 つのカタログ JSON ファイル — JARVIS の「私にできること」

JARVIS が起動すると、2 つのファイルを読み取り、機能リストを作成します。
**これは、コードを再コンパイルせずに機能を追加/削除できることを意味します。**

### `config/mcp_catalog.json` — ツール

JARVIS が直接呼び出すことができる関数タイプのツールのリスト。
各エントリは 1 つの MCP サーバー (HTTP または stdio) に対応します。

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

起動時に、各 MCP サーバーで `get_tools()` を呼び出します → ツール定義をマージします
そしてそれらを「利用可能なツール」としてルーターのシステムプロンプトに挿入します。

### `config/agent_registry.json` — エキスパート (A2A)

JARVIS はタスク全体を委任できるサブエージェントです。それぞれが別個のプロセス/マシンとして実行されます
A2A エンドポイントとして。

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

起動時に各URLから`AgentCard`をリクエスト→応答したもののみアクティブ化します。
**重要なトリック**: A2A 標準に準拠する外部エージェント (Python であっても)
他の人が作成した A2A ボット、別の NeoGraph インスタンスなど - JARVIS のものになる可能性があります
この JSON に URL を追加するだけで、従属するようになります。

## ルーター (意図分類) — JARVIS の頭脳

小型/高速 LLM (`gpt-4o-mini` またはローカル `llama-3.2-1b` など) を 1 回呼び出すと、次が返されます。

```json
{
  "mode": "chat" | "direct" | "delegate" | "parallel",
  "tool_calls": [{"tool": "time_weather.now", "args": {}}],
  "delegate_to": null,
  "skip_synthesis": false
}
```

- `chat` — ツールや委任はありません。シンセサイザーは、自身の知識 + 会話の記憶を使用して直接応答します。
  挨拶、自己紹介、世間話、「さっき何と言ったっけ？」スタイルの会話を思い出します。ルーターの場合
  カタログにないツール/エージェントを発明した場合、検証ステージはこのモードに降格します。
- `direct` — 単一のツール呼び出し。結果が単純 (`"3:30 PM"`) の場合は、`skip_synthesis=true` で合成をスキップします。
  そしてそのままTTSへ。 **速い。**
- `delegate` — `delegate_to` が指す A2A エンドポイントに完全に委任します。
  結果が得られたら、音声用の 1 行の要約のみを合成します。
- `parallel` — 複数の `tool_calls`。 NeoGraphの`make_parallel_group`を使って同時実行し、
  レデューサーは、シンセサイザーの結果を結合します。

### ルーターとシンセサイザーを分離する理由

ReAct を使用して 1 つの大きな LLM ですべてを実行すると、1 ターンあたり 1 ～ 3 秒かかり、JARVIS の感覚が失われます。
- ルーター: 小型モデル、約 200 ミリ秒、単一 JSON
- シンセサイザー: 大規模モデル、約 800 ～ 1500 ミリ秒、単一の自然言語応答
- ツールが即時に応答を提供する場合、シンセサイザーをスキップ → 応答は約 500 ミリ秒で開始されます

映画「JARVIS」における反応のタイミングの速さは、この分離に由来している。

## メモリ (`Store`)

各ターンの開始時に、`memory_lookup` ノードは最後の N ターン + ユーザー設定をプルします。
(`tony.prefers.language=ko`、`tony.last_topic=...`) NeoGraph `Store` から。

各ターンの終了時に、JARVIS は応答 + トニーの発言 + 使用したツールをストアにプッシュします。
次のターンのルーターは「前に話したあれ」のような参照を解決できます。 `JsonFileStore`
ファイルに保存されます — 再起動しても記憶されます。空ターン（STT故障・ノイズ）は除く
メモリ汚染を防ぐためにコミットから削除します。 `prefs.native_lang` は推定された母国語を維持します
（言語の一貫性）。

## 双方向 A2A — JARVIS が呼び出され、呼び出される

- **通話**: `agent_registry.json` から `A2AClient` 経由で専門家に委任します。
- **呼び出し中**: JARVIS 自体は `A2AServer` (ポート 8200) を公開します。
  - 外部システムは、`POST /v1/messages` 経由でテキスト メッセージを JARVIS に送信できます。
  - モバイル アプリ、他の NeoGraph インスタンス、さらに別の JARVIS からも呼び出すことができます。
  - テキスト入力はマイク/STT ステージをスキップし、ルーターに直接入力されます。

**JARVIS 間通信デモ**: ホーム JARVIS (8200) ↔ オフィス JARVIS (8201)。
「事務所JARVISから今日の議事録を入手」→自宅JARVISがA2A経由で事務所JARVISに電話
→ 応答は音声でトニーに届けられました。

## バックグラウンドトリガー (プロアクティブ)

別の非同期グラフがバックグラウンドで実行されます。
- タイマー（5分ごとにカレンダーを確認）
- 外部イベント（ホームセンサー、電子メール受信）
- 外部 A2A コール

イベントが発生すると、JARVIS のメイン グラフにメッセージが挿入されます → JARVIS が会話します
トニーが尋ねる前に。 （「先生、10分後に集合です。」）

NeoGraph の `27_async_concurrent_runs.cpp` パターンを正確に使用します。

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

## ビルド/実行

```bash
# 1. Download models (whisper-large-v3-turbo ~1.6GB + supertonic + silero VAD)
#    Lightweight: JARVIS_WHISPER=small bash assets/download.sh  (Raspberry Pi / CPU)
bash examples/cookbook/jarvis/assets/download.sh

# 2. Build — onnxruntime, whisper.cpp, miniaudio found on system (or mock if missing)
cmake -B build-jarvis -DNEOGRAPH_BUILD_COOKBOOK_JARVIS=ON
cmake --build build-jarvis --target cookbook_jarvis -j

# 3a. Run — text/wav input (Korean line-edit REPL recommended)
cd examples/cookbook/jarvis
python3 scripts/jarvis_repl.py                 # Automatically loads OPENAI_API_KEY from .env
#   Tony ▸ Hello?                                # Text
#   Tony ▸ wav:/path/to/audio.wav                # Audio file → STT

# 3b. Run — live microphone (miniaudio capture + Silero VAD)
JARVIS_MIC=1 bash scripts/run_jarvis.sh config-demo/real-tools
#   "Online" appears → speak → voice end detection → STT → response → TTS

# (Demo MCP server for tools — separate terminal)
python3 scripts/demo_mcp_server.py 8888        # Time/weather/calc
```

`.env` の `OPENAI_API_KEY` によって選択された LLM プロバイダー (直接 OpenAI) または
`OPENAI_BASE_URL`+`JARVIS_ROUTER_MODEL`/`JARVIS_SYNTH_MODEL` (Groq/Cerebras/etc.)
OpenAI互換）。これがないと、MockProvider (echo) を使用してオフラインで実行されます。

## 音声スタックの詳細

### ライブマイク (ミニオーディオ + Silero VAD)
`JARVIS_MIC=1` または構成 `use_microphone:true`。キャプチャ ワーカー スレッドが Silero VAD を実行する
512 サンプル ウィンドウで音声の開始/終了を検出します (200 ミリ秒のプリロール、500 ミリ秒の無音終了)。
**バックプレッシャー**: TTS エコー、古い発話、
そしてノイズを開始します。デバイスの障害 (WSL2 マイクの切断など) は、自動的に標準入力に戻ります。
チューニング: `JARVIS_VAD_THRESHOLD` (デフォルト 0.5)、観察: `JARVIS_MIC_DEBUG=1`。

### STT — 2 つのオプション (構成 `stt.type` による交換)
- **`whisper_stt`** (デフォルト): whisper.cpp。 `language:"auto"` は 99 の言語を自動的に検出します
  → **話者の言語での回答と TTS**。 **言語の一貫性**: 母国語を維持します
  store.prefs にあるため、短い発話が外国語として誤って認識されることはありません (必須)
  一貫した誤認による切り替え）。
- **`moonshine_stt`**: 密造酒のような小さな ONNX (27M、スーパートニックと ORT を共有)。
  エッジ、低遅延、韓国風味。言語固有のモデルなので、lang は固定です。

### GPU アクセラレーション (whisper.cpp ROCm/HIP)
バンドルされている whisper.cpp は CPU のみです。大きい場合は CPU で最大 32 秒かかります (クリップは 11 秒)。 AMD GPU
(gfx1201=R9700、ROCm≥7.2) GGML_HIP に対して `bash scripts/build_whisper_hip.sh` を実行します。
ビルド → **~7 秒 (4.5 倍)**。 run_jarvis.sh は、ROCm ランタイムと WSL dxg を自動的にロードします。

## ベンチマーク — NeoGraph 対 LangGraph (`bench/`)

同一のトポロジをミラーリング (マイク→stt→マージ→メモリ→ルーター→4-way→シンセ/スキップ→コミット→tts)
LangGraph (Python ツイン `langgraph_twin.py`) では、同一の制約で測定します
(`--cpus=2 --memory=2g`) コンテナ。

```bash
GROQ_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + groq 20 turns × both
```

## 実施状況

**完全に機能** — 検証済みのライブ音声シングルターンは実際のハードウェア (実際の LLM Groq) で実行されます。
マイク→VAD→STT→ルーター→4ウェイ→シンセ→TTSフルチェーン+メモリ永続化+A2Aセルフサーバー。

既知の制限事項 / 次のバージョン:
- **バージインはサポートされていません** — TTS 再生中の発話はバックプレッシャーによって破棄されます
  (v2 ではキャンセル トークンが追加されます)。
- **ストリーミング STT は適用されません** — 発話完了後のバッチ文字起こし。ムーンシャイン v2
  エルゴディック エンコーダのチャンクごとのストリーミングが次の候補です。
- **マルチ スピーカー · ロングメモリ圧縮** — シングル スピーカーを想定、24 ターン制限。
- **バックグラウンド トリガー (プロアクティブ)** — 設計されていますが、実装されていません。

## ライセンス/外部依存関係

|図書館 |ライセンス |役割 |
|---|---|---|
| [supertonic](https://github.com/supertone-inc/supertonic) |マサチューセッツ工科大学 | TTS (99M、ONNX、31 言語) |
| [whisper.cpp](https://github.com/ggerganov/whisper.cpp) |マサチューセッツ工科大学 | STT (99 言語自動検出、CPU/ROCm) |
| [Moonshine](https://github.com/moonshine-ai/moonshine) |マサチューセッツ工科大学 |エッジ STT オプション (27M ONNX) |
| [miniaudio](https://github.com/mackron/miniaudio) | MIT-0 / パブリックドメイン |マイクキャプチャ + スピーカー再生 |
| [Silero VAD](https://github.com/snakers4/silero-vad) |マサチューセッツ工科大学 |音声開始/終了検出 (ONNX) |
| ONNX ランタイム |マサチューセッツ工科大学 |スーパートニック・密造酒・VAD推論 |
