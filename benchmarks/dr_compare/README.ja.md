<!-- neograph-i18n: source=benchmarks/dr_compare/README.md locale=ja source_sha256=54863904c664b90c0e13360fef870ad45a396a3150352de4af85f1084ffff9e6 -->
# dr_compare — NeoGraph と LangGraph のディープリサーチベンチ

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

同じディープリサーチワークフローの 2 つの実装 (ルーター → 計画 →
送信 → 合成経由で 5 人の研究者をファンアウトし、エンジンごとに 1 人ずつ追加します。同じ
プロンプト、同じモデル、同じ Crawl4AI 検索、同じ Postgres チェックポイント
バックエンド (またはメモリ内)。違いはエンジンとそのエンジンに分離されます
HTTP トランスポート。

## ファイル

- `dr_neograph.py` — NeoGraph ランナー。環境駆動のノブ (以下を参照)。
- `dr_langgraph.py` — LangGraph と同等。 `def` ノードの同期 + 同期
  `app.invoke()` は `dr_neograph.py` とパリティです。
- `bench.py` — 本物の LLM ハーネス。ウォームアップ + 交互小節 +
  パーセンタイル。
- `bench_mock.py` — モック化された LLM を備えたエンジン スループット ハーネス。モジュール
  一度プリロードされると、反復処理はコンパイルされたエンジンを再利用します。
- `mem_probe.py` — ワーカーのスケーリングと同時ファンアウト RSS の比較。
- `mem_prod_stack.py` — 実稼働スタックのメモリ比較。
- `sweep.sh` — `(FANOUT, LLM_MOCK_MS)` 全体で `bench_mock.py` を実行します
  亜種。
- `_run_single.py` — 一発勝負のランナー。ワイヤー/配線プローブに使用されます。

メモリ プローブには [psutil](https://github.com/giampaolo/psutil) が必要です。
プロジェクトの文書に従ってインストールされます。

```sh
python -m pip install psutil
```

## 環境ノブ

|ヴァール |デフォルト |目的 |
|---|---|---|
| `LLM_MOCK_MS` | -1 (実数) | LLM を `time.sleep(MS)` に置き換えます。 >=0 はモックを有効にします。 |
| `MOCK_SEARCH` | "0" | Crawl4AI をスキップします。缶詰の証拠を返却します。 |
| `FANOUT` | 5 |研究者が送信した数。 |
| `USE_INMEMORY_CP` | "0" |メモリ内チェックポイントを使用します (PG_DSN を無視します)。 |
| `NG_TRANSPORT` | `ws-responses` | NG のみ: `ws-responses` (WebSocket レスポンス) または `http-chat` (`/v1/chat/completions`)。 |
| `NG_WORKER_COUNT` | 「4」 | NG のみ: 送信ファンアウト並列処理用のスレッド プール。 |

## 調査結果 (2026-04-26)

1. **純粋なエンジン スループット (模擬 LLM、FANOUT=5)** — NeoGraph 1.0ms
   中央値対 LangGraph 5.9 ミリ秒。 NG は、LLM コストゼロで **5.9 倍高速**です。
2. **実際の LLM ベンチ** — 第 1 ラウンドは NG p50 23.90 秒 (SD 5.90)、LG
   21.95秒（SD 1.23）。 LG は最大 10% 速く見えました。
3. **ワイヤ診断** — WSL2 の pcap が嘘をついていました (BPF は、経由でほとんどのパケットをドロップします)
   HyperV vswitch)。 `strace -e trace=connect` は 21 を行う NG を示しました
   7-LLM 呼び出しの実行ごとに connect() システムコール - 毎回新しい TCP+TLS。
4. **根本原因** — `SchemaProvider::complete_async` は無料の
   の代わりに `async::async_post()` (呼び出しごとにソケットを閉じる)
   既存の `async::ConnPool` (HTTP/1.1 キープアライブ)。
   `run_sync` の呼び出しごとの使い捨て io_context により、明らかな「プール」が作成されました
   プロバイダー内での配線は安全ではありませんが、長年にわたる背景
   プロバイダーが所有する io_context が機能します。
5. **修正 (コミット 6da4810 / bc2ab4f)** — SchemaProvider + OpenAIProvider
   独自の io_context + ワーカー スレッド + ConnPool を保持するようになりました。後
   修正、NG p90 は 35.34 秒 → 25.28 秒 (-10 秒) 低下、SD 5.90 → 1.28
   (4.6 倍の安定性)。並列送信ファンアウトのため、中央値は変更されません
   HTTP/1.1 では N 個の TCP 接続がまだ必要です。
6. **残りのギャップ** — LG の httpx は HTTP/2 をサポートし、N を多重化します
   単一の TCP 上の並列ストリーム。この差を埋めるにはNGが必要
   HTTP/2 クライアントのサポートを追加します (httplib は HTTP/1.1 のみです)。
7. **ワーカー プールの上限** — `set_worker_count(N)` が Python ノードをキャップします
   ファンアウト同時実行。ベンチコードの`set_worker_count(4)`は本物だった
   シーリング; `NG_WORKER_COUNT=50` は LG asyncio よりも先に NG 同期を反転します
   (FANOUT=50、LLM=100ms で 307ms 対 711ms)。

`feedback_schema_provider_no_pool.md` および
`feedback_pybind_worker_ceiling.md` のクロード メモリ全体
物語。

## 再生中

実際の LLM ベンチ:
```sh
set -a && source ../../.env && set +a
export NEOGRAPH_PG_DSN="postgresql://postgres:test@localhost:5433/neograph"
export CRAWL4AI_URL="http://localhost:11235"
export NG_TRANSPORT=http-chat   # apples-to-apples vs LG (both HTTP)
python bench.py --warmup 2 --iters 5
```

エンジンスループットスイープ:
```sh
./sweep.sh   # writes /tmp/sweep.log
```

配線診断 (pcap に疑問がある場合、strace がグランド トゥルースです):
```sh
strace -f -e trace=connect -o /tmp/ng.log \
    python _run_single.py neograph
grep "connect(" /tmp/ng.log | grep -oE 'sin_addr=inet_addr\("[^"]+"\)' \
    | sort | uniq -c
```
