<!-- neograph-i18n: source=examples/cookbook/jarvis/docs/architecture.md locale=ja source_sha256=0682c23a88e18a9bc1094429a0ccfbc9d49d84c279355eed29ce28f068f1da42 -->
# JARVIS グラフ — ノードごとの詳細

**Languages:** [English](architecture.md) | [한국어](architecture.ko.md) | [日本語](architecture.ja.md) | [简体中文](architecture.zh-CN.md)

`config/jarvis_graph.json` の各ノードの機能とその位置にある理由。
README.md の図と合わせて読むのが最適です。

## 1ターンの寿命

```
T0  Microphone active, Tony utterance start detected
T1  Utterance end (VAD detects 200ms silence)
T2  STT complete — text + detected language code
T3  Memory lookup complete — last 6 turns + preferences + last topic
T4  Router decision complete — {mode, tool_calls, delegate_to, skip_synthesis}
T5  4-way branch complete — one of self-response(chat) / direct tool / delegation / parallel done
T6  Response synthesis complete (or skip branch bypassed)
T7  Memory commit complete
T8  TTS first chunk playback starts ← point where Tony "starts hearing" the response
T9  TTS last chunk playback complete, microphone reactivated waiting
```

T0→T8 は、JARVIS の認識された応答時間です。ターゲット配布:
- 短い答え (直接 + スキップ合成): T0→T8 ≈ 1.0-1.5s
- 通常（ダイレクト＋シンセ）：T0→T8 ≒ 2.0～3.0s
- 委任（デリゲート）：T0→T8 ≈ 3.0-8.0s（専門家の作業時間に応じて）
- 総合 (パラレル + シンセ): T0→T8 ≈ 2.5-4.0s

## ノードごとの詳細

### mic_capture (`voice_in`) — ライブマイクの実装
- デフォルトは標準入力モード (テキスト / `wav:/path`) です。 **`use_microphone:true` または環境
  `JARVIS_MIC=1`** はライブマイクキャプチャを有効にします。
- `miniaudio` キャプチャ デバイスは 16kHz モノラル f32 をコールバック → ミューテックス バッファにストリーミングします
- VAD ワーカー スレッドが 512 サンプル (32 ミリ秒) ウィンドウで `Silero VAD`(ONNX) 推論を実行 → 音声エラー
- `vad_threshold`(0.5) を超えると、録画が開始されます (カットオフを避けるために 200ms プリロール)。
  500ms の連続した無音により発話が終了 → PCM が発話キューにプッシュ → run() が voice_in を取得
- 250ms 未満のノイズは無視され、`max_utterance_seconds` を超えた場合は強制終了
- **デバイスの初期化失敗時の自動 stdin フォールバック (WSL2 オーディオ ブリッジの欠落など)** —
  クラッシュはありません。 WSLg/PulseAudio ソースが利用可能な場合は、WSL2 上でライブで動作します。

### stt (`whisper_stt` または `moonshine_stt`)
- ノードの存続期間中、単一の whisper.cpp モデルを再利用します (リロード コストなし ×)
- `language="auto"` は最初の 30 秒 (または発話全体) を自動検出します
- 出力: `user_text` (単一文字列)、`user_lang` (ISO コード)
- 認識の信頼度が低すぎる場合、空の文字列 - ルーター段階でスキップされます

**GPU アクセラレーション (whisper.cpp ROCm/HIP)**: 同梱の whisper.cpp は CPU 専用であるため、
Whisper-large-v3-turbo は CPU で最大 32 秒 (jfk 11 秒) かかります。ライブには適していません。 AMD GPU
(gfx1201=R9700、ROCm≥7.2) GGML_HIP に対して `bash scripts/build_whisper_hip.sh` を実行します。
build → whisper_install を置き換え → **~7s (4.5x)**。 run_jarvis.sh は ROCm を自動的にロードします
ランタイムと WSL dxg ブリッジ (HSA_ENABLE_DXG_DETECTION)。 GPU を使用すると、リアルタイムで構成を大きく保つことができます。
なしの場合は、ウィスパースモール (CPU ~8s) に切り替えます。

**代替オプション `moonshine_stt`** (密造酒のような ONNX): 27M 超軽量、生 16kHz
波形入力(メルではありません)、seq2seq(エンコーダ+2モデル分離デコーダ+KVキャッシュ)。株式
スーパートニック TTS を使用した ONNX ランタイム。言語固有のフレーバー モデルにより、`user_lang` の設定が修正されました
(tiny-ko = 「子」)。トークナイザーは tokenizer.json の SentencePiece BPE から直接デコードします
( →スペース + ByteFallback + ヒューズ)。 config stt.type によるスワップは機能し、
Python 最適リファレンスでは、55 トークンの文字レベルのパリティが検証されました。 int8(~28MB) には完全な ORT が必要です
ビルド (バンドルされた縮小ビルドには ConvInteger は含まれません) → デフォルトの fp32(~183MB)。

### テキストまたは音声 (`channel_merge`)
- voice_in (STT 通過) と text_in (外部 A2A) の間のアクティブ パスを選択します。
- 両方が空の場合は空のターン — グラフは 1 サイクルを通過します
- 外部 A2A 呼び出しには `user_lang` も含める必要があります (欠落している場合は「en」を想定します)

### メモリルックアップ (`memory_lookup`)
- NeoGraph `Store` `jarvis.tony` 名前空間からの読み取り
- 最後の N ターン + prefs + last_topic を 1 つの `memory_context` プッシュに結合します
- コスト ×、常にグラフの開始時に実行されます

### ルーター (`intent_classifier`)
- 固定 DeepSeek モデル (OpenRouter、約 200 ～ 400 ミリ秒)
- システム プロンプト = persona.txt [ルーター] + MCP カタログ テキスト + エージェント レジストリ テキスト
- 出力 JSON 検証: 解析失敗 → フォールバック (mode=chat)。ツール/エージェント名
  カタログ・レジストリに対して検証されます。本物でない場合は、チャットに降格されます (LLM が発明したものを防ぎます)
  下流からの `delegate_to:"null"`)。
- チャット モードは、ツール/委任なしで、response_synth に直接移行します — 挨拶の場合、
  自己紹介、会話の思い出

### 直接ブランチ (`tool_dispatch`)
- `route_decision.tool_calls[0]`を1回ディスパッチします
- 結果を`tool_results`チャネルに追加します
- Skip_Synthetic=true の場合、次のノードを TTS に直接バイパスします。

### 並列ブランチ (`parallel_tool_fanout`)
- すべての `route_decision.tool_calls` を同時に実行 (`make_parallel_group`)
- `max_concurrent` による上限 (デフォルトは 4)
- すべての結果を順番に `tool_results` に追加します → リデューサーはシンセサイザーに使用します

### デリゲートブランチ (`a2a_delegate`)
- user_text を `route_decision.delegate_to` が指す A2A エンドポイントに送信します
- `timeout_seconds` を超えた場合はエラー応答 (JARVIS は「専門家が応答していません」と音声で伝えます)
- レスポンスから最初に`[SUMMARY]`行を抽出 → `delegated_reply`に保存

### 応答_synth (`llm_call`)
- 固定 DeepSeek モデル (OpenRouter、約 800 ～ 1500 ミリ秒)
- システムプロンプト = persona.txt [synth] (+ 言語指示 + セッション境界コメント)
- 会話履歴 (memory_context.recent_turns) は **ユーザー/アシスタントのメッセージ配列として渡されます
  ロールが ** になる — 以前は、ユーザー メッセージ内のインライン JSON により、モデルが過去の回答を処理していました。
  内容をそのままそのまま（記憶オウム返し）。
- 現在のターン ユーザー メッセージ = user_text + tool_results / delegated_reply が添付されました
- 逐語的ガード: トリム後の出力が過去の回答と正確に一致する場合、一度再生成します
- 出力 = `final_text` (TTS が読み取る文字列)
- Skip_Synthetic=true パスでバイパス (synth_skip がこの場所を占有)

### シンセ_スキップ (`passthrough`)
- tool_results の最後の項目 (通常はツールの生の応答) を、final_text として直接コピーします。
- 例：時間ツールが「午後 3 時 30 分」を返す → 直接音声
- JARVIS 応答速度の秘密兵器 — 約 1 秒の大規模な LLM 呼び出し全体を節約します

### メモリコミット (`memory_commit`)
- このターンの user_text + Final_text + 使用したツール名をストア ターンに追加します
- 次のターン、memory_lookup はルーター コンテキスト用にこれらを取得します。
- 非同期処理可能 (TTS と並列) - 現在はシリアル

### tts (`supertonic_tts`)
- Final_text + user_lang によるスーパートニック推論 → 44.1kHz PCM
- ミニオーディオ スピーカーの再生を開始 → 最初のチャンク ~100 ～ 300 ミリ秒後
- 再生中に voice_in のアクティブ化が検出された場合は、キャンセル トークンでキャンセルします (バージイン)
  - 初期スケルトンはバージインをサポートしていません。 v2で追加予定

## 外側のグラフ — バックグラウンド トリガー / A2A サーバー

JARVIS のメイン グラフは単純な 1 回の発話、1 回の応答のサイクルですが、main.cpp
JARVIS の雰囲気を完成させる 2 つの追加コンポーネントを開始します。

### バックグラウンドトリガーグラフ
- 別の `GraphEngine` (または単に std::thread)
- タイマー/外部イベントを監視します
- イベント発生時にJARVISメインの`text_in`チャネルにメッセージを挿入します。
- トニーが尋ねる前にジャービスが話す（「先生、10分後に集合です。」）

### A2A サーバー (JARVIS を外部に公開)
- `agent_registry.json` の `self` セクションに基づく
- `GraphAgentAdapter` でラップされた同じエンジンを公開します (例 38 パターン)
- 外部テキスト入力は STT ステージをスキップし、`text_in` → ルーターに直接進みます。
- 応答はテキストとして送り返すことも、ローカル TTS 経由で同時に再生することもできます

## 既知の制限事項 / 次のバージョン

- **バージインはサポートされていません** — TTS 再生中はマイク入力が無視されます。 v2ではキャンセルトークンを追加。
- **マルチスピーカーはサポートされていません** — 1 人を想定しています。話者を分離するには別のノード (pyannote など) が必要です。
- **ロングメモリ圧縮** — 会話が長くなるにつれて、ターンは無限に増加します。 #56history_compactionパターンが必要です。
- **カタログのホットリロード** — JSON 変更検出は手動です (SIGHUP など)。 v2 では inotify による自動リロード。
