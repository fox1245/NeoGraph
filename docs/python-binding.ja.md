<!-- neograph-i18n: source=docs/python-binding.md locale=ja source_sha256=61dd8227b6a8807710fb014cacdf14a64257a18b35778a30981f34bd1eefb35f -->
# Pythonバインディング

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

`neograph-engine`は、同じC++ランタイムのpybind11サーフェスです。ホイールによりCore、LLM、Program/QuickJS、MCP、SQLiteランタイムの永続性が有効になります。オプションのソースビルドでは、コンパイルされたコンポーネントのみが公開されます。

```bash
pip install neograph-engine
```

## Coreグラフクイックスタート

```python
import neograph_engine as ng

@ng.node("greet")
def greet(state):
    return [ng.ChannelWrite("messages", [
        {"role": "assistant", "content": f"Hello, {state.get('name')}!"}
    ])]

definition = {
    "schema_version": ng.TOPOLOGY_SCHEMA_VERSION,
    "name": "demo",
    "channels": {
        "name": {"reducer": "overwrite"},
        "messages": {"reducer": "append"},
    },
    "nodes": {"greet": {"type": "greet"}},
    "edges": [
        {"from": ng.START_NODE, "to": "greet"},
        {"from": "greet", "to": ng.END_NODE},
    ],
}

engine = ng.GraphEngine.compile(definition, ng.NodeContext())
result = engine.run(ng.RunConfig(thread_id="t1", input={"name": "NeoGraph"}))
print(result.output["channels"]["messages"]["value"])
```

## Core APIパリティ

Pythonは、独立したPythonスケジューラではなく、C++の実行機能を公開します:

- 同期およびasyncioのrun/stream/resume;
- 正確なチェックポイント`resume_from`、フォーク、状態検査、順序付き状態書き込み;
- グラフ割り込みと`NodeInterrupt`による静的および動的HITL;
- `RunMetadata`のデッドライン、トレース/実行ID、モデルトークン上限;
- グラフ全体およびノードごとの`RetryPolicy`（ジッターを含む）;
- 実行ローカルまたは明示的に再利用可能な`CacheScope`;
- チェックポイントおよび長期Storeバックエンド;
- カスタムノード、リデューサー、条件、プロバイダー、ツール;
- ツールゲート、実行ポリシー、必須ライフサイクルHook、厳格なランタイム介入。

### パリティ契約

ここでいう「パリティ」とは、Pythonが同じネイティブ実行パスと安全契約を利用するという意味です。すべての内部C++ストレージ型や権限型をPythonへそのまま複製するという意味ではありません。

| 機能 | ネイティブC++パス | Pythonサーフェス | 状態 |
|---|---|---|---|
| Coreグラフのコンパイルと実行 | `GraphEngine` | `GraphEngine.compile`、run/stream/asyncメソッド | 同じスケジューラとランタイム |
| ランタイムID、期限、予算 | `RunMetadata`, `RunConfig` | `RunMetadata`, `RunConfig.model_token_budget` | 実行ごとに同じ値 |
| 再試行とノードキャッシュポリシー | `RetryPolicy`, `CacheScope` | グラフ/ノードsetterとキャッシュスコープ | 同じランタイムポリシー |
| チェックポイント、HITL、タイムトラベル | チェックポイントStoreと再開API | 再開、厳密な`resume_from`、フォーク、状態履歴/更新 | 同じチェックポイント契約 |
| Programのオーサリングとローカル実行 | コンパイラ、Catalog、`ProgramRuntime` | `ProgramCompiler`、`LocalProgramHost`、ハンドル/結果 | ネイティブの所有者スコープ付き簡易ホスト |
| 必須ライフサイクルHook | レジストリ、ランナー、`HookRuntime` | 定義と`create_hook_runtime`コールバック | 同じフェイルクローズ型ライフサイクル境界 |
| ランタイムコンテキストと厳格なディスパッチ | コンテキストStore、レシート、インターポジション | 対応する不変値、Store、`StrictRuntimeProfile` | 同じネイティブコントローラ |
| 永続化を含むホイール既定値 | SQLite Core/コンテキスト/ディスパッチStore | `_HAVE_SQLITE`のエクスポート | PyPIホイールで有効 |

生の`ProgramCatalog`、遷移Store、置換/移行コントローラ、合成ゲートウェイ、Hookジャーナル、RPCエグゼキュータはホスト合成APIのままです。権限を持つこれらの経路を部分的にだけ公開すると、必須の`proposal -> compile -> admit -> publish -> migrate/spawn`プロトコルを迂回できます。将来のPythonホストコントローラは、このプロトコルと更新不能なリネージ予算を単一の所有者スコープ単位として束ねなければなりません。`_HAVE_PROGRAM`は、生のコントロールプレーン管理までパリティがあるとは主張しません。

### ランタイム再試行オーバーライド

```python
policy = ng.RetryPolicy()
policy.max_retries = 3
policy.initial_delay_ms = 100
policy.backoff_multiplier = 2.0
policy.max_delay_ms = 2_000
policy.jitter_pct = 0.2

engine.set_retry_policy(policy)
engine.set_node_retry_policy("remote_call", policy)
```

グラフ定義の`"retry_policy"`は、宣言的なデフォルトのままです。ランタイムセッターは、別個のC++/Python設定サーフェスです。

### メタデータと正確な再開状態

```python
config = ng.RunConfig(thread_id="job-42", input={"task": "..."})
config.model_token_budget = 20_000
metadata = ng.RunMetadata(
    timeout_ms=30_000,
    trace_id="trace-42",
    run_id="run-42",
    owner_scope="tenant-a",
)
result = engine.run(config, metadata)

# Never substitutes a newer checkpoint:
result = engine.resume_from(config, checkpoint_id, {"approved": True}, metadata)
```

Pythonノード内では、同じ値が`input.ctx.trace_id`、`run_id`、`has_deadline`、`deadline_remaining_ms`、および`model_token_budget`を通じて利用可能です。

### キャッシュスコープ

```python
engine.set_node_cache_enabled("pure_parser", True)  # execution-local default
engine.set_node_cache_enabled("pure_parser", True, ng.CacheScope.Reusable)
```

`Reusable`は、ノードがテナント、プロバイダー、Store、ツール、資格情報、時刻、およびレジューム状態から独立しているという明示的な表明です。

## ProgramとQuickJS

Pythonホイールは`neograph::program`と制限付きQuickJSフロントエンドを構築します。Python定義ノードは、不変のProgramレジストリに参加し、ネイティブの`ProgramRuntime`を通じて実行できます。

```python
import neograph_engine as ng

registry = (
    ng.ProgramRegistryBuilder()
    .add_registered_node(
        "my_node", "1.0.0", "sha256:" + "1" * 64
    )
    .add_registered_reducer(
        "overwrite", "1.0.0", "sha256:" + "2" * 64
    )
    .build()
)

source = ng.ProgramSource.from_javascript("agent.js", r'''
export function define() {
  const graph = ng.graph("main");
  graph.channel("value", {reducer: "overwrite", initial: 0});
  graph.node("work", {type: "my_node"});
  graph.entry("work");
  graph.exit("work");
  return graph;
}
export function* main(input) {
  return yield ng.callCore("main", input, "python:main");
}
''')

ceiling = ng.ProgramRunBudget()
ceiling.wall_time_ms = 10_000
ceiling.model_tokens = 1_000
ceiling.monetary_microunits = 1_000
ceiling.max_concurrency = 2
ceiling.max_program_operations = 32
ceiling.max_core_steps = 20
ceiling.max_dynamic_compiles = 1

run_budget = ng.ProgramRunBudget()
run_budget.wall_time_ms = 10_000
run_budget.max_concurrency = 2
run_budget.max_program_operations = 32
run_budget.max_core_steps = 20

host = ng.LocalProgramHost(registry, "tenant-a", ceiling)
version = host.compile_admit(source, run_budget)
result = host.run(version, {}, run_budget)
```

`LocalProgramHost`は、オーナースコープのインメモリ便宜ホストです。それでもC++コンパイラ、Catalog、admissionポリシー、遷移ストア、およびProgramRuntimeを使用します。生成された提案は、admission前にホストの意味検証を追加で通過する必要があります。[DSL機能評価](DSL_CAPABILITY_EVAL.md)を参照してください。

正確にインストールされたJavaScript語彙はdictとして利用可能です：

```python
manifest = ng.javascript_authoring_capability_manifest()
```

## 必須ライフサイクルHook

Hookは、モデルがツールを呼び出すことを決定するのではなく、ホストのライフサイクルイベントによってトリガーされます。

```python
data = ng.HookDefinitionData()
data.phase = ng.HookPhase.CheckpointPublished
data.target_id = "audit"
data.delivery = ng.HookDelivery.BlockingMandatory
data.failure_mode = ng.HookFailureMode.FailClosed
data.effect = ng.ToolEffectClass.ReadOnly

mapper = ng.HookInputMapper()
mapper.kind = ng.HookInputMapperKind.Template
mapper.value_template = {"kind": "checkpoint"}
data.input_mapper = mapper

definition = ng.HookDefinition.create(data)
runtime = ng.create_hook_runtime(
    [definition],
    {"audit": lambda arguments, event_type, event_data: persist(arguments)},
)
engine.set_hook_runtime(runtime)
```

`FailClosed`下でのコールバック失敗は、保護されたランタイム境界をブロックします。`Continue`は、観測上の損失が許容される場合にのみ利用可能です。

## ランタイムコンテキスト、Skill、および厳格なディスパッチ

バインディングは、不変のRAW履歴レコード、コンテキストアーティファクト、エポック、必須のSkill/制約、変換レシート、およびプロバイダーディスパッチレシートを公開します。

```python
requirements = ng.RuntimeContextRequirements()
requirements.required_artifact_ids = [skill.id, constraint.id]
requirements.required_skill_artifact_ids = [skill.id]

assembler = ng.RuntimeTurnAssembler(
    context_store,
    max_input_tokens=32_000,
    requirements=requirements,
)
```

`ContextTransformReceipt`は任意の派生証拠を許可しますが、すべての必須アーティファクトがバイト単位で同一のままであることを要求します。

完全な厳格パスには、永続的なSQLiteストアを使用します：

```python
contexts = ng.SQLiteContextStore("runtime.sqlite3")
receipts = ng.SQLiteProviderDispatchReceiptStore("runtime.sqlite3")
hooks = ng.create_hook_runtime(definitions, callbacks)

profile = ng.StrictRuntimeProfile(
    provider,
    contexts,
    receipts,
    hooks,
    provider_binding_identity,
    max_input_tokens=32_000,
    required_context_artifact_ids=[constraint.id],
    required_skill_artifact_ids=[skill.id],
)
profile.activate("tenant-a", strict_epoch)
completion = profile.invoke(params)
profile.attach(engine)
```

## HITLと状態

静的`interrupt_before`/`interrupt_after`、動的`NodeInterrupt`、同期`resume`、asyncio`resume_async`、および厳密`resume_from`はチェックポイントストアを必要とします。

```python
if result.interrupted:
    result = engine.resume(result_thread_id, {"approved": True})
```

検査とタイムトラベルには`get_state_history`、`update_state`、`fork`を使用します。`get_state_view()`はフラットなPydanticベースのチャネルアクセスを提供し、`get_state()`は標準のネスト表現を保持します。

## 非同期とキャンセル

`run_async`、`run_stream_async`、`resume_async`は`asyncio.Future`オブジェクトを返します。Futureのキャンセルは`CancelToken`を通じて実行中のネイティブI/Oに伝播します。ストリーミングコールバックは呼び出し元のasyncioループスレッドにマーシャリングされて戻されます。

Python定義のプロバイダは同期`complete`/`complete_stream`を実装します。非同期ネイティブのプロバイダ実装はC++拡張のままです。

## プロトコルと可観測性

- MCPクライアントツールは、ビルド時に`neograph_engine.mcp`を通じて利用可能です。
- A2Aクライアント型は、ビルド時に`neograph_engine.a2a`を通じて利用可能です。
- `ProtocolHostAdapter`は公式Python A2A/ACPサーバーSDKをNeoGraphセッションセマンティクスと統合します。
- `neograph_engine.tracing`と`neograph_engine.openinference`は、Phoenix、Langfuse、Arizeおよび互換バックエンド向けにベンダーニュートラルなOTel/OpenInferenceデータを出力します。

## オプションコンポーネント

公開パッケージはオプションのC++コンポーネントを正直に示します：

- `_HAVE_PROGRAM`, `_HAVE_SQLITE`, `_HAVE_POSTGRES`, `_HAVE_MCP`, `_HAVE_A2A`;
- 欠落コンポーネントはPythonでエミュレートされるのではなく、存在しないものとして扱われます；
- PyPIホイールはProgram/QuickJS、LLM、MCP、SQLiteを有効にします。ソースビルドはCMakeオプションに従います。

## テストと例

バインディングスイートは、Core実行、カスタムコールバック、asyncio、キャンセル、Programコンパイル/ランタイム、必須Hook、厳密コンテキスト、SQLite永続化、プロトコル、READMEの例をカバーします。

- [Python の例](../bindings/python/examples/README.md)
- [C++ の例](../examples/README.md)
- [QuickJS オーサリング境界](QUICKJS_PUBLIC_AUTHORING_BOUNDARY.md)
- [厳密なランタイムインターポジション](STRICT_RUNTIME_INTERPOSITION.md)
