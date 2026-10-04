<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/quickjs-authoring.md locale=ja source_sha256=53dc89ee33f26b7d0f9c6d328453f73aaaf9e53f0b8b4cea360ca1cdc2b800c8 -->
# QuickJS ソースの作成

**Languages:** [English](quickjs-authoring.md) | [한국어](quickjs-authoring.ko.md) | [日本語](quickjs-authoring.ja.md) | [简体中文](quickjs-authoring.zh-CN.md)

## ホスト契約の確認

コードを書く前に、次のバインディングを特定してください。提供された設定を使うか、
利用可能なスキーマを調べます。欠けている名前を、もっともらしい推測で埋めてはいけません。

| 契約 | 必要な情報 |
|---|---|
| JS API マニフェスト | このビルドの正確なビルダーとコマンドのシグネチャ |
| レジストリ | ノード／リデューサー／条件／インポートの名前、ノード設定スキーマと作用 |
| 呼び出し | 入力 JSON の形と callCore で呼び出せる Core 名 |
| 結果 | Core のチャネル名と、ジェネレーターに要求される終端出力 |
| 子 | 受け入れ済みバインディング名、子の入出力、付与された制限 |
| コンパイラーブリッジ | 受理するソースエンベロープ、診断、残りの修正許容枠 |

組み込み JS マニフェストが列挙するのは構文であり、アプリケーションのノードではありません。
たとえば probe.node は能力テスト用パレットに属し、アプリケーションの LLM ノードではありません。
固定の能力プローブが検査するのは各ケース固有のグラフ／コマンド契約であり、
任意の本番動作ではありません。

## コンパイル時のグラフと実行時の Program

**define()** は同期的なコンパイル時関数で、変更可能なビルダーを一つ返します。
そのビルダーでノードとエッジを作成します。メソッドはノードハンドルではなくビルダーを返します。
ノードの動作はホストに登録された C++ 実装によって決まります。通常の JS コールバックは
グラフノードではありません。

**main(input)** は省略可能な同期ジェネレーターで、後から実行されます。純粋な JS で分岐を選び、
配列やループを構築し、JSON を変換できます。実行時の作用には、ジェネレーターから yield する
封印済み ng コマンドを使います。define() から実行時コマンドを呼び出したり、公開済みグラフを
変更したり、グラフの形をしたオブジェクトを返したりしてはいけません。async 関数、Promise、
タイマー、require()、暗黙に利用できる I/O、eval、動的インポートも使ってはいけません。

次の接続例は、ホストが probe パレットを登録済みで、数値の input.value を要求する場合を
想定しています。このノードは何もしないため、示すのはデータフローであり、LLM の動作や
名前付き能力ケースへの回答ではありません。

~~~javascript
export function define() {
  const g = ng.graph("example");
  g.channel("value", {reducer: "probe.overwrite", initial: 0});
  g.node("step", {type: "probe.node"});
  g.entry("step");
  g.exit("step");
  return g;
}

export function* main(input) {
  const result = yield ng.callCore("example", {value: input.value}, "copy:value");
  return {value: result.channels.value.value};
}
~~~

callCore にはホストが提供したグラフ名を正確に指定してください。"main"、"example"、
"capability"、ノードの名前のいずれも、共通の別名ではありません。

## データフローとトポロジー

通常の callCore 入力は、チャネル名から入力値へのマップです。宣言された各チャネルが、
登録済みリデューサーを適用します。通常の Core 結果にはシリアライズされたチャネルが含まれます。
チャネルのラッパーではなく、その value を読んでください。

たとえば、チャットボットの既存テンプレートは、回答ノードを次のように呼び出します。

~~~javascript
const reply = yield ng.callCore(
  "main", {payload: {phase: "answer", task: task}}, "answer"
);
const answer = reply.channels.result.value;
~~~

ここでは payload/result が宣言済みチャネルで、chat.step が payload.phase を読みます。
これらの名前と動作は、そのホストのレジストリ／テンプレートから来ています。この断片を
別のレジストリにコピーしても、これらのバインディングは作成されません。

宣言だけのモジュールは Core の結果の形を保持します。main() がある場合、ジェネレーターの
戻り値は、別途受け入れられた Program 出力契約に適合する必要があります。能力評価器は、
{accepted: true} のような合成コマンド応答を提供することがあります。合成応答に Core の
チャネルラッパーを追加するのではなく、そのケースで指定された応答契約を使ってください。

| 目的 | 構築方法と注意点 |
|---|---|
| 直線的な経路 | ノード、入口／出口、それらをつなぐすべてのエッジを追加します |
| 条件ルーティング | conditionalEdge(from, registeredCondition, routes) を使い、すべての条件ラベルをノードに対応付けます |
| 静的な分岐／合流 | 分岐する両方の出力エッジと、合流先への入力エッジを追加します。合流にすべての分岐が必要なら barrier(joinNode, branchNames) を追加します |
| 並列書き込み | 同時書き込みを処理できるホストのリデューサー／チャネルを選びます。エッジだけではマージは定義されません |
| グラフの割り込み／再試行 | マニフェストの interruptBefore/After と retryPolicy キーを使います。これらは JS のループや論理的な再試行とは別のものです |

## 実行時コマンドの組み合わせ

- callCore(coreName, input, site) はコマンドを生成します。それを yield すると Core を
  呼び出し、結果を返します。
- all(commands, {max_in_flight: N}, site) は封印済みコマンドを受け取ります。通常の JS で
  リストを作り、all コマンドを一度 yield します。生の配列を yield したり、コマンドを
  Promise に変換したり、封印済みコマンドに yield* を使ったりしてはいけません。
- spawn(binding, input, site) はホストが受け入れた子バインディングを選びます。結果を
  待つには、そのコマンドを await(spawnCommand, timeoutMs, site) で包みます。
- checkpoint(state, site) は明示的な JSON 状態を公開します。それ単独ではコンパイル、
  受け入れ、置換を行いません。
- emit と cancelScope の意味はマニフェストの宣言に従います。hostCapability には
  受け入れ済みインポートスロットが必要で、任意のネイティブ API への経路ではありません。

たとえば、受け入れ済みバインディングと、その正確な元の入力がある場合:

~~~javascript
const result = yield ng.await(
  ng.spawn(binding, originalChildInput, "child:spawn"),
  timeoutMs,
  "child:wait"
);
~~~

emit された子 ID 自体は、封印済み await コマンドではありません。ホストの実際の
合流／回復契約を使ってください。置換をまたいで子を保持する場合は、
[runtime-handoffs.md](runtime-handoffs.md) を参照してください。

ソース位置ラベルは永続座標の構成要素です。安定したタスク識別子や決定的なインデックスから
導出し、時刻や乱数の値を避けてください。既存の完了済み操作は、リプレイ時にも元の入力を
保持する必要があります。JS のループ、再試行、並列分岐、新しい子によって予算が補充されることはありません。

accepted=false のような、成功した結果に基づくループは論理的な再試行です。失敗した Core
コマンドは Program の結果となります。通常の JS の try/catch で再開できると仮定してはいけません。
ホストの失敗／再開／照合契約を使ってください。

## 出力、コンパイル、修正

利用中のサーフェスが要求するエンベロープを使います。
- ソース評価: 完全なモジュールを source 文字列に含む、厳密に一つの JSON オブジェクト。
  Markdown フェンス、パッチ、ProgramBundle JSON、説明文を含めません。
- Harness MCP: まず neograph_schema を調べます。モジュールを harness.mode="javascript"、
  source_id、source に指定し、必須の task/worker/budget/policy フィールドもすべて含めます。
  ソースだけでは完全な MCP リクエストになりません。
- ホストネイティブの提案: そのホストのスキーマに従います。このスキルは、自由形式の
  汎用コンパイル RPC や置換 RPC を定義するものではありません。

提供されたコンパイラーブリッジ経由で送信してください。評価モードでは、ホストが返された
ソースを送信して診断を返します。モデルから呼び出せるツールの存在を意味するものではありません。
MCP モードでは、neograph_compile ツールが利用できる場合に限り使います。

失敗した場合は、報告されたコード／パス／ソース位置を特定し、違反した契約を修正します。
たとえば:
- 不明なノード／リデューサー／条件: 登録済みのバインディングと設定に合わせます。
- 誤った Core バインディング: コマンド全体で、受け入れ済みグラフ名に合わせます。
- コマンドでない yield: 封印済みコマンド一つ、または受け入れ済みの構造化された合流を生成します。
- 誤った出力／入力: チャネルの対応付け、または Program の結果契約を修正します。
- エッジ／バリアの不足: ソースの表現だけでなく、変換後のトポロジーを修正します。

既存の修正許容枠内で、完全な置換ソースを返してください。診断を消したり、権限を拡大したり、
無関係な操作を追加したり、コンパイラーとタスク固有の検査が受け入れる前に成功を主張したりしてはいけません。

このチェックアウトの能力ブリッジは、次のように調べて使えます。

~~~text
program_dsl_capability_probe --manifest
program_dsl_capability_probe graph_basics source.js
~~~

ビルドターゲットは program_dsl_capability_probe です。名前付きケースには、それぞれ提供された
契約が必要です。コンパイルが成功しただけではケースに合格したことにならず、プローブの成功も
本番実行の権限を与えません。ランナー scripts/run_dsl_capability_eval.ts は、上限を定めた
モデル／診断の反復を行います。
