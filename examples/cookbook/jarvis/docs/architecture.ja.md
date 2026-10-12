<!-- neograph-i18n: source=examples/cookbook/jarvis/docs/architecture.md locale=ja source_sha256=ed7f628ae6ae172e9e2cbd3d681d4880b503f251e10b745e7be089fbcdc42200 -->
# JARVISグラフ — ノードごとの詳細

**Languages:** [English](architecture.md) | [한국어](architecture.ko.md) | [日本語](architecture.ja.md) | [简体中文](architecture.zh-CN.md)

## 型付きプロバイダーへの移行 — ソースの状況

C++のルーター、シンセサイザー、スペシャリストのフィクスチャは、型付きの `ProviderRequest`、`sp::Message`、`sp::Event` を使います。プロバイダーの結果は、従来のレスポンス文字列ではなく、完全な不変の `sp::Outcome` の値（`sp::runtime::Result`）です。`src/provider_support.h` には、ローカルのJarvis／coder／researcherのモックプロファイルが実装されています。ルーターは決定的なJSONを返し、ユーザーのテキストをそのままエコーし、リサーチャーは合成であると明示した返信を返します。これらのフィクスチャに、認証情報もネットワークのプロバイダーも必要ありませんが、実際のリサーチも、エキスパートの推論も提供しません。以下に出てくる既存の設定とプロファイルのパスは、今回のドキュメントの更新では、修復も作成もしていません。

ローカルの音声は任意です。選択したwhisper／Moonshineのモデル、ONNX Runtime／Supertonicのアセット、miniaudio、使えるマイクとスピーカーが必要です。テキストやモックで動くことは、音声が動作する証拠にはなりません。クラウドを使わないと言えるのは、ローカルまたはモックで動かす場合だけです。実際のリクエストには、承認された `OPENROUTER_API_KEY`、ネットワークへのアクセス、プロバイダーの処理能力が必要です。プロンプト、会話のメモリ、添付されたツールや委任の結果が、OpenRouterに送られます。実際に使うモデルは固定されています。ネイティブのリクエストはZDRを設定しますが、データの所在地（residency）を保証するものではありません。キーは、ログやバージョン管理に含めないでください。プロバイダーのトークン使用量は、null値になりうる集計であり、金額の請求ではありません。コストを求めるには、現在のエンドポイントとモデルの料金、および課金対象の使用量が必要です。

`[jarvis:ttft]` が出力されるのは、`PartKind::Text` かつ `DeltaChannel::Content` の `sp::PartDelta` で、空でないものを最初に受け取ったときです。使用量、推論、ヘッダー、その他のイベントでは出力されません。これが測るのは、最初の合成テキストまでの時間で、TTSの音声が最初に再生されるまでの時間ではありません。Python REPLのドライバーは、引き続きプロトコルのクライアントです。pybindのベンチマークは、移行後の型付きバインディングを使いますが、別に実行したエビデンスが必要です。保存してあるC++のエビデンスが対象としているのは、CLIのあいさつ、合成したメモリの永続化、EOFでの正常終了です。以下の音声とライブの所要時間は過去のもので、移行後の経路の適格性確認にはなりません。

`config/jarvis_graph.json` の各ノードが何をするのか、そしてなぜその位置にあるのかを説明します。README.mdの図とあわせて読むのが最適です。

## 1ターンの流れ

```
T0  Microphone active, Tony utterance start detected
T1  Utterance end (VAD detects 500ms silence)
T2  STT complete — text + detected language code
T3  Memory lookup complete — last 6 turns + preferences + last topic
T4  Router decision complete — {mode, tool_calls, delegate_to, skip_synthesis}
T5  4-way branch complete — one of self-response(chat) / direct tool / delegation / parallel done
T6  Response synthesis complete (or skip branch bypassed)
T7  Memory commit complete
T8  TTS first chunk playback starts ← point where Tony "starts hearing" the response
T9  TTS last chunk playback complete, microphone reactivated waiting
```

T0→T8は、JARVISの応答時間としてユーザーが感じる時間です。目標とする分布は、次のとおりです。
- 短い回答（direct＋skip_synthesis）：T0→T8は約1.0〜1.5秒
- 通常（direct＋synth）：T0→T8は約2.0〜3.0秒
- 委任（delegate）：T0→T8は約3.0〜8.0秒（エキスパートの作業時間によって変わる）
- 総合的な回答（parallel＋synth）：T0→T8は約2.5〜4.0秒

## ノードごとの詳細

### mic_capture（`voice_in`）— ライブマイクに対応済み
- デフォルトはstdinモード（テキスト、または `wav:/path`）です。**`use_microphone:true` または環境変数 `JARVIS_MIC=1`** で、ライブのマイク入力が有効になります。
- `miniaudio` のキャプチャデバイスが、16 kHz・モノラル・f32の音声をコールバックにストリーミングし、ミューテックスで保護したバッファにためます。
- VADのワーカースレッドが、512サンプル（32 ms）のウィンドウごとに `Silero VAD`（ONNX）で推論し、音声の確率を求めます。
- `vad_threshold`（0.5）を超えると録音を開始します（途切れを防ぐため、200 msのプリロールを含みます）。500 ms無音が続くと、発話が終わったとみなします。PCMが発話キューに入れられ、run()がvoice_inを受け取ります。
- 250 ms未満のノイズは無視されます。`max_utterance_seconds` を超えると、強制的に終了します。
- **デバイスの初期化に失敗した場合（WSL2のオーディオブリッジがない場合など）は、自動でstdinにフォールバックします。** クラッシュはしません。WSLg／PulseAudioのソースがあれば、WSL2でもライブで動作します。

### stt（`whisper_stt` または `moonshine_stt`）
- ノードが存在する間、1つのwhisper.cppモデルを再利用します（再読み込みのコストはかかりません）。
- `language="auto"` では、最初の30秒（または発話全体）から言語を自動で検出します。
- 出力は、`user_text`（単一の文字列）と `user_lang`（ISOコード）です。
- 認識の信頼度が低すぎる場合は、空の文字列になります。そのターンは、ルーターの段階でスキップされます。

**GPUによる高速化（whisper.cpp ROCm/HIP）**：同梱のwhisper.cppはCPU専用なので、whisper-large-v3-turboは、CPUで約32秒かかります（jfkでは11秒）。ライブでの利用には向きません。AMD GPU（gfx1201＝R9700、ROCm 7.2以上）では、`bash scripts/build_whisper_hip.sh` を実行してGGML_HIPビルドを作り、whisper_installを置き換えると、**約7秒（4.5倍）**になります。run_jarvis.shは、ROCmランタイムとWSLのdxgブリッジ（HSA_ENABLE_DXG_DETECTION）を自動で読み込みます。GPUがあれば、設定をlargeのままリアルタイムで使えます。GPUがない場合は、whisper-small（CPUで約8秒）に切り替えます。

**代替の選択肢 `moonshine_stt`**（Moonshine-tiny ONNX）：27Mの超軽量モデルで、入力はメルスペクトログラムではなく、16 kHzの生の波形です。構成はseq2seq（エンコーダー、2つに分かれたデコーダーモデル、KVキャッシュ）です。supertonicのTTSとONNX Runtimeを共有します。言語ごとのモデルなので、`user_lang` は設定で固定します（tiny-koなら "ko"）。トークナイザーは、tokenizer.jsonのSentencePiece BPEから直接デコードします（▁→スペース、ByteFallback、Fuse）。設定のstt.typeを切り替えるだけで差し替えられます。Pythonのoptimumのリファレンスとは、55トークンの文字レベルで一致することを確認済みです。int8（約28 MB）には、フルビルドのORTが必要です（同梱の縮小ビルドにはConvIntegerがありません）。そのため、デフォルトはfp32（約183 MB）です。

### text_or_voice（`channel_merge`）
- voice_in（STTを通ったもの）と、text_in（外部のA2A）のうち、有効なほうの経路を選びます。
- 両方が空なら、空のターンになります。グラフは1サイクルをそのまま通過します。
- 外部のA2A呼び出しにも、`user_lang` を含める必要があります（ない場合は "en" とみなします）。

### memory_lookup（`memory_lookup`）
- NeoGraphの `Store` の `jarvis.tony` 名前空間から読み取ります。
- 直近N回分のターン、設定値、last_topicをまとめて、1つの `memory_context` にして渡します。
- コストはかかりません。常に、グラフの開始時に実行されます。

### router（`intent_classifier`）
- OpenRouter経由で、固定したDeepSeekのモデルを使います（約200〜400 ms）。
- システムプロンプトは、persona.txtの[router]に、MCPカタログのテキストとエージェントレジストリのテキストを加えたものです。
- 出力のJSONを検証します。パースに失敗した場合はフォールバックします（mode=chat）。ツール名とエージェント名は、カタログとレジストリに照らして確認し、実在しなければchatに降格します（LLMが作り出した `delegate_to:"null"` が下流に流れるのを防ぎます）。
- chatモードは、ツールも委任も使わず、response_synthに直接進みます。あいさつ、自己紹介、会話の振り返りに使います。

### direct_branch（`tool_dispatch`）
- `route_decision.tool_calls[0]` を1回ディスパッチします。
- 結果を、`tool_results` チャネルに追加します。
- skip_synthesis=trueの場合は、次のノードを飛ばして、直接TTSに進みます。

### parallel_branch（`parallel_tool_fanout`）
- `route_decision.tool_calls` をすべて同時に実行します（`make_parallel_group`）。
- 上限は `max_concurrent` で指定します（デフォルトは4）。
- すべての結果を、順番に `tool_results` に追加します。リデューサーが、シンセサイザー向けにそれを使います。

### delegate_branch（`a2a_delegate`）
- `route_decision.delegate_to` が指すA2Aエンドポイントに、user_textを送ります。
- `timeout_seconds` を超えると、エラーのレスポンスを返します（JARVISは、音声で「エキスパートが応答していません」と伝えます）。
- レスポンスから `[SUMMARY]` の行を最初に取り出し、`delegated_reply` に保存します。

### response_synth（`llm_call`）
- OpenRouter経由で、固定したDeepSeekのモデルを使います（約800〜1500 ms）。
- システムプロンプトは、persona.txtの[synth]に、言語の指示とセッション境界のコメントを加えたものです。
- 会話履歴（memory_context.recent_turns）は、**userとassistantのロールのターンからなるメッセージ配列として渡します**。以前は、履歴をユーザーメッセージの中にインラインのJSONとして入れていたため、モデルが過去の回答をそのままコンテンツとして扱ってしまいました（メモリのオウム返し）。
- 現在のターンのユーザーメッセージは、user_textに、tool_resultsやdelegated_replyを付けたものです。
- verbatimガード：出力をtrimしたものが、過去の回答と完全に一致した場合は、1回だけ再生成します。
- 出力は `final_text`（TTSが読み上げる文字列）です。
- skip_synthesis=trueの経路では、このノードを通りません（この位置にはsynth_skipが入ります）。

### synth_skip（`passthrough`）
- tool_resultsの最後の項目（通常は、ツールの生のレスポンス）を、そのままfinal_textにコピーします。
- 例：Timeツールが「午後3:30」を返したら、そのまま音声にします。
- JARVISの応答が速い理由です。約1秒かかる大きなLLM呼び出しを、まるごと省けます。

### memory_commit（`memory_commit`）
- このターンのuser_text、final_text、使ったツール名を、Storeのturnsに追加します。
- 次のターンのmemory_lookupが、これを取り出して、ルーターのコンテキストに使います。
- TTSと並行して、非同期に処理することもできますが、現在は直列です。

### tts（`supertonic_tts`）
- supertonicが、final_textとuser_langから推論して、44.1 kHzのPCMを作ります。
- miniaudioのスピーカー再生を開始します。最初のチャンクが鳴るまで、約100〜300 msかかります。
- バージイン（割り込み発話）は実装されていません。再生中のマイク入力は破棄されます。

## グラフの外側 — バックグラウンドトリガーとA2Aサーバー

メイングラフが処理するのは、1サイクルにつき1回の発話と応答です。A2Aサーバーは実装済みです。以下のバックグラウンドトリガーは、未実装の設計の説明です。

### バックグラウンドトリガーのグラフ
- 別の `GraphEngine`（または、単なるstd::thread）を使います。
- タイマーや外部のイベントを監視します。
- イベントが起きたら、JARVISのメイングラフの `text_in` チャネルにメッセージを入れます。
- JARVISは、Tonyが尋ねる前に話しかけます（「サー、10分後に会議があります」）。

### A2Aサーバー（JARVISを外部に公開する）
- `agent_registry.json` の `self` セクションに基づきます。
- 同じエンジンを `GraphAgentAdapter` で包んで公開します（サンプル38のパターン）。
- 外部からのテキスト入力は、STTの段階を飛ばして、`text_in` からルーターに直接入ります。
- 応答は、テキストで返すこともできますし、同時にローカルのTTSで再生することもできます。

## 既知の制限と次のバージョン

- **バージインには未対応** — TTSの再生中は、マイク入力が無視されます。v2で、キャンセルトークンを追加する予定です。
- **複数の話者には未対応** — 話者は1人であるとみなします。話者を分離するには、別のノードが必要です（たとえばpyannote）。
- **長期メモリの圧縮** — 検索のデフォルトは直近6ターンで、コミットでは最新の24ターンを保持します。それより古いターンは、要約されずに破棄されます。
- **カタログのホットリロード** — カタログは起動時に読み込みます。編集したら再起動してください。自動での再読み込みは実装されていません。
