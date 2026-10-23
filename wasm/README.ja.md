<!-- neograph-i18n: source=wasm/README.md locale=ja source_sha256=f395370984ce0ff537b358013a071205c840787625f2913d3f21b30ca3a238cc -->
# WebAssembly 上の NeoGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

このディレクトリには、CI がビルドして実行する WebAssembly プログラムがあります。

- `module.cpp`、`programs.cpp`、`neograph.mjs` は JavaScript モジュールです。JSON からグラフをコンパイルして実行するか、Program（一時停止、
  fork、すべての実行の記録を備えたもの）をコンパイル・承認・実行し、モデル呼び出しはホストの `fetch` で送ります。型は `neograph.d.ts` にあります。
  `examples/` にはこのモジュールを使う Node プログラムと Cloudflare Workers 方式のハンドラーが、
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
| ランタイム | CI の Node.js 22 と 24。Workers の例は `Request` を渡して Node で実行しており、workerd では実行していません。グラフ smoke は `smoke.html` と `serve_smoke.py` により Chromium でも動きます（下記）。ブラウザーでの JavaScript モジュール、Deno、WASI は未検証です。 |
| SDK | SchemaProvider 0.4.0、インターフェースのリビジョン 7。`cmake/NeoGraphSchemaProvider.cmake` が固定するコミットを使います。libcurl もスレッドも使わずにビルドされ、SDK の JavaScript ホスト（`sp_js_host`）がモジュールの executor と `fetch` トランスポートです。 |
| コンポーネント | Core、LLM（ホストを通すモデル呼び出し）、そして Program（ホストループ上のランタイム。ストアはメモリ上にあるか、ホストが用意します）。 |

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
  -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
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
node wasm/test/program_test.mjs build-wasm/wasm
```

Program ストアのテスト（JavaScript ストアに対する適合性スイートとストアのチェック。下の「JavaScript の Program ストア」と「テスト」を参照）も
ビルドするには、configure の行に `-DNEOGRAPH_BUILD_WASM_STORE_TESTS=ON` を足してください。googletest を取得します。そのうえで:

```bash
cmake --build build-wasm --target neograph_wasm_store_conformance neograph_wasm_store_checks
node wasm/test/store_conformance_test.mjs build-wasm/wasm
node wasm/test/js_stores_test.mjs build-wasm/wasm
```

`NEOGRAPH_BUILD_WASM_JS=ON` は `build-wasm/wasm/` に JavaScript モジュールを作ります。`neograph_module.wasm`、
`neograph_module.mjs`（Emscripten のローダー）、`neograph.mjs`（このディレクトリからコピーした API）、`stores.mjs`（`neograph.mjs` が import する、
ホストのストアへのブリッジ）、`neograph.d.ts`（型）です。最初の 4 つのファイルをプロジェクトにコピーして使います。
`NEOGRAPH_BUILD_PROGRAM=ON` ならモジュールに Program API が入ります（[下記](#programs-in-the-module)）。`NEOGRAPH_BUILD_QUICKJS_CONTROL=ON` は
JavaScript で書く Program に必要な QuickJS 制御ランタイムを加えます。これがないと、ドキュメント形式で渡した Program だけを受け付けます。
`NEOGRAPH_WASM_JS_PROGRAMS=OFF` はグラフだけを実行するモジュールを作り、サイズは約半分です（[測定したサイズ](#measured-sizes)を参照）。
pthread: configure の行に `-DNEOGRAPH_THREADS=ON` を足し、`NEOGRAPH_BUILD_WASM_JS` と `NEOGRAPH_BUILD_LLM` はオフのままにして、別のビルドディレクトリを使います。

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
書くツールやノードは、このマイルストーンの対象ではありません。Program には専用の表面があり、下で説明します。

<a id="programs-in-the-module"></a>
## モジュールの Program

Program は、ホストがコンパイルして承認する小さな JavaScript モジュールで、予算のもとでグラフを実行します。実行は割り込みで止まって答えを待つことができ、
再開や fork ができ、すべてのステップの記録をストアに残します（[Program ガイド](../docs/program-guide.md)）。このモジュールは、Program を JavaScript から
コンパイルし、承認し、ホストのイベントループの 1 本のスレッドの上で実行します。

```js
import { load } from './neograph.mjs';

const SOURCE = `
export function define() {
  return ng.graph("two-turns")
    .channel("messages", {reducer: "append", initial: []})
    .node("first", {type: "llm_call"})
    .node("second", {type: "llm_call"})
    .entry("first").edge("first", "second").exit("second")
    .interruptBefore("second");                      // the run stops here and waits for an answer
}
export function* main(input) {
  const reply = yield ng.callCore("two-turns", input, "run");
  return {messages: reply.messages};
}
`;

const neograph = await load();
const programs = neograph.programs({
  provider: {
    baseUrl: 'https://api.example.com', path: '/v1/chat/completions', apiKey: env.API_KEY, model: 'a-model',
    catalogue: { version: 1, models: [{ id: 'a-model', input_limit: 4000, output_limit: 1000 }] },
  },
});
const version = await programs.admit(SOURCE);                  // compile and admit
const run = programs.start(version, {
  input: { messages: [{ role: 'user', content: 'Hello' }] },
  onEvent: (event) => console.log(event.kind),                 // optional: the run's events, in order
});
const paused = await run.result;                               // status 'interrupted'; paused.interrupt says where
const done = await run.resume('Go on').result;                 // the answer is the next message; 'completed'
console.log(done.output.messages.at(-1).content);
programs.release();
```

- **`neograph.programs({ provider, owner, limits, budget, stores })`** は、1 つのプロバイダーと 1 組のストアの上の `Programs` オブジェクトを作ります。プロバイダーが不正なとき、
  またはモジュールが Program 対応なしでビルドされているときは `NeoGraphError` を投げます。`provider` は `compile` と同じで、1 つ加わります。`catalogue` は SDK のモデルカタログ
  （`{ version: 1, models: [...] }`）で、組み込みのカタログの上に重ねられます。`owner` はオブジェクトの Program と実行のスコープを決めます。`stores` はホスト自身のストアです
  （[JavaScript の Program ストア](#program-stores-in-javascript)）。省いたストアはメモリ上にあります。`limits` は承認される Program に与えられる最大値、`budget` は予算を指定しない実行の予算で、
  どちらも下のように書きます。
- **`programs.admit(source)`** は Program をコンパイルして承認し、`{ id, bundleId, name }` で解決します。`source` は JavaScript 制御ソースのテキスト、`{ javascript, name }`、
  またはすでにドキュメント形式の Program のための `{ document, name }` です。コンパイラーや承認ポリシーが拒否したソースは、`compile` または `admission` タイプの `NeoGraphError` で拒否され、
  `diagnostics` に `{ code, message, pointer, witness }` が並びます。同じソースをもう一度承認すると、同じバージョンになります。
- **`programs.start(version, { input, budget, runId, onEvent, signal })`** は `ProgramRun` をすぐに返します。**`programs.run(version, options)`** は `start(...).result` です。
- **`ProgramRun`** には `id`、`versionId`、`result`、`resume(value, options)`、`fork(value, options)`、`cancel()` があります。`result` は実行が終わるか答えを待つときに解決されます。
  失敗した実行や取り消された実行はその状態で解決され、呼び出し自体ができなかったときだけ拒否されます（不明なバージョン、バージョンが承認する範囲を超える予算、すでに使った run id）。
  `resume` は実行が待っている割り込みに答え、同じ id の `ProgramRun` を返します。`fork` は同じ一時停止を、別の答えとともに新しい実行（`options.runId` で名前を付けます）として続け、
  一時停止中の実行が残した予算の半分（または `options.budget` が求めた分で、一時停止中の実行にウォールクロック時間と Core ステップが少しは残らなければなりません）を受け取り、一時停止中の実行は引き続き再開できます。終わったノードは再実行されません。1 つの一時停止の fork は 1 つずつ行ってください。
  `cancel()` は、この呼び出しが実行を取り消したかどうかで解決されます。まだ送っていない resume や fork（続きとなる実行の結果を待っているもの）は、`cancel()` や中断された `signal` が代わりに
  取り下げ、その `result` は `cancelled` エラーで拒否されます。すでに中断された `signal` なら、呼び出しは行われません。
- **`programs.result(runId)`** は、ストアが実行について持っているものを読みます。最新の結果（待っている実行の一時停止も結果です）で、まだ結果がなければ `{ runId, status: 'running' }`、ストアがその実行を知らなければ `null` です。**`programs.release()`** は実行中の run を取り消して
  オブジェクトを解放し、すでに行った呼び出しは先に終わります。割り込みで待っている実行は、ストアにあるままになります。
- **結果**は `{ runId, versionId, status, output, trace, usage, remainingBudget, checkpointId?, interrupt?, failure? }` です。`status` は `completed`、`interrupted`、`cancelled`、
  `budget_exhausted`、`timed_out`、`failed`、`ambiguous_effect`、`checkpoint_incompatible` のいずれかです。`output` は `main()` が返した値です（実行が待っている間は `output.channels` の下の
  グラフの状態）。`trace` は、実行が始まってから、または再開してから実行されたノードの一覧です。`interrupt` は `{ node, value, kind, pendingId }`、`failure` は
  `{ code, message, node, attempts, witness }` です。
- **イベント**は、ランタイムが書いた順に届く実行のジャーナル項目 `{ sequence, kind, run_id, operation_id, payload, ... }` です。モデルの返答は `payload.type === 'llm_token'` のイベントとして届きます。
  `onEvent` が投げたエラーはその実行を取り消し、`result` をそのエラーで拒否します。`signal` は、呼び出しが保留中に中断されると呼び出しを取り消し、呼び出しが確定したあとは効果がありません（一時停止した実行には `cancel()` を使ってください）。ランタイムが対応していない場合が 1 つあります。最初のノードが始まる前に取り消された、再開された実行は `P_TRANSITION_CONFLICT` で `failed` になり、
  レコードは `running` のまま残ります。ランタイムは、中断されたコマンドの予約を保持している間は取り消しを公開できないからです。再開された実行は、ノードが始まってから取り消すか、終わるまで待ってください。
- **予算。** 予算は `{ wallTimeMs, modelTokens, monetaryMicrounits, maxConcurrency, maxProgramOperations, maxCoreSteps, ... }` です。省いた項目は既定値のままです。
  ウォールクロック時間 60 秒、モデルトークン 10,000、同時分岐 4、Program 操作 32、Core ステップ 100で、これは JavaScript ソースが宣言できる最大値でもあります。金額は 0 から 2^53 までの整数で、名前は予算のメンバーでなければならず、そうでなければ呼び出しは拒否されます。
  モデル呼び出しは、最悪の場合が残りの予算に収まるときだけ送ります。最悪の場合とは、モデルの入力上限に出力上限を足した値で、カタログから読みます（出力は `maxOutputTokens` でも指定できます）。
  どのカタログも上限を与えないモデルは呼び出されず、実行は `NG.ConfigError` で `failed` になります。`catalogue` がその上限を伝えます。`modelTokens` には正の値を与えてください。0 を与えた実行は呼び出しの前には制限されず、ランタイムは呼び出しをあとから課金します。
- **エラー。** `compile` と `admission` のほかに、ランタイムの拒否は `code`（たとえば、バージョンが承認する範囲を超える予算なら `P_START_BUDGET`）と `witness` を持つ `program` タイプの
  `NeoGraphError` です。

すべてはホストのイベントループの上で動きます。ランタイムはストア、タイマー、モデル呼び出しをそこで await し、それらのどれもループをブロックしません。制御ソースは例外です。`define()` と `main()` の各ステップは
同じスレッドの QuickJS で実行されるので、終わらないソースは、その時間制限が止めるまでループを握ります。時間制限は、Program を承認するときに実行される `define()` では 5 秒、`main()` のステップでは実行のウォールクロック時間の予算（既定 60 秒、`budget.wallTimeMs`）です。
QuickJS のスタック制限はこのビルドでは有効なので、際限なく再帰するソース、またはパーサーが追えないほど深く入れ子になったソースは、ホストのスタックをあふれさせる代わりに診断 `P_JS_RESOURCE_LIMIT` で失敗します。`admit` は `compile` エラーで拒否され、そうなる `main()` は実行を `failed` で終えます。この制限が数えるのはモジュール自身のリニアメモリ上のスタックで、コンパイラーがいくつを求めても、その 48 KiB だけを使います。モジュールが読むことも回復することもできないホストの呼び出しスタックは、何が再帰するかによって、それより 1.5 倍から 20 倍の速さで使い果たされるからです。48 KiB で使えるのは、入れ子の呼び出し約 200 個（`sort` の比較関数を経由すると約 30 個、`map` のコールバックを経由すると約 80 個）と、ソース内の入れ子の括弧、配列・オブジェクトのリテラル、呼び出し約 130 個（入れ子の関数は約 40 個）です。512 KiB を使うネイティブビルドは約 3 倍まで許します。トラップのない Proxy オブジェクトの連鎖を `Object.getPrototypeOf` や `Object.isExtensible` がたどる場合はこの制限には見えず、約 5,000 個の連鎖はなおホストのスタックをあふれさせます。48 KiB は Node 22 と 24 のホストスタック（約 984 KiB）に合わせて調整した値です。試した形のうち最悪のものでも、そのスタックの約 3 分の 1 で通り、スタックがもっと小さいホストでは余裕が減ります。

信頼しない制御ソースを承認するホストは、捨てられる Worker でそうするべきです。上の制限は試した再帰の形を止めますが、WebAssembly のトラップは同じモジュールインスタンスでは回復できません。ホストのスタックがあふれると、呼び出しは確定せず、そのインスタンスは使えなくなり、Node ではホストが処理しなければ捕捉されない `RangeError` でプロセスが終了します。モジュールでは、ホストが信頼する、または検証した制御ソースだけを実行してください。

一時停止したグラフのチェックポイントはモジュールのメモリにあるので、実行を再開したり fork したりできるのは、それを始めた `Programs` オブジェクトが生きている間だけです。

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
ライブラリが要りません。Embind はライブラリとすべての型の登録を足すことになりますが、表面は数個の関数だけです。グラフには `ng_compile`、`ng_run`、
`ng_cancel`、`ng_release`、`ng_error`、Program には `ng_programs_create`、`ng_programs_admit`、`ng_programs_start`、`ng_programs_resume`、`ng_programs_fork`、`ng_programs_cancel`、
`ng_programs_result`、`ng_programs_release` があります。結果は、ホストのイベントループの後のターンから呼ばれるコールバックで返ります。グラフは `Module.ngOnEvent` と
`Module.ngOnDone`、Program は `Module.ngProgramEvent` と `Module.ngProgramDone` です。キーは `compile` に渡すプロバイダー記述の一部で、そのグラフの
SDK クライアントが保持します。バイナリにも、どの結果にも、エラーメッセージにも入りません。

## ホストループ上の Program

`program::ProgramRuntime` はループの executor を受け取り、すべてをその上で実行します。Program の attempt、デッドライン
タイマー、`ProgramHandle::wait_async` の待機者がそうです。スレッドは起動しません。読み書きするストアは awaitable で、
このビルドが提供するのはインメモリのストアです。

```cpp
async::HostLoop loop(host_executor);                  // the sp::runtime::Executor the host runs
program::RuntimeConfig config{catalog, checkpoints, /*state_store=*/{}, transitions, /*scheduler_threads=*/1};
config.executor = loop.executor();                    // required here: a build without threads has no pool to fall back to
program::ProgramRuntime runtime(std::move(config));

loop.spawn(runtime.run_async(owner, version, invocation),
           [](std::exception_ptr error, program::ProgramResult result) { /* from a later turn */ });
```

- **必須。** `RuntimeConfig::executor` がないと、スレッドのないビルドのコンストラクターは `graph::ConfigurationFailure` である
  `ProgramExecutorRequired` を投げます。ネイティブビルドでは、未設定ならランタイム自身のプールを従来どおり使います。
- **awaitable 形式。** すべての操作にあります。`start_async`、`reconnect_async`、`resume_handle_async`、`fork_async`、
  `replace_async`、`migrate_graph_async` などです。ループ上で実行され、待機しているコルーチンの executor で完了します。
  `run_async` と `resume_async` は結果を待ちます。
- **同期呼び出しは拒否されます。** `start`、`resume`、`fork`、`run` などの同期呼び出しと、結果がまだないときの
  `ProgramHandle::wait` は、Program を動かすべきスレッドをブロックしてしまいます。これらは何かをする前に
  `program::ProgramBlockingCallRefused`（`graph::ConfigurationFailure`）を投げます。`operation()` は呼び出しを、
  `alternative()` は awaitable 形式を示します。`hook_runtime` は同じ理由でコンストラクターが拒否します。
- **タイマー。** ランタイムは `async::HostTimer` だけで待ちます。このタイマーはデッドラインをループへ知らせ、
  キャンセル、再設定、破棄のときに取り下げるので、終了した run がホストタイマーを残すことはありません。
- **ノード本体。** executor があると、専用のワーカーが必要なノードの attempt は 32 スレッドのプロセス全体のプールではなく
  その executor の strand で実行されるので、ノード本体は自分のスレッドをブロックしてはいけません。
- **ロック。** ストアに触れる臨界区間は、コルーチンがブロックせずに待てる `OperationLock` を持ちます。`std::mutex` は
  メモリだけを守ります。
- **awaitable なストア。** Program ストアのインターフェース 5 つは `asio::awaitable` を返し、ランタイムのすべての操作がループ上でそれを await します。
  エントリーポイント、終端の公開、子の起動、Core ツール grant リゾルバー（`RuntimeConfig::core_tool_grant_awaitable_resolver`）、
  `ProgramCatalog` の `_async` 形式、`ModuleResolver::resolve_async` がそうです。インメモリのストアのように操作がすぐ完了するストアは
  ターンを使わず、ホストループの後のターンで完了するストアも動きます。同期形式は同じコルーチンを呼び出したスレッドで最後まで実行し、
  上記のとおり拒否するので、待っているスレッドしか回せないループを待つことはありません。
- **ランタイムを破棄すると** 実行中の run はキャンセルされますが、終了は待ちません。attempt が終わるまでランタイムの
  状態を保持するので、ハンドルが終了状態になるまでループを回し続けてください。

<a id="program-stores-in-javascript"></a>
## JavaScript の Program ストア

WebAssembly で NeoGraph を動かすホストは、Program ランタイムのストアを JavaScript のオブジェクトとして渡します。C++ 側はそのオブジェクトを 5 つの awaitable なストアのインターフェースに
合わせるので、ランタイムとカタログはそれをほかのストアと同じように使います。契約が運ぶのは値と判断だけで、ストアがそれをどう扱うかは C++ に残るため、ホストが状態機械を書くことはありません。

```js
const neograph = await load();
const programs = neograph.programs({
  provider,
  stores: { transitions, programs: programStore, fragments, modules },    // each member is optional
});
```

省いたメンバーは、モジュール自身のインメモリストアで、メンバーでない名前（綴り間違いなど）は拒否されます。`transitions` ストアは、下で必須と記したメソッドに加えて `loadJavascriptCommands`、`loadMigrationPlan`、`loadRunLineage`、
`loadGeneration` も持っていなければなりません。実行はコマンド履歴を、fork はマイグレーションの証明を、再開と fork は実行の系譜を読むからです。`Programs` オブジェクトは、ランタイムがそのオブジェクトから作ったストアをすべて手放すまで、オブジェクトを登録したままにします。それは
`programs.release()` が取り消した実行が自分の終わりを公開したあとなので、オブジェクトを解放してもその公開は失敗しません。モジュールのランタイムが使うのは `transitions`、`programs`、`fragments`、
`modules` のストアです。モジュールの Program には与えるツールがないので、`grants` はアダプターをリンクする C++ コードのためのものです。

```js
import { installStoreBridge, registerStores, releaseStores, memoryStores } from './stores.mjs';

installStoreBridge(module);                       // once per module instance; load() does it for neograph.mjs
const handle = registerStores(module, {           // each member is optional
  transitions, programs, grants, fragments, modules,
});
// C++: neograph::wasm::JsStoreSet stores = neograph::wasm::adapt_js_stores(handle);
releaseStores(module, handle);
```

`wasm/js_stores.h` は `adapt_js_stores(handle, lease)` を宣言していて、これは `shared_ptr` 5 つの `JsStoreSet` を返します（オブジェクトにその種類のストアがなければ、そのメンバーは null です）。
呼び出すたびに同じオブジェクトの上に新しいアダプターを作るので、2 つの集合は同じデータへの 2 人の書き手です。`lease` は、与えられると全アダプターが保持し、最後のアダプターとともに解放されます。
モジュールの `Programs` オブジェクトは、その deleter で登録を終わらせます。`stores.mjs` は JavaScript 側の半分で、`neograph.mjs` の隣にコピーされます。`neograph_wasm_js_stores` をリンクしたモジュールは、
ブリッジが呼ぶ関数（`ng_store_resolve`、`ng_store_reject`、`ng_store_alloc`、`ng_store_free`）と、参照ストアの関数（`ng_memory_*`）を export します。

**呼び出し。** ストアのメンバーは、C++ の操作名を camelCase にしたメソッドを持つオブジェクトで、メソッドは C++ の操作の引数を同じ順序で受け取ります。メソッドは値か、その値の Promise を返します。

- thenable でない値は、待っているコルーチンを中断せず、ホストのループのターンも使わずに、呼び出しをすぐに完了します。同期の呼び出し側（`Blocking...Store`）はそのとき待てます。
- Promise はコルーチンを中断します。Promise が確定するとブリッジが task を post し、コルーチンはループの 1 ターンの中で、自分の executor 上で続きます。`set_js_store_executor(executor)` は完了を
  別の executor に送らせます。既定では、待っているコルーチンが動いている executor です。
- 引数は、コルーチンが中断しうるより前に呼び出しの中へコピーされます。
- Promise が保留中に待っていたコルーチンが破棄された場合（ループが閉じられた、実行が取り消された）、その待ちも一緒に消えます。あとから届いた結果には受け取る相手がいないので、捨てられます。
- 呼び出しは直列化されません。1 つが保留中でも、ほかの呼び出しができます。

**失敗。** 投げられた例外や拒否された Promise は、待っている呼び出しから投げられる C++ 例外になり、ランタイムはそれを失敗したストアとして扱います（fail closed）。JavaScript エラーのメッセージ、
または拒否値を文字列にしたものを持つ `std::runtime_error` です。`kind` プロパティを持つエラーは、代わりに C++ のクラスを指定します。`invalid_argument`、`out_of_range`、`domain_error`、
`length_error`、`logic_error`、`runtime_error` のいずれかで、それ以外の `kind` は無視されます。インメモリストアが空の所有者スコープを `std::invalid_argument` で拒否するように、独自の理由で呼び出しを
拒否するストアは、この方法で知らせます。パースできない値や形の違う値は、メソッド名を含むメッセージ（`JavaScript store method 'transitions.load' returned a malformed value: ...`）つきの同じ失敗です。
アダプターは、失敗した呼び出しに既定値を返すことはありません。

**値。**

| C++ | JavaScript |
|---|---|
| `std::string`, `std::string_view` | 文字列 |
| `std::uint64_t`（シーケンス、世代、個数） | 数値: 2^53 未満の非負整数 |
| `bool` | 真偽値 |
| `std::optional<T>` | `T` の値、または `null`（`undefined` は `null` として読みます） |
| `std::vector<T>` | 配列 |
| `ProgramTransitionPublishResult` | `"published"`, `"already_present"`, `"conflict"` |
| `ProgramActivationResult` | `"activated"`, `"already_present"`, `"conflict"` |
| `ProgramCoreToolGrantAdmission` | `"admitted"`, `"already_present"`, `"conflict"` |
| `TaskGraphPublishResult` | `"published"`, `"already_present"`, `"conflict"`, `"missing"` |
| `ModuleLifecycle` | `"active"`, `"deprecated"`, `"quarantined"`, `"revoked"` |
| `ProgramRunRecord`, `ProgramJournalRecord`, `ProgramEvent`, `ProgramEffectOutboxEntry`, `ProgramJavaScriptCommandJournalEntry`, `HookOutboxEntry`, `ProgramChildSynthesisRecord`, `MigrationPlan`, `ProgramRunLineage`, `ProgramRunGeneration`, `ProgramTransitionPublication`, `GraphMigrationCapsule`, `ProgramExecutionLease`, `ProgramRuntimeStateTransferReceipt`, `ContextEpoch`, `ContextArtifact`, `ProgramBundle`, `ProgramVersion`, `ProgramActivation`, `TaskGraphFragmentRecord`, `ProgramModule` | 文字列: 正規テキスト、つまり `serialize_canonical()` で、その型の `parse(bytes)` が読み戻します。ホストはそのまま保存し、そのまま返します。 |
| `ProgramRunPublicationHead` | `{ run_record, journal_record }` |
| `ProgramCommandPublicationHead` | `{ run_record, journal_record, latest_command }`。`latest_command` はコマンドジャーナル項目または `null` です |
| `ProgramContextPublication` | `{ epoch, artifacts: [...], assembly_receipt }`。それぞれ正規テキストです |
| `ProgramEffectiveRuntimeState` | `{ context_publications: [ProgramContextPublication...], hook_outbox_entries: [...], inherited_hook_outbox_entries: [...], transfer_receipt }`。`transfer_receipt` は正規テキストまたは `null` です |
| `ProgramActivationBinding` | `{ activation, version }`。それぞれ正規テキストです |
| `ProgramRetentionReport` | `{ versions_removed, bundles_removed, references: [{ version_id, reason }...] }` |
| `ProgramCoreToolGrantRecord` | `{ owner_scope, program_version_id, run_id, operation_id, attempt, binding_fingerprint, grant_id }` |
| `ProgramCoreToolGrantContext` | `grant_id` のない同じ形 |

オブジェクトは、挙げたメンバーだけを正確に持ちます。足りないメンバーや余分なメンバーは不正な値です。

**メソッド。** `ownerScope`、`runId` などの名前は文字列です。必須と記したメソッドは存在しなければなりません。ないと `adapt_js_stores` が
その名前を添えて `std::invalid_argument` を投げます。省略できるメソッドがないときは C++ インターフェースの既定の動作のままで、最後の列に書きました。

| メンバー | メソッド | 結果 | ないとき |
|---|---|---|---|
| `transitions` | `load(ownerScope, runId)` | run レコードまたは `null` | 必須 |
| | `latest(ownerScope, runId)` | ジャーナルレコードまたは `null` | 必須 |
| | `loadRunPublicationHead(ownerScope, runId)` | 公開ヘッドまたは `null` | `load` と `latest` から読みます。ヘッドが動いていないことを確かめるため、もう一度読みます |
| | `loadEvents(ownerScope, runId, afterSequence)` | イベントの一覧 | 必須 |
| | `loadEffects(ownerScope, runId, afterSequence)` | エフェクトアウトボックス項目の一覧 | 必須 |
| | `loadJavascriptCommands(ownerScope, runId, afterSequence)` | コマンドジャーナル項目の一覧 | 失敗します |
| | `loadCommandPublicationHead(ownerScope, runId)` | コマンド公開ヘッドまたは `null` | `load`、`latest`、`loadJavascriptCommands` から読みます（そのため、それらと一緒に失敗します） |
| | `loadContextPublications(ownerScope, runId, afterSequence)` | コンテキスト公開の一覧 | 失敗します |
| | `loadHookOutboxEntries(ownerScope, runId)` | フックアウトボックス項目の一覧 | 失敗します |
| | `loadEffectiveRuntimeState(ownerScope, runId)` | 有効なランタイム状態 | コンテキスト、フック、run、系譜、世代の読み取りから読みます |
| | `loadChildSyntheses(ownerScope, runId)` | 子の合成レコードの一覧 | 失敗します |
| | `loadMigrationPlan(ownerScope, runId)` | マイグレーション計画または `null` | `null` |
| | `loadLineage(ownerScope, lineageId)` | 系譜ヘッドまたは `null` | `null` |
| | `loadRunLineage(ownerScope, runId)` | 系譜ヘッドまたは `null` | `null` |
| | `loadLineageHead(ownerScope, lineageId, headId)` | 系譜ヘッドまたは `null` | `null` |
| | `loadGeneration(ownerScope, lineageId, generation)` | 世代または `null` | `null` |
| | `loadGenerationInitialPublication(ownerScope, lineageId, generation)` | 公開または `null` | 失敗します |
| | `loadGraphMigrationCapsule(ownerScope, runId, lineageHeadId)` | カプセルまたは `null` | `null` |
| | `loadExecutionLease(ownerScope, runId)` | リースまたは `null` | `null` |
| | `comparePublish(ownerScope, expectedJournalHead, publication)` | 公開の結果 | 必須 |
| | `comparePublishExecution(ownerScope, expectedJournalHead, publication, expectedLease, nextLease)` | 公開の結果。リースは正規テキストまたは `null` です | `"conflict"` |
| `programs` | `publishAdmitted(bundle, version)` | なし | 必須 |
| | `getBundle(id)`, `getBundle(ownerScope, id)` | バンドルまたは `null` | 必須 |
| | `getVersion(id)`, `getVersion(ownerScope, id)` | バージョンまたは `null` | 必須 |
| | `getActivation(ownerScope)` | アクティベーションまたは `null` | 失敗します |
| | `getActiveBinding(ownerScope)` | アクティベーションバインディングまたは `null` | `getActivation` と `getVersion` から読みます |
| | `compareActivate(ownerScope, expectedGeneration, versionId, policySnapshotHash)` | アクティベーションの結果 | 失敗します |
| | `listVersions(ownerScope)` | バージョンの一覧 | 失敗します |
| | `collectGarbage(ownerScope, pinnedVersionIds)` | 保持レポート | 失敗します |
| `grants` | `admit(record)` | 承認 | 必須 |
| | `load(context)` | grant レコードまたは `null` | 必須 |
| | `revoke(context)` | 真偽値: grant を取り消したかどうか | 必須 |
| `fragments` | `publish(record)` | 公開の結果 | 必須 |
| | `load(fragmentId)` | フラグメントレコードまたは `null` | 必須 |
| | `findPublishedLineage(ownerScope, parentRunId, expansionOperationId)` | フラグメントレコードまたは `null` | `null` |
| | `findPublished(ownerScope, parentRunId, expansionOperationId, proposalHash)` | フラグメントレコードまたは `null` | `null` |
| | `compareUpdate(fragmentId, expectedRevision, record)` | 公開の結果 | 必須 |
| `modules` | `publish(module)` | なし | 必須 |
| | `get(contentIdentity)`, `get(ownerScope, contentIdentity)` | モジュールまたは `null` | 必須 |
| | `setLifecycle(contentIdentity, lifecycle)` | なし | 必須 |



- **1 つのメソッドの 2 つの形。** `getBundle`、`getVersion`、`get` は、C++ にそれぞれ所有者なしの形と所有者つきの形があり、カタログは両方を使います。JavaScript のメソッド 1 つが両方を受け持ち、
  引数の数で区別します。1 つなら識別子、2 つなら所有者スコープと識別子です。片方の引数用に書いたメソッドは、もう片方では間違いになります。
- **公開は、渡す前に検査されます。** `comparePublish` と `comparePublishExecution` は、遷移ストアのどのバックエンドとも同じように始まります。保存された状態を必要としない検査（公開の所有者、
  期待するヘッドの形、エフェクトのアウトボックス、公開の正規テキスト）です。これに通らない公開は `"conflict"` になり、オブジェクトには届きません。保存された状態がその公開について何を言うかは、
  ストアが決めます。
- **非対応。** `compare_publish_graph_safe_point` はシリアライズされない証拠を運ぶため、JavaScript ストアはライブのグラフマイグレーションには参加しません。基底クラスと同じく `"conflict"` を返します。

**参照ストア。** `neograph.memoryStores()`（`stores.mjs` の `memoryStores(module)`）は、モジュールのインメモリ C++ ストアを呼び出してこの契約を実装する
`{ transitions, programs, grants, fragments, modules }` を返します（`wasm/memory_stores.cpp`）。引数はアダプターがエンコードするのと同じ表でデコードし、ストア自身の操作を実行し、結果はアダプターが
デコードする表でエンコードします。メソッドは Promise ではなく値を返し、インメモリストアの拒否は `kind` を通じて同じ C++ 例外クラスとしてアダプターに届きます。ライブラリには grant ストアのインメモリ形が
ないので、参照ストアは SQLite の grant ストアの判断に従う小さな実装を持っています。`stores.release()` は集合を捨てます。その上の実行が終わったとき（`result` が確定したあと）に行ってください。捨てられた集合への呼び出しは失敗するからです。

永続的なストアが欲しいホストは、規則を書き直す代わりに参照ストアをラップします。すべてのメソッドは参照ストアに委譲し、何かを変える呼び出しの引数はホスト自身のストレージに書き、起動時にラッパーがそれを
順番に新しい参照ストアへ再生します。参照ストアは各呼び出しを、その引数とそれ以前の状態から決めるので、再生すればログが表す状態に行き着きます。下のラッパーは、参照ストアが決めたあと、結果を返す前にログへ追記します。その間のクラッシュで呼び出しを失ってはならないホストは、ログ項目を先に書き、呼び出しをそのあとで適用します。
`test/program_stores_test.mjs` は、Program の実行全体についてこれを行います。

```js
const CHANGES = new Set(['transitions.comparePublish', 'transitions.comparePublishExecution', 'programs.publishAdmitted',
  'programs.compareActivate', 'programs.collectGarbage', 'grants.admit', 'grants.revoke', 'fragments.publish',
  'fragments.compareUpdate', 'modules.publish', 'modules.setLifecycle']);

function durable(reference, log) {                          // log.append(entry) is the host's storage
  const wrapped = {};
  for (const [member, target] of Object.entries(reference)) {
    wrapped[member] = {};
    for (const [method, call] of Object.entries(target)) {
      wrapped[member][method] = async (...args) => {
        const result = call(...args);                       // the reference store decides
        if (CHANGES.has(`${member}.${method}`)) await log.append({ member, method, args });
        return result;
      };
    }
  }
  return wrapped;
}
// On start: for (const { member, method, args } of log.entries()) fresh[member][method](...args);
```


## 例

- `examples/node/chat.mjs`: Node からの 1 回の呼び出しです。
  `API_KEY=... node wasm/examples/node/chat.mjs <base url> <path> <model> "<prompt>" [--stream]`。
- `examples/node/program.mjs`: モデル呼び出し 2 回とその間の一時停止を持つ Program を Node から実行します。
  `API_KEY=... node wasm/examples/node/program.mjs <base url> <path> <model> "<prompt>" [--answer <text>]`。
  最初の返答をトークン単位で出力し、答え（フラグ、または stdin の 1 行。空の答えは実行を取り消します）を待ち、その答えで会話を続けます。
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
- `test/program_test.mjs`: 同じサーバーに対する Program API。Program をコンパイルして承認し（拒否されたソースは理由を伝えます）、実行はイベントをストリームし、
  割り込みで止まってから再開されます。fork は最初の呼び出しを聞き直さずに同じ一時停止へ別の答えを返し、一時停止した実行は取り消されてストアから読み戻され、
  同時に進む実行はそれぞれの答えを守り、`AbortSignal` はモデル呼び出しの接続を閉じ、`onEvent` が投げたエラーはその実行を取り消します。送る前に取り消された fork は取り下げられ、すでに中断された signal は何も始めず、終わらない制御ソースは時間制限が止め、深すぎる再帰をするソース（`define()` とソースの 17 の形、そして `main()`）は診断つきで失敗し、その後もモジュールが Program を承認して実行できるようにします。失敗のままでなければならない失敗も確かめます。
  カタログに上限がないモデルは呼び出されず、呼び出しをまかなえない予算は呼び出しを送る前に実行を使い切らせ、バージョンが承認する範囲を超える予算は拒否され、キーはどの結果にもイベントにもありません。
  Node の例は子プロセスとして実行します。
- `test/program_stores_test.mjs`: モジュールの `stores` オプション。参照ストアは、そのままでも、後の task で答えるようにラップしても、ライフサイクル全体をこなします（呼び出しの合間にホストのタイマーは動き続けます）。
  拒否された呼び出し、投げられたエラー、値でない値は、ストアのメッセージとメソッド名つきで fail closed し、モデルは呼び出されません。必須メソッドのないオブジェクトは拒否されます。
  変わったことをログに書くラッパーのおかげで、ログから再生したストアの上の 2 つ目のオブジェクトが、終わった実行を読み戻し、止まった実行がまだ止まっていることを確かめ、最初のオブジェクトの Program を始めます。
  ストアがまだ答えている間に取り消された fork は取り消しで終わり、綴りを間違えたメンバーと足りない系譜の読み取りメソッドは拒否され、オブジェクトを解放すると、実行中の run は取り消され、待っている run はそのまま残ります。
- `test/workers_test.mjs`: Workers の例がエクスポートする `fetch` ハンドラーを、同じサーバーに対して `Request` を渡して
  呼び出します。バッファ済みとストリーミングの両方です。ハンドラーのロジックとプラットフォーム API の使い方を示すもので、
  workerd での実行ではありません。
- `checks.cpp`（下記）は、ホストが回すプロバイダーに対する同期の実行を行い、拒否されること、何も送られなかったことを
  確認します。
- `checks.cpp` は `HostLoop` の上に `ProgramRuntime` も作ります。実行器のないランタイムと同期の `start` は拒否され、
  インメモリのストアを使うノード 1 つの Program がホストのターンで結果まで実行されます。
  続いて、ホストのターンで awaitable なストアから run とバージョンを読み戻し、ブロッキングビューがループ上での待機を拒否することを示します。
  また、Program ストアと遷移ストアが呼び出しをループの後のターンでしか完了しない状態で Program を実行し、そのようなストアでカタログを読みます。
- `tests/test_program_runtime_executor.cpp` は、シングルスレッドの `io_context` と手動の実行器の上の `HostLoop` で Program を
  ネイティブに実行するので、ネイティブビルドも同じ経路を扱います。
- `tests/test_program_host_pumped_stores.cpp` は、呼び出しをループの後のターンでしか完了しないストアの上の `HostLoop` で、Program の
  ライフサイクル全体をネイティブに実行します。開始、モデルノード、中断と再開、フォーク、子、Core ツール grant、完了、キャンセルを扱い、
  何もブロックされず、拒否もされません。
- `tests/test_program_store_awaitable.cpp` は、ストアの契約をネイティブで実行します。インラインのストアは中断せずに完了し、
  プールに回すストアは待っているコルーチンを自分の executor で再開し、ブロッキングビューはあとで完了するストアを待つか拒否します。
- `tests/test_host_loop.cpp` は、手動の executor を相手にループとホストシームのプロバイダーをネイティブで実行するので、
  ネイティブビルドも同じ経路を扱います。
- `test/store_conformance_test.mjs` は、`tests/` のストア適合性スイート（`ProgramTransitionStoreConformance`、`ProgramStoreConformance`、グラフマイグレーションカプセルの行）を、
  JavaScript ストアに対して Node で実行します。モジュールは `-DNEOGRAPH_BUILD_WASM_STORE_TESTS=ON`（googletest を取得します）で、ターゲット `neograph_wasm_store_conformance` からビルドします。
  JavaScript バックエンドを持つスイートは、すべてのケースに参照ストアの上のアダプターを通したストアを渡し、2 つの形で実行します（`JavaScript` は同期の呼び出し側が使う操作を、
  `JavaScriptAwaited` は awaitable な操作を呼びます）。スレッドで 2 人の書き手を競わせるケースは、1 本のスレッドの上で交代して動かします。スクリプトはバックエンドごとの件数とスキップした
  すべてのケースの理由を出力し、失敗があれば 0 以外で終了します。
- `test/js_stores_test.mjs` は、後の task で確定する Promise を返すメソッドを持つストアで、ターゲット `neograph_wasm_store_checks` を動かします。Promise はループの 1 ターンの中で待っていたコルーチンを
  再開し、保留中は何も待ちません。Promise でない値はターンを使いません。拒否、throw、パースできない値は、メソッド名を含む C++ 例外になります。両方を混ぜたオブジェクトも動きます。コルーチンが破棄または
  取り消されたあとに届いた結果は捨てられます。必須メソッドがなければ `adapt_js_stores` は失敗します。Program 全体が Promise ストアの上で動き、拒否するストアの上では fail closed します。

<a id="measured-sizes"></a>
## 測定したサイズ

Release、`-O3 -flto`、Emscripten 6.0.13。グラフ smoke には Core エンジンと SDK ランタイムが含まれ、グラフ専用のモジュールはそこに
LLM コンポーネント、SDK の JavaScript ホスト、C API を加えたものです。残りの大部分は Program コンポーネントです。Program 付きのモジュールは
グラフ専用のモジュールの 2 倍の大きさで、そのうち QuickJS が約 1 MB です。

| ビルド | `.wasm` | `.wasm` gzip -9 | `.js` |
|---|---:|---:|---:|
| グラフ smoke、シングルスレッド | 5,615,386 B (5.4 MiB) | 1,642,238 B (1.6 MiB) | 83,223 B |
| グラフ smoke、pthread | 5,448,684 B (5.2 MiB) | 1,660,246 B (1.6 MiB) | 103,691 B |
| Program、LLM、QuickJS を含む checks、シングルスレッド | 12,495,665 B (11.9 MiB) | 3,741,204 B (3.6 MiB) | 91,040 B |
| JavaScript モジュール、グラフ専用（`NEOGRAPH_WASM_JS_PROGRAMS=OFF`）、シングルスレッド | 6,466,458 B (6.2 MiB) | 1,893,168 B (1.8 MiB) | 87,762 B |
| Program 付き、QuickJS なしの JavaScript モジュール、シングルスレッド | 12,148,715 B (11.6 MiB) | 3,551,238 B (3.4 MiB) | 95,154 B |
| Program と QuickJS 付きの JavaScript モジュール、シングルスレッド | 13,209,571 B (12.6 MiB) | 3,931,479 B (3.7 MiB) | 96,889 B |

Program と QuickJS 付きのモジュールを `-DCMAKE_CXX_FLAGS_RELEASE="-Oz -DNDEBUG"`（C も同様）でビルドすると 10,378,540 B、gzip 後 3,122,630 B です。
速度は測っていません。

CI はシングルスレッドの smoke を `.wasm` 6,000,000 バイト、gzip 後 1,750,000 バイト（測定したサイズに約 9% を足した値）で、
モジュール（CI は Program と QuickJS 付きでビルドします）を `.wasm` 14,400,000 バイト、gzip 後 4,300,000 バイト で制限し、サイズをジョブのサマリーに報告します。Worker のスクリプトにはプランに応じたサイズ制限が
あるので、モジュールの圧縮後のサイズをご利用のプランの制限と比べてください。このページの以前の版にあった 712 KB は、型付き SDK より
前に測った値で、今は当てはまりません。

## これらのホストで使えないもの

NeoGraph が呼び出しを所有している場合は型付きのエラーで失敗し、そうでなければビルドされません。

- **ホストを通さないモデル呼び出し。** ASYNC、MCP クライアント、A2A にはソケットと TLS が必要です。モデル呼び出しは上の
  モジュールでホストの `fetch` を通します。別の WebAssembly ホスト向けのビルドは、自前の executor とトランスポートを
  `SchemaProvider` に渡します。
- **ホストが回すプロバイダーに対するブロッキングのエントリーポイント。** `run()`、`Provider::dispatch` などは拒否します（上記）。
- **ホストのデータベース上の Program ストア。** ストアのインターフェースは awaitable です。SQLite と PostgreSQL のストアはシステムライブラリを必要とするため、ホストのデータベースには自前の
  ストアが必要です。操作がすぐ完了するストア（ホストの同期ストレージ）も、ホストのイベントループを通してしか完了しないストアも、
  `_async` 形式を通して、ホストループ上のランタイムで使えます（[#557](https://github.com/fox1245/NeoGraph/issues/557)）。
  同期の Program 呼び出しはホストループで拒否されるので、`_async` 形式を使ってください。JavaScript モジュールは、そのようなストアを JavaScript オブジェクト（`stores`、上記）として受け取ります。
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

`smoke.html` は、`serve_smoke.py` で配信すると `http://127.0.0.1:8765/smoke.html` で生成済みの `smoke.js` を実行します。

```bash
python3 wasm/serve_smoke.py build-wasm/wasm --port 8765
```

`serve_smoke.py` は `Cross-Origin-Opener-Policy: same-origin` と `Cross-Origin-Embedder-Policy: require-corp` を返します。
`smoke.html` はクロスオリジン分離と `SharedArrayBuffer` を確認してから `/smoke.js` を読み込み、プログラムが
`doubled = 42` と `trace = d` を出力して標準エラー出力なしに終了コード `0` で終わると、`document.body.dataset.result = "pass"`
と `document.body.dataset.exitCode = "0"` を設定します。既定のシングルスレッドビルドをヘッドレス Chromium 140.0.7339.186 で
確認済みです（`doubled = 42`、`trace = d`、終了コード `0`）。pthread ビルドは Node のみで実行し（`-sPROXY_TO_PTHREAD` なしでは
`main` がメインスレッドを塞ぎます）、CI が実行するのは Node のみで、JavaScript モジュール（`neograph.mjs`）はブラウザーでは
実行していません。

## マイルストーン

ツールチェーン調査後の計画は [#535](https://github.com/fox1245/NeoGraph/issues/535) にあります。

- **M0 と M1。** wasm32 の修正、シングルスレッドビルド、CI での Node smoke とサイズゲート。
- **M2（このページ）。** ホストの `fetch` を通すモデル呼び出し。ホストループ、ホストシームのプロバイダー、JavaScript モジュール、
  Node と Workers の例。
- **M3.** ホストのストア上の Program（[#557](https://github.com/fox1245/NeoGraph/issues/557)、
  [#556](https://github.com/fox1245/NeoGraph/issues/556)）。ランタイム側は完了しました。`ProgramRuntime` が
  `HostLoop::executor()` の上で動きます（上の「ホストループ上の Program」）。ストアのインターフェースも awaitable になりました。JavaScript の表面もそうです（上の「モジュールの Program」）。
- **M4.** リアルタイムセッション、WASI、ネイティブアーカイブ用の保管バックエンド、リモート Harness。
