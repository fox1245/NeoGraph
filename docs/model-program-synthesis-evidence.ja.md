<!-- neograph-i18n: source=docs/model-program-synthesis-evidence.md locale=ja source_sha256=b0a57aa0f6c4a726e417d88daf8d0e3f90178de20d610884fcba052b0f622105 -->
# モデル生成QuickJS Program合成エビデンス

**Languages:** [English](model-program-synthesis-evidence.md) | [한국어](model-program-synthesis-evidence.ko.md) | [日本語](model-program-synthesis-evidence.ja.md) | [简体中文](model-program-synthesis-evidence.zh-CN.md)

- ステータス: 有界PoC検証済み
- 観測: 2026-08-21
- モデル: `deepseek/deepseek-v4-flash-0731`（OpenRouter経由）
- プロバイダー応答: `gen-1787288110-o3PCpNZgsnE8eyQF1TzM`

## 検証済みチェーン

1件のモデル応答がこのQuickJS Programソースを生成した:

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

ホストはこの提案を直接実行しなかった。プローブは以下を強制した:

```text
model source
  -> immutable ProgramSynthesisProposal
  -> nonrenewable dynamic-compile reservation
  -> bounded QuickJS ProgramCompiler
  -> independent ProgramCatalog admission
  -> immutable ProgramVersion publication
  -> ProgramRuntime execution
```

チェックイン済みフィクスチャは同一ホストパイプラインを決定論的に検証する。その永続的識別子は:

| 証拠 | アイデンティティ |
|---|---|
| Programソース | `sha256:4e994637bfa31884f3a0090ffee7b0135f591656ee6217448d435d4a2b6384a3` |
| Proposal | `sha256:c51cd4737dc19939ee25a08799e9308a4dbf8943bffbcbd225e0cc9d7e361347` |
| 予約 | `sha256:531ed6ed5fd713f8d5eda5d3d26df1bedf36d59d44d35b2af8cbfa53ff1ec628` |
| バンドル | `sha256:3f21798666ddc5ad76c73ba9706e93db032064b76e68a4965f0bc49b2c89a375` |
| ProgramVersion | `sha256:71b0ea551fc37ccd92b89c740b58824752397046d5fef78e13a4c21adca84728` |
| 合成レシート | `sha256:350775f4c0bc9bb40937cec5c91fd5887f5678c7af349aa177406cec9c5e2f99` |

カタログ検索により、承認済みの正確なバージョンが見つかりました。Programの実行はトレース`seed -> double -> finish`で完了し、すべてのノードは正確に1回実行されました。最終チャネルは`value = 12`と`path = "model-generated"`でした。

## ランタイムトポロジー置換

合成ゲートウェイは現在、ホスト所有の`ProgramBudgetBounds`配下で後続をコンパイルする。すべてのリソースの上限は予約の`remaining_after_reservation`であり、したがって、すでにデビット処理された系統から導出された権限上限であり、モデル出力から導出されるものではない。下限はJavaScriptランタイム構造上のフロアのみである。つまり、壁面時間単位1つ、ワーカー1つ、Program操作1つ、Coreステップ1つである。消費可能および子の許可はゼロ・フロアを維持する。反転されたホスト境界はソース評価の前に拒否される。

これにより、正確な置換が、より小さな壁面時間デビット済みの系統残りを保持したまま伝搬でき、非再生可能な予算を拡大することはない。通常の正確な予算コンパイラのオーバーロードは固定スタンドアロン呼び出しのために引き続き利用可能である。

別のソースProgramが、永続的なトップレベルの`ng.checkpoint`に到達した。ホストはそのハンドオフを消費し、モデルトポロジーを後継者として`ProgramRuntime::replace`を呼び出した。ターゲットは上記の合成ゲートウェイの正確なProgramVersionであり、2番目のバンドルはコンパイルも許可もされなかった。フィクスチャの遷移は以下を生成した:

| 証拠 | 値 |
|---|---|
| ソースProgramVersion | `sha256:24d9f2b64ee55e212039d31fe8d9b59a619b0b285346730b06d712f10716c09f` |
| ソース実行 | `run-e153d50d90cc8d222c5f363c99399569` |
| ターゲット実行 | `sha256:590048ab76d42119e184f89eb88701cd8df32a84bc244286f415c0b53086a089` |
| ターゲットProgramVersion | `sha256:71b0ea551fc37ccd92b89c740b58824752397046d5fef78e13a4c21adca84728` |
| 置換レシート | `sha256:79a334ee185bf2ac0115d7d79038c2adff7b4bb854f294c7a5585924d8376fc3` |
| アクティブ世代 | `2` |
| ターゲットステータス | `completed` |

古いソースノードはゼロ回実行された。後継トレースは`seed -> double -> finish`であり、すべての後継ノードが1回実行され、最終出力には再び`value = 12`および`path = "model-generated"`が含まれていた。

最新のライブDeepSeek実行は`replacement_uses_synthesis_version =
true`、ターゲットステータス`completed`、アクティブ生成`2`、および同じゼロ・ステイル・ノード結果を報告しました。

## 否定的な証拠とプロンプト契約

以前のモデル出力は実行前に拒否されました：

- `P_JS_DEFINE_MISSING`: 同期エクスポート`define()`がない；
- `P_JS_DEFINE_VALUE`: `define()`は、不透明な`ng.graph()`ビルダーではなくプレーンなグラフ形状のデータを返した；そして
- `P_JS_GRAPH_ARGUMENT`: チャネル/ノード・ビルダー引数がレビュー済みDSLスキーマに一致しなかった；および
- `P_JS_EVALUATION`: 引用符で囲まれていないリデューサ識別子が、有界なQuickJSコンテキストに存在しないアンビエント状態を参照した。

したがって、成功したプロンプトは、信頼された正確なオーサリング面を指定した： `ng.graph`、`graph.channel`（`initial`付き）、`graph.node(name, {type})`、entry、edge、exit、および封印された`ng.callCore`コマンド。無効な提案はProgramVersionを生成せず、ノードも実行しなかった。

## スコープ境界

これは、外部モデルがQuickJSトポロジーソースを合成できることを証明し、そのソースが、持続的なランタイムチェックポイントにおいて別のProgram世代として予約、コンパイル、admission、公開、実行、選択されることを裏付けている。さらに、合成ゲートウェイ自身のadmission版が、そのsuccessorになり得ることを証明する（動的コンパイルのデビット後）。ランタイムは提案を権威として再利用せず、置換パス上でソースを再コンパイルもしない。

このProgramレベルの置換は、任意のGraphEngine状態/フロンティア移行と混同してはならない。ソースCoreトポロジーからモデルトポロジーへの移行プランは、そのマテリアライゼーションとランタイム契約が異なるため、`blocked`として正しく分類された。

NeoGraphには現在、意図的に狭いP1 GraphEngineパスも存在する: `GraphSemanticMigrationAdapter`。ホストはこの不変アダプターを、正確に承認されたソースおよびターゲットのアーティファクトから準備しなければならない。これは、宣言のみ（ランタイムJavaScript制御なし）、単一ルートの `call_core` 同一のチェックポイント化されたチャンネル/リデューサー、ノード名、エッジ、ルーティング、バリア、リトライ/割り込み形状、ケーパビリティバインディング、オーソリティ、および入出力契約を持つProgramのみを受理する。したがって、アイデンティティマップされたフロンティアとチャンネルスナップショットを、異なるシールドされたCore定義とコンパイル済みプラン識別子を持つ後継 Program へ運ぶことができる。このアダプターはマイ modding, マイグレーション領収書に保存され、リカバリ中に再検証される。

QuickJS制御、ノード/フロンティアの改名、チャンネルまたはレデューサーの変換、変更されたバリアメンバーシップ、ペンディングエフェクト、子、任意のトポロジー編集は、依然としてフェイルクローズである。これらの場合、後のマッピングクラスが影響を受けるすべての状態次元を証明するまで、明示的なハンドオフ/再スタートを継続的に必要とする。自動チャイルドバインド/スポン、すべての合成境界にまたがるクラッシュリカバリ、およびin-Program `ng.proposeProgram`コマンドサーフェスも、別個の認証ゲートのままである。

## モデル生成P1 GraphEngine移行

P1アダプターは、ライブの`deepseek/deepseek-v4-flash-0731`OpenRouter応答`gen-1787291529-fCOHp8pry7EwHHHu1MUH`でエンドツーエンドにわたって実行した。モデルは、宣言型のみのQuickJS`define()`ソース（SHA-256`346329bf39790cc5557a9961a7faa5da0b35168f84257b12d6166565d594df08d`）を生成した。そのトポロジーは、ソースグラフの`work -> followup`フロンティア形状を保存しつつ、`migration_epoch: 2`を通じて明確なターゲットCore定義を導入した。

```text
model QuickJS define()
  -> ProgramSynthesisProposal
  -> dynamic-compile reservation
  -> ProgramCompiler + ProgramCatalog admission
  -> GraphSemanticMigrationAdapter preparation
  -> durable GraphEngine generation-2 migration
  -> recovery-proof validation
```

通常の移行プランは、変更されたバンドル/マテリアライゼーションに必要な`blocked`のままだ。ホスト作成のアダプタは、狭いアイデンティティ投影をadmissionした。ターゲットは世代`2`で完了した；`work`はソース世代で一度実行され、`followup`は後継で一度実行された。正確なアダプタ識別子は、移行レシートに永続化された。

`program_model_synthesis_probe` ターゲットと以下を使って再現します:

```powershell
bun run scripts/run_model_program_synthesis_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_synthesis_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```

GraphEngine P1パスには、`program_model_semantic_migration_probe`を使用し、以下を適用します:

```powershell
bun run scripts/run_model_semantic_migration_probe.ts `
  --probe build-agent-vs/tests/Release/program_model_semantic_migration_probe.exe `
  --model deepseek/deepseek-v4-flash-0731
```
