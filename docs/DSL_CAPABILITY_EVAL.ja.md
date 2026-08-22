<!-- neograph-i18n: source=docs/DSL_CAPABILITY_EVAL.md locale=ja source_sha256=a2b954c6e311d3acabd6fcc547511af245a49b2303e4897e12628ae7187191c4 -->
# QuickJS DSL 能力とモデル合成評価

**Languages:** [English](DSL_CAPABILITY_EVAL.md) | [한국어](DSL_CAPABILITY_EVAL.ko.md) | [日本語](DSL_CAPABILITY_EVAL.ja.md) | [简体中文](DSL_CAPABILITY_EVAL.zh-CN.md)

ステータス: 実装済み、決定論的適合性ゲート付き。ライブモデル評価はオプトイン。観測日: 2026-08-22

## 質問

NeoGraph は2つの主張を区別しなければならない:

1. 承認された QuickJS DSL が能力を表現できること、および
2. LLM がその能力を正しく実際に使用するソースを合成できること。

2番目の主張は最初の主張からは含意されない。ソースは、実際の `ProgramCompiler` がそれを受け入れ、ケース固有の意味検証が低レベル化された Core IR または封印された JavaScript コマンドツリーを確認した場合にのみ、この評価を通過する。ソーステキストのキーワード一致では不十分である。

## 能力マニフェスト

[`tests/fixtures/dsl_capabilities/cases.json`](../tests/fixtures/dsl_capabilities/cases.json) は機械可読な評価インベントリである。現在、以下をカバーしている:

- グラフ/チャネル/ノード/エントリ/エッジ/エグジットおよび通常の JavaScript 構築;
- 登録済み条件付きルーティング;
- 静的 fan-out、fan-in、およびバリア;
- 静的 HITL 割り込みおよびグラフ再試行ポリシー;
- レジストリ仲介による動的 `Send` および `NodeInterrupt` 動作;
- `callCore` と通常の JavaScript 分岐/ループ制御;
- 境界付き `ng.all` に低レベル化された JavaScript マップ;
- `all`、`parallel`、汎用の`join`、`race`、および`quorum`;
- `spawn`の子が`await`の内部にネストされている;
- `emit`、`checkpoint`、および`cancelScope`; そして
- 承認されたネイティブ`hostCapability`インポートスロット。

チェックインされた各JavaScriptフィクスチャは、`program_dsl_capability_probe`によってコンパイルされ意味検証されます。これらは決定的なCTestテストであり、モデルやネットワークを必要としません。

```powershell
cmake --build build --config Release --target program_dsl_capability_probe
ctest --test-dir build -C Release --output-on-failure `
  -R '^Program\.DslCapability\.'
```

焦点を絞ったグラフビルダー回帰テストは、ミューテーターの繰り返し呼び出しが正しく蓄積されること、およびバリア、割り込み、リトライの宣言がloweringを生き延びることも証明しています:

```powershell
build\tests\Release\neograph_program_tests.exe `
  --gtest_filter=ProgramCompilerTest.JavaScriptGraphBuilderLowersEveryDeclaredPrimitiveAndAccumulatesCalls
```

## ライブモデル評価

オプトインランナーは、自然言語セマンティクスに加えて公開APIシグネチャを使用してソースを要求します。モデルにはチェックイン済みの回答は渡されません。各応答は、決定的CTestで使用される同じネイティブプローブに送信されます。

```powershell
bun --env-file=C:\path\to\.env run scripts/run_dsl_capability_eval.ts `
  --probe build\tests\Release\program_dsl_capability_probe.exe `
  --model deepseek/deepseek-v4-flash-0731 `
  --repair-attempts 2 `
  --output dsl-capability-evidence.json
```

`--case`はコンマ区切りのサブセットを受け入れ、`--attempts`は独立したワンショット試行を繰り返し、`--repair-attempts`は拒否された完全なソースをモデルに権威あるプローブ診断を返します。プロバイダー/応答の失敗は、コンパイルまたは意味拒否とは別に保持されます。

## 観察されたDeepSeek結果

初期の実行と完全一致識別子の再評価を通じて、モデルは全11の能力グループに対してプローブ検証済みのソースを生成した。静的HITL/再試行には、診断に基づく1回の修復が必要だった。構造化並行処理には2回の修復が必要だった。1回目はESモジュールのエクスポートを復元するため、2回目は誤ったCoreバインディングを完全一致の admission名に置き換えるためである。制御フローは1回の完全一致バインディング修復後に合格した。Mapは、ホストがネイティブAPIマニフェストを提供し、admissionされたCore識別子を曖昧さなく明示した後、一発で合格した。

| 能力ケース | モデルの証拠 | 重要な観察 |
|---|---|---|
| `graph_basics` | 合格 | ループ構築ノードが正しくlowered |
| `graph_routing` | 繰り返し試行で合格した | 以前の出力は禁止されたCommonJS/`require`を使用した |
| `graph_fanout_barrier` | 合格 | fan-outエッジとバリアメンバーシップは正確であった |
| `graph_hitl_retry` | 1回の修復後に合格 | 初期出力は誤ったノード名を使用した |
| `registry_mediated` | 合格 | モデルはホストが許可した動的ノードを正しく参照する |
| `program_control_flow` | 1回の修復後に合格 | 以前の試行は`core`/`Core`を繰り返し呼び出したものであり、許可されたCore `capability`ではなかった |
| `program_map` | 正確な識別子注入後に合格 | 以前の試行はCommonJS、`yield*`、および誤ったCore名を使用した |
| `program_structured_concurrency` | 2回の修復後に合格 | 正確なネストされたコマンドとCoreバインディングを検証した |
| `program_spawn_await` | 合格 | `Await(Spawn(...))`とタイムアウトは構造的に検証された |
| `program_durability` | 合格 | emit、チェックポイント、キャンセルコマンドは正確でした |
| `program_host_capability` | 合格 | インポートスロットと正規入力が一致しました |

初期のバリデータは、 `callCore` ケースに対して誤検知を生成しました。これは、コマンドの種類と入力をチェックしたものの、正確なCore名をチェックしなかったためです。バリデータは、すべてのネストされた `capability` に対して `callCore`を要求するように強化されました。以前に `core`, `Core`またはノード名 `work` を使用して受け入れられていたモデルソースは、したがって現在は正しく拒否されます。

これは能力の証明であり、統計的信頼性の主張ではありません。ケースごとのワンショットおよび修復成功率には、プロバイダーの障害を個別に報告した反復試行が依然として必要です。

## 生成されたProgramへの影響

生のワンショットソース生成は、十分な製品保証ではありません。最小限の安全な合成パスは次のとおりです:

```text
capability manifest + exact admitted identifiers
  -> model source proposal
  -> bounded QuickJS compilation
  -> semantic capability probe
  -> diagnostic-guided repair within a fixed budget
  -> ordinary admission and publication
```

モデルは、ESモジュールとCommonJS、グラフ名とノード名、要求されたCoreの同一性と`core`などの一般的な単語を繰り返し混同しました。NeoCodeはしたがって、正確なシグネチャとadmissionされた識別子を注入し、可能な場合は固定モジュールスキャフォールドを保持し、もっともらしいソースを要求されたトポロジーが構築された証拠として決して扱わないべきです。

NeoGraphは現在、`ProgramSynthesisGateway`でこの境界を強制します: すべてのゲートウェイ設定は、ホスト所有の意味検証ボリッドを提供しなければなりません。検証が成功すると、Catalog admissionの前にコンテンツアドレス指定のレシートが生成されます。拒否された決定は`ProgramSynthesisValidationError`をスローし、正確な証拠を保持し、admissionリゾルバを決して呼び出しません。すでに消費された動的コンパイル予約は消費されたままです。

`javascript_authoring_capability_manifest()`は、インストールされたグラフビルダーとコマンド語彙、正確なシグネチャ、分類、制限、プロファイル制約を機械可読データとして公開します。適合性テストは、そのマニフェストを両方のQuickJSコンテキストに実際にインストールされたプロパティと比較するため、APIドリフトはテストスイートを失敗させます。
