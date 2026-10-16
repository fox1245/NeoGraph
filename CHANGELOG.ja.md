<!-- neograph-i18n: source=CHANGELOG.md locale=ja source_sha256=3ec0aeca50cfc4308b63cdf7c9d5c91f6d46fd74042d784e1de1fbbea2d6bb65 -->
<!-- neograph-i18n: source=CHANGELOG.md locale=ja source_sha256=eccdfd77485685d085575f9bf352ac89afc251515fbfcdb1677a340561fbaa73 -->
# 変更履歴

**Languages:** [English](CHANGELOG.md) | [한국어](CHANGELOG.ko.md) | [日本語](CHANGELOG.ja.md) | [简体中文](CHANGELOG.zh-CN.md)

以下の日付付き項目は過去の API と測定を保持します。旧 Provider 互換保証は [0.14.0] の breaking C++ typed 契約に置き換わり、現在の API 案内ではありません。

NeoGraph に対するすべての重要な変更は、このファイルに記載されています。

形式は [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) に従います。バージョニングは [Semantic Versioning](https://semver.org/spec/v2.0.0.html) に従います。

---

## [未リリース]

### 変更

- **C++ の利用側を再ビルドしてください。`WsClientOptions` に member が、`A2AServer` と `WsClient` に関数が追加されました（#456, #458）。** `neograph::async::WsClientOptions` に `handshake_timeout` と `read_timeout` が追加されて layout が変わり（`ws_connect` に値で渡されます）、`A2AServer` に `set_trust_forwarded_headers` が、`WsClient` に private member 関数が追加されました。NeoGraph、C++ の利用側、Python 拡張をまとめて再ビルドしてください（[`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照）。
- **SchemaProvider `91fb993e50e49e3b03d290b170c83cf57afbc518`（`maint/interface6` の SDK 0.3.2）を採用します。SchemaProvider #33 を修正し、Google の OpenAI 互換エンドポイントからのストリーミングが再び動作します（#459）。** Google の `https://generativelanguage.googleapis.com/v1beta/openai/chat/completions` は、最後を除くすべてのストリーミングチャンクで `finish_reason` を省略しますが、SDK の Chat コーデックはそのチャンクを拒否していました。そのため、NeoGraph が provider 層を SchemaProvider に移した 0.13.0 以降、このエンドポイントからの 2 チャンク以上の `openai.chat` ストリームはすべて `ProtocolCorrupt`（"invalid response protocol"）で終わっていました。SDK は今後、ストリームが開いている間は欠けた member を `null` として読みます。ストリームには引き続き `[DONE]` の前に文字列の `finish_reason` が必要で、member のない buffered choice は引き続き拒否されます。OpenRouter、ネイティブの `google.generate` ファミリー、buffered 呼び出しは影響を受けていませんでした。interface revision と shared-library generation は 6 のままで、public な struct や enum の layout は変わりません。NeoGraph はビルドとインストールされた `NeoGraphConfig` の `find_dependency` で `SchemaProvider 0.3.2 EXACT` を要求するようになり、インストール済みの 0.3.1 パッケージは一致しなくなります。NeoGraph、C++ の利用側、Python 拡張を 0.3.2 のヘッダーとライブラリでまとめて再ビルドしてください（[`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照）。
- **他言語で実装する側への gRPC 契約の注記（#453）。** proto と reference は、0.14 がすでに要求していたことを明記するようになりました。`CheckpointService` は `RequiresManagedBudget` を含むすべての RPC を提供しなければなりません。エンジンは新規実行と再開のたびにこれを呼び、`UNIMPLEMENTED` なら実行を失敗させるため（管理された budget がないとはみなしません）、0.13 の RPC セットで書いたサーバーには、budget を管理しない場合に `required = false` を返す `RequiresManagedBudget` が必要です。`RunGraphRequest.thread_id` は必須と記載しました（空の値は 0.14 から `INVALID_ARGUMENT` で拒否されています）。0.14 から gRPC ヘッダーが gRPC 自身のヘッダーを含むため、`using namespace neograph;` があるコードでは NeoGraph の型を `neograph::grpc::...` と書く必要があります。そこでは裸の `grpc::` が曖昧になります。ドキュメントのみの変更で、挙動の変更はありません。
- **同梱の cpp-httplib を 0.41.0 から 0.61.0 に更新しました（#460）。** cpp-httplib を使うのは A2A サーバー、MCP Streamable HTTP サーバー、テスト用のピアだけで、NeoGraph の HTTP クライアントは libcurl と Asio を使います。`deps/httplib.h` は再び無変更の upstream リリースです。以前のヘッダーにあった MSVC 向けのローカル修正（`X509_get_subject_name` は `const X509_NAME *` を返す）は 0.44.0 から upstream に入っており、#450 の backport 2 件は 0.61.0 に含まれています。サーバーは 0.41.0 以降に upstream が強化したリクエスト解析を得ます。`Content-Length` が不正なリクエスト、request-target や chunk-size 行に制御バイトを含むリクエスト、`Host` フィールドが 2 つ以上あるリクエストには 400 で応答して接続を閉じます。パイプライン化されたリクエストは keep-alive のタイムアウトを待たずに処理され、コールバックが投げた例外はプロセスを終了させずにその接続だけを切り、listen backlog は 5 ではなく 128 になりました。**挙動の変更:** cpp-httplib 0.44.0 からリクエストヘッダーの値をパーセントデコードしないため、ヘッダーの値は送られたとおりにサーバーへ届きます。サーバーが確認する `Authorization`、`Origin`、`Mcp-Session-Id` などのヘッダーはバイト単位で比較され、こうした値をパーセントエンコードして送るクライアントは一致しなくなります。

### 追加

- **組み込みの gRPC サーバーが標準の health サービスに応答し、停止できるようになりました（#454）。** `run_server`、`run_tool_server`、`run_checkpoint_server` は `grpc.health.v1.Health` を提供します。呼び出しを受け付けている間はサーバーとそのサービス（`neograph.v1.GraphService`、`neograph.v1.ToolService`、`neograph.v1.CheckpointService`）に `SERVING` を、停止後は `NOT_SERVING` を返すため、Kubernetes の gRPC probe や `grpc-health-probe` が使えます。health サービスに関する gRPC のプロセス全体の既定値は、組み込みサーバーの起動中だけオンにして元に戻すため、ホスト自身のサーバーには影響しません。新しい overload は `ServeOptions`（`<neograph/grpc/serve_options.h>`）を受け取ります。`std::stop_token` が要求されると実行中の呼び出しのために `shutdown_grace`（既定 5 秒）の後でサーバーを止め、`on_serving` コールバックはバインドしたポートを受け取るので、アドレスにポート 0 を指定できます。サーバーリフレクションはホストが選びます。`gRPC::grpc++_reflection` をリンクし、サーバーの起動前に `grpc::reflection::InitProtoReflectionServerBuilderPlugin()` を呼ぶと、gRPC は組み込みサーバーを含むすべてのサーバーにリフレクションを追加します。既存のシグネチャは変わりません。

### 修正

- **サーバーが GET イベントストリームを開いたままでも `MCPClient::shutdown()` が戻ります（#448）。** Streamable HTTP では、クライアントは `initialize` の後に任意の GET イベントストリームを開きます。サーバーがセッションの `DELETE` の後もそのストリームを開いたままにすると、`shutdown()` とデストラクタはサーバーが閉じるまで、場合によっては永遠に待っていました。公式の Python・TypeScript サーバーは `DELETE` でストリームを閉じるため問題が隠れており、conformance suite の `sse-retry` シナリオは閉じないため終了時にタイムアウトしていました。ストリームが終わった直後には、再接続前の待機、つまりサーバーが指定した `retry` の分も待っていました。listener の停止シグナルがキャンセルされたタイマー待ちで、`awaitable_operators::operator||` はそれを失敗と数えてストリームの読み取りや待機を待ち続けていたためです。停止待ちはタイマーがキャンセルされても満了しても正常に完了するようになり、shutdown はタイマーを満了させるので、まだ待ち始めていない listener も止まります。API の変更はありません。
- **MCP HTTP クライアントが、1 つの HTTP チャンクで届いた 1 MiB を超える SSE イベントを読み、エンドポイントと同じ origin の 307・308 リダイレクトに従います（#449）。** Node や uvicorn のサーバーのようにイベントごとに 1 チャンクで書くサーバーで、1 MiB を超えるイベントが 1 チャンクで届いた応答ストリームは、クライアントが 16 MiB までのイベントを受け付けるにもかかわらず `async_post_stream: response chunk limit` で失敗していました。チャンクはイベントと同じ大きさまで許されるようになりました。`mcp` 1.x の FastMCP サーバーが `POST /mcp` を `/mcp/` に移すときに返すような `307`・`308` の応答はエラーでした。クライアントは `Location` がエンドポイント URL と同じ origin（同じ scheme、host、port。相対 `Location` も含む）の場合に限り、最大 5 回まで、メソッド・本文・ヘッダーをそのまま `Location` に送り直します。https から http へのものを含め、別の origin へのリダイレクトには従わず、そこへは何も送りません。呼び出しは以前と同じく `307`・`308` で失敗し、エラーメッセージはリダイレクトに従わなかったことを伝えます。`301`、`302`、`303` には従いません。**挙動の変更:** 同じ origin の中でリダイレクトするサーバーに、HTTP 307・308 で失敗せず到達するようになりました。
- **MCP HTTP サーバーが本文のない `DELETE` に応答した後、接続を閉じます（#450）。** 公式クライアントはセッションの `DELETE` を `Content-Length` なしで送ります。同梱の cpp-httplib はハンドラーの後でこのリクエストに本文があるものとみなし、クライアントが閉じるか読み取りがタイムアウトするまで本文を読もうと接続を開いたままにし、その接続の次のリクエストを本文として読むおそれもありました。また `Connection: close` と述べた応答の後も接続を開いたままにしていました。同梱の cpp-httplib を 0.61.0 に更新し（#460）、両方を修正しました。`Content-Length` も `Transfer-Encoding` もないリクエストには読み捨てる本文がなく（RFC 9112 §6.3）、`Connection: close` 付きの応答は接続を閉じます（RFC 9112 §9.6）。**挙動の変更:** いまも同梱の cpp-httplib で作られている 2 つの NeoGraph サーバー、A2A サーバーと MCP Streamable HTTP サーバーは（NeoGraph の HTTP クライアントは libcurl と Asio を使うため影響はありません）、`Connection: close` 付きの応答の後に接続を閉じるようになりました。cpp-httplib はすべての `4xx`・`5xx` 応答にこのヘッダーを付けます。このヘッダーに従うクライアントは、NeoGraph 自身を含め、すでに新しい接続を開いていました。
- **gRPC: `RunGraphStream` が投げられた例外の文言を送らなくなりました（#451）。** ノードが例外を投げると、ストリームはエンジンの `ERROR` イベントをそのまま `DEBUG` イベントとして転送していたため、例外の `what()` がサニタイズされた失敗より先にクライアントに届いていました。unary の `RunGraph` は公開メッセージだけを送っていました。リトライを含むノードのエラーは、`error` が固定文言 `node failed` の `DEBUG` イベントになり（`attempts`、`handled`、`routed_to`、`retry`、`max_retries`、`delay_ms` は保持されます）、実行の失敗イベント（公開メッセージを持つ `{error, code}`）と最終 status は変わりません。**挙動の変更:** ストリームイベントから例外の文言を読んでいたクライアントは、そこで `node failed` を受け取ります。
- **gRPC: `RunGraphStream` が `stream_mode` に従い、状態のスナップショットは `VALUES` になりました（#452）。** `RunGraphRequest.stream_mode` が無視されていたためすべてのストリームがすべてのイベントを運び、状態全体のスナップショット（ノード `__state__`）が `UPDATES` として送られていたため `VALUES` は一度も送られていませんでした。`StreamMode` のビット（`EVENTS=1`、`TOKENS=2`、`VALUES=4`、`UPDATES=8`、`DEBUG=16`。0 は以前どおりすべてのイベントを送り、未知のビットは無視します）が適用され、`TOKEN` イベントは `TOKENS` があるときだけ送られ、状態のスナップショットは `VALUES` イベントになりました。proto に記載したとおり、interrupt とノードのエラーは `DEBUG` イベントのままです。`FINAL` はモードにかかわらず送られます。**挙動の変更:** `stream_mode` を指定したリクエストは要求したイベントだけを受け取り、`UPDATES` イベントから状態のスナップショットを読んでいたクライアントは `VALUES` イベントで見つけることになります。
- **gRPC `CheckpointService`: サーバーがそのまま保存できない記録は、切り詰めずに拒否します（#453）。** サーバーは保存前に `checkpoint_json` を `Checkpoint` に解析するため、サーバーの知らないトップレベルの member（より新しいエンジンや別の実装が書いたもの）が黙って捨てられていました。`SaveCheckpoint` と `PublishManagedBudgetCheckpoint` は、そのような記録と、`schema_version` がサーバーより新しい記録を `INVALID_ARGUMENT`（`NG.InvalidRequest`。managed-budget の公開では他の拒否と同じく `FAILED_PRECONDITION`）で拒否し、何も保存しません。`channel_values` と `metadata` の中の自由な JSON は以前どおり保持されます。**挙動の変更:** 捨てられていたトップレベルの member を余分に送っていたクライアントは、エラーを受け取るようになります。
- **A2A: JSON の `null` で送られた任意の member は、存在しないものとして扱います（#455）。** メッセージの解析が、存在するが null の `taskId` や `contextId` を空文字列として読んでいたため、0.3 と 1.0 の両方の dialect で `"taskId": null` は `-32001 Task not found` を返し、`"contextId": null` は空の context id でタスクを保存していました。設定の member にも同じ欠陥がありました。`"blocking": null` は `false` と読まれて send が SUBMITTED のタスクをすぐに返し、`"historyLength": null` は 0 と読まれていました。null の `taskId`、`contextId`、`name`、`description`、`blocking`、`returnImmediately`、`historyLength`、`configuration` は、サーバーでもクライアントでも存在しないものと同じになりました。NeoGraph が失敗し参照サーバーが通過していた、0.3 conformance kit で唯一の quality テスト（`test_null_optional_fields`）でした。**挙動の変更:** これらの member を `null` にしたリクエストは、存在しない場合と同様に成功するようになりました。
- **A2A: TLS を終端するプロキシの背後でのカード URL と、空の `url` の廃止（#456）。** `AgentCard::url` が空のとき、`A2AServer` はクライアントが使ったアドレスを常に `http` scheme で広告していたため、TLS を終端するプロキシの背後ではカード（とその `supportedInterfaces`）が SDK クライアントを平文の HTTP に向かわせていました。`A2AServer::set_trust_forwarded_headers(true)` にすると、RFC 7239 の `Forwarded`（最初の要素）から、なければ `X-Forwarded-Proto` / `X-Forwarded-Host` から scheme と host を取ります。受け付けるのは `http` / `https` ともっともらしい host だけです。クライアントがこれらのヘッダーを自分で送れるため既定ではオフで、設定された `AgentCard::url` が引き続き優先されます。`url` のない `AgentCard` の 0.3 JSON（たとえばレジストリ用に `A2AServer` の外で書いたもの）には `"url": ""` が含まれなくなりました。優先 transport の最初の interface を使い、なければ member を省きます。**挙動の変更:** そのようなカードには、空の `url` の代わりに `url` member がありません。
- **ACP: `$/cancel_request` が実行中の `session/prompt` をキャンセルします（#457）。** サーバーは自分の保留中のクライアントリクエストには `$/cancel_request` を送っていましたが、クライアントからの通知は無視していたため、通知が指す prompt は最後まで実行されていました。`requestId` が実行中の `session/prompt` のものである `$/cancel_request` は、`session/cancel` と同様にその turn をキャンセルし、prompt は引き続き `stopReason: "cancelled"` の応答を 1 つ受け取ります。未知または終了済みのリクエスト、あるいは別のメソッドのリクエストの `requestId` は無視します。**挙動の変更:** prompt に `$/cancel_request` を送るクライアントは、完了した turn ではなく `cancelled` を受け取ります。
- **`WsClient` に handshake の期限と任意の読み取り期限ができました（#458）。** `ws_connect` は、TCP 接続を受け付けて Upgrade リクエストに応答しないサーバーを永遠に待っていました。`WsClientOptions::handshake_timeout`（既定 30 秒）は名前解決、接続、TLS handshake、Upgrade のやり取りを制限し、`WsClientOptions::read_timeout`（既定 0、期限なし。サーバーの push を待つアイドル接続は正常なため）は 1 回の `recv()` を制限します。どちらも `asio::system_error(asio::error::timed_out)` で期限切れになり、frame が途中まで読まれている可能性があるため、読み取りのタイムアウトは接続を閉じます。負の値は `std::invalid_argument` を投げます。**挙動の変更:** 既定のオプションの `ws_connect` は、永遠に待たずに 30 秒後に失敗します。

## [0.14.1] - 2026-10-14

### 変更

- **Linux のホイールを固定したイメージでビルドします（#333）。** `manylinux_2_34` のイメージを 0.14.0 をビルドした digest に固定し、CI のランナーは `-latest` ラベルを使わなくなりました。Linux x86_64 のホイールに同梱したライブラリは、LGPL の `keyutils-libs` と `libxcrypt` を含めて 0.14.0 とバイト単位で同じなので、公開しているライブラリのソースがそのまま当てはまります。
- **C++ の利用側を再ビルドしてください。`RuntimeConfig` に member が、`ProgramHandle` に関数が追加されました（#431）。** `RuntimeConfig` に `core_provider_calls_journaled` が、`ProgramHandle` に `notices()` が追加され、`ProgramNotice` が新しく加わりました。NeoGraph、C++ の利用側、Python 拡張を一緒に再ビルドしてください([`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照)。
- **永続的なプロバイダー呼び出しのジャーナルがないホストでのフォークは、そのことを知らせます（#431）。** フォークは、一時停止した呼び出しのプロバイダー呼び出しを、永続的なプロバイダー呼び出しのジャーナル（`SQLiteProgramProviderCallJournal`。`LocalProgramHost` は `sqlite_directory` があると持ちます）からだけリプレイします。プロセス内のブローカーにはジャーナルがないため、フォークで再び実行されるノードは先に行った呼び出しを再送し、再課金します。この点は変わっていません。**動作の変更:** `RuntimeConfig::core_provider_calls_journaled` が `false`（既定値）で、一時停止した試行が、止まっているオペレーションで価格付きのプロバイダー呼び出しを記録していた場合、`ProgramRuntime::fork` が返すハンドルに、コードが `P_FORK_PROVIDER_CALLS_NOT_JOURNALED` の `ProgramNotice` が付くようになりました（`ProgramHandle::notices()`、Pythonでは `handle.notices`）。`witness` には、それらの呼び出し（`call_id`、`node`、`model`、`microunits`）と合計が並びます。通知はエラーではなく、フォークが送るものも課金するものも変えません。一時停止したステップで終わったノードの呼び出しは、そのステップに保存された書き込みからリプレイされるため、列挙された呼び出しのうちどれをフォークが再実行するかは、通知からは分かりません。通知は実行とともには保存されないので、`reconnect()` や `resume()` が返すハンドルにはありません。resolver が `SQLiteProgramProviderCallJournal` のブローカーを返すホストは、`core_provider_calls_journaled = true` を設定してください（`LocalProgramHost` は `sqlite_directory` があると設定します）。Program ガイド、Python バインディングガイド、リファレンスに記載しました。

### 修正

- **0.14.0 で失った Core の実行経路の速度を大部分取り戻しました（#442）。** 0.14.0 で barrier 停止の判定、失敗ルーティング、リトライ試行、`any_of` スロットを入れたことで、通常のグラフの 1 回の実行が #343 直後より 14〜19% 遅くなっていました。そのため 0.14.0 の「Core エンジンの速度は v0.11.1 の水準に戻った」という記述は、リリースされたタグには当てはまりませんでした。現在は、1 回の実行が管理用データを一度だけ確保し、失敗ルーティングのないグラフはルーティング表を読まず、fan-out は分岐の状態を 1 つのベクターに持ち、ノードの呼び出しは Asio が再利用する 1 つのコルーチンフレームで動きます。アイドル状態のマシンで `bench_neograph 10000 5000 1` を 24 ラウンド交互に測った中央値は次のとおりです。自前の ARM サーバーでは逐次実行 1 回が 11.56 µs から 10.34 µs に、並列実行 1 回が 24.90 µs から 23.04 µs になりました（#343 直後：10.16 µs と 21.91 µs）。x86_64 マシン（Core i7-8750H）では 7.68 µs から 6.78 µs に、18.64 µs から 16.87 µs になりました（#343 直後：6.45 µs と 15.91 µs）。#343 直後との差は 2〜7% 残っています。API と動作の変更はありません。（リリース時に引用した数値がアイドル状態のマシンで再現しなかったため、リリース後に数値を訂正しました。）
- **Program: 金額、Coreのステップ、wall time の許可量を超えたまま終わった実行は、その予算の記録とともに終わります (#443)。** 使用量は plan が完了した後に精算されますが、Coreのステップ、金額、wall time の許可量を超えていると分かった実行は、`failure` のないまま `BudgetExhausted` で終わっていました。そのため、呼び出し側はどの予算を、どれだけ超えたのか分かりませんでした。wall time を超えた実行も、期限に止められた実行は `TimedOut` で終わるのに、`BudgetExhausted` で終わっていました。今は予算を超えて終わるすべての場合に failure の記録があり、witness には、モデルトークンの記録（#420）と同じように、予算のキーの下に `granted`、`used`、`over_by` が入ります。金額は `P_MONEY_BUDGET` と `failure.witness["monetary"]`、Coreのステップは `P_CORE_STEP_BUDGET` と `failure.witness["core_steps"]`、wall time は `P_RUNTIME_TIMEOUT` と `failure.witness["wall_time_ms"]` です。wall time はどちらの経路でも `TimedOut` で終わります。複数の予算を超えた実行は witness にそれぞれを並べ、ステータスとコードは wall time、モデルトークン、金額、Coreのステップのうち最初のものに従います。Core エンジンが Core ステップの許可量で止めた実行の記録と、期限が止めた実行の記録にも witness が付きます（許可量で止まった場合 `over_by` は 0）。**動作の変更:** 金額または Core ステップの許可量を超えたまま終わった実行には `failure` が付きます。wall time の許可量を超えたまま終わった実行は `BudgetExhausted` ではなく `TimedOut` で終わり、タイムアウトの failure には `witness["wall_time_ms"]` が付きます。
- **素の `GraphEngine` に接続したStrictプロファイルは、ツールループの2回目のLLM呼び出しで最初のターンのコンテキストを送り直す代わりに、その呼び出しを拒否します（#444）。** `StrictRuntimeProfile` を接続すると、`engine.run()` 1回の中のツールループ（LLM呼び出し、ツールのディスパッチ、LLM呼び出し。`create_react_graph` が作る構成）は、プロンプトを毎回エポックに固定されたRAWの範囲から作っていました。実行の中でアシスタントとツールのレコードをコンテキストフィードに追加する処理も、新しいエポックをアクティブにする処理もないためです。ツールの結果はグラフの状態に残るだけでプロバイダーには届かず、モデルは、ステップ数の上限か予算で実行が止まるまで、同じツールを要求し続けることができました。コントローラーを設定した `llm::Agent` にも、同じ欠陥がありました。strictの経路は、ホストがステップを進めます。ホストは、プロバイダーを呼び出すたびに、RAWレコードを追加し、新しいエポックをアクティブにします。グラフがすでに進めたターンのエポックにプロバイダー呼び出しが届くと、つまり、提示されたメッセージに、エポックの最初のディスパッチ以降に追加されたアシスタントまたはツールのメッセージがあり、そのメッセージをホスト指示や信頼された補足入力としても運んでいない場合は、この契約を明示するメッセージとともに `std::logic_error` を投げるようになりました（ノードは再試行されず、Pythonでは `RuntimeError` になります）。**動作の変更:** strictプロファイルのもとで1回の実行の中でループし、古いコンテキストを送っていたグラフやエージェントは、2回目のプロバイダー呼び出しで失敗するようになります。同じターンの再試行、エポックの最初のディスパッチの時点ですでにそれらのメッセージを持っていた会話、会話を信頼された補足入力として運ぶ組み込みノード（plan executorなど）は、変わりません。エポックをアクティブにすると、次のターンが始まります。同じ実行の後のエポックに進むときに、レシートの競合で失敗していた問題も直しました。コントローラーはアクティブにするたびにディスパッチのカウンターを0に戻していたため、シーケンスが1より大きいエポックの最初のディスパッチが、実行の最初のディスパッチのIDを再利用していました。そのようなディスパッチのIDは `<run id>-epoch-<シーケンス>-dispatch-<n>` になり、実行の最初のエポックのもとのIDは変わりません。strictガイドにこの契約を記載しました。実行の中でエポックを進めるヘルパーは、NeoGraphにはまだありません。
- **ドキュメント: コンテキストの管理、予算、配置、並行性（#328、#326、#329、#330）。** 変更はドキュメントとコメントだけで、動作は変わりません。strictガイドは、`messages` チャネル、`history::compact_history`、Recordedプロファイル、Strictプロファイルを比較しています（#328）。コンテキストの見積もり（エンコードしたリクエストのバイト数を3で割った値）は、エンコードしたリクエスト全体に対するヒューリスティックであり、証明された上限ではないことも明記しました（#326）。配置のガイダンスも加えました。どの `ContextPlacement` の値がターンの間で動くか、安定したアーティファクトは `BeforeHistory` に置くこと、`anthropic.messages`、`google.generate`、`google.interactions` ではSystem役割のアーティファクト（`RequiredSkill`、`HardConstraint`）が配置にかかわらず最上位のsystemフィールドに入ることです（#329）。並行性のガイドには、コンテキストフィードは追記の競合でフェイルクローズすること、チェックポイントストアは `thread_id` ごとに最後に書いたものが勝つこと、1つの `thread_id` の実行を直列化するのはホストの役目であること、プロセス内のガードを計画していることを記載しました（#330）。
- **Send による実行が Send を返すと、それを失う代わりに実行が失敗します（#432）。** Send で始まったノードが返した Send は捨てられ、その送り先は実行されず、何も知らされませんでした。今後は、スーパーステップの結果を一つも反映する前に、ノードと送り先を示すエラーで実行が失敗します。**動作の変更:** Send の送り先が Send を返すグラフは、これまで送り先を実行せずに終わっていましたが、今後は失敗します。項目をチャネルに書き、通常のノードから Send を出してください。
- **失敗した Program の試行が、Send による実行をエンジンのトレースと同じ名前で記録します（#433）。** 失敗した Core はエンジンのトレースなしで例外を投げ直すため、失敗した試行は `NODE_END` イベントからトレースを作り、Send による実行をノード名だけで記録していました。今後は Send による実行の `NODE_END` イベントに `"send": true` が入り、試行のトレースはそれを完了した順に `<対象>[send]` と記録します。**動作の変更:** 失敗した Program の試行のトレースは Send による実行を `<対象>[send]` と記録し、Send による実行の `NODE_END` イベントには `send` フィールドが入ります。
- **Python でもビルド時の lint を止められます (#415)。** `GraphEngine.compile` にキーワード `stderr_notices` を追加しました。デフォルトは `True` で、C++ の `EngineConfig::stderr_notices` と同じです。`stderr_notices=False` にすると、strict なグラフの lint（`Send` や `Command.goto` だけが開始するノードに対する E7 など）と、fan-out が直列で実行されたことを知らせる 1 回限りの通知を出力しません。検証エラーは従来どおり例外になります。E7 自体は変えていません。グラフ定義には動的な対象を宣言する場所がないため、検証器は Send が開始するノードと、どこからも到達されないノードを区別できません。ドキュメントにこのことを明記しました。ドキュメントは、Send の対象への静的エッジが lint を黙らせる手段にならないことも説明しています。対象は、Send がすべて終わったあとに共有状態でもう一度実行されます。キーワードを渡さないコードの動作は従来どおりで、グラフの実行方法も変わりません。
- **`RunResult::execution_trace` の契約を明記しました (#424)。** 変更はドキュメントとテストだけで、動作は変わりません。`Send` が開始したノードがトレースから抜けるという報告は、再現できませんでした。`run`、`run_stream`、`run_async`、`run_stream_async`、`resume` は、ワーカープールの有無にかかわらず、Send の実行をすべて `<対象>[send]` として記録します。`types.h`、`engine.h`、ドキュメントに並び順を明記しました。スーパーステップは順番どおり、1 つのスーパーステップの中では実行可能なノードが先、そのあとに Send が送出された順で並び、実行が終わった順は関係ありません。一時停止したスーパーステップは何も記録せず、再開した実行は入り直したスーパーステップ全体を記録し、サブグラフは 1 件です。
- **ドキュメント：チャネルの `version` は、すべてのチャネルが共有する書き込みカウンター1つの値です（#416）。** `RunResult` のドキュメント、ヘッダー、ガイドは、`output["channels"][name]["version"]` をそのチャネルに書き込んだ回数、`output["global_version"]` をスーパーステップのカウンターと説明しており、Pythonの `global_version()` の docstring はチャネルのバージョンの合計と説明していました。実際には、どのチャネルへの書き込みでも（実行入力の書き込みを含めて）1ずつ進むカウンター1つの値です。チャネルのバージョンは、そのチャネルへの最後の書き込み時点のカウンター、`global_version` は実行の最後の書き込み後のカウンターです。バージョンは書き込みのたびに大きくなり、2つのチャネルが同じバージョンになることはありませんが、どちらも書き込みの回数やスーパーステップの番号ではありません。一度も書き込まれていないチャネルのバージョンは、`initial` の値があっても0です。チェックポイントは両方の値を保存し（`Checkpoint::channel_versions` はエンジンが設定しません）、再開した実行やフォークした実行は、その値から続きを数えます。数値は変わっておらず、テストが固定しています（`ChannelVersions.*`、`test_channel_versions.py`）。

## [0.14.0] - 2026-10-13

### 変更

- **C++ の利用側を再ビルドしてください。task graph の型に member が追加されました（#412）。** `TaskGraphProposalLimits` に `min_schema_version`、`max_attempts`、`max_literal_bytes`、`max_total_literal_bytes` が追加されたため、これを含む `TaskGraphProposalOptions`、`TaskGraphFragmentCompileOptions`、`TaskGraphExpansionPolicy` の layout が変わりました。`TaskGraphTask` に `literals` と `max_attempts` が、`CompiledTaskGraphTask` に `input_literals` と `max_attempts` が追加され、`TaskGraphInputLiteral` が新設されました。`TaskGraphProposal::SCHEMA_VERSION_2` と `CompiledTaskGraphFragment::SCHEMA_VERSION_2` が新設され、`TaskGraphProposal::schema_version()` は文書のバージョンを返します。NeoGraph、C++ の利用側、Python extension をまとめて再ビルドしてください([`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照)。
- **C++ の利用側を再ビルドしてください。ツール、Hook、MCP、gRPC、グラフの型に member が追加されました。** `RunResources` に `hook_runtime`、`InvocationResult` に `failure`、`MCPTool` に base `ContextualAsyncTool` と `execute_result_async(arguments, context)`、`MCPClient` に制限付きの `call_tool_result_async`、`MandatoryHookRunner` に `blocks` と private member function、`GrpcRemoteTool` に base `ContextualAsyncTool` と `set_call_timeout`、`GrpcCheckpointStore` に `set_rpc_timeout` が追加され、`graph::FailureVerdict`、`graph::classify_failure`、`ToolExecutionError`、`grpc::make_tool_service`、`grpc::make_checkpoint_service` が新設されました。`proto/neograph.proto` には新しい field 番号で `ToolCallResponse.error_code` が追加され、wire の互換性は保たれます。NeoGraph、C++ の利用側、Python extension をまとめて再ビルドしてください([`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照)。
- **ツールの Hook がすべてのツール経路で実行されます。** エンジンの `HookRuntime`(`EngineConfig::hook_runtime`、`GraphEngine::set_hook_runtime`)は以前はチェックポイントの Hook だけを発行していましたが、今は `llm::Agent::set_hook_runtime` が Agent に対して行うのと同じく、ノードが行うすべてのツール呼び出し(`tool_dispatch`、plan-and-execute の executor、deep-research の researcher、`make_tool_execution_context` を使うホストのノード)の `BeforeToolExecution` と `AfterToolExecution` の Hook も実行します。`RunResources::hook_runtime` は 1 回の実行でそれを置き換え、サブグラフのツール呼び出しは親の実行のものを、Program Core のツール呼び出しは Program ホストの `RuntimeConfig::hook_runtime` を使います。**動作の変更:** Hook registry がツール段階の Hook を受け入れているホストには、グラフと Program のツール呼び出しも見えるようになり、必須 Hook はそれらをブロックできます。実行できない Hook 境界(owner scope のない実行)は、チェックポイント、プロバイダー、Program のイベントでも、`std::invalid_argument` ではなく state `failed` の `HookBoundaryBlocked` で閉じた側に失敗します。
- **Hook のブロックはグラフ実行を `HookBoundaryBlocked` として抜けます。** グラフ実行は以前は原因が `HookBoundaryBlocked` の `NodeExecutionError` を送出し、`llm::Agent` はそれを直接送出していました。今はどちらも、原因の連鎖のどこにあったかに関係なく `HookBoundaryBlocked` そのものを invocation id と state とともに送出し、ノードはそれについての `ERROR` イベントを発行しません。失敗の原因の連鎖の中で見つかったキャンセルも同じく `CancelledException` として抜けます。Python はすでに `ng.HookBoundaryBlocked` を受け取っていましたが、その例外には `node_name` と `attempts` 属性がなくなりました。
- **Hook はタイムアウトしたときだけ `timed_out` を報告します。** 最後の試行が失敗した必須 Hook は以前は `timed_out` として記録・報告されていましたが、今は `InMemoryHookJournal`、`SQLiteHookJournal`、`HookBoundaryBlocked::state()` のいずれでも `failed` であり、使い切った失敗は `pending()` に列挙されなくなりました。期限が過ぎた Hook、またはネイティブのターゲットがタイムアウトした Hook(RPC の Hook はすでにそうでした)は、リースが期限を越えた場合も含めて `timed_out` を報告します。
- **実行の期限の後にツール呼び出しは開始しません。** すべてのディスパッチ経路で、`RunContext::deadline`(`ToolExecutionContext::deadline`)以降に開始されることになる呼び出しは、Hook なしで、ステータス `expired`、エラー `run deadline elapsed before the tool started` のツール結果になります。以前は Hook が構成されていなければそのまま実行され、構成されていれば `timed_out` の `HookBoundaryBlocked` で実行が止まっていました。期限は受け入れキューでの待機も制限します。
- **MCP と gRPC のサーバーが失敗の分類を区別して保ち、例外テキストを送りません。** `MCPServer` は Hook のブロックに JSON-RPC エラー `-32010`、割り込みに `-32011` で応答し、その他のすべての失敗の安定したコードを `isError` の結果の `_meta["io.neograph/failure"]` に入れます。クライアントがキャンセルした呼び出しには、MCP の求めるとおりどのトランスポートでも応答せず(応答 sink の `NoResponseSink` がその id を受け取ります)、`stop()` がキャンセルした呼び出しには `-32000` を送ります。gRPC は標準のステータスコードを使います。失敗した `RunGraph` や `RunGraphStream` は、分類のステータス(`UNKNOWN`、`DEADLINE_EXCEEDED`、`UNAVAILABLE`、`RESOURCE_EXHAUSTED`、`FAILED_PRECONDITION`、`INTERNAL`、`ABORTED`、`CANCELLED`)と、`ErrorInfo.reason` が安定したコードである `google.rpc.Status` で終わります。不正なリクエストは何かが実行される前に `INVALID_ARGUMENT`(グラフがなければ `FAILED_PRECONDITION`)です。`ToolService` はツール自身の失敗を `error_code` とともにペイロードに入れ、実行制御シグナルにはステータスで、未知のツールには `NOT_FOUND` で、不正な引数には `INVALID_ARGUMENT` で応答します。すべてのメッセージは `what()` ではなく公開メッセージ(`NodeFailureError::public_message` または分類の説明)です。**動作の変更:** gRPC で失敗したグラフ実行は以前は `error` が設定されたステータス `OK` で、失敗したストリームは `OK` で終わっていました。`RunGraphResponse.error` は非推奨で 1 リリースの間残ります。`MCPServer` のツールエラーは以前は例外テキストを含み、キャンセルまたは停止された呼び出しは `isError` の結果でした。[プロトコル失敗のマッピング](docs/reference-en.md#protocol-failure-mapping)を参照してください。
- **`ToolExecutionController::execute_async` が型付きの結果を送出します。** 例外を送出するこのファサードは `timed_out`、`expired`、`rejected` に `asio::system_error` を、その他すべての失敗に `std::runtime_error` を送出していましたが、今は `ToolExecutionResult` 全体を持つ `ToolExecutionError` を送出し、キャンセルのステータスには引き続き `CancelledException` を送出します。ノード失敗の分類器はその中の不確定な効果を `NG.EffectUncertain` として扱うので、ノードのリトライポリシーはそのような呼び出しを再実行しなくなり、タイムアウトまたは期限切れの呼び出しは `NG.Timeout` として扱います。`timed_out` の asio または `std::system_error` も `NG.TaskFailed` ではなく `NG.Timeout` になりました。どちらも引き続きリトライされ、ルーティングできます。
- **C++ の利用コードを再ビルドしてください。A2A、ACP の layout に member が追加されました。** `a2a::AgentCard` に `skills`、`security_schemes`、`security_requirements`、`extensions` が、`a2a::GraphAgentAdapter` に virtual の `build_resume_value` が、`a2a::A2AClient` に endpoint ポリシーの状態が追加されました。`acp::ClientCapabilities` に `elicitation` が、`acp::ACPClient` に member が、`acp::ACPGraphAdapter` に virtual 関数が追加されました。NeoGraph、C++ の利用コード、Python 拡張を一緒に再ビルドしてください([`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照)。
- **A2A: 1.0(v1.0.1)が主要バージョンになり、native server がその task 規則に従います。** 0.3 は互換 dialect として残ります。動作の変更: task id は server が生成するため、未知の `taskId` を指定した message はその task を作らずに TaskNotFoundError(`-32001`)で失敗します(NeoGraph の collaboration envelope は送信者が決めた id を保ちます)。terminal な task への message は `-32004`、実行中の task への message は `-32603`(ErrorInfo `TASK_BUSY`)、input を待つ Program task への message は `-32004` で失敗します。Program の input 要求に A2A で答える機能はまだありません。terminal な task の `CancelTask` は TaskNotCancelableError(`-32002`)で失敗し、task は変わりません。in-flight の上限は `REJECTED` の task ではなく `-32603`(ErrorInfo `RESOURCE_EXHAUSTED`)で応えます。`A2A-Version` は 1.0 と 0.3 だけを受け付けます。`AgentCard::streaming` の既定値が `true` になり、`streaming = false` の card は streaming method を拒否します。card はバージョンごとに提供されます。`A2A-Version: 1.0` には純粋な 1.0 card、バージョンのないリクエストには 0.3 card(引き続き 1.0 interface を先頭に載せる)を返します。task の history はユーザーの message から始まります。Program 経路では、credential がなければ `AUTH_REQUIRED` の task ではなく `WWW-Authenticate` 付きの HTTP 401、許可されていない collaboration peer は HTTP 403 になり、card は Bearer security scheme を宣言します。
- **A2A client: 最初の呼び出しで AgentCard を取得し、選んだ interface に従います。** 動作の変更: card を取得していない client は 0.3 から probe していました。現在は最初の呼び出しの前に card を取得し、取得できない場合だけ 1.0 から probe して、`-32601`、`-32009`、HTTP 400 なら 0.3 に切り替えます。選んだ interface URL が `base_url` と同じ origin なら RPC はその URL に送られます。`set_allow_cross_origin_interfaces(true)` は card が別 origin を指定することを許可し(許可しなければ client は `base_url` を使い続け、stderr に警告を書きます)、`endpoint_url()` は使用中の endpoint を返します。message id はランダムな UUID です。`A2ACallerNode` は task が input を待つ間だけ `<output_key>_task_id` を保ち、context id は常に送るため、terminal で終わった turn の後は同じ会話で新しい task が始まります。
- **ACP: 失敗した prompt turn は JSON-RPC error になり、例外テキストは client に届きません。** 失敗した turn は public message と `data: {code, retryable}` を持つ `-32603` です(以前は例外テキストを含む chunk の後に `stopReason: end_turn` が続いていました)。provider の拒否は `refusal`、model token 予算の超過は `max_tokens`、step 上限は `max_turn_requests`(空の chunk なし)で終わり、client が求めていない取り消しは `-32800` です。
- **ACP: NodeInterrupt は同じ turn で `session/request_permission` を尋ね**、選ばれた option で checkpoint から再開します(以前は `_meta` 付きの chunk と `end_turn` で、次の prompt が回答でした)。`ACPGraphAdapter::interrupt_permission` と `interrupt_resume_value` でカスタマイズできます。
- **ACP の error code が v1 schema に従います。** busy・overload の turn は `data.reason` を持つ `-32603` です(ACP が "authentication required" に予約する `-32000` はもう使いません)。`initialize` 前の呼び出しは `-32600`、未知の `session/resume` は `-32002` です。
- **ACP の取り消し:** 進行中の turn がないときの cancel は無視します(`set_pre_armed_cancel` で以前の事前取り消しを戻せます)。待機中の agent→client リクエストは `$/cancel_request` とともに破棄します。permission リクエストには固定の timeout がありません(`set_permission_timeout`)。stdin の EOF は進行中の turn を取り消します(`set_eof_grace`)。
- **ACP の prompt content と session context:** `resource_link` と広告した embedded resource が graph に届きます。不正な、または広告していない content block は `-32602` です。`_acp_cwd` と `_acp_additional_directories` は `build_session_state` から来ます。`terminal/*` と `elicitation/*` には client capability が必要です。`agentInfo` は常に有効な Implementation です。
- **C++ の利用側を再ビルドしてください。`HookRuntime` に member が追加されました。** `HookRuntime` に `compose`、`emit_once_async` と private member が 2 つ、`MandatoryHookRunner` に private member 関数が追加され、`graph::RemoteControlFailure` と `tool_call_not_started` が新しく加わりました。NeoGraph、C++ の利用側、Python 拡張を一緒に再ビルドしてください([`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照)。
- **サブグラフのツール呼び出しが両方の Hook ランタイムを通ります。** サブグラフのエンジンと親の実行の両方に `HookRuntime` がある場合、そのツール呼び出しは親の Hook を先に、次に子の Hook を通り(`HookRuntime::compose`)、どちらかがブロックすると呼び出しはブロックされます。**動作の変更:** 以前は親の実行のランタイムがサブグラフのエンジンのランタイムを置き換え、そのツール Hook は実行されませんでした。
- **ツールの Hook イベントに永続的な識別ができ、一度だけ配信されます。** グラフ実行と、呼び出しにタスクスロット(`ToolExecutionContext::effect_task_id`)があるすべての場所で、`BeforeToolExecution` と `AfterToolExecution` のイベントは呼び出しの効果の識別(実行、スレッド、ノードの試行を含むタスク、呼び出しの序数)である `task_id` と `call_ordinal` を持ち、`request_id` はプロセスごとのカウンターではなくこの識別から導かれます。batch を再び実行する再開や再生は、その識別ですでに配信されたイベントを再び配信しません。Hook がブロックした発行は配信ではなかったので、再び実行されます。`llm::Agent` はターンごとの batch に固有のタスクスロットを与えます。**動作の変更:** 以前は再開された batch が呼び出しのツール Hook をすべて再び実行しました。
- **gRPC の `ErrorInfo.reason` が AIP-193 に従います。** `reason` は安定したコードから導いた UPPER_SNAKE 定数になり(`HOOK_BLOCKED`、`TIMEOUT`、`TOOL_NOT_FOUND`、`INVALID_REQUEST`、`NO_GRAPH`、`CAPACITY`、`BACKEND_UNAVAILABLE`、`BARRIER_STALLED` など。作成者コードはそのクラスの reason になります)、`NG.X` のコードそのものは `ErrorInfo.metadata["code"]` に移りました。domain は `neograph` のままです。**動作の変更:** 以前は `reason` が `NG.X` のコードでした。それを読んでいたクライアントは `metadata["code"]` を読んでください。NeoGraph 自身のクライアントは両方を受け付けます。
- **リモートの Hook ブロックや割り込みは型付きのツール失敗です。** `MCPTool` が JSON-RPC エラー `-32010`(サーバーの Hook が呼び出しをブロックした)や `-32011`(サーバーの実行に入力が必要)を受け取るか、`GrpcRemoteTool` が `ErrorInfo` のコードが `NG.HookBlocked` または `NG.Interrupted` であるステータスを受け取ると、`graph::RemoteControlFailure` を送出します。これはコード `NG.HookBlocked` または `NG.Interrupted` とリモートの公開メッセージを持ち、リトライされない通常の失敗です。ローカルの実行を停止も一時停止もさせず、ローカルの実行は失敗したツール結果とともに続きます。リモートの割り込みのネイティブな伝達は後の段階です。**動作の変更:** 以前はこれが型のないツール失敗(`NG.TaskFailed`)で、タイムアウトしたリモートの Hook ブロック(`DEADLINE_EXCEEDED`)は呼び出し自身の期限のように見えました。
- **C++ の利用側を再ビルドしてください。MCP と async の型に member が追加されました。** `MCPError` に `http_status()`、`StdioClientConfig` に `cancel_grace`、`MCPHttpServerConfig` に `max_http_threads`、`session_idle_timeout`、`protected_resource_metadata`、`MCPServer` に `protocol_version()`、`NoResponseSink` 型とそれを受け取る `set_response_sink` の overload、`MCPClient` に期限とキャンセルトークンを受け取る `initialize` と `initialize_async` の overload および `set_tool_list_changed_handler`、`MCPTool` に private member 関数、`SseEventParser` に `last_event_id()`、`retry_ms()` と private member が追加され、`async::async_request_stream`、`async::HttpResponseHead`、`mcp::MCP_SUPPORTED_PROTOCOL_VERSIONS`、`mcp::is_supported_protocol_version`、`MCPServer::Reply` とそれを受け取る `MCPServer::handle_message` の overload が新しく加わりました。`ToolExecutionController` には private member 関数が追加されました。NeoGraph、C++ の利用側、Python 拡張を一緒に再ビルドしてください([`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照)。
- **MCP クライアントは URL を与えられたとおりに使います。** `MCPClient(url)`、`MCPTool(url, ...)`、Python の `mcp.MCPClient(url)` は、MCP の Streamable HTTP トランスポートが求めるとおり、すべてのリクエストをパスとクエリを含む設定済みの URL に送ります。**動作の変更:** 以前はパスがすでに `/mcp` で終わっていなければ `/mcp` を付け加え(`http://host:8000` は `http://host:8000/mcp` に、`/v1/rpc` は `/v1/rpc/mcp` になる)、クエリ文字列を拒否していました。`http://host:8000/mcp` のように、サーバーの完全な MCP エンドポイント URL を渡してください。サンプルの既定値もこの形になりました。
- **MCP サーバーは 2025-06-18、2025-03-26、2024-11-05 にも応答し、クライアントは既知の版だけを受け入れます。** `MCPServer` は公式 SDK サーバーと同様に、クライアントが求める版が `2025-11-25`、`2025-06-18`、`2025-03-26`、`2024-11-05` のいずれかならそれで、そうでなければ `2025-11-25` でネゴシエートし、古い版のクライアントにはその版が定義するフィールドだけを送ります(`2025-06-18` には `icons` と `execution` がなく、`2025-03-26` には `title`、`outputSchema`、`_meta`、`structuredContent`、`resource_link` のコンテンツもなく、構造化データだけの結果はそれをテキストで運びます。`2024-11-05` には `annotations` と音声のコンテンツもありません。その版にないコンテンツブロックは、その JSON を持つテキストブロックで送ります)。HTTP トランスポートは `MCP-Protocol-Version` をセッションの版と照合し、ヘッダーがなければその版を使います。NeoGraph の MCP クライアントは、同じ 4 つの応答を stdio と Streamable HTTP で受け入れます (`MCP_SUPPORTED_PROTOCOL_VERSIONS`)。以前の HTTP+SSE トランスポートには対応しません。**動作の変更:** サーバーはすべてのクライアントに `2025-11-25` で応答し、セッションでヘッダーがなければ 400 でした。クライアントは `1999-01-01` のようなどんな `protocolVersion` も受け入れていましたが、今は一覧にないバージョンならプロトコルエラーで `initialize` が失敗し、切断します。MCP `2026-07-28` はまだ提供も使用もせず、第二のモードとして予定しています。
- **MCP ツールの登録は、公式クライアントが拒否するものを拒否します。** `MCPServer::register_tool` と `register_raw_tool` は、`"type": "object"` と述べ、この検証器が対応する JSON Schema 2020-12 である `inputSchema`(あれば `outputSchema` も)と、`A-Za-z0-9_.-` からなる 1〜128 文字の名前(SEP-986)を求めます。`ToolDefinition::input_schema` の既定値は `{"type": "object"}` になり、空のオブジェクトはその既定値として読みます。**動作の変更:** 以前はこのような登録が成功し、スキーマなしで登録されたツール 1 つのために公式クライアントが `tools/list` 全体を拒否していました。
- **未知の MCP ツールはプロトコルエラーです。** サーバーにないツールへの `tools/call` は、MCP のツールの「Error Handling」が求めるとおり、`data.code` が `NG.ToolNotFound` の JSON-RPC エラー `-32602` `Unknown tool: <name>` です。**動作の変更:** 以前は `isError` の結果でした。
- **MCP の HTTP セッションは期限切れになって空きを作り、キャンセルされた呼び出しには応答しません。** `MCPHttpServer` は `session_idle_timeout`(30 分)リクエストのないセッションを閉じ、新しいクライアントが `max_sessions` で initialize すると、処理中のリクエストのない最も長く使われていないセッションを閉じます。閉じたセッションの id には 404 が返り、クライアントに再度 initialize するよう伝えます。すべてのセッションが使用中のときだけ、initialize は 503 と `Retry-After` で拒否されます。**動作の変更:** セッションは `DELETE` まで生き続けたため、`close()` で `DELETE` を送らない公式 TypeScript クライアントが 64 個つながった後は、すべての新しいクライアントが恒久的に 503 を受けていました。クライアントがキャンセルした呼び出しは、JSON-RPC エラー `-32800` の代わりに空の `text/event-stream` で POST を終えるようになりました。すべての公式 TypeScript クライアントが `-32800` を未知のリクエストへの応答として報告していたためです。`MCPServer` の response sink も `-32800` を受け取らなくなり、`NoResponseSink` とともに設定した sink は代わりにその呼び出しの id を受け取ります。
- **強化された MCP stdio セッションは stderr への出力でも動き続け、順序立てて止まり、終了の前にサーバーへ知らせます。** 強化されたセッション(`StdioClientConfig`)はサーバーの stderr を読み続けて最後の `max_stderr_bytes` を保持します。キャンセルまたはタイムアウトしたリクエストは `notifications/cancelled` を送り、`cancel_grace`(500 ms)待ってからサーバーを終了します。すべての stdio サーバーは停止時に stdin を閉じ、SIGTERM の前に最大 1 秒、SIGKILL の前にさらに最大 1 秒待ちます。**動作の変更:** 強化されたセッションは stderr が 64 KiB 書かれると終わり、キャンセルやタイムアウトではサーバーに知らせずに即座に kill していました。すべての stdio の停止は SIGTERM と SIGKILL を 20 ms 間隔で送っていました。
- **MCP クライアントはどのリクエストよりも先にハンドシェイクし、HTTP セッションのメッセージを待ち受けます。** まだ初期化していないクライアントの `MCPClient::rpc_call_async` は先にハンドシェイクを行うため、MCP 上の hook RPC トランスポートが `initialize` の前にリクエストを送ることはなくなりました。`initialize` の後、HTTP クライアントは公式クライアントと同様にセッションのオプションの `GET` イベントストリームを開き、どの呼び出しにも属さないサーバーのリクエストと通知を受け取ります(FastMCP v1 は ping をそこへ送ります)。**動作の変更:** HTTP セッションごとに接続を 1 つ多く開いたままにします。そのストリームを持たないサーバーは 405 を返し、クライアントはそれ以上要求しません。
- **C++ の利用コードは再ビルドが必要です。Program の provider 呼び出し構造体に member が加わり、価格の付く呼び出しには `monetary_microunits` が適用されます (#372)。** `ProgramResultData` に `monetary_records`、`ProgramCoreProviderCallContext` に `monetary_account`、`SQLiteProgramProviderCallJournal::Record` に `monetary_record` が加わり、`Provider` には静的関数 `invocation_upper_bound` ができました。SchemaProvider `f7ed816` の採用で `sp::OpenRouterRouting` に `max_price` が加わり、interface revision 6 のまま `ProviderControls`、`SchemaProvider::Defaults`、SDK の Chat・Messages・Responses リクエストも大きくなりました。NeoGraph、C++ の利用コード、Python 拡張を対応する SDK で一緒に再ビルドしてください（[`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照）。provider journal は既存のデータベースに `monetary_record` 列を追加します。保存された `ProgramResult` は価格の付いた呼び出しがあるときだけ `monetary_records` を持つため、ほかの結果のバイトと id は変わりません。**動作の変更:** すべての `RunBudget` の量と同じく `monetary_microunits = 0` は何も与えないため、金額予算がゼロの実行は、価格付け broker が価格を付けるすべての呼び出しを `P_MONEY_BUDGET` で拒否します。以前は何も金額を請求しませんでした。
- **C++ 利用側の再ビルドが必要です: 割り込みの識別でメンバーが増えました (#388)。** `RunResult` に `interrupt_id` と `interrupts`、`RunMetadata` に `resume_interrupt_id`、`ProgramPendingInputData` と `ProgramPendingEffectData` に `core_interrupt_id` が追加され、`graph/types.h` に `PendingInterrupt` が新設されました。NeoGraph、C++ 利用側、Python 拡張を一緒に再ビルドしてください（[`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照）。
- **Core: 単一の `Send` も、複数のときと同じく payload を自身だけに留めます (#389)。** 動作の変更: ステップの唯一の `Send` の payload は、共有 run 状態、次のステップのノード、run の最終状態に届かなくなり、Send が複数あるステップの各 payload がすでにそうだったように、その Send 自身の実行にだけ見えます。Send ステップの後で単一 Send の payload を読んでいたグラフは、その値を write として返す必要があります。このリポジトリの例やテストで以前の動作に依存していたものはありません。
- **Core: 再開値が後続のステップに届かなくなりました (#393)。** 動作の変更: 再開した run の後続のスーパーステップで `ctx.resume_value` (Python の `input.ctx.resume_value`、tool gate の `ToolGateContext.resume_value`) を読んでいたノードには値が見えなくなり、`interrupt_after` の一時停止後の再開、ステップのチェックポイントや safe point からの再開のすべてのノードも同様です。値は再開が答える呼び出し、つまり再開が答える `NodeInterrupt` を送出したノードや `Send`、または `interrupt_before` のゲートが止めていたノードにだけ届きます。`messages` チャネルを持つグラフは、引き続きその値をユーザーメッセージとして受け取ります。このリポジトリの例とテストで以前の動作に依存していたものはありませんでした。
- **Core: 尋ねる兄弟のインデックスにかかわらず失敗がステップを終わらせます (#392)。** 動作の変更: 一つの兄弟が `NodeInterrupt` を送出し、別の兄弟が失敗した fan-out ステップや Send のステップは、順序にかかわらず run を失敗させるようになりました。以前は、インデックスの小さい割り込みが run を一時停止させ、失敗を捨てていました。キャンセルは順序にかかわらずそのようなステップを止めるようになりました。照合 (reconciliation) の割り込みは失敗の横でも引き続き一時停止し、そのときチェックポイントはそれが唯一でも `_neograph.interrupts` に列挙します。その他のチェックポイントはバイトを保ちます。
- **Core: ステップの各 `interrupt_before` ゲートを個別に尋ねます (#396)。** 動作の変更: 一つのスーパーステップで複数のゲート付きノードが ready になると、再開は run が止まったゲートに答え、すべてのゲートが答えられるまで run はそのステップの他の各ゲート付きノードの前でもう一度止まります。以前は一度の再開がすべてを実行していました。ステップの後続のゲートの Before チェックポイントは、すでに答えられたゲートを `_neograph.answered_gates` に列挙し、ゲートが一つのステップはチェックポイントのバイトを保ちます。
- **Program: join は失敗したメンバーの横で照合のために一時停止します (#397)。** 動作の変更: JavaScript の `ng.all` (fail-fast)、`ng.race`、`ng.quorum` とプランレベルの `parallel`、`quorum` で、不確かな効果 (Core の `tool_effect_reconciliation` の割り込み) で一時停止したメンバーは、メンバーの失敗が join を終わらせる場面で join を一時停止させるようになり、再開されるとその失敗が join を終わらせます。以前は失敗がすぐに join を終わらせていました。成功が結果を決めた join は変わりません。
- **Program: JavaScript の `main()` のフォークは一時停止したコマンドを続けます (#395)。** 動作の変更: フォークは元の実行の入力で `main()` を実行し、実行記録はフォークの入力ではなくその入力を保持します。元の実行が `callCore` で一時停止していないフォークは、`main()` を最初からやり直す代わりに `P_FORK_STATE` で拒否されます。
- **Program: フォークやグラフ移行は持ち込むプロバイダーの証拠を結び直します (#398)。** 動作の変更: フォークやグラフ移行が新しい実行に持ち込む、プロバイダーの証拠を持つ記録された失敗は、元の実行のカストディで復元され、新しい実行のカストディで保存し直されます。元の実行のカストディに一致しない記録は、何かが公開される前に `P_FORK_CUSTODY` でフォークを拒否します。
- **Program: プランの quorum は Core ノードが失敗した分岐を数えます (#399)。** 動作の変更: そうした分岐は quorum の失敗した分岐であり、quorum は続行して自身の規則 (達成、`P_QUORUM_UNREACHABLE`、または後続の分岐による一時停止) で終わります。以前はノードの失敗がすぐに実行を失敗させていました。
- **Program: 再開された join は応答を受けるメンバーを待ちます (#400)。** 動作の変更: 再開が応答するメンバーが実行されている間、再生されたメンバーの完了が下す決定 (fail-fast の失敗、race の最初の完了者、達成された、または到達不能になった quorum) は保留され、他のメンバーは開始されません。決定は応答を受けたメンバーが終わったときに下されます。
- **Program: 一時停止する join は照合を先に尋ねます (#401)。** 動作の変更: 失敗なしに一時停止する JavaScript の join やプランレベルの `parallel` は、宣言順にかかわらず、通常の質問で一時停止したメンバーより先に、不確かな効果で一時停止した最初のメンバーで一時停止するようになりました。収集型の `ng.all` は失敗の横でも一時停止したメンバーで一時停止し続け、これは #392/#397 の優先順位の意図的な例外として文書化されました。
- **C++ の利用コードは再ビルドが必要です。グラフと Program の構造体に member が加わりました (#357, #358, #361, #365, #367)。** `EngineConfig` に `validation` と `stderr_notices`、`RunMetadata` と `RunContext` に `shared_scheduler`、`ProgramCompilerConfig` に `require_acyclic`、`ProgramCompilerConfig` と `ValidationOptions` に `soundness_check` と `soundness_state_cap` が加わり、`GraphEngine` には private member、`RunStatus` には `BarrierStalled` が加わりました。#364 で `EngineConfig` に `failure_text_redactor`、`RunMetadata` に `node_retry_ceiling`、`RunBudget` に `max_node_retries`、`ProgramUsage` に `node_retries`、`CatalogConfig` に `failure_text_redactor` が加わり、`Scheduler`、`NodeExecutor`、`GraphEngine` にメンバーが、`RunStatus` に `FailureExit` が加わり、`ProgramDiagnosticError`、`ProgramCompileError`、`ProgramAdmissionError` は基底クラス `graph::ConfigurationFailure` を得ました。これらは layout を変えるため、NeoGraph、C++ の利用コード、Python 拡張を一緒に再ビルドしてください（[`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照）。すべての値を列挙する `RunStatus` の `switch` には `BarrierStalled` と `FailureExit` の case が必要です。
- **C++ の利用コードは再ビルドが必要です。試行タイムアウトでグラフのレイアウトが変わり、ノード本体は試行のトークンを受け取ります (#364, #367, #368)。** `NodeRetryPolicySpec` に `attempt_timeout_ms` が、`NodeExecutor` にメンバーが、`GraphEngine` にメンバーと宣言されたデストラクターが加わりました。NeoGraph、C++ の利用コード、Python 拡張を一緒に再ビルドしてください（[`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照）。**動作の変更:** Program の実行では、またタイムアウトのあるすべての試行では、ノード本体は実行を駆動するコルーチンではなくワーカープールで実行され、その `ctx.cancel_token` はその試行に属する実行のトークンの子です。実行をキャンセルすればこれまでどおりそのトークンもキャンセルされますが、そのトークンをキャンセルしてもキャンセルされるのはその試行だけです。そのため Program の実行では並列ステップのノード本体が重なって実行され、その `NODE_END` イベントは完了した順に届きます。`attempt_timeout_ms` を使わない定義はバイト列と id を保ちます。
- **Program: 失敗したノードは分類されたコードを報告します (#364)。** failure edge のないノードの失敗は、Program の実行を `P_RUNTIME_CORE_FAILURE` ではなくノード失敗の分類器のコード(`NG.TaskFailed`、`app.compile_error` のような作成者コード、`NG.ProviderFailed` など)で `Failed` にし、`failure.witness.node_failure` に `{node, code, kind, attempts, retryable, routable}` が入ります。`P_RUNTIME_CORE_FAILURE` はノード失敗のレコードを持たない Core の失敗にだけ残ります。オーケストレーション計画の `retry` 操作も同じ分類器に問い合わせ、論理、構成、予算、プロバイダーの失敗、コミットされた効果、到達したリトライ上限の後には本体を再実行しなくなりました。
- **ノードのリトライとツールの失敗が 1 つの失敗分類器に従います (#364)。** ノードのリトライループとツール失敗の契約は 1 つの分類器に問い合わせます。分類器は原因の連鎖全体(`NodeExecutionError` の原因、`std::nested_exception`、サブグラフの失敗)をたどり、見つかった最も深刻な分類を採用します。引き続きリトライされるもの: `std::runtime_error` とそのサブクラス、キャンセル以外の asio エラー、根本原因がそれらのいずれかであるサブグラフの失敗。リトライされなくなったもの: ノードの中で発生した Program の診断(C++ または Python の `ProgramDiagnosticError`、`ProgramCompileError`、`ProgramAdmissionError`)。`graph::ConfigurationFailure` の印により `NG.ConfigError` となり、ノードを再実行しても同じ診断が出ます。リトライされなくなったもの: 根本原因が `std::logic_error` または未知の例外であるサブグラフの失敗、`std::runtime_error` と `std::logic_error` の系統以外のすべての例外(`throw 42` を含む)、`std::nested_exception` の背後にあるクォータ失敗。ツールの原因の連鎖のどこかにある実行制御のシグナル(たとえばツールが実行したグラフの失敗に含まれる Hook のブロック)は、ツールメッセージにならず実行を停止するようになりました。[ノード失敗の契約](docs/reference-en.md#node-failure-contract)を参照してください。
- **ノードのリトライの各試行が独自の効果名前空間を持ちます (#364)。** 試行 0 はタスク id(`s3:act`)をそのまま使うので既存のジャーナルはスロットを保ち、試行 k は `s3:act#a<k>` を使います。そのためリトライは `ProviderCallBroker` に最初の試行のスロットではなく新しい呼び出しスロットを求め、リトライされたノードが実行するサブグラフは新しい子スレッドで始まります。試行 k の開始前に、その名前空間をキーとする空の保留書き込みが開始を記録するので、再開は試行 0 からやり直さずにその試行を続け、残りのリトライ回数も保ちます。`ToolEffectBroker` の下では、冪等でない効果をコミットした試行は決してリトライされず `NG.EffectCommitted` で失敗し、結果が不明な効果は、ノードがその割り込みをエラーに変えた場合でも照合のために実行を一時停止させます。
- **例 11（Clay チャットボット）を再設計しました。** テキストは UTF-8 を考慮した計測とともにシステムの TrueType/OpenType フォントを使うため、ハングルなどの文字も描画されます（`NEOGRAPH_CHAT_FONT`、`NEOGRAPH_CHAT_BOLD_FONT`、`NEOGRAPH_CHAT_MONO_FONT` で選択を上書きできます）。ウィンドウには、モデルと状態を示すヘッダー、アバター、コード ブロックを含む軽量な Markdown、提案付きのウェルカム カード、新しい出力を追うスクロールバー、応答のストリーミング中は Send ボタンが Stop になる入力欄があります。Stop と終了は応答を取り消します。
- **SchemaProvider `maint/interface6` の SDK 0.3.1、`69f8aba5c75decd21d786853de134ac713634966` を採用します。SchemaProvider の #26 から #29 を取り込みます。** interface revision と shared-library generation は 6 のままで、公開されている構造体や enum のレイアウトは変わりません。NeoGraph はビルドと、インストールされる `NeoGraphConfig` の `find_dependency` で `SchemaProvider 0.3.1 EXACT` を要求するため、インストール済みの 0.3.0 パッケージは合わなくなります。NeoGraph、C++ のコンシューマー、Python 拡張を、0.3.1 のヘッダーとライブラリーでまとめて再ビルドしてください。変わる点は次のとおりです。OpenAI に直接つなぐ Chat（`https://api.openai.com`）は、出力上限を `max_completion_tokens` で送ります。現行の OpenAI モデルが `max_tokens` を HTTP 400 で拒否するためです。OpenRouter と互換 endpoint は `max_tokens` のままで、明示した `bindings.max_output_tokens` はどの origin でも優先されます。ベンダーの 400 応答が拒否したパラメーターを名指ししている場合、その名前は `Error::safe_message` にも入ります（`invalid request: unsupported parameter 'max_tokens'`）。複数の結果を持つ Chat の `Tool` メッセージは、結果ごとに wire の `tool` メッセージ1件として送られます。世代 4.7 以降のすべての Claude モデルは `temperature` をローカルで拒否するので、`claude-haiku-5-5` が API まで届いて HTTP 400 を返されることはなくなります。途中で切れた Anthropic のストリームは、`message_start` の仮の出力トークン数を報告しなくなり、Gemini は `candidatesTokenCount` がないとき `output_total` と `total` を導出します。完了した HTTP 4xx 応答は `RejectedBeforeOutput` になります（次の項目を参照）。**挙動の変更:** 出力上限の member は native history の origin identity に含まれます。そのため、今回の採用より前に OpenAI に直接つなぐ Chat で封印された history は、続けようとすると `ReplayIneligible` になります。その会話で `bindings.max_output_tokens` を `max_tokens` に明示すれば続けられます。ほかの origin や family の会話、新しい会話には影響しません。
- **プロバイダーが返した完了済みの HTTP 4xx 応答は hold を解放し、何も課金しません (SchemaProvider #29)。** `provider_failure_proves_not_sent` は、トークン reservation、Program の金額 hold、dispatch receipt を `NotSent` のときだけ解放していたので、401、400、404 の応答でも予約の全額が hold のまま残っていました。SDK 0.3.1 では、こうした応答は `RejectedBeforeOutput` です。サーバーがリクエストを拒否したので、何も実行されず、請求もされません。新しい `provider_failure_proves_not_accepted` が、すべての精算経路（`ProviderBudgetClaim`、`ControlledProvider`、managed budget journal、Program のインプロセス broker と SQLite provider broker、MCP harness provider）が使う唯一の規則です。`NotSent` の根拠がある場合、または 408・425・429 を除く完了済みの HTTP 4xx 応答で、失敗に出力・stop reason・使用量がなく、請求された可能性のある以前の試行もない場合に成り立ちます。**挙動の変更:** こうした呼び出しは、予約の全額が請求され、`ProgramResult::monetary_records()` に `held` として残り、dispatch receipt も `ReconciliationRequired` でした。今後は金額の記録が残らず、モデルトークンも請求されず、`Failed` が記録されます。HTTP 408・425・429・5xx の応答、途中で切れた呼び出し、リクエストが出た後の transport 失敗は hold を維持し、以前のバージョンが保存した結果もそのままです。使用量の stage `Missing` は証拠がない状態、`Partial` はそこまでに見えた値で、どちらも呼び出しを精算しません。導出された件数は報告された件数と同じように精算されるので、出力予算を思考で使い切った Gemini の呼び出しは、予約の全額ではなく実際の使用量が請求されます。
- **SchemaProvider の `maint/interface6` の当時の先端 `f7ed8161fbcf39a6bf9358f78cfdcfda133537a2` を採用します: OpenRouter `provider.max_price` (#372)。** `sp::OpenRouterRouting` に、OpenRouter がルーティングしてよい endpoint 価格の上限 `max_price`（`sp::OpenRouterMaxPrice`）が加わりました。省略可能な整数 nanoUSD の金額で、`prompt` と `completion` は100万トークンあたり、ほかに `image`、`audio`、`request` があり、正確な10進の USD で送信されます（`75'000'000` は `0.075`）。OpenRouter は上限以下で掲載された endpoint にだけルーティングし、該当する endpoint がなければリクエストを失敗させます。金額が一つもない上限や 2^53 を超える金額を含む上限は I/O の前に拒否されます。`ProviderControls::provider` が Chat Completions、Responses、Messages のリクエストにこれを載せ、`SchemaProvider::Defaults::provider` はほかの routing フィールドと同じように統合します。リクエスト自身の `max_price` が既定値を置き換えます。Python は `ng.OpenRouterMaxPrice`（`prompt_nano_usd`、`completion_nano_usd`、`image_nano_usd`、`audio_nano_usd`、`request_nano_usd`）と `OpenRouterRouting.max_price` を公開します。後者は切り離されたコピーを返すので、編集してからオブジェクト全体を代入し直してください。上限は native configuration binding に含まれるため、ターンの間に変えるとキャプチャ済みの native history は `ReplayIneligible` になります。上限のない routing は以前のバイトと digest を保ちます。ライブラリは `RequiredProvider` に含まれる capability bit `sp::capability::OpenRouterRoutingMaxPrice` でこのフィールドを示すので、NeoGraph の handshake は上限を落とす以前の interface-6 ライブラリを拒否します。SDK 0.3.0 と interface/shared ABI 6 は変わりませんが、リクエストのレイアウトは大きくなりました。#372 の再ビルド項目を参照してください。 同じ SDK リビジョンでは、HTTP transport が libcurl のビルドや TLS バックエンドの既定値にかかわらず、すべてのプラットフォームで TLS 1.2 より古いバージョンを拒否します。
- **code scanning に基づくセキュリティ強化。** CI ワークフローはリポジトリの読み取り権限だけで実行します。NeoGraph 自身の HTTP/2 プールは TLS 1.2 以上だけを使い、HTTP と HTTPS だけを話します。テストのループバック TLS サーバーも古いプロトコルを拒否します。vendored QuickJS の `js_realloc_array` は増加量とバイト数を 64 ビットで計算し、`int` の乗算を桁あふれさせずに OutOfMemory で失敗します(`deps/quickjs/NEOGRAPH_PROVENANCE` に記録)。vendored Clay のサンプル ヘッダーは arena のサイズを `size_t` で計算します。`docs/promo` のプロモーション動画ツールは修正版の `source-map-js`、`postcss-selector-parser`、`fast-uri` を使います。
- **SchemaProvider の `maint/interface6` の当時の先端 `0d536be66017dfb8c4c91d21f4f62bd8ce698d55` を採用します。** OpenAI Responses を使うグラフ・エージェントのツールループは、ツールの parameter schema が strict 互換でない場合（例: `additionalProperties` を開いたままの MCP ツール schema）に HTTP 400（`InvalidRequest`）で失敗しなくなります。policy の strict 既定値は、すべての object が `"additionalProperties": false` を指定し各 property を `required` に列挙した schema にのみ適用され、それ以外のツールは `"strict": false` で送信されます。互換でない schema に明示的に `strict = true` を指定すると、ツール名と最初に失敗した JSON pointer を示す `InvalidRequest` で dispatch 前に失敗します。`created` タイムスタンプがチャンクごとに進む OpenAI 互換ローカルサーバーの Chat ストリームは `ProtocolCorrupt` で失敗しなくなります。値が減少した場合や `id`・`model` が変わった場合は引き続き失敗します。Responses の `response.failed` イベントやバッファリングされた失敗本文のように、codec が既に受け入れた in-band のベンダー失敗は、サイズにかかわらず自身のエラーコードで分類されるようになりました。以前は runtime の `max_error_bytes`（既定 64 KiB）を超えるもの、たとえば hosted `image_generation` の結果を繰り返す 3 MB の `response.failed` が、ベンダーの失敗ではなく `RetryClass::Never` の `ResourceLimit` として報告されていました。runtime が自らバッファリングして解析するエラーは引き続き `max_error_bytes` で制限されます。SDK 0.3.0 と interface/shared ABI 6 は変わりません。
- **SchemaProvider の `maint/interface6` にマージされた PR #25 の `f7cb27e68ee02f3a27d01c31af85bcac96f00963` を採用します。** `Usage::input_uncached` はすべての family で同じ規則（入力 − キャッシュ読み取り − キャッシュ書き込み。書き込みカウンタを送らない wire でのみ欠落値を0とみなす）に従います。OpenAI 直結の Chat もこの値を報告し、Responses は報告されたキャッシュ書き込みを差し引きます。Gemini の stop 種別はテストで固定しました。SDK 0.3.0 と interface/shared ABI 6 は変わりません。
- **SchemaProvider の `maint/interface6` にマージされた PR #23 の `45dd2ad36e18717adae3e2d10b2b9c8bb0e85471` を採用します。使用量がベンダー値と正確に一致します。** 実際の API 呼び出しで確認しました。OpenRouter Messages のストリーミングは末尾の `[DONE]` で失敗せず、報告されたコストも保持します。Chat は `video_tokens` などのベンダー詳細カウンタを保持します。報告される USD 金額がベンダーの10進値と一致し（+1 nanoUSD なし）、入力 + 出力 = 合計が成り立ちます。Gemini は合計が証明する場合に推論0を報告し、Interactions は modality を名前で記録し、`claude-haiku-4-5` のような Anthropic エイリアスは日付付きモデルで応答されても受け入れます。保存された結果に `CostRounding::CeilingShortestDecimal` が入ることがあり、以前の記録もそのまま読め、Python は `ProviderCostRounding.CeilingShortestDecimal` を公開します。SDK 0.3.0 と interface/shared ABI 6 は変わりません。
- **`llm::Agent` と deep-research、plan-execute のツールループが一時停止したターンを継続します (#311)。** `sp::StopKind::PauseTurn`（Anthropic/Chat の `pause_turn`）を受け取ると、一時停止した assistant ターンを含む履歴をそのまま再送し、既存の反復回数の予算内で動作します。以前は一時停止したターンを最終結果として返していました。同じ判定は `paused_turn()` で利用できます。リファレンスに Responses の `response.done` エイリアス規則と、宣言済みゲートウェイのサービングモデル規則を記載しました。
- **SchemaProvider の `maint/interface6` ブランチにマージされた PR #21 の `83112573ba59e3b561fc33c22394638be7aa5294` を採用します。SDK 0.3.0、interface/shared ABI 6 はそのままで、ルーティングゲートウェイの修正が加わります。** Responses と Messages は、宣言済みの OpenRouter origin が `~deepseek/deepseek-v4-flash-latest` のようなルーティングエイリアスに対して返す実際のサービングモデル名を受け入れるようになりました。以前はこのような Responses 呼び出しがすべて `invalid response protocol (HTTP 200)` で失敗していました。サービングモデルは 1 つの応答内で変わってはならず、replay は要求したモデルに束縛されたままです。NeoGraph の API・ABI に変更はありません。
- **マージ済みSchemaProvider PR #16の`3b88e4ba020c3a4d39ff0660014e7292b516b7cb`を採用し、SDK 0.3.0 EXACT、interface/shared ABI 6で再ビルドします。** 以下に記録した以前のinterface-5 SDK手順を置き換えます。blocking呼び出しは単一async SDK pipelineを使い、event-loop/callback workerからの再入拒否を維持します。outcome、usage観測、journal、native archive再読込、Python型付き値はprovider報告金額、status、source、evidence、rounding、競合を保持します。canonical v2 JSONは金額情報が完全に欠落する場合のみ`provider_cost`を省略し、過去の欠落のみのbytesを保持します。ゼロやBYOK falseも金額の根拠であり、不明なtoken holdをproven-not-sentとして返金させません。金額をtokenや支出権限に変換しません。Native archive v3とportable JSON v2は変わりません。
- **Core エンジンの速度を v0.11.1 の水準に戻しました。** 過去の測定では、3 ノードの逐次ベンチマークが 1 回あたり 10.19 µs から 6.29 µs に(v0.11.1: 6.10)、parallel-3 が 30.03 µs から 15.66 µs に(15.80)短縮され、スレッドごとにエンジンを 1 つ使う場合のスレッドあたりの時間も 2・4・8 スレッドで v0.11.1 と同じです。損失の原因は、fan-out の枝ごとの strand、ノードごとの `RunContext` コピー、コンテキスト束縛レジストリを囲んでいたプロセス全体の mutex(現在は 64 シャード)、複数の JSON ドキュメントで作っていた状態スナップショット、何も返さないコルーチン呼び出し(Send がない `run_sends_async`、チェックポイントがないときの `record_pending_write_async`)、スケジューラの順序付き集合でした。コミットごとの測定値は #343 にあります。
- **`neograph::core` だけをリンクするプログラムは libcurl を読み込まなくなりました。** `neograph::core` が SchemaProvider のランタイムをリンクし、そのランタイムが libcurl のトランスポートをリンクしていたため、グラフだけを実行するプロセスでも libcurl とそれが必要とする約 25 個のライブラリを読み込んでいました(ベンチマーク開始時の常駐メモリは 12.7 MB、v0.11.1 は 5.0 MB)。トランスポートをリンクするのは `neograph::llm` だけになり、同じベンチマークは 7.7 MB で始まり、共有ライブラリは 36 個から 7 個になりました。`neograph::llm` を使わずに `sp::runtime::Client` を自分で構築するコードは `SchemaProvider::transport` をリンクする必要があります。SchemaProvider はこの分離を含むリビジョンに固定されます。#347 を参照してください。
- **SchemaProvider 0.2.0 alpha、interface/shared-library 世代 5 で再ビルドしてください。** SDK に、任意の接続・最初の応答・アイドル timeout が加わりました。既定では無効で、総 deadline を延長せず、`ProviderRequest::options`(`sp::runtime::RunOptions`)から呼び出しごとに設定できます。グラフの LLM ノードはまだ公開していません。五つの API family の実 transport lifecycle 検証と独立した private test fixture が SDK #2・#3・#4 に対応します。SDK header/library を揃えて C++ 利用コードと Python wheel を再ビルドしてください。Native archive は v3、portable JSON は v2 のままです。
- **`neograph::core` だけをリンクするプログラムは libcrypto を読み込まなくなりました。** SDK が SHA-256、HMAC-SHA256、OS の乱数源の利用を自前で実装したため、SDK ランタイムは OpenSSL をリンクせず、archive と policy のバイトは変わりません。グラフだけを実行するベンチマーク(`bench_neograph 1 1 1`)は、常駐メモリ 7.7 MB ではなく 6.3 MB で始まり(Linux x86_64、31 回の中央値)、共有ライブラリは 7 個から 6 個になって、5.0 MB で始まっていた v0.11.1 と同じになりました。OpenSSL を直接呼ぶコードは `OpenSSL::Crypto` を自分でリンクする必要があります。

### 追加

- **Core: ノードは、再開がどの割り込みに答えているかを判別できます (#406)。** `RunContext::resume_interrupt_id`（Python では `input.ctx.resume_interrupt_id`）は、`resume_value` がノード自身の `NodeInterrupt` への答えであるとき、その割り込みの id です。再開が割り込みを指定した場合も、最初の割り込みに答えた場合も、`RunResult::interrupt_id` と `RunResult::interrupts` が報告する id と一致します。値がノードを止めていた `interrupt_before` ゲートの承認であるときは空です（Python では `None`）。ゲートの後ろにあって自分の質問もするノードは、実行の承認を自分の質問への答えとして受け取らずに済みます。id が空の間は質問すればよいからです。再開値をどの呼び出しが受け取るかは変わりません (#393)。`RunContext` にメンバーが増えたため、ネイティブの利用側は対応するヘッダーで再ビルドしてください。
- **Program: 平らな step plan（`step-plan/v1`）を検査し、task graph proposal に変換します。** `intake_step_plan`（`neograph/program/step_plan.h`）は planner の応答のテキストか解析済みの文書を受け取り、すべての問題を安定した `STEP_*` コードを持つ `{path, code, message, hint, detail}` violation のソート済み一覧として返すか、計画をバイト単位で決定的にスキーマ 2 の `TaskGraphProposal` に変換して、step に戻す source map とともに返します。計画は、ホストの allow-list にあるテンプレートを指名し、各入力にリテラルか前の step への参照を与え、順序エッジと再試行のヒントを加えられる、順序のある step のリストです。形式は閉じており、プロバイダー、認証情報、予算、実行可能なテキストを持たず、循環も含められません。変換後の proposal は変更のない信頼経路（`TaskGraphProposal::parse`、`TaskGraphFragmentCompiler::compile`、`expand_task_graph`）を通ります。intake を通ったことは、計画の形式が正しくポリシーの範囲内にあることを意味し、要求を解決することは意味しません。フィードバックでは、計画から取った文字列を表示可能な 64 バイトに切り、リテラルの値は決して出しません。`format_violations_for_model` は violation を長さの制限されたプロンプトのテキストにし、`step_plan_json_schema` はカタログ向けの JSON Schema を生成し、`schemas/step-plan-v1.schema.json` はカタログに依存しない形です。[`docs/plan-intake.md`](docs/plan-intake.md) を参照してください。
- **Program: 回数に上限のある plan repair ループ。** `add_step_plan_repair` は `neograph.step_plan_intake/v1`、`neograph.step_plan_route/v1`、`neograph.step_plan_report/v1` を登録し、`plan -> intake -> (plan again)` を普通の Core グラフにします。intake ノードは planner の最後の応答を検査し、変換後の proposal とレポート（`status`、`attempts`、`last_violations`、`violation_code_histogram`、`plan_digests`、`envelope`）をチャネルに書き、violation を入れて `messages` を作り直します。`feedback` が `replace`（既定）ならコンテキストは増えず、`append` なら会話を保ちます。試行回数、run の予算、プロバイダー呼び出しのジャーナル、Core のチェックポイントがループを制限して記録するので、再開した run がモデルに聞き直すことはありません。あきらめたループは、フォールバックの分岐か failure exit（`plan.repair_exhausted`）で終わります。ホストの allow-list と上限は登録される実行体のオプションなので、封印されたグラフは変更できません。`repair_step_plan` は、run を始める前に計画するホストのために、同じループをコールバックの周りで実行します。
- **Program: task graph の task がリテラル入力と試行回数を持てるようになり、スケジューラーが再試行します（proposal と fragment のスキーマ 2）。** スキーマ 2 の task は `literals`（binding の後に、宣言された入力フィールドへ代入する JSON 値）と `max_attempts` を持てます。新しいコードは `P_PROPOSAL_LITERAL_TARGET`、`P_PROPOSAL_LITERAL_DUPLICATE_TARGET`、`P_PROPOSAL_LITERAL_SIZE`、`P_PROPOSAL_ATTEMPTS` で、スキーマ 1 の文書がどちらかのメンバーを使うと `P_PROPOSAL_SCHEMA_VERSION` です。ホストは `TaskGraphProposalLimits::max_literal_bytes`、`max_total_literal_bytes`、`max_attempts` で利用を許可し、既定値（0、0、1）ではリテラルも再試行も拒否します。集計は task の予算を試行回数分だけ予約し、試行ごとに子は予算全体を受け取って起動します。task の子が再試行可能な Core ノード失敗（plan レベルの `retry` 演算と同じ規則）で終わり、試行が残っているとき、スケジューラーは次の試行番号、新しい試行 identity、新しい子で task を再びディスパッチします。再試行する試行には `Failed` を書き込まず、依存する task は待ち、`fail_fast` も作動しません。どちらも使わない文書と fragment は、バイト列と identity が変わりません。
- **Core: ノードは再開がどの割り込みに答えるのかを知ることができます (#406)。** `RunContext::resume_interrupt_id`（Python では `input.ctx.resume_interrupt_id`）は、`resume_value` がノード自身の `NodeInterrupt` への答えであるときの割り込みの id で、再開が指定したか最初の割り込みに答えたかにかかわらず `RunResult::interrupt_id` と `RunResult::interrupts` が報告する id です。値がノードを止めた `interrupt_before` ゲートの承認であるときは空です（Python では `None`）。ゲートの後ろにあり自分の質問もするノードは、実行の承認を答えとして受け取る必要がなくなりました。id が空の間は質問します。再開値を受け取る呼び出しは変わりません (#393)。`RunContext` にメンバーが増えたため、ネイティブの利用側は対応するヘッダーで再ビルドしてください。
- **A2A: native server の `ListTasks`、`SubscribeToTask`、non-blocking send。** `ListTasks` は `contextId`、`status`、`statusTimestampAfter` で絞り込み、新しい順にページングします。`SubscribeToTask`(0.3 では `tasks/resubscribe`)は現在の task とリアルタイムの event を stream します。`returnImmediately`(0.3 では `blocking: false`)は送信された task を即座に返します。すべての run は専用の worker で実行されるため切断された接続より長く存続し、すべての status に timestamp が付きます。push notification、extended card、HTTP+JSON/gRPC binding は主張せず、それらの method は `-32003` / `-32004` で応えます。
- **ACP: client が提供する stdio MCP server** を NeoGraph の MCP client で起動し、node は `ACPClient::session_tools()` でその tool にアクセスします。server が受け取るのは最小限の環境、つまり agent の `PATH`(Windows では `SystemRoot` も)と client の `env` だけで、provider の API キーを含みうる agent のその他の環境は受け取りません。
- **A2A: card が collaboration envelope を extension として宣言します。** `CollaborationMailbox` を持つ Program agent は `capabilities.extensions`(`AgentCard::extensions`)に `a2a::COLLABORATION_EXTENSION_URI` を載せるため、collaboration message の送信者が決めた冪等な task id は宣言された extension になります(A2A spec 4.6)。
- **ACP: 増分 update。** model token は `agent_message_chunk` として stream され、tool 呼び出しは `tool_call`/`tool_call_update` として報告されます。
- **公式 SDK の interop suite と protocol-interop CI job。** `bindings/python/tests/interop/a2a_suite` は a2a-sdk 1.2.2 と @a2a-js/sdk 1.3.0、0.3.14 で native の A2A server と client を動かし、すべての message を A2A v1.0.1 の proto と JSON Schema、v0.3.0 の JSON Schema で検証します。`acp_suite` は agent-client-protocol 0.12.1、1.0.0rc2 と @agentclientprotocol/sdk 1.8.0 で native の ACP agent(`neograph_acp_probe_agent`)を動かし、すべての message を ACP schema v1.25.0 で検証します。suite は SDK のバージョンを固定し、固定した SDK がなければきれいに skip し、pin と Node.js 22 をインストールする `.github/workflows/protocol-interop.yml` で実行されます。
- **MCP: 2025-03-26 と 2024-11-05 の相手からの JSON-RPC バッチ。** MCP 2025-03-26 は、受信側が JSON-RPC バッチを受け付けることを求めます。`MCPServer` と `MCPHttpServer` は、`2025-03-26` をネゴシエートしたクライアント、またはバッチを定義していないものの同じように扱う `2024-11-05` のクライアントからのバッチに、stdio と HTTP で 1 つの配列で応答するようになりました(通知だけのバッチは HTTP 202)。キャンセルされた呼び出しは除かれ、応答するものが残らないバッチには何も送りません。後の版はバッチを廃止し、initialize はバッチに入れられないため、そのようなバッチは以前と同じく `-32600` のエラー 1 つ (HTTP 400) です。`MCPServer::handle_message(message, reply)` は各メッセージの結果 (応答、バッチの配列、またはなし) を呼び出し元に渡し、`MCPHttpServer` と stdio ループがこれを使います。NeoGraph の MCP クライアントは、サーバーが送ったバッチをメッセージごとに処理し、その中のリクエストには 1 つの配列で応答します。MCP 相互運用テストスイートは、各セッションのメッセージを、そのセッションがネゴシエートした版のスキーマで検証します。
- **MCP: protected resource metadata、制限付きの initialize、ツール一覧の変更、継承される環境。** `MCPHttpServerConfig::protected_resource_metadata` は OAuth の配備向けに RFC 9728 のメタデータを公開し、すべての 401 チャレンジで `scope` とともにその場所を示します。`MCPClient::initialize(name, deadline, cancel_token)` はハンドシェイクを制限します。`MCPClient::set_tool_list_changed_handler` は `notifications/tools/list_changed` を知らせます。`replace_environment` なしの `StdioClientConfig` は、継承される環境に追加または上書きする `environment` の項目を受け付けるようになりました。以前は拒否されていました。
- **Python の MCP クライアント: トランスポートエラー、強化された stdio、shutdown、制限付きの呼び出し。** `mcp.MCPTransportError` は `failure` と `http_status` を、`mcp.MCPError` も `http_status` を持ち、`mcp.StdioClientConfig` と `MCPClient(config)` は強化された stdio サーバーを起動し、`MCPClient.shutdown()` はクライアントを恒久的に終え、`initialize()` と `call_tool_result()` は `timeout` と `cancel_token` を受け取り、`set_tool_list_changed_handler()` は callable を受け取ります。
- **公式 MCP SDK との相互運用テストとその CI ジョブ。** `bindings/python/tests/interop/mcp` は、公式の Python SDK(`mcp` 2.3.0 と 1.30.0)と TypeScript SDK(`@modelcontextprotocol/sdk` 1.32.1、`@modelcontextprotocol/client` と `@modelcontextprotocol/server` 2.3.1)を NeoGraph の MCP サーバーとクライアントに対して実行し、取得したすべてのメッセージを MCP 2025-11-25 の `schema.json` で検証します。新しい `MCP interop` workflow は、固定された SDK と Node をインストールしてこれを実行します。SDK がなければテストはスキップします。
- **Program: オプトインの健全性チェック (#363, #364, #365)。** `ProgramCompilerConfig::soundness_check`（Python: `ProgramCompiler` と `LocalProgramHost` の `soundness_check=True`、`GraphValidator`: `ValidationOptions::soundness_check`）は、ルートグラフのルートが許すすべての実行を探索します。状態は実行準備の集合、各 join が保持するシグナル（`any_of` スロットを含む）、`max_visits` を持つノードの訪問回数で、conditional ルートと失敗 edge は非決定的な選択、failure exit は実行を終えます。チェックは、それを示す最短のルートの並びを witness とする警告を返します: `P_CORE_S1`（ある時点からどの実行も完了できない）、`P_CORE_S2`（join がシグナルを保持したまま実行が終わる。barrier-stalled または明示的な終了）、`P_CORE_S3`（発火しない join）、`P_CORE_S4`（出口のないループ）、`P_CORE_S5`（1 つの overwrite チャネルに書く 2 つのノードが同じ super-step で準備される。`P_CORE_E5` の fan-out の兄弟以外）。help は witness のルート選択を指し、スロット外のノード 1 つが埋められる停止には `any_of` スロットを提案します。`P_CORE_SOUNDNESS` の要約は、すべての実行を探索した場合はノート、そうでなければ警告です。タイプが `Command.goto` または `Send` を宣言した（effect 契約 `"control": ["goto"]` または `["send"]`）ノードの後の実行は解析できず、`soundness_state_cap`（既定 100 000 状態）に達するとチェックは not checked として止まります。どちらの場合も探索していないことは主張しません。何も封印しないため、バンドル id は変わりません。有界網羅の join ハーネスが列挙するすべてのグラフで、判定がその参照インタープリターと一致し、各 witness が段階ごとに再現されます。capability manifest にフラグ `diagnostics.soundness_check` が加わりました。
- **Program: 失敗ルーティングのキーをコンパイルします (#364)。** `graph.retryPolicy(node, {max_retries, non_retryable, max_visits, error_text, on_failure, error_channel, ...})` は厳格な Core キー `node_retry_policies`（書いたフィールドだけ。`max_retries` 0〜10、`max_visits` 1〜100 などの範囲付き）と `failure_edges` に変換され、`graph.failureExit(node, {code})` は `failure_exits` を書きます。コンパイラーはこれらを検査し（番兵、自分自身、join、暗黙の終端への対象、1 つのノードからの 2 本目の edge、append でない error channel には `P_CORE_GC_FAILURE_EDGE`、範囲とコードには `P_CORE_GC_FIELD_VALUE`、未知の名前には `P_CORE_E3`）、failure edge をルーティング済みの後続マップに加えて E7、E11、E15 から見えるようにし、`error_channel` を書き込み済みと数え、誰も読まないノード設定の `retry_policy` キーを `P_CORE_E18` で警告します。グラフ単位のポリシーの `on_failure` はヒント付きの未知のフィールドです。これらのキーは封印され、`node_retry_policies` を除いてマイグレーションの形状に含まれます。使わない定義は id を保ちます。これを実行するランタイムは次の項目です。`TopologySpec` と `CompiledGraph` にメンバーが追加されました。[Program グラフ機能](docs/program-graphs.md)を参照してください。
- **失敗ルーティングのランタイム (#364)。** 最終的な失敗がルーティング可能で failure edge を持つノードは、実行を失敗させなくなりました。失敗した試行は何も書かず、構造化されたレコード 1 つ(`node`、`kind`、`code`、`attempts`、`retryable`、`step`、`routed_to`、`effects`)が edge の `error_channel` に追加され、ノードは edge の対象にだけルーティングされます。静的ノード、ファンアウトの分岐、1 つまたは複数の `Send` の対象、サブグラフのノードは同じ経路をたどり、クラッシュ後の再開はノードを再実行せずに pending write からルートを再生し、`interruptBefore` の対象は `{"reason": "failure_route", "failure": record}` とともに停止し、`ERROR` イベントには `handled` と `routed_to` が加わります。自由テキストはノードごとに選択(`error_text`)し、型付きの `public_message` からだけ取り、ホストの `EngineConfig::failure_text_redactor`(`CatalogConfig::failure_text_redactor`、Python は `GraphEngine.compile` と `LocalProgramHost` の `failure_text_redactor=`)が必要で、見えない文字を取り除いて 512 バイトに切り詰めます。リトライのタイミングはフィールドごとに決まり、定義がノードごとのポリシーを宣言したノードでは、ホストのノードごとのポリシーは厳しくすることしかできません(`GraphEngine::resolved_retry_policy`)。`non_retryable` のコードはリトライしません。`max_visits` は上限を超えた活性化を `NG.VisitLimit` としてルーティングし、カウンターはチェックポイントのメタデータにあります。failure exit のノードは実行を `RunStatus::FailureExit` と `output["_neograph"]["failure_exit"]` で終え、Program の実行を宣言されたコードで `Failed` にします。`RunBudget::max_node_retries`(Python は `ProgramRunBudget.max_node_retries`)は実行全体のノードのリトライを制限し、`ProgramUsage::node_retries` として報告されます。どちらも設定されたときだけシリアライズされるため、既存のアイデンティティは変わりません。マニフェストのフラグ `retry_policy.per_node`、`failure_edges`、`failure_exits` がオンになりました。[Program グラフ機能](docs/program-graphs.md#failure-routing)を参照してください。
- **試行タイムアウトと期限を考慮したバックオフ (#364)。** `graph.retryPolicy(node, {attempt_timeout_ms})`（Core `node_retry_policies[node].attempt_timeout_ms`、100〜3 600 000）はノードの各試行を制限します。タイムアウトした試行は自分の子の `CancelToken` を通じてキャンセルされ、リトライされルーティングできる `NG.Timeout` で失敗します。最初の終了イベントが勝つので、タイムアウトの時刻かその後の完了はタイムアウトです。その試行の遅れた完了は捨てられます。書き込み、結果、イベントは実行に届かず、その試行が始める仲介された Tool 呼び出しは拒否され、次の試行はその試行が止まってから始まります。停止の期限（もう一度タイムアウト分、少なくとも 1 秒）までに止まらない試行は `NG.EffectUncertain` で実行を失敗させます。その試行はバックグラウンドで続いて自分のノードを生かしておき、`wait_for_node_attempts()` がそれを待ちます（Python バインディングはインタープリターの終了時に待ちます）。次の試行の前のバックオフが実行の期限（Program の `wall_time_ms`、`RunMetadata::deadline`）を越えるなら、ノードは予算が実行を終えるまで眠らず、ただちに kind `deadline` でリトライをやめます。オーケストレーション計画の `retry` 操作も、そのような停止の後には本体を再実行しません。静的なノード、fan-out の分岐、`Send` の対象、サブグラフのノード、同期と非同期の入口がすべて同じように振る舞い、クラッシュ後の再開は実行中だった試行を続けます。`GraphEngine::set_node_attempt_timeout_ms` と `resolved_attempt_timeout_ms`（Python にもあります）で、ホストはタイムアウトを設定でき、定義のタイムアウトは短くすることしかできません。`NodeAttemptTimeout` はタイムアウトした試行の失敗の原因です。マニフェストのフラグ `retry.attempt_timeout` が有効になりました。[Program のグラフ機能](docs/program-graphs.md#attempt-timeouts-and-the-deadline)を参照してください。
- **Program: 金額予算のもとでバインドされた `llm_call` provider (#372)。** Python のホストが Program の `llm_call` ノードに provider をバインドします。`ProgramRegistryBuilder.add_provider(name, version, digest)` が provider を宣言し、`add_registered_node("llm_call", ..., provider=name)` がそれを要求し、`LocalProgramHost(..., node_context=ng.NodeContext(provider=..., model=..., provider_controls=...), binding_label=...)` が MCP harness と同じやり方の `CatalogCapabilityBinder` でバインドします。identity に入るのは検証済みの秘密でないラベルだけで、provider の capability receipt として入ります。キーとモデルは入らず、provider を必要としない Program はバンドル id とバージョン id を保ちます。バインドされたすべての呼び出しは、`neograph::program` にコンパイルされるようになった価格カタログで、実行の `monetary_microunits` に対して価格が付けられます。dispatch の前に broker は呼び出しの入力上限と出力 cap に対するカタログの上限（OpenRouter では route ceiling）を予約し、上限を決められない呼び出しや予約が収まらない呼び出しは、リクエストを送る前に `P_MONEY_*` の失敗で拒否します。結果が出ると、報告費用（OpenRouter の `usage.cost`、証拠 `reported`）またはカタログの推定（カタログの version と digest を記す `estimated`）で精算し、結果や usage が分からなければ予約額を保持します（`held`）。`ProgramResult::monetary_records()`（Python の `result.monetary_records`、`ng.ProgramMonetaryRecord`）が呼び出しを列挙し、`ProgramUsage::monetary_microunits` はその合計です。C++ のホストは `bind_priced_provider_call(context)` または `SQLiteProgramProviderCallJournal::bind_priced(context, deployment)` を使います。後者は各呼び出しの記録を journal 行に残し、クラッシュ前に記録された呼び出しは復旧後に 1 回だけ請求され（期限切れの execution lease が実行を終えるときも同じ）、以前の attempt が記録した呼び出しは再び請求されません。`LocalProgramHost(sqlite_directory=..., native_history_archive=...)` は実行、チェックポイント、その journal を SQLite ファイルに保持し、`reconnect(run_id)` は停止したプロセスが実行中のまま残した実行を続けます。OpenRouter では broker が Chat Completions、Responses、Messages のリクエストに route ceiling を `provider.max_price` として固定し、ホストがすでに指定したより低い上限はフィールドごとに保ちます。そのため OpenRouter は、呼び出しを予約した価格以下で掲載された endpoint にだけルーティングします。[価格の証拠](docs/reference-en.md#pricing-evidence-and-the-price-catalog) と [Python ガイド](docs/python-binding.md#model-calling-nodes) を参照してください。
- **型付きのノード失敗 (#364)。** `NodeFailureError(code, public_message, retryable)` は `app.compile_error` のような作成者コードでノードを失敗させ、`retryable = false` ならリトライループを止めます。`node_failure_of()` はエグゼキューターが送出する `NodeExecutionError` に付いた `NodeFailure` レコードを返します。レコードにはコード、分類、リトライをやめた理由、試行回数、リトライ可否とルーティング可否のフラグ、`public_message`、最後の試行の Tool の効果が入ります。Python のノードの例外も `code`、`retryable`、`public_message` 属性から同じように読み取られ、呼び出し元はそれでも元の例外を受け取ります。
- **Program: witness の経路、`any_of` の提案、ルート表 (#363, #365)。** `P_CORE_E15` の witness に、barrier のメンバーから各迂回ルートを通るルートの並び `paths` が加わり、その並びが join に戻る場合は `substitute` と `suggested_wait_for` も入ります。返される診断は、join の直前のノードをメンバーの代わりにする `any_of` スロットを提案します（#357 なら `[{"any_of": ["B", "FIX"]}, "C"]`）。止まった実行の witness にも、トレースとルーティング済みの後続マップから求めた同じ `paths` と `suggested_wait_for` が加わります。`ProgramCompilerConfig::echo_routes`（Python: `ProgramCompiler(..., echo_routes=True)`）はルーティング済みの後続表を `P_CORE_ROUTES` ノートとして返し、`GraphValidator::routed_successors()` がそれを公開します。いずれも封印されません。
- **Program: barrier join の `any_of` スロット (#363)。** `barrier("D", [{any_of: ["B", "FIX"]}, "C"])` はスロットごとに選択肢を 1 つ待ちます。いずれかの選択肢が join にシグナルを送るとスロットが満たされ、すべてのスロットが満たされると join が実行され、その後すべてのシグナルがクリアされます。#357 の形は両方のルートで完了するようになりました。join の実行後に到着した選択肢は終わらないラウンドを開くため、実行は `P_CORE_BARRIER_STALLED` で終わります（安全側の失敗）。新しい警告 `P_CORE_E17` は両方とも実行され得る選択肢を報告します。名前が 1 つのスロットはその名前と同じなので、従来と同等の指定はバイト列と id を保ち、それ以外の指定は UTF-8 のバイト順に並べて新しい `TopologySpec::join_specs` に出力され、`CompiledGraph` と `Scheduler` もこれを持ちます。バリデーターは E3、E8、E15 をスロットごとに検査し、停止の witness はスロットを出力し、マイグレーションアダプターはスロット join を barrier として挙げます。any や quorum などの join モードは引き続き保留です。`Scheduler` にメンバーとコンストラクターが追加され、レイアウトが変わりました。[Program グラフ機能](docs/program-graphs.md)を参照してください。
- **Program: グラフメタデータ (#362)。** `ng.graph("plan").metadata({...})` は、ホストが読む作成者の主張を Core 定義の最上位 `"metadata"` オブジェクトとして保存し、`bundle.sealed_core_definitions()` から読めます（Python: `bundle.sealed_core_definitions[0]["definition"]["metadata"]`）。定義とともにハッシュされ、ノードファクトリー、状態、チェックポイント、出力、イベントには渡りません。上限は正規 JSON で 8 KiB、入れ子の深さ 4、メンバー 64、キー 1〜128 バイトで、不可視文字はエラーです（`P_CORE_GC_METADATA_LIMIT`、`P_CORE_GC_METADATA_TEXT`）。`GraphSemanticMigrationAdapter::metadata_changes()` はメタデータの差分を挙げ、この差分は `topology_shape_id()` を変えません。メタデータを含め Program API v2 のキーを使う Core 定義はバリデーターの警告を封印せず、使わないプログラムはバンドル id を保ちます。`TopologySpec` と `CompiledGraph` にメンバーが追加され、レイアウトが変わりました。[Program グラフ機能](docs/program-graphs.md)を参照してください。
- **Program: JavaScript 機能マニフェストが機能フラグ、シグネチャ、引数スキーマを公開します (#365)。** `javascript_authoring_capability_manifest()` に `features`（作成機能ごとのブール値。例: `graph.barrier`、`graph.metadata`、`failure_edges`。ホストやモデルがこのビルドで何が使えるかを確認できます）、すべてのメソッドの TypeScript `dts` シグネチャと共通宣言 `dts_prelude`、グラフビルダーの引数ごとの JSON Schema が加わりました。新しいスキーマファイル: `schemas/javascript-capability-manifest-v1.schema.json` はマニフェストを、`schemas/core-definition-v1.schema.json` は制約付きデコード用に封印済みの Core 定義を記述します。
- **Program: 診断に自己修復用の help が付きます (#365)。** コンパイルが返すすべての診断（`ProgramCompileError::diagnostics()`、`ProgramBundle::compile_diagnostics()`、Python 例外の `.diagnostics`）に `help` があります。新しい[診断ガイド](docs/diagnostics.md)の該当項目へのリンク、ラベル付きの関連位置、適用可能性（`machine`、`maybe`、`placeholder`）付きの RFC 6902 JSON Patch 編集による修正案、そして未知の名前から編集 2 回以内にある宣言済みの名前 `candidates` が入っています。同じ未知の名前についての診断は 1 つにまとめられ、残りの位置は関連位置になります。返される診断はエラーを先頭に最大 20 件で、その後に `P_DIAGNOSTICS_TRUNCATED` ノートが続きます。JavaScript のメッセージはビルダー呼び出しの名前を使います。例えば `graph.retryPolicy({...}) has an unknown field 'on_exhausted'` です。封印された診断には `help` が含まれないため、バンドル id は変わりません。`program::Diagnostic` にメンバーが追加されレイアウトが変わったため、C++ の利用コードを再ビルドしてください（[`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照）。
- **Program: `P_JS_CONTROL_CHARACTER` 警告 (#361)。** JavaScript の設定文字列にタブ、改行、復帰以外の C0 制御文字が含まれる場合（例: 正規表現の単語境界のつもりの `"\b"`）、その文字列の呼び出し位置で警告します。この警告はコンパイルが返す診断にのみ含まれます。
- **トポロジー検証が scheduler の実際の経路に従い、さらに 4 つの問題を報告します (#357, #358)。** conditional edge を持つノードは最初の conditional edge だけでルーティングするため、E7（到達不能なノード）、E8（barrier liveness）、E11（`__end__` への経路なし）は、そのノードの static edge や後続の conditional edge を数えなくなりました。E13 は conditional edge も持つノードから出る static edge を報告します。この edge は決して発火しません。E14 は同じノードからの 2 つ目の conditional edge を報告します。この edge は決してルーティングしません。E15（警告）は、join または `__end__` 以外へ向かう route を持つ barrier メンバーを報告します。その route を通ると run は barrier-stalled で終わります。E16（エラー、opt-in）は DAG を必要とするホスト向けにあらゆる cycle を報告し、`ValidationOptions::require_acyclic`、`ProgramCompilerConfig::require_acyclic`、または Python の `ProgramCompiler` と `LocalProgramHost` の `require_acyclic=True` で有効にします。reads も writes も列挙しない effects 契約は未宣言として扱われるようになったため、E6 は Python ノードが書き込むチャネルを dead として報告しなくなりました(#361)。**動作の変更:** E13 と E14 は strict ドキュメント（`schema_version` 1 以上で、すべての JavaScript Program を含む）ではエラーになるため、そのようなグラフはコンパイルできなくなりました。legacy ドキュメントでは警告になります。bundle id は変わりません。Program のコンパイルは現在の規則で失敗し、その警告を `compile_diagnostics` で返しますが、封印するのは以前の規則の警告（`ValidationOptions::original_rules`）で、admission もその規則で link するため、以前に封印された bundle は引き続き materialize されます。`EngineConfig::validation` は `GraphEngine` の build と link で使う規則を選びます。
- **`EngineConfig::stderr_notices` (#361)。** 既定値は `true` です。`false` のとき、`build()` と `link()` は検証 lint を stderr に出力せず（エラーは引き続き throw されます）、fan-out が直列に実行されるという 1 回限りの通知も出力されません。Program Catalog はこれを無効にしてエンジンを link するため、`compile_admit` は `[neograph] lint [E6] ...` を出力しなくなり、Program の run も、`LocalProgramHost` の利用者が設定できなかった `EngineConfig::worker_count` に言及する通知を出力しなくなりました。Program の lint は診断を通じてのみ呼び出し側に届きます。
- **Python: `LocalProgramHost(..., worker_count=1)` (#370)。** グラフ内の並列 fan-out のために、`CatalogConfig::worker_count` を通じて各 Core エンジンのワーカープールを設定します。admission 済みの Program の `max_concurrency` を超えてはなりません。クラスの docstring で `scheduler_threads` と `worker_count` を説明しています。
- **`ChatTool::strict` でツールごとに OpenAI Responses の strict モードを指定できます。C++ の利用コードは再ビルドが必要です。** このフラグは既定で未設定の `std::optional<bool>` で、`make_provider_request` が Responses の function ツールへ渡します。未設定なら上記の SDK 既定値に従い、`false` はそのまま送信され、strict 互換でない schema に `true` を指定すると dispatch 前に `InvalidRequest` で失敗します。他の family はこのフラグを送れないため、`true` を `std::invalid_argument`（Python では `ValueError`）で拒否し、`false` は受け入れます。Python: `ChatTool(..., strict=None)` と `strict` 属性。`ChatTool` に member が加わり layout が変わったため、NeoGraph、C++ の利用コードと独自の `Tool` 実装、Python 拡張を一緒に再ビルドしてください（[`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照）。
- `CheckpointCoordinator::records_pending_writes(parent_cp_id)`: `record_pending_write[_async]` が実際に保存するときに true を返すので、呼び出し側は呼び出し自体を省略できます。

### 修正

- **Program: 実行リースを持つ実行の `reconnect()` が、その理由を伝えます (#421)。** 子の予算なしで始めた `define()` だけの Program の実行は、実時間の間、実行リースを保持します。そのため `reconnect()` は、リースが切れるまで「別の稼働中のランタイムが所有している」という文言の `P_RUN_LEASE_HELD` を送出し、子の予算が必要であるかのように見えました。エラーは今、リースを明示し、リースが切れた後は `reconnect()` が実行を `TimedOut` で終わらせると伝え、リースを持たず最後のチェックポイントから続行されるものとして `main()` と子の予算（`max_child_depth`、`max_total_children`）を挙げます。witness には `expires_at_ms` と `retry_after_ms` が入ります。どの実行がリースを持つかは変わりません。Program ガイドの実行の再起動の項に、この規則を載せています。
- **Program: フォークは元の量から自分の取り分だけを取り、並行度は取らず、元の実行を再開できなくすることもありません (#414)。** フォークの予算はそっくり、一時停止した実行の残りの予算から差し引かれ、`max_concurrency` も例外ではありませんでした。そのため、実行の `remaining_budget` をまるごと渡したフォークは、元の実行に並行度を残さないか（`P_CONCURRENCY_BUDGET`、`P_TRANSITION_CONFLICT`）、実時間を残しませんでした（`P_RUN_LEASE`。`main()` があると 0 ms で `TimedOut`）。実時間、モデルトークン、金額、Program の操作数、Core のステップ数、動的コンパイル数、子の予算は「量」です。フォークが自分の取り分を取り、残りは元の実行に残ります。`max_concurrency` は「上限」です。フォークは元の上限以下の自分の上限を持ち、元の実行は自分の上限をそのまま保ちます。遷移ストアは、フォークがこの値を変えていないことを確認します。**動作の変更:** フォークが元の実行に残っている実時間または Core のステップ数をすべて取ろうとすると、`fork()` は何も変えずに `P_FORK_SOURCE_BUDGET` を送出します。一時停止した実行の `remaining_budget` をまるごと渡したフォークは、以前は受け付けられていました。今後は取り分だけを渡してください。この規則は Program ガイドと Python バインディングガイドに記載しています。
- **Program: モデルトークン予算が尽きた実行は、1つの形で終わります (#420)。** 応答が実行を `model_tokens` の上限より先へ進めた場合、実行は `failure` のない `BudgetExhausted` で終わりました。一方、予約が収まらず送信前に拒否された呼び出しは、`NG.BudgetExhausted` を持つ `Failed` で終わったため、呼び出し側は一方では `status` を、もう一方では `failure.code` を読む必要がありました。今は両方とも、`failure.code` が `NG.BudgetExhausted` の `BudgetExhausted` で終わります。このコードは `graph::classify_failure` が予算の枯渇に与えるコードで、MCP、gRPC、A2A、ACP のホストもこのコードで報告します。`failure.witness["model_tokens"]` には `granted`、`used`、`over_by` が入り、`over_by` は最後の応答が超えた量です（拒否された呼び出しでは 0 で、その failure は provider の証拠を保ちます）。`model_tokens` を与えられていないのにモデルを呼んだ実行にも、この記録が付きます。**動作の変更:** 応答が予算を超えさせた実行には `failure` が付くようになり、次の呼び出しが送信前に拒否された実行は `Failed` ではなく `BudgetExhausted` で終わります。
- **Program: `main()` があっても `remaining_budget.max_node_retries` は上限から実施済みのリトライを引いた値です (#419)。** JavaScript の `main()` を持つ Program は、リトライをまだしていないかのように、開始時の上限をそのまま報告していました（`usage.node_retries` が 1 のとき 5）。コマンドの残りを精算するときに、その試行のリトライを引いていなかったためです。`define()` だけの Program はすでに 4 を報告していました。今は両方とも、上限からその試行のリトライを引いた値を報告し、再開した試行はその残りから始まります。
- **Program: 再試行を終えた失敗は、`main()` と再試行の上限があっても `interruptBefore` ハンドラーで停止します (#418)。** JavaScript の `main()` と `max_node_retries` を持つ run で、ノードが再試行を使い切って `interruptBefore` ハンドラーへ経路付けられると、run は `Interrupted` で停止せず、`P_TRANSITION_CONFLICT` を伴う `Failed` で終わっていました。`callCore` は停止するまで予約を保持し、その予約を解放する遷移の検査は、残りの再試行上限が呼び出しの開始時の値と等しいことを求めていましたが、呼び出しが使った再試行の分だけ上限はすでに減っていました。検査は、その attempt が使った再試行の分だけ減った上限を受け入れるようになりました。それより高い値も低い値も受け入れず、上限のない run は上限のないままです。**動作の変更:** このような run は `Interrupted` で終わり、残りの上限は使った再試行の分だけ減り、再開できます。`main()` がない run、上限がない run、再試行がない run は影響を受けていませんでした。この検査はメモリ、SQLite、PostgreSQL の各ストアが共有しており、保存済みデータは変わりません。
- **Program: native 状態を含む provider の失敗を、native archive のないホストでも公開します (#426)。** ストリームが途中で切れた場合や、provider が応答の一部だけを返して失敗した場合、`native_history_archive` のないホストでは run の terminal 状態が公開されませんでした。メモリ上の `LocalProgramHost` もこれに当たります。stderr には `terminal publication exception ... Program provider evidence lacks durable native custody` が出力され、`run()` は失敗した結果を返しましたが、`reconnect()` は `P_RUN_LEASE_HELD` を報告しました。provider の native replay 状態は archive の認証された custody の下でしか保存できないため、失敗全体が拒否されていました。このようなホストは、証拠のうち可搬な部分だけを含めて失敗を公開します。メッセージ、usage、stop reason、error、raw event はそのままで、各メッセージの native seal だけを除き、witness の `provider_outcome_native_retained: false` がそれを示します。開始時に構成を拒否すると、provider をバインドしたメモリ上のホストがすべて使えなくなり、すべてのホストで証拠を捨てると、archive のあるホストが保持できる分まで失われるため、この代替動作は archive のない場合にだけ適用します。**動作の変更:** archive のないホストでは、結果の `failure.provider_outcome` は native 状態を除いたメッセージを持ち(元の outcome は `provider_cause` に残ります)、run を再接続して読み取れます。archive のあるホストはこれまでどおり native の証拠を保存し、archive への書き込みが失敗した場合は、引き続き失敗が公開されません。
- **A2A: 再起動後に復旧した Program task への `SubscribeToTask` は、run を最終 status まで追います (#413)。** メールボックスから Program task を復旧したサーバーには、このプロセスが開始していない run の channel がありません。そのためストリームは Task を送ってすぐに閉じ、最終 status がありませんでした。ストリームは、run に結果が出るか cancel が確定するまで永続 run を追い、そのあとサーバーが開始した run と同じように結果 artifact と最終 status を送ります(停止した run は `TASK_STATE_INPUT_REQUIRED` で終わります)。呼び出しの前にすでに終わっていた復旧 run は先に読み出し、terminal として `-32004` で答えます。run を追えないときは、ストリームは空のまま閉じず、`-32603` のエラーイベント(ErrorInfo reason `RUN_UNAVAILABLE`)で終わり、このサーバーに run がない task なら呼び出しが `-32603` で答えます。**動作の変更:** 復旧した実行中の task の購読者は、run が終わるまで接続を開いたままにし、`A2AServer::stop()` は、サーバーが開始した run のストリームを待つのと同じように、この接続も待ちます。
- **Python ノードを持つエンジンが生きたまま、Python プロセスが正常に終了します (#429)。** `run_async` を使い、エンジンを参照したまま（たとえばモジュールのグローバル変数として）終わるスクリプトは、インタープリターの終了時にクラッシュすることがありました。負荷がかかると 10 回に 1 回ほど、SIGSEGV または SIGABRT になりました。`run_async` を支えるスレッドが最後の実行の Python オブジェクトを解放している最中に、メインスレッドがインタープリターをファイナライズしている間に GIL を取ろうとし、`PyThread_exit_thread` がそのスレッドを強制的に終了させました。強制的なスタック巻き戻しは `std::terminate` で終わるか、すでに解放されたスレッド状態に触れるデストラクターを実行しました。スクリプトの終了時にまだ Python コードを実行していた実行は、毎回クラッシュしました。モジュールはインポート時に `atexit` フックを登録するようになりました。フックは、ファイナライズが始まる前に、実行より長く生きたノードの試行を待ち、`run_async` の受け付けをやめ、実行中のコールバックが終わるのを（GIL を手放して）待ってから、`run_async` のスレッドと、Python のノードやツールの本体を実行するプールを join します。その後に GIL を持たないスレッドが解放するもの、たとえばプロセス終了時に破棄される、放棄された実行のハンドラーは、インタープリターを呼ばずに捨てられます。**動作の変更:** フックの実行後（後続の `atexit` ハンドラーやファイナライザーから）は、`run_async`、`run_stream_async`、`resume_async` が `RuntimeError` を送出します。終了時にまだ待機していた実行は、プロセスをクラッシュさせる代わりに破棄されます。テストは再現スクリプトを一度に 8 つの子プロセスとして起動し、すべての終了ステータスが 0 であることを求めます。
- **サーバーが実行前に拒否した MCP・gRPC のリクエストは、`reconciliation_required` としてモデルに届かなくなりました (#423)。** 失敗した MCP 呼び出しは、`isError` の結果でも JSON-RPC エラーでも、効果を除外する実行ポリシーがツールにない限り、すべて `{"status":"reconciliation_required","retryable":false,"effect_uncertain":true}` としてモデルに届いていました。サーバーが何も実行する前にリクエストを拒否し、モデルは引数を直すだけでよい場合も同じでした。拒否されたリクエストは、ポリシーによらず、サーバーのメッセージを持ち `retryable` と `effect_uncertain` がどちらも false の失敗したツール結果になります。対象は、JSON-RPC エラー `-32700`、`-32600`、`-32601`、`-32602`、`_meta["io.neograph/failure"].code` が `NG.InvalidRequest` の `isError` 結果、そして `GrpcRemoteTool` のステータス `NOT_FOUND` と `INVALID_ARGUMENT` です。`MCPServer` は、ツールの入力スキーマに合わない引数に対する結果にこの目印を付けて送ります（以前は目印のない `isError` 結果でした）。ツールが開始した後に起きた可能性のある失敗（それ以外の `isError` 結果、`-32603`、サーバーエラー）は、ポリシーが効果を除外しない限り、従来どおり `reconciliation_required` です。**動作の変更:** 拒否されたリクエストのツールメッセージと、`MCPServer` のスキーマ検証結果の `_meta`。新規: `ToolRequestRejected` と `ToolRequestRefusal` インターフェース（`<neograph/tool_execution.h>`）。ツールは、自分の呼び出しは実行されなかったと分かるときに前者を送出するか、独自の基底クラスを持つ例外型が後者を実装します。ノード失敗の分類器は拒否を通常の失敗として扱い、`retryable()` が true のときだけ再試行します。`MCPTool` は、拒否された JSON-RPC エラーに対して引き続き `MCPError` を送出するので、それを捕捉する呼び出し元に影響はありません。[ツール失敗の契約](docs/reference-en.md#tool-failure-contract)を参照してください。
- **`make_tool_service()` と `make_checkpoint_service()` は、それぞれのヘッダーだけで使えます (#425)。** どちらも `std::unique_ptr<::grpc::Service>` を返しますが、`<neograph/grpc/tool_service.h>` と `<neograph/grpc/grpc_checkpoint.h>` は `grpc::Service` を前方宣言しているだけでした。そのため、呼び出し側が生成された `neograph.grpc.pb.h` も include しない限り、戻り値を破棄するコードはコンパイルできませんでした。両方のヘッダーが gRPC のサービス基底ヘッダー（`<grpcpp/impl/service_type.h>`）を include するようになりました。テストは、gRPC のほかのヘッダーを一切 include しない翻訳単位で、各ヘッダーをビルドします。
- **直列 fan-out の通知は、分岐が順に実行されたときだけ出力されます (#417)。** `[neograph] warning: fan-out ... executing serially` という通知は、ワーカープールのないエンジンで幅が 2 以上の fan-out を実行するたびに出ていました。I/O を待つコルーチンノードも対象でしたが、これらは 1 つのスレッド上で交互に進み、`worker_count` が 1 でも 3 つの分岐が同時に進行していました。エンジンは分岐ごとにノード本体が実行された時間帯を記録し、静的な fan-out でも `Send` でも、どの 2 つの分岐も重ならなかった場合にだけ、fan-out の終了後に通知を出力します。**動作の変更:** 通知は fan-out の前ではなく後に出て、分岐が重なる場合は出ません。同期的な分岐（`SyncGraphNode`、Python ノード）は従来どおりエンジンごとに 1 回出力し、`NEOGRAPH_SUPPRESS_FANOUT_WARNING=1` で止められる点も変わりません。
- **Program: フォークは、続ける呼び出しの provider 呼び出しを再生します (#405)。** 一時停止した run のフォークは、一時停止した呼び出しをフォークの run で再実行していました。呼び出しを run ごとに記録する SQLite provider ジャーナルは、その呼び出しが質問する前に行った provider 呼び出しを再送し、再課金していました。元の run を再開すれば、この呼び出しは再生されます。フォークのすべての試行がブローカーに元の run（`ProgramCoreProviderCallContext::replay_source`）を渡すようになり、ジャーナルは同じ呼び出しについての元の記録、つまりフォークより前に元の Core スレッドの同じスロットに記録された同じリクエストを引き継ぎます。結果はフォークの dispatch id で再エンコードされ、行は元の試行番号を保つため、その呼び出しは再送も再課金もされません。結果が分からない元の呼び出しは、フォークでも不確かなままです。ジャーナルを持たないプロセス内ブローカーは、ノードの再試行と同じく引き続き再送します。
- **Program: チェックポイントを公開する前に止まった typed plan は、ブロックされずに復旧します (#404)。** replay-safe ではなく、チェックポイントも公開していない plan に対して、`reconnect()` は `P_RUN_RECOVERY_BLOCKED` を送出していました。今は JavaScript の `callCore` の規則 (#402) に従います。plan の各呼び出しは、plan の順に、自分が開始した Core 実行をその最新チェックポイントから続けます。チェックポイントを 1 つも保存していない最初の呼び出しは、その操作を指す `pending_effect`（`core_node` は `"plan"`）とともに、run を `Interrupted` で返します。`Completed` で `reconcile()` すると結果をその操作の結果として記録し、plan はそこから続きます。`Unknown` は run を `AmbiguousEffect` のままにします。
- **Program: 止まった JavaScript の `callCore` は、自身の Core チェックポイントから続きます (#402)。** プロセスが `callCore` の中で止まった run を、`reconnect()` は、呼び出しの Core がチェックポイントを保存していても照合のために返していました。そのため、すでに完了していた呼び出しや、run がまだ公開していない質問で一時停止していた呼び出しも、手で照合する必要がありました。今は復旧が、保留中のコマンドの各呼び出しを、その呼び出し自身の Core スレッドの最新チェックポイントから、何も答えずに続けます。完了していた呼び出しは結果を再生し、一時停止していた呼び出しはもう一度質問し、join のメンバーも同じように続きます。チェックポイントを保存しなかった呼び出しは、痕跡を残さず開始していた可能性があるため、開始していないことが確かな場合（コマンドジャーナルが開始を記録しないタイムアウト付きの `ng.await`）を除き、引き続き照合のために返されます。最新のチェックポイントが `interrupt_before` や `interrupt_after` の一時停止である呼び出しも同様で、復旧が答えのないゲートを通過することはありません。再開した試行は、操作をバインドするとき、再開前にジャーナルに記録した呼び出しのトークンも金額と同じように請求されます。そのため、チェックポイントから再生される呼び出しは一度だけ請求されます。
- **Program: JavaScript コマンドを Completed で照合すると、`main()` が続きます (#403)。** コマンドは run の残りの wall time と Core ステップをすべて予約しますが、照合されたコマンドがそれを持ったままだったため、`main()` は照合の直後に `TimedOut`（"Program wall-time budget (0 ms) expired"）で終わっていました。run が前のコマンドのチェックポイントを持っているときは、照合のための一時停止が、照合が精算するはずの予約まで解放していました。今はコマンドを照合すると、その wall time と Core ステップが run に戻り、一時停止は照合まで予約を保持します。コマンドが予約したトークンと金額は、何を使ったか分からないため、消費されたままです。
- **ACP が Windows のワークスペースパスを受け付けます。** `session/new` と `session/resume` は、`/` で始まらない `cwd` と `additionalDirectories` の項目をすべて拒否していたため、Windows の ACP クライアントはセッションを開けませんでした。今は POSIX パスと同じく、ドライブ付きのパス（`C:\work`、`C:/work`）と UNC 共有（`\\host\share`）も絶対パスとして扱います。相対パスとドライブ相対パス（`C:work`）は、引き続き `-32602` です。
- **Program: 予算の次元を空けた task graph task も、実際に受け取る量で集計されます（#412）。** `TaskGraphProposal` の task が `budget` のある次元を 0 のままにすると、proposal と fragment の上限はその次元を 0 と数えましたが、子はその次元について link の上限全体を受け取って起動し、空の `output_bytes` はどんな出力も許していました。そのため planner は、検査された集計上限を超えて使う expansion を作れました。proposal スキーマ 2 がこの隙間を塞ぎます。task はテンプレートの上限が 0 でないすべての次元に 0 でない値を明示する必要があり（`P_PROPOSAL_BUDGET_ZERO`）、スキーマ 2 の fragment は各 task を予約した値どおりに起動します。0 の次元は `RunBudget` と同じく何も許さず、出力は明示したバイト数に制限されます。スキーマ 1 の proposal と fragment は canonical バイト列、identity、起動時の動作が変わらないため、保存済みデータと既存のホストには影響しません。planner の proposal を受け取るホストは `TaskGraphProposalLimits::min_schema_version` を 2 にしてスキーマ 1 を拒否します。拒否された expansion の proposal は、最初のいくつかの diagnostic のコードとポインターを `failure.witness.diagnostics` で報告するようになりました。
- **Program: フォークは続ける呼び出しの provider 呼び出しを再生します (#405)。** 一時停止した run のフォークは一時停止した呼び出しをフォークの run で再実行し、呼び出しを run ごとに記録する SQLite provider ジャーナルは、その呼び出しが質問する前に行った provider 呼び出しを再送・再課金していました。元の run の再開はそれを再生します。フォークのすべての試行がブローカーに元の run（`ProgramCoreProviderCallContext::replay_source`）を渡すようになり、ジャーナルは同じ呼び出しについての元の記録、つまりフォーク前に元の Core スレッドの同じスロットに記録された同じリクエストを引き継ぎます。結果はフォークの dispatch id で再エンコードされ、行は元の試行番号を保つため、その呼び出しは再送も再課金もされません。結果が分からない元の呼び出しはフォークでも不確かなままです。ジャーナルを持たないプロセス内ブローカーは、ノードの再試行と同じく引き続き再送します。
- **Program: チェックポイントを公開する前に止まった typed plan は、ブロックされずに復旧されます (#404)。** replay-safe ではなくチェックポイントを公開していない plan に対して、`reconnect()` は `P_RUN_RECOVERY_BLOCKED` を送出していました。JavaScript の `callCore` の規則 (#402) に従うようになりました。各 plan 呼び出しは plan の順に、自分が開始した Core 実行をその実行の最新チェックポイントから続け、何も保存していない最初の呼び出しは、その操作を指す `pending_effect`（`core_node` は `"plan"`）とともに run を `Interrupted` で返します。`Completed` で `reconcile()` すると結果をその操作の結果として記録し、plan はそこから続きます。`Unknown` は run を `AmbiguousEffect` のままにします。
- **Program: 止まった JavaScript の `callCore` は自身の Core チェックポイントから続きます (#402)。** プロセスが `callCore` の中で止まった run を、`reconnect()` は呼び出しの Core がチェックポイントを保存していても照合のために返していたため、完了していた呼び出しや、run がまだ公開していない質問で一時停止していた呼び出しも手で照合する必要がありました。復旧は、保留中のコマンドの各呼び出しを、その呼び出し自身の Core スレッドの最新チェックポイントから何も答えずに続けるようになりました。完了していた呼び出しは結果を再生し、一時停止していた呼び出しはもう一度質問し、join のメンバーも同じように続きます。チェックポイントを保存しなかった呼び出しは痕跡を残さず開始していた可能性があるため、開始していないことが確かな場合（コマンドジャーナルが開始を記録していないタイムアウト付きの `ng.await`）を除き、引き続き照合のために返されます。最新のチェックポイントが `interrupt_before` や `interrupt_after` の一時停止である呼び出しも同様で、復旧が答えのないゲートを通過することはありません。再開した試行は、操作をバインドするときに、再開前にジャーナルに記録した呼び出しのトークンも金額と同じく請求されるようになったため、チェックポイントから再生される呼び出しは一度だけ請求されます。
- **Program: JavaScript コマンドを Completed で照合すると `main()` が続きます (#403)。** コマンドは run の残りの wall time と Core ステップをすべて予約し、照合されたコマンドがそれを保持したままだったため、`main()` は照合の直後に `TimedOut`（"Program wall-time budget (0 ms) expired"）で終わっていました。run が前のコマンドのチェックポイントを持っているときは、照合のための一時停止が、照合が精算するはずの予約まで解放していました。コマンドを照合すると、その wall time と Core ステップが run に戻り、一時停止は照合まで予約を保持するようになりました。コマンドが予約したトークンと金額は、何を使ったか分からないため消費されたままです。
- **A2A のタスクストリームは、実行が静かな間 SSE コメントを送ります。** `SendStreamingMessage`、`message/stream`、`SubscribeToTask`、`tasks/resubscribe` のストリームは、Task と次のステータスの間に何も送りませんでした。そのため、1 つのノードにクライアントの読み取りタイムアウト（httplib と httpx の既定は 5 秒）より長く留まる実行は、まだ作業中なのに切断され、購読者は最終ステータスの代わりに「応答なし」を受け取りました。5 秒間静かだったストリームは、SSE パーサーが無視するコメント `: keep-alive` を書くようになりました。**動作の変更:** ストリームの `data:` イベントの間にコメント行が入ることがあります。SSE パーサーを使わず生の行を読むクライアントは、それを読み飛ばす必要があります。実行中のタスクに購読者が参加する経路に欠陥はありませんでした。Task、カーソル、チャネルは 1 つのロックの中で取得され、実行の最終ステータス、チャネルのクローズ、実行の除去も同じロックの中で行われます。
- **gRPC のチェックポイントの失敗が「チェックポイントなし」にならなくなりました。** `CheckpointService` はロード時のバックエンド障害に「見つからない」で、一覧には空のリストで応答していたため、再開した実行が黙って最初からやり直し、`GrpcCheckpointStore::delete_thread` は失敗した削除を成功として報告していました。今はバックエンドの失敗はクライアントが送出する `UNAVAILABLE`、失敗した削除は例外を送出し、形式の不正な `SaveCheckpoint` は `INVALID_ARGUMENT` です。`GrpcCheckpointStore` はすべての RPC に(`set_rpc_timeout`、既定 30 秒)、`GrpcRemoteTool` はすべての呼び出しに(`set_call_timeout`、既定 60 秒)期限を設けます。
- **NeoGraph の MCP クライアントは待つのをやめるとサーバーに伝えます。** クライアントが放棄したリクエストには、キャンセルでもタイムアウトでも、続けて `notifications/cancelled` が送られる(`initialize` を除く)ので、サーバーは処理を止められます。以前はサーバーが放棄されたツールを最後まで実行していました。
- **実行がキャンセルされると実行中の MCP 呼び出しが止まります。** `MCPTool` はディスパッチャーの cancel token と期限を無視していたため、実行のキャンセルや試行タイムアウトでもサーバーの応答を待っていました(その結果、試行が停止の期限を越えて `NG.EffectUncertain` で失敗していました)。今は stdio と HTTP の両方でどちらも守ります。キャンセルは `CancelledException`(`cancellation_requested`)、期限またはリクエストタイムアウトは `timed_out` の呼び出しで、遅れた応答は破棄されます。`grpc::GrpcRemoteTool` も同じ規則に従います。実行の cancel token がその RPC をキャンセルし、実行の期限が RPC の期限になります。
- **不正な形式の UTF-8 がテキストを空にしなくなりました。** `json::dump` は不正な形式の文字列を含むドキュメントに空文字列を返していたため、失敗したツールメッセージ、MCP の応答行、gRPC のペイロードが空で届き、Python は `UnicodeDecodeError` を送出していました。今はすべての境界で不正な形式の最大部分列ごとに U+FFFD になり、正しいドキュメントのバイトは変わりません。
- **ツールコードからのシステムエラーはツールの失敗です。** 実行中のツールが送出した `std::system_error`(存在しないファイル、拒否された接続)は受け入れのステータスである `rejected` として報告されていましたが、今は `failed` または `reconciliation_required` です。ホストの受け入れによる拒否は、`failed` ではなくリソースアービターと同じく `rejected` または `expired` になりました。
- **`HookBoundaryBlocked` を送出した Python のノードはリトライされません。** C++ のノードや Python のツールの Hook のブロックは実行を止めていましたが、Python のノードの Hook のブロックはリトライされルーティングされる通常の失敗でした。今はリトライなしで実行を止めます。
- **長い、または UTF-8 でない Hook の失敗も記録されます。** 4096 バイトを超える、または UTF-8 のシーケンスの途中で切れた Hook ターゲットの失敗メッセージはレシートを無効にし、無関係なエラーで Hook 境界を失敗させていました。今はレシートのテキストがコードポイントの境界で切られ、修復されます。
- **Python の `mcp.MCPError` が `code` と `data` を保持します。** 以前はメッセージだけが Python に届いていましたが、今は JSON-RPC エラーの `code` と `data` が属性として入ります。
- **A2A: 例外テキストが peer に届かなくなりました。** 失敗した run の status message に `what()`(例外メッセージ内のパスや credential など)が入っていました。現在 peer が受け取るのは失敗の public message と `metadata["neograph/failure"] = {code, retryable}` だけで、失敗した Program run も同様です。node が 1 つも完了する前の hook によるブロックは `REJECTED`、その後は `FAILED` になります。`retryable` は MCP、gRPC、ACP と同じく `graph::classify_failure` から来ており、provider の失敗(`NG.ProviderFailed`)でも `true` になりました。node retry loop はこれを provider runtime に任せますが、新しいリクエストは通る可能性があるためです。
- **A2A: interrupt が必要なものを伝え、後続の message で再開します。** `INPUT_REQUIRED` は interrupt の reason を text として、payload を data part として運び、task への message は graph を最初からやり直さずに checkpoint から run を再開します。
- **A2A: リクエストと card が仕様に従います。** 不正な params は field violation 付きの `-32602`、不正な envelope と batch は `-32600` になり、すべての error に `google.rpc.ErrorInfo` detail が付きます。skill は常に tag を持ち、1.0 card には 0.3 の field がなく、card は `ETag`、`Cache-Control`、`If-None-Match` に応えます。0.3 の stream は task で始まり `final: true` で終わります。
- **Python の example 28 と `test_protocol_sdk_e2e.py` が agent-client-protocol 0.12 と 1.0 で動作します。** 1.0 はこれらが import していた `text_block`/`image_block`/`update_*` helper を削除しました。
- **開始されなかった仲介ツール呼び出しがスロットを解放します。** `SQLiteToolEffectBroker` は、開始されずに終わった呼び出し(`expired`、`rejected`、`cancelled_before_start`)を `reconciliation_required` として記録し、スロットを保留のまま残していたため、実行は照合のために一時停止し、そのスロットの後の呼び出しもすべて同様でした。今はその呼び出しを元のステータスのまま、終端で効果が不確定でない結果として返し、スロットを解放します。ノードの試行のメモリ上の記録はすでにそのように扱っていました。
- **`output_limit_bytes` を超えるツール出力は、呼び出しに効果があったかもしれない場合は効果が不確定です。** 以前は常に `effect_uncertain` が false の `failed` でした。今は呼び出しの開始後のすべての失敗と同じ規則に従い、ポリシーが `ReadOnly`、`ExternalRead`、`Idempotent` のいずれでもなければ `reconciliation_required` で効果が不確定です。
- **タイムアウトした Hook ターゲットをツール実行コントローラーが再び開始しなくなりました。** Hook ランナーはすでにタイムアウトしたネイティブのターゲットを最終状態として扱っていましたが、ターゲットを実行するコントローラーは、ターゲットのポリシーが再試行を許すとき(冪等または読み取り専用のターゲットで `retry_max_attempts` が 1 より大きいとき)その 1 回の試行の中でターゲットを再試行していたため、まだ実行中かもしれないターゲットがその横で再び開始されていました。タイムアウトした Hook ターゲットの試行は、今はコントローラーでも最終です。ツール呼び出しはポリシーの再試行をそのまま保ちます。
- **MCP ツールの引数と結果は JSON Schema 2020-12 で検証されます。** サーバーとクライアントがそれぞれキーワードの一部しか検査していなかったため、サーバーは引数が `minimum`、`anyOf`、`$ref` に反するツールを実行し、両方とも `1.0` を整数として受け入れませんでした。今は 1 つの検証器が、サーバー、クライアント、Harness のために core、applicator、unevaluated、validation の語彙とローカル参照を扱います。クライアントは検査できない dialect のスキーマでは、呼び出しを失敗させずに検査を省略します。
- **MCP のエラー応答に `"id": null` は入りません。** id を読み取れた不正な envelope にも、parse エラー、batch、すべての HTTP エラー本文にも null の id で応答しており、これは 2025-11-25 のスキーマが許しません。読み取れた id は返し、読み取れないエラーには id を付けません。
- **NeoGraph の MCP クライアントが公式サーバーのイベントストリームを読みます。** Server-Sent Events を `"\n\n"` でしか区切らなかったため、公式 Python サーバーの CRLF のストリームでは、通知の後のすべての応答(ログや進捗を送るツール)を失っていました。今は WHATWG の規則に従い、応答が届いたら読み取りをやめます。サーバーが応答の前に閉じたストリームは、`retry` 間隔の後に `GET` と `Last-Event-ID` で再開します。
- **NeoGraph の MCP クライアントがサーバーのリクエストに応答します。** stdio でも HTTP の呼び出しのイベントストリームでも、サーバーの `ping` には空の結果を、それ以外のリクエストには `-32601` を返します。以前は応答しなかったため、クライアントに ping を送るサーバーが失敗していました。保留中の呼び出しの id を再利用したサーバーリクエストが、その呼び出しを完了させることはなくなりました。
- **NeoGraph の MCP クライアントが期限切れのセッションを回復し、自分のセッションを終えます。** `Mcp-Session-Id` を付けたリクエストへの HTTP 404 は新しいセッションを始め、リクエストを 1 回送り直します。以前は終わったセッションの id を送り続けていました。`shutdown()` と最後の参照の解放は `DELETE` を送り、URL コンストラクタ `MCPTool(url, ...)` は、呼び出しごとにセッションを開いて終えずにサーバーのセッション上限を使い切る代わりに、すべての呼び出しで 1 つのセッションを保ちます。
- **MCP の HTTP エラー本文の JSON-RPC エラーがコードを保ちます。** HTTP のエラーステータスと JSON-RPC エラーの本文でリクエストを拒否したサーバー(MCP `2026-07-28` だけを話すサーバーの `-32022` など)は、本文をテキストとして持つトランスポートエラーを起こしていました。今はコード、data、`http_status()` を持つ `MCPError` です。
- **キャンセルされたグラフ実行が MCP サーバーに知らせます。** 実行のキャンセルトークンが MCP 呼び出しを待つコルーチンをキャンセルしたとき、トランスポートは中断された待機を shutdown や接続の失敗として報告し、`notifications/cancelled` を送っていませんでした。今はキャンセルとして扱い、サーバーに知らせます。
- **MCP HTTP: キャンセルが待たされず、応答のタイムアウトがツールをキャンセルし、`DELETE` が接続を閉じます。** cpp-httplib のプールは、`notifications/cancelled` を、それが止めるべき呼び出しの後ろで待たせることがありました。今は HTTP スレッドが必要に応じて `max_http_threads` まで起動します。`response_timeout` を超えたリクエストは 504 を返すだけでなく、ツールの呼び出しもキャンセルします。公式 Python クライアントのように `Content-Type` があって本文のない `DELETE` は、同じ接続の次のリクエストを失わせていました。
- **構造化された MCP ツールの結果は JSON をテキストでも運びます。** Harness のツールとサンプル 59 は、構造化 content を読めないクライアントのために MCP が求めるテキストブロックの直列化 JSON なしに `structuredContent` を返していました。
- **Program: 再開された試行のクラッシュ復旧が再開値で続くようになりました (#377)。** 復旧は `resume()`、`reconcile()`、`fork()` が開始した試行を、その試行がチェックポイントの再開に使った値ではなく実行の入力で再実行していました。`define()` Program のノードは実行の入力を再開値として受け取り、`main()` Program の中断された `callCore` はコマンドの入力で再開され、プロバイダーがバインドされている場合は SQLite ジャーナルが "logical call binding conflict" で失敗していました（fail closed）。実行レコードはその値 `ProgramRunRecord::attempt_resume()` を試行の開始前に永続的に保持するようになり、復旧はその値で再開します。この値を持つレコードは実行レコードの保存スキーマ 5 を使い、それ以外のレコードはスキーマ、バイト列、id を保ち、保存済みのレコードは変更なく読み込まれます。`ProgramRunRecordData` にメンバーが追加されたため、C++ の利用側を再ビルドしてください（[`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照）。フォークされた `main()` Program の最初の試行はまだ対象外です。
- **Program: 再生されたプロバイダー呼び出しのトークンは実行ごとに一度だけ課金されます (#378)。** SQLite プロバイダージャーナルを使う中断と再開で、再開された試行は以前の試行がジャーナルに記録し精算した呼び出しのモデルトークンを再び課金していました。新しいリクエストがないのに残りの `model_tokens` が 999,976 から 999,952 に減っていました。ジャーナルは引き続きそれらの呼び出しを認識するため、再送も再計上もしませんが、試行にはその試行自身が記録した呼び出しだけを課金します。#372 の金額会計はすでにそうしていました。クラッシュ前に記録された呼び出しは復旧後も一度課金され、ノードのリトライによる新しい呼び出しは課金されます。
- **Program: join は自身を止めた失敗を報告します (#379)。** fail-fast の `ng.all` は宣言順で最初に失敗したメンバーを報告していたため、先に宣言され、別のメンバーの失敗によってキャンセルされた兄弟がその失敗に取って代わっていました。`P_MONEY_BUDGET` で拒否されたメンバーが操作のキャンセルとして報告されていました。join は自身を止めた失敗のメンバーを報告するようになり、`collect` では引き続き宣言順で最初の失敗を報告します。プランレベルの `parallel` 操作にも同じ欠陥があり、キャンセルされたブランチが自身をキャンセルした失敗に取って代わっていました。`ng.race` と `ng.quorum` はすでに正しく動作しており、テストで両方を固定しました。
- **Program: join メンバーの割り込みは join を一時停止し、再開は終了したメンバーを再生します (#380)。** メンバーの Core が割り込んだ `ng.all` は正しく再開されませんでした。実行は最後にチェックポイントを書いたメンバーのチェックポイントを保持していたため、SQLite ホストはすでに完了したメンバーを再開し、join は再び `Interrupted` になりました。インメモリホストは完了したメンバーを再実行し、そのモデル呼び出しを二度送信して二度課金しました。兄弟が割り込んだときにまだ実行中だったメンバーはキャンセルされ、放棄した呼び出しの予約はそのまま残りました。メンバーの割り込みはその join を一時停止するようになりました。join はそれ以上メンバーを開始せず、すでに実行中のメンバーは終了させ、それらが得たすべての結果を保持します。その結果が join 自身の規則で join を終わらせる場合（fail-fast の失敗、race の勝者、達成されたか到達不能になった quorum）、join はそのように終了するため、`ng.race` と `ng.quorum` は可能なら一時停止したメンバーなしで終了します。そうでなければ join は宣言順で最初に一時停止したメンバーで `Interrupted` になり、その出力はここまでの結果を並べ、コマンドジャーナルは終了したメンバーを記録します。`resume()` は一時停止したメンバーを再開値で続行し、記録されたメンバーは再実行も再課金もせずに再生します。それ以外のメンバーは再実行されます。インメモリホストと SQLite ホストは同じように動作し、ネストした join も同様です。終了したメンバーを記録するコマンドジャーナルのエントリーはエントリーの保存スキーマ 2 を使い、それ以外のエントリーはスキーマ、バイト列、id を保ちます。`ProgramJavaScriptCommandJournalEntryData` にメンバーが追加されたため、C++ の利用側を再ビルドしてください（[`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照）。
- **Program: 期限切れの execution lease が終えた実行には、ジャーナルに記録された呼び出しのトークンが課金されます (#381)。** 停止したプロセスのルート execution lease が期限切れになり `reconnect()` が実行を `TimedOut` で終えたとき、その試行がジャーナルに記録したプロバイダー呼び出しには金額（#372）だけが課金され、モデルトークンは課金されていませんでした。`usage.model_tokens` は 0 で、`remaining_budget.model_tokens` にはそのトークンが残っていました。両方が一度だけ課金されるようになりました。`ProgramCoreProviderCallContext` に `journaled_usage` が追加されました。このような実行のためにバインドされたとき、ジャーナルを持つブローカーが課金するトークン勘定です。C++ の利用側を再ビルドしてください。
- **Core: 再開は、それが答える割り込みのゲートだけを飛ばします (#382)。** どの再開でも、再開後の最初の super-step は `interrupt_before` のチェックを飛ばしていました。あるステップのコミットと次のステップの割り込み保存のあいだでプロセスが停止した後の復旧は、そのステップのチェックポイントから再開し、ゲートのあるノードが承認なしに実行されました。`interrupt_after` の後の再開や `update_state(..., as_node)` の後の再開も、次のゲート付きノードを尋ねずに実行し、`interruptAfter` の後に `interruptBefore` が続く Program はこの経路に達しました。現在は、`interrupt_before` の一時停止を再開したときだけ止めていたノードが実行され、`NodeInterrupt` を再開するとそのステップが続きます。それ以外の再開は、これから実行する `interrupt_before` ノードの前で引き続き停止します。`ResumeContext` に `current_node` が追加されたため、C++ の利用側を再ビルドしてください。
- **Core: すべてのチェックポイント保存経路が同じ実行メタデータを書きます (#383)。** ノード実行器が `NodeInterrupt` のために保存するチェックポイントは、エンジン自身の保存経路が加える `barrier_stalled` と `failure_exit` なしでメタデータを作っていました。現在はすべての経路が一つのヘルパーでメタデータを作り、このヘルパーは `max_visits` のカウンターのために、チェックポイントがステップの途中で保存されるかステップの間で保存されるかを受け取ります。現在のスケジューラーでは、実行が停滞するか failure exit に達した後に `NodeInterrupt` は起こり得ないため、まだ異なるチェックポイントはありませんでした。テストは、同じ実行状態についてすべての経路のチェックポイントを比較します。
- **Program: 中断したブランチは plan レベルの `parallel` を一時停止させます (#384)。** 両方のブランチが中断した plan レベルの `parallel`（`ng.all` ではなく型付きの operation plan）は、最初のブランチだけを再開していました。二つ目のブランチは再開値を入力として再実行されるか、再開のたびに尋ね直し、すでに終わったブランチは再実行され再び課金されました。現在の `parallel` は #380 の join/interrupt 契約に従います。失敗したブランチは引き続きそれを終わらせます。そうでなければ、宣言順で最初に停止したブランチで `Interrupted` になり、実行はそのブランチのチェックポイントから再開します。再開は保留中の質問が指す Core 呼び出しを続け、終わったブランチは実行も課金もせずに再生し、それ以外の停止したブランチは停止したまま再生して自身の質問を改めて尋ねさせます。記録は実行レコードの `ProgramRunRecord::settled_branches()` として、`Interrupted` レコードとそれを再開する試行の `Running` レコードに保持されます。これを持つレコードは実行レコードのストレージスキーマ 6 を使い、ほかのレコードはスキーマと id をそのまま保ちます。`ProgramRunRecordData` に `settled_branches` が追加されたため、C++ の利用側を再ビルドしてください。
- **soundness チェック: 訪問回数は failure の対象が完了したときだけリセットされます (#364)。** オプトインの soundness チェックは、ノードの failure edge の対象が実行されただけで、その対象自身が失敗して自分の failure edge をたどった場合でも、ノードの `max_visits` の回数をリセットしていました。ランタイムは対象が完了したときだけリセットします。現在はチェックも同じに従い、ノードが上限にとどまる状態も探索します。
- **Core: 中断する `Send` target は run を一時停止します (#385)。** `Send` の target が送出した `NodeInterrupt` は例外として run の外へ抜け、チェックポイントを残さなかったため、再開するものがありませんでした。map ステップが尋ねるノードへ Send を送った Program は `P_RUNTIME_CORE_FAILURE` で失敗しました。現在の run は、ready batch のノードと同じく、同期・非同期・ストリーミングのすべての入口と Python で一時停止します。`NodeInterrupt` チェックポイントは開始時の状態から super-step 全体に再突入し、一時停止した結果もその状態を報告します。再開は Send を送出したノードと終了したすべての Send を再生するため、これらは一度だけ実行され一度だけ課金されます。尋ねた Send は同じ task id で回答とともに実行されるため、それがジャーナルに記録した provider 呼び出しは再生されます。入口の map ノードのように前にチェックポイントのないステップでは、終了した作業を interrupt チェックポイントの下に記録し、そのようなステップで終了した ready batch のノードも再実行されず再生されるようになりました。
- **Program: plan の再開は plan が止まった所から続きます (#386)。** 一時停止した plan レベルの Program（`main()` ではなく型付きの operation plan）を再開すると、再開値を状態として root から plan を再実行していました。一時停止した呼び出しより前の operation が再実行され再び課金され（トークン、金額、Core ステップ、Program operation）、`branch` は再開値で条件を評価しました。現在、一時停止した run レコードは、終了した `parallel` ブランチの結果と他の一時停止した呼び出しの質問に加えて、run が実行中だったすべての operation の位置（`sequence` の次の子、`loop` の反復、`retry` の試行、`map` の項目、`race` の勝者とその位置の状態、root には plan の入力）を operation id ごとに保持します。resume、reconcile、fork、graph migration とそれぞれの復旧がこれらのレコードを引き継ぎます。終了したものは再実行も再課金もされず、`branch` は最初に見た状態を見て、回答された呼び出しだけが再開値を受け取ります。fork は記録された一時停止中の各呼び出しにチェックポイントのコピーを個別に与え、`reconcile` が曖昧なまま残した effect もレコードを保持します。`parallel` と `race` のブランチ 0 はその operation 自身の Core スレッドで続くようになったため、ネストした `parallel` の最初のブランチが root の最初のブランチと並んで root のスレッドで実行されることはなくなりました。ネストした parallel のないプログラムはスレッド id を保ち、再開が回答する呼び出しは自分のチェックポイントのスレッドで続くため、この変更前に一時停止した run も再開できます。レコードは `ProgramRunRecord::settled_branches()`（#384）を拡張し、一時停止した呼び出しのレコードは呼び出しの operation id をキーにするようになりました。`sequence`、`loop`、`retry`、`map`、`race`、`branch` の中で一時停止した plan は run レコードスキーマ 6 を保存し、`P_PLAN_PARALLEL_RECORD` は `P_PLAN_REPLAY_RECORD` になりました。
- **Core: `NodeInterrupt` で一時停止した run は自身が保存したチェックポイントを報告します (#386)。** 以前はストア内のスレッドの最新チェックポイントを報告していました。SQLite と PostgreSQL はスレッドのチェックポイントをミリ秒単位のタイムスタンプ、次に step の順に並べ、スレッド上の run はそれぞれ step 0 からやり直します。そのため、同じスレッドの以前の run が同じミリ秒に最後のチェックポイントを保存していると、そのチェックポイントが最新でした。報告されたチェックポイントを再開すると、一時停止したノードを実行せずに以前の run の最終状態を返しました。SQLite の書き込みがすぐに戻る macOS では、再開した Program plan の呼び出しが Core ステップを一つも実行せず、前の呼び出しの出力を返しました。現在、ノード、並列ステップ、または `Send` が `NodeInterrupt` で一時停止させた run の `RunResult::checkpoint_id` は、run がその一時停止のために保存したチェックポイントです。
- **Core: SQLite と PostgreSQL でスレッドの最新チェックポイントは最後に保存されたものです (#386、#396)。** `SqliteCheckpointStore` と `PostgresCheckpointStore` は、ミリ秒単位のタイムスタンプ、次に step でスレッドの最新チェックポイントを選び、`list()` と `get_state_history()` を並べていました。両方が同じ保存や step が戻る保存は、誤った順序になりました。スレッド上の次の run は step 0 からやり直し（Program plan の次の Core 呼び出し）、一つの step の `interrupt_before` ゲート二つはそれぞれその step のチェックポイントを保存し（#396）、SQLite の書き込みがすぐに戻る macOS では保存が同じミリ秒になります。そのため `resume()` は最後に保存されたものより前のチェックポイントから続け、すでに答えたゲートや割り込みにもう一度答えました。その結果、run が再び一時停止するか、ノードが別の質問への答えを受け取りました。現在は両ストアとも `InMemoryCheckpointStore` と同じくスレッドのチェックポイントを保存順に並べ、`load_latest()` は `list()` の最初の要素です。スレッドがすでに持つチェックポイント ID を保存すると、三つのストアすべてでそのチェックポイントを置き換えて最新にします。インメモリストアはそのような ID を二度列挙しなくなりました。各ストアはデータベースを開くときに `neograph_checkpoints` に NULL 可の `save_seq` 列を追加します。PostgreSQL はシーケンス `neograph_checkpoints_save_seq` で保存に番号を付けます。このシーケンスは列のデフォルトでもあるため、列を設定しない writer の行にも番号が付きます。SQLite はスレッドの最大の番号より一つ大きい番号を付け、writer が番号なしで残した行にはトリガーが番号を付けます。マイグレーションはどの行も書き換えず、繰り返しても影響はありません。マイグレーション前に保存された行は `save_seq` が NULL のまま (timestamp, step) の順序を保ち、その後に保存されたすべての行の後ろに並びます。インデックス `neograph_checkpoints_save_order` が `neograph_checkpoints_recent` に置き換わります。以前のリリースもマイグレーション済みのデータベースを開くことができ、そのリリースが保存した行にも番号が付きます。
- **SQLite: 同じ新しいデータベースを同時に開くストアがすべて開けます。** 別のストアやプロセスが同じ新しいデータベースファイルを同じ瞬間に開くと、ストアは busy timeout に関係なく、すぐに `database is locked` で失敗することがありました。二つの接続が一つのファイルを同時に WAL に切り替えると、両方が読み取りロックを持っており、待つとデッドロックしうるため、SQLite は書き込みロックを二番目に求めた接続に busy handler を呼ばずにすぐ `SQLITE_BUSY` を返します。macOS の二つのテストプロセスがこれに当たりました。現在は `SqliteCheckpointStore`、`SQLiteContextStore`、`SQLiteHookJournal`、`SQLiteProviderDispatchReceiptStore`、`SQLiteToolEffectBroker`、`SQLiteProgramProviderCallJournal`、`SqliteHarnessRecordStore` が busy timeout を使い切るまで切り替えを再試行し、切り替えの後に timeout を設定していたストアは先に設定します。`SqliteCheckpointStore` はスキーマとマイグレーションを一つの `BEGIN IMMEDIATE` トランザクションで実行し、そのトランザクションが timeout 前に `SQLITE_BUSY` で終わると再実行します。`GateIdentity` と `ResumeValueScope` のテストは各ケースにランダムな名前のディレクトリを排他的に作るため、同時に始まった二つのケースが一つのデータベースを共有したり削除したりすることはなくなりました。
- **ビルド: MSVC はすべてのターゲットを `/bigobj` でコンパイルします。** Windows AddressSanitizer ビルドで `src/program/runtime.cpp` が COFF オブジェクトのセクション上限を超えました（`C1128`）。現在はトップレベルの `CMakeLists.txt` が `/utf-8` とともに `/bigobj` をすべての MSVC ターゲットに渡すため、ライブラリ、テスト、Python ホイール、インストール済みパッケージのビルドのすべてに適用されます。`neograph_program_tests` だけにあったフラグは削除しました。
- **Core と Program: 一時停止した run を fork しても、終わった処理は再生されます (#390)。** `NodeInterrupt` チェックポイントに対する `GraphEngine::fork`、Program の fork、一時停止した呼び出しのチェックポイントの複製は、チェックポイントの pending write を捨てていました。そのため中断した super-step で終わっていたノードと `Send` の対象が、fork した run で効果や provider 呼び出しごと再実行されていました。fork は元の run を resume したときに再生される write を複製するようになり、fork した run も resume と同じように再生します。終わった処理が再実行、再送信、再課金されることはありません。Python のホストは `LocalProgramHost.fork(run_id, value, version, budget, ...)` で Program の run を fork できます。
- **Program: `ng.await` が resume と復旧のあとも期限を守ります (#391)。** `main()` 内の `ng.await(..., timeout_ms)` は resume のたびにタイムアウトを 0 から数え直していたため、interrupt と resume を繰り返すほど作者が書いた期限が延びていました。await の開始時刻を開始時に command journal に記録するようになり(`await_started_at` を持つ pending エントリ、command journal エントリ schema 3)、resume または復旧した attempt は残り時間だけ待ちます。プラン レベルの `await` は開始時刻を replay 記録 (#386) に残します。このフィールドを持たない journal エントリの schema とバイト列は変わりません。
- **Program: 割り込まれた branch がプランレベルの `quorum` を一時停止します (#387)。** プランレベルの `quorum`（`ng.quorum` ではなく型付き操作プラン）は、Core 呼び出しが割り込まれた branch を成功しなかったものと数えて次の branch に進んでいました。質問は捨てられ、quorum は後の branch で完了するか失敗していました。現在は #380 と #384 の join/割り込み契約に従います。一時停止した branch はまだ到達しうる成功として数えるため、完了した branch がすでに quorum を決めていない限り quorum を一時停止し、次の branch は開始しません。run レコードの `settled_branches` は自身で終わった各 branch の結果を保持し、再開はそれらを再実行も再課金もせずに再生し、一時停止した branch を再開値で続けてからその先へ進みます。後の branch が尋ねると quorum は再び一時停止します。残りの branch で `min_success` に届かなくなった quorum は、少ない成功で完了せず `P_QUORUM_UNREACHABLE` で失敗するようになり、quorum は branch の実行トレースを報告します。
- **Core: スーパーステップの各割り込みに個別に答えます (#388)。** 一つのスーパーステップの複数の ready ノードや `Send` target が `NodeInterrupt` を送出すると、一度の `resume(value)` が同じ値を全員に渡していたため、二つ目の質問は一つ目の答えで答えられ、一度も尋ねられませんでした。現在、一時停止した `RunResult` はステップのすべての割り込みを `interrupts`（`{id, node, value}`、id は一時停止した呼び出しの task id）に列挙し、`interrupt_node`、`interrupt_value`、`interrupt_id` で最初のものを報告します。再開は `RunMetadata::resume_interrupt_id`（Python では `resume`、`resume_async`、`resume_from` の `interrupt_id=`）が指定した割り込み、または最初のものに答え、ステップの他の呼び出しは値なしで実行されて自身の質問をもう一度行います。保留中でない id は、何かが実行される前に `std::invalid_argument`（`ValueError`）で拒否されます。複数の割り込みで一時停止したチェックポイントは `_neograph.interrupts` にそれらを列挙し、一つで一時停止したチェックポイントはバイトを保ちます。そのようなステップの Program の pending input や effect は `core_interrupt_id` で Core 割り込みを指し（pending 値の保存スキーマ 2、他の値はスキーマ 1 とバイト、id を保持）、run の再開はまさにその割り込みに答えます。JavaScript の join とプランレベルの `parallel`、`quorum` は、すでに再開ごとに一時停止したメンバー一つに答えていました。
- **Core: `Send` 一つでも複数でも一つの契約に従います (#389)。** `Send` が一つのステップは payload を共有 run 状態に適用してその上で target を実行し、複数のステップは各 target を隔離したコピーで実行していました。再生される単一 Send は payload をまったく適用しませんでした。現在はすべての Send が payload を適用した自身の状態コピーで、再試行付きで、実行時も再生時も実行され、返した write だけが共有状態に届きます。複数 Send の経路は再試行なしで実行されるとしていた `executor.h` のコメントを訂正しました。すべての Send の試行は自身の effect namespace で再試行します（#364）。
- **テスト: プランの一時停止テストが QuickJS なしでビルドされます。** `JoinTestStores` は `tests/test_program_runtime.cpp` の QuickJS 専用ブロックの中にありましたが、その外のプランレベルの `parallel` と再開のテスト（#384、#386）が使っていたため、`NEOGRAPH_BUILD_QUICKJS_CONTROL=OFF` ではファイルがコンパイルできませんでした。共有ストアはそのブロックの外に移し、JavaScript ソースを admit する三つのテストをガードしました。
- **Core: 再開値はそれが答える呼び出しにだけ届きます (#393)。** 再開後、`ctx.resume_value` が再開した run の後続のすべてのスーパーステップのすべてのノードに残っていたため、承認を尋ねる後続のノードや、ツールの実行前に尋ねる `ToolGate` が以前の答えを自分のものとして受け取り、尋ねませんでした。一つの承認が別の質問に答えていたのです。現在、値は再開が再び入るステップで、ちょうど一つの呼び出しに、そのすべての試行を含めて届きます。再開が答える `NodeInterrupt` を送出したノードや `Send` (#388)、または `interrupt_before` のゲートが止めていたノードです。その他の呼び出しと後続のステップのすべてのノードには値が見えないため、尋ねるノードやゲートは run をもう一度一時停止します。親を通じて再開したサブグラフは親の答えを受けたノードを通じて自身の答えを受け取り、Program の `call_core` と JavaScript の `callCore` の再開も同じ規則に従います。
- **Core: fan-out のステップと一つのステップの Send は一つの規則で終わります (#392)。** 兄弟が失敗と割り込みの両方を起こしたステップが一時停止するか失敗するかは、どちらのインデックスが小さいかで決まっていました。現在は fan-out の分岐と Send に、ワーカーが一つでも複数でも、インデックスや完了順にかかわらず一つの規則が適用されます。キャンセルは run を止めます。そうでなければ、不確かな効果 (`tool_effect_reconciliation` の割り込み) は失敗の横でも照合のためにステップを一時停止してそうした割り込みだけを報告し、失敗した兄弟は再開時にもう一度実行されます。そうでなければ、ディスパッチ順で最初の失敗が run を失敗させ、そのステップの通常の割り込みは報告されません。それ以外の場合、ステップはすべての割り込みで一時停止します (#388)。failure edge に沿ってルーティングされる失敗 (#364) はノードの結果であり、ステップを終わらせません。JavaScript の join とプランの `parallel`、`quorum` はすでにメンバーの失敗で終わっていました (#380, #384, #387)。
- **テスト: フォークは保留中の呼び出しが指す Core 割り込みに答えます (#394)。** 複数の割り込みを持つ Core ステップで一時停止した Program の実行のフォークが、最初の試行に `core_interrupt_id` を渡さないと報告されていました。実際には渡しています。フォークは id を保った元の保留中の呼び出しを最初の実行記録に公開し、試行は再開や照合と同じくそこからそれを読みます。テストがその記録と答えを、メモリと SQLite で、また Python で再送も再課金もされないバインド済み `llm_call` とともに固定するようになりました。
- **Core: 一つのステップの二つの `interrupt_before` ゲートが一度の再開で通過されなくなりました (#396)。** 同じスーパーステップで二つのゲート付きノードが ready になると、一度の再開が両方を実行し、二人目の人間の承認は尋ねられませんでした。現在、再開はゲート付きノードの task id (#388) で識別される、run が止まったゲートに答え、そのステップの他のすべてのゲート付きノードは何かが実行される前に自身の Before 割り込みで run をもう一度止めます。すべてのゲートが答えられるとステップが実行され、各ゲート付きノードは自身のゲートへの答えを受け取ります (#393)。
- **Program: join は Core と同じく照合の一時停止をメンバーの失敗より優先します (#397)。** JavaScript の `ng.all`、`ng.race`、`ng.quorum` とプランレベルの `parallel`、`quorum` は、別のメンバーが結果の分からない効果を照合するために一時停止していても、メンバーの失敗で終わり、その効果を放棄していました。現在は #392 の優先順位に従います。キャンセル、照合の一時停止、失敗、通常の割り込みの順です。失敗したメンバーは記録され、再開時に再実行されずに再生されます。プランレベルの `parallel` は Core ノードが失敗した分岐を記録し、その失敗がキャンセルした分岐をもう一度実行します。
- **Program: JavaScript の `main()` のフォークは一時停止した `callCore` を再開します (#395)。** フォークの最初の試行はコマンドジャーナルも Core の再開値も受け取らなかったため、`main()` は最初からやり直されていました。完了したコマンドが再実行されてそのプロバイダー呼び出しと効果が再送・再課金され、すべての質問がもう一度尋ねられ、答えは失われていました。現在、フォークは対象バンドルに結び直した元のコマンドジャーナルを持ち、一時停止した呼び出しのチェックポイントのコピーをその呼び出しの Core スレッドに保存し、元の入力で `main()` を実行し、再開と同じくフォークの値で一時停止した呼び出しを再開します。最初の試行は再開するコマンドの資源を予約し、復旧も同じように続けます。
- **Program: フォークは記録されたプロバイダーの失敗を正しいカストディで再生します (#398)。** プロバイダーの証拠を持つ失敗の横で一時停止した実行のフォークは、その失敗がコマンドジャーナルに記録された収集型の `ng.all` (#395) にあってもプランの `settled_branches` (#386) にあっても、その証拠を記録と一致しないフォークのカストディで復元していたため、カストディの不一致で失敗していました。現在、フォークとグラフ移行はそうした記録を持ち込むたびに元の実行のカストディから新しい実行のカストディへ結び直すため、再生は証拠 (`result.failure.provider_outcome`) を復元し、何も再送しません。プランの `parallel` は、完了した分岐のプロバイダーの証拠を quorum と同じように保存します。
- **Program: 分岐の Core ノードが失敗してもプランの quorum は実行を失敗させなくなりました (#399)。** ノードが例外を送出した分岐は quorum の外に投げられ、実行の失敗になっていました。現在その分岐は、実行が報告したはずのコード、ノード、試行回数、プロバイダーの証拠を持つ失敗した分岐であり、後続の分岐が quorum を一時停止させると記録され、再開時に再生されます。キャンセルと失われたチェックポイントは引き続き実行を終わらせ、#392/#397 の優先順位は保たれます。
- **Program: 再開された join は応答を受けるメンバーをキャンセルしなくなりました (#400)。** 失敗したメンバーの横で照合のために一時停止した (#397) fail-fast の `ng.all`、`ng.race`、`ng.quorum` は、再開時にそのメンバーをすぐに再生します。その完了が join を終わらせ、ノードがまだ応答を消費している照合メンバーをキャンセルしていたため、効果は確定しませんでした。現在は応答を受けたメンバーが先に最後まで実行され、その後に再生された失敗が join を終わらせます。
- **Program: 再開された join は再生するメンバーを先に確定します (#400)。** 再開時に JavaScript の join はメンバーを宣言順に開始していたため、一時停止前に記録された結果を再生するメンバーが、再開が応答するメンバーがすでに終了した後に登録されることがありました。その場合、結果はタイミングに依存しました。失敗したメンバーの横で照合のために一時停止した `ng.race` が、再生された失敗で終わらず、応答を受けたメンバーの値で完了することがありました。現在、再開された join は再生するメンバーを先に開始し、次に再開が応答するメンバー、次に残りを開始します。再生されたメンバーは実行されるどのメンバーよりも先に確定するため、race の勝者と fail-fast の join の失敗は毎回同じです。
- **Program: 一時停止した join で通常の質問が不確かな効果より優先されなくなりました (#401)。** 失敗のない join (収集型かどうかにかかわらず) やプランレベルの `parallel` は、宣言順で最初に一時停止したメンバーで一時停止していたため、不確かな効果で一時停止したメンバーより先に通常の質問を尋ねていました。現在は Core のステップ (#392) と同じく照合を先に尋ねます。すべてのメンバーを待つため失敗の横でも一時停止したメンバーで一時停止する収集型の `ng.all` の例外は保たれ、文書化され、テストで固定されました。
- **Program: ブランチがすぐに終わる plan の `parallel` や `race` が止まらなくなりました。** plan レベルの `parallel` や `race` は自身を実行したスケジューラスレッドでブランチを待ち、ブランチはスレッドセーフではない asio channel を通じて strand 上で完了を通知していました。別のスケジューラスレッドで待機を始めている最中に最後のブランチが通知すると、その通知が失われることがあり、run は終わりませんでした。Windows ではすぐに戻るブランチでこのように止まりました。現在、待機は他の join と同じくブランチが通知する strand 上で実行され、メンバーを登録している間に決着した JavaScript の join もその strand から通知を送ります。
- **Core: プロセスが途中で止まった再開をやり直せます。** 復旧するホストは、実行していた再開を同じ値と同じ割り込み id でやり直します。step が 1 つの割り込みで一時停止し、回答を受けたノードまたは `Send` がプロセスの停止前に終わっていた場合、やり直しは失敗していました。割り込みを指定すると `std::invalid_argument`（"is not pending"）になり、既定で回答すると、まだ実行されていない step の最初の `Send` が値を受け取っていましたが、その `Send` は何も尋ねていませんでした。そのようなチェックポイントの割り込みは、終わったかどうかにかかわらず一時停止したノードになり、終わった `Send` を指定するやり直しも受け付けます。終わったものは再生されます。再開モデルハーネスが見つけました。
- **Program: 回答を受けたメンバーが既に終わった、再開された join の復旧。** メンバーを 1 つずつ実行する JavaScript の join で、回答を受けたメンバーが終わり join がそれを記録した後、次のメンバーの一時停止が公開される前にプロセスが止まることがありました。すると `reconnect()` が run を `P_TRANSITION_CONFLICT`（"Recorded join members do not match the paused join"）で失敗させていました。復旧は記録されたメンバーを再生して join を続けるようになり、次のメンバーが自分の質問を尋ねます。再開モデルハーネスが見つけました。
- **Program: 後の Core 呼び出しの中で止まった JavaScript `main()` の復旧。** run は最後に公開した Core 呼び出しのチェックポイントを保持します。後の `callCore` が pending として記録された後、その中でプロセスが止まると、`reconnect()` は前の呼び出しのチェックポイントを pending の呼び出しのものとして扱い、run を `CheckpointIncompatible`（"Exact Core resume checkpoint does not match a pending JavaScript callCore coordinate"）で終わらせていました。最初の呼び出しの中で止まったときと同じく、pending の呼び出しの照合のために run を返すようになり、その呼び出しを再実行しません。再開モデルハーネスが見つけました。
- **ノードのリトライが以前の試行の効果と衝突しなくなりました (#364)。** 1 つの呼び出しスコープがリトライループ全体を覆っていたため、すべての試行が最初の試行の効果タスク id と呼び出しの序数を再利用していました。そのため `ToolEffectBroker` の下では、リトライが最初の試行のレシートを再生するか、引数が変わったとして拒否され、リトライ中のクラッシュは最初の試行のスロットで再開していました。
- **確認できない Program のプロバイダーディスパッチは不確定な結果になります (#364)。** SQLite のプロバイダージャーナルがディスパッチのマーカーをコミットした後にトランスポートが例外を送出すると、元の例外がそのまま再送出され、ノードのリトライがリクエストを再送できました。ジャーナルはこの場合と、未解決のスロットを後で再び呼び出した場合に、結果を持たない `ProviderOutcomeError` である `ProgramProviderDispatchUncertainError` を送出するようになりました。この失敗はリトライもルーティングもされません。
- **Program: `LocalProgramHost.resume(run_id, value)` が pending id なしで動作します (#359)。** 待機中の呼び出しが 1 つならそれを再開し、ない場合や複数ある場合は `result.interrupt.pending_input.call_id` を示す `ValueError` を送出します。明示した id は完全一致の契約を維持します。
- **Program: 失敗した run が完了済みノードの trace を保持します (#359)。** `execution_trace` は失敗前に終わったノードを列挙し、`failure.core_node` は例外を送出したノードを示します。失敗結果の identity もそれに応じて変わります。
- **Program: コンパイル時の JavaScript が run 予算の wall time を守ります (#360)。** 終了しない `define()` は 60 秒ではなく、コンパイラ上限と予算の `wall_time_ms` の小さい方で `P_JS_TIMEOUT` により停止します。Python では `ProgramCompiler` と `LocalProgramHost` の `javascript_limits=` に `ng.ProgramJavaScriptCompileLimits` を渡してコンパイル上限を設定します。
- **Program: 作成時の診断がソース位置を示します (#361)。** JavaScript ビルダーの診断は、コンパイルが返す診断（`ProgramBundle::compile_diagnostics()`、Python の `bundle.compile_diagnostics`）に呼び出し位置（pointer、行、列）を持ちます。bundle に封印される診断は変わらないため、bundle id は安定したままです。コンパイル失敗は `ng.ProgramCompileError`（`RuntimeError`）を送出し、メッセージは `source:line:col` で終わり、`.diagnostics` に詳細が入ります。レジストリ未登録のヒントは Python の登録呼び出し名を示します。
- **Python: `ProgramRegistryBuilder` が未登録の実行単位を 1 つのエラー型で拒否します (#370)。** `add_registered_node` は、`add_registered_reducer` と `add_registered_condition` と同様に、ノードタイプが登録済みかを確認するようになり、見落としを admission まで持ち越しません。3 つのメソッドはいずれも `ng.ProgramRegistryError` を送出します。その `kind` は `"node"`、`"reducer"`、`"condition"` のいずれか、`name` は見つからなかった名前で、メッセージは Python の登録呼び出しを示します。`ValueError` と `RuntimeError` の両方を継承します。リデューサーと条件のメソッドは以前 `RuntimeError` を送出していました。レジストリスナップショットにない名前を使う Program は、引き続き `P_CORE_GC_REGISTRY_MISS` を持つ `ng.ProgramCompileError` でコンパイルに失敗します。
- **もう発火できない barrier で止まった run は `Completed` ではなくなりました (#357)。** 実行可能なノードがなく、最後のスーパーステップが `__end__` に明示的に到達せず、join がまだ signal を保持している場合（たとえば barrier メンバーが conditional route で join を迂回した場合）、`RunResult::status()` は新しい `RunStatus::BarrierStalled`（Python では `ng.RunStatus.BarrierStalled` と `RunResult.barrier_stalled`）を返し、`output["_neograph"]["barrier_stalled"]` は停止した各 join を `{node, wait_for, received, missing}` として列挙します。Program の run は `P_CORE_BARRIER_STALLED` で `Failed` となり、`failure.core_node` はその join を示し、witness は停止した join を列挙します。stall を抑止するのは、最後のスーパーステップでの `__end__` への明示的な route だけです。つまり `__end__` への static edge（`.exit()` が書き込むもの）、`__end__` への conditional route、`Command.goto("__end__")` です。後続のないノードや Send worker のような暗黙の終了は決して抑止せず、それより前のスーパーステップでの明示的な終了も同様です。run が保存するすべてのチェックポイントに stall が記録されるため、それを再開すると再び報告されます。`interruptAfter` チェックポイントは barrier で保留された次のノードを保存するため、再開しても最後のメンバーが到着する前に join を実行しなくなりました。Core が step 予算を使い切った JavaScript の `callCore` は、run を `P_CORE_STEP_BUDGET` で `BudgetExhausted` として終了させるようになりました。以前は完了として `main()` に戻っていました。**動作の変更:** これらの状況で `Completed` で終わっていた run は、`BarrierStalled`、Program では `Failed` で終わるようになりました。
- **予約済みのノード名と entry のないグラフはコンパイルできなくなりました (#358)。** `__start__` または `__end__` という名前のノードは、同名のルーティング sentinel に隠されていました。`__end__` への edge は run を終了させ、JavaScript ビルダーは `__end__` → `__end__` の self-loop を封印していました。ノードはあるが `__start__` からの edge がないグラフは何も実行せず、`Completed` を報告していました。グラフパーサーは、すべての schema バージョンで `GC_RESERVED_NODE` と `GC_NO_ENTRY`（Program では `P_CORE_GC_RESERVED_NODE` と `P_CORE_GC_NO_ENTRY`）を報告するようになりました。ノードのないグラフは引き続き受け入れます。JavaScript ビルダーは予約名を `.node()` 呼び出しの時点で `P_JS_GRAPH_ARGUMENT` により拒否し、`define()` は `.entry()` のないグラフを `P_JS_GRAPH_ENTRY` で拒否します。evolution の `remove_edge` mutation は、グラフの唯一の entry edge を残します。**動作の変更:** これらのグラフは以前はコンパイルできましたが、現在は `GraphEngine::compile` と Program のコンパイルが拒否します。
- **Program: C++ のノード本体と Core のサブグラフが Program scheduler を占有しなくなりました (#367, #368)。** Python のノード本体の修正では、ほかのすべてのノード本体が Program scheduler のスレッドに残っていました。既定の `scheduler_threads=1` では、スレッドを 800 ms 占有する C++ のノードのために、そのようなメンバー二つの `ng.all` に 1.6 秒かかり、`ng.race` はより速いメンバーではなくそのノードを返し、ノードがブロックする Core のサブグラフも一段下で同じことをしていました。いまでは Program の実行のすべてのノード本体がプロセス全体のワーカープール（`NEOGRAPH_NODE_ATTEMPT_THREADS` で 1〜1024、既定 32）で実行され、各試行は自分のキャンセルトークンを持つ一つのディスパッチ単位なので、scheduler はオーケストレーションだけを行います。キャンセルは引き続き協調的で、キャンセルされた実行も本体が止まるまで待ちます。通常の `GraphEngine` の実行では、タイムアウトのない本体は引き続き呼び出し元のスレッドで実行されます。
- **Program: Python ノード本体が Program scheduler を占有しなくなりました (#367, #368)。** Python ノード本体は実行中ずっと Program scheduler スレッド上で動いていたため、join メンバーが 1 つずつ順番に実行されていました。800 ms のメンバー 2 つの `ng.all` に 1.6 秒かかり、`ng.race` は先に宣言された遅いメンバーを返し、`ng.quorum` は遅いメンバーを待って結果に含め、既定の `scheduler_threads=1` では `ng.await` の deadline が発火できませんでした。Program の run では、Python ノード本体は binding が所有するワーカープール（32 スレッド。`NEOGRAPH_PYTHON_NODE_THREADS` で 1～1024 に設定）で実行されるようになったため、メンバーは並行に進み、タイマーも発火します。Python コードは引き続き GIL によって直列化されます。通常の `GraphEngine` の run は、引き続き呼び出しスレッドで Python 本体を実行します。`ng.race` と fail-fast の join は、先に宣言されたメンバーではなく最初に終わったメンバーで決着し、`ng.quorum` は quorum を満たした成功だけを正確に保持します。`ng.await` は自身の deadline を記録します。deadline 後に届いた完了は、タイマーがまだ発火していなくても timeout となり、取り消された子が送出したエラーよりも timeout が優先されます。キャンセルは引き続き協調的なため、await は子が停止してから戻ります。**動作の変更:** 遅れた完了は、run を `Completed` ではなく `P_AWAIT_TIMEOUT` で `TimedOut` として終了させるようになり、`ng.race` が別のメンバーを返すことがあります。
- **Program: provider のないモデル呼び出しノードがプロセスを中断させなくなりました (#366)。** provider なしで構築された `llm_call` または `intent_classifier` ノードは、最初の実行時に provider を参照外しし、プロセスを中断（SIGABRT）させていました。これは通常の `GraphEngine` でも、ノードを `ProgramRegistryBuilder.add_registered_node` で登録した Program でも同じでした。現在は両ノードともノード名を示す `std::invalid_argument` を throw し、executor はこれをリトライしないため、run は `NodeExecutionError` で失敗します。構造だけを扱うツールのために、`GraphEngine` はこうしたグラフを引き続きコンパイルします。Program の admission は `P_ADMIT_PROVIDER` でこれを拒否し、Python ではこれを `ng.ProgramAdmissionError` として送出します。#372 から Python は `LocalProgramHost(node_context=...)` で provider をバインドします。
- **Python: Program の admission エラーと実行時の診断エラーに型が付きました (#369)。** admission の失敗は素の `RuntimeError("Program admission failed with 1 diagnostic(s)")` を送出し、`P_START_BUDGET` などの実行時の診断エラーはコードと witness を失っていました。現在は、すべての binding のエントリポイントから `ng.ProgramAdmissionError` と `ng.ProgramDiagnosticError` を送出し、どちらも `ng.ProgramCompileError` と同様に `RuntimeError` のサブクラスです。メッセージは診断ごとに 1 行で、コード、JSON pointer、メッセージを含み、ソース位置がある場合は `source:line:col` も付きます。`.diagnostics` はコード、severity、phase、メッセージ、ソース位置、witness を保持します。予算エラーはリソースとその上限を示します。例: `Program invocation budget wall_time_ms=9000 is outside its admitted bounds [10000, 10000] (policy ceiling 10000)`。構造的な各 start-budget 規則、admission の policy-ceiling と structural-floor の検査、Catalog の `worker_count` 検査も同様です。
- **Program: Python ホスト向けのメッセージと trace (#370)。** `LocalProgramHost` が保持していない run を再開すると `P_RUN_NOT_FOUND` を持つ `ng.ProgramDiagnosticError` が送出され、メッセージは run がそれを開始したホストのメモリに保持されることを説明します。wall-time の timeout は `Program wall-time budget (N ms) expired` で失敗し、timeout が中断した処理のテキストは `failure.witness["cause_message"]` に移ります。`TimedOut`、`Cancelled`、診断による失敗は、ノード失敗が既にそうしていたように、完了済みノードの `execution_trace` を保持します。これらの結果の identity もそれに応じて変わります。JavaScript ソースの場合、コンパイルが返す E9 の fan-in ヒントは `graph.barrier("<node>", [...])` を示します。封印されるコピーは Core の文言を維持するため、bundle id は変わりません。**動作の変更:** timeout の `failure.message` は新しいテキストになりました。メッセージではなく `status` または `failure.code` を確認してください。
- **Program: 2 回目に interrupt された `callCore` がプロセスを終了させなくなりました (#371)。** 再開後または最初の `run()` の途中で 2 回目の `callCore` が interrupt された JavaScript の `main()` は、`std::terminate` で終わっていました。interrupt された予約の解放は attempt の累積使用量と照合されていましたが、この累積使用量には以前の command が既に精算した作業も含まれるため、store が publication を拒否し、fallback が不正な結果を組み立てていました。in-memory、SQLite、PostgreSQL の transition store は、この解放を `is_valid_program_journal_reservation_release` で検査するようになりました。使用量を持つ各リソースは、予約から attempt の使用量を引いた値から予約全体までの任意の量で戻ってきてかまいません。interrupt された各 `callCore` は、`resume()` が継続する `Interrupted` 結果を返すようになりました。再開できない結果は interrupt を持たなくなり、terminalization から漏れた例外はホストを終了させる代わりに、run を `P_RUNTIME_INTERNAL` で `Failed` として終了させます。
- **厳密なコンテキスト組み立てが、required artifact が複数ある短い history を受け付けます (#373)。** mandatory トークン数は各 required artifact のメッセージ projection JSON を合計し、入力トークン数はそれより小さい準備済み wire body を測っていました。history が短く required artifact が 2 つ以上あると mandatory 数が入力数を超え、`StrictRuntimeProfile.invoke`（および `RuntimeTurnAssembler::assemble`）が `Mandatory token count exceeds estimated input tokens` で失敗していました。両方の数は、準備済みリクエストが運ぶバイトを 3 バイトあたり 1 トークンで数えるようになりました。required artifact は自身のテキストを数え、role と JSON の枠は入力数にだけ入ります。`max_input_tokens` を超える required コンテンツは引き続き `ContextBudgetBlocked` を送出し、mandatory 数が入力数を超える receipt は両方の数を示して拒否されます。例: `Mandatory input tokens (41) exceed the estimated input tokens (40) they are part of`。新しい assembly receipt はより小さい `mandatory_input_tokens` を記録するため、identity は以前に作った receipt と異なります。保存済みの receipt はそのまま parse できます。
- **Program: `main()` の各結果が自身の実行した Core ノードを列挙します (#374)。** interrupt する Core に対して `callCore` を 2 回呼ぶと、各結果の `execution_trace` が 1 attempt ずつ遅れていました: `["draft"]`、`["draft"]`、`["deploy", "deploy"]`。interrupt された command は自分の trace だけで attempt を終えていたため、同じ attempt で先に完了した command が抜け落ち、resume は以前の attempt が完了したすべての command の記録済み trace を再び加えていました。各 `Interrupted` または `Completed` の結果は、前の結果以降に実行された Core ノードを順に過不足なく列挙するようになりました: `["draft"]`、`["deploy", "draft"]`、`["deploy"]`。後の command が別の status で終えた結果も、その attempt で先に実行された command のノードを保持します。失敗した結果も、`define()` のみの run と同様に Core が失敗する前に完了したノードを列挙します。以前は Core が失敗した `callCore` の `execution_trace` が空でした。対象の結果の identity もそれに応じて変わります。
- **すべての schema バージョンに適用されるトポロジーエラーが `schema_version` の削除を勧めなくなりました (#375)。** `GraphEngine::compile` はすべてのエラー一覧の末尾に `Remove 'schema_version' to fall back to lenient parsing` を付けており、`schema_version` のない文書でも同様でした。lenient パースが緩和しない `GC_NO_ENTRY` と `GC_RESERVED_NODE` については、要約が代わりに何を直すべきかを示すようになりました: `"__start__"` からの edge を追加するか、ノード名を変えるかです。lenient の案内と `strict` の接頭辞は、未知のキー（`GC_UNKNOWN_FIELD`）またはノード config schema のエラー（`GC_NODE_CONFIG`）が一覧にある場合にだけ表示されます。診断コード、pointer、メッセージは変わりません。
- **ノード単位のエンジン設定が未知のノード名を拒否します (#376)。** `set_node_retry_policy`、2 つの `set_node_cache_enabled` オーバーロード、`EngineConfig` の `node_retry_policies`、`cached_nodes`、`node_cache_policies` マップは、グラフにないノード名を受け付け、その設定は適用されませんでした。現在は、設定、グラフ、未知のノード、グラフのノードを示す `std::invalid_argument`（Python では `ValueError`）を送出します。例: `set_node_retry_policy: graph 'demo' has no node 'fecth' (nodes: 'fetch', 'parse')`。`build()` と `link()` はエンジンを構築する前に `EngineConfig` のマップを検査します。**動作の変更:** 存在しない、または綴りを誤ったノードにポリシーを設定していたコードは、黙って無視されていた箇所で失敗するようになります。
- **リトライの backoff がオーバーフローしなくなりました (#364)。** 次のリトライ遅延は、`max_delay_ms` に制限される前に `int` へ変換された `delay_ms * backoff_multiplier` でした。`int` の範囲外の積は未定義動作で、x86 では `INT_MIN` になったため、以降のリトライはすべて待たずに実行されていました。jitter を加えた遅延にも同じ変換がありました。現在はどちらも `double` で計算します。NaN と `max_delay_ms` を超える値は最大値に、負の値は 0 に制限され、変換は最後に行います。
- **Program: ゼロの予算上限がそのフィールドを示します (#370)。** `PolicySnapshot` と Python `LocalProgramHost` は、ゼロのフィールドを含む上限を `Every policy budget ceiling except child budgets must be positive; zero: model_tokens, max_dynamic_compiles` のようにゼロのフィールドをすべて挙げて拒否します。`max_child_depth` と `max_total_children` は引き続き両方ゼロにできます。
- **Program: Python と QuickJS のドキュメントがランタイムと一致します (#370)。** Python の Program クイックスタートが再び動作します。以前の例は、バインディングが受け付けないデフォルト引数付きで `GraphState.get` を呼んでいました。QuickJS の `main()` の例は `define()` を export してその Core だけを呼び出し、失敗した `callCore` は `try`/`catch` に届かず実行を終了すると説明します。Python バインディングガイドは、`callCore` の結果の形、ソートして保存される barrier の `wait_for`、`LocalProgramHost` で `P_CHILD_BINDING` により失敗する `ng.spawn`、`compile_admit` が固定する予算、すべて正である必要がある上限、グラフの検査と barrier の停滞、型付きの Program エラー、封印された `diagnostics` と並ぶ `compile_diagnostics`、`P_ADMIT_PROVIDER` で admission に失敗する `llm_call` ノード、`scheduler_threads`、`worker_count`、`NEOGRAPH_PYTHON_NODE_THREADS`、タイムアウト、メモリ内にのみ存在する実行、`resume()` の既定の動作、再開後の `execution_trace` を扱います。トラブルシューティングガイドは対応するエラーを挙げ、リファレンスと概念ガイドは `RunStatus::BarrierStalled` を挙げます。README は、Program API を使わない Python サンプルではなくその Program の例を指します。
- **例がエンドツーエンドで動作します。** すべての例を実際の provider またはオフライン モードで実行し、失敗を修正しました。マルチエージェント討論と自己進化チャットボットは、構成の異なる呼び出しにネイティブ状態を再生する代わりに、ターンを移植可能なテキストとして渡します。the-beast の各変種は、モデルが作成した `llm_call` ノードにモデルを指定し、共有の zero-data-retention provider ファクトリを使い、実行失敗を報告します。A2A クライアントは例 38 のポートを既定にします。Harness MCP サーバーはトークンなしの loopback HTTP で動作します。例 34 は生成画像用にストリーム上限を引き上げ、そのセクションに画像入力対応モデルを使います。program chatbot のデモ ポリシーは descriptor ポリシーの revision 4 に一致します。マルチテナント live サーバーは `NG_MULTI_TENANT_REQUESTS` と `NG_MULTI_TENANT_WORKERS` を受け付けます。Crawl4AI クライアントはパス付きの base URL を受け付け、bearer トークンを送ります。RE エージェントはキーが必要になる前に GhidraMCP バックエンドを確認します。
- **並列ツール dispatch の fallback が捕捉したツール失敗が正しい JSON になります。** 複数呼び出しの batch で、呼び出しごとのエラー処理の外で失敗した呼び出しのツールメッセージは文字列連結で `{"error": "..."}` として組み立てられていたため、例外テキストに引用符、バックスラッシュ、改行が含まれると不正な JSON になっていました。現在は JSON ライブラリでシリアライズします。
- **すべてのツール経路でツール失敗が 1 つの契約に従います。** 複数呼び出しの batch では `HookBoundaryBlocked` がモデルが読んで回避できる `{"error": ...}` ツールメッセージになり、1 回の呼び出しでは実行が停止していましたが、現在はどちらも実行を停止します。ツールコードが C++ または Python で送出した `HookBoundaryBlocked` や `NodeInterrupt` は、すべての経路で `ToolExecutionController` が失敗したツール結果に変えていましたが、現在は実行を停止または一時停止するため、ツールが人に確認を求められます。実行できない必須のツール Hook は(たとえば `llm::Agent` の実行に owner scope がない場合)、すべての呼び出しをモデルへのメッセージ付きで失敗させる代わりに、現在は実行をブロックします。例外を送出する `get_name()` は、batch と同様に 1 回の呼び出しでもその呼び出しの `failed` 結果になります。以前は 1 回の呼び出しではノードが失敗していました。`error` だけを持ち、エラーではない結果としてモデルに届いていた batch の fallback メッセージも、他の失敗した呼び出しと同じ `tool_status` と `{"error", "status", "retryable", "effect_uncertain"}` の形になります。ノードのリトライポリシーは `HookBoundaryBlocked` をリトライしなくなり、batch の複数の呼び出しが実行を停止させる場合は、キャンセル、Hook のブロック、割り込みの順に優先されます。Python の `HookBoundaryBlocked` に `invocation_id` と `state` が加わりました。[ツール失敗の契約](docs/reference-en.md#tool-failure-contract)を参照してください。
- **`outputSchema` を宣言した MCP ツールのエラーメッセージが保持されます。** クライアントは `isError` を確認する前に output schema を検証していたため、`structuredContent` のない正当なエラー結果が "MCP tool advertised outputSchema but returned no structuredContent" で失敗し、`[FUNCTION_NOT_FOUND] No function found named 'x'` のようなツールのメッセージは呼び出し側にもモデルにも届きませんでした。MCP 仕様が認め、公式 Python SDK が行うとおり、エラー結果は output schema の検証を省略するようになりました。成功した結果は引き続き検証します。
- **Program publicationはnode handleのムーブ代入に依存せずlibc++でビルドできます。** 全index publicationの後で抽出したhandleを直接初期化し、ライブラリ固有の`noexcept` traitなしでcommitの原子性を保持します。
- **Windowsのinterface-6 archive fixtureは非公開custodyディレクトリを原子的に作成します。** 継承を遮断した現在ユーザー専用DACLでnative archive admissionを満たし、outcome・金額の根拠・replay・誤ったbindingの検証をすべて維持します。
- **Anthropic MessagesとGeminiの並列client-tool結果を一つのturnに保持します。** 隣接するportableな結果専用メッセージをfamily encode前にまとめ、mixed・native・turn境界は維持します。他のAPI familyと保存historyは変更しません(#311)。
- **History compactionは権限と未完了の根拠を保持します。** 指定されたtyped controlsをそのまま渡し、正常終了した空でない要約だけを採用します。拒否したoutcomeを記録してもhistoryを切り詰めず、要約をsystem命令ではなくuser由来として扱います。真正なnative replay prefixは保護します(#314, #325)。
- **Program fan-out joinは待機前の完了通知を失いません。** 完了状態を保持し、scoped cancellationは新規dispatchを止め、現在および遅れて接続した子を取消します。部分launch失敗は借用状態を使う全producerをdrainしてから伝播します。Awaitは同期的に開始し得る子publicationより先にdeadlineを登録します(#341)。
- **Child generationと保留migrationは整合したdurable headを使います。** immutable admitted lineageでgenerationを確認し、record/journal snapshotが異なるpublicationを混ぜないようにします。保留sourceの解放で消費済みwall-time budgetを更新しません(#324)。 Native PostgreSQL headは一つのMVCC statementで読み、admission・recoveryは所有するheadメンバーをコピーせず移動します。
- **子の予約復旧は承認済みwall-time grantを保持します。** 置換後の残り時間が少ない場合、cold reconnectは保持した予約を元の不変generationに束縛します。現在使用可能な予算、token・金額上限、子の元のdeadlineは更新しません。
- **Synthesis persistence fixtureは検証目的と無関係なローカルawait deadlineではなく親run budgetを使います。** 専用のtimeout・取消テストはoperation deadlineを引き続き検証します。
- **Blocking Tool例外handoffのterminal所有者は一つです。** typed例外とsystem error codeをasync-to-blocking bridgeで保持し、publication後にworkerが例外オブジェクトへアクセスしません(#339)。
- **例とbenchmark driverは利用できない出力で失敗します。** partial/refusal出力を成功routing fallbackに変えず、UTF-8 previewは完全なcode pointを維持します。失敗した試行を成功timing sampleから除外します(#317)。
- **Durable Program統合検証は隔離と復旧を確認します。** cold rematerialization後もnode別の最小権限を維持し、provider journalとTool effectはrun・attempt・slotを区別します。変更された権限を拒否し、reconnect後も未確定dispatchの根拠を保持します(#293, #295, #297)。
- **エンジンのワーカープールを使う multi-Send の fan-out で、ある枝の処理が完了する最中にその枝のキャンセルハンドラが実行されることがありました。** multi-Send ステップの枝は素のプール実行器で動いており、静的 fan-out の枝には既に枝ごとの strand があったため、別のプールスレッドから送られたキャンセルが枝のコルーチンと競合していました(`cancellation_signal::emit` のデータ競合を ThreadSanitizer で再現)。プールを使う Send の枝はそれぞれ自分の strand で動くようになり、プールのない実行は変わりません。#344、#345 を参照してください。
- **HTTP を有効にした `neograph-harness-mcp` と `example_harness_mcp_server` が `OpenSSL::Crypto` をリンクするようになりました。** どちらも OpenSSL(`SHA256`、`CRYPTO_memcmp`)を直接呼びながら、自身ではリンクしていませんでした。SchemaProvider 0.2.0 が libcrypto をリンクしなくなったため、`NEOGRAPH_BUILD_MCP_HTTP_SERVER=ON` の共有ライブラリビルドで `SHA256` の未定義参照によりリンクに失敗していました。
- **同期グラフ実行は scheduler の post 呼び出しが完全に終わってから private `io_context` を破棄します。** 完了ハンドラが実行済みでも pool thread が wake-up 中に残り、`pthread_cond_signal` と `pthread_cond_destroy` が競合することがありました(#346)。同期 bridge は外部スレッドの post と tracked-work の解放を待ち、キャンセル通知の受付を閉じ、遅れてキューに入った通知も処理してから context を破棄します。呼出元スレッドでの実行、入れ子呼出し、親キャンセルの権限は維持し、post されたハンドラの例外でも所有中の coroutine frame を放棄しません。
- **SDK のピンを SchemaProvider `8f32af7` に進めます。SDK 0.2.0、interface/shared-library 世代 5 は変わりません。** SDK は archive 有効化キーを provisioning 失敗のあらゆる経路でゼロ化し(SDK #10)、libcurl 8.21 が合成した完了タイミングを「リクエストが送信された」証拠として読まず(接続拒否と信頼できない TLS は `NotSent` のままで、安全な再試行が働きます)、リテラルアドレスを libcurl の DNS キャッシュに登録して c-ares なしでビルドされた libcurl(macOS の Homebrew)が設定したワーカー数を超えるリゾルバースレッドを作らないようにし、qualification meter の固定 table をスレッドスタックの外に置きます。SDK 自身のテストが native Windows と実際の macOS ランナーで実行されるようになりました。新しい SDK リリースやバージョンではなくソースのピンです。
- **エンジンが自身の fan-out ワーカーで解放されてもプロセスがクラッシュしなくなり、Python の GIL を保持したまま解放してもデッドロックしなくなりました。** 失敗した fan-out ブランチは、実行が戻った後にコルーチンフレームがワーカー上で巻き戻されるまで、自身の例外を保持します。その例外がエンジンを所有するホストオブジェクトを所有していると（traceback が `GraphEngine` を保持する Python 例外など）、ワーカーが最後の参照を手放し、`~GraphEngine` はエンジンのスレッドプールをそのプール自身のスレッドで破棄していました。プールを自身のスレッドから join するとそのスレッドは飛ばされるため、ワーカーの足元でプールが解放され、ワーカーはハンドラを抜ける際にそのプールを使い続けました。負荷のかかった Python テストスイートで断続的に発生した segmentation fault です。このようなプールは直ちに停止され、ワーカーが去った後にプール外のスレッドが破棄します。Python バインディングでは、`GraphEngine` の最後の参照を手放すスレッドが、デストラクタがワーカーを join する間 GIL を解放します。失敗したブランチの Python 例外を解放するワーカーは GIL を必要とするため、以前は join が永久に待つことがありました。

## [0.13.1] - 2026-10-07
NeoGraph `0.13.1` は alpha SDK `0.1.1` を対象にします。interface revision と shared-library generation は 4 のままなので、consumer は SDK `0.1.1` を選択して（インストールされた package config は厳密なバージョンを要求します）再ビルドするだけです。Native archive は v3 / `spna3`、portable JSON は v2 のままです。機能上の変更は下記の Windows transport 修正で、0.13.0 として公開された Windows wheel にはこの欠陥があるため 0.13.1 に置き換えてください。修正コミットの hosted CI と Windows wheel gate は通過しました（GitHub Actions run 37576205702、37576205395、CPython 3.12 のみ）。CPython 3.9–3.13 の全 matrix はリリースタグで実行します。
### 修正
- **Windows: 大きなリクエスト本文が処理の期限まで止まることがありました。** SDK の transport は `FD_WRITE` を待っていましたが、Winsock はこのイベントを `send()` が `WSAEWOULDBLOCK` で失敗した後にしか記録しません。成功してソケットを満杯にした送信ではイベントが生じず、4 MB アップロードの約 4〜6%（loopback で計測）が約 131 KB または 197 KB で止まり、再試行安全性 `PossiblyAccepted` の `DeadlineExceeded` で終わりました。固定した SDK は、書き込み待ちの間に短いタイマーで `select()` も確認します。公開済みの 0.13.0 Windows wheel にはこの欠陥があります。回帰テスト: `test_large_post_does_not_stall_when_the_send_buffer_fills`（Windows では新規接続 150 回の繰り返しで、0.13.0 wheel では 8 回中 8 回失敗）。Issue #340。

### 変更
- **CI を階層化しました。** すべての push と pull request は `ci.yml` のみを実行します。Linux・macOS・Windows の全 native suite と installed-consumer 2 行で、ドキュメントのみの変更はスキップします。sanitizer、fuzz canary、性能 gate、実 ARM64、gRPC、Visual Studio generator、残り 8 行の installed-consumer はそのまま `ci-extended.yml` に移し、毎晩および手動で実行します。Wheel は packaging の変更ではプラットフォームごとに CPython 3.12 を、リリースタグ・週次 canary・手動実行では CPython 3.9–3.13 をインタープリターごとに 1 job でビルドします。Windows の依存関係は vcpkg binary archive を再利用します。`verify_ci.py` は `--jobs auto`、`wheel --python`、`native-windows --generator` を受け付け、`native-windows` は既定で Ninja を使います（x64 MSVC 環境で実行するか `--generator "Visual Studio 17 2022"` を指定）。すべての runner は標準の GitHub-hosted runner のままです。
- **ctest をテストごとのタイムアウト付きで実行します。** `verify_ci.py` が `--timeout 600`（ASan と MSVC ASan は 1200）を渡すため、止まったテストが job の上限を使い切らず、名前付きで失敗します（Issue #341）。

## [0.13.0] - 2026-10-07

NeoGraph `0.13.0` は alpha SDK `0.1.0`、loaded interface revision/shared generation 4 を使います。全 native consumer と Python wheel を一致する header/library で再ビルドしてください。Native archive は v3 / `spna3`、portable JSON は v2 のままです。Retained-feature 統合はローカル Linux x86_64 native と installed wheel 検証に合格しました。コミット `79c21e1` の hosted Wheels と CI(GitHub Actions run 37459723655、37459723545)は Linux x86_64、Linux aarch64、macOS arm64、Windows AMD64 の wheel を作成し、ctest の前に 30 分の制限を超える hosted ネイティブ Windows job(`build-windows`)を除くすべての CI job に合格しました。公開は保護された release 承認の後です。以前の有料観測と benchmark cohort は SDK3 範囲を保持します。

SDK4 Linux x86_64 検証は登録済み 27 case を実行しました。最初の full run で 25 case が合格し、修正した二 case は限定再実行で合格しました。二回目の full-suite 27/27 実行ではありません。未変更 Stop CLI の buffered/SSE valid/invalid client-call 四 case と、正確な installed README consumer の三 variant（reported zero、unknown count、HTTP400 failure）が合格しました。このローカル実証は vendor や Windows/macOS/ARM64 検証を追加しません。

NeoGraph SDK4 の検証範囲は初回と修正後の限定実行を合わせた Core 1261 case、Program 691 pass と設計上の Memory process-loss skip 14 case、installed Python の初回 499 pass と修正後の限定 5 pass を合わせた 504 case です。二回目の full-suite 実行は主張しません。ローカル A2A 0.x/1.0 peer、evolution file mode、plan replay、変更した C++ helper と Python application 11 本を実行しました。

### 変更
- **Windows の provider 生成と byte-budget Bridge。** SDK ゲート `NativeSocketRuntime`(旧名 `PosixRuntime`)を Windows にも付与します。この transport にはすでに Winsock イベント経路があります。出荷されていた `max_host_connections` の許容上限は `INT64_MAX` で、Windows の 32 ビット `long` より大きく、Windows 実行では拒否されました。現在は 2147483647 で、回帰テストが固定します。NeoGraph の SDK Bridge は pmr コンテナを作る前に byte budget を受け取ります。MSVC の `unordered_map` はコンストラクタで確保するため、以前の 0 budget がそこで `bad_alloc` を起こしていました。ネイティブ MSVC Release では登録 1915 件中 1915 件が成功し(設計上の skip 2 件)、変更前は 1916 件中 209 件が失敗していました。テスト専用の変更: `exception_ptr` の同一性アサーション 3 か所は、MSVC の `current_exception()` がコピーを返すためプラットフォーム対応ヘルパーを使います。trusted-archive fixture テスト 1 件は POSIX 権限ビットしか設定しないため、同系統のテストと同様に Windows ではスキップします。Images/Veo の HTTPS peer は `::1` でも待ち受けます。`localhost` が先にそちらへ解決され、Windows は loopback 接続の拒否に約 2 秒かかるためです。1915 件の実行は開発マシンで行いました。hosted のネイティブ Windows CI job は ctest の前に時間制限を超えるためです。hosted の Windows wheel job は CPython 3.9-3.13 についてビルド、repair、installed-wheel テストを実行しました。SDK 自身の POSIX 専用テストは Windows では実行されません。
- **Windows wheel が hosted CI でビルド・テストされます。** 最初の hosted Windows wheel 実行には四つの修正が必要でした。wheel job はマニフェストの `builtin-baseline` に合わせた vcpkg チェックアウトを自前で用意します(runner image のチェックアウトはそれより古いものでした)。CMake define は `SKBUILD_CMAKE_DEFINE` で渡します。引用符を含む `CMAKE_ARGS` の値が cibuildwheel で無意味な変数に分割され、CMake が `<vcpkg root>/installed` だけを探していたためです(ツールチェーンは manifest モードでのみ `VCPKG_INSTALLED_DIR` に従います)。`verify_ci.py` は TLS テスト fixture のために vcpkg の `openssl.cnf` を指定し、アーカイブ保管の Python テスト一件は Windows でスキップします(`mkdir(mode=0o700)` は所有者専用 DACL を作りません)。wheel は libcurl 8.22.0、OpenSSL 3.6.5、libpq 18.4、nghttp2 1.70.0、SQLite 3.53.4、zlib 1.3.2 と Microsoft Visual C++ ランタイム DLL `msvcp140`、`msvcp140_1`、`msvcp140_atomic_wait` を含みます。この三つは Microsoft の Distributable Code 条件に合わせて未変更のまま有効な Microsoft 署名を保つよう、delvewheel の名前変更から除外しています。`THIRD_PARTY_LICENSES.md` は成果物から観測した macOS、Windows、Linux の構成と、まだ不足しているもの(追加構成要素のバージョン別告知全文)を記録します。hosted run 37459723655 は四つのプラットフォームの CPython 3.9-3.13 wheel を作成し、公開される Windows 成果物の署名確認はリリース点検に含まれます。
- **Local/hosted 共通の検証入口。** `python scripts/verify_ci.py <profile> --work-dir <fresh-output> --jobs N` で既存 CI gate 十四件を統合し、native platform、sanitizer、installed consumer、wheel、archive、release gate は除去しません。Installed consumer は Git/Bash を不要とし、別の immutable QuickJS performance gate は Linux/Git provenance を要求します。Native Linux は native PostgreSQL test 後に全 Python/protocol suite を直列実行します。Benchmark checker は元の四 workload と数値境界を保持し、曖昧または無効な metric を拒否します。Source review と CLI help は runtime/platform/performance qualification ではなく、統合実行が必要です。
- **単一の QuickJS matrix コマンド。** 重複 shell build driver を `python scripts/verify_ci.py quickjs-performance --work-dir <fresh-build-root> --jobs 4` に置き換え、`<fresh-build-root>/quickjs-performance.json` を保持します。所有する新しい出力が既存なら拒否します。不変 performance runner、preregistration、ソースルート Git provenance 要件は変更しません。Native Windows toolchain の転送と work-dir 固有の wheel vcpkg root は宣言済み bootstrap/repair gate を保持し、PostgreSQL 前提検査は `psql` に明示的な DB URL を渡します。これはソース変更であり、新プロファイルの合格を主張しません。
- **完了済み取消 context の寿命。** Detach は context 破棄前に完了済み Asio slot handler を解放し、generation 検査は古い待機 emit を破棄します。Callback の rebind は token lock への再入なしに実行され、取消状態は初期化しません。NeoGraph と全 native consumer を一緒に再ビルドしてください。ローカル取消/fork 23 case と実際の plain/ASan retained-token consumer は合格しました。[Commit `24d3166` CI](https://github.com/fox1245/NeoGraph/actions/runs/37275542831/job/111651716661) の 1,333-case TSan 実行も、retained-parent-token teardown 回帰を含め、skip 三件で合格しました。既存 dependency suppression と ASLR 無効化の範囲の実証であり、普遍的な race-freedom は主張しません。Detach 前に operation を完了させ、実行中の signal callback 内から detach しないでください。
- **Platform wheel の依存と cold loading。** pybind11 を `2.13.6` に固定し、SDK の直接 OpenSSL Crypto 依存を解決し、macOS 14 には一致する LLVM 20 header/runtime を使います。CPython 3.9–3.13、全 platform 行、installed wheel の full suite を保持し、その後 Linux replacement と Homebrew アクセスを拒否する macOS cold-loader 検査を実行します。正確な最終 artifact は release gate であり、source のみで portability を主張しません。
- **一致する Linux LGPL library source。** SDist の `deps/redistribution/` に library-only keyutils/libxcrypt source、downstream patch、build/replacement material を含めます。File-scoped grant と受領者の変更権を保持し、grant 未確認の upstream test は library input ではないため除外します。Source/notice delivery だけでは最終 wheel の replacement arrangement や法的確認完了を証明しません。
- **Python cookbook の import 修正。** Native OpenRouter の例は、削除された最上位 export ではなく、installed package の `neograph_engine.llm` から `SchemaProvider` を import します。
- **明示的 SDist 許可リスト。** scikit-build-core 1.0 以上と `explicit` inclusion mode、root に固定したファイル pattern を使用します。実際の 1131-file SDist は許可リスト、private/cache 除外、SDK4 pin と必須内容の検査に合格しました。`manual` mode には無関係のファイルが含まれていました。Wheel command は展開した package directory を使い、空白や Windows backslash を含む test path も独立した引数として渡します。
- **保持した型付き controls とモデル規則 (#309)。** Newer-master `c708e51a`、`165dbbbd`、`5553d9ea` を family 別 controls に統合しました。Chat reasoning/include/usage/代替モデル、Responses provider-held cursor/parallel tools/verbosity/truncation/include、Messages manual/adaptive/disabled thinking/effort/cache/tool choice/routing、Gemini thinking level/safety/tool choice/temperature を提供します。誤った family/origin/value は I/O 前に拒否します。承認済み model prefix 規則は gateway suffix も大文字小文字を区別せず照合し、禁止された明示 temperature を拒否します。Messages thinking は temperature を省略します。JSON knob interpreter は復元しません。
- **明示 history と deployment headers (#305/#306, #310)。** `9e161fc8`、`c5035e00`、`02b4c275` を統合しました。真正な same-route native reasoning の所有を保持します。Gemini `PortableForeign` は unsigned assistant text/client-tool history を受け、最初の foreign function call だけに文書化された bypass を入れます。失敗 native seal は降格しません。Responses `previous_response_id` は新 input を使い、真正な tool ownership が必要な場合に非 wire の `previous_response_history` を受けます。Provider-held state は full native/archive replay 権限ではありません。Deployment environment header は admission 前の明示 host preprocessing で、optional 省略と大文字小文字を区別しない override 優先順位を適用します。
- **保持した outcome、usage、retry 意味 (#307/#308/#312)。** 新しい failure/terminal 修正（`782e23ea`、`0eb271de`、`f89afe08`、`99fa81a2`）は HTTP-200 error を含む typed 所有 failure/partial evidence として保持します。`ec69b845` の cache/reasoning 意図は欠落を zero にしない nullable 全報告で提供します。`054a2c1c`/`b67ccf4c` は旧 retry wrapper なしで SDK no-output-replay safety と bounded exponential full-jitter policy に対応します。`51374ce8` の client-tool intent は valid/invalid call の Stop event と最終 outcome で一致し、具体的 stop reason と server 実行 tool を区別します。
- **保持した A2A wire と HTTP streaming 修正。** `08c4df70`、`cd21e3e3` を統合しました。Card が選択した 0.x/1.x dialect は method/body/version header を一緒に変更し、SSE task/status/artifact 組立は output を保持して観測 event 後に dialect retry しません。Server encoding は version header に従います。回答の優先順位は terminal/interrupted agent status、artifact、agent history です。Fixed-length stream reply は bounded body/status を保持し、early EOF/buffered surplus を拒否します。Bound discovery/restart 動作を保持します。ローカル native/Python peer 検証は合格し、macOS 検証は未完了です。
- **保持した research、fork、example 修正。** `861619a4`、`03e78f6d`、`5553d9ea`、`9b4b2776` を typed 経路に統合しました。Research は tool call のない完了済み empty `MaxTokens` だけに最大二回追加呼び出しし、元の bank/deadline 内で cap を 16384 まで倍増し、別 call identity と全 outcome/usage report を保持します。表示 partial report は再呼び出しせず `Incomplete` とし、empty final report は失敗します。Exception/配信済み stream part は redispatch を許可しません。Fork は pending continuation を保持し、example 08 は new-turn フローのままです。Evolution は両 mode で `pnoop` を登録し、plan replay は四呼び出しを節約します。Research、file mode と変更 helper の限定検証はローカルで合格しました。
- **Breaking 型付き Python provider API。** Python は `make_provider_request`、`Provider.prepare`、一回だけ使う `Provider.dispatch`、所有 `ProviderOutcome` の completion/failure view を使います。`SchemaProvider` は `ValidatedDescriptor`、`ProviderRuntimeOptions`、`SchemaProviderDefaults` を受け取り、型付き `ProviderMessage` は graph 便宜型 `ChatMessage` とは別です。欠落 usage は `None` のままです。旧 completion API と `OpenAIProvider`/`RateLimitedProvider` は shim なしで削除しました。Installed Linux wheel と実際の例の実行は上記範囲で合格し、全 platform/hosted vendor の新しい検証ではありません。
- **イベント駆動 provider handoff；C++ 再ビルド必須。** SDK Bridge の1ms完了/取消 polling をcapacity-one coalesced Asio通知へ置換しました。Active drain は余分なwake提出なしでpublication hintを再観測し、context shutdownはchannel teardown前にnon-owning publisher endpointを切り離します。`CancelToken::stop_token()`と`is_cancelled()`は同じ`std::stop_source`状態を使い、fork/Asio-slot取消を保持し、unbound/shared tokenもslotを奪わず転送します。全C++ consumerを一致header/libraryで再ビルドし、pre-v1 shared libraryのみの交換は不可です。Local TLS GraphEngine A/Bは48 process record、4800 measured graph run、失敗0；256B padding p50は1.302474→0.952992ms、処理量738.90→1022.96 graph run/s。Tool/SSE処理量は2.54%/6.69%減少し、普遍的speedupではありません。Targeted regression43件、ASan/UBSan publisher-teardown200回、installed-SDK consumer300回がpass；TSanはこのhostで実行不能でした。Parser/ISA flagとpaid/Python qualificationは変更していません。[実測結果](benchmarks/provider-notification-summary.json)を参照してください。
- **E2Eで観測した protocol/demo 修正。** `neograph_get` は URI/cursor page を無視せず retained 結果を選択し、stdio/HTTP lifecycle・URI と SQLite 再起動検証が pass。空の A2A discovery URL は実際の bind endpoint と再起動に追従し、明示 public URL は保持します。uppercase 例は実証 SSE capability を広告し、MCP time demo は指定 IANA timezone を使用します。ProgramChat mock は bounded admission を迂回せず供給された明示 model-fact policy を選びます。新検証は番号 target39個、offline Beast9 mode、Assembly A2A、Jarvis 永続性、専用 mock topology load、retrieval admission、ProgramChat SQLite/PostgreSQL/browser。measurement-only/live/voice/GUI/延期 Python の境界は別途明示します。
- **文書/コメント整合性。** アーカイブされた Stage 3 提案は削除済み Provider completion API を明示的に後継へ置き換えられたものとし、現在の型付き prepare/dispatch 指針へリンクします。Cookbook 状況は実証されたオフライン Assembly/Jarvis/Beast と ProgramChat PostgreSQL/browser 範囲を過去の時間記録、未検証 live/voice/Python 変種と別の multitenant load recipe から区別します。現在の cancellation/HTTP/isolation コメントは削除された provider API を説明しません。Runtime 動作や qualification 境界の変更はありません。
- **Breaking C++ typed provider/custody 契約。** 全 consumer/custom provider を一致する新 header/library で再ビルドします。`NEOGRAPH_BUILD_LLM=OFF` でも Core は外部 `SchemaProvider::runtime` を要求します。Provider は所有 `ProviderRequest` → move-only `PreparedProviderRequest` → 不変 `sp::runtime::Result`、明示 Collect/Stream、宣言済み family control を使用します。Completion 互換 type、JSON descriptor interpreter、primitive registry、Responses WebSocket は alias/shim なしで削除されました。SDK `0.1.0` は alpha（interface revision/shared generation 4）で、安定 interface ではありません。下記の以前の C++ runtime/archive 検証は SDK3 Linux/POSIX の記録で、Python wrapper や retained SDK4 変更は実行していません。

  Fresh installed find_package Program C++/C ABI/dualQuickJS consumer と NeoGraph/SchemaProvider typed2-request lifetime/native/raw/mismatch consumer は pass しました。Interface/ABI 宣言だけと実証 package 結果は別です。より広い platform や安定 release は主張しません。
  Native custody は closed v3 / `spna3`（v2 拒否）で、全 semantic descriptor field と正確な owner/custody を結び付けます。診断 duplicate key は元の raw evidence を保持しますが実行 config にはならず、non-2xx JSON/named SSE error は factual SDK 証拠として残ります。Host-positive `observer_limits` は上限を下げるだけで、digest v3 は実効上限を結び付けます。PMR ownership は queued/draining container を含み、cancellation は queue mutex 外で行います。Generic messages は Generic のままで、mapped native history は C++ sidecar を保持します。
  共通 `ProviderOutcomeError`、`ProviderFailure`、`ProgramFailure` は全 SDK 結果と元の/二次 live cause を保持し、outer Node/Program の再 dispatch はありません。Native failure witness には真正な archive custody/Runtime hydration が必要です。事前入力 data-only witness は admission を迂回できず、nonpersistable 失敗は publish/replay できず、再起動後の元の exception pointer は null です。
  `RecordedBindingSet` は trusted Catalog が source event から capture して認証する source-bound data です。`replay_recorded` は元の selected-source permission を検証後、実際の remaining credit を durable CAS で移し、inherited spend は新 grant ではありません。更新型 `start_recorded` API は削除され、正確な owned Program lease は expiry で更新せず、captured JavaScript command は external effect を繰り返せません。Archive 所有は spending 権限を与えません。個別承認の最小 Images/Veo/Decisions 3 呼出し成功は広範な qualification や invoice 証拠ではありません。実際の統合検証と残る制約は下記で、以前の M5 測定は歴史です。

  **Recorded-control causal fix は full suite で実証済み。** Captured command replay は実行前に新しい CPU wall-time/Core work だけを durable に reserve し、測定済み work と新しく生成した Core checkpoint を result CAS で publish します。新しい model、money、Program-operation allowance を消費せず、captured external effect を再 dispatch しません。未精算 reservation は debit を保持します。Reservation により、最初の新しい Core checkpoint を拒否した通常の Running→Running transition ではなく認証済み settlement transition を選びます。Await channel receive、timer wait/cancel、handoff wait の開始/release は owning executor/strand 上で直列化します。既存 Recorded CPU/Memory await/handoff scenario は full suite で pass しました。Remote TSan coverage 制約は下記に明記します。

  **有料観測は完了；普遍的な qualification ではありません。** 元の `SPQUAL1` base630/1000000 microUSD は不変です。同じ元 ledger の ONE hash-chained `A` が承認済み extension480/3000000 を受け入れ、aggregate1110/4000000 になります。Calls/spent/hold/settlement は累積で新 grant ID/header/reset はありません。正確な declaration byte/file identity と original authorization/baseline/catalog/activation/ledger-prefix の hash/totals は固定され、削除・置換・変更は fail closed です。最終 canonical ledger は calls1110/spent437958/held1287828 microUSD、eventA1、limits1110/4000000；spent+held US$1.725786 は LOCAL catalogue meter で invoice ではありません。記録済み five-family60-pair baseline は600 request 完了：Chat60/60、Responses60/60、Messages60/60、Generate56/60（incorrect-vision SSE4件）、Interactions57/60（incorrect-vision buffered1件/SSE2件）；合計293/300 pair で300/300ではありません。他の old600 financial record は保持しますが完全な behavioral proof ではありません。以前の M5/media one-shot cohort は不変です。以前の Google3-round prerequisite は invalid-tool2件/unreadable-positive1件の失敗状態を保持します。最終 SDK 証拠と native-axis 制約は baseline 成功とは別です。 以前の activation/reopen smoke は2回 reopen 後 calls610/spent219159/held751233、SDK meter/canary/vision4-test19.38秒 pass として保持します。これは限定された以前の checkpoint で最終 ledger totals ではありません。以前の検証済み Chat60-pair cohort は実際の attempt120、UpperBound charge120、UnknownHold なしを保持します。

  **Native-axis 観測は cryptographic 検証・native consumption/equivalence ではありません。** Generate は mutation/omission/duplication を受け入れました。Interactions は isolated genuine source/positive control、one-owner signature mutation、thought-carrier omission、call-carrier omission、duplication を受け入れました。全 thought/signature 削除は generic400、THOUGHT item を保持して全 signature field を削除した場合も generic400 でした。最後の capture は local encoded-original retention control で、same-capture server positive ではありません。以前の positive cohort は真正です。観測は aggregate-carrier-absence boundary のみを示し、issuer/signature 検証や vendor consumption を証明しません。実際の report：SDK `config/qualification-extension-results.json`、`qualification-final-summary.json`、`qualification-native-axis-results.json`、`qualification-combined-omission-results.json`、`qualification-signature-presence-results.json`。Prerequisite-failed/not-run/negative-inconclusive の状態は事実のままです。 Thought-only/carrier-only omission は別 carrier が残る状態で受け入れられました。Issuer-validation/native-consumption の主張を強めません。

  **実際の統合証明と残る制約。** 最新 Core full run は2242 test、失敗0、skip16（RAM process-loss 非適用14件/live-credential gate2件）、130.17秒です。`PgNestedJsonRoundTrips` は duplicate key/order/null metadata、blob、residual を正確に保持し0.18秒で pass。未変更の元 shared-bank fork と既存 Recorded CPU/Memory await/handoff scenario も pass。実際の wrappedMemory/SQLite/PostgreSQL/gRPC finite130/hold65/lower129/strip/old-head/pruning/no-archive/import probe は plain と ASan+UBSan で pass。LOCAL Memory/SQLite/PostgreSQL TSan scope は7件 pass、warning0。System Abseil/Protobuf を含む full mixed gRPC TSan は exit66、dependency/generated-RPC stack に race warning402件。これは instrumentation/coverage 制約で proven false positive ではありません。Remote TSan/race-free は主張せず warning を suppress しません。Installed find_package Program C++/C ABI/dualQuickJS の3 consumer は pass。Fresh installed NeoGraph/SchemaProvider typed consumer は実際の HTTP request2件、coroutine 開始前の provider 破棄、native/tool replay、refusal、known-zero/raw 保持、実際の LinkedMismatch 拒否で pass。Browser Alice/Bob isolation と generation2 replacement を目視検証し、PostgreSQL Program Chat black-box6件は18.989秒で pass。最新 SDK26/26 は失敗0、74.07秒で pass。最終 ReleaseGraph16設定 ×fresh process3回/48記録は38.29秒、失敗0、全 actual protocol/owned-outcome check pass で完了しました。NeoGraph `benchmarks/provider-cutover-final-results.json` と `benchmarks/provider-cutover-final-summary.json` は独立した最終 cohort を保持します。測定中 compiler/有料 model は実行せず、歴史 cohort は不変で semantic/resource equivalence は主張しません。Unstable SDK/ABI3 は安定 release や広い platform qualification ではありません。

  完了した chat pair は downstream vendor の native-continuation 消費を証明しません。
- **Standalone bank journal 修正 — 現在の契約を改訂；実際の runtime 証拠は下記。** Protocol は単調 trusted-store namespace obligation と、実際の不変 original owner/thread/graph scope、ceiling、deadline/clock identity、generation を要求します。全 checkpoint commitment/revision に対する正確な durable head CAS だけが host-owned opaque lease を発行できます。正確な pending effect window を provider I/O 前に永続化し、真正な SDK outcome と実際の charge、nullable report、hold、dedup identity で精算しなければなりません。Checkpoint/next head は同じ owned actor/revision 下で原子的に publish します。Bank metadata 削除、checkpoint pruning、old authenticated snapshot replay、同一 ID overwrite、actor 喪失で credit を与えてはなりません。既存 65 hold がある ceiling 130 を 129 に下げると別の 65 は許可できません。証明済み no-effect 失敗は unchanged head を release し authentic 130 復旧を可能にできます。Crash/unknown/lost-lease window は refund/retry/fallback なしで hold を保持します。Plain/pristine archive 設定は money/native spending lease を与えず、現在の `config.usage` は既存 standalone obligation を置換できません。Program/external-bank journal 所有は不変です。これは要求契約です。実際の currency/custody 証拠と instrumentation 制約は下記であり、安定 released API 保証ではありません。
  実際の宣言は `ManagedBudgetLeaseScope`、`OwnedManagedBudgetLease`、`ManagedBudgetEffectReceipt` と checkpoint-store acquire/begin/settle/publish/release 操作を含みます。`begin_managed_budget_effect` は正確な claim/prepared request digest を結び付けます。実際の backend/runtime currency probe と installed-consumer 結果は下記です。宣言だけは runtime 証拠ではありません。

  **真正な InMemory shared-bank fork は保持・実証済み。** 元の真正な C++ fork は ONE original financial journal と trusted current branch head を使い、grant を複製しません。`publish_managed_budget_fork(authenticated_source, genuine_shared_bank_fork)`（および `_async`）は authentic current source/full commitment と実際の same-bank native C++ pointer を要求し、durable standalone fork は明示的に unsupported のままです。`OwnedManagedBudgetLease::scope()` と original owner/thread/graph、ceiling、deadline/clock、generation は不変です。Read-only store-issued `execution_thread_id()` / `execution_storage_thread_id()` は execution branch を別に選び、`GraphState::budget_original_thread_id()` は元の financial bank を示します。正確な selected-branch head CAS と global actor/revision は canonical current counter、pending effect、burned identity に対して全 branch を直列化します。Original/fork branch は補充なしで使用可能なままです。Stale snapshot、checkpoint copy、imported JSON は alias を発行したり head を巻き戻したりできません。元の root30 → charge3 → original continuation6 → fork lower20 → continuation9 の same-bank 証明は未変更 test_graph_engine.cpp:810–913 で PASSED です。Saved original ceiling30 は effective fork ceiling20 と別です。Widening31 と JSON-only restore は拒否必須です。Unbounded reported observation は factual data で finite grant ではありません。証明済み zero-effect lease だけが unchanged head を release でき、unknown/pending effect は obligation を保持します。

  **現在の release-error 契約；実際の suite/probe は下記。** `<neograph/graph/engine.h>` の `graph::ManagedBudgetLeaseReleaseError` は `ProviderOutcomeError` を継承します。`cause()` は元の execution exception を保持し、`release_error()` は二次 durable lease-disposition 失敗を公開します。`outcome()` は真正な SDK 証拠があれば保持し、SDK outcome がなければ null です。Release 失敗は結果を捏造せず再 dispatch も許可しません。Closed `_neograph_managed_budget_scope` metadata は元の logical scope/cap/deadline clock/generation を記述しますが、backend CAS 権限ではなく data です。

  **Archive-owner/retention 契約；実際の suite/probe は下記。** Finite standalone root または authenticated finite source だけが、実際に設定された `sp::NativeArchive::owner_scope()` から省略された original owner を継承します。Unbounded/plain owner metadata の意味は不変です。明示的に矛盾する archive owner は lease acquire 前に拒否します。`CheckpointStore::retains_native_checkpoint() const noexcept` と対応する Core/Async storage capability は既定 false で、実際の InMemory backend は true に override し、wrapper は実際の retention を委譲します。この read-only 記述は正当な unleased/plain/unbounded C++ native checkpoint custody を許しますが、spending credit や native replay authority は与えません。Leased custody は JSON flag や推測した store type ではなく実際の store-issued receipt を使います。

  **Native-custody pre-I/O gate；実際の suite/probe は下記。** Managed effect begin は pending-effect/slot/held-window 変更前に、真正に結び付けた NativeArchive または実際の local store-issued private C++ retention capability を要求します。Private capability は JSON から import せず wire にも転送しません。C++ sidecar は境界を越えられないため、remote backend が InMemory でも gRPC は実際の client/server archive を要求します。Archive が finite source owner を提供しなければ元の anonymous owner scope は空のままで、実際の archive binding は original scope に一致しなければなりません。Financial head/lease 証拠だけでは native-custody readiness を証明しません。

### 追加
- **所有者スコープの実験的なアクティブ Program 起動。** C++ の
  `ProgramRuntime` と Python の `LocalProgramHost` は、新規実行時に
  承認済みの不変なアクティベーションを一度だけ選択し、選択した記録と
  そのバージョンに固定されたハンドルを返します。ロールバックは以後の
  起動にのみ影響します。グラフ移行では、未知のノードローカル状態を
  移せるとみなさず、不透明なチェックポイントメタデータを拒否します。
  ストア間の永続 GC と本番向け性能検証は別途必要です。
- **Program Core Tool grant の永続 ID。** ホストは所有者スコープ内の
  正確な Tool grant を SQLite に承認し、Program 操作と再接続のたびに
  gate/controller を再結合できます。ランタイムは承認された実行バインディング
  fingerprint も確認し、取消は再起動後も有効で同じ grant ID の全試行を
  拒否します。効果の結果には別途、呼び出し単位の永続 Tool broker が必要です。
- **パフォーマンスビルド構成。** ローカルの GCC/Clang ビルド向けに
  `NEOGRAPH_ENABLE_NATIVE_OPTIMIZATION` オプションを追加し、単一構成
  ジェネレーターで `CMAKE_BUILD_TYPE` が空のため Release 最適化フラグが
  付かない場合に警告するようにしました。Release のハードニングは既定で
  有効です。
- **Program ベース A2A サービスの厳格認証。** Program A2A コンストラクタに
  オプトインの `require_authenticated_requests` を追加しました。有効時は
  collaboration envelope だけでなく通常の message、stream、task 取得・取消
  RPC にも authenticator を適用します。互換性のため既定値は false です。
- **ホスト所有 A2A 制御メッセージのインターセプト。**
  `ProgramAgentAdapter` は認証済み typed message を `ProgramRuntime` 開始前に
  ホスト callback へ渡せます。callback は task/context identity を保持し、
  返された Task は通常の task 観測用に保存されます。
- **信頼されていない補助ランタイムコンテキスト。**
  `ContextArtifactKind::UntrustedSupplemental` と
  `ContextPlacement::AfterHistory` を追加しました。既存の artifact kind の
  system message 動作は維持しつつ、明示的に信頼されていない RAW または
  derived context を receipt に結び付け、完全な Human/AI/Tool 時系列履歴の
  後に user data として渡せます。

### 修正
- **サブグラフの復旧境界。** 管理用状態更新は中断した実行の継続 ID を保持し、
  保持型の子は新しい親呼出しと同じ呼出しの再開を区別します。Stateless の
  静的中断は効果の前に拒否し、中断復旧は非同期チェックポイント読込を使います。
- **MCP とホストプロセスの分離。** 採用したツールには独立した固定承認と正確な
  引数値が必要です。取消・schema 変更は参照が残る client も終了し、起動・stderr
  上限、handle/FD 分離、子孫終了、出力読込上限を適用します。
  macOS では Darwin のシグナルマクロに対応し、fork 前にプラットフォームの
  アクセサーから継承環境を取得します。
  POSIX の close 代替経路は 65536 の上限ではなく descriptor の hard limit
  を使い、soft limit を下げる前に開いた高番号 FD も閉じます。Windows の
  継承検証は再利用可能な handle 番号ではなく、親の元の event 状態を確認します。
- **クリーンな runner での契約検証。** CI は Protobuf の compiler/runtime
  だけでなく、開発用ヘッダーとライブラリも明示的にインストールします。
  分離 stdio fixture は Python の検出に使った検索パスを明示的に承認します。
- **テナント安全な永続 Harness 記録。** File/SQLite でスコープ ID と参照を
  往復し、長い File キーには固定長 hash ファイル名を使います。SQLite 保持処理
  は別 namespace の記録や journal を削除しません。
- **実行可能な Harness CLI。** 起動ポリシーが Program の対応する子の深さ上限を
  使い、実際の stdio compile/start/get 回帰検証で配布 binary を確認します。
- **新しいカタログからのアクティブ Program 起動。** 実行をバージョンに固定する
  前に、選択済みの永続バージョンを復元し、アクティベーションポインタを再読込
  しないようにしました。
- **生成 artifact の永続再生。** Provider receipt は再オープンと結果調整後も
  artifact の順序、payload、入れ子の metadata を保持します。揮発性 SQLite
  パスと artifact の証拠がない旧 receipt は、空の結果を捏造したり再送したり
  せず、明示的に拒否します。
- **プロモ動画の依存関係のセキュリティ。** `docs/promo/package-lock.json` の
  間接依存 `js-yaml` を 4.3.1 から 4.3.2 に更新し、空のマッピングの
  マージで CPU を消費するサービス拒否脆弱性
  (CVE-2026-84375 / GHSA-2883-xcg3-v3hh) を修正しました。
- **TSan CI の RSS 検査。** shadow memory とランタイムの管理コストが
  通常ビルド用の閾値を超えることがあるため、GCC/Clang ThreadSanitizer
  では通常ビルド用の RSS 上限検査をスキップします。sanitizer での
  200 並行実行 stress test と、非 sanitizer ビルドの既存の 10 MiB RSS
  上限は維持します。

## [0.12.1] - 2026-08-23

### 修正
- **Pythonサンプルのランタイム契約。** ルートチャネルのサンプルを予約済み
  `__route__`へ更新し、動的`Send`ペイロードチャネルを宣言し、純粋なキャッシュ
  サンプルを`CacheScope::Reusable`へ切り替えました。さらにOpenRouterのベース
  URLを保持し、Windowsコンソール出力をUTF-8安全にし、成功扱いでも空の
  reasoningモデル応答を拒否または再試行します。
- **Responsesトランスポートのデモ。** 公式OpenAIでは実際のWebSocket
  ストリーミングエントリポイントを使用し、互換ゲートウェイではビルド済みの
  HTTP/2またはHTTP/1.1を選択します。ディープリサーチデモには上限付きの
  トークン・時間予算と3-way/2-worker fan-outを適用し、OpenRouterで実地検証しました。
- **対処可能なWindowsネットワーク障害。** Pythonで`_HAVE_LIBCURL`を公開し、
  ロケール符号化された`std::system_error`メッセージをcategoryとcode付きの安全な
  ASCII診断へ変換します。Winsock 10060などの原因が`UnicodeDecodeError`に
  置き換わることはありません。

## [0.12.0] - 2026-08-23

### 追加
- **厳格なランタイム・インターセプション完了。** `StrictRuntimeProfile`、永続的なプロバイダ終端結果レシート、SQLite スキーマ v3 マイグレーション、トランスポートに依存しない必須 Hook バックエンド（JSON-RPC stdio/HTTP による成果物公開）、一般必須コンテキスト、`HardConstraint` 成果物、完全保存の `ContextTransformReceipt`、永続的なランタイム開発者指示決定、およびコンパイル前の予約 `ProgramSynthesisGateway` を追加しました。生成されたソースは依然として自身をアクティブ化、バインド、マイグレート、または生成できません。これらは引き続きホスト所有の Program 遷移です。`DurableProviderDispatchReceiptStore` のカスタムサブクラスは、今後、終端の `settle`/`outcome` を実装する必要があります。C++ コンシューマーはこの ABI 変更に対して再ビルドする必要があります。

- **分離された PostgreSQL Program ストア統合フィクスチャ。** ダイジェスト固定、ループバックのみの `tests/fixtures/q7-postgres/compose.yaml` を tmpfs ストレージとヘルスゲート付きで追加しました。`NEOGRAPH_TEST_POSTGRES_URL` が使い捨てテストデータベースを指定する場合、`ProgramCatalogTest.PostgreSQLProgramStoreReopensActivationAndOwnerVisibility` は公開、アクティベーション、再オープン、およびオーナー分離を対象とします。これはテストインフラストラクチャです。

- **フェイルクローズ QuickJS レガシードレイン監査。** `scripts/audit_legacy_drain.py` とその CTest 契約を追加しました。このツールは、明示的に列挙された凍結 Program/Harness ストレージスナップショットから、検証可能なコンテンツアドレス指定の証明を生成します。未知のレコードや変更可能なレコード、未分類のレガシーソース、ドレイン専用レコード、アクティブまたはリカバリ可能なレガシー実行を拒否します。ライブの `-wal`、`-shm`、または `-journal` のサイドカーが付属する SQLite 入力は、開く前に拒否されるため、アクティブな WAL データベースの生のコピーは最終証明として機能しません。これにより証拠メカニズムが確立されますが、デプロイメント固有の最終ドレインやレガシーパーサーの削除が完了したとは主張していません。

- **PostgreSQL最終ドレインアーカイブスキャン。**レガシードレイン監査人は、現在、凍結された `program_postgres_dump` カスタムアーカイブを受け入れ、 `pg_restore` をデータのみ、厳密テーブル、スクリプト出力モードでのみ呼び出します。データベースへの復元は決して行いません。Programバンドル、バージョン、およびアクティベーションテーブルを、永続化されたIDに対して検証し、必須テーブルを省略または変更したアーカイブを拒否し、レガシーProgramバージョンのアクティベーションを最終削除のブロッカーとして扱います。

- **デプロイメントなし最終証明モード。** レガシードレイン監査人は、プレリリースまたは本番のNeoGraphデプロイメントが一度も存在しない場合にのみ、名前付き演算子の証明文を受け入れるようになりました。このモードは、ストレージ対象も過去のレガシーアーティファクトも受け入れず、生成された証明に `evidence_mode: "no_deployment_attestation"` とラベル付けし、混合または未証明の空のインベントリの場合はフェイルクローズします。消去、削除、紛失、またはアクセス不能な過去の状態をカバーすることはできません。

- **OpenRouterプロバイダールーティング。** `OpenAIProvider` は、以下で渡されたオブジェクトを `CompletionParams::extra_fields.provider` から `provider`としてChat Completionsリクエストボディへ転送する。非オブジェクトの値はHTTPリクエスト前に失敗する。これにより、OpenRouterの文書化された per-call ルーティング設定が公開される一方、他のネイティブな `extra_fields` キーは無視される。ライブのBeast cookbookはプロバイダを固定し、4,000トークン生成予算に対して明示的な180秒タイムアウトを使用する。

- **Copy Ninja ローカルグラフノードブリッジ。** トランスポートフリーの `a2a::CopyNinjaNode`を追加しました。これは、別途マテリアライズされた Copy Ninja ハーネスをラップし、`prompt`を読み取り、`response`を上書きします。`cookbook_the_beast_copy_ninja` ライブcookbook を追加しました： その LLM はこの固定されたローカルノードのみをオーサリングでき、通常のCore ゲートの後に4番目のローカルバインディングゲートをパスする必要があり、合成ソースエージェントが RPC を観測した場合はフェイルクローズします。カードテキスト、エンドポイント、資格情報、ソースは未承認のキャンディデートから除外され、呼び出し元プロンプトがオーサリング LLM リクエストに入ることはありません。


- **オプションのProgramコンポーネント境界。** オプトインの`NEOGRAPH_BUILD_PROGRAM`スイッチ、エクスポートされた`neograph::program`ターゲット、および`<neograph/program/program.h>`エントリポイントを追加しました。インストールされたパッケージコンポーネントの検出は、ビルド時のみProgramを報告するようになりました。Coreのみのインストールでは、既存の`neograph::core`リンクインターフェースを維持します。

- **不変のProgram値モデル。** 安定した型付き診断、深く所有されたcanonical-JSONおよびC++ビルダー `ProgramSource` 入力を追加。不変のコンテンツアドレス指定 `ProgramBundle`/`ProgramVersion` 値、canonicalシリアライゼーション、SHA-256アルゴリズムタグ付きID、ソースマップ、インポート、および厳格なバージョン付き保存値スキーマ。 `neograph::program` は現在コンパイル済みのエクスポートライブラリであり、Coreのみに依存し続ける。Bundle/version v1プロジェクションは現在、封印されたCore定義とプランID、セマンティックバージョン付き実行可能ダイジェスト、契約、クロージャ、境界、および型付きadmission/materializationレシートを必要とする。それらのIDはフォーマットとストレージバージョンをバインドし、セマンティックセットは安定した順序を使用し、診断は無効なポインタ、逆転したスパン、未知の列挙を拒否しつつ、正確なオフセットが利用できない場合はパーサースパンを省略する。

- **Sealed Program の admission クロージャ。** イミュータブルな `RegistrySnapshot`、`AdmissionProfile`、および `PolicySnapshot` の値を追加しました。ビルダー時の呼び出し可能キャプチャ、厳密な正規マニフェスト、ドメイン分離されたフィンガープリント、および `ProgramVersion` におけるフェイルクローズのクロスフィンガープリント検証を備えています。Core は現在、Program の実体化のために、明示的に名前付けられたローカルのみの parse/link/validate エントリポイントを公開しています。既存のローカルファースト/グローバルフォールバックのオーバーロードは変更されません。レジストリエントリは現在、推移的 admission クロージャのための正規の正確な実行依存エッジを記録しており、ローカルのみの条件チェックは、プロセスグローバルレジストリを参照せずにレガシーキーエッジドキュメントをカバーしています。

- **単一ルート`call_core` Programコンパイラ。** `ProgramCompiler`を追加しました。これは閉じたProgram-v1エンベロープのみを受け入れ、シール前に純粋なローカルCoreパース/ラウンドトリップ/検証を実行し、RFC 6901ポインタとソースマップ帰属を備えた集約型の型付き診断を出力します。コンパイルは、ファクトリや呼び出し可能を起動することなく、正規のProgram、レジストリ、推移的実行可能クロージャ、機能/効果、インポートMerkle、シール済み定義、およびCoreプラン同一性を導出します。著者ドキュメントスキーマ、完全な有限予算契約、ゼロディスパッチ拒否テスト、静的および共有インストール済みコンシューマカバレッジがコンパイラに含まれています。Coreは加算的な全量パース/ラウンドトリップおよびローカル検証レポートを獲得しましたが、レガシーのthrowing APIは既存の動作を維持しています。

- **Pinned Program ランタイム垂直スライス。** `ProgramCatalog`、`EngineGenerationCache`、`ProgramRuntime`、共有の`ProgramHandle`、不変の`ProgramResult`、型付きのProgramイベントエンベロープ、インメモリの`ProgramStore`、そして追加専用CASの`ProgramJournal`を追加した。Admissionは、マテリアライズ前に未検証のバンドルセマンティクスを再計算する。各試行は不変のCore世代を1つピン留めし、既存の`GraphEngine`非同期パスを呼び出す。ランタイム実行は現在、完了、中断、正確なチェックポイント再開、キャンセル、タイムアウト、Coreステップ枯渇、チェックポイント非互換性、および失敗を型付き終端状態にマッピングし、更新不能なバジェットとチェックポイント系統を保持する。ジャーナルコミットはチェックポイント/終端イベント配信に先行し、同時再開には1つのCAS勝者のみが存在し、PR6スライスはCoreブローカーが存在するまで副作用のある、または空でないスキーマのProgramを拒否する。

- **QuickJS 制御言語フロントエンド。** オプトインの`NEOGRAPH_BUILD_QUICKJS_CONTROL`、シールドされた`ProgramSource::from_javascript(...)`、そしてプライベートなコンパイル専用のQuickJSコンテキストを追加した。ソース・エンベロープはエンジン/言語/ホストAPIのバージョンを固定する。その唯一の`ng`ホストサーフェスはバージョン管理されたグラフビルダーであり、メモリ、スタック、割り込みポーリングの制限はフェイルクローズする。JavaScriptは、1つの不変の`call_core` Program プランを生成し、ランタイムVM、バイトコードアーティファクト、またはCore依存関係になることは決してない。

- **A2A Agent Card 互換性候補。** ワンリクエスト、未認証、リダイレクトなしのwell-known-cardコレクタと、ファクトリー専用の不変候補コンパイラを追加した。候補は、ダイジェストピン留めされた来歴、有界なプロトコルファクト、安全なスキルIDのみを保持し、自由形式のカードテキスト、アドバタイズされたRPCエンドポイント、プロバイダー/セキュリティ設定、資格情報は除外される。Copy名PoCはさらに、そのダイジェストに固定された独立して観察された挙動を必要とし、ソースエージェントをディスパッチすることは決してない。

- **SQLite Harness レコードストア (issue #147 フォローアップ).** オプションの`neograph::mcp_sqlite`ターゲットと、WAL裏付け、スキーマバージョン管理されたアーティファクト/ラン永続化のための`SqliteHarnessRecordStore`を追加した。不変のアーティファクトとアーティファクトへのアーティファクト・バインドを追加した。Harness MCP バイナリは、現在、`runs.db`にレコードを格納し、チェックポイントは`checkpoints.db`に残る。
- **AMD OpenMP ターゲット・オフロードの概念実証。** 同一の数値 fan-out ワークロードで、シリアル CPU 実行、OpenMP 自動スレッド化、反復毎の GPU マッピング、永続 GPU データを比較する、オプトインの`bench_openmp_offload`ベンチマークを追加した。実デバイスとホストフォールバック実行、正確性、転送を含むレイテンシ、カーネルのみのレイテンシ、そしてスピードアップをレポートする。`NEOGRAPH_OPENMP_OFFLOAD_ARCH=gfx1201`は、Radeon AI PRO R9700のROCم/Clangデバイスイメージを可能にする。


### 変更

- **C++ ABI および SOVERSION ポリシー (issue #194)。** コンパイルされたすべての公開 `neograph_*` ライブラリは、プロジェクト `VERSION` とメジャー `SOVERSION` を保持します。インストールされた共有ライブラリは、自身のディレクトリから依存関係を解決します。v1 より前のリリースは ABI 世代 0 を使用しますが、必須の再ビルド境界を宣言することがあります。`0.11.1` 以前に対してビルドされたすべての C++ コンシューマは、次のリリースのために再ビルドする必要があります。`NodeCache`、`EngineConfig`、`CompletionParams`、`Agent`、`RequestOptions`、`SseEventParser`、およびプロバイダー構成の公開レイアウトが変更されたためです。境界付き `UsageAccumulator` 予約を含むリリースは、別の必須の再ビルド境界です。その公開オブジェクトのレイアウトには、予約会計状態が含まれるようになりました。バージョン 1.0 は、ABI 世代を 1 に変更し、サポートされる v1 レイアウトを固定します。CI は、静的および共有のインストール済みコンシューマの分離ビルドとテストを実行し、ELF/Mach-O ローダーメタデータをチェックします。[`docs/ABI_POLICY.md`](docs/ABI_POLICY.md) を参照してください。
- **`GraphNode::run(input)` マイグレーションガイド完了。** Python `GraphNode` 基底クラスは削除された `execute*` メソッドを参照しなくなりました。`run(input)` が欠落している場合、マイグレーションドキュメントのパスを含む `NotImplementedError` を送出します。C++/Python リファレンス、async/streaming ガイド、およびサンプル README は、実際の v0.9.0 単一エントリポイントに合わせて調整されています。マイグレーション手順は、[`docs/migration-v0.4-to-v1.0.md`](docs/migration-v0.4-to-v1.0.md) に C++ と Python の例とともに文書化されています。
- **Provider API 恒久互換性ポリシー (issue #5)。** `Provider::complete()`、`complete_async()`、`complete_stream()`、`complete_stream_async()`、およびコールバックベースの`invoke()`の削除予定は撤回され、`[[deprecated]]`警告は削除されました。既存のAPIは引き続き互換性とセキュリティ修正を受けます。新規のProvider実装と直接呼び出し元には、それぞれ`CompletionProvider::do_invoke()` と`invoke_request(CompletionRequest)`の使用が推奨されます。既存APIへの全新機能のバックポートは保証されません。パブリックシグネチャ、仮想順序付け、オブジェクトサイズ、vtableは変更されていません。

### 削除

- **非推奨のTransformerCPP統合例。** 削除されました`example_inproc_gemma`、`NEOGRAPH_BUILD_LOCAL_INFERENCE_EXAMPLE`、および`TRANSFORMERCPP_DIR`。これらは、利用できなくなった外部ホスト型リポジトリに依存していました。`example_local_transformer`は、標準のOpenAI互換ローカルサーバーを使用しており、保持されています。

### 修正済み

- **Python Program wheel の依存関係を完結。** PyPI ビルドは QuickJS 制御ランタイムを有効化し、拡張モジュールの隣に `neograph_program` ローダーを同梱し、Windows の Program/Harness DLL 境界を越えて使用される非公開評価器をエクスポートするようになりました。これにより、`P_JS_UNAVAILABLE`、ローダー不足による import 失敗、および共有ビルドの `LNK2019` 失敗を防ぎます。また、コマンド identity の構築ではコンポーネント dylib 境界を越えるネストした initializer-list JSON コピーを回避し、Program 実行中に macOS arm64 で発生し得たクラッシュを防ぎます。Windows wheel の repair は外部 DLL 名をハッシュでマングルし、先に読み込まれたシステムの `sqlite3.dll` が同梱ビルドを上書きして `SQLiteContextStore` の構築中にクラッシュすることを防ぎます。
- **境界付きのリモート輸送と資格情報の起源。** HTTP/1.1、HTTP/2、SSE、WebSocket の受信は、信頼されないサイズが割り当てられる前に、保守的な応答、ヘッダー、チャンク、ライン、フレーム、ハンドシェイク、メッセージ制限を強制する。リダイレクトされた POST リクエストは、正規化された同一オリジン内でのみフォローされ、プロバイダーの資格情報は、明示的な数値ループバック開発例外が有効でない限りTLS を必要とし、WebSocket デバッグ出力には、リクエストヘッダーやペイロードは含まれません。
- **QuickJS `all` join 起動時レース。** 完了ハンドラは、JavaScript の join を閉じる前に、初期メンバー起動登録を待つようになりました。即座に完了する子は、その兄弟の初期コマンドまたは置換コマンドがディスパッチされる前に、ジェネレーターを再開できなくなりました。繰り返しのランタイムリグレッションは、両方のパスをカバーしています。
- **Harness 集約ファインディングの来歴（issue #174）。** 詳細には、 `finding_sources` 配列が既存のフラットな `findings` 配列に揃えて含まれるようになりました。各エントリは、スキーマ検証済みのワーカー出力や確立された `findings` 形状を変更することなく、その集約インデックス、ソースワーカーID、およびワーカーローカルインデックスを記録します。
- **Harness エクスポート結果 lint (issue #173)。** Node effect 契約では、呼び出し側がグラフ実行後にそれらを消費する場合、オプションの `exports` 配列で書き込みチャネルを宣言できるようになりました。したがって、Harness のコンパイルと `GraphEngine` のランタイム検証の両方が、真に書き込み専用のチャネルに対して E6 を維持し、`final_result` に対して誤って警告することはありません。
- **MCP 2025-11-25 ツールクライアント契約の近代化（issue #147 M0）。** 初期化は現在冪等であり、ネゴシエーションされたサーバーメタデータを保持します；HTTPツールはディスカバリーセッションを再利用します； `/mcp` エンドポイント構築はリクエストと通知で共有されます；ツールディスカバリーは不透明なカーソルに従います；そしてJSON-RPCのcode/data、完全なツールメタデータ、非テキストコンテンツ、 `structuredContent`, `isError`、および `_meta` はC++とPythonのパスで存続します。構成可能なHTTPタイムアウト/静的/動的ヘッダー、出力スキーマ検証、厳格なレスポンスIDチェック、および型付き `InitializeResult`, `ToolDefinition`, `ListToolsPage`、および `CallToolResult` APIsが追加されました。SSE検出は現在、 `Content-Type` を使用し、 `data:` URLを含むJSONを誤分類することはありません。
- **タスクごとのキャンセルステータスと公開emitのライフタイム安全性。** `GraphEngine::run`、`run_async`、`run_stream`、`run_stream_async`はそれぞれ、呼び出し元が提供する親から実行ごとに1つの実行子を作成し、その子のみを内部の`co_spawn`/同期ブリッジにバインドし、同じ子を`RunContext`として渡します。したがって、単一の親の下で並行するすべての実行をキャンセルしても、互いのキャンセルスロットを上書きすることはできません。フォークされた実行子は、既存の`shared_ptr`の所有権を公開されたemitを通じて保持し、エンジン作業完了とemit実行の間でのuse-after-freeを防止します。キャンセルによって引き起こされるasio `operation_aborted`は、再試行可能なノードエラーではなく`CancelledException`として伝播されます。`CancelToken` 0.11.xのオブジェクトレイアウトとインライン/ヘッダー専用の動作は変更されていません。更新された`fork()`のライフタイム動作を反映するには、すでにコンパイルされたC++コンシューマーの再コンパイルが必要です。共有ライブラリのみを置き換えるとオブジェクトレイアウトの互換性は維持されますが、コンシューマーバイナリに埋め込まれた既存のインライン関数本体は変更されません。ただし、外部コードが直接作成したトークンに対して`bind_executor()`を呼び出す場合、エグゼキューターの投稿された作業が完了するまでトークンを生かし続ける責任は呼び出し元に残ります。
- **PostgreSQL非同期接続のグローバルタイムアウトポリシーを文書化。** 非同期の初期接続と置換では、すべてのホスト/IPアドレスに対して単一のタイムアウトが使用されます。正の接続文字列に直接書かれた明示的な `connect_timeout` は、最低2秒で適用が強制されます。未指定、ゼロ、負の値、または環境変数/サービスファイル専用の値は、運用上安全なデフォルト（30秒）が使用されます。これは、libpq のホストごとの同期タイムアウトと意図的に異なりますが、同期の作成/置換動作は変更されません。
- **JARVIS mock ビルド修正（issue #130）。** 修正しました `cookbook_jarvis` のコンパイル失敗。原因は `MicCapture` がオーディオ依存関係が存在しない場合に不完全型のままであることでした。追加しました `NEOGRAPH_JARVIS_FORCE_MOCK` これにより、ASan CI はランナーにインストールされたパッケージに関係なく、常に mock 構成をビルドします。セッションランナーは現在、実際の CMake 出力パスと専門ターゲット名を使用し、既存の `demo_mcp_server.py` を正しく起動します。
- **ノード障害コンテキストの保持（issue #123）。** C++実行エラーは、 `NodeExecutionError` 元の `exception_ptr`、失敗したノード名、および試行回数を含む形で伝播されます。終端の `ERROR` イベントにも同じコンテキストが記録されます。Pythonでは、元の例外オブジェクト、型、引数、ユーザー属性、およびトレースバックがそのまま保持され、追加されるのは `.node_name` と `.attempts` 属性のみです。 `NodeInterrupt`、キャンセル、およびメモリ不足の例外は、ラップされずに既存の制御フローに従います。

### 修正済み（ドキュメント）

- **Providerクックブックから無視されていたノード単位のプロンプトを除去しました（issue #116）。** 以下の使用に関する説明をした3つのPythonサンプルを修正しました： `config.system` 組み込みの `llm_call` で読み取られない複数役割の動作。各サンプルは、 `NodeContext.instructions`を使用した厳密な単一呼び出しグラフとして書き直され、関連するREADMEも実際の動作に合わせて整えられています。
- **予約済み`RunContext::deadline`ドキュメント修正（issue #115）。** `deadline`と`trace_id`を実行ごとのメタデータとして使用可能であると提示していたドキュメントとDoxygenコメントを修正しました。これらは`RunConfig`を介して設定できず、Pythonでは公開されていません。
- **`GraphNode::run`例のシグネチャ修正（issue #129）。** `const NodeInput&`（参照渡し）を受け入れる公開ヘッダーの例を修正しました。これは実際の値渡し仮想関数のオーバーライドに失敗しており、コルーチン引数の有効期間に必要な値渡し契約をコンパイル時テストで固定しました。

### 追加

- **後方互換性のあるProvider移行パス。** 新しい`CompletionRequest`はストリーミングモードをコールバックの存在から分離し、`CompletionProvider`は新しい実装が`do_invoke()`のみを書き込むことを要求します。既存の`Provider`vtable、4つのレガシー仮想関数、コールバックベースの`invoke()`、およびPythonの`complete()`サブクラス契約は保持されます。

- **Python永続化バックエンド**（#117）— `Store`と`CheckpointStore`は現在、C++仮想ディスパッチをPythonに持つ構築可能なサブクラスベースです。`StoreItem`、`CheckpointPhase`、`Checkpoint`、および`PendingWrite`はJSON形式のフィールドで公開されています。チェックポイントの保留中書き込みメソッドはオプションのままです。
- **Python同期キャンセル**（#119）— Python呼び出し元は`CancelToken`を構築し、それを`RunConfig.cancel_token`に割り当て、別のスレッドから`engine.run()`を協調的に停止できます。

- **Pythonチェックポイント履歴**（#118）— `GraphEngine.get_state_history()`は新しい順のチェックポイントレコードを公開するため、呼び出し元は履歴状態からフォークする前に親リンク、メタデータ、ステップ、IDを検査できます。

- **DSLサーフェス（精緻化レイヤー）+ スキーマ進化ゲート**（#75 M4）。
  - **エラボレータ**：`vars`（`{"$var":...}` / `${...}` 補間、非巡回強制）/ `templates`+`use`（パラメータ完全一致の強制、ノードプレフィックスリネーム—ローカル参照、バリア、ルートを含む; チャネルは共有状態であるためグローバルにマージされる）/ `when` 条件付き組み込み。**チューリング完全ではなく全関数である**：すべてのDSLドキュメントは有限時間で一意のCoreに正規化され、そのCoreに関して冪等である。すべてのエラーはDSLソース座標（`use[2].args`、`vars.model`）とともに報告され、ソースマップ（出力位置→生成構文）が含まれる。ロックファイルワークフロー：`./example_elaborate harness.dsl.json > harness.json`（例53）。
  - **`GraphCompiler::upgrade_to_latest()`**: 可逆的なv0→v1機械的変換 — strictが拒否するキーは`x-upgraded-<key>`コメント名前空間に分離され（データ削除ゼロ）、空のバリアは明示的に削除される。コーパス全体がテストされ、「レガシー寛容コンパイルIR == アップグレード後strictコンパイルIR」（正準等価性、バージョンスタンプは除く）が保証される。
  - **スキーマ進化ゲート**：`tests/fixtures/schema_snapshot.json` ベースラインに対する追加のみの部分集合判定（JSON Subschema ファミリの決定可能な部分集合）—ノードタイプ/プロパティ/リデューサー/条件の削除、required 集合の増加、閉条件ラベルの変更、およびエフェクトコントラクトの変更はいずれもテスト失敗 = CI マージブロックを引き起こす。非互換な変更は、同じレビューコミット内でバージョンバンプ + アップグレーダー + スナップショット再生成を強制する。

- **PBT / デルタ検証ハーネス**（#75 M3）。300シードの決定論的トポロジージェネレータ（スキーマエンベロープからの有効な厳格なドキュメント、自己計測された特徴カバレッジ—conditional_edges/barrier/interrupt の出現率が30%を下回るとテストが失敗する：テストされていない特徴は失敗になり、静かな穴にはならない）。
  - **変異検出**: 300シードのコーパスで、変換検証が5種類すべてのドロップ型(conditional_edges/edge/barrier/interrupt/channel)と3種類の誤配線型(ルート崩壊/エッジ再ターゲット/ノード改名=ドロップ+捏造の相殺)の全適用を捕捉することを確認した。適用率の下限(シードの10%)も検証した。
  - **参照インタプリタのデルタ**：コードが分離された実装から文書化されたスーパーステップセマンティクス（goto preemption、バリア蓄積、辞書式フォールバック、暗黙的__end__）を再実装する独立モデルで、12ステップ× 300グラフでスケジューラと比較される（DESILの教訓：単独の検証パイプラインでは誤った実行条件を把握できない）。
  - **Engine ↔ Studio 共有コーパス**: `tests/fixtures/topology_corpus/` 15のバリアント（3つの有効 + E3–E11に違反する12）はNeoGraph-Studioとバイト単位で同一である `tests/corpus/`、両者は同じ判定（code:severity 多重集合）を主張する — 2つの実装は暗黙的に乖離することはできない。

- **GraphValidator — トポロジー静的意味チェック (E3–E11 + 効果)** (#75 M2)。パース (M1) と実行の間のパスレイヤー。厳密なドキュメント (schema_version>=1) では、エラーはコンパイル失敗、警告は stderr の lint です。寛容なドキュメントでは、エラーレベルの診断のみが stderr 警告として表面化します (既存のグラフではノイズゼロ)。判定哲学 = チェッカーの健全性を優先: エンジンセマンティクスで決して正しくなり得ないものだけがエラーです (未解決参照 E3、シグナルパスのないバリア E8 — goto はバリア会計をバイパスするため回復不能、空ルート E10 — ディスパッチは rend() UB を逆参照する、未宣言チャネル書き込み E4 — ランタイムで送出が確認済み)。Command.goto/Send が正当化できるものは警告です (到達可能性 E7、脱出不能サイクル E11、バリアなしのプレーン fan-in E9、上書き競合 E5、デッドチャネル E6)。すべての診断には、マシン可読な証拠 (反例) JSON が付随します — Studio キャンバスのハイライト用 (M3)。
  - **ルート完全性（E10）**: `ConditionSpec` ラベルコントラクトを導入。`register_condition` の3引数オーバーロードを介して条件の出力ラベルセットを宣言する場合、クローズド条件ルートがラベルと正確に一致することが要求される — カバーされていないラベルはスケジューラの「辞書式順序最後のルート」フォールバック（順序依存の任意ターゲット）に落ち、これはエラーである。組み込みの `has_tool_calls` = クローズド {false,true}、`route_channel` = オープン + 既知の {default}。
  - **チャネル効果契約**: `register_type` の4引数オーバーロードは、ノードタイプごとの読み取り/書き込みチャネルを宣言する。E4/E5/E6分析は、グラフ内の**すべての**ノードタイプが宣言されている場合にのみ有効化される（単一の不明なタイプで分析全体がスキップされる — カバレッジに対する健全性）。組み込みの3タイプ（llm_call/tool_dispatch/intent_classifier）は完全に宣言されている。
  - `node_effects` · `condition_specs` を `export_schema()` に追加（既存の `conditions` 配列は後方互換用に維持）。新規テスト22件。

- **トポロジーコンパイル時整合性ゲート — 消費キー会計 + 翻訳検証** (#75 M1)。「サイレントな意味的損失」クラス (v0.1.0–v0.1.7 `conditional_edges` サイレントドロップと同種) を構造的にブロックする二重メカニズム:
  - **消費キー会計（Consumed-key accounting）**: 以下の文書で宣言されている `"schema_version": 1` は厳密コンパイルに切り替わる — 未消費キー（タイポ `conditionnal_edges`、未対応フィールド、空の `wait_for`によって静かに破棄されるバリア、インライン条件分岐で無視される `to` ）はすべて収集され、コンパイルエラーとして報告される。マーク付けはパースブロックの**内部**で行われるため、パース段階を消去するとマークも消去され、それらの機能を使用する厳密なドキュメントは直ちに失敗する — ドロップ回退（回退 regressions）が決して静かにならない構造である。 `_`/`x-` 接頭辞を持つキー（`_comment`, `x-studio-*`）は常にコメント名前空間として許可される。既存のドキュメント without `schema_version` は寛容な動作（バイト保存）を維持し続ける。
  - **翻訳検証**: `CompiledGraph::to_json()` 再出力 + `GraphCompiler::canon()` 正規形チェック `canon(input) ==
    canon(re-emit)` をすべてのコンパイルで実施。不一致 (= コンパイラが何かを落とした、または誤配線した) は、厳密なドキュメントでは throw、寛容なドキュメントでは stderr 警告になります。等価性は構造比較です — ルートキーの入れ替えのような誤配線も検出されます (存在比較では見逃されるクラス)。
  - `NodeFactory::config_schema(type)` query が追加され、 `schema_version` フィールドが `export_schema()`に文書化されました。27件の新しいテスト (`tests/test_compiler_strict.cpp`) — v0.1.x ドロップミュータントシミュレーション (conditional_edges/barrier/interrupt のドロップ)
    + route miswirings) included.

## [0.11.1] - 2026-06-25

### 変更

- **stdio MCP並行呼び出し — I/Oオーバーラップ用の相関IDデマルチプレクサ。** `0.11.0`の並行ツールディスパッチは、実際にはHTTP MCPのみがオーバーラップしていた。stdio MCPは`StdioSession::rpc_call_async`において、**リクエスト→レスポンスの往復全体**にわたって容量1のチャネルロックを保持しており、単一のセッションパイプを通じて1ターン内の複数呼び出しを直列化していた(ウォールタイム≈レイテンシの合計)。単一パイプが根本原因ではなかった — JSON-RPC `id`は、まさに1つの接続上でパイプライン処理を行うために存在する。ロックを相関IDデマルチプレクサに置き換えた:
  - 容量1のチャネルを write-only ロックとして再利用 — フレーム書き込みの瞬間のみ保持されるため、2つの呼び出しのバイトは絶対に interleave せず、読み取りはもはや serial 化されない。
  - 単一のリーダーコルーチン(`run_reader`)が読み取り側を排他的に所有し、JSON-RPC `id`を介して各レスポンス行を正しい呼び出し元のシンクに配信する。N個の並行呼び出しは読み取りをオーバーラップさせるため、ウォールタイム≈最大(レイテンシ)となる — ただし、**ピアMCPサーバーが並行処理する場合のみ**(シングルスレッドの逐次サーバーはAmdahlの限界に達する)。
  - リーダーは、実行中の呼び出しが存在する間のみ遅延実行され、待機者がいなくなると終了するため、プライベートな`run_sync` io_contextは正常に戻る。待機者は、呼び出し元が待機している間のみ存在し、`MCPTool`の`shared_ptr`を介してセッションを維持するため、リーダーが破棄されたセッションに触れることはない(デストラクタのjoinは不要)。パイプのEOF/エラー時、リーダーはすべてのシンクを閉じるため、待機中の呼び出し元は無限にハングする代わりに例外を受け取る。
  - **API/構文の変更なし** — 公開ヘッダーは変更なし、既存コードの再コンパイルは不要。エンジンオーバーヘッドの回帰は0(`bench_neograph`のインターリーブA/B、seq/par Δ 0%)。
  - テスト: スレッドベースの遅延フィクスチャ`tests/fixtures/mcp_stdio_slow.py` + `ConcurrentStdioCallsOverlapIO`(5×100 msの呼び出しが約130 msで完了、500 msの直列下限に対して; 各レスポンスが`id`を介して呼び出し元にルーティングされることを検証)。ASan+UBSan ×3クリーン。

## [0.11.0] - 2026-06-25

### 追加

- **並行ツールディスパッチ — `Tool::execute_async` 公式非同期パス。** `ToolDispatchNode` は単一のアシスタントターンから複数の `tool_call`をエンジンの `make_parallel_group`を使用して**並行して**実行します。以前は各呼び出しが同期 `execute()`を介して順次実行され、MCPツールは特に、呼び出しごとに `io_context` を `run_sync` 経由で生成する際にブロックされ、並行MCP呼び出しの重複を妨げていました（並行MCP呼び出しを持つ外部C++フォークで発見）。修正:
  - 仮想`execute_async()`を`Tool`に追加 — デフォルト実装は同期`execute()`にブリッジするため、既存ツールは変更なしで動作する。
  - `MCPTool`を`AsyncTool`に変換し、ネイティブ`execute_async`を使用(stdioは`rpc_call_async`、HTTPは新しい`MCPClient::initialize_async`/`call_tool_async`を非同期ハンドシェイクに使用 — `run_sync`は削除)。
  - `ToolDispatchNode::run`は、ノードfan-outと同じ`make_parallel_group`イディオムを介して呼び出しを並行してディスパッチし(単一呼び出しはインライン化)、結果は呼び出し順に適用される。同期`execute()`ファサードを介して後方互換。
  - 検証: 478/478 ctest、Valgrind 0 リーク、TSAN 0 競合。

### 修正済み

- **Python非同期実行の例外保持(issue #122)。** `run_async`、`run_stream_async`、および`resume_async`が、元のPythonノード例外を文字列としてラップする新しい`RuntimeError`で上書きする問題を修正。現在、元のPython例外オブジェクト、型、ユーザー属性、トレースバックはpybind11の標準例外変換パスを通じて保持され、C++ `py::type_error`は同期実行と一致するPython `TypeError`として配信される。`resume_async`の空のコールバックは、コルーチンが完了するまで保持され、pybind11 3.xで露呈したダングリング参照の競合も修正。

### 修正済み（ドキュメント）

- **README 概要バッジを、サンドボックス測定で明らかになった欠落条件と内部矛盾について修正しました。**「4つの軸」の概要表バッジは、本文/詳細から測定条件が取り除かれ、誇張して読めていました。本文の測定値と条件に一致するように修正しました（測定データ表自体は変更なし）：
  - **`p99 17 µs flat` → `p99 7 µs @ 10 K (1 CPU sandbox)`** — バッジの17 µsは本文(`At N=10,000 concurrent ... 7 µs p99`)と矛盾し、`flat`はGPU負荷テストの実行レイテンシ(648 ms)を説明しており、µs測定ではなかった。バッジを本文の測定値と条件に合わせた。
  - **`1.2 MB stripped binary` → `... (MinSizeRel static)`** — `libc.so.6`専用で1.2 MBという条件は、MinSizeRel + 静的libstdc++ビルドにのみ当てはまる（デフォルトのReleaseはlibstdc++/libgcc_s/libm/libcを動的リンクする）。deep-dive §sizeに既に記載されている条件をバッジに復元した。
  - **`2 wheel deps` → `2 direct wheel deps (... ; 7 with transitive)`** — 直接依存は確かに`certifi` + `pydantic`（2つ）だが、実際のインストールツリーはpydanticの推移的依存（pydantic-core、typing-extensions、annotated-types、typing-inspection）を含む7パッケージである。
- **deep-diveのMinSizeRel再現コマンドに`-DNEOGRAPH_BUILD_POSTGRES=OFF`を追加。** PostgreSQLはデフォルトでONのため、libpqを持たないホストではそのまま実行するとconfigureが失敗する。修正した。

## [0.10.0] — 2026-05-20

### 追加

- **シリアルfan-outのワンショットstderr警告（issue #62、PR #63）。** `compile()`のデフォルトは`set_worker_count(1)` — fan-outブランチはエンジン所有のスレッドプールなしで呼び出し側のexecutor上でシリアルに実行される。この意図された動作は、ドキュメントのみに基づいてマルチSendグラフを構築したユーザーには無言のシリアル実行に見える。`NodeExecutor`がプールなしでマルチSend（またはマルチ出力エッジ）fan-outをディスパッチする最初の回に、ワンショットのガイダンスメッセージをstderrに追加した。`std::atomic` + compare-exchangeにより、並行fan-out下でも正確に1回の出力が保証される。`set_worker_count(N>=2)`を呼び出すと`NodeExecutor`が再構築され、フラグは自然にリセットされる。環境変数`NEOGRAPH_SUPPRESS_FANOUT_WARNING=1`（または`true` / `yes`）で抑制可能 — 意図的なworker=1シリアル実行、ベンチマーク、CIのstderrアサーションケース向け。Linux + macOSの5つのユニットテスト（`test_fanout_worker_warning.py`）でカバー：発火 / ワンショット / プールオプトインの沈黙 / 環境変数の沈黙 / シングルSendの警告なし。Windows：pytest capfdはホイールバイナリのMSVC CRT fdキャッシュと非互換のためモジュールレベルでスキップ — ホイールバイナリのstderr出力自体は正常である。

- **トポロジーJSON Schemaエクスポート — `NodeFactory::export_schema()`**（issue #56、コード不要のビジュアルブロックエディタの前提条件）。エンジンが消費するトポロジーJSON形式を、機械可読なスキーマ（JSON Schema Draft 2020-12）として1つの`{ neograph_version, $schema, topology (fixed
  envelope), node_types, reducers, conditions }`にエクスポートする。別リポジトリのブロックエディタはこのスキーマからパレットを自動生成する → エディタとエンジンはバージョン間でドリフトできない。完全に追加的：
    - `NodeFactory::register_type(type, fn, json config_schema)` 3-argument
      variant added. Existing 2-argument delegates to permissive default
      schema — existing user nodes/calls unaffected.
    - `ReducerRegistry::names()` / `ConditionRegistry::names()` /
      `NodeFactory::registered_types()` query accessors added.
    - Configuration schema declared for 4 built-in types (`llm_call`/
      `tool_dispatch`/`intent_classifier`/`subgraph`). `NEOGRAPH_VERSION`
      exposed as a compile definition (pyproject.toml single source of truth)
      → schema version stamp.
    - `examples/52_export_schema.cpp` (`example_export_schema`):
      `./example_export_schema > schema.json` — standard path for the editor
      repo CI to produce the artifact pinned to a NeoGraph version.
    - Python: `neograph_engine.export_schema()` → dict (editor repo CI
      dumps after `pip install neograph-engine`).
    - `tests/test_schema_export.cpp` 8 + `test_export_schema.py` 4. Key:
      top-level `conditional_edges` surviving the loader→compile round-trip
      (regression guard against v0.1.0–v0.1.7 silent-drop recurrence).

### 修正済み

- **トポロジー最上位コンテナ形式の検証（#126）。** `channels`/`nodes`はオブジェクトでなければならない。そうでない場合は全モードで拒否される。`edges`/`conditional_edges`の配列検証はstrictモードで強制され、レガシーのキー付きエッジマップ互換性は維持される。エラーはパスとJSON型を報告し、入力全体は報告しない。
- **`max_steps`終了状態の公開（#114）。** `RunResult::max_steps_exhausted()`と読み取り専用のPythonプロパティ`RunResult.max_steps_exhausted`を追加。`max_steps`に到達し、実行すべきノードが残っている場合にのみTrueになる。同じ状態がgRPCの単一応答とストリーミング最終JSONの両方で提供される。C++構造体のサイズは変更なし。

- **`set_worker_count` / `set_worker_count_auto`のdocstring修正（issue #62、PR #63）。** v1.0準備サイクルは意図的に`compile()`のworkerプールデフォルトを`set_worker_count(hardware_concurrency())`から`set_worker_count(1)`に戻した（`src/core/graph_engine.cpp:69-93`のコメントに根拠あり）が、4つのユーザー向けdocstringには古い記述が残っていた → ドキュメントを信じてマルチSend fan-outグラフを構築したユーザーは、単一スレッドで無言のシリアル実行を得た。ユニットテストでは見えない（モックのspawn、即時ボディ）。実時間のe2eでのみ露呈する。
  - `set_worker_count` / `set_worker_count_auto`の両方のPython docstringを`bindings/python/src/bind_graph.cpp`で実際の動作に合わせて書き直した：`compile()`のデフォルトは1、`set_worker_count_auto()` / `set_worker_count(N>=2)`は明示的なオプトインである。
  - `include/neograph/graph/engine.h`の両方のDoxygenコメントをそれに合わせて修正した。Doxygen Pagesはmasterへのpush時に自動再構築される。
  - `docs/concepts.md` / `docs/troubleshooting.md` / `docs/reference-en.md`の同じ古い記述（デフォルト = hardware_concurrency）を修正した。

- **v0.9.0から欠落していた3つのAPI移行を補足して出荷。** v1.0準備サイクルのPR `9b`（`19819d8`）は`GraphNode`のレガシー8仮想チェーンを破壊的に削除したが、PR `#48`（`6e654ad`、「C++ examples migrate to `GraphNode::run()`」）は`examples/`のみを移行した — 以下の3ファイルが漏れ、v0.9.0がビルド破損状態で出荷された：
    - `benchmarks/stress/bench_sustained_concurrent.cpp` (Phase 3
      sustained-burst verification key benchmark)
    - `benchmarks/concurrent/bench_concurrent_neograph.cpp` (memory/
      concurrency comparison matrix body against LangGraph and other engines)
    - `wasm/smoke.cpp` (Phase 1 WASM feasibility smoke)

CIは、これらのターゲットをadd_executablesとして検出せず、(Dockerビルド依存関係により)別の環境に分離したため、masterへのマージとタグ付けは通過した。

**修正**: 3つすべてが `std::vector<ChannelWrite> execute(const
  GraphState&) override` → `asio::awaitable<NodeOutput> run(NodeInput in)
  override` + `co_return out` パターンから移行。ノードロジックは変更なし。

**v1.0 主要セールスポイントのネイティブ再検証** (`benchmarks/concurrent/results_v0.9.0_native_recheck.jsonl`):
    - Concurrency 10K · wall 10–23 ms · p99 17–21 µs · peak RSS **5.6 MB**
      (matches v0.3.0 / v0.5.0 measurements — no memory selling-point
      regression after destructive 9b)
    - 0 errors at 10K
  **Docker matrix (LangGraph / Haystack / pydantic-graph / LlamaIndex /
  AutoGen 6-way comparison) also re-measured within the same session**
  (`results_v0.9.0_docker_recheck.jsonl`).

マトリックス再実行中に、欠落していた API 移行と並行して独立した回帰が1件発見された — `benchmarks/concurrent/Dockerfile.neograph` は master 上の CMake オプションのデフォルト変更を追跡できなかったため、まったくビルドできなかった（v0.9.0 出荷時も同様）。時間の経過とともに、以下のオプションのデフォルトが OFF → ON に反転した:
    - `NEOGRAPH_BUILD_POSTGRES` / `NEOGRAPH_BUILD_SQLITE`
      (requiring `libpq-dev` / `libsqlite3-dev` respectively)
    - `NEOGRAPH_BUILD_A2A` / `NEOGRAPH_BUILD_ACP`
    - `NEOGRAPH_USE_LIBCURL` (one prior incident closed in
      public build guidance — only the option toggle was
      added while the default remained ON, breaking the empty-container build
      path again)
    - `find_package(OpenSSL REQUIRED)` is unconditional without an option
      toggle (CMakeLists.txt:256) — separate v1.0 cleanup candidate

**Dockerfile 修正**: `libssl-dev` apt 追加 + すべての非 Core オプションを明示的な `-DNEOGRAPH_BUILD_*=OFF` / `-DNEOGRAPH_USE_LIBCURL=OFF` で固定。コメントには「2件のドリフト事故による明示的な凍結」と記載。`find_package(OpenSSL REQUIRED)` の CMakeLists.txt での条件化は別タスクとして残す — 他のビルドパス（PyPI wheel、ARM64 など）への影響検証が必要。

**6方向マトリックス主要結果** (並行度=10000, 2 CPU / 1 GiB):

    | engine（エンジン）          | モード          | wall_ms | p99_us      | peak_MB | ok/err |
  |---|---|---|---|---|---|
    | **neograph**    | スレッドプール    | **16**  | **18**      | **5.1** | 10000/0 |
    | pydantic-graph  | asyncio       | 895     | 160         | 42.8    | 10000/0 |
    | haystack        | mp-pool-8     | 1472    | 2972        | 68.3    | 10000/0 |
    | langgraph       | mp-pool-8     | 3802    | 74415       | 60.6    | 10000/0 |
    | autogen         | mp-pool-8     | 22428   | 82361       | 49.1    | 10000/0 |
    | LlamaIndex      | asyncio       | 26303   | 25912204    | 582.7   | 10000/0 |

NG vs LangGraph（マーケティング比較軸): **237倍高速**、p99 **4134倍高速**、ピークRSS **12倍低減**。

**過酷なシナリオ**(並行度=10000、1 CPU / 512 MiB):
    - NG: 8 ms / 5.2 MB / 0 err / **ok**
    - LangGraph mp-pool-8: 7821 ms / 60.9 MB / 0 err / ok
    - **LlamaIndex asyncio: OOM killed** (exceeded 512 MB cap)
    - **AutoGen asyncio: OOM killed**

Token: ⟦c39661f30e9e⟧ 同じv0.3.0/v0.5.0の測定値 — **破壊的な9bの後も、NeoGraphの「10K同時ワーカー、ピークRSS 5 MB、OOMなし」という売りポイントにリグレッションはありません。**

## [0.9.0] — 2026-05-14 — v1.0 準備 (Candidate 1 Phase B + Candidate 6)

2つのv1.0単一ディスパッチ統合が1つのサイクルで収束する：

  - **候補 1 フェーズ B (`9b`–`9f`)** — `GraphNode` のレガシー 8 仮想関数 (`execute` / `execute_async` / `execute_stream` / `execute_stream_async` / `execute_full` / `execute_full_async` / `execute_full_stream` / `execute_full_stream_async`) + `add_cancel_hook` + `CurrentCancelTokenScope` + `state.
    run_cancel_token_` + すべての 6 つの `PyGraphNodeOwner` レガシーオーバーライドが削除された。**破壊的** — 非推奨期間は終了。ユーザーの GraphNode サブクラス / ユーザー Python ノードは、単一メソッド `run(NodeInput)` / `def run(self, input)` に移行する必要がある。
  - **候補 6** — `Provider` 4仮想関数のクロス積 → 1仮想関数 `invoke()`。まだ追加 + 非推奨フェーズ中 — レガシー 4 仮想関数は変更されず機能し、非推奨警告のみ表示される。その側のフェーズ B (`Provider` レガシー削除) も v1.0.0 出荷直前に終了する。

同じサイクルには、b59444f の並行回帰リバートの可能性 (`e5ecb08`) + 明示的な fan-out の例呼び出し + 3 件の CI 環境修正 (httplib マクロガード / Windows MSVC unistd.h / pybind pytest 移行) も含まれ、すべてこの [Unreleased] の一部である。

### 追加

- **`Provider::invoke(params, on_chunk = nullptr)`** — v1.0 標準の単一ディスパッチエントリポイント。非ストリーミング (`on_chunk == nullptr`) とストリーミング (`on_chunk` 提供時) の両方を 1 つのメソッドで処理する。以前の 4 仮想関数のクロス積 (`complete` / `complete_async` / `complete_stream` / `complete_stream_async`) を 1 つの非同期ストリーミングスーパーセットに統合する。デフォルト実装は 4 つのレガシー仮想関数に転送するため、既存の Provider サブクラスは変更なしで動作する。新しい ctest 6 件 (`ProviderInvokeDefault`)。(PR #40)
- **`invoke()` キャンセル伝播のパリティ** — `params.cancel_token` が設定されておらず、エンジンのスレッドローカルスコープがアクティブな場合、`current_cancel_token()` が自動的にスタンプされる。レガシー同期 `complete()` の動作と同等 (エンジン内のノード本体が `provider->invoke(params, ...)` を呼び出すと、実行中のグラフのキャンセル信号を自動的に受信する)。新しい ctest 3 件 (`InvokeCancelPropagation`)。(PR #43)
### 変更

- **エンジン内のすべての内部 LLM 呼び出しが `invoke()` 経由にルーティング** — `LLMCallNode`、`IntentClassifierNode` (PR #41/#42)、`Agent::complete` / `Agent::run_stream` (PR #43)、`SupervisorLLMNode` / `ResearcherLLMNode` / `CompressNotesNode` / `FinalReportNode` (PR #43)、`PlannerNode` / `ExecutorNode` (PR #44)。NeoGraph 内の LLM ディスパッチが単一のサーフェスに統合された。
- **C++ 例の移行 (2 ファイル)** — `31_local_transformer.cpp`、`cookbook/ai-assembly/member_server.cpp` は新しい `invoke()` を使用するようになった。ユーザービルドで非推奨警告は発生しない。(PR #45)
- **`GraphEngine::compile()` デフォルトワーカー数を1に戻した** (`e5ecb08`)。`b59444f` は、潜在的な18日間（2026-04-26 → 2026-05-13）の並列マイクロベンチ回帰 11.8 → 283 µs（24倍）の根本原因であり、コミットは二分探索（11のワークツリーを並列使用）で特定された。v1.0 からデフォルト=1（CPU負荷の小さい逐次/並列ディスパッチに最適）。意図的な fan-out には、`engine->set_worker_count_auto()` に1行追加して hardware_concurrency を開放する。影響を受ける5つの fan-out サンプル（10/14/21/36 + deep_research_graph ビルダー）に明示的な呼び出しを追加した。

### 非推奨

- **`Provider::complete` / `complete_async` / `complete_stream` / `complete_stream_async`** — 4つのレガシー仮想関数すべてに `[[deprecated("v1.0 single-dispatch: use invoke(...)")]]` マーカーが付いている。レガシーメソッドは非推奨期間中はそのまま機能する。v1.0.0 で削除。内部フォワーダーは `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` でラップされ、警告はユーザー向けのオーバーライド/呼び出しサイトでのみ表示される。(PR #44)

### 削除済み (候補1 フェーズB — 破壊的)

- **`GraphNode` レガシー仮想関数8つ** — `execute(GraphState&)` / `execute_full(...)` / 6つのバリアント + `ExecuteDefaultGuard` 再帰ガード
  + デフォルトチェーン300行以上。すべて削除。`run(NodeInput)` のみが唯一の純粋仮想関数。(コミット `19819d8`)
- **`add_cancel_hook` + `Hook` RAII + `hooks_*` メンバー + `cancel()` フック反復** — `cancel.h` は `fork()` + `cancel()` + `is_cancelled()` + `slot()` のみを保持。(コミット `1d786a5`)
- **`CurrentCancelTokenScope` + `current_cancel_token()` thread_local + `GraphState::run_cancel_token_` + 3つのアクセサ** — `RunContext::cancel_token` が唯一のキャンセルチャネル。`src/core/cancel.cpp` はスタブまで縮小（ファイル自体が将来の削除候補）。(コミット `9e8e956`)
- **6つの `PyGraphNodeOwner` レガシーオーバーライド** — pybind トランポリンは `run(self, input)` のみを呼び出す。Python ユーザーコードも v0.9.0 から単一メソッドを必要とする。(コミット `9e8e956`)
- **2つの廃止された pytest ファイル** — `test_execute_stream_dispatch.py` (v0.3.2 ストリーム専用フォールバックディスパッチ検証) + `test_streaming_only_error_
  hint.py` (execute_full_stream が優先される — v1.0 では無意味)。(コミット `4392fbb`)

### 修正済み

- **5つの fan-out サンプルに明示的な呼び出しを追加** — `e5ecb08` のデフォルトワーカー数戻しで埋もれていた真の並列意図を復元: `examples/10_send_command.cpp`、`examples/14_plan_executor.cpp`、`examples/21_mcp_fanout.cpp`、`examples/36_classifier_fanout.cpp`、`src/core/deep_research_graph.cpp` の `create_deep_research_graph()` ビルダーが `set_worker_count_auto()` を呼び出すようになった。検証: `classifier_fanout` 4.22倍の高速化 (25.2 ms 逐次 → 6.0 ms 並列)。(コミット `99c470b`)
- **`bench_async_http` httplib マクロガード** — `bench_async_http.cpp` は `<httplib.h>` を `<neograph/async/conn_pool.h>` 経由でインクルードするが、`CPPHTTPLIB_OPENSSL_SUPPORT` が未定義で、ODR ガードが拒否していた。CMake ターゲットに `target_compile_definitions(... PRIVATE ...)` を追加。(コミット `d4be42a`)
- **Windows MSVC `unistd.h` が欠落** — `test_schema_provider_extra_
  fields_temperature.cpp` は POSIX 専用の `mkstemps` + `close` を使用しており、Windows ビルド全体が失敗していた。ファイル全体を `#ifndef _WIN32` ガードでラップした (カバレッジは Linux/macOS で保証)。(コミット `3c49f12`)
- **Pythonテスト16件を移行** — wheel CIのpytestが `AttributeError` を28ノードクラスでレガシー `def execute(self, state)` パターンに遭遇しました。バッチ移行を `def run(self, input)`に実施。ストリーミングノードには `input.stream_cb` Noneガードを追加。(コミット `4392fbb`)

### 移行（ユーザーコード）

**プロバイダー呼び出し（候補6 — 非推奨フェーズ）**

新しいコード:
```cpp
// non-streaming
auto completion = co_await provider->invoke(params, nullptr);

// streaming
auto completion = co_await provider->invoke(params, on_chunk);

// sync site (replaces old complete())
auto completion = neograph::async::run_sync(provider->invoke(params, nullptr));
```

4つのレガシー仮想オーバーライドは非推奨期間を通じて引き続き動作しますが、ユーザーのオーバーライド箇所では`-Wdeprecated-declarations`警告が表示されます。削除はv1.0.0の直前に発生します。非推奨期間内での移行が推奨されます。

**`GraphNode`サブクラス（候補1フェーズB — 破壊的）**

C++コード：
```cpp
// old (up to v0.8.x)
class MyNode : public GraphNode {
    NodeResult execute_full(const GraphState& state) override {
        auto x = state.get("x");
        NodeResult out;
        out.writes.push_back(ChannelWrite{"y", json(/*...*/)});
        return out;
    }
};

// v0.9.0+ current code (single method, coroutine entry)
class MyNode : public GraphNode {
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto x = in.state.get("x");
        // in.ctx.cancel_token / in.ctx.step / in.stream_cb also accessible
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"y", json(/*...*/)});
        co_return out;
    }
};
```

Pythonコード：
```python
# old (up to v0.8.x)
class MyNode(neograph_engine.GraphNode):
    def execute(self, state):
        x = state.get("x") or 0
        return [neograph_engine.ChannelWrite("y", x * 2)]

# v0.9.0+ current code
class MyNode(neograph_engine.GraphNode):
    def run(self, input):
        state = input.state  # input.ctx.cancel_token / input.stream_cb etc. also accessible
        x = state.get("x") or 0
        return [neograph_engine.ChannelWrite("y", x * 2)]
```

**fan-out意図（ワーカー数のデフォルト変更）**

```cpp
// old (v0.x April+): default was hardware_concurrency but micro-bench burden
// v1.0: default=1. Add one line for intentional fan-out.
auto engine = GraphEngine::compile(def, ctx);
engine->set_worker_count_auto();  // ← this line added (hardware_concurrency)
// or engine->set_worker_count(N);  // explicit N
```

`docs/migration-v0.4-to-v1.0.md`内の移行1/2/3セクション（run() / ctx.cancel_token / ワーカー数デフォルト）+ Providerセクション（次のドキュメント更新で追加予定）は、ケースバイケースの移行前/移行後ガイダンスを提供します。

## [0.8.0] — 2026-05-13 — DX ポリシー + ダウンストリーム主導のAPIギャップに対処しました。

実際のダウンストリーム（ProjectDatePop）からのフィードバックと内部カバレッジ差分によって明らかになった8件の問題（#22、#25、#26、#27、#28、#34、#35 + #16のフォローアップ）を、単一のマイナーバンプにまとめます。2つの新しい公開ヘルパー（`RunResult::channel<T>`、`RunContext::store`）、11の新しいオフライン例、`docs/migration-v0.4-to-v1.0.md`移行ガイド、および新規ユーザーが最初の30分で遭遇する摩擦を軽減する5項目のDXバンドルが含まれます。

### 追加

- **`RunResult::channel<T>(name)` / `channel_raw(name)` / `has_channel(name)`** — 結果からチャネル値を抽出するワンライナーヘルパー。両方の出力形状（ネストされた`output["channels"][name]["value"]`標準 + `react_graph`などのビルダーによって追加されるフラットキー）が自動的に処理されます。新しいctestが9件追加されました。（Issue #25）
- **`RunContext::store`** — ノード本体が1行の`in.ctx.store->get(ns, key)`でStoreに到達します。従来のパターン（`shared_ptr<Store>`を`NodeFactory`ラムダでキャプチャする）は引き続き動作します — 新しいコードは新しい形状のみを必要とします。新しいctestが3件追加されました。（Issue #27）
- **`Provider::complete_stream`非純粋デフォルト本体** — 最小限のモック/テストフィクスチャは`complete()`のみをオーバーライドする必要があります。既存のストリーミングネイティブオーバーライドは変更されません。新しいctestが2件追加されました。（Issue #22）
- **`neograph::json`配列 `.front()` / `.back()`** — nlohmannの筋記憶パターン（`msgs.back()["content"]`）がコンパイルされるようになりました。新しいctestが4件追加されました。（Issue #26）
- **11件の新しいオフライン例（41-51）** — `resume_if_exists_chat`、`custom_reducer_condition`、`store_personalization`、`request_queue_backpressure`、`cancel_token`、`node_cache`、`sqlite_checkpoint`、`openinference`、`async_tool`、`minimal`。すべてrc=0、APIキー/外部サービス依存なし。以前は参照がゼロだった27/53の`NEOGRAPH_API`クラス間のギャップを埋めます。
- **`examples/51_minimal.cpp`** — 1つのノード、LLMなし、ツールなし、モックプロバイダーなしの30行の入門例。NeoGraphの実行方法を5分以内で理解できます。
- **`docs/migration-v0.4-to-v1.0.md`** — `[[deprecated]]` の古い8仮想チェーン (`execute` / `execute_async` / など) から新しい `run(NodeInput) ->
  awaitable<NodeOutput>` への移行に関する、ケースバイケースの前後4例と一般的な間違い。`NEOGRAPH_DEPRECATED_VIRTUAL` マクロメッセージからもリンクされています。
- **README「Common pitfalls 5」セクション** — 新規ユーザーが最初の30分で遭遇する5つの事柄（`channel<T>`の使用法、`in.ctx.store`、`neograph::graph::`サブ名前空間、`<httplib.h>`マクロ、GCC 13コルーチンICE）を1か所にまとめています。各項目には修正方法と関連する例/問題へのリンクがあります。
- **コンパイル時 `#error` ガード (`include/neograph/api.h`)** — ユーザーのTUが `<httplib.h>` をNeoGraphヘッダーの前に `CPPHTTPLIB_OPENSSL_SUPPORT`なしでインクルードすると、コンパイルは明確なメッセージとオプトアウトマクロ (`NEOGRAPH_SKIP_HTTPLIB_MACRO_GUARD`) で失敗します。旧#16のランタイムSEGVをコンパイル時失敗に昇格させます。
- **`example_minimal` 5つの新しい親切なエラーメッセージctest** — `Unknown reducer` / `Unknown condition` / `Unknown node type` / `Write to
  unknown channel`メッセージに対する契約ロック。メッセージ本文に利用可能な名前、登録方法、トラブルシューティングリンクを埋め込みます。
- **`docs/troubleshooting.md` 4つの新しいエントリ** — Tracerアダプタ`close()`のハング/クラッシュ（#24）、GCC 13コルーチンICE（#23）、親切なエラーメッセージガイダンス（#22）、`RunResult::output`の形状（#25）。
- **`Tracer` + `OpenInferenceTracerSession::close()` `@warning`ブロック** — アダプタ作成者向けの生ポインタの落とし穴を明示的に文書化します。`RecordedSpan` + ラッパー分離パターンを正しいアプローチとして指摘します。既存の`tests/test_openinference_cpp.cpp::InMemoryTracer` + 新しい`examples/49_openinference.cpp::PrintTracer`を参照します。（Issue #24）

### 修正済み

- **`SchemaProvider::build_body` サイレントドロップ `extra_fields` が `params.tools` が空の場合。** 旧コードは `extra_fields` の適用を `if (!params.tools.empty())`内でゲートしており、 `reasoning` や `response_format` のようなコアスキーマフィールドが、ツールなしの呼び出しから完全に消えていました。修正: ツールブランチの外に移動し、常に適用されるようにしました。新しいctestを3件追加。(Issue #34)
- **`temperature_path`スキーマ側のオプトアウト。** 推論モデル（gpt-5.x、oシリーズ）は`temperature`と`reasoning.effort`が相互排他的ですが、スキーマには「このプロバイダーはtemperatureを受け付けない」と宣言する方法がなく、毎回の呼び出しで`params.temperature = -1.0f`センチネル回避策を強制していました。**修正：** スキーマで`"temperature_path": null`を指定すると、build_bodyがそれを完全にスキップします。新しいctestを4つ追加。（Issue #35）
- **親切なRuntimeErrorメッセージ** — `ReducerRegistry::get`/`ConditionRegistry::get`/`NodeFactory::create`「不明な<thing>: foo」および`GraphState::write`/`apply_writes``Write to unknown channel`が、メッセージ本文に利用可能な名前+登録方法+トラブルシューティングリンクを埋め込むようになりました。新規参入者はメッセージだけで次のステップを判断できます。
- **`SchemaProvider::complete_stream_async` HTTP/SSEブランチ** は、長寿命の専用`bridge_thread_`でディスパッチするようになりました（旧：`Provider`ベースのデフォルトが呼び出しごとに新しい`std::thread`を生成）。旧動作は、コールドのスレッドローカルリゾルバ/NSS状態でglibc`internal_strlen`のSEGVを引き起こしました。WSブランチはすでにネイティブのco_awaitなので影響を受けません。トークンのディスパッチはawaiterのexecutor上で維持されます（PR #10の不変条件）。（Issue #16）
- **`example/09_all_features.cpp`** ストアデモ — ノードボディ読み取りパターンについて`examples/43_store_personalization.cpp`を指すdocstringポインタを追加。オプション2 — オプション3（インラインライブノード）は、#27の`RunContext::store`が着地したら一緒にクリーンアップします。（Issue #28）

### ドキュメント

- `RunResult::output`（チャネルラップ）の正規の形状と、`react_graph`のようなビルダーが追加するフラットキープロジェクションとの関係をヘッダーのdocstringに文書化。新しいヘルパー（`channel<T>`/`channel_raw`/`has_channel`）の使用を推奨。（Issue #25）
- `RunContext::store`フィールド`@brief`ブロック — 2つの配管パターン（`in.ctx.store`推奨/旧ファクトリクロージャキャプチャ互換）をコード例を並べて提示。（Issue #27）
- 両方のパスを`examples/43_store_personalization.cpp`ファイルヘッダーコメントに文書化。

## [0.7.0] — 2026-05-11 — C++ openinference + 非同期ストリーミングブリッジ

v0.6.0に対して提出された4つの問題を1つのマイナーバンプでクローズします。見出し: `Provider::complete_stream_async`デフォルトは、外部エンジンコルーチン内から待機されたときにセグフォルトしなくなりました(issue #4) — NeoGraphの前にあるSSE / ストリーミングHTTPバックエンドの最も一般的な形状です。付随: v0.6.0 Python OpenInferenceレイヤーのC++版で、Phoenix / Arize / LangfuseがC++駆動のトレースをPython版と同じようにレンダリングします(issue #9)。さらに: 装飾的なPython OTelデタッチノイズを抑制(issue #2)、同じ`thread_id`の並行実行 + `schema_mutex_` × on_chunkロック不変条件がdocstringに固定されました(issue #6)。

### 追加

- C++ 版の`neograph_engine.openinference`（issue #9）。新しい`neograph::observability`モジュールは2つの部分をカバーする：
  - `Tracer` / `Span` — 依存関係のない小さな抽象インターフェースで、NeoGraph自体がopentelemetry-cppを引き込まないようにする。下流側は自身のバックエンド（OTel SDK、インメモリテストフェイク、ロギングレコーダーなど）をラップするアダプタを提供する。4つの属性セッター（string、int64、double、bool — boolは意図的に`set_attribute_bool`に改名され、`const char*`リテラルが誤って解決されないようにしている）、さらにストリームトークン診断用の`add_event`、ステータス、`end()`。
  - `openinference_tracer(tracer)` — CHAIN種別のルートスパンを開き、`OpenInferenceTracerSession`を返す。その`cb`フィールドは`engine.run_stream()`に接続され、ノードごとにCHAIN種別の子スパンを開き、`NODE_START`/`END`ペイロードを`input.value` / `output.value` JSONブロブに詰め込み、`LLM_TOKEN`イベントを個別のスパンイベントとして記録する。
  - `OpenInferenceProvider(inner, tracer)` — 任意の`Provider`をラップし、OpenInference LLM種別の属性セット（`llm.model_name`、`llm.invocation_parameters`、`llm.input_messages.{i}.message.{role,content}`、`llm.output_messages.0.message.{role,content}`、`llm.token_count.{prompt,completion,total}`）をすべての`complete*`呼び出しに付与する。ストリーミングオーバーロードはさらに`llm.token`イベントと最終的に組み立てられた`output.value`を追加する。
  - `tests/test_openinference_cpp.cpp`内の7つのパリティテストが`InMemoryTracer`参照アダプタを駆動する — ルート＋ノードごとのCHAINスパン階層、ERROR / INTERRUPTステータスの表示、LLM_TOKENスパンイベントの記録、セッションクローズ時の遅延スパンのクリーンアップ、LLMプロバイダ属性セット、ストリーミングトークンイベント、例外ステータスの伝播を検証する。

### 修正済み

- `Provider::complete_stream_async`のデフォルトブリッジは、ストリームの実行中に待機中コルーチンのエグゼキュータをブロックしなくなりました。修正前は`co_return complete_stream(...)`をインラインで実行していたため、(a) HTTP/SSE受信ループの間ずっとエンジンの`io_context`ワーカースレッドを占有し、同じエグゼキュータ上の他のノードコルーチンを停止させていました。また(b) `SchemaProvider`のWebSocket Responses分岐では、新しい`run_sync` io_contextを`run_sync(complete_stream_ws_responses(...))`を通じてエンジンワーカー上にさらにネストしていました。このため共有プロバイダー状態が競合し、外側の`GraphEngine::run_stream_async`内から呼び出すと断続的なセグメンテーションフォールトが発生していました。新しいデフォルトは同期`complete_stream`専用のワーカースレッドを生成し、各トークンを待機側のエグゼキュータへ戻します。これによりユーザーの`on_chunk`は待機中コルーチンと同じ単一スレッド上で実行され、再入も起きません。コルーチンは一度限りの`steady_timer.cancel()`で再開され、ワーカースレッドの例外は待機側で再送出されます。`SchemaProvider`にはネイティブの`complete_stream_async`オーバーライドが追加され、WebSocket経路ではワーカースレッドさえ使わず、`co_await`で`complete_stream_ws_responses`を直接待機します。`OpenAIProvider`はWebSocket経路や特別処理を持たないため、新しい基底クラスのデフォルトをそのまま利用します。`tests/test_provider_async_default.cpp`には、ストリーム中も並行ティッカーコルーチンが進み、チャンクがワーカーではなく待機側スレッドへ届くことを検証する`StreamAsyncBridgeDoesNotBlockExecutor`と、`StreamAsyncBridgeRethrowsWorkerException`の2テストを追加しました。（issue #4）

- `openinference_tracer`: OTelのSDKが、トレーサーを `Failed to detach context` と `engine.run_stream_async` + `StreamMode.ALL`と共に使用した際、毎回のシャットダウン時にstderrへ出力していたトレースバックを抑制する。NODE_STARTで作成されたOTelのcontextvarsトークンが、異なる `asyncio.Task` からデタッチされていた（NODE_ENDコールバックは呼び出し元のタスクではなくエンジンの継続から発火する）ため、 `Context.reset(token)` が `ValueError`を発生させた。SDKはこのraiseを握りつぶしたが、完全なトレースバックを `logger.exception`経由でルーティングし続けたため、意味論には影響を与えずに本番ログを汚染した。修正では、アタッチ時に（スレッド、タスク）を記録し、不一致の場合はデタッチをスキップする。さらに、 `logging.Filter` 上に、当該 `opentelemetry.context` をインストールする。同期呼び出し元と同一タスクの非同期呼び出し元は、ノードスパンの下でLLMスパンのネストを引き続き正しく取得する。（Issue #2） `_safe_detach` がスタック上にある間のみメッセージを破棄する狭い

---

## [0.6.0] — 2026-05-07 — OpenInferenceオブザーバビリティレイヤー

LangSmithのUXギャップを解消します。NeoGraphはすでにOTelシェイプのスパンを出力していました(したがってトレースは任意のOTelバックエンドに流れました)。今回のリリースでは、Phoenix / Arize / Langfuseがトレースをフラットな汎用アプリケーションスパンリストではなく、チャットバブル + トークンカウントUIとしてレンダリングするために使用するLLM固有の属性レイヤーを追加します。ローカルのPhoenixコンテナに対してエンドツーエンドで検証済み — ライター→クリティックのグラフは、6スパンの階層(CHAINルート → ノードスパン → LLMスパン)を生成し、モデル名、プロンプト/レスポンス、トークンカウントがPhoenix UIで表示されます。

### 追加

- `neograph_engine.openinference`モジュール:
  - `openinference_tracer(tracer)` — `otel_tracer`をミラーリングするが、ルートノードとノードスパンに`openinference.span.kind = "CHAIN"`をタグ付けし、ノードペイロードを`input.value` / `output.value` JSONブロブに詰め込むコンテキストマネージャ。
  - `OpenInferenceProvider(inner, tracer)` — 任意の`Provider`をラップする。各`complete()`で`llm.complete`子スパンを開き、`span.kind = "LLM"`をタグ付けし、`llm.model_name`、`llm.invocation_parameters`、`llm.input_messages.{i}.message.{role,content}`、`llm.output_messages.0.message.{role,content}`、`llm.token_count.{prompt,completion,total}`、およびLangfuse互換の`input.value` / `output.value`ブロブをキャプチャする。
- `bindings/python/tests/test_openinference.py`内の4つのテスト — 属性の存在、スパン階層、例外パス、ノード入力/出力JSONブロブに関するInMemorySpanExporterアサーション。

### 修正済み

- `openinference_tracer`は、各ノードスパンをOTelの*現在の*コンテキストとして(`otel_context.attach`経由で)アタッチするようになったため、ノード本体内部で開かれた子LLMスパンは、そのノードスパンの下にネストされる。これがないと、C++→Python pybindコールバック境界をまたぐcontextvar伝播により、期待される単一のトレイスツリーではなく、実行ごとに3つ以上の無関係なtrace_idが生成された。トークンはNODE_END / ERROR / INTERRUPTでデタッチされ、以前の現在のスパンを復元する。既存の`otel_tracer`が文書化しているのと同じパターンである。つまり、`trace.use_span(...).__enter__()`ではなく明示的なアタッチ/デタッチであり、`__exit__`が一致しないと安全に使用できない。

### 注記

- OpenTelemetryはオプション依存のままです。`neograph_engine.openinference`のインポートは、`opentelemetry-api`がインストールされていない場合にのみ、最初の使用時に明確なImportErrorを発生させます。インポート時ではありません。
- Phoenixのエンドツーエンド実行の場合::

      docker run -d -p 6006:6006 -p 4317:4317 arizephoenix/phoenix
      pip install opentelemetry-exporter-otlp

OTLP gRPCエクスポーターを`http://localhost:4317`に設定し、`http://localhost:6006`を開いてトレースを表示します。モジュールのdocstringに完全なスニペットがあります。

---

## [0.5.0] — 2026-05-07 — バインディングの操作性: ライブミューテーションリストプロパティ

バインディング経由で公開されるmessage / writes / sendsリストを変更する最も自然なPythonイディオムにおける、サイレントなno-opトラップを解消します。以前は`params.messages.append(msg)`がコピーを変更し、基盤となるC++ベクターは新しい項目を認識しませんでした——グレースフルな失敗（クラッシュも警告もなし）で、劣化したLLM応答を生み出していました。現在は`.append()`がライブのstd::vectorまで押し通します。

### 追加

- `bindings/python/src/opaque_types.h` — 5つのベクター型に対する`PYBIND11_MAKE_OPAQUE`: `std::vector<ChatMessage>`、`<ChatTool>`、`<ToolCall>`、`<graph::ChannelWrite>`、`<graph::Send>`。
- `module.cpp` `init_opaque_vectors` — `py::bind_vector`は各々をPythonクラス（`ChatMessageList`、`ChatToolList`、`ToolCallList`、`ChannelWriteList`、`SendList`）として登録し、ライブのC++ベクトルに対する完全な可変シーケンスプロトコルをサポートします。
- 各々に対する`py::implicitly_convertible<py::list, …>` — レガシーのビルドしてから代入するパターン（`params.messages = [m1, m2]`）は変更なしで動作し続けます。代入はPythonリストをバインドされたクラスに自動変換します。
- `bindings/python/examples/23_evolving_chat_agent.py` — スレッドごとに進化するチャットエージェント（ライブLLM）: エージェントのJSON定義は、蓄積された会話履歴に基づいてターン間で書き換えられます。進化をまたぐチェックポイント再開（以前のメッセージが存続）、`__graph_meta__`監査チャネルパターン、およびバリデータ境界（ノードタイプのホワイトリスト、必須チャネル）を示します。

### 変更

- `params.messages` / `.tools` / `chat_message.tool_calls` / `node_result.writes` / `.sends` は、プレーンな`list`ではなく、バインドされたクラスを返すようになりました。`len()`、反復、`__getitem__`、`__setitem__`、`.append()`、`.extend()`、スライシング——すべてPythonリストのように動作します。`isinstance(x, list)`のみがFalseを返します。リポジトリと下流のgrepで、そのようなisinstance呼び出しサイトがゼロであることを確認しています。
- `.github/workflows/nightly.yml` — `ops/s ≥ 600K`ゲートを削除します。`err=0`と`leak=false`で4回連続失敗した後、しきい値（ローカルハードウェアで969K ops/sに較正）は共有GitHubホストランナーでは到達不能でした（測定値233〜273K ops/s、ローカルの3〜4分の1）。スループット回帰検出はPR時の`bench-regression`ジョブ（安定したハードウェア、µs単位のシングルショットディスパッチ）にあります。夜間ソークの実際の値は、5分間の`err==0` + `leak_suspect==false`です——両方ともハードゲートとして維持されます。

### 注記

- `ChatMessage.image_urls`（`std::vector<std::string>`）は意図的に移行されていません——`vector<string>`はバインディング全体で広く使用されており、すべての呼び出しサイトを掃討せずにグローバルなOPAQUEを適用するのは安全ではありません。残存する制限として文書化されています。v0.6+の候補です。

---

## [0.4.0] — 2026-05-05 — v1.0準備: 統一された`run(NodeInput)`ディスパッチ

v1.0シャープニングトラックの最初のリリース。8つの仮想`GraphNode`クロス積（`execute` / `execute_async` / `execute_full` / … / `execute_full_stream_async`）は、単一の正規メソッド`run(NodeInput) -> awaitable<NodeOutput>`に収束します。実行ごとのキャンセルメタデータは、非チャネルセットの`GraphState`メンバーとスレッドローカルの密輸チャネルから、明示的な`RunContext`引数に移動します。`deadline`と`trace_id`は、予約済みの拡張スロットとしてのみ追加され、`RunConfig`によっては設定されません。`CancelToken`は階層的な`fork()`を獲得し、マルチSend fan-outワーカーはそれぞれ、親の`cancel()`がカスケードするプライベートシグナルを所有します。

### 追加

- `RunContext`（`include/neograph/graph/engine.h`）— 実行ごとの明示的なメタデータ：使用可能な`cancel_token`、`thread_id`、`step`、`stream_mode`、および予約済みの`deadline`と`trace_id`スロット。エンジンはこれをすべての`NodeExecutor::run`呼び出しに通します。**PR 1、コミット`a473f0e`。**
- `GraphNode::run(NodeInput) -> awaitable<NodeOutput>` — 単一の正規ディスパッチエントリポイント。`NodeInput { state, ctx,
  stream_cb }`; `NodeOutput { writes, command, sends }`。デフォルトのボディはレガシーの8つの仮想関数に転送するため、既存のサブクラスはコンパイルを続けます。**PR 2、コミット `607ce66`。**
- `CancelToken::fork() -> shared_ptr<CancelToken>` — 独自の`cancellation_signal`を持つ子トークン。親の`cancel()`は、すべてのライブ子（および再帰的に孫）にカスケードします。`run_sync(aw, parent_token)`は`parent_token->fork()`に切り替わり、各ネストされたopが独自のスロットをバインドします — v0.3.xのemit対bindの競合とマルチSendの単一ハンドラー上書きを閉じます。v0.3.xの`add_cancel_hook`リストは、非推奨を通じて機能し続けます。**PR 3、コミット `897645c`。**
- `[[deprecated]]` 8つのレガシー`GraphNode`仮想関数 + `add_cancel_hook`上。内部呼び出しサイト（graph_node.cppのデフォルトチェーン、デフォルトの`run()`フォワーダー）は、新しい`NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED`マクロ（`api.h` — GCC / clang / MSVCポータブル）で囲まれています。非推奨の仮想関数をオーバーライドするユーザーコードは移行警告を確認します。エンジン内部はクリーンなままです。**PR 4、コミット`35a4517`。**
- `engine.get_state_view(thread_id) -> StateView`が正規の状態読み取りになりました。生の辞書`engine.get_state(...)`は、docstringでソフト非推奨です（警告は発行されません — 生の辞書は有効なエスケープハッチのままです）。**PR 5、コミット `f31aa53`。**
- 7つのC++ + 19のPython例が`run(NodeInput)`に移行されました。スモーク実行はv0.3.2の出力とビット単位で一致します。**PR 6a/6b、コミット `a2a24ef` / `0a76e3a`。**
- Pybindの`PyGraphNodeOwner`は`run(NodeInput)`をオーバーライドし、Pythonユーザーの`run`メソッド（定義されている場合）にディスパッチし、それ以外の場合はレガシーチェーンにフォールスルーします。`RunContext` / `NodeInput` / `CancelToken`はPythonに公開されます。`cancel_token`は、スレッドローカルのスマイグリングなしで`input.ctx.cancel_token`として到達可能です。**PR 7、コミット `4e186a5`。**
- `docs/reference-en.md` §6 GraphNodeは単一の`run()`に収縮しました。RunContext + `fork()`の例のサブセクションが§7の下に追加されました。READMEの「Differences from LangGraph」に「One node method」エントリが追加されました。**PR 8、コミット `519a00b`。**
- 組み込みのC++ノード（`LLMCallNode`、`ToolDispatchNode`、`RouteToNode`）は、`run(NodeInput)`オーバーライドに移行されました。**PR 9a、コミット `d1070dc`。**
- 新規参入者モードのトラップ修正：READMEのCMakeスニペットは、`graph::`サブ名前空間、cppdotenvパス、`OpenAIProvider::create()`と`create_shared()`の比較、nlohmannサブセットとしての`neograph::json`、3引数と2引数の`compile()`を文書化しています。Pythonの`compile(def, ctx, store=None)`キーワード引数が追加されました（追加的、非破壊的）。**コミット `ee11ed6`。**

### 変更

- README：「10Kワーカー測定ストレステスト」セクション — neoclaw上のRTX 4070 Ti + Gemma 4 E2B Q4、N=10000完了 @ 0エラー / 424秒 / 2572 MBピーク / ~1 KB追加ワーカーコスト / p99 648 ms（`7840b81`）。
- README: 「本番経済性」セクション — フリート安全性 + RAM デルタの枠組み (`b82b15a`)。
- README: LangGraph デルタリスト内の「Docker 不要」+「依存関係ドリフト耐性」の箇条書き (`333b482`、`a6061d7`)。

### 非推奨

- `GraphNode::execute / execute_async / execute_full /
  execute_full_async / execute_stream / execute_stream_async /
  execute_full_stream / execute_full_stream_async` — v0.5.x まで `[[deprecated]]` アノテーションと併用して動作、v1.0 で削除。
- `CancelToken::add_cancel_hook` — `fork()` に置き換え。同じ非推奨期間。

### 注記

- 検証: v0.4.0タグで442 → 452 ctest (NodeRunDispatch 3件 + CancelTokenFork 7件追加) + 96 pytest + 5件のライブLLM/WSがグリーン。
- サブPR (`run(const NodeInput&)` 参照パラメータ) が、pybind 非同期パスにおける v0.2.0 RunConfig コルーチン参照 UAF クラッシュ形状を引き起こした。修正はマージ前に適用: `NodeInput in` 値渡し。`node.h` に文書化。

---

## [0.3.2] — 2026-05-05 — キャンセル伝搬の強化 (5ラウンド)

v0.3.0 の単発キャンセルが明らかにしたギャップを埋める5 ラウンドのパッチシリーズ: Send fan-out 伝播、インプロセスポーリング、Python 用フック、C++ スコープ、例外型。また、FastAPI SSE チャットデモ評価からの TODO_v0.3.md フィードバックバッチも収録 — `resume_if_exists`、dict-or-list `update_state`、型付き状態読み取り用の StateView。

### 追加

- `RunConfig::resume_if_exists` — 明示的な `resume()` 呼び出しなしで、以前のスレッドのチェックポイントをオプトインで再開。標準的なマルチターンチャットのセマンティクス: `engine.run(cfg)` は `thread_id` が存在する場合に会話を継続する。
- `engine.update_state(thread_id, dict | list[ChannelWrite],
  as_node="")` — 両方の形状を受け入れる。修正前は `dict` のみが機能し、リストを渡すと黙って no-op となった。リスト形式は、すべてのノード本体の emit 形状と対称。
- `StateView` (`bindings/python/neograph_engine/state_view.py`) — Pydantic 型付き状態読み取り。`engine.get_state_view(thread_id) ->
  StateView` はフラットなドットアクセス (`view.messages` / `view.foo`) に加え、dict エスケープハッチ用の `view.raw` を返す。型付きチャンネル定義用のサブクラス: `class ChatState(ng.StateView): messages: list[dict] = []`。
- `bindings/python/tests/test_async_cancel_live_llm_fanout.py` — 実行中のキャンセルが、ソケット層で Send が生成したすべての兄弟を本当に中止することを検証 (v0.3.1 の根本原因パッチ)。
- `examples/22_self_evolving_graph.py` — TODO_v0.3.md #9 クックブックの折り込みとともに v0.3.2 に移動。
- Doxygen `/* */` ワイルドカード修正 — `acp/types.h` に `/**` ブロックが含まれており、パスワイルドカード（`fs/*`, `terminal/*`）がネストされたコメントを開き、後続のすべての診断を抑制していました。置き換え先： `&#42;` HTMLエンティティ。

### 修正済み

- キャンセル伝播、累計5ラウンド:
  1. v0.3.0 シングルノード — `cancel_token` が `Provider::complete` に到達。
  2. v0.3.1 マルチSendポインタのドロップ — fan-outワーカーが `run_cancel_token_shared()` を共有するようになった（`init_state +
     restore` がチャネルセット外のワーカーごとの状態を再構築した際に、このポインタは失われていた）。
  3. v0.3.1+ プロセス内ポーリング — エンジンのスーパーステップループがステップ間でポーリングするようになった（LLM入出力時のみではない）。
  4. v0.3.2 Python用フック — `add_cancel_hook` が実行ごとのトークンにコールバックを登録し、`cancel()` で発火する。同期Python `execute()` がスレッドローカルスコープなしでアドホックなキャンセルハンドラをインストールできるようにする。
  5. v0.3.2 C++スコープ + リトライ + 例外型付け — メインスレッドでの新規スロー `NodeInterrupt`（libstdc++ `__exception_ptr::_M_release` の競合を回避）、リトライ予算はキャンセルを尊重、ランタイム対ロジックの例外分割。
- `execute_stream` のみのPythonノードがデフォルトの `execute` パスに黙ってフォールスルーしていた（NotImplementedError）。現在は `run_stream` が、ユーザーがストリーミングバリアントのみをオーバーライドした場合に `execute_stream` を直接配線する。
- `update_state` が list[ChannelWrite] を受け入れる — 黙示的なno-opを解消（TODO_v0.3.md #5）。

### 注記

- 442 ctest + 96 pytest + 2 ライブLLM（シングル + fanoutキャンセル）が v0.3.2 タグでグリーン（`915e90e`）。
- 27/30 C++ 例 + 20/22 Python 例が `examples/run_all.py` で合格。スキップされたテストは外部サービス（Postgres / Crawl4AI / ライブOpenAI）を必要とする。
- Valgrind 6 例で 0 エラー、815 allocs / 815 frees クリーン。
- ベンチマーク中央値 5.185 µs/iter（seq パス、v0.3.0 ベースライン） — ラウンド全体でパフォーマンス回帰ゼロ。

---

## [0.3.0] — 2026-05-04 — 協調的キャンセル伝播

FastAPI SSEチャットデモ評価中に報告された本番コスト漏れのギャップを解消：フロントエンドの `AbortController` が asyncio タスクをキャンセルしても、上流の OpenAI リクエストが完了まで実行され続けることはなくなった。キャンセルは実行のすべてのレイヤーに伝播する。

### 追加

- `neograph::graph::CancelToken`（アトミックフラグ + asio `cancellation_signal`）と `CancelledException` — `include/neograph/graph/cancel.h`。協調的キャンセルプリミティブ。`RunConfig::cancel_token` 経由で渡す（オプションの `shared_ptr`）；エンジンのスーパーステップループはステップ間で `is_cancelled()` をポーリングし、`CancelledException` で脱出する。トークンの `cancellation_slot()` は実行の `co_spawn` にバインドされるため、実行中の LLM HTTP ソケット操作はワイヤ上で中止される（asio `operation_aborted`）。
- `CompletionParams::cancel_token` — 複数の `provider.complete()` 呼び出しにまたがってabortをスレッド化するユーザー向けの明示的なピン。 `Provider::complete` はそれを読み取り（または `current_cancel_token()` によって設定されたスレッドローカル `PyGraphNode::execute_full_async`にフォールバックし）、スロットをその内部の `run_sync` io_context にバインドするため、キャンセルがヒットした同期Pythonノードでも課金が停止する。
- `GraphState::run_cancel_token()` — 実行ごとの、非シリアライズ化されたハンドルであり、pybind `PyGraphNode` が `CurrentCancelTokenScope` を同期Python `execute()` 呼び出しの周囲にインストールするために使用します。これにより、同期Pythonユーザーはノードコードを変更することなく、透過的なキャンセル伝播を得られます。
- pybind `engine.run_async` / `run_stream_async`: asyncio `Future.cancel()` は現在 `add_done_callback` を経由して `CancelToken::cancel()` に配線され、`co_spawn` はトークンのキャンセルスロットをバインドします。
- pybind safe-resolve ヘルパー `_safe_set_future_result` / `_safe_set_future_exception` — ガード `future.set_result` / `set_exception` 経由でポストされた呼び出しを `call_soon_threadsafe` キャンセル済みフューチャーの `InvalidStateError` ストームから保護します。
- `bindings/python/tests/test_async_cancel_live_llm.py` — ライブ OpenAI E2E テスト。OpenAI HTTP が `Future.cancel()` から 3 秒未満で完了することを検証します（実際には即時。修正前は約 7〜8 秒の未キャンセルストリーミングでした）。`NEOGRAPH_LIVE_LLM=1` が設定されていない限りスキップされます。
- `examples/22_self_evolving_graph.py` — 自己進化グラフの PoC: `prompted_llm` ノードが自身のプロンプトを JSON 設定から読み取り、LLM リライターが実行間でグラフ定義を変更して再コンパイルできるようにします。`0.0 → 0.4` スコアの改善を示し、リライターにおけるチャネルフロー推論のギャップを文書化します。

### 変更

- `Provider::complete(params)` は、次の場合に内部キャンセルスロットをその `run_sync` にバインドするようになりました： `params.cancel_token` が設定されている場合、またはスレッドローカルの `current_cancel_token()` がアクティブな場合。オプトインしない呼び出し元に対しては、以前のデフォルト動作（キャンセルなし）が維持されます。
- `neograph::async::run_sync` はオプションの `graph::CancelToken*` パラメータを取得しました。非 null の場合、バインドされたスパウンはトークンのスロットをバインドします。
- pybind `resolve_future_async` は、`future.set_result` を `call_soon_threadsafe` 経由で直接呼び出す代わりに、safe-resolve ヘルパーを経由してルーティングされる。

### ロードマップ（v0.3.x に延期 — `TODO_v0.3.md` を参照）

- 同じ `thread_id` での LangGraph スタイルの自動チェックポイント再開。
- `run_async` エラーメッセージ内のストリーミング専用ノードのヒント。
- `cb.emit_token(node, data)` の使いやすいヘルパー。
- README の「LangGraph との違い」セクション。
- `update_state` のシグネチャをドキュメントに合わせて調整。
- `get_state` フラットヘルパー / Pydantic アクセサ。
- `run_parallel_async`および`run_sends_async`ブランチfan-outにおけるキャンセル伝播のライブ検証。
- pgvector RAG の例。

---

## [未リリース] — ステージ 4

ステージ4は、非同期パス上の最後の`run_sync`ホップを閉じる。`run_async`は、呼び出し元のexecutor上にエンドツーエンドで留まるようになった: 1つの`io_context`スレッド上の3つの50msエージェントが、`examples/27_async_concurrent_runs`において約150ms(直列)から約50ms(オーバーラップ)に低下する。

### 破壊的変更

- **`GraphNode::execute_full_async` デフォルトがasync-firstに変更されました。** 現在は `co_await execute_async(state)` を `NodeResult` にラップしており、同期の `execute_full(state)`を呼び出す代わりになっています。同期の `Command`/`Send` オーバーライドからのみ `execute_full` を発行するサブクラスは、1行の `execute_full_async` ブリッジを追加する必要があります:
  ```cpp
  asio::awaitable<NodeResult>
  execute_full_async(const GraphState& state) override {
      co_return execute_full(state);
  }
  ```
ブリッジがない場合、`Command`/`Send`は非同期パス上で静かに破棄される — 3.0が同期経由のルーティングによって修正した2.0の潜在的なディスパッチバグであり、スーパーステップごとに`io_context`スポーンのコストがかかる。ツリー内のすべてのサブクラス(`deep_research_graph`、例10/14/21、テスト5サイト)は現在、ブリッジを備えている。

### パフォーマンス

- 例27のウォールタイム: **152 ms → 53 ms**(1つの`io_context`スレッド上の3エージェント×50msタイマーステップ、完全オーバーラップ)。
- シングルラン・ベンチマークで測定可能な回帰はありません。 `run()` 依然として同じコルーチンを、新しいシングルスレッドの `io_context` 経由で駆動します。 `run_sync`.

### テスト

- 341/341 ctest グリーン
- 295/295 ASan+UBSan グリーン
- コルーチン多用サブセット(20 テスト、2.4 秒)で Valgrind クリーン

### リリース後検証（同日）

- **全30例を再実行:** 26/29 PASS、0 FAIL、3件は環境依存(clay_chatbot → raylib、postgres_react_hitl → docker compose、deep_researchフルループ → crawl4aiサービス)。`21_mcp_fanout`は3 MCPコール/8msウォルで測定 — ステージ4のオーバーラップは、実際のネットワークI/O下でも維持される。

- **ARM64互換性(docker buildx --platform linux/arm64):** リポジトリルートの`Dockerfile.arm64-smoke`。ubuntu:24.04-arm64 + core+llm+async+sqlite+testsビルドはQEMUエミュレーション下で約15分で完了; **ARM64上で306/306 ctest green**。ストリップされたバイナリサイズは0.81〜0.88 MB(x86_64とほぼ同一)。例27はエミュレーション下で65 msで実行(ネイティブx86_64: 53 ms)。macOSベータ(Apple Silicon)に加えて、Linux/ARM64がサポート対象として確認された。

- **キャッシュ局所性(Ryzen 5800X / Zen 3、Valgrind cachegrind、32 KB L1i/d 8-way、32 MB L3 16-way):** `bench_concurrent_neograph`スイープN=1 → 10,000。

    | N | I refs | LLiミス | LLiミス率 | ネイティブp50 |
  |---:|---:|---:|---:|---:|
    | 1 | 5.3 M | 4,313 | 0.08% | 17 µs |
    | 100 | 11.8 M | 4,320 | 0.04% | 6 µs |
    | 10,000 | 648 M | 4,329 | 0.00% | 5 µs |

最終レベル命令ミスは、N の 4 桁の範囲にわたって約 4,320 で横ばいのまま。ユニークなホットコードのワーキングセットは約 277 KB(L3 の 0.85%)。N=10,000 での 648 M 命令は、わずか 4,329 回の LL ミスを発生させる — 命令 150,000 件あたり約 1 回のミス。ネイティブ p50 は、純粋に I-cache のウォーミングにより 17 µs から 5 µs に低下する。「バースト並行性の堅牢性」というポジショニングに対する最初の測定された証拠。

---

## [3.0.0] — 2026-04-22

3.0はTaskflow依存を削除し、同期および非同期のスーパーステップ実行を単一のasioコルーチンパスに統合します。グラフ定義JSON、ノードABI、チェックポイントスキーマ、および公開エントリポイント（`run`, `run_async`, `run_stream`, `resume`）は2.0とソース互換です。破壊的変更は、 `GraphNode` サブクラスに限定され、これらは**同期** `Command`/`Send` を出力します。 `execute_full` オーバーライドからのみ

### 破壊的変更

- **`deps/taskflow/`とTaskflow INTERFACEターゲットは削除された。** 同期スーパーステップループ、`run_one`、`run_parallel`、`run_sends`、およびプロセス全体の`tf::Executor`静的変数は削除された。NeoGraphのインクルードパスを介して`#include <taskflow/...>`する下流コンシューマは、Taskflowを別途ベンダー化する必要がある。
- **`GraphNode::execute_full_async` default は現在、同期 `execute_full` への直接呼び出しを介してブリッジします（ `co_await execute_async`なし）。**これにより、同期専用オーバーライドから発行される `Command`/`Send` が保持されます — これは一般的な2.0パターンです — すべてのエントリポイントが現在共有する非同期パスを通じて。非ブロッキングI/Oと `Command`/`Send` の両方を必要とするAsyncネイティブノードは、 `execute_full_async` を直接オーバーライドする必要があります。docstringは2.0以降これを述べてきましたが、2.0は同期 `run()` がコルーチンパスを完全にバイパスしたため、これを実行したことはありませんでした。
- **`NodeExecutor::run_one` / `run_parallel` / `run_sends` の同期メソッドは削除されました。** `_async` の同等メソッドを使用してください。
- **CPU並列fan-outはオプトインです。** 以前はTaskflowがデフォルトでプロセス全体のスレッドプールを提供していました。3.0では、`run_parallel_async` と `run_sends_async` のマルチSendブランチは、コルーチンを駆動するエグゼキュータ（同期 `run()` によって起動されるシングルスレッドのio_context、または `run_async()` の呼び出し元自身のエグゼキュータ）に応じてブランチをディスパッチします。I/Oバウンドのfan-outは依然としてオーバーラップします（シングルスレッドでのco_awaitサスペンション）。CPUバウンドのfan-outは、呼び出し元が `run_async()` にマルチスレッドエグゼキュータを使用するか、`engine->set_worker_count(N)` を介してエンジン所有のプールをオプトインしない限り、シリアル化されます。

### 追加

- `neograph::async::run_sync_pool(awaitable, n_threads)` — 既存のシングルスレッド `run_sync` に加えて、Nワーカー同期↔非同期ブリッジ。呼び出しのために新しい `asio::thread_pool` を起動し、内部の `make_parallel_group` ブランチが別々のワーカーで実行されるようにします。
- `GraphEngine::set_worker_count(n)` — 並列fan-outディスパッチのために `NodeExecutor` が使用する、オプトインのエンジン所有スレッドプール。エグゼキュータを再構築します。並行実行の前に呼び出す必要があります。

### 変更

- `GraphEngine::execute_graph` (sync) は廃止されました。すべてのエントリポイント（`run`、`run_stream`、`resume`）は`execute_graph_async`を経由して`neograph::async::run_sync`にルーティングされるため、スーパーステップループ、リトライバックオフ、チェックポイントI/O、および並列fan-outは、エンドツーエンドで単一のコルーチンパス上に存在します。
- `benchmarks/concurrent/bench_concurrent_neograph.cpp` は、呼び出し側ドライバーを `tf::Executor` / `tf::Taskflow` から `asio::thread_pool` + `asio::post` に切り替えました。

### パフォーマンス（リファレンスLinux上のbench_neograph Release -O3 -DNDEBUG、10回実行の中央値）

- `seq` エンジンオーバーヘッド（3ノードチェーン、カウンター）：呼び出しあたり **約5.0 µs**。
- `par` エンジンオーバーヘッド（5ワーカーfan-out + サマライザー）：呼び出しあたり **約11.8 µs**。
- ベンチプロセス全体のピークRSS（ウォームアップ＋seq＋parイテレーション）：**4.8 MB**。
- 同じワークロードでのLangGraph 1.1.9との比較: イテレーションあたり**seqで131倍、parで199倍高速**、RSS約12倍軽量。

このCHANGELOGの以前のドラフトでは、「約46 µs シーケンシャル / 約114 µs パラレル」を3.0のリグレッションとして挙げていました。これらの数値は、`CMAKE_BUILD_TYPE` が未設定のビルドツリーからのものであり、ベンチバイナリが `-O3 -DNDEBUG` なしでコンパイルされていました。適切なReleaseビルドでは、非同期ピアの統合は2.0のTaskflow同期パス（2.0 READMEが同じホスト上で20.65 µs シーケンシャル / 150.7 µs パラレルと宣伝していた）に対して**有利**です。修正されたチャートは [`docs/images/bench-engine-overhead.png`](docs/images/bench-engine-overhead.png) にあります。

### 移行

- ノードが `execute()` / `execute_async()` をオーバーライドし、`Command` / `Send` を発行しない場合、アクションは不要です。
- 同期 `execute_full` をオーバーライドして `Command` / `Send` を発行する場合：変更は不要です — 3.0の非同期パスのデフォルトは、現在、同期オーバーライドを直接呼び出します。`Command.goto_node` ルーティングは、同期エントリポイントと非同期エントリポイントの両方で機能します。
- オーバーライドする場合 `execute_async` (async-native I/O) かつ `Command` / `Send`を希望する場合: `execute_full_async` を直接オーバーライドし、 `NodeResult` をそこで組み立ててください。`execute_async` のみをオーバーライドすると `Command` / `Send` を黙って失います。なぜなら、デフォルトの `execute_full_async` は現在、同期 `execute_full`経由でルーティングされ、非同期 `execute_async` ではないからです。
- Taskflowのプロセス全体のプールをCPU並列fan-outに依存していた場合、 `engine->run()`: compile()の後に `engine->set_worker_count(N)` を一度呼び出すか、または `run_async()` を介してエンジンを自身のマルチスレッド `asio::thread_pool` / io_context上で駆動してください。

---

## [2.0.0] — 2026-04-22

Stage 3 async APIを備えた最初の公開リリース。これは破壊的リリースです。以下の変更はコンパイル（C++標準）とABI（抽象基底クラスにasyncピアが追加）に影響します。Sync呼び出しサイトはビット単位で保持されるため、**`Provider` / `CheckpointStore` / `GraphNode` / `Tool`をオーバーライドしないアプリケーションコードは変更なしで動作し続けます**。

### 破壊的変更

- **C++20 必須。** 公開APIは `asio::awaitable<T>` を公開しており、これには `std::coroutine` のサポートが必要です。利用者は `-std=c++20` （またはそれ以上）でコンパイルする必要があります。GCC 13+、Clang 15+ でテスト済み。GCC 13のコルーチン回避策については `docs/ASYNC_GUIDE.md` §4.1 を参照してください。
- **libpqxx依存関係を削除。** `neograph::postgres`は現在libpqを直接リンクします。Ubuntu 24.04ユーザーは、libpqxx-7.8t64のC++17/C++20 ABI分割によって導入された`pqxx::argument_error::argument_error(..., std::source_location)`リンクエラーに遭遇しなくなりました。CMake findは現在`PostgreSQL::PostgreSQL`（CMakeバンドルFindPostgreSQL）を対象としています。`libpqxx-dev`のみをインストールしたコンシューマは、`libpq-dev`もインストール/保持する必要があります。
- **`Provider`、`CheckpointStore`、`GraphNode`、`MCPClient` ABIを拡張。** それぞれにasyncピア仮想関数（`complete_async`、`save_async`、`execute_async`、`rpc_call_async`およびその変種）が追加されました。下流のサブクラスは2.0ヘッダーに対して再コンパイルします。ソースは、サブクラスがネイティブasyncオーバーライドを提供したい場合を除き変更されません（実際のI/Oを行う実装者には推奨）。
- **`CheckpointStore::save` / `load_latest` / `load_by_id` / `list` / `delete_thread` はもはや純粋仮想ではありません。** これらには、対応する `_async` ピアに `neograph::async::run_sync`を介してブリッジするデフォルト実装が含まれるようになりました。同期側をオーバーライドするサブクラスは引き続き動作します。オーバーライドを一切提供しなかったサブクラス（以前はコンパイルエラーになっていた）は、今では無限再帰します。契約: 各同期/非同期ペアの少なくとも一方をオーバーライドすること。

### 追加

- **Async API** すべてのI/Oレイヤーにわたって（完全なリファレンスは`docs/ASYNC_GUIDE.md`）：
  - ベースクラスとすべての組み込みプロバイダー（OpenAI、Schema、RateLimited）上の`Provider::complete_async`。
  - HTTPとstdioトランスポートの両方の`MCPClient::rpc_call_async`。stdioは`asio::posix::stream_descriptor`を使用します。
  - 8つのsyncメソッドすべての`CheckpointStore::*_async`。
  - `GraphNode::execute_async` + stream / full / full_stream変種。asyncネイティブクロスオーバーデフォルト付き。
  - `GraphEngine::run_async` / `run_stream_async` / `resume_async` を駆動する `execute_graph_async` — 並列fan-outを`asio::experimental::make_parallel_group`経由で含む、エンドツーエンドのコルーチン・スーパーステップループ。
  - `neograph::AsyncTool` ユーザーツール向けのアダプターで、同期`Tool`インターフェースを維持しながらコルーチンボディを必要とするものです。
- **`neograph::async`名前空間** — HTTPクライアント、コネクションプール、SSEパーサー、run_syncブリッジ、URLエンドポイント分割。`include/neograph/async/*.h`を参照。
- **新しい例**:
  - `examples/27_async_concurrent_runs.cpp` — 1つの`io_context`上で複数のエージェント。
  - `examples/05_parallel_fanout.cpp`（書き直し） — `run_parallel_async`を使用した単一グラフ実行内の非同期fan-out。
- **CIベンチマーク回帰ゲート** (`.github/workflows/ci.yml`) — PRチェックが`bench_async_http` / `bench_async_fanout` / `bench_neograph`の下限を強制します。

### パフォーマンス

Stage 2 同期ベースラインに対する feat/async-api ブランチで測定:

- `bench_async_http --mode async_pool --concur 1000`: 6064 ops/s → **17834 ops/s** (2.9×)。
- `bench_async_fanout --concur 50000`: スレッド・パー・エージェントは実現不可能 → **541K ops/s / 67 MB RSS**。
- `examples/27_async_concurrent_runs` (3 × 50msの非同期処理): 150ms (同期) → **50ms** (1 io_contextスレッド)。
- `examples/05_parallel_fanout` (3 × 100-150msの非同期処理): 370ms (逐次) → **150ms** (1 io_contextスレッド)。
- `bench_neograph` エンジンオーバーヘッド: 変化なし (~30 µs 逐次 / ~205 µs 並列)。コルーチン機構はホットパスを劣化させない。

### 2.0.0には未搭載

- **Taskflow依存** は残る。同期の`engine.run()`パスはfan-outに依然としてそれを使用する。Sem 4.5では、同期パスを`run_sync(*_async)`に置き換えられるか再検討し、依存関係を完全に削除できるようにする。

### クロスプラットフォーム

2.0.0では、3つのプラットフォームが異なる安定度レベルでサポートされています。このレベルは、リリース前にプラットフォームが受けた実世界での検証の度合いを反映しています（機能カバレッジではありません。コードベースは`#ifdef _WIN32`分割で単一ソースであり、テストが通れば機能はプラットフォーム間で同等です）。

#### Linux — **GA**（本番対応）

* Ubuntu 24.04、GCC 13。
* ローカルで完全な332/332 ctestグリーン（docker `postgres:16-alpine`経由のPostgres）に加え、コミット済みCI下限内のすべてのベンチマーク。
* fork/pipe/execvp上のMCP stdio + `asio::posix::stream_descriptor`。
* Postgres 非同期ピア（libpq ノンブロッキング上 + `asio::posix::stream_
  descriptor` ラッピング `PQsocket`）
* 上記のすべての性能数値の参照プラットフォーム。

#### macOS — **ベータ**

* macos-latest（Apple Silicon）、Clang via Xcode。
* CIがビルドおよび非Postgresテストを実行する。Postgres統合ケースはサービスコンテナなしで自己スキップされる。POSIXパス（同じfork/pipe + asio::posixコード）は実行される。
* `CoreFoundation` + `Security`フレームワークがhttplib経由でリンクされ、TLSのシステム証明書読み込み用。
* 2〜4週間のCI実行とユーザーレポートでランタイム動作の相違（コルーチンスケジューリング、SIGPIPE/EPIPE形状、パイプバッファサイズ）を確認するまでベータとして扱う。インシデントなしにそれらが実現すればGAへの昇格をターゲットとする。

#### Windows — **アルファ**

* windows-latest、MSVC 19.44 (VS 2022)、x64。
* CIスコープ: **core + async + MCP + LLMのみ**。PostgreSQLとSQLiteバックエンドはWindows CIジョブで無効化されています。vcpkgが毎回の実行でOpenSSL / libpq / zlib / lz4をソースからコンパイルするためです（約20分、`x-gha`が削除されて以来、上流に動作するバイナリキャッシュバックエンドがありません）。Windowsユーザーは、自身のvcpkg / chocoセットアップを介してローカルでこれらをコンパイルします。
* ランナーのプリインストール済みchocoパッケージ（`C:/Program Files/OpenSSL-Win64/`）経由のOpenSSL。httplib + asio::sslのTLSパスがコンパイルおよびリンクされます。
* MCP stdio: `CreateProcess` + 名前付きパイプ（FILE_FLAG_OVERLAPPED）+ `asio::windows::stream_handle`。オーバーラップパイプパスは、ローカルのWindows検証なしでMSDN仕様に基づいて記述されました。最初のユーザーがエッジケース（ERROR_IO_PENDING処理、大きなJSONレスポンスでのパイプバッファ境界）を表面化することが予想されます。
* Postgres 非同期ピア（ローカルで有効な場合）：`asio::ip::tcp::
  socket::assign` が `PQsocket` が返す SOCKET をラップ（64ビット SOCKET 値を保持するため `native_handle_type` を介してキャスト）。Windows CI では実行されない — ローカルのみ。
* コルーチンメカニズムはMSVCの`<coroutine>`に存在します。仕様上GCC/Clangと一致する動作が期待されますが、`examples/27`のクロスランオーバーラップ測定はWindowsではまだ確認されていません。
* 2.0.0 までは **alpha** として扱う。本番ユーザー1名が stdio/パイプまたはコルーチンスケジューラの問題に遭遇せずにマルチエージェントワークロードを1週間実行し、かつ Postgres 非同期ピアが vcpkg の完全な libpq ビルドを実行する意思のあるユーザーによってローカル検証されたら、beta に昇格する。

> **パターン**: CI グリーンは下限であり、上限ではない。レイヤー3 ランタイム
> の動作の違い（コルーチンスケジューリングのタイミング、パイプバッファ
> 境界、ソケットテイクオーバーセマンティクス）は、実際の
> ワークロードでのみ表面化する。上記のティア言語は、3つすべてが初日から
> 交換可能であると装うのではなく、各プラットフォームに対してユーザーに正しい
> 期待値を与える。

### バンプ後の修正

- **`async::HttpResponse` headers map** — レスポンスサーフェスは現在、 `headers` ベクターを公開します。 `(name, value)` ペアはワイヤー順序と元のケーシングを保持し、さらに `get_header(name)` を大文字小文字を区別しないアクセサーとして提供します。Retry-After と Location は後方互換性のための専用フィールドとして残ります。これにより、以下の MCP セッショントラッキング修正が可能になります。
- **MCP `Mcp-Session-Id`ヘッダートラッキング** — Sem 2.6のhttplib→async_post移行でこれが静かに失われました。初期化後のすべてのRPCは、新しいヘッダーアクセサーを介してサーバー割り当てのセッションIDをエコーバックするため、サーバーのセッション状態はルーティング可能なままです。
- **MCP stdio 待機可能ミューテックス** — `StdioSession::rpc_call_async` は `std::mutex` を使用していたが、同じシングルスレッド io_context 上の2つのコルーチンが同じセッションを呼び出したときにデッドロックした（2番目の `lock_guard` が、1番目が必要とするワーカーをブロックした）。容量1のセマフォである `asio::experimental::channel<void(error_code)>` に置き換え、2番目の取得者が協調的にサスペンドするようにした。
- **`PostgresCheckpointStore` async peers** — 8つの CheckpointStore 非同期メソッド（`save_async`、`load_latest_async`、`load_by_id_async`、`list_async`、`delete_thread_async`、`put_writes_async`、`get_writes_async`、`clear_writes_async`）はすべて真の非同期になった。内部構造: `PQsetnonblocking(1)` + `PQsendQueryParams` + `asio::posix::stream_descriptor` を `PQsocket()` + `co_await sock.async_wait(wait_read/wait_write)` 上で使用。4スロットのプール上での4つの並行 `save_async` 呼び出しは、`run_sync` を介して直列化されるのではなく、ワイヤーレベルで並列に commit-fsync されるようになった。

---

## [0.1.0] — 2026-04 より前

プレリリース開発。公開 API の安定性は保証されません。
