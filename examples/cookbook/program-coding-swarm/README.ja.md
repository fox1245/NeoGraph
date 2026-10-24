<!-- neograph-i18n: source=examples/cookbook/program-coding-swarm/README.md locale=ja source_sha256=cd6756c6ba1b7b59c59c32a59a1b1f97d2bd05b49a37e998fc635870b85d52a6 -->
<!-- neograph-i18n: source=examples/cookbook/program-coding-swarm/README.md -->
# Program コーディングスウォーム (`program-coding-swarm`)

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

単一の線形な「記述・テスト・レビュー」ループではなく、明示的で予算制限された Program の階層構造としてマルチエージェント・コーディングワークフローを実行する方法を示す実験的な概念実証（experimental proof of concept）です。自己合成または置換されたトポロジー自体が安全・高信頼であると主張するものではなく、本例の安全性はホスト所有のコンパイル、アドミッション、予算上限、および決定論的な検証ゲートによって担保されます。

本クックブックのコーディング対象（`slugify`、`escape_html`、`ini_kv`）はトイターゲット（ホスト所有の検証ノードがテストベクターと照合する小規模な決定論的テキスト変換パイプライン）です。

## この例が示す内容

1. **並列作業 (`root -> children`)**: ルート Program はモデル駆動の `orchestrator` ノードでモジュール分割を計画し、モジュールごとに 1 つの子 Harness を並列（`ng.spawn` / `ng.await`）で起動して結果を結合し、最終統合レビューを行います。`--concurrency <n>` または `--sequential`（`--concurrency 1`）を渡すと、同じトポロジーと入力を制限付き並列または逐次実行で動かし、レポートで実時間の重なりを比較できます。
2. **モジュールごとのリソース配分**: 各モジュールには重要度（`standard` または `critical`）があります。重要モジュール（`escape_html`）には、ルート予算から切り出されたより大きな非更新型（non-renewable）トークン・金額予算、より多くの試行回数、より強力な実装ロール（`implementer-strong`）、および孫（grandchild）のセカンドオピニオン Harness が割り当てられます。実行終了時に各ノードの残余予算が表示され、レポートに記録されます。
3. **ロールベースのモデルマッピング**: ノードはハードコードされたモデル ID や価格ではなく、モデルロール（`orchestrator`、`implementer-cheap`、`implementer-strong`、`reviewer`、`second-opinion`）を参照します。ロールの割り当て、プロバイダーエンドポイント、および `SchemaProvider` モデルカタログは [`demo-models.json`](demo-models.json)（オフライン既定）と [`live-models.json`](live-models.json)（ライブ計測用）に格納されています。
4. **ホスト所有の事前条件ゲート**: モジュールの子は、ホスト所有の `verify` ノードが決定論的テストに合格し、**かつ**レビュアーの判定が読み取られ `"accept"` である場合（重要モジュールでは孫のセカンドオピニオン判定 `"accept"` も必要）にのみ結果を公開（publish）できます。オフライン既定実行では、`slugify` の試行 1 が `verify` で失敗し、`escape_html` の試行 1 が `review` で却下され、ゲートが両方の公開をブロックして予算内での再試行を行わせます。
5. **多段階の監督 (`root -> child -> grandchild`)**: 重要モジュール（`escape_html`）では、一次レビュー通過後に子 Harness が孫 Harness（`second-opinion:escape_html`）を起動します。3 階層（`root -> child -> grandchild`）のロール、モデルロール、付与予算・残余予算、および結果を表示します。
6. **データとしてのトポロジーと SQLite 永続化（実行中の置換とロールバック）**: コンパイルされたすべての `ProgramBundle`、承認された `ProgramVersion`、実行系譜（lineage）、および世代（generation）は SQLite（`SQLiteProgramStore`、`SQLiteProgramTransitionStore`、`SqliteCheckpointStore`）に保存されます。実行中、`slugify` の試行 1 がブロックされた後、ホストは保持された `ng.checkpoint` で `slugify` の子トポロジーをより厳格な承認済みバージョン（世代 2）に置換し、残余予算をリセットせず引き継ぎ、子関係を保持します。試行 2 の完了後、以前の承認済みバージョン（世代 3）へロールバックし、SQLite から読み出した 3 世代の履歴を表示します。
7. **中央（central）対 委任（delegated）合成 (`--synthesis=central|delegated`、既定 `central`)**:
   - `central`: 実行開始前にホストがツリー全体（`root`、全 `child` モジュール、重要モジュールの `grandchild`）をコンパイル、リンク（`link_module_child`）、および承認します。子は事前承認された孫テンプレートのパラメータのみを埋めることができます。
   - `delegated`: ホストはルートと子モジュールのみを事前承認します。実行時に重要な子（`escape_html`）がチェックポイントで一時停止し、レビュー済みテンプレートライブラリから孫の JavaScript トポロジーを提案します。ホストは予算超過の提案が `authorize_program_child_synthesis` によって拒否（`admissions_refused`）されることを確認した上で、`ProgramSynthesisGateway` と `ProgramRuntime::prepare_child_synthesis` を通じて子の権限と残余予算内で孫トポロジーを合成・承認します。
   - 同じタスク、モデル、予算で両モードを実行し、出力と JSON レポートで比較します。

## オフラインモード（既定、CTest で実行）

API キーやネットワーク通信なしでビルド・実行できます:

```bash
cmake --build build --target cookbook_program_coding_swarm
./build/cookbook_program_coding_swarm
ctest --test-dir build -R cookbook.program_coding_swarm --output-on-failure
```

## ライブモードと計測レポート

ライブモードは [`live-models.json`](live-models.json) で設定された実プロバイダーを使用します:

- `orchestrator`（ルート）: Anthropic Messages API（`claude-sonnet-5-5`）、環境変数 `ANTHROPIC_API_KEY`。
- `implementer`、`implementer-cheap`、`implementer-strong`、`reviewer`（子）: OpenRouter Chat Completions（`deepseek/deepseek-v4.1-flash-1017`）、環境変数 `OPENROUTER_API_KEY`。
- `second-opinion`（孫）: OpenRouter Chat Completions（`inception/mercury-3`）、環境変数 `OPENROUTER_API_KEY`。

[`.env.example`](.env.example) を `.env` にコピーするか環境変数を設定して実行します:

```bash
export ANTHROPIC_API_KEY="..."
export OPENROUTER_API_KEY="..."

./build/cookbook_program_coding_swarm \
  --live \
  --models examples/cookbook/program-coding-swarm/live-models.json \
  --synthesis=central \
  --runs 3 \
  --max-cost-usd 0.50 \
  --max-cost-usd-other 0.25 \
  --report swarm-report.json
```

### CLI フラグ

- `--mock`（既定）/ `--live`: 決定論的オフラインプロバイダーまたは実プロバイダーを選択。
- `--models <file.json>`: ロールおよびモデルカタログ設定ファイルのパス（`demo-models.json` または `live-models.json`）。
- `--synthesis=central|delegated`: 主合成モードを選択（既定は `central`）。もう一方のモードも実行され、`synthesis_comparison` に比較記録されます。
- `--concurrency <n>` / `--sequential`: 並列実行する子モジュール数の上限（`--sequential` は並列度 `1`）。
- `--runs <k>`: タスクを `k` 回繰り返し、並列・逐次の実時間中央値、1 回あたりコスト中央値、ゲートブロック総数を集計します。
- `--max-cost-usd <usd>`: `RunBudget::monetary_microunits` と `bind_priced_provider_call` で強制される実行全体の金額予算上限。
- `--max-cost-usd-other <usd>`: Anthropic 以外のプロバイダー（OpenRouter の子・孫呼び出し）に対する個別の金額予算上限。
- `--report <file.json>`: 機械可読な JSON レポートの出力先ファイル。

### OpenRouter ルーティングと Zero Data Retention (`zdr`)

[`live-models.json`](live-models.json) では OpenRouter の各ロールに以下を設定しています:

```json
"provider": {
  "sort": "price",
  "allow_fallbacks": true,
  "data_collection": "allow"
}
```

これにより、`zdr` フィルター（多くの安価なエンドポイントを除外します）をかけずにフォールバックを許可し、安価なエンドポイントを選択します。各呼び出しはディスパッチ前に `live-models.json` の `ceiling_input` / `ceiling_output` レートを使って `bind_priced_provider_call` により上限計算され、送信リクエストに `provider.max_price` が設定されます。なお、現在の C++ SDK の `sp::OpenRouterRouting` は `zdr`、`allow_fallbacks`、`require_parameters`、`data_collection`、`only`、`order`、`ignore`、`max_price` を転送し、`live-models.json` の `"sort"` はレポートの `openrouter_routing` メタデータに記録されます。

Zero Data Retention が必要な場合や特定のプロバイダーに固定する場合は、`live-models.json` の `"provider"` を次のように変更します:

```json
"provider": {
  "zdr": true,
  "allow_fallbacks": false,
  "data_collection": "deny",
  "only": ["deepinfra", "together"]
}
```

### `--report <file.json>` の読み方

- `primary_run.total_wall_time_ms` と `primary_run.sum_children_wall_time_ms`: 全体の実時間と子の実行時間の合計を比較します。`children_intervals_overlapped` は子の `[wall_clock_start_ms, wall_clock_end_ms]` 区間が重なったことを示します。
- `primary_run.cost_by_module` と `primary_run.cost_by_model_role`: モジュール別およびモデルロール別のコスト内訳です。
- `primary_run.nodes`: 各階層ノード（`root`、`child`、`grandchild`）のプロバイダー・モデル、試行回数、入出力トークン、コスト（`cost_source`: `"provider_reported"` または `"catalogue"`）、開始・終了時刻、結果、付与予算と残余予算。
- `primary_run.calls`: OpenRouter が実際にルーティングした上流プロバイダー（`provider_routed_to`）を含む呼び出し単位の記録。
- `primary_run.gate_decisions`: ホストゲートが評価した各試行の `verify_passed`、`reviewer_verdict_read`、`reviewer_verdict`、`second_opinion_verdict`、`published`、`block_reason`。
- `primary_run.generation_transitions`: SQLite の世代履歴（`initial` -> `replace_stricter` -> `rollback_initial`）と各遷移前後の予算。
- `synthesis_comparison`: 同じタスク・モデル・予算における `central` と `delegated` の比較結果。
- `aggregates`: `--runs <k>` 実行時の中央値集計。

## ライブ実行の計測結果 (2026-10-24)

- **設定**: `--live --synthesis=central --runs 3`、上限 `--max-cost-usd 2.00 --max-cost-usd-other 0.50`。モデルは [`live-models.json`](live-models.json) を使用（ルート `orchestrator` は Anthropic Messages API、子および孫は `{"sort":"price","allow_fallbacks":true,"data_collection":"allow"}` を設定した OpenRouter 経由）。Linux x86-64。
- **全 3 回の実行**: すべてのモジュールが初回試行で公開（1 回の実行あたり 3/3 モジュール、`central` モードでのゲートブロック 0 件、承認拒否 0 件）。実行検証は `PASS` を出力。
- **実時間（3 回の中央値）**: モジュールの子を並列実行した場合 23.6 秒、同じ作業を逐次実行した場合 32.6 秒（約 1.38 倍）。各回の並列実時間は 23.6 秒、21.6 秒、28.8 秒。すべての実行で子の実行区間が重なった。
- **1 回あたりのコスト（実行の金額記録より）**: 0.0291〜0.0299 USD、中央値 0.0298 USD。内訳は OpenRouter の子および孫が 0.0014〜0.0020 USD、ルート `orchestrator` が約 0.028 USD。1 回あたりのトークン数: 入力が約 2.7k、出力が 3.9k〜4.7k。
- **実行中のトポロジー置換とロールバック**: オフライン実行と同様に動作。SQLite から 3 世代を読み出し、非更新型予算を引き継ぎ（トークン 20000 -> 20000）、子を保持。
- **委任（`delegated`）モード（比較のため同一実行内で実施）**: 3 モジュール公開、ゲートブロック 1 件および承認拒否 1 件（予算超過の孫提案が設計どおり拒否されたもの）、実時間 32.6 秒。
- **この実行が示さないこと**: 今回のライブモデルでは `central` モードでゲートがブロックする必要が生じなかったため、ゲートによるブロックと再試行の経路はこのライブ実行ではなくオフライン実行によって示される。また、3 回の実行は信頼性の計測ではない。
