<!-- neograph-i18n: source=docs/python-binding.md locale=ja source_sha256=8dc07611a5d489c5f48231f91efa341362b9f2a47558a81ad11f2ca60b4d38af -->
# Pythonバインディング

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

`neograph-engine`は、同じC++ランタイムのpybind11サーフェスです。ホイールによりCore、LLM、Program/QuickJS、MCP、SQLiteランタイムの永続性が有効になります。オプションのソースビルドでは、コンパイルされたコンポーネントのみが公開されます。

このガイドには SchemaProvider SDK `0.3.0`、インターフェース revision `6`、共有ライブラリー ABI revision `6` でビルドした wheel が必要です。バンドルされたコンポーネントをまとめてインストールします。`complete` を公開する旧ホイールは異なるプロバイダーインターフェースを使うため、以下の型付き例を実行できません。

```bash
pip install neograph-engine
```

## 型付きプロバイダーリクエストと結果

`Provider` はリクエストの準備とディスパッチを分離します。`SchemaProvider` は SchemaProvider SDK を通じて、ファミリー別のリクエストを検証しエンコードします。`PreparedProviderRequest` は、一度のディスパッチが消費するまで、そのネイティブの準備結果を保持します。

| 操作 | Python シグネチャ | 結果 |
|---|---|---|
| 記述子の受け入れ | `load_provider_descriptor(source: str, policy=None)` | `ValidatedDescriptor`。不正な閉じた JSON は `ValueError` |
| プロバイダーの構築 | `SchemaProvider(descriptor, options, defaults)` | 受け入れ済みのエンドポイント/ファミリーとランタイムポリシーを持つプロバイダー |
| リクエストの構築 | `make_provider_request(provider, model, messages, tools=[], controls=ProviderControls(), mode=ProviderMode.Collect)` | 型付き `ProviderRequest` |
| 準備 | `provider.prepare(request)` | `PreparedProviderRequest` |
| 一度のディスパッチ | `provider.dispatch(prepared)` | 所有された `ProviderOutcome` |
| 準備とディスパッチ | `provider.invoke(request)` | 所有された `ProviderOutcome` |

`model` は明示的に指定します。`ProviderControls` は `max_output_tokens`、`temperature`、`top_p` などの型付き制限と生成制御を提供します。ファクトリーは SDK のファミリー別ペイロードを構築し、生の辞書では置き換えられません。ストリーミングを要求するには `request.mode` を `ProviderMode.Stream` に設定します。`on_event` の設定だけではモードを選択しません。`request` は `cancel_token`、`on_event`、`observer_limits`、省略可能な `timeout_ms` も保持します。

`ProviderControls.provider` と `response_format`、`SchemaProviderDefaults.provider`、`ProviderToolResult.host` は独立した省略可能なレコードを返します。値があれば、レコードを読み、編集してからプロパティに再代入します。`controls.provider.order` だけを変更しても controls は更新されません。レコードを解除するには `None` を代入します。

`request.timeout_ms` に表現可能な非負の値を代入すると、その時点で絶対期限を設定します。準備の直前に設定してください。その後の読み取りは残りのミリ秒を返し、期限切れ後はゼロに制限されます。`None` のままなら、準備時にプロバイダーの既定タイムアウトを使います。準備済みハンドルを保持して待機しても、期限は更新されません。

### 記述子とランタイムポリシー

記述子は `load_provider_descriptor` が受け入れる、バージョン付きの閉じた JSON です。ファミリー、`base_url`、ルート、フィールドバインディング、認証規則を受け入れます。`model` はリクエストで、資格情報はランタイムオプションで指定します。`ProviderRuntimeOptions` は `api_key`、`default_timeout_ms`、`ca_file`、`workers` と SDK のトランスポート/リソース制限を提供します。`SchemaProviderDefaults` は型付きプロバイダー既定値を提供します。資格情報はランタイムオプションに保存し、記述子ファイルやログには含めません。

`ProviderDescriptorPolicy.identity` は SDK ポリシーの生の SHA-256 ダイジェストを含む Python `bytes` を返します。表示には `policy.identity.hex()` を使います。識別子そのものは UTF-8 テキストでも、16 進文字列の別名でもありません。

現在のトランスポートは libcurl を使います。`prefer_libcurl` スイッチ、ランタイムのエンドポイント上書き、WebSocket トランスポートはありません。[C++ プロバイダーリファレンス](reference-en.md)は記述子の受け入れと対応リクエストファミリーを説明しています。

[SDK 使用ガイド](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#run-the-first-request-without-a-hosted-api)の一要求ループバックピアを起動し、別のターミナルでこのテキスト専用例を実行します。エンドポイント、モデル、出力上限はそのピアに一致し、資格情報を送信しません。HTTPS ピアには `NG_EXAMPLE_CA_FILE` で信頼する CA を指定します。ホストされたエンドポイントでは、記述子の origin とリクエストモデルを明示的に変更し、その origin 用の資格情報をランタイムオプションに指定します。ホストされた呼び出しには料金が発生する場合があります。

```python
import json
import os
import neograph_engine as ng
from neograph_engine.llm import SchemaProvider

descriptor = ng.load_provider_descriptor(json.dumps({
    "descriptor_version": 1,
    "revision": 1,
    "id": "python-guide-chat",
    "family": "openai.chat",
    "connection": {
        "base_url": "http://127.0.0.1:8765",
        "paths": {
            "buffered": "/v1/chat/completions",
            "streaming": "/v1/chat/completions",
        },
    },
    "bindings": {
        "model": "model", "messages": "messages", "stream": "stream",
        "max_output_tokens": "max_tokens", "usage": ["usage"],
    },
    "stop_reasons": {"stop": "EndTurn", "length": "MaxTokens",
                     "tool_calls": "ToolUse", "content_filter": "ContentFilter"},
}))
options = ng.ProviderRuntimeOptions(
    api_key="",
    ca_file=os.getenv("NG_EXAMPLE_CA_FILE", ""),
    default_timeout_ms=30_000,
)
provider = SchemaProvider(descriptor, options=options)
controls = ng.ProviderControls()
controls.max_output_tokens = 64
request = ng.make_provider_request(
    provider, "example-model",
    [ng.ProviderMessage(role=ng.ProviderRole.User, parts=[ng.Text("Say hello.")])],
    controls=controls,
)
prepared = provider.prepare(request)
outcome = provider.dispatch(prepared)
print("consumed:", prepared.consumed)
if outcome.failure is not None:
    print("failed:", outcome.failure.error.kind,
          outcome.failure.error.safe_message)
else:
    print(outcome.text)
count = outcome.usage.output_total
print("output tokens:", count.value if count is not None else "unknown")
```

ハンドルが不要な場合は `provider.invoke(request)` が準備とディスパッチをまとめて実行します。`outcome.text` は可視テキストの投影であり、`outcome.messages` は完全な型付きパートを保持します。使用量カウンターが省略された場合は `unknown`、ゼロが報告された場合は `0` と表示します。

リンク先の合成応答ピアを使った場合の期待出力:

```text
consumed: True
Hello.
output tokens: 0
```

カウンター省略の経路を確認するには、ピアの `usage` メンバー全体を削除して再起動します。最後の行は `output tokens: unknown` になるはずです。これは実モデルのトークン計数ではなく、プロトコルのマッピングを確認する例です。

### ファミリー別制御

パッケージ版、native archive v3、portable JSON v2 は別です。例は制御構築のみで、対応ファミリーの承認済み provider が必要です。最初のループバックは変更しません。OpenRouter 専用制御は未承認ローカル origin で I/O 前に拒否されます。

```python
chat = ng.ProviderControls()
reasoning = ng.ChatReasoningOptions()
reasoning.effort = "low"
reasoning.enabled = True
chat.chat_reasoning = reasoning
chat.include_reasoning = True
chat.usage_include = True
chat.models = ["openai/gpt-4.1", "openai/gpt-4.1-mini"]

responses = ng.ProviderControls()
responses.parallel_tool_calls = False
responses.verbosity = ng.ResponsesVerbosity.Low
responses.truncation = ng.ResponsesTruncation.Disabled
responses.responses_include = [ng.ResponsesInclude.ReasoningEncryptedContent]

messages = ng.ProviderControls()
messages.max_output_tokens = 4096
messages.thinking_mode = ng.MessagesThinkingMode.Adaptive
messages.output_effort = ng.MessagesOutputEffort.High
cache = ng.MessagesCacheControl()
cache.ttl = ng.MessagesCacheTtl.FiveMinutes
messages.cache_control = cache
choice = ng.MessagesToolChoice()
choice.mode = ng.MessagesToolChoiceMode.Auto
choice.disable_parallel_tool_use = True
messages.messages_tool_choice = choice

gemini = ng.ProviderControls()
gemini.temperature = 0.7
gemini.gemini_thinking_level = ng.GeminiThinkingLevel.Low
safety = ng.GeminiSafetySetting()
safety.category = ng.GeminiSafetyCategory.Harassment
safety.threshold = ng.GeminiSafetyThreshold.BlockMediumAndAbove
gemini.safety_settings = [safety]
choice = ng.GeminiToolChoice()
choice.mode = ng.GeminiToolChoiceMode.Auto
gemini.gemini_tool_choice = choice
```

既定構築後にフィールドを代入します。キーワードコンストラクターはありません。optional レコード/ベクトルは独立コピーなので編集後に再代入します。Chat reasoning の `effort/max_tokens/exclude/enabled`、`include_reasoning`、`usage_include`、代替 `models` は宣言された OpenRouter origin が必要です。既存の `reasoning_effort/service_tier/provider/response_format` も残ります。

Responses verbosity は `Low/Medium/High`、truncation は `Disabled/Auto`、include は `ReasoningEncryptedContent/WebSearchSources/FileSearchResults/MessageOutputTextLogprobs/ComputerCallOutputImageUrl/CodeInterpreterCallOutputs` です。`responses_include=None` は既定 encrypted reasoning を維持し、`[]` を含む明示リストはその内容だけを選択して後続 native replay が不適格になる場合があります。store/reasoning/hosted tools/tool-call 上限も残ります。

Messages thinking は `Manual/Adaptive/Disabled`。Manual budget は承認最小値以上、output cap 未満で、mode なしの budget は manual です。Adaptive/disabled は budget を禁止します。有効な thinking は temperature を省略しますがモデル禁止は明示 temperature を拒否します。effort は `Low/Medium/High/Max`、TTL は `FiveMinutes/OneHour`、tool choice は `Auto/Any/None_/Tool`。`Tool` は宣言された client tool の `name` が必要です。routing には宣言された OpenRouter origin が必要です。

Gemini level は `Minimal/Low/Medium/High` で `thinking_budget` と排他的です。tool choice は `Auto/Any/None_/Validated`、`allowed_function_names` はリストで `required_tool` と排他的です。safety category は `Harassment/HateSpeech/SexuallyExplicit/DangerousContent/CivicIntegrity`、threshold は `BlockNone/BlockOnlyHigh/BlockMediumAndAbove/BlockLowAndAbove/Off`。誤ったファミリー/範囲/thinking・cap・tool 組み合わせは I/O 前に拒否します。temperature 禁止 prefix は ASCII 大小文字を無視し最後の `/` 後のモデルにも一致します。Chat/Responses: `gpt-5/gpt-6/o1/o3/o4`、Messages: `claude-opus-4-7/claude-opus-4-8/claude-opus-5/claude-sonnet-5/claude-fable-`。[SDK 制御の承認](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#reasoning-sampling-and-tool-controls)を参照してください。

### 承認前のデプロイヘッダー

`load_provider_descriptor(source, policy=None)` は `${VAR}` をリテラルとして扱います。次の helper は実際の承認前にホスト値を処理します。`source` は Messages 記述子の JSON 文字列です。

```python
environment = ng.ProviderDeploymentHeaderEnvironment()
environment.anthropic_workspace_id = "workspace-example"
environment.anthropic_beta = None
# source is Messages descriptor JSON text.
descriptor = ng.load_provider_descriptor_with_deployment_headers(
    source, [("anthropic-workspace-id", "workspace-override")], environment,
)
```

`load_provider_descriptor_with_environment_headers(source, overrides=[], policy=None)` は Messages 用の optional `ANTHROPIC_WORKSPACE_ID/ANTHROPIC_BETA` を読み、未設定/空値を省略します。リテラルヘッダーが環境より優先し、明示 pair が両方を大小文字無視で上書きします。重複 override/不正・予約名/改行は承認失敗です。両 helper は optional `policy` を受け取り、承認済み記述子の変更や encoder template 評価を行いません。

### Responses カーソルと外部 Gemini 履歴

`previous_response_id` は provider 保管状態を選択します。全履歴ではなく新入力だけを送信し、最初の成功した Responses 呼び出しの bound controls と真正な terminal ID を維持します。

```python
# first_request/first_outcome belong to responses_provider and response_model.
responses.previous_response_id = first_outcome.messages[-1].id
responses.previous_response_history = (
    first_request.messages + first_outcome.messages
)
new_input = [ng.ProviderMessage(
    role=ng.ProviderRole.User, parts=[ng.Text("Continue.")],
)]
next_request = ng.make_provider_request(
    responses_provider, response_model, new_input, controls=responses,
)
```

`previous_response_history` は `ProviderMessage` ベクトルで、単一応答/JSON オブジェクトではなく送信されません。真正な client-tool 所有権には元の全 prefix と cursor ID の terminal assistant が必要な場合があります。サーバー保管テキストは空ベクトルでも可能です。後続プロセス内 cursor の private terminal 所有権は全 replay/archive 権限にはなりません。失敗/編集/origin・model・config・route 不一致は拒否します。全 native replay/archive には元の全 prefix が必要です。[SDK Responses 継続](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#responses-provider-held-continuation)を参照してください。

caller 作成の外部 assistant `Text/ProviderToolCall` に native state/wire output/signature がなければ `gemini.gemini_history_mode = ng.GeminiHistoryMode.PortableForeign` を明示します。既定は `NativeOnly`。最初の外部 function call だけが Google validator bypass を受け、テキスト専用 turn に signature はありません。真正な native group は厳密に検証し、失敗 seal 修復/削除/降格、reasoning 権限取り込み、portable data への replay 付与を行いません。[SDK 外部 Gemini 履歴](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#explicit-portable-gemini-history)を参照してください。

PortableForeign は wire metadata も拒否します。cursor 出力は `NativeReplay.complete == False` で archive 不適格です。private owner は全履歴 replay を承認しません。


### 準備済みハンドルと Python プロバイダー

ディスパッチは失敗時も含め、準備済みハンドルを一度消費します。ディスパッチ前に `prepared.valid` と `prepared.error`、その後に `prepared.consumed` を確認します。Python オブジェクトを保持しても再利用はできません。再試行には新しいリクエストを構築し、準備します。

Python サブクラスは `Provider(family)` を呼び、`get_name()` と `prepare(request)` を実装します。準備を `SchemaProvider` に委譲し、その真正な準備済みハンドルを返せます。成功した SDK 結果を任意に作成したり、JSON からディスパッチ権限を取り込んだりすることはできません。共通の `invoke` と `dispatch` はネイティブのライフサイクルを使い、旧 `complete` メソッドのオーバーライドでは実装できません。

ネイティブ実行が Python の `prepare` オーバーライドを呼ぶ際に `None` を返すと、ハンドルを消費する前に `TypeError` が発生します。真正な `PreparedProviderRequest` を返してください。

`NodeContext(provider=provider)` と `ctx.provider` への代入は、ネイティブの共有所有権リースで元の Python プロバイダーを保持します。ネイティブコンテキスト、コンパイル済みノード、エンジンのコピーはこのリースを保持するため、`ctx.provider` を再代入したり、外部の Python 参照が回収されたりした後も、同じオブジェクトのオーバーライドを呼びます。再代入は変更可能なコンテキストのリースだけを解放し、既存のエンジンではなく以後のコンパイルに反映されます。オブジェクトの同一性と寿命を保持するもので、プロバイダーの変更可能な状態を固定するものではありません。最後のリースは GIL を取得して Python 所有者を解放します。

削除された `CompletionParams`、`ChatCompletion`、`OpenAIProvider`、`RateLimitedProvider` に互換エイリアスはありません。検証済み `SchemaProvider` を構築し、リクエストファクトリーを使います。`ChatMessage` と `ToolCall` はグラフ用の便宜的な値として残り、SDK の完全な `ProviderMessage`、`ProviderToolCall` とは別の型です。

### 結果、失敗、使用量

`ProviderOutcome` は所有された不変の SDK 結果を保持します。`outcome.completion` または `outcome.failure` を確認します。該当しない分岐は `None` です。完了と失敗のビューは、順序付きの完全なメッセージ、使用量、試行の証拠、停止/エラー情報、保持されたワイヤー証拠を保存します。失敗でも部分出力が残る場合があります。部分テキストを成功として返さず、失敗を報告するときにその証拠を保持します。

`ProviderCompletion.wire_envelope` と `ProviderPartialCompletion.wire_envelope` はプロバイダーファミリーごとの証拠で、`None` の場合があります。読む前に `wire_envelope is not None` を確認してください。現在のバッファ型 Chat デコーダーはこの値を `None` のままにし、完全な応答 JSON を `raw_events` に保持します。その項目は `type == "chat.completion"` の `ProviderRawWire`（SDK の `RawWire`）で、文書は `payload` にあります。エンベロープがあると仮定せず、実際の型付き raw イベントを調べてください。raw イベントが失敗を成功に変えることはなく、エンベロープがなくても保持された応答がないとは限りません。SDK のフォールバックでエンベロープを合成しません。

所有された結果または保持された型付きビューを残してください。真正なメッセージ、ネイティブ所有権、存在するワイヤー文書は、呼び出し終了後やプロバイダーオブジェクトのガベージコレクション後も有効です。raw payload はプロバイダーの非公開フィールドを保持しますが、ネイティブトレースは raw エンベロープ/イベントとネイティブ再生/推論を除外します。Python の JSON ビューはデータのコピーであり、ネイティブ再生権限や財務上の権限ではありません。

完全な型付きメッセージ/パートを使い、必要な意味が論理的なロールとテキストなら、その値を比較してください。チェックポイントや Chat リクエストのテキスト content は、テキスト文字列でも型付きテキストパート配列でも有効です。呼び出し側は偶然選ばれた一つの直列化形式を要求してはいけません。続行には、平坦なテキストや JSON の代用品ではなく、完全なパートと真正なネイティブ所有権を使ってください。

`ProviderMessage.parts`、完了/部分結果の `messages` と `raw_events`、使用量の `extra`/`conflicts` は、内部のバインドされた値を含む独立したリストやマップを返します。元のコレクションを置き換えた後も、保持したパートを安全に参照できます。コピーを変更しても不変の結果は書き換わりません。メッセージを編集するには `parts = message.parts` で読み、`parts` を変更してから `message.parts = parts` で代入します。`message.parts.append(...)` は一時的な Python リストだけを変更します。

SDK の失敗は戻り値のデータです。ホストのオブザーバーや予算精算の失敗は、`ProviderOutcomeError` から派生する `ProviderObserverError` または `ProviderBudgetSettlementError` を送出し、実際の結果と原因を保持します。したがって、コールバックの失敗は再ディスパッチの許可にはなりません。

保存された Python のプロバイダー例外やグラフ例外の原因を繰り返し参照しても、元の例外オブジェクトと traceback を保持します。ネイティブの入れ子の例外変換を通じて参照する原因も同じ規則に従います。

使用量カウンターは `value` と `evidence` を持つ `UsageCount`、または不明な場合は `None` です。報告されたゼロは既知の使用量であり、省略されたカウンターとは異なります。`count.value` を読む前に `count is not None` を確認します。会計処理で `count or 0` を使ったり、不明な入力/出力/合計カウンターをゼロに置き換えたりしないでください。

`outcome.usage.provider_cost`は不変の`ProviderReportedCost`ビューです。任意の`ProviderUsdAmount`フィールド`total/upstream_total/upstream_input/upstream_output`に`nano_usd/evidence/rounding`があり、`status`はこの順の4要素、`byok_status`はnullableな`is_byok`の状態です。`ProviderCostStatus/ProviderCostSource/ProviderCostRounding`は欠落、利用可能、不正、精度超過、overflow、不明通貨、競合の根拠を保持します。金額は元のdecimal字句ではなく解析済みbinary64から切り上げます。報告ゼロや`is_byok=False`は欠落ではありません。請求書やtoken予算の課金ではないため、呼び出しごとの金額は集計token bankではなく順序付き`provider_outcomes`で確認します。

`UsageAccumulator`はtokenのみの集計です。単一報告は金額メタデータを保持しますが、2回目以降のsnapshotの`provider_cost`はcanonical Missing/Noneに明示的に戻します。古い最初の費用や合計金額ではありません。このsnapshotの復元は費用を再生成せず、token権限を更新しません。正確な金額の根拠は各元outcomeまたはprovider journal項目から読みます。

再開や続行時、`RunResult.provider_outcomes` は元の結果を順序どおり保持し、その後に新しい結果を追加します。`RunResult.usage` は現在の会計バンクを反映し、チェックポイントから以前の報告を復元する場合があります。再開後に新しいプロバイダー呼び出しがなくても、使用量が `None` またはゼロになる保証はありません。証拠の復元で元のプロバイダーリクエストを再ディスパッチしたり、二重に課金したりしてはいけません。保持された報告は以前の呼び出しを記述するもので、新たな支出枠を与えません。

### ネイティブ履歴とポータブルなエクスポート

返された `ProviderMessage` は、完全な型付きパートと真正なネイティブ再生状態を持ちます。ネイティブ再生とワイヤー出力は読み取り専用です。プロバイダーを直接続行する場合は、元のリクエストの `request.messages` 全体を先行履歴として保持し、`outcome.messages` を追加します。この結果メッセージは返された出力であり、入力履歴全体ではありません。最初の呼び出し例の `request` と `outcome` から `history = request.messages + outcome.messages` を構成し、`ng.make_provider_request(provider, "example-model", history, controls=controls)` で次のリクエストを作ります。この構成だけでは何も送信しません。次のターンには新しいユーザーメッセージや必要なツール結果も含めます。

真正な `NativeContext` 再生は、返された Assistant メッセージに加えて元の先行履歴も検査します。Assistant 出力だけを渡すと、ワイヤーディスパッチ前に `ReplayIneligible` で失敗します。その Assistant メッセージを `NativeArchive` から読み込んでも、保管権限と先行履歴のバインディングは維持されるため、元の先行履歴全体が必要です。

グラフでは `RunConfig.provider_messages` が履歴全体を受け取り、`RunResult.native_messages` はすでにキャプチャされた履歴全体を含みます。元の入力を再び先頭に追加せず、このグラフ結果を履歴として使います。`RunResult.provider_outcomes` は保持された結果を公開します。ノード内では `RunContext.provider_outcomes` と `provider_loop_history` がプロバイダー証拠を保持します。

`RunConfig.provider_messages`、`RunResult.native_messages`、`ProviderLoopEntry.messages` も独立した履歴コピーを返します。変更可能な入力履歴を更新するには、返されたリストを編集して `config.provider_messages` に再代入します。入力を置き換えた後も保持したメッセージとパートは有効で、結果とループ履歴のプロパティは読み取り専用のままです。

カスタムノードでは `provider_messages_write(messages_or_outcome)` でチャネル書き込みにネイティブ履歴を保持します。`portable_message(ChatMessage)` はポータブルな内容を取り込み、ネイティブ推論権限の取り込みを拒否します。`project_message(ProviderMessage)` は観測専用のグラフ投影を作ります。

ネイティブ永続化用の `NativeArchive.provision/open(directory, independent_key_file, owner_scope, descriptor)` はアーカイブまたは `ProviderError` を返します。`save(messages, binding="")` は参照またはエラー、`load(reference, binding="")` は型付きメッセージまたはエラーを返します。独立したホスト鍵を使い、SDK の認証されたローカル保管を行います。ポータブルな JSON に再生権限や管理予算権限を与えるものではありません。

`RuntimeHistoryRecord.serialize_canonical()` は引き続きポータブルなレコードに使えます。真正なネイティブメッセージを含むレコードを永続化するには、`owner_scope` が `owner_id` と一致する実際の `NativeArchive` で `record.serialize_canonical(archive, owner_id)` を呼びます。復元には `RuntimeHistoryRecord.parse(stored_bytes, archive=None, owner_id="")` を使います。ポータブルなレコードには既定引数で十分ですが、ネイティブアーカイブ参照には同じ所有者範囲のアーカイブが必要です。引数なしの直列化や取り込んだ JSON はネイティブ権限を再作成できません。Python の parse とアーカイブ対応の直列化は、アーカイブ I/O を含むネイティブ処理中に GIL を解放します。

`RuntimeHistoryRecord.message` は、真正な共有ネイティブ所有権を保持する独立した型付きコピーを返します。このコピーを変更しても、不変の RAW レコードの識別子は変わりません。`ContextStore.hydrate_records(range)` は型付きレコードのリストを、`history_record_by_message_id(feed, message_id)` はレコードまたは `None` を返します。`InMemoryContextStore` と `SQLiteContextStore` はこれらを継承します。`SQLiteContextStore(database_path, archive=None)` にはネイティブ保管用の実際のアーカイブを渡せます。保管権限が必要なネイティブ履歴は、アーカイブなしのストアでは拒否されます。構築と型付き取得は GIL を解放します。`LocalProgramHost` は最後の任意キーワード `native_history_archive=None` を受け取り、実際の Program ランタイムに渡します。これは保管権限を供給するもので、追加の権限や永続 Program ストアのバックエンドを供給しません。

ポータブルな状態辞書や JSON エクスポートはメッセージデータを記述します。ネイティブ再生状態、プロバイダーの出自、管理予算の権限を再作成することはできません。エクスポートされた JSON の `native` フラグは記述用であり、認可トークンではありません。`ChatMessage`、平坦なテキストチャネル、JSON への変換ではプロバイダー固有のパートが失われる場合があります。それらが必要なときは、元の先行履歴を含む型付き履歴全体を保持します。

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

### コンパイル時のツール所有権

Python `Tool`、ネイティブ C++ ツール、`MCPClient.get_tools()` の結果を
`ng.NodeContext(tools=[...])` に渡します。コンパイル前に所有する `ToolSet` に
スナップショットされ、エンジンが `run()` と `resume()` の間保持します。
後からコンテキストの `tools` を再代入しても既存のエンジンは変わらず、
次回のコンパイルにだけ反映されます。MCP ツールのネイティブ非同期処理も維持されます。

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

`RunMetadata(timeout_ms=None, trace_id="", run_id="", owner_scope="", budget_cancel_token=None)` は既定では期限なしで開始します。指定するタイムアウトと `metadata.set_timeout_ms(timeout)` は、steady clock の残りの範囲に収まる非負の整数ミリ秒を受け入れ、符号付き duration への変換や期限の加算前に範囲を検証します。負の整数や大きすぎる整数は `OverflowError` または `ValueError` になり、失敗した setter は以前の期限を保持します。ゼロは即時の期限を設定します。解除には `metadata.clear_deadline()` を使います。

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

@ng.node("my_node")
def my_node(state):
    return [ng.ChannelWrite("value", state.get("value", 0) + 1)]

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
print(result.status, result.output)
```

`LocalProgramHost`は、オーナースコープのインメモリ便宜ホストです。それでもC++コンパイラ、Catalog、admissionポリシー、遷移ストア、およびProgramRuntimeを使用します。生成された提案は、admission前にホストの意味検証を追加で通過する必要があります。[DSL機能評価](DSL_CAPABILITY_EVAL.md)を参照してください。

`LocalProgramHost` の破棄時は、実際の `ProgramRuntime` がスケジューラーの処理をキャンセルし、完了を待ち、join する間、呼び出し元の GIL を解放するため、実行中の Python ノードが終了できます。残りのホストメンバーを破棄する前に GIL を再取得し、Python コールバック/オブジェクトの所有者による GIL 安全な破棄も維持します。

ネイティブ Program の記録済み実行 API は、`start_recorded` を置き換えた `ProgramRuntime::replay_recorded` です。`LocalProgramHost` は `run`、`start`、`resume` を公開しますが、生の記録済みバインディング制御プレーンは公開しません。Program の結果は、既存のハンドル、予算、権限フィールドに加えて、ネイティブランタイムから渡された型付きプロバイダー結果と失敗の証拠を保持します。

`ProgramResult.failure` は辞書ではなく、読み取り専用の `ProgramFailure` 値または `None` です。`provider_outcome` と `provider_cause` はプロバイダー失敗の証拠を保持し、`code`、`message`、`operation_id`、`core_node`、`attempts`、`witness` は失敗した操作を記述します。結果には `bundle_id`、`operation_id`、`attempt`、`checkpoint`、`interrupt`、`provider_budget_authority` も含まれます。

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

RAW `RuntimeHistoryRecord` を作成するとき、Assistant メッセージには `RuntimeHistoryRecordData.trust` を `RuntimeTrustClass.ModelOutput` に設定します。既定の `UntrustedInput` は User メッセージだけを受け入れるため、Assistant 出力に使うと拒否されます。これらのラベルはメッセージの出自を表します。ツール実行、予算、コードの権限を与えず、真正なネイティブ再生の保管権限を代替しません。

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
outcome = profile.invoke(request)
profile.attach(engine)
```

## HITLと状態

静的`interrupt_before`/`interrupt_after`、動的`NodeInterrupt`、同期`resume`、asyncio`resume_async`、および厳密`resume_from`はチェックポイントストアを必要とします。

Python の `CheckpointStore` サブクラスは `requires_managed_budget(thread_id) -> bool` を実装できます。この同期仮想メソッドは、永続化された管理予算バンクの拒否義務を読み取ります。エンジンのネイティブ非同期ファサードがこのメソッドを呼び、バインディングは Python のオーバーライドを実行するときに GIL を取得します。チェックポイント状態からバンクを除去したり、チェックポイントを削除したりした後も、実際の永続義務を報告してください。オーバーライドを省略すると、`False` を返す代わりに、未対応バックエンドを示す明示的なネイティブエラーが維持されます。

この読み取りは支出、復元、リースの権限を付与しません。上限付きの管理実行には、対応する真正なネイティブ管理予算リースが引き続き必要です。読み取りメソッドだけを実装しても、リースは提供できません。

```python
if result.interrupted:
    result = engine.resume(result_thread_id, {"approved": True})
```

検査とタイムトラベルには`get_state_history`、`update_state`、`fork`を使用します。`get_state_view()`はフラットなPydanticベースのチャネルアクセスを提供し、`get_state()`は標準のネスト表現を保持します。

## 非同期とキャンセル

`run_async`、`run_stream_async`、`resume_async` は `asyncio.Future` を返します。これらのグラフ Future のキャンセルは `CancelToken` を通じてキャンセルを要求します。処理が終了するには、ネイティブ I/O がキャンセル境界に到達する必要があります。グラフのストリーミングコールバックは呼び出し元の asyncio ループスレッドに戻ります。

プロバイダーの `invoke` と `dispatch` は同期 Python メソッドです。バインディングはネイティブ呼び出し中に GIL を解放し、Python のプロバイダーオーバーライドやイベントコールバックでは再取得します。イベントコールバックは所有された `ProviderEvent` 値を受け取り、ネイティブのワーカースレッドで実行される場合があります。asyncio が所有する状態を更新する場合は `loop.call_soon_threadsafe` を使います。

`request.on_event = callback` でオブザーバーを指定し、`None` で解除します。`request.on_event` の読み取りは元の Python 呼び出し可能オブジェクトの同一性を保ち、`RunConfig.on_provider_event` も同じ規則に従います。各イベントは文字列の `kind` と型付き `value` を公開し、`ProviderPartDelta` 値は自身の `bytes` を所有します。コールバック後にイベントを保持しても、借用された SDK テキストビューを保持することにはなりません。キャンセルを要求するには、`request.cancel_token` に設定したトークンで `token.cancel()` を呼びます。

同期プロバイダー呼び出しをイベントループの外で実行するには、`asyncio.to_thread(provider.invoke, request)` を使います。この await のキャンセルだけでは、プロバイダー呼び出しはキャンセルされません。`request.cancel_token` に `CancelToken` を設定して、明示的にキャンセルを要求します。

有効な準備済みハンドルのトークンがすでにキャンセルされている場合、プロバイダーディスパッチの事前検査は所有された SDK の `Cancelled` 失敗を返します。同期ディスパッチも同じネイティブ非同期プロバイダーパスが終了するまで実行します。グラフ、ホスト、コルーチンの入口でのキャンセルは別の境界であり、`ProviderOutcome` が返る前に例外を送出する場合があります。キャンセル要求は、すべての境界で型付きの `Cancelled` データを保証するものではありません。また、リクエストが送信されなかったこと、リモート処理が停止したこと、課金されないことも証明しません。

[pybind11 の GIL 文書](https://pybind11.readthedocs.io/en/stable/advanced/misc.html#global-interpreter-lock-gil) は、GIL の解放と Python コールバックのための再取得が、別々のバインディング責務である理由を説明しています。

## プロトコルと可観測性

- MCPクライアントツールは、ビルド時に`neograph_engine.mcp`を通じて利用可能です。
- A2Aクライアント型は、ビルド時に`neograph_engine.a2a`を通じて利用可能です。
- `ProtocolHostAdapter`は公式Python A2A/ACPサーバーSDKをNeoGraphセッションセマンティクスと統合します。
- `neograph_engine.tracing`と`neograph_engine.openinference`は、Phoenix、Langfuse、Arizeおよび互換バックエンド向けにベンダーニュートラルなOTel/OpenInferenceデータを出力します。

### ネイティブ A2A discovery と streaming

`a2a.WireDialect` は `V0_3/V1_0`。`client.wire_dialect()` は構築/強制 card fetch 後、最初の card-selected RPC または成功 probe まで `None` です。`AgentCard.supported_interfaces` は `url/protocol_binding/protocol_version/tenant` を持つ独立した `AgentInterface` リスト、`card.raw` は観察 JSON です。互換 JSONRPC のうち正規化 base URL を優先し、card URL は RPC endpoint を変更しません。card なしでは数値 RPC error `-32601` だけが probe を許可し、card-selected 呼び出しは fallback しません。`a2a.A2ARpcError.code` は実際の整数コードです。

```python
from neograph_engine import a2a
from uuid import uuid4

client = a2a.A2AClient("http://127.0.0.1:8080")
card = client.fetch_agent_card()
for interface in card.supported_interfaces:
    print(interface.protocol_version, interface.tenant)

params = a2a.MessageSendParams()
params.message.message_id = str(uuid4())
params.message.role = "user"
params.message.parts = [
    a2a.Part.text_part("Explain this item."),
    a2a.Part.text_part("Keep the answer short."),
]
configuration = a2a.MessageSendConfiguration()
configuration.blocking = True
configuration.accepted_output_modes = ["text/plain"]
params.configuration = configuration
events = []

def on_event(event):
    events.append(event)  # Owned snapshot remains valid after the callback.
    return True

task = client.send_message_stream(params, on_event)
print(client.wire_dialect(), task.id, task.state)
```

この endpoint で実際のローカル A2A server を先に起動します。Chat ループバックとは別です。`send_message(params)` は非 streaming multipart overload で text overload も残ります。`client.set_authorization_header(authorization_header)` で認証を明示しログには残しません。`Part.media_type/file/data/metadata`、message extensions/reference IDs、task artifacts は型/データ観察で provider native 権限ではありません。

`params.message` と `Task.status` は live inline レコード、optional configuration/ベクトル/event 子は独立 snapshot です。入力変更には再代入が必要です。`StreamEvent.Type.StatusUpdate/ArtifactUpdate/Task` と `status_update/artifact_update/task/is_final()` を確認します。V1 opening task は final ではなく native SSE が status と append/replace artifact を組み立てます。blocking 呼び出しは GIL を解放し callback/所有者破棄は GIL を取得します。出力観察後に再 dispatch しません。callback thread が asyncio loop とは限らないため必要なら `loop.call_soon_threadsafe` を使います。


`neograph_engine.openinference` から `OpenInferenceProvider` をインポートします。コンストラクターは `OpenInferenceProvider(inner: Provider, tracer, *, span_name="llm.complete")` です。この Python クラスは、型付き `prepare(request)`、`dispatch(prepared)`、`invoke(request)` を継承するネイティブ C++ ラッパーに委譲します。デフォルトの span 名はラベルであり、`complete` メソッドを復元しません。`opentelemetry-api` と `opentelemetry-sdk` をインストールしてください。構築には OTel API が必要で、エクスポートには SDK の span processor/exporter の設定が必要です。

準備、承認失敗、準備済みリクエストの破棄では LLM span を作りません。承認済みディスパッチは一つの span を開始し、完了、SDK 失敗、またはディスパッチ例外で終了します。準備済み操作は、ラッパーと外部 tracer 参照が回収された後も Python tracer アダプターを保持します。ブロッキング `invoke`/`dispatch` は GIL を解放し、OTel 呼び出しと Python 参照の破棄時には GIL を取得します。Tracer 失敗にはネイティブのベストエフォート方針を適用し、所有された結果、元のイベント、製品の例外を置き換えません。

アダプターはディスパッチ時に Python tracer の `start_span` を呼びます。呼び出し元のコンテキストはネイティブのワーカースレッドへ自動伝播しません。ディスパッチ前に意図した OTel 親コンテキストを取得し、下のように tracer アダプターを通じて明示的に渡します。グラフ/ノードの親が必要なグラフノードのプロバイダーにも同じ方法を使います。OTel の [明示的な親コンテキスト選択の文書](https://opentelemetry-python.readthedocs.io/en/latest/api/trace.html#opentelemetry.trace.Tracer.start_span) を参照してください。

例の `ParentContextTracer` は取得した一つの親コンテキストを保持します。別の論理的な親を使う場合は、新たにコンテキストを取得し、新しいアダプター/プロバイダーラッパーを構築します。以前のアダプターを再利用すると、以前の親を保持したままになります。

上のプロバイダー例の `provider` と `controls` を使います。次の断片を実行する前に、一要求ピアを再起動してください:

```python
import neograph_engine as ng
from opentelemetry import context as otel_context
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import ConsoleSpanExporter, SimpleSpanProcessor
from neograph_engine.openinference import OpenInferenceProvider

class ParentContextTracer:
    def __init__(self, tracer, parent_context):
        self.tracer = tracer
        self.parent_context = parent_context

    def start_span(self, name):
        return self.tracer.start_span(name, context=self.parent_context)

traces = TracerProvider()
traces.add_span_processor(SimpleSpanProcessor(ConsoleSpanExporter()))
tracer = traces.get_tracer("python-guide")
with tracer.start_as_current_span("request"):
    observed = OpenInferenceProvider(
        provider, ParentContextTracer(tracer, otel_context.get_current()),
        span_name="llm.request",
    )
    request = ng.make_provider_request(
        observed, "example-model",
        [ng.ProviderMessage(role=ng.ProviderRole.User,
                            parts=[ng.Text("Say hello.")])],
        controls=controls,
    )
    prepared = observed.prepare(request)
    outcome = observed.dispatch(prepared)
    print("consumed:", prepared.consumed)
    print("failure:", outcome.failure is not None)
    print(outcome.text)
traces.shutdown()
```

コンソール exporter には `openinference.span.kind="LLM"` の `llm.request` が表示され、その `parent_id` は `request` span の `span_id` と一致するはずです。リンク先のピアでは、出力結果は消費済み、失敗なしで `Hello.` を含みます。新しいリクエストには `observed.invoke(request)` で同じ準備とディスパッチをまとめて実行できます。消費済みハンドルを再ディスパッチしないでください。

LLM span フィールドには承認されたモデル、宣言された temperature/出力上限、公開メッセージの役割と可視 `Text` 内容、既知の入力/出力/合計使用量カウンターが入ります。欠落したカウンターの属性は省略され、報告されたゼロはゼロのままです。ストリーミングリクエストでは可視テキスト内容のデルタだけに `llm.token` イベントを追加し、テキストを `attributes["chunk"]` に入れます。SDK 失敗は安全なメッセージで ERROR 状態を設定し、部分的な公開出力を保持します。正常完了は OK を設定します。`request.on_event` は引き続き元の型付きイベントを受け取ります。

ネイティブ再生、推論 part、生のエンベロープと生のイベントは、これらの LLM span フィールドに入りません。結果はネイティブメッセージと証拠を保持したままです。トレースのエクスポートは再生の custody や会計上の権限を与えません。公開テキストにも機微なアプリケーションデータが含まれるため、プロンプトと exporter をそれに応じて選びます。`openinference_tracer(tracer, *, root_name="graph.run", node_span_prefix="node.", on_event=None)` は別のグラフイベント用コンテキストマネージャーとして残り、プロバイダー LLM span を置き換えません。

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
