<!-- neograph-i18n: source=wasm/README.md locale=ja source_sha256=af08dda8e4780898848c6ccf35b39f7c895ee49d16baa4bf157795491950c3cf -->
# WebAssembly 上の NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

このディレクトリには、CI がビルドして実行する WebAssembly プログラムがあります。

- `module.cpp` と `neograph.mjs` は JavaScript モジュールです。JSON からグラフをコンパイルして実行し、モデル呼び出しは
  ホストの `fetch` で送ります。`examples/` にはこのモジュールを使う Node プログラムと Cloudflare Workers 方式のハンドラーが、
  `test/` には CI が実行するオフラインのテストがあります（ローカルの HTTP サーバーがモデルの代わりをします）。
- `smoke.cpp` は `DoubleNode` 1 つだけのグラフをコンパイルし、`doubled = seed * 2` を書き込み、
  `InMemoryCheckpointStore` で実行します。`seed = 21` なら `doubled = 42` と `trace = d` を出力します。
- `checks.cpp` は、スレッドのない 32 ビットホストで変わる点（下記）を確認します。

計画は [#535](https://github.com/fox1245/NeoGraph/issues/535) にあり、これはブラウザー向け SDK ではありません。

## 現状

| 項目 | 状態 |
|---|---|
| ツールチェーン | Emscripten 6.0.13。CI では emsdk のリリースコミットに固定しています。 |
| ビルドモード | シングルスレッド（WebAssembly の既定）と pthread。どちらも CI でビルドして実行します。JavaScript モジュールはシングルスレッドです。 |
| ランタイム | CI の Node.js 22 と 24。Workers の例は `Request` を渡して Node で実行しており、workerd では実行していません。ブラウザー、Deno、WASI は未検証です。 |
| SDK | SchemaProvider 0.4.0、インターフェースのリビジョン 7。`cmake/NeoGraphSchemaProvider.cmake` が固定するコミットを使います。libcurl もスレッドも使わずにビルドされ、SDK の JavaScript ホスト（`sp_js_host`）がモジュールの executor と `fetch` トランスポートです。 |
| コンポーネント | Core、LLM（ホストを通すモデル呼び出し）、および任意で Program。 |

## ビルド

ビルドには Emscripten 6.0.13（C++20 のコルーチン、`<stop_token>`、`std::bit_cast` が libc++ にあります）、CMake 3.20 以上、
Ninja が必要です。ASYNC、MCP クライアント、A2A、gRPC、SQLite と PostgreSQL のストアは、WebAssembly ホストが提供しない
ソケット、TLS、システムライブラリを必要とするため CMake が拒否します。以下ではオフにしています。LLM はホスト向けに
ビルドされます（下記）。

シングルスレッド（WebAssembly の既定）:

```bash
source /path/to/emsdk/emsdk_env.sh

emcmake cmake -S . -B build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DNEOGRAPH_BUILD_WASM=ON -DNEOGRAPH_BUILD_WASM_JS=ON \
  -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_ASYNC=OFF -DNEOGRAPH_BUILD_LLM=ON -DNEOGRAPH_BUILD_MCP=OFF \
  -DNEOGRAPH_BUILD_MCP_CLIENT=OFF -DNEOGRAPH_BUILD_MCP_SERVER=OFF -DNEOGRAPH_BUILD_MCP_HTTP_SERVER=OFF \
  -DNEOGRAPH_BUILD_A2A=OFF -DNEOGRAPH_BUILD_ACP=OFF -DNEOGRAPH_BUILD_GRPC=OFF \
  -DNEOGRAPH_BUILD_POSTGRES=OFF -DNEOGRAPH_BUILD_SQLITE=OFF -DNEOGRAPH_BUILD_UTIL=OFF \
  -DNEOGRAPH_BUILD_EXAMPLES=OFF -DNEOGRAPH_BUILD_TESTS=OFF -DNEOGRAPH_BUILD_BENCHMARKS=OFF \
  -DNEOGRAPH_USE_LIBCURL=OFF
cmake --build build-wasm --target neograph_wasm_smoke neograph_wasm_checks neograph_js
node build-wasm/wasm/smoke.js
node build-wasm/wasm/checks.js
node wasm/test/llm_host_test.mjs build-wasm/wasm
node wasm/test/workers_test.mjs build-wasm/wasm
```

`NEOGRAPH_BUILD_WASM_JS=ON` は `build-wasm/wasm/` に JavaScript モジュールを作ります。`neograph_module.wasm`、
`neograph_module.mjs`（Emscripten のローダー）、`neograph.mjs`（このディレクトリからコピーした API）です。この 3 つの
ファイルをプロジェクトにコピーして使います。`NEOGRAPH_BUILD_PROGRAM=ON` が必要なのはネイティブプラグインのチェックだけです。
グラフ smoke とモジュールだけをビルドするならオフにしてください。pthread: configure の行に `-DNEOGRAPH_THREADS=ON` を足し、
`NEOGRAPH_BUILD_WASM_JS` と `NEOGRAPH_BUILD_LLM` はオフのままにして、別のビルドディレクトリを使います。

2 つのモードを切り替えるスイッチは `NEOGRAPH_THREADS` だけです。Emscripten と WASI では `OFF`、それ以外では `ON` です。

- **シングルスレッド（`OFF`）。** `-pthread` も `Threads::Threads` もありません。`asio` ターゲットが `ASIO_DISABLE_THREADS` を
  エクスポートし、SDK も同じ方法でビルドされます（`SP_THREADS=OFF`、libcurl トランスポートなし）。処理はホストが
  提供する executor で実行されます。
- **pthread（`ON`）。** すべてに `-pthread` を付け、smoke プログラムにはワーカー 4 つのプールを置きます。SDK は
  `SP_THREADS=ON` でビルドされ、WebAssembly では libcurl トランスポートは引き続きありません。
- **例外。** `-fexceptions`（JavaScript ベースの例外）を、C ソースを含むすべての NeoGraph ターゲットに設定し、SDK も同じフラグを
  使います。すでに `-fexceptions` または `-fwasm-exceptions` を指定した設定はそのままにし、例外が無効なら configure が
  失敗します。
- **プログラムごとに設定は 1 つ。** Asio はヘッダーオンリーなので、スレッドの設定が食い違う翻訳単位は、何も検出してくれない
  ODR 違反になります。生成される `neograph/build_config.h` が設定を記録し、すべての公開ヘッダーが `neograph/api.h` を通して
  このヘッダーに届き、設定が食い違う翻訳単位はコンパイルされません。SDK も自分の設定を同じ方法で記録しており
  （`sp/build_config.h`）、両者が違えば configure が失敗します。

## JavaScript モジュール

```js
import { load } from './neograph.mjs';

const neograph = await load();                      // a Worker passes its imported module: load({ wasmModule })
const result = await neograph.run({
  graph: {                                          // a graph definition, as JSON text or an object
    channels: { messages: { reducer: 'append' } },
    nodes: { llm: { type: 'llm_call' } },
    edges: [{ from: '__start__', to: 'llm' }, { from: 'llm', to: '__end__' }],
  },
  provider: {                                       // given at run time; nothing of it is in the module
    baseUrl: 'https://api.example.com',             // the origin of an OpenAI-compatible Chat Completions endpoint
    path: '/v1/chat/completions',
    apiKey: env.API_KEY,
    model: 'a-model',
  },
  input: { messages: [{ role: 'user', content: 'Hello' }] },
  onEvent: (event) => console.log(event),           // optional: streams the run
});
console.log(result.text);
```

- **`load(options)`** はモジュールを 1 回だけインスタンス化します。Node とブラウザーでは、`neograph_module.mjs` の隣にある
  `neograph_module.wasm` を見つけます。Worker は実行時にバイト列をコンパイルできないので、バンドラーが import した
  `WebAssembly.Module` を `wasmModule` として渡します。`wasmBinary` と `locateFile` は Emscripten のオプションです。
- **`neograph.compile(graph, provider)`** は、`graph.release()` まで何度でも実行できる `Graph`（`graph.run(input, run)`）を
  返します。**`neograph.run({ graph, provider, input, ...run })`** は、コンパイルして 1 回実行し、解放します。
- **`provider`** は、Chat Completions のエンドポイントなら `{ baseUrl, path, apiKey, model }` です。`family` は別のワイヤー
  family を、`paths: { buffered, streaming }` は別のルートを指定し、`descriptor` には完全な SchemaProvider descriptor を
  渡せます。`instructions`、`maxOutputTokens`、`temperature` は任意です。キーは descriptor の family がキーを送る方法で
  送られます。
- **`run`** のオプション: `onEvent`（実行をストリーミングします。モデルをストリーミングモードで呼び出します）、`signal`
  （実行とモデル呼び出しをキャンセルする `AbortSignal`）、`timeoutMs`。
- **結果**は `{ status, text, output, trace, usage }` です。`status` は `completed`、`interrupted`、`step_limit` などで、
  `text` は最後のアシスタントメッセージ、`output` は最終状態（`output.channels.<name>.value`）、`trace` は実行されたノードの
  一覧、`usage` はプロバイダーが報告すれば `input_tokens`、`output_tokens`、`total_tokens` を持ちます。
- **イベント**は `{ type, node, data }` で、`type` は `node_start`、`node_end`、`llm_token`（`data` はデルタのテキスト）、
  `channel_write`、`interrupt`、`error` のいずれかです。エンジンが生成した順に届きます。
- **エラー**は `NeoGraphError` です。`type` は `provider`（モデル呼び出しが失敗しました。`kind`、`reason`、`httpStatus` が
  経緯を示します）、`cancelled`、`error`（不正なグラフやプロバイダー、または失敗したノード）のいずれかです。不正なグラフや
  プロバイダーは `compile` が投げます。

グラフでは、ツールを必要としない組み込みのノードタイプ `llm_call`、`intent_classifier`、`subgraph` を使えます。JavaScript で
書くツールやノード、チェックポイントストア、Program は、このマイルストーンの対象ではありません。

## 仕組み

### 2 つのイベントループ

NeoGraph のグラフエンジンは、Asio の `io_context` 上のコルーチンの集まりです。WebAssembly ホストがスレッドとイベントループを
所有しており、モジュール内のどれもそれをブロックしてはいけません。Asyncify も JSPI も、モジュール自身のループもありません。
SchemaProvider も同じ問題を抱えており、ホストシームで解決しています。ホストが実行する `Executor` と、HTTP を送る
`HostTransport` で、`sp_js_host` がこれを `queueMicrotask`、`setTimeout`、`fetch` の上に実装します。モジュールは次の 4 つで
エンジンをそのループにつなぎます。

- **`io_context` を誰が回すか。** `async::HostLoop` が `io_context` を所有し、`run()` を呼びません。1 ターンは、ループ専用の
  SchemaProvider executor に投稿されたタスクです（Client は終了時に自分の executor をシャットダウンするため、両者は 1 つを
  共有できません）。ターンは実行可能なハンドラーがなくなるまで `poll_one()` を呼びますが、1 ターンで最大 256 個までとし、
  仕事が残っていればホストに処理を返して次のターンを投稿します。ループの executor で処理が実行されるとターンが予約されます。
  `HostLoop::executor()` は `io_context` の executor を包み、その上でハンドラーを実行するとターンを投稿するようにします。
  同時に 1 つだけで、ターンの中からは投稿しません。実行の開始、キャンセル、SDK の完了のいずれもそうします。アイドルの
  ループは何も予約しません。
- **SDK の完了がグラフのコルーチンに届く方法。** `llm_call` は SDK の処理を開始し、起床チャネルで中断します。SDK は fetch の
  チャンクや結果を、自分の executor のタスク、つまり JavaScript のマイクロタスクで届けます。コールバックはイベントを保持して
  チャネルにシグナルを送り、チャネルはコルーチンの continuation をループの executor に投稿します。これでターンが予約され、
  そのターンでコルーチンが再開してイベントを順に実行のオブザーバーへ渡し、結果が来るとノードを終えます。SDK のコールバックの
  中でインラインに実行されるものはなく、スレッドもロックも関わりません。
- **`run_async` をブロックせずに最後まで進める方法。** `HostLoop::spawn(awaitable, done)` は `co_spawn` にターンを 1 つ足した
  ものです。すぐに戻り、`done(exception, result)` は後のターンで呼ばれます。モジュールの `ng_run` が戻ってホストの
  イベントループが続き、実行が終わると `ng_js_done` から JavaScript の `Promise` が解決されます。
- **Asio のタイマー。** リトライのバックオフやリソース待ちのポーリングは Asio のタイマーで、ホストのループは Asio のタイマー
  キューを見られません。エンジンは `async::HostTimer`（`src/core/host_timer.h`）で待ちます。これは Asio のタイマーを設定する呼び出し 1 回で
  ループに期限を知らせ（`async::note_deadline`）、ループは最も早い期限にホストのタイマーを 1 つ設定します。このタイマーは、
  同じ時刻に満了する Asio のタイマーを Asio がディスパッチする前に実行されることがあるため（Asio の Windows 用リアクターは
  タイマーが満了したことを専用のスレッドから知らされ、ホストの時計は Asio の時計より進んでいることがあります）、ループは
  Asio がその期限までのタイマーをディスパッチするまで期限を保持し、まだなら 1 ミリ秒後にもう一度確認します。`src/` や
  `include/` の素の Asio タイマーはホストでは決して発火しないため、`host_timer_guard` テストがこれを拒否し、ホストの
  ループで動かないコードは `scripts/host_timer_allowlist.txt` に列挙されます。`HostLoop` の下で Asio のタイマーを待つ
  独自のコードは、`async::note_deadline` を自分で呼びます。

同期のエントリーポイントはシンボルを残したまま拒否します。`GraphEngine::run`、`resume`、`Provider::dispatch` と `invoke`、
同期のエンドポイント呼び出しは、呼び出しが終わるまで専用の `io_context` を回しますが、これは待っているそのスレッドしか
回せないホストのループを待つことになります。ホストが回す executor 上のプロバイダー（`Provider::host_pumped()`。そう宣言する
executor で作った `SchemaProvider` は true です）にこうした呼び出しが届くと、何も送る前に SDK の
`runtime.blocking.host_pumped` という理由（`ErrorKind::Misuse`、再試行の安全性 `NotSent`）で失敗します。メッセージは使うべき
`_async` 形式を示します。ホストが回すプロバイダーに届かない実行は、smoke プログラムのように `run()` で動きます。

### TLS と OpenSSL

TLS はホストの役目で、`fetch` が行います。SDK に libcurl トランスポートがない場所（WebAssembly、または
`NEOGRAPH_THREADS=OFF` のビルド）では、`neograph::llm` はホスト専用にビルドされます。CMake は OpenSSL を探さず、
`neograph::llm` は `neograph::async` も `SchemaProvider::transport` もリンクせず、ネイティブホストで LLM、MCP クライアント、
A2A が使う `asio::ssl` の HTTP クライアントである `NEOGRAPH_BUILD_ASYNC` は引き続き拒否されます。このとき `SchemaProvider` の
libcurl コンストラクターは、SDK の `ConfigError` を `/transport` で投げます。ネイティブビルドは変わりません。同じライブラリ、
同じリンク行、同じコンストラクターです。

### ホストシームの上にプロバイダーを作る

`SchemaProvider` には、2 つ目のコンストラクター `SchemaProvider(descriptor, options, executor, transport, defaults)` と、
それに対応する `create` があります。SDK の公開 `Client(descriptor, options, executor, transport)` に渡すもので、libcurl も
OpenSSL も不要で、executor の `host_pumped()` の宣言を引き継ぎます。モジュールは、コンパイルしたグラフごとに executor を 1 つと
fetch トランスポートを 1 つ作ります。

```cpp
auto executor  = sp::runtime::js::make_executor();
auto transport = sp::runtime::js::make_fetch_transport(executor);
auto provider  = neograph::llm::SchemaProvider::create(descriptor, sp::runtime::Options::with_api_key(key),
                                                       executor, transport);
```

### JavaScript の表面

モジュールは C API をエクスポートし、`neograph.mjs` がそれを包みます。JSON 文字列を使う C API にはバインディング
ライブラリが要りません。Embind はライブラリとすべての型の登録を足すことになりますが、表面は `ng_compile`、`ng_run`、
`ng_cancel`、`ng_release`、`ng_error` の 5 つの関数だけです。結果は、ホストのイベントループの後のターンから呼ばれる 2 つの
コールバック `Module.ngOnEvent` と `Module.ngOnDone` で返ります。キーは `compile` に渡すプロバイダー記述の一部で、そのグラフの
SDK クライアントが保持します。バイナリにも、どの結果にも、エラーメッセージにも入りません。

## 例

- `examples/node/chat.mjs`: Node からの 1 回の呼び出しです。
  `API_KEY=... node wasm/examples/node/chat.mjs <base url> <path> <model> "<prompt>" [--stream]`。
- `examples/workers/worker.mjs` と `wrangler.toml`: `POST {"message": "..."}` にモデルの返答を JSON またはサーバー送信
  イベントで返すリクエストハンドラーです。テンプレートに秘密は含まれません。キーは Worker のシークレット
  （`wrangler secret put API_KEY`）で、`neograph_module.wasm` は `WebAssembly.Module` として import します。

## テスト

CI はモジュールをビルドし、Node 22 と 24 で、ネットワークもキーもなしに次を実行します。

- `test/llm_host_test.mjs`: ローカルの HTTP サーバーが Chat Completions のエンドポイントのように応答します。バッファ済みの
  応答と、1 文字が 2 つのチャンクにまたがって切れるサーバー送信イベントの応答です。テストはモジュールで 1 ノードのグラフを
  実行し、最終状態、ストリーミングイベントの順序、送り出されたリクエスト（実行時に渡したキーとモデル）、プロバイダーの拒否、
  `AbortSignal` によるキャンセル、タイムアウト、並行実行、モデル呼び出しが進行中でもホストのタイマーが動き続けることを
  確認します。
- `test/workers_test.mjs`: Workers の例がエクスポートする `fetch` ハンドラーを、同じサーバーに対して `Request` を渡して
  呼び出します。バッファ済みとストリーミングの両方です。ハンドラーのロジックとプラットフォーム API の使い方を示すもので、
  workerd での実行ではありません。
- `checks.cpp`（下記）は、ホストが回すプロバイダーに対する同期の実行を行い、拒否されること、何も送られなかったことを
  確認します。
- `tests/test_host_loop.cpp` は、手動の executor を相手にループとホストシームのプロバイダーをネイティブで実行するので、
  ネイティブビルドも同じ経路を扱います。

## 測定したサイズ

Release、`-O3 -flto`、Emscripten 6.0.13。グラフ smoke には Core エンジンと SDK ランタイムが含まれ、モジュールはそこに
LLM コンポーネント、SDK の JavaScript ホスト、C API を加えたものです。

| ビルド | `.wasm` | `.wasm` gzip -9 | `.js` |
|---|---:|---:|---:|
| グラフ smoke、シングルスレッド | 5,509,570 B (5.3 MiB) | 1,608,986 B (1.5 MiB) | 83,223 B |
| グラフ smoke、pthread | 5,448,684 B (5.2 MiB) | 1,660,246 B (1.6 MiB) | 103,691 B |
| Program と LLM を含む checks、シングルスレッド | 6,039,737 B (5.8 MiB) | 1,842,525 B (1.8 MiB) | 84,263 B |
| JavaScript モジュール、シングルスレッド | 6,270,525 B (6.0 MiB) | 1,835,562 B (1.8 MiB) | 87,762 B |

CI はシングルスレッドの smoke を `.wasm` 6,000,000 バイト、gzip 後 1,750,000 バイト（測定したサイズに約 9% を足した値）で、
モジュールを `.wasm` 6,850,000 バイト、gzip 後 2,010,000 バイト で制限し、サイズをジョブのサマリーに報告します。Worker のスクリプトにはプランに応じたサイズ制限が
あるので、モジュールの圧縮後のサイズをご利用のプランの制限と比べてください。このページの以前の版にあった 712 KB は、型付き SDK より
前に測った値で、今は当てはまりません。

## これらのホストで使えないもの

NeoGraph が呼び出しを所有している場合は型付きのエラーで失敗し、そうでなければビルドされません。

- **ホストを通さないモデル呼び出し。** ASYNC、MCP クライアント、A2A にはソケットと TLS が必要です。モデル呼び出しは上の
  モジュールでホストの `fetch` を通します。別の WebAssembly ホスト向けのビルドは、自前の executor とトランスポートを
  `SchemaProvider` に渡します。
- **ホストが回すプロバイダーに対するブロッキングのエントリーポイント。** `run()`、`Provider::dispatch` などは拒否します（上記）。
- **Program。** Program コンポーネントはコンパイルできますが、`ProgramRuntime` がスレッドを起動するため、シングルスレッドの
  ビルドでは、その生成、ノードの attempt タイムアウト、`n > 1` の `set_worker_count(n)` が `thread: Not supported` で
  失敗します（#557）。ホスト上の Program ストアは #556 で扱います。
- **ネイティブ Program コントロール。** C v1 プラグイン ABI は 64 ビットのレイアウトなので、wasm32 では
  `NativeControlBinding::create` が `NativeControlUnavailable` で拒否します。
- **同期のチェックポイントストア。** 非同期でないストアは、各操作をワーカースレッドのプールで実行します。
  シングルスレッドのビルドにはプールがないため、スレッドを待たずに `SynchronousCheckpointStoreUnavailable` で操作を
  拒否します。`AsyncCheckpointStore` を実装してください。組み込みのインメモリストアは、managed 予算を含むすべての操作を
  呼び出したスレッドで実行するため、`model_token_budget` を指定した実行がその上で動作します。
- **期限付きの managed 予算。** 期限は、再起動後も回復できるクロックの原点に結び付けられますが、WebAssembly ホストには
  それがありません。実行は、ストアに問い合わせる前に `ManagedBudgetDeadlineClockUnavailable` で拒否されます。
- **ネイティブアーカイブ。** ファイルの保管（custody）バックエンドがなく、SDK は `Unsupported` エラーで拒否します。
- **サブプロセスへの委譲**（Harness CLI、stdio の MCP サーバー）、**待ち受けソケット**、**gRPC**、**ファイルパスによるアップロード**、**Python**。

## ブラウザー

ブラウザー用のハーネスはツリーにありません。`serve_smoke.py` は存在しない `smoke.html` を前提としており、ブラウザーでの
実行記録もありません（#435）。Node.js での実行は、ブラウザーでの動作の証拠にはなりません。モジュールは `web`、`worker`、
`node` の環境向けにビルドされますが、実行しているのは Node だけです。

## マイルストーン

ツールチェーン調査後の計画は [#535](https://github.com/fox1245/NeoGraph/issues/535) にあります。

- **M0 と M1。** wasm32 の修正、シングルスレッドビルド、CI での Node smoke とサイズゲート。
- **M2（このページ）。** ホストの `fetch` を通すモデル呼び出し。ホストループ、ホストシームのプロバイダー、JavaScript モジュール、
  Node と Workers の例。
- **M3.** ホストのストア上の Program（[#557](https://github.com/fox1245/NeoGraph/issues/557)、
  [#556](https://github.com/fox1245/NeoGraph/issues/556)）。`HostLoop::executor()` が Program ランタイムの受け取る
  `any_io_executor` です。
- **M4.** リアルタイムセッション、WASI、ネイティブアーカイブ用の保管バックエンド、リモート Harness。
