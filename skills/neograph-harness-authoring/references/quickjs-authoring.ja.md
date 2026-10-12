<!-- neograph-i18n: source=skills/neograph-harness-authoring/references/quickjs-authoring.md locale=ja source_sha256=53dc89ee33f26b7d0f9c6d328453f73aaaf9e53f0b8b4cea360ca1cdc2b800c8 -->
# QuickJSソースの書き方

**Languages:** [English](quickjs-authoring.md) | [한국어](quickjs-authoring.ko.md) | [日本語](quickjs-authoring.ja.md) | [简体中文](quickjs-authoring.zh-CN.md)

## ホストの契約を確認する

コードを書く前に、次のバインディングを確認します。渡された設定を使うか、利用できるスキーマを調べてください。足りない名前を、それらしい推測で埋めてはいけません。

| 契約 | 必要な情報 |
|---|---|
| JS APIマニフェスト | このビルドにおける、ビルダーとコマンドの正確なシグネチャ |
| レジストリ | ノード、リデューサー、条件、インポートの名前、ノードの設定スキーマ、エフェクト |
| 呼び出し | 入力のJSONの形と、callCoreで使えるCoreの名前 |
| 結果 | Coreのチャネル名と、ジェネレーターが最後に出力すべきもの |
| 子 | admission済みのバインディング名、子の入力と出力、付与された上限 |
| コンパイラーブリッジ | 受け付けるソースのエンベロープ、診断、残りの修復の回数 |

組み込みのJSマニフェストに載っているのは、構文です。アプリケーションのノードは載っていません。たとえば、probe.nodeはケイパビリティのテスト用パレットのノードで、アプリケーションのLLMノードではありません。固定のケイパビリティプローブが確認するのは、そのケースに固有のグラフとコマンドの契約だけです。本番での任意の動作は確認しません。

## コンパイル時のグラフと実行時のProgram

**define()** は、コンパイル時に実行される同期関数で、開いたビルダーを1つ返します。ノードとエッジは、そのビルダーで作ります。ビルダーのメソッドが返すのはビルダーで、ノードのハンドルではありません。ノードの挙動は、ホストが登録したC++の実装で決まります。通常のJSのコールバックは、グラフのノードではありません。

**main(input)** は、あとで実行される、省略可能な同期ジェネレーターです。純粋なJSで、分岐の選択、配列やループの構築、JSONの変換ができます。実行時のエフェクトは、ジェネレーターからyieldする、封印されたngコマンドで行います。define()の中で実行時のコマンドを呼び出したり、公開済みのグラフを変更したり、グラフの形をしたオブジェクトを返したりしてはいけません。async関数、Promise、タイマー、require()、環境のI/O、eval、動的インポートも使えません。

次の配線の例は、ホストがプローブ用のパレットを登録していて、入力のvalueが数値であることを前提にしています。ノードは何もしないので、これはデータの流れを示す例です。LLMの動作でも、名前の付いたケイパビリティのケースの答えでもありません。

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

callCoreには、ホストが指定したグラフ名をそのまま使ってください。"main"、"example"、"capability"、ノードの名前のどれも、どこでも使える別名ではありません。

## データの流れとトポロジー

通常のcallCoreの入力は、チャネル名から受け取る値へのマップです。宣言された各チャネルには、登録されたリデューサーが適用されます。通常のCoreの結果には、シリアライズされたチャネルが入っています。読み取るのは、ラッパーではなく、チャネルの値です。

たとえば、チャットボットの既存のテンプレートは、回答のノードを次のように呼び出します。

~~~javascript
const reply = yield ng.callCore(
  "main", {payload: {phase: "answer", task: task}}, "answer"
);
const answer = reply.channels.result.value;
~~~

ここで、payloadとresultは宣言されたチャネルで、chat.stepはpayload.phaseを読みます。これらの名前と挙動は、そのホストのレジストリとテンプレートで決まります。このコードの断片を別のレジストリにコピーしても、これらのバインディングが作られるわけではありません。

宣言だけのモジュールは、Coreの結果の形のままです。main()がある場合、ジェネレーターの戻り値は、別にadmissionを通ったProgramの出力の契約に一致していなければなりません。ケイパビリティの評価器は、{accepted: true}のような、合成したコマンドのレスポンスを返すことがあります。その場合は、Coreのチャネルのラッパーを付け足さずに、そのケースに示されたレスポンスの契約に従ってください。

| 意図 | 構築方法と重要な点 |
|---|---|
| 直線のパス | ノード、エントリー／イグジット、それらをつなぐすべてのエッジを追加する |
| 条件付きルーティング | conditionalEdge(from, registeredCondition, routes)。条件のすべてのラベルを、ノードに対応づける |
| 静的なfan-out／fan-in | 出ていく分岐と、joinに入るエッジの両方を追加する。joinがすべての分岐を必要とする場合は、barrier(joinNode, branchNames)を追加する |
| 並列の書き込み | 並行する書き込みを扱えるリデューサーとチャネルを、ホストのものから選ぶ。エッジだけではマージは定義されない |
| グラフの割り込み／リトライ | マニフェストのinterruptBefore／AfterとretryPolicyのキーを使う。JSのループや、論理的なリトライとは別のもの |

## 実行時のコマンドを組み立てる

- callCore(coreName, input, site) はコマンドを作ります。それをyieldするとCoreが呼び出され、その結果が返ります。
- all(commands, {max_in_flight: N}, site) は、封印されたコマンドを受け取ります。リストは通常のJSで作り、allコマンドを1回だけyieldします。生の配列をyieldしたり、コマンドをPromiseに変えたり、封印されたコマンドにyield*を使ったりしてはいけません。
- spawn(binding, input, site) は、ホストがadmissionに通した子のバインディングを選びます。その結果を待つには、await(spawnCommand, timeoutMs, site) でコマンドを包みます。
- checkpoint(state, site) は、明示的なJSONのステートを公開します。これだけでは、コンパイル、admission、置き換えのどれも行われません。
- emitとcancelScopeの意味は、マニフェストに宣言されているとおりです。hostCapabilityにはadmission済みのインポートスロットが必要で、任意のネイティブAPIへの入り口ではありません。

たとえば、admission済みのバインディングと、元の入力が正確にわかっている場合は、次のようになります。

~~~javascript
const result = yield ng.await(
  ng.spawn(binding, originalChildInput, "child:spawn"),
  timeoutMs,
  "child:wait"
);
~~~

emitされた子のIDは、それ自体では、封印されたawaitコマンドではありません。ホストの実際のjoinと復旧の契約を使ってください。置き換えをまたいで子を保持する場合は、[runtime-handoffs.md](runtime-handoffs.md)を参照してください。

ソース上のsiteラベルは、永続化される座標の一部になります。安定したタスクの識別子か、決定的なインデックスから導出し、時計やランダムな値は避けてください。完了済みの操作は、リプレイのときも元の入力を保つ必要があります。JSのループ、リトライ、並列の分岐、新しい子によって、予算が補充されることはありません。

acceptedがfalseのような、成功した結果に基づくループは、論理的なリトライです。失敗したCoreコマンドはProgramの結果です。通常のJSのtry/catchで再開できると考えてはいけません。ホストの失敗、再開、照合の契約を使ってください。

## 出力、コンパイル、修復

有効なインターフェースが求めるエンベロープを使います。
- ソースの評価：JSONオブジェクトをちょうど1つ返し、そのsource文字列が、モジュール全体であること。Markdownのコードフェンス、パッチ、ProgramBundleのJSON、説明の文章は付けません。
- Harness MCP：最初に neograph_schema を調べます。モジュールは、harness.mode="javascript"、source_id、sourceに入れ、必須のタスク、ワーカー、予算、ポリシーのフィールドもすべて指定します。sourceだけでは、MCPの完全なリクエストになりません。
- ホスト固有のプロポーザル：そのホストのスキーマに従います。このSkillは、どこでも使える自由形式のコンパイルや置き換えのRPCを定義していません。

提出は、渡されたコンパイラーブリッジを通して行います。評価モードでは、返したソースをホストが提出し、診断を返します。モデルから呼び出せるツールがあるとは限りません。MCPモードでは、neograph_compileが使える場合にだけ、それを使います。

失敗した場合は、報告されたコード、パス、ソースのsiteを手がかりに、違反している契約を直します。たとえば、次のとおりです。
- 未知のノード／リデューサー／条件：登録されたバインディングと設定に合わせる。
- Coreのバインディングの誤り：コマンド全体で、admission済みのグラフ名に合わせる。
- コマンドでないyield：封印されたコマンドを1つ出すか、admission済みの構造化されたjoinを使う。
- 出力／入力の誤り：チャネルの対応づけか、Programの結果の契約を直す。
- エッジ／バリアの不足：ソースの書き方ではなく、ロワリング後のトポロジーを直す。

修復の残り回数の範囲で、完全な置き換え用のソースを返します。診断を消したり、グラントを広げたり、無関係な操作を加えたりしてはいけません。コンパイラーとタスク固有のチェックが受理する前に、成功したと主張してもいけません。

チェックアウトにあるケイパビリティのブリッジは、次のように確認して使えます。

~~~text
program_dsl_capability_probe --manifest
program_dsl_capability_probe graph_basics source.js
~~~

ビルドターゲットは program_dsl_capability_probe です。名前の付いたケースには、それぞれ別の契約が渡されます。コンパイルが通っただけでは、ケースに合格したことになりません。プローブが成功しても、本番での実行が許可されるわけではありません。ランナーのscripts/run_dsl_capability_eval.tsは、上限付きで、モデルと診断の反復を実行します。
