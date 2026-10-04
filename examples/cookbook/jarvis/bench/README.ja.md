<!-- neograph-i18n: source=examples/cookbook/jarvis/bench/README.md locale=ja source_sha256=3985b561c728357e75ec0cab68d2711e19334c47a588c07f2b836c6ce3fa8eb3 -->
# JARVIS オーケストレーションベンチマーク — NeoGraph vs LangGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

## 型付きプロバイダー移行 — ソースの状態

C++ルーター・合成器・専門家フィクスチャは型付き `ProviderRequest`、`sp::Message`、`sp::Event` と完全な不変 `sp::Outcome` (`sp::runtime::Result`) を使用し、旧文字列応答APIではない。`src/provider_support.h` のJarvis/coder/researcher mockは固定ルーターJSON、ユーザーテキストecho、明示的な架空の研究応答を返す。キーやネットワーク提供者は不要だが、本物の研究・推論ではない。以下の既存設定/プロファイルのパスを、この文書変更で作成・修正することはない。

ローカル音声は任意で、選択したwhisper/Moonshineモデル、ONNX Runtime/Supertonic資産、miniaudio、利用可能なマイク・スピーカーが必要。テキスト/mock動作は音声動作の証拠ではない。クラウド不要はローカル/mockのみ。ライブには承認された `OPENROUTER_API_KEY`、ネットワーク・提供者容量が必要で、プロンプト・会話メモリ・添付ツール/委譲結果をOpenRouterへ送信する。モデルは固定され、ネイティブ要求のZDRは地域内常駐保証ではない。キーをログ・リポジトリへ入れない。nullableトークン使用量は請求額ではなく、費用には現行のエンドポイント/モデル価格と実際の請求対象使用量が必要。

`[jarvis:ttft]` は最初の非空 `sp::PartDelta` の `PartKind::Text`・`DeltaChannel::Content` で発生し、使用量・推論・ヘッダーイベントでは発生しない。最初の合成テキストであり、実際のTTS音声開始ではない。Python REPL driver は protocol client です。pybind benchmark は移行済み型付き binding を使い、別の実行証拠が必要です。現在の[Jarvis CLI実行証拠](../README.md)は挨拶、永続化した合成メモリturn、正常なEOF終了のみで、このベンチroundの証拠ではない。以下のベンチ時間・実行主張は過去の記録。CLI実行はマイク・ASR・TTS・pybindベンチやvendor推論を検証しない。

NeoGraph(C++モックビルド)とLangGraph(Pythonツイン `langgraph_twin.py`)の同一のトポロジー(mic→stt→merge→memory→router→4-way→synth/skip→commit→tts)を反映し、同一制約の`--cpus=2 --memory=2g`コンテナ内で計測します。

```bash
OPENROUTER_API_KEY=... bash bench/run_bench.sh     # mock 200 turns + OpenRouter 20 turns × both
```

## 過去の結果(2026-07-05,pre-OpenRouter移行, Groq)

| メトリック | NeoGraph | LangGraph | Delta |
|---|---|---|---|
| 純グラフオーバーパーヘッド/ターン(モック0ms LLM, 200ターン) | **0.38ms** | 3.07ms | +2.7ms (8.1×) |
| Groq実推論/ターン（8bルーター+70bシンセサイザー、20ターン） | 684ms | 706ms | +22ms（約3%） |
| Groq p99 | 775ms | 870ms | +95ms（n=20、ノイズマージン） |
| コールドスタート | 7.9ms | 716ms | ~90× |
| RSS（モック） | 7.5MB | 68MB | ~9× |

解釈:
- グラフエンジン自体は両側ともLLMに比べて安価である（0.4ms対3ms）。Groqの差+22ms のうち~19ms はHTTP クライアントスタックの差（langchain-openaiのhttpx+pydantic と比較して asio）。
- ターン間ギャップは**成長型**です — 推論が高速になるほど大きくなり、200msターン(Cerebras級 / シングルコールパス)では10%以上、ローカル小規模モデル(約50ms/コール)では20〜30%です。
- この過去の container 設定の startup/RSS 比率は約90×/9×でした。production JARVIS100個の memory capacity を保証しません。

## E2Eラウンド — 実MCPツールラウンドトリップ含む (2026-07-05)

```bash
OPENROUTER_API_KEY=... bash bench/run_bench_e2e.sh
```

共有デモ MCP サーバーコンテナ(time/calc/weather) + 24ターンmixed set(ダイレクトtool call · 並列 fan-out · chatチャット · memory recall)、ABBA順序のインターリーブを各2ラウンドずつ:

| ラウンド（実行順） | mean | p50 | max | 注記 |
|---|---|---|---|---|
| neograph r1 | 810ms | 791 | 1052 |  |
| langgraph r1 | 673ms | 667 | 934 |  |
| langgraph r2 | 1442ms | 1025 | 3830 | 直近7ターン 2.4〜3.8秒 — Groqスロットル窓 |
| neograph r2 | 689ms | 665 | 983 | LG r2の直後に実行しても安定 |

**結論: これらの条件下（韓国→Groq WAN、1ターン約700ms）では、プロバイダ側のばらつき（ラウンド間で±130〜770ms）がフレームワーク差分（モック計測約3ms + HTTPスタック約19ms）を完全に覆い尽くす。** 実行順序を入れ替えると勝者が入れ替わった — E2Eターン遅延ではフレームワークの優位性を判定できず、固定オーバーヘッドと起動/メモリを計測できるのは制御されたモックラウンドのみ。E2E検証済み: 両ハーネスとも実ツールで正しく動作（ルーティングモード一致 21/24、直接/並列の実ラウンドトリップ）、起動74ms vs 1944〜2483ms、RSS 14MB vs 122MB確認。

含意: フレームワーク差分が意味を持つのは、**低ばらつき + 低絶対遅延**（ローカル推論、同一データセンター内推論）の場合のみ — 「高速推論」だけでは不十分。クラウド推論をWAN経由で行うと、フレームワークに関係なくネットワークが支配的になる。

## 境界計測ラウンド — プロバイダ分散の排除（2026-07-05）

```bash
OPENROUTER_API_KEY=... bash bench/run_bench_proxy.sh
```

E2Eの「分散が差分を覆い尽くす」問題をプロキシ境界計測で解決: nginxをGroqの前に配置し**呼び出しごとのアップストリーム（WAN + Groq）時間を記録**し、ターンラウンドトリップから差分を引いた残差（グラフ + HTTPクライアントシリアライゼーション + ローカルMCP + パイプ）のみを比較する。統計的な回避策（ABBA/リトライ回数の増加）ではなく、ノイズ源自体を計測して減算する — ラウンドが異なるGroqウィンドウに当たった場合でも結果は揺れない。

|  | ターン毎の上流送信平均 (Avg/turn upstream) | **残差p50** | 残差p90 | 残差最小〜最大 |
|---|---|---|---|---|
| NeoGraph | 1613ms | **3.5ms** | 19.1ms | 1.9~80.5 |
| LangGraph | 1417ms | **14.7ms** | 25.1ms | 10.8~33.3 |

- 生の壁時計時間では「LGが189ms速い」という結果（GroqがNG側に不利な時間帯を与えた——上流平均+196ms）。残差では**NGがp50で−11.1ms**——ノイズの方向にかかわらず手法がシグナルを復元することを明確に示している。
- 残差p50はモックラウンド予測と一致（グラフ0.4対3.1ms + HTTPスタック差）——ペイロードの相互検証成功。
- 呼び出し↔ターン対応は**順序ベース**（呼び出し数=ターン数×2であること、ログ順=ターン順であることを確認）。時間枠対応にはWSL2壁時計ステップ（実行中の-0.8s反転測定）があり、フォールバックのみ。ドライバのタイムスタンプも単調アンカーから導出。
- 注意点：Groq(Cloudflare)は`Python-urllib` UAを403でブロック——プロキシ問題と誤認しやすい。実際のスモークテストはcurl/httpx系UAを使用。

## ストリーミングTTFTラウンド（2026-07-05）

この過去の round は両 synthesis call を streaming に変えました。当時の C++ streaming provider と LangGraph `SYNTH_LLM.stream()` を使い、現在の C++ は `ProviderMode::Stream`/`sp::Event` です。driver は `[jarvis:ttft]` で turn-send → first synthesis text を測ります。nginx の `proxy_buffering off` は SSE を通し `$upstream_header_time` は first byte。round 別 log(mv + `nginx -s reopen`)で境界を分けます。

|  | 知覚 TTFT p50 | 完了時間 p50 | ターン毎の上流送信平均 (Avg/turn upstream) |
|---|---|---|---|
| NeoGraph | **631ms** | 744ms | 726ms |
| LangGraph | **629ms** | 723ms | 753ms |

- **知覚TTFTは実質的に同程度（差 −2ms）。** 先ほどNeoGraphのTTFTが遅く見えたのは（800 vs 603）、純粋にプロバイダ分散によるものだった — 今回はGroqが両方に公平なウィンドウを与え（上流 726 vs 753）、ギャップが解消された。「NGラウンドは単に不運だった」という推測が再現により確認された。
- **過去の completion residual** — NeoGraph4.1ms/LangGraph14.6ms(以前の proxy3.5/14.7)でした。residual は client serialization、local MCP、pipe overhead を含み、graph computation だけの直接測定ではありません。
- **TTFT残差は±数十msのノイズ内で0である**（負の値も現れる）。知覚されたTTFT 625msと上流合計673msを比較すると、2つの独立したクロック（クライアントのモノトニッククロックとnginxのウォールクロック）を減算する分解能（±50ms）は、フレームワークの寄与（ms）よりも大きい。すなわち、**フレームワークの差はTTFT経路では観測限界以下である** — シグナルがノイズを超えて現れるのは、総残差/モックのみである。
- **Streaming observation** — 過去の first-synthesis-text は631ms、completion は744msでした。`[jarvis:ttft]` は text marker で、最初の audible TTS playback や0.6秒での聴取開始を測りません。

過去の streaming text-marker TTFT は同率でした。この測定は現在の SDK transport、audio latency、production tenant capacity を検証しません。

## 公平な条件欄

- Prompt (Persona.txt 共有済み) · 決定検証（チャットダウングレード） · 記憶形式（JsonFileStore） · 忠実な再現ガード · stdout マーカー同一。フレームワークと言語のみが異なる。
- LangGraph 側はイディオムな技術スタック (LangGraph + LangChain-OpenAI) を用いる.
- 測定はコンテナ内部`driver.py` (標準入力注入 → `[jarvis:tts]` マーカー往復)。

## Files

- `langgraph_twin.py` — LangGraph 双生（同一トポロジー・プロトコル、MCP_URL 設定時は公式 mcp SDK 永続セッション経由の実ツール呼び出し）
- `driver.py` / `analyze.py` — 計測・比較表
- `Dockerfile.neograph` / `Dockerfile.langgraph` / `Dockerfile.mcp` — ベンチマーク画像
- `run_bench.sh`（core）/ `run_bench_e2e.sh`（実ツールE2E） — ランナー
- `turns_mock.txt`（200） / `turns_openrouter.txt`（20） / `turns_e2e.txt`（24） — ターンセット
- `../config-bench/` — 空のカタログ（チャットパス修正）/ `../config-bench-e2e/` — 共有MCPサーバーカタログ
