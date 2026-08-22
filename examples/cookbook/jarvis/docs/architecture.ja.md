<!-- neograph-i18n: source=examples/cookbook/jarvis/docs/architecture.md locale=ja source_sha256=27b6441685a1293819d0813b44b53142b1fa05c36ec9ba960702d5243eb9bf92 -->
# JARVIS Graph — ノード別詳細

**Languages:** [English](architecture.md) | [한국어](architecture.ko.md) | [日本語](architecture.ja.md) | [简体中文](architecture.zh-CN.md)

`config/jarvis_graph.json`内の各ノードが何を行うか、そしてなぜその位置にあるのか。README.mdの図と併せて読むのが最適です。

## 1ターンのライフスパン

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

T0→T8はJARVISの認識応答時間です。目標分布は次のとおりです:
- 短い応答 (直接 + skip_synthesis): T0→T8 ≈ 1.0〜1.5秒
- 通常 (直接 + synth): T0→T8 ≈ 2.0〜3.0秒
- 委任 (delegate): T0→T8 ≈ 3.0〜8.0秒 (エキスパートの作業時間に依存)
- 包括的 (並列 + synth): T0→T8 ≈ 2.5〜4.0秒

## ノード別詳細

### mic_capture (`voice_in`) — ライブマイク実装
- デフォルトはstdinモード (テキスト / `wav:/path`)。**`use_microphone:true`またはenv `JARVIS_MIC=1`** がライブマイクキャプチャを有効化します。
- `miniaudio` キャプチャデバイスは16kHzモノラルf32をコールバック→ミューテックスバッファへストリーミングします
- VADワーカースレッドは`Silero VAD`(ONNX)推論を512サンプル(32ms)ウィンドウで実行→音声確率
- `vad_threshold`(0.5)を超えた場合、録音が開始され（カットオフを防ぐため200msのプリロール）、500msの連続無音で発話を終了判定→PCMがutterance queue
- 250ms未満のノイズは無視。`max_utterance_seconds`を超えた場合、強制終了。
- **デバイス初期化失敗時(WSL2オーディオブリッジなし、など)の自動stdinフォールバック** — クラッシュしません。WSLg/PulseAudioソースが利用可能であればWSL2上でライブ動作します。

### stt (`whisper_stt` または `moonshine_stt`)
- ノードの生存期間中、単一の whisper.cpp モデルを再利用します（再ロードコストなし ×）
- `language="auto"` は最初の30秒（または発話全体）から自動検出します
- 出力: `user_text`（単一文字列）、`user_lang`（ISOコード）
- 認識信頼度が低すぎる場合は空文字列 — ルーター段階でターンはスキップされます

**GPU アクセラレーション（whisper.cpp ROCm/HIP）**: 同梱の whisper.cpp は CPU のみ対応のため、whisper-large-v3-turbo は CPU で約32秒かかります（jfk では11秒）— ライブ用途には不適切です。AMD GPU（gfx1201=R9700、ROCm≥7.2）では `bash scripts/build_whisper_hip.sh` を実行して GGML_HIP ビルドを生成します → whisper_install を置き換え → **約7秒（4.5倍）**。run_jarvis.sh は自動的に ROCm ランタイムと WSL dxg ブリッジ（HSA_ENABLE_DXG_DETECTION）を読み込みます。GPU があれば設定はリアルタイム用に large のままにできます。GPU がない場合は、whisper-small（CPU 約8秒）に切り替えます。

**代替オプション `moonshine_stt`**（Moonshine-tiny ONNX）: 27M の超軽量モデル。raw 16kHz 波形入力（メル特徴量ではない）。seq2seq（エンコーダ + 2モデル分離デコーダ + KVキャッシュ）。supertonic TTS と ONNX Runtime を共有します。言語固有のフレーバーモデルであるため、`user_lang` は設定固定です（tiny-ko = "ko"）。トークナイザーは tokenizer.json 内の SentencePiece BPE から直接デコードします（▁→空白 + ByteFallback + Fuse）。config stt.type で値の切り替えが可能で、Python optimum リファレンスで 55 トークンの文字レベル一致を検証済みです。int8（約28MB）はフル ORT ビルドが必要です（同梱の縮小ビルドは ConvInteger を除外しているため）— デフォルトは fp32（約183MB）。

### text_or_voice (`channel_merge`)
- voice_in（STT経由）と text_in（外部 A2A）の間でアクティブなパスを選択します
- 両方が空の場合は空のターン — グラフは1サイクル通過します
- 外部 A2A 呼び出しには `user_lang` も含める必要があります（"en" が欠落している場合は "en" を想定）

### memory_lookup (`memory_lookup`)
- NeoGraph の `Store` `jarvis.tony` ネームスペースから読み取ります
- 直近の N ターン + プリファレンス + last_topic を単一の `memory_context` プッシュに結合します
- コスト×、常にグラフ開始時に実行されます

### router (`intent_classifier`)
- OpenRouter経由でDeepSeekモデルを固定（約200〜400ms）
- システムプロンプト = persona.txt [router] + MCPカタログテキスト + エージェントレジストリテキスト
- 出力JSON検証：パース失敗 → フォールバック（mode=chat）。ツール・エージェント名はカタログ・レジストリと照合；実在しない場合はchatに降格（LLMが捏造した`delegate_to:"null"`が下流に流れるのを防止）
- Chatモードはツール・委任なしで直接response_synthへ進む — 挨拶、自己紹介、会話の想起などのユースケース向け

### direct_branch（`tool_dispatch`）
- `route_decision.tool_calls[0]`を一度だけディスパッチ
- 結果を`tool_results`チャネルに追記
- skip_synthesis=trueの場合、次のノードをバイパスしてTTSへ直接送る

### parallel_branch（`parallel_tool_fanout`）
- すべての`route_decision.tool_calls`を同時に実行（`make_parallel_group`）
- `max_concurrent`による上限（デフォルト4）
- すべての結果を順番に`tool_results`に追記 → reducerがシンセサイザーに使用

### delegate_branch（`a2a_delegate`）
- user_textを`route_decision.delegate_to`が指すA2Aエンドポイントに送信
- `timeout_seconds`を超過した場合のエラー応答（JARVISが音声で「エキスパートが応答していません」と発話）
- 応答から最初に`[SUMMARY]`行を抽出 → `delegated_reply`に保存

### response_synth (`llm_call`)
- OpenRouter経由でDeepSeekモデルを固定使用（約800〜1500ms）
- システムプロンプト = persona.txt [synth]（言語指示 + セッション境界コメント付き）
- 会話履歴（memory_context.recent_turns）は**ユーザー/アシスタント役割ターンのmessages配列として渡される** — 以前は、ユーザーメッセージ内のインラインJSONにより、モデルが過去の回答をそのままコンテンツとして扱っていた（メモリ鸚鵡）。
- 現在のターンのユーザーメッセージ = ユーザーテキスト + ツール結果 / 委任された返信を結合。
- 逐語ガード: 出力がトリム後に過去の回答と完全一致する場合、一度だけ再生成
- 出力 = `final_text`（TTSが読み上げる文字列）
- skip_synthesis=true パスではバイパス（synth_skipがこの位置を占める）

### synth_skip (`passthrough`)
- tool_resultsの最後の項目（通常はツールの生レスポンス）をfinal_textとして直接コピー
- Example: Time tool returns "3:30 PM" → 直接voice応答
- JARVIS応答速度の秘密兵器 — 約1秒の大規模LLM呼び出しをまるごと節約

### memory_commit (`memory_commit`)
- このターンのuser_text + final_text + 使用ツール名をStore turnsに追記
- 次のターンのmemory_lookupがこれらをルーターコンテキスト用に取得
- TTSとparallelにasynchronous処理可能 - currently seriesで実行。

### tts (`supertonic_tts`)
- 上位音推論：final_text + user_lang → 44.1kHz PCM
- miniaudioのスピーカー再生を開始 → 最初のチャンクは約100〜300ミリ秒後
- Playback中にvoice_inアクティベーションが検出された場合は、cancel token付きでキャンセルする（barge-in）。
  - Initial skeletonはbarge-in非対応対象; v2で追加。

## Graph外部 — バックグラウンドトリガー / A2Aサーバー

JARVIS main graphはシンプルな単一発話・単一応答サイクルですが、main.cppはJARVISの雰囲気を補完する2つの追加コンポーネントを開始しています:

### バックグラウンドトリガーグラフ
- 別の`GraphEngine`(または単に std::thread)
- タイマー / 外部イベントを監視する
- イベント発生時にJARVISメインの`text_in`チャンネルへメッセージを注入する
- JARVISがトニーの質問前に発話する（「サー、10分後に会議です。」）

### A2A. Server (JARVIS を外部に公開)
- `self`の`agent_registry.json`内のセクションに基づく
- :同じエンジンを`GraphAgentAdapter`でラップして公開します（例38のパターン）
- 外部テキスト入力はSTTステージをスキップし、`text_in` → ルーターへ直接進みます
- 応答はテキストとして返送するか、ローカルのTTSで同時に再生することができます。

## 既知の制限 / 次のバージョン

- **Barge-in は非対応** — TTS再生中のマイク入力を無視します。v2でキャンセルトークンを追加します。
- **マルチスピーカーは非対応** — 一人を想定しています。話者分離には別のノード(例:pyannote)が必要です。
- **長いメモリの圧縮** — 会話が長くなるとターンが無限に伸びます。#56 の history_compaction パターンが必要です。
- **カタログのホットリロード** — JSONの変更検出は手動です(SIGHUPなど)。v2ではinotifyによる自動リロードを提供します。
