<!-- neograph-i18n: source=benchmarks/dr_compare/README.md locale=ja source_sha256=d24db116d5f5f587178a373b78a2b9b6b492ad07dcb0c4dfc29288abad0fd636 -->
# dr_compare: ディープリサーチのオーケストレーション比較

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

ランナーはルーター → 計画 → 研究者Send分岐 → 統合を同じプロンプトとモデル選択で実装します。エンジン、バインディング、クライアント、チェックポイント実装が異なるため、総時間はエンジンや転送だけを分離しません。以下の2026年4月の観測は過去の証拠で、現在のcutoverの再実行ではありません。

## ファイルと依存関係

`dr_neograph.py`, `dr_langgraph.py`, `bench.py`, `bench_mock.py`, `mem_probe.py`, `mem_prod_stack.py`, `sweep.sh`, `_run_single.py`は実呼び出し、プレーンテキストの模擬処理、メモリ観測、sweep、単発診断を扱います。[Pythonバインディング案内](../../docs/python-binding.md)に従い現在のソースに合うwheelを入れてください。`CompletionParams`/`OpenAIProvider`を持つ古いwheelは現在のAPIではありません。Coreのソースビルドにも外部SchemaProvider SDKが必要です。
現在のビルドには SDK `0.3.0`、interface revision/shared generation 6、マージ済み SchemaProvider PR #21（`83112573ba59e3b561fc33c22394638be7aa5294`）と一致する wheel/native build が必要です。現在の統合検証は未完了です。この比較 runner は組み込み Deep Research の回復経路とは別です。

ワークフローはrequests、LangGraph、langchain-openaiを読み込み、メモリ観測はpsutilを使います。PostgreSQLには対応するチェックポイントパッケージとDBが必要です。`mem_prod_stack.py`は各スタックのWeb/DB/観測パッケージも読み込むため、エンジンのみのRSS測定ではありません。

## 現在の環境設定

| Variable | Default | Purpose |
|---|---|---|
| `LLM_MOCK_MS` | `-1` | 負数は実呼び出し、>=0はsleep付きテキスト処理でProvider/outcomeなし。 |
| `MOCK_SEARCH` | `0` | 1はCrawl4AIを省き固定証拠を返します。 |
| `FANOUT` | `5` | 研究者分岐数/上限。 |
| `USE_INMEMORY_CP` | `0` | 1はメモリ内保存、模擬モードもメモリを選択。 |
| `NG_TRANSPORT` | `http-chat` | NG: http-chatかhttp-responses。WebSocketなし、Responsesは別API。 |
| `NG_WORKER_COUNT` | `4` | NG fan-outワーカー数。 |
| `DR_MODEL` | `gpt-5.4-mini` | 両側の実呼び出しで明示するモデル。 |
| `NEOGRAPH_PG_DSN` | `empty` | NG PostgreSQL DSN、空ならメモリ内。 |
| `LANGGRAPH_PG_DSN` | `NEOGRAPH_PG_DSN` | LG PostgreSQL DSNの上書き。 |
| `CRAWL4AI_URL` | `empty` | 検索サービス、空なら模擬以外で検索不可。 |

`OPENAI_API_BASE`は実呼び出しの承認済みorigin/gateway prefixを選び、NGの`NG_PROVIDER_DESCRIPTOR`は完全なdescriptorを指定できます。認証情報とCAはdescriptorではなく実行時オプション（`OPENAI_API_KEY`、`NG_EXAMPLE_CA_FILE`）に置きます。NGの既定HTTP ChatはLGのChat APIに合わせます。HTTP Responsesは異なるAPIの比較です。HTTP/2はlibcurlと相手に依存し、このランナーは多重化や固定の接続数を証明しません。

## 過去の観測: 2026-04-26

1. 模擬LLM、FANOUT=5の中央値はNG 1.0 ms、LG 5.9 msでした（その処理の比率5.9×）。
2. 最初のリモートモデル実行はNG p50 23.90 s（sd 5.90 s）、LG 21.95 s（sd 1.23 s）でした。
3. 過去の接続診断はモデル7呼び出しのNG実行で`connect()`を21回記録しました。その数だけでTLSセッション数、HTTP版、ペイロード同等性は証明できません。
4. 過去の`6da4810` / `bc2ab4f`のプール変更に関連する記録はNG p90 35.34 s → 25.28 s、sd 5.90 s → 1.28 sです。当時のプロバイダー実装は削除され、現在のtyped SchemaProviderは外部SDK/libcurlを使います。
5. FANOUT=50、LLM_MOCK_MS=100、NG_WORKER_COUNT=50の実験はNG 307 ms、LG asyncio 711 msを報告しました。別の処理であり、一般的なサーバー能力ではありません。

NeoGraphにHTTP/2対応がまだ必要という過去の主張は現状に合いません。この記録は新SDK、現在のwheel、他の環境、リモート推論の高速化を検証しません。模擬モードはオーケストレーションと指定sleepだけを測り、プロバイダー証拠を作りません。実モードはtyped所有outcomeから表示テキストを取り出しますが、native replay、portable履歴export、プロバイダー報告量と予算課金量の精算を測定しません。

## 新しい測定の実行

以下の模擬コマンドはリモート呼び出しと永続化を避けます。新記録にはソース/SDK/wheelリビジョン、Python/依存版、ホスト制限、ワーカー数、ウォームアップ、反復数、チェックポイントモード、失敗数を添えてください。過去のファイルを変えないでください。
両 harness は空のレポートを拒否し、ウォームアップまたは測定が一つでも失敗すれば nonzero で終了します。時間統計には成功サンプルだけを含めます。合格と報告する前に `Failed runs` が 0 であることを確認してください。リポジトリー root で `python -m unittest discover -s benchmarks/dr_compare -p test_bench.py` を実行すると、プロバイダーなしの集計回帰テストを実行できます。

```sh
# Install a current-cutover wheel using the Python binding build guide first.
python -m pip install requests langgraph langchain-openai psutil
cd benchmarks/dr_compare
env -u NG_WORKER_COUNT LLM_MOCK_MS=0 MOCK_SEARCH=1 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench_mock.py --warmup 5 --iters 50
```

fan-out の重複を観測するには `LLM_MOCK_MS=100` で再実行し、`FANOUT=5`、ウォームアップ、反復数、チェックポイント設定を同一にして、unset `NG_WORKER_COUNT`（既定 4）と明示 `NG_WORKER_COUNT=1` を比較してください。stderr と失敗を記録してください。直列 fan-out 警告がないことだけでは重複の証拠になりません。モデルなしの処理であり、有料プロバイダーの証拠ではありません。

次のリモートモデルコマンドは料金を発生させる場合があります。認証を意図的に設定してください。メモリ内チェックポイントを使い、PostgreSQL比較には同等のDSN、パッケージ、別記の耐久性範囲が必要です。システムコールやパケット記録だけでは意味的同等性や請求を証明しません。

```sh
# From benchmarks/dr_compare; hosted calls require explicit credentials.
: "${OPENAI_API_KEY:?Set a hosted key only if you intend paid calls}"
: "${CRAWL4AI_URL:?Set a running Crawl4AI service}"
LLM_MOCK_MS=-1 MOCK_SEARCH=0 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench.py --warmup 2 --iters 5
```

このコマンドは支出上限ではありません。`FANOUT=5` の調査クエリーは片側最大 7 論理モデル呼び出し（plan、研究者 5、synthesis）を実行します。両側でウォームアップ 2 回と測定 5 回なら client retry 前でも 98 論理呼び出しを送信できます。承認された制限付き検証では全 SDK/LangChain retry を含む呼び出し数と設定された最大出力許容量の合計をベンチマーク外部で予約してください。NG helper の既定 `NG_EXAMPLE_MAX_TOKENS=1600` は LangChain の出力上限を設定しません。ホストが明示承認したモデル、endpoint、認証、検索サービスのみ使用してください。縮小した代表 cohort は過去の複数反復測定とは異なるため別記してください。
