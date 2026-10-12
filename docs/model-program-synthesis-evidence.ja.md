<!-- neograph-i18n: source=docs/model-program-synthesis-evidence.md locale=ja source_sha256=c34041ccc8bf5143dd152f292a26c02eb4a487757b4657e0ecfe75b4740a3373 -->
# モデルが生成したQuickJS Programの合成のエビデンス

**Languages:** [English](model-program-synthesis-evidence.md) | [한국어](model-program-synthesis-evidence.ko.md) | [日本語](model-program-synthesis-evidence.ja.md) | [简体中文](model-program-synthesis-evidence.zh-CN.md)

- 状況：範囲を限った、過去のPoCのエビデンスです。現在のバインディングを新たに検証したものではありません
- 観測日：2026-08-21
- モデル：OpenRouter経由の `deepseek/deepseek-v4-flash-0731`
- プロバイダーのレスポンス：`gen-1787288110-o3PCpNZgsnE8eyQF1TzM`

## 検証した一連の流れ

1回のモデルの応答から、次のQuickJS Programのソースが生成されました。

```javascript
export function define() {
    const graph = ng.graph("model-synthesized");
    graph.channel("value", { reducer: "probe.overwrite", initial: 0 });
    graph.channel("path", { reducer: "probe.overwrite", initial: "" });
    const pairs = [["seed", "probe.seed"], ["double", "probe.double"], ["finish", "probe.finish"]];
    for (const [name, type] of pairs) {
        graph.node(name, {type});
    }
    graph.entry("seed");
    graph.edge("seed", "double");
    graph.edge("double", "finish");
    graph.exit("finish");
    return graph;
}

export function* main(input) {
    return yield ng.callCore("model-synthesized", input, "model-generated:1");
}
```

ホストは、この提案を直接実行しませんでした。プローブは、次の手順を強制しました。

```text
model source
  -> immutable ProgramSynthesisProposal
  -> nonrenewable dynamic-compile reservation
  -> bounded QuickJS ProgramCompiler
  -> independent ProgramCatalog admission
  -> immutable ProgramVersion publication
  -> ProgramRuntime execution
```

リポジトリに含まれるフィクスチャは、同じホストのパイプラインを、決定的に検証します。その永続的なIDは、次のとおりです。

| エビデンス | ID |
|---|---|
| Programのソース | `sha256:4e994637bfa31884f3a0090ffee7b0135f591656ee6217448d435d4a2b6384a3` |
| 提案 | `sha256:c51cd4737dc19939ee25a08799e9308a4dbf8943bffbcbd225e0cc9d7e361347` |
| 予約 | `sha256:531ed6ed5fd713f8d5eda5d3d26df1bedf36d59d44d35b2af8cbfa53ff1ec628` |
| バンドル | `sha256:3f21798666ddc5ad76c73ba9706e93db032064b76e68a4965f0bc49b2c89a375` |
| ProgramVersion | `sha256:71b0ea551fc37ccd92b89c740b58824752397046d5fef78e13a4c21adca84728` |
| 合成のレシート | `sha256:350775f4c0bc9bb40937cec5c91fd5887f5678c7af349aa177406cec9c5e2f99` |

Catalogの検索で、admission済みの正確なバージョンが見つかりました。Programの実行は、トレース `seed -> double -> finish` で完了し、すべてのノードがちょうど1回ずつ実行されました。最終的なチャネルは、`value = 12` と `path = "model-generated"` でした。

## 実行時のトポロジーの置き換え

合成ゲートウェイは、ホストが所有する `ProgramBudgetBounds` のもとで、後継をコンパイルするようになりました。すべてのリソースの最大値は、予約の `remaining_after_reservation` です。つまり、すでに引き落とし済みの系統から導かれる権限の上限であり、モデルの出力から決まるものではありません。下限は、JavaScriptランタイムの構造上の最小値だけで、実時間1単位、ワーカー1つ、Programのオペレーション1つ、Coreのステップ1つです。消費されるグラントと子のグラントの下限は、0のままです。大小が逆転しているホストの境界は、ソースを評価する前に拒否されます。

これにより、正確な置き換えが、実時間を引き落とした後の、より小さな系統の残りを引き継げます。更新できない予算が、広がることはありません。固定の単独の呼び出しのために、通常の、予算を正確に指定するコンパイラーのオーバーロードも、引き続き使えます。

別のソースのProgramが、永続的なトップレベルの `ng.checkpoint` に到達しました。ホストはそのhandoffを消費し、モデルが作ったトポロジーを後継として、`ProgramRuntime::replace` を呼び出しました。置き換え先は、上の、合成ゲートウェイが作った正確なProgramVersionです。2つ目のバンドルのコンパイルやadmissionは行っていません。フィクスチャの遷移の結果は、次のとおりです。

| エビデンス | 値 |
|---|---|
| 置き換え元のProgramVersion | `sha256:24d9f2b64ee55e212039d31fe8d9b59a619b0b285346730b06d712f10716c09f` |
| 置き換え元の実行 | `run-e153d50d90cc8d222c5f363c99399569` |
| 置き換え先の実行 | `sha256:590048ab76d42119e184f89eb88701cd8df32a84bc244286f415c0b53086a089` |
| 置き換え先のProgramVersion | `sha256:71b0ea551fc37ccd92b89c740b58824752397046d5fef78e13a4c21adca84728` |
| 置き換えのレシート | `sha256:79a334ee185bf2ac0115d7d79038c2adff7b4bb854f294c7a5585924d8376fc3` |
| アクティブな世代 | `2` |
| 置き換え先のステータス | `completed` |

古い置き換え元のノードは、1回も実行されませんでした。後継のトレースは `seed -> double -> finish` で、後継のすべてのノードが1回ずつ実行され、最終的な出力には、再び `value = 12` と `path = "model-generated"` が含まれていました。

直近の、実際のDeepSeekでの実行は、`replacement_uses_synthesis_version =
true`、置き換え先のステータス `completed`、アクティブな世代 `2`、そして同じく、古いノードが0回だったという結果を報告しました。

## 否定的なエビデンスと、プロンプトの契約

以前のモデルの出力は、実行される前に拒否されました。

- `P_JS_DEFINE_MISSING`：同期の、エクスポートされた `define()` がない
- `P_JS_DEFINE_VALUE`：`define()` が、不透明な `ng.graph()` ビルダーではなく、グラフの形をしただけの通常のデータを返した
- `P_JS_GRAPH_ARGUMENT`：チャネルやノードのビルダーの引数が、レビュー済みのDSLのスキーマに合っていない
- `P_JS_EVALUATION`：引用符で囲まれていないリデューサーの識別子が、機能を絞ったQuickJSのコンテキストには存在しない、環境依存の状態を参照した

そのため、成功したプロンプトでは、信頼できるオーサリングの範囲を、正確に指定しました。`ng.graph`、`initial` を持つ `graph.channel`、`graph.node(name, {type})`、entry、edge、exit、シール済みの `ng.callCore` コマンドです。不正な提案からは、ProgramVersionは作られず、ノードも実行されませんでした。

## 範囲の境界

これによって、外部のモデルが、QuickJSのトポロジーのソースを合成でき、そのソースが、予約、コンパイル、admission、公開、実行され、永続的なランタイムのチェックポイントで、別のProgramの世代として選ばれうることが示されました。さらに、合成ゲートウェイ自身がadmissionしたバージョンが、動的コンパイルの引き落としの後で、その後継になれることも示されました。ランタイムは、提案を権限として再利用することも、置き換えの経路でソースをコンパイルし直すこともありません。

このProgramレベルの置き換えを、GraphEngineの状態とフロンティアの任意のマイグレーションと、混同してはいけません。置き換え元のCoreトポロジーから、モデルが作ったトポロジーへのマイグレーション計画は、実体化とランタイムの契約が異なっていたため、正しく `blocked` に分類されました。

NeoGraphには、意図的に範囲を狭くした、P1のGraphEngineの経路として、`GraphSemanticMigrationAdapter` もあります。ホストは、admission済みの、置き換え元と置き換え先のアーティファクトから、この変更できないアダプターを用意する必要があります。このアダプターがadmissionするのは、宣言だけで（実行時のJavaScriptによる制御がない）、`call_core` のルートが1つのProgramで、チェックポイントされるチャネルとリデューサー、ノード名、エッジ、ルーティング、バリア、リトライと割り込みの形、capabilityのバインディング、権限、入力と出力の契約が、すべて同一のものだけです。そのため、シール済みのCore定義とコンパイル済みプランのIDが異なる後継に、恒等写像でのフロンティアとチャネルのスナップショットを引き継げます。アダプターは、マイグレーションのレシートに保存され、復旧のときに再検証されます。

QuickJSによる制御、ノードやフロンティアの名前の変更、チャネルやリデューサーの変換、バリアのメンバーの変更、保留中の効果、子、任意のトポロジーの編集は、引き続きフェイルクローズです。これらの場合は、後の写像のクラスが、影響を受けるすべての状態の次元を証明するまで、明示的なhandoffと再起動が必要です。子の自動バインディングとspawn、すべての合成の境界にまたがるクラッシュからの復旧、Program内の `ng.proposeProgram` コマンドも、別個の適格性確認のゲートのままです。

## モデルが生成したP1のGraphEngineマイグレーション

P1のアダプターは、実際の `deepseek/deepseek-v4-flash-0731` のOpenRouterのレスポンス `gen-1787291529-fCOHp8pry7EwHHHu1MUH` を使って、エンドツーエンドでも確認しました。モデルは、宣言だけのQuickJSの `define()` のソース（SHA-256 `346329bf39790cc5557a9961a7faa5da0b35168f84257b12d6166565d594df08d`）を生成しました。このトポロジーは、置き換え元のグラフの `work -> followup` というフロンティアの形を保ちながら、`migration_epoch: 2` によって、別の置き換え先のCore定義を導入しています。

```text
model QuickJS define()
  -> ProgramSynthesisProposal
  -> dynamic-compile reservation
  -> ProgramCompiler + ProgramCatalog admission
  -> GraphSemanticMigrationAdapter preparation
  -> durable GraphEngine generation-2 migration
  -> recovery-proof validation
```

通常のマイグレーション計画は、バンドルや実体化が変わっているため、想定どおり `blocked` のままでした。そのうえで、ホストが作成したアダプターが、範囲の狭い恒等写像による射影をadmissionしました。置き換え先は、世代 `2` で完了しました。`work` は置き換え元の世代で1回、`followup` は後継で1回、実行されました。アダプターの正確なIDは、マイグレーションのレシートに保存されました。

`program_model_synthesis_probe` ターゲットを使って、次のように再現できます。

```powershell
bun run scripts/run_model_program_synthesis_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_synthesis_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```

GraphEngineのP1の経路には、`program_model_semantic_migration_probe` を使い、次のようにします。

```powershell
bun run scripts/run_model_semantic_migration_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_semantic_migration_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```
