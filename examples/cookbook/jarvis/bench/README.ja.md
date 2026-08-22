<!-- neograph-i18n: source=examples/cookbook/jarvis/bench/README.md locale=ja source_sha256=9a8d8defc5b23d66cb6abe96f28e5a3dd82b273a8833a472cf355d9f5b836b35 -->
# JARVIS オーケストレーションベンチマーク — NeoGraph vs LangGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

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
- 90× startup · 9× RSSは**固定ギャップ**であり、推論速度とは無関係 — エッジ常時稼働・コールドスタート・マルチテナント（JARVIS 100個 = 1GB未満）に即関連する。

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

現代のLLMサービスはすべてストリーミングを行うため、ベンチマークの一致を取る：両方のsynth呼び出しをストリーミングに変更し（C++ `invoke(p, on_chunk)`、LangGraph `SYNTH_LLM.stream()`）、ドライバは`[jarvis:ttft]`マーカーで**ターン送信 → 最初のsynthトークン**までの時間を計測する。nginxは`proxy_buffering off`でSSEを通過させるため、`$upstream_header_time`が実際の最初のバイトとなる。ラウンド分割の推測を排除するため、ラウンドごとにログを分ける（mv + `nginx -s reopen`）。

|  | 知覚 TTFT p50 | 完了時間 p50 | ターン毎の上流送信平均 (Avg/turn upstream) |
|---|---|---|---|
| NeoGraph | **631ms** | 744ms | 726ms |
| LangGraph | **629ms** | 723ms | 753ms |

- **知覚TTFTは実質的に同程度（差 −2ms）。** 先ほどNeoGraphのTTFTが遅く見えたのは（800 vs 603）、純粋にプロバイダ分散によるものだった — 今回はGroqが両方に公平なウィンドウを与え（上流 726 vs 753）、ギャップが解消された。「NGラウンドは単に不運だった」という推測が再現により確認された。
- **完了時間の残差（純粋なフレームワーク）を再現**: NeoGraph 4.1ms vs LangGraph 14.6ms（前回のプロキシラウンド 3.5 vs 14.7 と一致）。フレームワークのオーバーヘッドという結論は確固たるものだ。
- **TTFT残差は±数十msのノイズ内で0である**（負の値も現れる）。知覚されたTTFT 625msと上流合計673msを比較すると、2つの独立したクロック（クライアントのモノトニッククロックとnginxのウォールクロック）を減算する分解能（±50ms）は、フレームワークの寄与（ms）よりも大きい。すなわち、**フレームワークの差はTTFT経路では観測限界以下である** — シグナルがノイズを超えて現れるのは、総残差/モックのみである。
- **ストリーミングの利点**：知覚TTFT（631）≪ 完了時間（744）— ユーザーは0.6秒で回答を聞き始める。非ストリーミング（完了を待つ）に比べ、知覚速度の向上が確認された。

要約：フレームワークの純粋なパフォーマンスは NeoGraph が有利（トータル残差・mock・再現可能）だが、

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
