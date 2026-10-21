<!-- neograph-i18n: source=benchmarks/dr_compare/README.md locale=ja source_sha256=2b082a63b44c2beb85ce4d89781581abaac211bd192807db4439301f62e36f74 -->
# dr_compare：ディープリサーチのオーケストレーションの比較

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

各ランナーは、ルーター → 計画 → リサーチャーのSend分岐 → 統合という流れを、同じプロンプトとモデルの選択で実装しています。ただし、エンジン、バインディング、クライアント、チェックポイントの実装はそれぞれ違います。そのため、エンドツーエンドの所要時間から、エンジンのコストやトランスポートのコストだけを切り出すことはできません。下の2026年4月の調査結果は過去の記録で、現在のSDKで再実行したものではありません。

## ファイルと依存関係

`dr_neograph.py`、`dr_langgraph.py`、`bench.py`、`bench_mock.py`、`mem_probe.py`、`mem_prod_stack.py`、`sweep.sh`、`_run_single.py` は、実際の呼び出し、プレーンテキストのモックのワークロード、メモリのプローブ、スイープ、単発の診断を扱います。現在のソースに合ったwheelを、[Pythonバインディングガイド](../../docs/python-binding.md)に従ってインストールしてください。`CompletionParams` や `OpenAIProvider` を持つ古いwheelは、現在のAPIではありません。ソースからビルドする場合は、Coreだけでも外部のSchemaProvider SDKが必要です。
現在のビルドには、SDK `0.4.0`（インターフェースのリビジョンと共有世代は7、`cmake/NeoGraphSchemaProvider.cmake` が固定するSchemaProviderのコミット）に合ったwheelまたはネイティブビルドが必要です。
この比較用のランナーは、組み込みのDeep Researchの回復経路とは別のものです。

このワークフローは、requests、LangGraph、langchain-openaiをインポートします。メモリのプローブはpsutilを使います。PostgreSQLモードでは、対応するチェックポイントのパッケージと、起動済みのデータベースも必要です。`mem_prod_stack.py` は、名前を付けたスタックごとに、Web、データベース、オブザーバビリティのパッケージを追加でインポートします。エンジンだけのRSSを測るプローブではありません。

## 現在の環境変数

| 変数 | デフォルト | 用途 |
|---|---|---|
| `LLM_MOCK_MS` | `-1` | 負の値なら実際の呼び出し。0以上なら、スリープを伴うプレーンテキストのノード処理で、Providerも結果オブジェクトも使いません。 |
| `MOCK_SEARCH` | `0` | 1にすると、Crawl4AIを使わず、固定のエビデンスを返します。 |
| `FANOUT` | `5` | リサーチャーの分岐数と上限。 |
| `USE_INMEMORY_CP` | `0` | 1にすると、インメモリのチェックポイントを使います。モックモードでも、インメモリを選びます。 |
| `NG_TRANSPORT` | `http-chat` | NG側：http-chatまたはhttp-responses。WebSocketは使えません。Responsesを選ぶと、ワイヤーのAPIが変わります。 |
| `NG_WORKER_COUNT` | `4` | NG側のfan-outのワーカー数。 |
| `LG_MAX_CONCURRENCY` | `NG_WORKER_COUNT`、なければ `4` | LangGraph側のプールの幅（`invoke` の `max_concurrency`）。既定はNeoGraph側と同じで、`--allow-unmatched-widths` を付けない限り、`bench_mock.py` は幅が異なる実行を拒否する。 |
| `DR_MODEL` | `gpt-5.4-mini` | 両側の実際の呼び出しで使うモデル（明示指定）。 |
| `DR_PLAN_TEMPERATURE` | 未設定 | 両側のプランナーのtemperature。未設定の場合は送りません。`gpt-5.4-mini` などの推論モデルが、この値を拒否するためです。 |
| `NG_EXAMPLE_MAX_TOKENS` | `1600` | NG側とLG側の両方で、呼び出しごとの出力の上限。自動では増やしません。 |
| `NEOGRAPH_PG_DSN` | `empty` | NG側のPostgreSQLのDSN。指定しないと、インメモリに切り替わります。 |
| `LANGGRAPH_PG_DSN` | `NEOGRAPH_PG_DSN` | LG側のPostgreSQLのDSNを上書きします。 |
| `CRAWL4AI_URL` | `empty` | 検索サービス。サービスがなく、モックでもない場合は、検索を使えません。 |

実際の呼び出しでは、`OPENAI_API_BASE` で、許可済みのオリジンまたはゲートウェイのプレフィックスを選びます。NeoGraph側では、`NG_PROVIDER_DESCRIPTOR` で完全なディスクリプターを渡すこともできます。認証情報とカスタムCAは、ディスクリプターのデータではなく、ランタイムのオプション（`OPENAI_API_KEY`、`NG_EXAMPLE_CA_FILE`）に置きます。NG側のデフォルトの経路はHTTP Chatで、LG側のChatのワイヤーAPIに合わせています。HTTP Responsesを選ぶと、意図的に別のAPIを比較することになります。HTTP/2を使えるかどうかは、libcurlと接続先に依存します。これらのランナーは、多重化や接続数が固定であることを保証しません。

## 過去の調査結果：2026-04-26

1. LLMをモックにし、FANOUT=5にしたときの記録上の中央値は、NeoGraphが1.0 ms、LangGraphが5.9 msです（このワークロードでの比は5.9倍）。
2. ホスト型モデルを使った最初のラウンドでは、NGのp50が23.90 s（標準偏差5.90 s）、LGが21.95 s（標準偏差1.23 s）でした。
3. 当時の接続の診断では、モデル呼び出しが7回のNGの実行で、`connect()` のシステムコールが21回記録されました。この回数だけでは、TLSセッションが21回だったことも、HTTPのバージョンも、ペイロードが同等であることも証明できません。
4. コミット `6da4810` / `bc2ab4f` とされる当時のプーリングの変更に関連して、NGのp90は35.34 sから25.28 sに、標準偏差は5.90 sから1.28 sに変わったと記録されています。これらの変更は、すでに削除されたプロバイダー実装についてのものです。現在の型付きSchemaProviderは、外部のSDKとlibcurlのランタイムを使います。
5. ワーカー数を変える実験では、FANOUT=50、LLM_MOCK_MS=100、NG_WORKER_COUNT=50のとき、NGが307 ms、LangGraphが711 msと報告されています。LangGraphのランナーはasyncioではなく、langchain-coreが呼び出しごとに作るスレッドプール上で動く同期の `invoke` であり（以前のラベルは誤りでした）、そのプールは既定の幅 `min(32, CPU数 + 4)` のまま、NeoGraph側のプールだけを50に広げていました。プールが揃っていなかったため、エンジンの比較としては撤回します。以下の再測定ではプールの幅を揃えています。

NeoGraphにはまだHTTP/2のサポートが必要だという過去の記述は、すでに当てはまりません。ここに記録した所要時間は、新しいSDKと現在のPythonのwheelより前に測ったもので、他のプラットフォームには当てはまらず、リモート推論の高速化を予測するものでもありません。モックモードが測るのは、オーケストレーションと設定したスリープだけで、プロバイダーのエビデンスは作られません。実際の呼び出しのモードでは、型付きの所有された結果から、表示用のテキストを取り出します。ただし、ネイティブのリプレイ、ポータブルな履歴のエクスポート、プロバイダーのレポートと予算の課金の精算は、ベンチマークの対象ではありません。

## プールの幅を揃えた再測定

どちらのランナーもスレッドプールでファンアウトします。NeoGraphはワーカープール（`NG_WORKER_COUNT`）を、LangGraphの同期 `invoke` は langchain-core が呼び出しごとに作るプール（`max_concurrency` として渡す `LG_MAX_CONCURRENCY`）を使います。再測定では、すべての構成で両側に同じ幅を与え、モックモデル（呼び出しごとのスリープ）、固定の検索エビデンス、インメモリのチェックポイントを使い、キーもネットワークも使いません。失敗した実行や、幅が揃っていない実行は、そのセルをエラーにして表から除きます。

<!-- bench-record:begin dr_compare -->
| 構成 | ファンアウト | モデル呼び出しあたりの遅延 (ms) | プールの幅、両側とも | NeoGraph、1 実行あたり ms | LangGraph、1 実行あたり ms | LangGraph ÷ NeoGraph |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| A: モデルの遅延なし（エンジンの下限） | 5 | 0 | 4 | 1.39 [1.37, 1.40] | 6.23 [6.11, 6.34] | 4.47× |
| B: リサーチャー 5 つ | 5 | 100 | 4 | 402 [402, 402] | 407 [407, 407] | 1.01× |
| C: リサーチャー 50、プールの幅はファンアウトと同じ | 50 | 100 | 50 | 307 [307, 308] | 329 [328, 329] | 1.07× |
| D: リサーチャー 50、狭いプール | 50 | 100 | 4 | 1,508 [1,508, 1,508] | 1,516 [1,516, 1,516] | 1.01× |

モックモデル（呼び出しごとのスリープ）、固定の検索エビデンス、インメモリのチェックポイント、キーもネットワークもなし; 5 回のインターリーブしたラウンドの中央値と、ラウンド間の [最小, 最大]; 各ラウンドは構成ごとに新しいプロセス 1 つで、片側あたりウォームアップ 3 回と計測 50 (A); 20 (B, C, D) 回; neograph_engine 0.16.0、コンパイラ g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0。両側は同じプールの幅を使います: `NG_WORKER_COUNT` と `LG_MAX_CONCURRENCY`。

ホスト: ARM Neoverse-N1、4 コア、カーネル 6.17.0-1018-oracle、Python 3.12.3、NeoGraph のビルドに使ったコンパイラ g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0; CPU ガバナー: ホストが公開していない。
<!-- bench-record:end dr_compare -->

`session.py` は表の 4 つの構成をインターリーブしたラウンドで、構成ごとに新しい `bench_mock.py` プロセス 1 つで実行し、生のサンプルと来歴を含む記録を書き出します。上の表は、その記録から `scripts/render_bench_report.py` が生成します。

```sh
# Environment: the dr_compare section of benchmarks/requirements-compare.txt (benchmarks/compare/setup_venvs.sh creates it).
# The NeoGraph Python module is built from the source tree (-DNEOGRAPH_BUILD_PYBIND=ON, shared libraries) in $WORK/build-pybind.
python3 benchmarks/dr_compare/session.py --python "$WORK/venvs/dr_compare/bin/python" \
    --pythonpath "$WORK/build-pybind" --build-dir "$WORK/build-pybind" --out dr-record.json --rounds 5 \
    --harness-commit "$(git rev-parse HEAD)" --engine-commit "$(git rev-parse HEAD)"
python3 scripts/render_bench_report.py --dr-record dr-record.json --docs benchmarks/dr_compare/README
```

## 新しいコホートの実行

次のモックのコマンドは、ホスト型のモデル呼び出しと永続化を使いません。新しい結果には、ソース、SDK、wheelのリビジョン、Pythonと依存パッケージのバージョン、ホストの制限、ワーカー数、ウォームアップ、反復回数、チェックポイントのモード、失敗の件数を添えて記録してください。過去のファイルは変更しないでください。
どちらのハーネスも、空のレポートを受け付けません。ウォームアップか測定のどれか1回でも失敗すると、ゼロ以外の終了コードで終了します。所要時間の集計に含まれるのは、成功したサンプルだけです。コホートが合格したと報告する前に、`Failed runs` が0であることを確認してください。リポジトリのルートから `python -m unittest discover -s benchmarks/dr_compare -p test_bench.py` を実行すると、プロバイダーを使わない集計の回帰テストを実行できます。

`bench_mock.py` は、LLM_MOCK_MSが未設定なら `LLM_MOCK_MS=0` を設定します。また、どちらのランナーをインポートする前にも、モックの検索とインメモリのチェックポイントを強制します。遅延が負の値の場合や、モックでないランナーがすでにキャッシュされている場合は、ディスパッチの前に拒否します。環境から引き継いだ認証情報で、有料の呼び出しが選ばれることはありません。どちらのハーネスも、測定の反復回数は正の値、ウォームアップは0以上の値であることを求めます。LGは、空の応答やリッチな応答を文字列に変換せず、本物のテキストブロックを保持します。途中の返信やプランが空だった場合は、成功したサンプルではなく失敗として扱います。実際の呼び出しの経路でも、途中で切れた応答、拒否、ツール呼び出し、不明な終了状態を、完了したレポートとして計時することはしません。NGのエラーは元の所有されたOutcomeを、LGのエラーは元のレスポンスを保持します。プロバイダーを使わない回帰テストが分離して検証するのは、コンシューマー側の射影のポリシーだけです。トランスポート、SDK、ホスト型モデルについてのエビデンスではありません。

```sh
# Install a current-cutover wheel using the Python binding build guide first.
python -m pip install requests langgraph langchain-openai psutil
cd benchmarks/dr_compare
env -u NG_WORKER_COUNT LLM_MOCK_MS=0 MOCK_SEARCH=1 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench_mock.py --warmup 5 --iters 50
```

fan-outの重なりを観測するには、`LLM_MOCK_MS=100` で再実行し、未設定の `NG_WORKER_COUNT`（デフォルトは4）と、明示的な `NG_WORKER_COUNT=1` を比べます。`FANOUT=5`、ウォームアップ、反復回数、チェックポイントのモードは、揃えたままにしてください。標準エラー出力と失敗も記録してください。直列のfan-outを知らせる警告が出ていないことだけでは、重なって実行された証拠にはなりません。これらはモデルを使わないワークロードで、有料のプロバイダーを使ったエビデンスではありません。

次のホスト型モデルのコマンドは、料金がかかることがあります。認証情報は、意図して設定してください。インメモリのチェックポイントを使います。PostgreSQLで比較するには、対応するDSN、パッケージの準備、別に記録した耐久性の範囲が必要です。システムコールのトレースやパケットキャプチャだけでは、意味が同等であることも、プロバイダーの課金も確認できません。

```sh
# From benchmarks/dr_compare; hosted calls require explicit credentials.
: "${OPENAI_API_KEY:?Set a hosted key only if you intend paid calls}"
: "${CRAWL4AI_URL:?Set a running Crawl4AI service}"
LLM_MOCK_MS=-1 MOCK_SEARCH=0 USE_INMEMORY_CP=1 NG_TRANSPORT=http-chat \
  python bench.py --warmup 2 --iters 5
```

このコマンドは、支出の上限にはなりません。`FANOUT=5` の場合、1つのリサーチクエリは、片側で最大7回の論理的なモデル呼び出し（プラン1回、リサーチャー5回、統合1回）を行います。両側でウォームアップ2回と測定5回を実行すると、論理的な呼び出しは合計98回になります。現在のNG SDKのデフォルトでは、トランスポートのリトライは無効です。LG側は、`max_retries=0` を明示しています。呼び出しの回数と、設定した出力上限の合計には、ベンチマークの外で上限を設けてください。どちらの側も、`NG_EXAMPLE_MAX_TOKENS=1600` がデフォルトです。明示した値は、そのまま使われます。使用を許可されているモデル、エンドポイント、認証情報、検索サービスだけを使ってください。規模を縮小した代表的なコホートは、過去の複数回反復した測定とは別物です。別のものとして区別して記載してください。

次は、回数に上限を設けた、有料プロバイダーでの診断です（レイテンシーの比較ではありません）。

```sh
# From benchmarks/dr_compare; this makes paid provider calls.
: "${OPENAI_API_KEY:?Requires an intentionally authorized hosted credential}"
env -u NG_WORKER_COUNT LLM_MOCK_MS=-1 MOCK_SEARCH=1 USE_INMEMORY_CP=1 \
  FANOUT=5 NG_TRANSPORT=http-chat NG_EXAMPLE_MAX_TOKENS=8192 \
  python bench.py --only neograph --warmup 0 --iters 1
```

これは、モデル呼び出しが最大7回、設定した出力トークンが57,344で、入力トークンが別にかかります。実際のウェブ検索は行わないので、実際の検索を使った実行ではなく、有料プロバイダー／モック検索と明記する必要があります。合格の条件は、終了コード0、成功したサンプルが1件、空でないレポート、`Failed runs: 0` です。プラン、回答、レポートのどれかが失敗した場合や空だった場合は、不合格になります。8,192トークンの上限は、診断のために明示的に選んだ値です。デフォルトを引き上げたものでも、料金の見積もりでも、支出の許可でもありません。承認済みで起動中のCrawl4AIサービスがあれば、`MOCK_SEARCH=0` で、同じモデル呼び出しの上限のまま、実際の検索を試せます。
