<!-- neograph-i18n: source=docs/python-binding.md locale=ja source_sha256=c7e44f8d5761953d3ea6c22c6a7aec8ca992120ecabaab1015cd859bed8e1e2d -->
# Pythonバインディング

**Languages:** [English](python-binding.md) | [한국어](python-binding.ko.md) | [日本語](python-binding.ja.md) | [简体中文](python-binding.zh-CN.md)

`neograph-engine` は、同じC++ランタイムをpybind11で公開したパッケージです。wheelでは、Core、LLM、Program/QuickJS、MCP、SQLiteによるランタイムの永続化が有効になっています。wheelを使わずソースからビルドした場合は、コンパイルしたコンポーネントだけが公開されます。

このガイドの内容は、SchemaProvider SDK `0.4.0`（インターフェースリビジョン `7`、共有ライブラリのABIリビジョン `7`）でビルドしたwheelが前提です。同梱されているコンポーネントは、まとめてインストールしてください。`complete` を公開している古いwheelはプロバイダーのインターフェースが異なるため、以下の型付きのサンプルは動きません。

```bash
pip install neograph-engine
```

## 型付きのプロバイダーリクエストと結果

`Provider` は、リクエストの準備とディスパッチを分けています。`SchemaProvider` は、SchemaProvider SDKを通じて、ファミリー固有のリクエストを検証してエンコードします。`PreparedProviderRequest` は、その準備結果（ネイティブ側のもの）を、1回のディスパッチが消費するまで保持します。

| 操作 | Pythonのシグネチャ | 結果 |
|---|---|---|
| ディスクリプターのadmission | `load_provider_descriptor(source: str, policy=None)` | `ValidatedDescriptor`。closedなJSONとして不正な場合は `ValueError` |
| プロバイダーの構築 | `SchemaProvider(descriptor, options, defaults)` | admission済みのエンドポイント／ファミリーとランタイムポリシーを持つプロバイダー |
| リクエストの構築 | `make_provider_request(provider, model, messages, tools=[], controls=ProviderControls(), mode=ProviderMode.Collect)` | 型付きの `ProviderRequest` |
| 準備 | `provider.prepare(request)` | `PreparedProviderRequest` |
| 1回だけディスパッチ | `provider.dispatch(prepared)` | 所有権を持つ `ProviderOutcome` |
| 準備とディスパッチ | `provider.invoke(request)` | 所有権を持つ `ProviderOutcome` |

`model` は明示的に指定します。`ProviderControls` で、`max_output_tokens`、`temperature`、`top_p` などの上限値と生成のコントロールを、型付きで指定します。SDKのファミリー固有のペイロードは、ファクトリーが組み立てます。生の辞書で置き換えることはできません。ストリーミングを要求するには、`request.mode` を `ProviderMode.Stream` にします。`on_event` を設定しただけでは、ストリーミングにはなりません。`request` には、`cancel_token`、`on_event`、`observer_limits`、省略可能な `timeout_ms` もあります。

`ProviderControls.provider` と `response_format`、`SchemaProviderDefaults.provider`、`OpenRouterRouting.max_price`、`ProviderToolResult.host` は、元のオブジェクトから切り離された省略可能なレコード（独立したコピー）を返します。値がある場合は、レコードを読み出して編集し、プロパティに代入し直してください。`controls.provider.order` を書き換えるだけでは、controlsは更新されません。レコードを消すには `None` を代入します。

`request.timeout_ms` に、表現できる範囲の0以上の値を代入すると、その時点から絶対的なデッドラインが動き出します。準備の直前に設定してください。その後に読み出すと残りのミリ秒数が返り、期限が切れた後は0になります。`None` のままにすると、準備の時点でプロバイダーのデフォルトのタイムアウトが使われます。準備済みのハンドルを持ったまま待っても、デッドラインは延長されません。

### ディスクリプターとランタイムポリシー

ディスクリプターは、`load_provider_descriptor` が受け付ける、バージョン付きのclosedなJSONです。ここでadmissionの対象になるのは、ファミリー、`base_url`、ルート、フィールドのバインディング、認証規則です。`model` はリクエストで、認証情報はランタイムオプションで渡します。`ProviderRuntimeOptions` には、`api_key`、`default_timeout_ms`、`ca_file`、`workers`、およびSDKのトランスポートとリソースの上限があります。`SchemaProviderDefaults` は、型付きのプロバイダーのデフォルト値を提供します。認証情報はランタイムオプションに置き、ディスクリプターのファイルやログには含めないでください。

`ProviderDescriptorPolicy.identity` は、SDKのポリシーのSHA-256ダイジェスト（生のバイト列）を、Pythonの `bytes` として返します。表示には `policy.identity.hex()` を使ってください。identity自体は、UTF-8のテキストでも、16進文字列の別名でもありません。

現在のトランスポートはlibcurlです。`prefer_libcurl` というスイッチも、実行時にエンドポイントを上書きする手段も、WebSocketトランスポートもありません。ディスクリプターのadmissionと、対応するリクエストファミリーは、[C++プロバイダーリファレンス](reference-en.md)で説明しています。

まず、[SDK使用ガイド](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#run-the-first-request-without-a-hosted-api)にある、1リクエスト分だけ応答するループバックのピアを起動します。そのうえで、別のターミナルから、テキストだけを使うこのサンプルを実行します。エンドポイント、モデル、出力の上限は、このピアに合わせてあります。認証情報は送信しません。HTTPSのピアを使う場合は、信頼するCAを `NG_EXAMPLE_CA_FILE` で指定します。ホスト型のエンドポイントを使う場合は、ディスクリプターのoriginとリクエストのモデルを明示的に変更し、そのorigin用の認証情報をランタイムオプションで渡してください。ホスト型の呼び出しには、料金がかかることがあります。

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

ハンドルが不要な場合は、`provider.invoke(request)` で準備とディスパッチをまとめて行えます。`outcome.text` は、見えるテキストだけを取り出したものです。`outcome.messages` には、型付きのパートがすべて残っています。使用量のカウンターが欠けている場合は `unknown`、ゼロが報告された場合は `0` と表示されます。

リンク先の模擬ピアを使ったときの、期待される出力は次のとおりです。

```text
consumed: True
Hello.
output tokens: 0
```

カウンターが欠けた場合の経路を試すには、ピアの `usage` メンバーをまるごと削除して、再起動します。最後の行が `output tokens: unknown` になるはずです。これで確認できるのはプロトコルのマッピングで、実際のモデルのトークン計数ではありません。

### ファミリー別のコントロール

ポータブルなコントロール（`max_output_tokens`、`temperature`、`reasoning_effort`、`store` など）は `ProviderControls` のメンバーで、ファミリー固有のコントロールはそのファミリーのネスト（`chat`、`responses`、`messages`、`generate`、`interactions`）にあります。呼び出しはプロバイダーのファミリーのネストだけを適用し、他のネストは無視するので、ノードごとに異なるファミリーを呼ぶグラフでも `ProviderControls` 1つで済みます。そのファミリーが表現できないポータブルなコントロールは、引き続き `ValueError` になります。以下の断片は、ディスパッチせずにコントロールを組み立てるだけです。最初のループバックのリクエストは、変更しないでください。OpenRouter専用のコントロールは、admission前のoriginに対して、I/Oの前に拒否されます。

```python
controls = ng.ProviderControls()
controls.max_output_tokens = 4096

# Chat Completions (OpenRouter-only members need a declared OpenRouter origin).
reasoning = ng.ChatReasoningOptions()
reasoning.effort = "low"
reasoning.enabled = True
controls.chat.reasoning = reasoning
controls.chat.usage_include = True
controls.chat.models = ["openai/gpt-6-luna", "openai/gpt-5.6-luna"]
routing = ng.OpenRouterRouting()
price = ng.OpenRouterMaxPrice()
price.prompt_nano_usd = 75_000_000       # 0.075 USD per million prompt tokens
price.completion_nano_usd = 300_000_000  # 0.30 USD per million completion tokens
routing.max_price = price
controls.provider = routing

# Responses: hosted tools the service runs.
image = ng.ResponsesImageGenerationTool()
image.size = "1024x1024"
controls.responses.hosted_tools = [image, ng.ResponsesWebSearchTool()]
controls.responses.verbosity = ng.ResponsesVerbosity.Low
controls.responses.include = [ng.ResponsesInclude.ReasoningEncryptedContent]

# Anthropic Messages: thinking, effort, caching and server tools.
controls.messages.thinking_mode = ng.MessagesThinkingMode.Adaptive
controls.messages.output_effort = ng.MessagesOutputEffort.High
search = ng.MessagesToolDefinition()
search.type = "web_search_20250305"
search.name = "web_search"
search.max_uses = 3
controls.messages.tools = [search]

# Gemini generateContent: speech output.
controls.generate.response_modalities = [ng.GeminiResponseModality.Audio]
voice = ng.GeminiVoiceConfig()
voice.prebuilt_voice_name = "Kore"
speech = ng.GeminiSpeechConfig()
speech.voice_config = voice
controls.generate.speech_config = speech

# The same controls, written as data. Unknown keys are refused.
controls = ng.ProviderControls.from_dict({
    "max_output_tokens": 4096,
    "responses": {"hosted_tools": [{"type": "image_generation", "size": "1024x1024"},
                                   {"type": "web_search"}]},
    "generate": {"response_modalities": ["Audio"],
                 "speech_config": {"voice_config": {"prebuilt_voice_name": "Kore"}}},
})
```

レコードはデフォルトで構築してからフィールドを代入するか、`from_dict` で作ります。すべてのコントロールクラスとそのネストのSDK型はdictを受け取り、未知のキーを拒否し、enumはメンバー名（`"Audio"`）で受け取り、ホスト型ツール・組み込みツール・レスポンス形式は `"type"`（`"image_generation"`、`"google_search"`、`"audio"`）からクラスを選びます。SDKにクラスがないResponsesのホスト型ツールの型は、`ResponsesOpenRouterServerTool`（`openrouter:` の型）か `ResponsesGenericHostedTool` になり、SDKのポリシーによるadmissionが必要です。ネスト自体は元のオブジェクトにつながっています（`controls.generate.speech_config = ...` は `controls` を変更します）が、その中の省略可能なレコードとベクトルは独立したコピーを返すので、編集したら代入し直してください。`OpenRouterRouting.max_price` もその1つです。`routing.max_price.prompt_nano_usd = ...` はコピーを書き換えるだけなので、`ng.OpenRouterMaxPrice` を作って、オブジェクトごと代入します。金額は整数のnanoUSDで指定します。項目は、100万トークンあたりの `prompt_nano_usd` と `completion_nano_usd`、および `image_nano_usd`、`audio_nano_usd`、`request_nano_usd` です。送信時には、正確な10進数のUSDになります（`75_000_000` は `0.075`）。少なくとも1つは必須で、どの値も2**53を超えられません。`SchemaProviderDefaults.provider` は、`max_price` を他のルーティング項目と同じように統合します。リクエスト自身の値が、デフォルト値を置き換えます。`ChatReasoningOptions` には、`max_tokens` と `exclude` もあります。`chat.reasoning`、`chat.include_reasoning`、`chat.usage_include`、`chat.models`、`chat.plugins`、`chat.image_config` には、宣言済みのOpenRouterのoriginが必要です。スカラーの `reasoning_effort`、`service_tier`、型付きのルーティング、レスポンスフォーマットは、引き続き使えます。0.15のフラットなメンバー（`chat_reasoning`、`responses_include`、`messages_tool_choice`、`gemini_thinking_level` など）は0.16.0でも動作し、対応するネストに移されます。ほかのfamilyは0.15と同じくこれを拒否し、フラットなメンバーとそのネストのメンバーを両方指定すると `ValueError` になります。0.17.0で削除されます。

Responsesのverbosityは `Low/Medium/High`、truncationは `Disabled/Auto` です。includeでは、`WebSearchSources`、`FileSearchResults`、`MessageOutputTextLogprobs`、`ComputerCallOutputImageUrl`、`CodeInterpreterCallOutputs` も指定できます。`responses.include = None` なら、暗号化されたreasoningをincludeするデフォルトのまま変わりません。`[]` を含め、リストを明示すると、そのリストだけが選ばれ、後続のネイティブリプレイの対象外になることがあります。既存のstore、reasoning、ホスト型ツール、ツール呼び出しの上限も、引き続き型付きで指定します。

Messagesのthinkingは `Manual/Adaptive/Disabled` です。Manualでは、`thinking_budget` を、admission済みの最小値以上、出力上限未満にする必要があります。モードを省略してbudgetだけを指定すると、Manualになります。Adaptive/Disabledでは、budgetを指定できません。thinkingを有効にすると、ベンダーの仕様によりtemperatureは省かれますが、モデルごとの禁止事項によって、temperatureを明示すると拒否されます。effortは `Low/Medium/High/Max/XHigh`、キャッシュのTTLは `FiveMinutes/OneHour`、tool choiceは `Auto/Any/None_/Tool` で、`Tool` には、宣言済みのクライアントツールの `name` が必要です。Messagesでルーティングを使うには、宣言済みのOpenRouterのoriginが必要です。

Geminiのthinking levelは `Minimal/Low/Medium/High` で、`thinking_budget` とは併用できません。tool choiceは `Auto/Any/None_/Validated` で、`allowed_function_names` にはリストを渡し、`required_tool` とは併用できません。safetyのカテゴリは `Harassment/HateSpeech/SexuallyExplicit/DangerousContent/CivicIntegrity`、しきい値は `BlockNone/BlockOnlyHigh/BlockMediumAndAbove/BlockLowAndAbove/Off` です。ファミリーの誤り、範囲外の値、組み合わせられないthinking・上限・ツールのコントロールは、I/Oの前に拒否されます。temperatureを禁止するモデルの接頭辞は、ASCIIの大文字小文字を区別せずに比較します。最後の `/` より後ろの部分も対象です。Chat/Responsesでは `gpt-5`、`gpt-6`、`o1`、`o3`、`o4`、Messagesでは `claude-opus-4-7`、`claude-opus-4-8`、`claude-opus-5`、`claude-sonnet-5`、`claude-fable-` が該当します。詳しくは[SDKのコントロールのadmission](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#reasoning-sampling-and-tool-controls)を参照してください。

### admission前のデプロイ用ヘッダー

通常の `load_provider_descriptor(source, policy=None)` は、`${VAR}` をそのままの文字列として扱います。次のヘルパーは、実際のディスクリプターのadmissionの前に、ホスト側の値を前処理します。

```python
environment = ng.ProviderDeploymentHeaderEnvironment()
environment.anthropic_workspace_id = "workspace-example"
environment.anthropic_beta = None
# source is Messages descriptor JSON text.
descriptor = ng.load_provider_descriptor_with_deployment_headers(
    source, [("anthropic-workspace-id", "workspace-override")], environment,
)
```

`load_provider_descriptor_with_environment_headers(source, overrides=[], policy=None)` は、Messages用に、省略可能な `ANTHROPIC_WORKSPACE_ID` と `ANTHROPIC_BETA` を環境変数から読み取ります。未設定または空の値は省かれます。優先順位は、ディスクリプターにリテラルで書いたヘッダーが環境変数より上で、明示的に渡したペアが、その両方を、大文字小文字を区別せずに上書きします。overridesの重複、不正な名前や予約された名前、改行を含む値は、admissionで失敗します。どちらのヘルパーも省略可能な `policy` を受け取ります。admission済みのディスクリプターを変更することも、エンコーダーのテンプレートを評価することもありません。

### ResponsesカーソルとGeminiの外部履歴

`previous_response_id` は、プロバイダー側が保持しているResponsesの状態を指定します。送るのは新しい入力だけで、履歴全体をもう一度送ることはしません。成功したResponses呼び出しから始めて、その呼び出しに結び付いたコントロールと、本物の終端ID（terminal ID）を保持してください。

```python
# first_request/first_outcome belong to responses_provider and response_model.
responses = ng.ProviderControls()
responses.responses.previous_response_id = first_outcome.messages[-1].id
responses.responses.previous_response_history = (
    first_request.messages + first_outcome.messages
)
new_input = [ng.ProviderMessage(
    role=ng.ProviderRole.User, parts=[ng.Text("Continue.")],
)]
next_request = ng.make_provider_request(
    responses_provider, response_model, new_input, controls=responses,
)
```

`previous_response_history` は `ProviderMessage` のベクトルで、単一のレスポンスやJSONオブジェクトにはなりません。リクエストには含まれません。クライアント側ツールの所有権が本物であることを示すために、元の先頭部分（prefix）全体と、IDがカーソルと等しい終端のアシスタントメッセージが必要になる場合があります。サーバーが保持するテキストをそのまま継続するだけなら、空のままでかまいません。その後にプロセス内で得たカーソルの出力は、終端メッセージの非公開の所有権を保持しますが、履歴全体のネイティブリプレイの権限にも、archiveの権限にもなりません。失敗した状態、編集された状態、origin・モデル・設定・ルートが一致しない状態は、拒否されます。完全なネイティブリプレイとarchiveには、これまでどおり、元の先頭部分全体が必要です。詳しくは[SDKのResponsesの継続](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#responses-provider-held-continuation)を参照してください。

呼び出し側が作った外部のアシスタント履歴（`Text` / `ProviderToolCall`）に、ネイティブな状態、ワイヤー出力、署名がない場合は、`controls.generate.history_mode = ng.GeminiHistoryMode.PortableForeign` を明示的に設定します。デフォルトは `NativeOnly` です。Googleの署名検証を迂回する仕組み（bypass）が適用されるのは、最初の外部の関数呼び出しだけです。テキストだけのターンには、署名は付きません。本物のネイティブグループには、これまでどおり厳密な検証が行われます。この設定では、失敗したシールの修復も、除去も、格下げもできません。reasoningの権限を取り込むことも、ポータブルなデータにネイティブリプレイを許可することもできません。詳しくは[SDKの外部Gemini履歴](https://github.com/fox1245/SchemaProvider/blob/poc/curl-asio-transport/docs/USAGE.md#explicit-portable-gemini-history)を参照してください。

PortableForeignでは、署名だけでなく、取り込んだワイヤーのメタデータも拒否されます。カーソルの出力は `NativeReplay.complete == False` で、archiveの対象外です。非公開の所有者に、履歴全体のリプレイを承認する効力はありません。


### 準備済みハンドルとPythonプロバイダー

ディスパッチは、失敗した場合も含めて、準備済みのハンドルを1回だけ消費します。ディスパッチの前に `prepared.valid` と `prepared.error` を、後に `prepared.consumed` を確認してください。Pythonのオブジェクトを手元に残しても、再利用はできません。もう一度試すには、新しいリクエストを組み立てて、準備し直します。

Pythonでサブクラスを作る場合は、`Provider(family)` を呼び出し、`get_name()` と `prepare(request)` を実装します。準備を `SchemaProvider` に委譲して、その本物の準備済みハンドルを返すこともできます。サブクラスが、成功したSDKの結果を勝手に作ったり、JSONからディスパッチの権限を取り込んだりすることはできません。共通の `invoke` と `dispatch` はネイティブのライフサイクルを使います。古い `complete` メソッドをオーバーライドしても、これは実装できません。

ネイティブ側の実行がPythonの `prepare` のオーバーライドを呼び出したとき、`None` を返すと、ハンドルが消費される前に `TypeError` が発生します。本物の `PreparedProviderRequest` を返してください。

`NodeContext(provider=provider)` と `ctx.provider` への代入は、元のPythonプロバイダーを、ネイティブ側の共有オーナーのリースとして保持します。ネイティブのコンテキスト、コンパイル済みノード、エンジンのコピーはこのリースを持ち続けます。そのため、`ctx.provider` を別のオブジェクトに代入し直したり、Python側の外部参照が回収されたりした後でも、同じオブジェクトのオーバーライドが呼び出されます。代入し直すと解放されるのは、変更可能なコンテキストが持つリースだけです。影響するのは以後のコンパイルで、すでにあるエンジンには影響しません。保持されるのはオブジェクトの同一性と寿命で、プロバイダーの変更可能な状態が固定されるわけではありません。最後のリースが解放されるときに、GILを取得して、Python側のオーナーを解放します。

削除された `CompletionParams`、`ChatCompletion`、`OpenAIProvider`、`RateLimitedProvider` には、互換用のエイリアスがありません。検証済みの `SchemaProvider` を構築し、リクエストファクトリーを使ってください。`ChatMessage` と `ToolCall` は、グラフ向けの便宜的な値として残っています。SDKの完全な `ProviderMessage` や `ProviderToolCall` とは、別のものです。

### 結果、失敗、使用量

`ProviderOutcome` は、SDKの結果を、変更できない形で所有して保持します。`outcome.completion` か `outcome.failure` を調べてください。該当しないほうは `None` です。completionとfailureのどちらのビューにも、順序付きの完全なメッセージ、使用量、試行のエビデンス、停止／エラーの情報、保持されたワイヤーのエビデンスが残ります。失敗した場合も、部分的な出力が残ることがあります。失敗を報告するときは、そのエビデンスを一緒に残してください。部分的なテキストを成功として返してはいけません。

`ProviderCompletion.wire_envelope` と `ProviderPartialCompletion.wire_envelope` は、ファミリー固有のエビデンスで、nullの場合があります。読む前に `wire_envelope is not None` を確認してください。現在のバッファ型のChatデコーダーは、これを `None` のままにします。その代わり、レスポンス全体のJSONを `raw_events` に、`ProviderRawWire`（SDKの `RawWire`）として保持します。`type == "chat.completion"` で、JSONの文書は `payload` に入っています。エンベロープがあると決めつけずに、実際の型付きのraw eventを確認してください。raw eventがあっても、失敗が成功になるわけではありません。エンベロープがなくても、保持されているレスポンスがないとは限りません。SDKのフォールバックでエンベロープを合成することはありません。

所有権を持つ結果か、保持されている型付きのビューを手元に残してください。本物のメッセージ、ネイティブのオーナー、利用できるワイヤーの文書は、呼び出しが終わった後も、プロバイダーのコレクションが回収された後も、有効なままです。raw payloadには、プロバイダー固有の非公開フィールドが残ります。一方、ネイティブのトレースには、rawエンベロープ／イベントも、ネイティブのリプレイやreasoningも含まれません。PythonのJSONビューはデータのコピーであり、ネイティブリプレイの権限でも、金銭に関する権限でもありません。

型付きのメッセージとパートは、丸ごと扱ってください。論理的なロールとテキストの一致だけが必要な場面では、その2つを比較します。チェックポイントとChatリクエストのテキスト内容は、テキストの文字列でも、型付きのテキストパートの配列でもかまいません。たまたま出てきた特定のシリアライズ形式を前提にしてはいけません。継続には、平坦化したテキストやJSONの代用品ではなく、完全なパートと本物のネイティブ所有権を使ってください。

`ProviderMessage.parts`、completion／partialの `messages` と `raw_events`、使用量の `extra` と `conflicts` は、結び付いた値も含めて、独立したリストまたはマップを返します。元のコレクションを置き換えた後でも、保持したパートは安全に参照できます。コピーを変更しても、変更できない結果が書き換わることはありません。メッセージを編集するには、`parts = message.parts` で取り出して `parts` を変更し、`message.parts = parts` で代入し直します。`message.parts.append(...)` では、一時的なPythonのリストが変わるだけです。

SDKの失敗は、戻り値として返るデータです。ホストのオブザーバーや予算の精算が失敗した場合は、`ProviderOutcomeError` から派生した `ProviderObserverError` か `ProviderBudgetSettlementError` が送出されます。これらは、実際の結果と原因を保持しています。したがって、コールバックが失敗したからといって、再ディスパッチしてよいことにはなりません。

保存されたPythonのプロバイダー例外やグラフ例外の原因は、何度参照しても、元の例外オブジェクトとトレースバックが保たれます。ネイティブ側の入れ子の例外変換を経由して到達した原因も同様です。

使用量のカウンターは、`value` と `evidence` を持つ `UsageCount` で、不明な場合は `None` です。報告されたゼロは、使用量が分かっている状態であり、カウンターが省かれた状態とは別です。`count.value` を読む前に、`count is not None` を確認してください。会計処理で `count or 0` を使ったり、不明な入力・出力・合計のカウンターをゼロに置き換えたりしてはいけません。

`outcome.usage.provider_cost` は、変更できない `ProviderReportedCost` のビューです。省略可能な `ProviderUsdAmount` のフィールドは `total`、`upstream_total`、`upstream_input`、`upstream_output` で、それぞれに `nano_usd`、`evidence`、`rounding` があります。`status` は、この順に並んだ4要素の配列です。`byok_status` は、null許容の `is_byok` の状態を表します。`ProviderCostStatus`、`ProviderCostSource`、`ProviderCostRounding` が保持するのは、欠落、利用可能、不正な形式、精度超過、オーバーフロー、不明な通貨、競合という各状態のエビデンスです。金額は、元の10進数の字句からではなく、パース済みのbinary64から切り上げて求めます。報告されたゼロや `is_byok=False` は、「ない」という意味ではありません。これらの観測値は、請求書でも、トークン予算の課金でもありません。呼び出しごとの金額のエビデンスは、集計したトークンバンクではなく、順序付きの `provider_outcomes` で確認してください。

`UsageAccumulator` は、トークンだけを集計します。レポートが1件だけなら、金額のメタデータもそのまま保持されます。2件目のレポートを受け取ると、スナップショットの `provider_cost` は、正規のMissing/Noneに明示的にリセットされます。古い1回目の費用でも、合計金額でもありません。このスナップショットを復元しても、費用は再現されず、トークンの権限も更新されません。正確な金額のエビデンスは、元のそれぞれの結果、またはプロバイダーのジャーナルの項目から読んでください。

再開や継続のとき、`RunResult.provider_outcomes` には、元の結果が順序どおりに残り、その後ろに新しく得られた結果が並びます。`RunResult.usage` は、現在の会計バンクを反映します。このバンクは、チェックポイントから以前のレポートを復元することがあります。新しいプロバイダー呼び出しがない再開でも、使用量が `None` やゼロになるとは限りません。エビデンスの復元で、元のプロバイダーリクエストを再ディスパッチしたり、二重に課金したりしてはいけません。保持されたレポートは、過去の呼び出しを記録したものです。新しい支出枠を与えるものではありません。

### エンドポイントの呼び出し

`Provider` は、チャット呼び出しが使うクライアントで、自身のoriginにあるSchemaProviderの非チャット・エンドポイントも実行します：

| 領域 | リクエスト | 結果 |
|---|---|---|
| 画像、decisions、埋め込み | `ImagesRequest`, `DecisionsRequest` (`DecisionQuestion`, `DecisionOption`), `EmbeddingsRequest` (`EmbeddingInput`) | `ImagesResult`, `DecisionsResult` (`DecisionAnswer`), `EmbeddingsResult` |
| rerank、moderation | `RerankRequest`, `ModerationRequest` (`ModerationInput`) | `RerankResult` (`RerankRanked`), `ModerationResult` (`ModerationVerdict`) |
| 音声合成、文字起こし | `SpeechRequest`, `TranscriptionRequest` | `SpeechResult`, `TranscriptionResult` |
| 動画ジョブ | `VideoRequest`, `JobPoll`, `JobDownload` (`JobHandle`) | `JobState` (`JobStatus`), `JobContent` |
| ファイル | `FileUpload` (`FileSource`), `FileGet`, `FileList`, `FileDelete`, `FileDownload`; チャンク方式: `FileUploadStart`, `FileUploadPart`, `FileUploadComplete`, `FileUploadQuery`, `FileUploadCancel` (`FileUploadSession`) | `ProviderFile` (`FileState`), `FilePage`, `FileDeleted`, `JobContent`, `FileUploadProgress` |
| バッチ | `BatchSubmit` (`BatchItem`), `BatchGet`, `BatchCancel`, `BatchDelete`, `BatchResults` | `Batch` (`BatchCounts`), `BatchDeleted`, `JobContent` |
| トークン数のカウント、生成統計 | `TokenCount`, `GenerationGet` | `TokenCountResult`, `GenerationResult` |

```python
request = ng.EmbeddingsRequest.from_dict({
    "model": "text-embedding-3-small",
    "inputs": [{"text": "A graph of agents."}, {"text": "A list of chores."}],
})
result = provider.complete_endpoint(request)  # blocks without the GIL
if isinstance(result, ng.ProviderFailure):
    raise RuntimeError(result.error.safe_message)
vectors = result.vectors  # one list of floats per input

# Without blocking a thread; cancelling the awaiting task cancels the call.
result = await provider.complete_endpoint_async(request, timeout_seconds=30)

# Inspect, or journal, a call before it runs.
prepared = provider.prepare_endpoint(request)
print(prepared.endpoint, prepared.model, prepared.error)  # "embeddings", the model, None
result = provider.complete_endpoint(prepared)
```

`complete_endpoint` は、リクエストの結果オブジェクトか `ProviderFailure` を返します。チャットと同じく、失敗は戻り値として返るデータです。ディスクリプターのポリシーは、呼び出しを準備するときに、originごとに各エンドポイントを許可（admission）します。ポリシーが拒否した呼び出しは、そのエラー（`prepared.error`）を持ち、ネットワークに到達しないまま `ProviderFailure` として完了します。標準のポリシーは、エンドポイントを、それを提供するプロバイダーのoriginでは許可し、ループバックのテスト用originでは許可しません。

- **リクエスト。** リクエストは、デフォルトコンストラクターで作ってフィールドを代入するか、`from_dict` で組み立てます。`from_dict` は未知のキーを拒否し、enumはメンバー名（`"Choice"`）で受け取り、入れ子の値は型に従って組み立てます：`{"inputs": [{"text": "hello"}]}`。メディア（`ProviderMedia`）、`Batch`、`ProviderRequest` はオブジェクトとして渡します。`extensions` は、SDKのポリシーがoriginに対して許可するトップレベルのメンバーで、素のPythonデータがそのまま送られます。リクエストが返すリストは独立したコピーです。リストを作ってから代入してください。
- **結果。** 結果クラスのプロパティは読み取り専用で、コピーを返します。そのため `result.vectors` はループの中で読まず、変数に保持してください。再利用される型は `ProviderMedia`、`ProviderUsage`、`ProviderServedModel`、`ProviderAttemptEvidence`、`ProviderRawWire`（`raw_events`）です。ペアはタプルのリストです（`ModerationVerdict.categories`、`DecisionAnswer.probabilities`）。
- **準備。** `prepare_endpoint(request, timeout_seconds=None)` は、`endpoint`、`model`、`error`、`consumed` を持つ `PreparedEndpoint` を返します。`timeout_seconds` は、準備の時点から数える呼び出しのデッドラインです。準備済みの呼び出しは自分のデッドラインを持つので、完了の呼び出しにデッドラインを渡すと `ValueError` が送出されます。`PreparedEndpoint` は1回だけ完了でき、2回目の完了は `ValueError` を送出します。準備のスロットに空きがないときは `prepare_endpoint` が `RuntimeError` を送出し、その場合の完了の呼び出しは代わりにSDKの `ProviderFailure` を返します。
- **ブロックしない呼び出し。** `complete_endpoint_async` は `asyncio.Future` を返し、実行中のループが必要です。`run_async` が使う、1つのasioワーカースレッド上で動きます。待機しているタスクをキャンセルすると、呼び出しとプロバイダーへの接続がキャンセルされ、そのときSDKが報告する結果（`Cancelled` の失敗）は破棄されます。
- **指定したとおりに送信。** `SchemaProviderDefaults` はチャット呼び出しにだけ適用されます。ここでは、呼び出しをジャーナルに記録することも、予算を予約したり課金したりすることもありません。
- **エンドポイントを持たないプロバイダー。** Pythonで書いた `Provider` のサブクラスにはエンドポイントがありません。`prepare_endpoint` と完了の呼び出しは `RuntimeError` を送出します。
- **ジョブ、ファイル、セッション。** `JobHandle` と `FileUploadSession` は、呼び出しの間にジャーナルへ記録し、再起動後に組み立て直せる素の値です。Googleのセッションの `url` があれば、誰でもAPIキーなしでファイルを書き込めます。認証情報と同じように非公開にしてください。`FileSource` は `bytes` を保持するか、ローカルファイル `path` の `offset` から `size` バイトを保持し、これらは送信中に読み込まれます。
- **バッチとトークン数のカウント。** `BatchItem.request` と `TokenCount.request` は、`make_provider_request` がChat、Responses、Messages、Generateのプロバイダー向けに作った `ProviderRequest` から設定します（Interactionsのリクエストは `ValueError` を送出します）。読み戻すことはできません。`batch_jsonl(descriptor, items)` はファミリーのディスクリプター向けにOpenAIの入力ファイルを書き出し、`batch_custom_id(line)` は結果の行がどのリクエストへの応答かを示し、`batch_decode_line(descriptor, request, line)` はその行をそのリクエストの結果（outcome）としてデコードします（`BatchLine.outcome` は `ProviderOutcome` です）。`batch_inline_lines(batch)` はGoogleのバッチのインライン結果を一覧にします。
- **メディアとしてのファイル、webhook。** `files_media(file, origin, now)` は、有効で期限切れでない `ProviderFile` を、チャットのリクエストが指定する `ProviderMedia` の参照に変換し、別のoriginの場合は `ValueError` を送出します。`verify_webhook(secret, headers, body, now, tolerance=300)` は、署名付きのwebhook（OpenAIが送るもの）を検証して `WebhookEvent` を返し、不正なものや古いものは `ValueError` を送出します。ホストは生のbodyと自分の時計を渡し、リプレイを拒否するために、処理済みの配信idを記憶しておきます。

### ネイティブ履歴とポータブルなエクスポート

返された `ProviderMessage` は、型付きのパートをすべて持ち、本物のネイティブリプレイの状態を保持しています。ネイティブリプレイの状態とワイヤー出力は、読み取り専用です。プロバイダーを直接継続する場合は、元のリクエストの `request.messages` 全体を先頭部分として残し、その後ろに、返された出力だけを含む `outcome.messages`（入力履歴の全体ではありません）を追加します。最初の呼び出しのサンプルの `request` と `outcome` を使うと、`history = request.messages + outcome.messages` を作り、それから `ng.make_provider_request(provider, "example-model", history, controls=controls)` で次のリクエストを組み立てます。この組み立てだけでは、何も送信されません。次のターンには、新しいユーザーメッセージや、必要なツールの結果も含めてください。

本物の `NativeContext` のリプレイでは、返されたアシスタントメッセージだけでなく、元の先頭部分も検査されます。アシスタントの出力だけを渡すと、ワイヤーへのディスパッチの前に `ReplayIneligible` で失敗します。そのアシスタントメッセージを `NativeArchive` から読み込んでも、保管と先頭部分の結び付きは保たれます。それでも、元の先頭部分全体が必要です。

グラフでは、`RunConfig.provider_messages` に履歴全体を渡せます。`RunResult.native_messages` には、取得済みの履歴全体がすでに入っています。このグラフの結果を履歴としてそのまま使い、元の入力をもう一度先頭に付け足さないでください。`RunResult.provider_outcomes` は、保持された結果を公開します。ノードの内側では、`RunContext.provider_outcomes` と `provider_loop_history` がプロバイダーのエビデンスを保持します。

`RunConfig.provider_messages`、`RunResult.native_messages`、`ProviderLoopEntry.messages` も、独立した履歴のコピーを返します。入力として渡す履歴を更新するには、返されたリストを編集して、`config.provider_messages` に代入し直します。その入力を置き換えた後でも、保持したメッセージとパートは有効です。結果とループ履歴のプロパティは、読み取り専用のままです。

カスタムノードで、チャネルへの書き込みにネイティブ履歴を残すには、`provider_messages_write(messages_or_outcome)` を使います。`portable_message(ChatMessage)` は、ポータブルな内容を取り込みます。取り込んだネイティブreasoningは拒否します。`project_message(ProviderMessage)` は、観測専用のグラフ向け射影を作ります。

ネイティブな履歴を永続化するには、`NativeArchive.provision/open(directory, independent_key_file, owner_scope, descriptor)` を使います。archiveか `ProviderError` が返ります。`save(messages, binding="")` は参照かエラーを、`load(reference, binding="")` は型付きのメッセージかエラーを返します。これは、SDKの認証付きのローカル保管を、ホスト側の別の鍵で行うものです。ポータブルなJSONが、リプレイや管理対象の予算の権限になるわけではありません。

ポータブルなレコードには、これまでどおり `RuntimeHistoryRecord.serialize_canonical()` を使えます。本物のネイティブメッセージを含むレコードを永続化するには、`owner_scope` が `owner_id` と一致する実際の `NativeArchive` を使って、`record.serialize_canonical(archive, owner_id)` を呼びます。復元には `RuntimeHistoryRecord.parse(stored_bytes, archive=None, owner_id="")` を使います。ポータブルなレコードならデフォルトの引数で足りますが、ネイティブarchiveへの参照を含む場合は、同じオーナースコープのarchiveが必要です。引数なしのシリアライズや、取り込んだJSONから、ネイティブの権限を再作成することはできません。Pythonのparseと、archiveを渡すシリアライズの呼び出しは、archiveのI/Oを含むネイティブ側の処理の間、GILを解放します。

`RuntimeHistoryRecord.message` は、本物の共有ネイティブオーナーを保持した、独立した型付きのコピーを返します。このコピーを変更しても、変更できないRAWレコードの識別情報は書き換わりません。`ContextStore.hydrate_records(range)` は型付きのレコードを返し、`history_record_by_message_id(feed, message_id)` はレコードか `None` を返します。`InMemoryContextStore` と `SQLiteContextStore` は、これらのメソッドを継承します。`SQLiteContextStore(database_path, archive=None)` には、ネイティブの保管用に、本物のarchiveを渡せます。保管が必須の場合、archiveのないストアはネイティブ履歴を拒否します。構築と型付きの取得では、GILを解放します。`LocalProgramHost` は、最後の省略可能なキーワード引数として `native_history_archive=None` を受け取り、実際のProgramランタイムに渡します。これが提供するのは保管であり、追加のグラントや、永続的なProgramストアのバックエンドではありません。

ポータブルな状態の辞書とJSONのエクスポートは、メッセージのデータを表すものです。ネイティブリプレイの状態、プロバイダーのorigin、管理対象の予算の権限は、これらからは再作成できません。エクスポートしたJSONの `native` フラグは、状態の説明にすぎず、認可のトークンではありません。`ChatMessage`、平坦なテキストチャネル、JSONのいずれかを経由して変換すると、プロバイダー固有のパートが失われることがあります。そうしたパートが必要な場合は、元の先頭部分を含む型付きの履歴全体を保持してください。

## Coreグラフのクイックスタート

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

### コンパイル時のツールの所有権

Pythonの `Tool` インスタンス、ネイティブのC++ツール、`MCPClient.get_tools()` の結果は、まとめて `ng.NodeContext(tools=[...])` に渡します。コンパイル時には、グラフのノードを構築する**前**に、このリストがネイティブ側の `ToolSet` としてスナップショットされ、エンジンが所有します。コンパイル済みのエンジンは、コンテキストの `tools` のリストを置き換えたり、外部の参照を手放したりした後でも、`run()` と `resume()` を通じて、まったく同じツールを保持し続けます。後からリストを変更しても、影響するのはその後のコンパイルだけです。ネイティブのMCPツールは、非同期の実行経路をそのまま使います。コンパイル後に所有権を移す処理や、ディスパッチごとに所有権のための確保を行うことはありません。

### 標準エラー出力の lint 通知

`schema_version` が 1 のグラフを `GraphEngine.compile` でビルドすると、検証の警告が標準エラー出力に出ます。たとえば `[neograph] lint [E7] $: node(s) unreachable from __start__ ...` です。`GraphEngine.compile(..., stderr_notices=False)` は、この警告と、fan-out が直列で実行されたことを知らせる 1 回限りの通知をまとめて止めます。C++ の `EngineConfig::stderr_notices` に当たる Python 側のスイッチで、デフォルトは `True` です。検証エラーは従来どおり例外になります。

`Send` または `Command.goto` だけが開始するノードは、到達できないノードとして報告されます。グラフ定義に動的な対象を宣言する場所がないためで、この場合は通知が想定どおりです。通知を消すためにエッジを足さないでください。Send の対象への静的エッジは、対象を送信側の状態でもう一度実行させます（[Send](concepts.md#5-send--dynamic-fan-out) を参照）。

## Core APIの同等性（parity）

Pythonでは、Python専用のスケジューラーを別に用意するのではなく、C++の実行機能をそのまま公開しています。

- 同期とasyncioの、run／stream／resume
- チェックポイントを正確に指定する `resume_from`、フォーク、状態の確認、順序付きの状態書き込み
- グラフの割り込みと `NodeInterrupt` による、静的・動的なHITL
- `RunMetadata` によるデッドライン、トレース／実行のID、モデルのトークン上限
- グラフ全体とノードごとの `RetryPolicy`（ジッターを含む）
- 実行ごとに閉じた、または明示的に再利用できる `CacheScope`
- チェックポイントと長期Storeのバックエンド
- カスタムのノード、リデューサー、条件、プロバイダー、ツール
- ツールのゲート、実行ポリシー、必須のライフサイクルHook、strictなランタイムのインターポジション

### 同等性の契約

ここでの「同等性」とは、Pythonが、C++と同じネイティブの実行経路と安全性の契約に到達することを指します。C++内部のストレージや権限の型が、すべてPythonにコピーされているという意味ではありません。

| 機能 | ネイティブのC++の経路 | Pythonのインターフェース | 状況 |
|---|---|---|---|
| Coreグラフのコンパイルと実行 | `GraphEngine` | `GraphEngine.compile`、run／stream／asyncのメソッド | 同じスケジューラーとランタイム |
| 実行ごとの識別情報、デッドライン、予算 | `RunMetadata`、`RunConfig` | `RunMetadata`、`RunConfig.model_token_budget` | 実行ごとの値は同じ |
| リトライとノードキャッシュのポリシー | `RetryPolicy`、`CacheScope` | グラフ／ノードのセッターとキャッシュスコープ | 同じランタイムポリシー |
| チェックポイント、HITL、タイムトラベル | チェックポイントストアと再開のAPI | resume、`resume_from` の正確な指定、フォーク、状態の履歴／更新 | 同じチェックポイントの契約 |
| A2A または ACP でエージェントを提供する | `A2AServer`、`ACPServer` | `ProtocolHostAdapter` の背後の公式 SDK サーバー | セッション、キャンセル、トークンのストリーミング、グラフのインタラプト(`input-required`、権限リクエスト)。コラボレーションエンベロープ、Program ベースのタスク、テナント分離はなし |
| Programの作成とローカル実行 | コンパイラー、Catalog、`ProgramRuntime` | `ProgramCompiler`、`LocalProgramHost`（`admit`、`start`/`resume`/`fork`、`replace`、`migrate_graph`、`register_synthesis_validator`、`prepare_child_synthesis`）、ハンドル／結果 | ネイティブの、オーナー単位の簡易ホスト |
| 必須のライフサイクルHook | レジストリ、ランナー、`HookRuntime` | 定義と `create_hook_runtime` のコールバック | 同じフェイルクローズのライフサイクル境界 |
| ランタイムコンテキストとstrictディスパッチ | コンテキストストア、レシート、インターポジション | 対応する不変の値、ストア、`StrictRuntimeProfile` | 同じネイティブのコントローラー |
| wheelのデフォルトの永続化 | SQLiteのCore／コンテキスト／ディスパッチのストア | `_HAVE_SQLITE` のエクスポート | PyPIのwheelで有効 |

生の `ProgramCatalog`、遷移ストア、Hookのジャーナル、RPCエグゼキューターは、これまでどおり、ホストが組み立てるためのAPIです。`LocalProgramHost` はオーナー単位のランタイム上で、`register_synthesis_validator` と `proposal -> compile -> admit -> publish` の子合成ゲートウェイ、ハンドオフに基づく `replace` / `replace_async`、および `migrate_graph` / `migrate_graph_async` を公開し、バインディング独自のポリシーは追加しません。`_HAVE_PROGRAM` は、生のマルチテナントコントロールプレーンの管理までC++と同等であることを示すものではありません。

`GraphEngine::run_until_safe_point_async` と `resume_from_until_safe_point_async` には、Pythonでの対応メソッドがありません。セーフポイントのリクエストは正確に1つの `GraphGenerationIdentity` を指し、エンジンは、その世代（generation）にリンクされている場合にだけリクエストを受け付けます。Programカタログは、実体化するエンジンに対してこのリンクを行います。`GraphEngine.compile` は世代を持たないエンジンを作るので、すべてのリクエストを拒否します。セーフポイントの取得は、上に挙げた置き換え／マイグレーションのコントローラーが担います。

### 実行時のリトライの上書き

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

グラフ定義の `"retry_policy"` は、これまでどおり宣言的なデフォルトです。実行時のセッターは、C++とPythonで共通の、これとは別の設定手段です。

### メタデータと正確な再開

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
checkpoint_id = result.checkpoint_id

# Never substitutes a newer checkpoint:
result = engine.resume_from(config, checkpoint_id, {"approved": True}, metadata)
```

Pythonのノードの内側では、同じ値を `input.ctx.trace_id`、`run_id`、`has_deadline`、`deadline_remaining_ms`、`model_token_budget` から参照できます。

すべての run、stream、resume のエントリポイントが `metadata` を受け取ります。`run`、`run_stream`、`run_async`、`run_stream_async`、`resume`、`resume_from`、`resume_async`、`resume_from_async` です。オブジェクトは呼び出した時点でコピーされるので、あとから変更しても進行中の実行には届かず、1つの `RunMetadata` を複数の呼び出しに使えます。再開は独自の `RunMetadata` を受け取ります。以前の試行で決めた絶対デッドラインは古くなっている可能性があるため、中断した実行のデッドラインは引き継がれません。`resume` と `resume_async` では、キーワードで渡します。`RunMetadata` 以外の引数を渡すと `TypeError` が発生します。

`RunMetadata(timeout_ms=None, trace_id="", run_id="", owner_scope="", budget_cancel_token=None)` は、デフォルトではデッドラインなしで始まります。渡すタイムアウトと `metadata.set_timeout_ms(timeout)` には、steady clockの残り範囲に収まる、0以上の整数のミリ秒を指定できます。符号付きのdurationへの変換や、デッドラインへの加算を行う前に、この範囲が検証されます。負の整数や大きすぎる整数を渡すと、`OverflowError` か `ValueError` が発生します。セッターが失敗しても、以前のデッドラインは変わりません。0を指定すると、即時のデッドラインになります。デッドラインを取り除くには、`metadata.clear_deadline()` を使います。

### キャッシュスコープ

```python
engine.set_node_cache_enabled("pure_parser", True)  # execution-local default
engine.set_node_cache_enabled("pure_parser", True, ng.CacheScope.Reusable)
```

`Reusable` は、そのノードが、テナント、プロバイダー、Store、ツール、認証情報、時刻、再開の状態のいずれにも依存しない、と明示的に宣言するものです。

## ProgramとQuickJS

Pythonのwheelには、`neograph::program` と、機能を絞ったQuickJSのフロントエンドが含まれています。Pythonで定義したノードは、変更できないProgramレジストリに登録でき、ネイティブの `ProgramRuntime` で実行できます。

```python
import neograph_engine as ng

@ng.node("my_node")
def my_node(state):
    # GraphState.get takes only the channel name; a missing channel is None.
    return [ng.ChannelWrite("value", (state.get("value") or 0) + 1)]

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
print(result.status, result.output["value"])  # ProgramTerminalStatus.Completed 1
```

`ProgramRegistryBuilder.add_registered_node`、`add_registered_reducer`、`add_registered_condition` は、このプロセスにすでに登録されている実行可能なもの（`@ng.node` または `ng.NodeFactory.register_type`、`ng.ReducerRegistry.register_reducer`、`ng.ConditionRegistry.register_condition` で登録したもの）をコピーします。登録されていない名前を渡すと、いずれも `ng.ProgramRegistryError` を送出し、メッセージには、どの登録呼び出しが必要かが書かれています。このエラーの `kind` は `"node"`、`"reducer"`、`"condition"` のいずれかで、`name` は見つからなかった名前です。`ValueError` と `RuntimeError` の両方のサブクラスなので、どちらかを想定して書いたハンドラーも、引き続きこのエラーを捕捉します。レジストリのスナップショットにないノードタイプ、リデューサー、条件を使うProgramは、代わりにコンパイル時に失敗します。`ProgramCompiler.compile` でも `LocalProgramHost.compile_admit` でも同じで、`ng.ProgramCompileError` が発生し、診断コードは `P_CORE_GC_REGISTRY_MISS` になります。

診断の辞書にはそれぞれ `help` という項目があります。コンパイルが返す診断では、`doc`（[診断ガイド](diagnostics.md)での該当項目）、`related`、`suggestions`、`candidates` を持つ辞書です。シール済みの `bundle.diagnostics` では `None` です。例外には、常にこれが付いています。`ProgramCompiler(..., echo_routes=True)` を指定すると、ルーティング後の後続ノードの表を示す `P_CORE_ROUTES` の注記も返ります。`ProgramCompiler` または `LocalProgramHost` に `soundness_check=True` を指定すると、[健全性チェック](program-graphs.md#soundness-check)が走ります。出力は、witnessのトレース付きの `P_CORE_S1` から `P_CORE_S5` までの警告と、`P_CORE_SOUNDNESS` の要約です。これらはいずれも、バンドルにはシールされません。

`LocalProgramHost` は、オーナー単位で、メモリ上で動く簡易ホストです。内部では、C++のコンパイラー、Catalog、admissionポリシー、遷移ストア、ProgramRuntimeをそのまま使います。生成されたproposalは、admissionの前に、ホスト側の意味検証（semantic validator）も通すようにしてください。詳しくは[DSLの機能評価](DSL_CAPABILITY_EVAL.md)を参照してください。

`LocalProgramHost` を破棄すると、実際の `ProgramRuntime` がスケジューラーの処理をキャンセルし、drainし、joinする間、呼び出し側のGILが解放されます。そのため、実行中のPythonノードは終了できます。残りのホストのメンバーを破棄する前には、GILを再取得します。メンバーが持つPythonのコールバックとオブジェクトのオーナーは、GILに対して安全な破棄を保ちます。

ネイティブのProgramには、記録済みの実行を扱うAPIとして `ProgramRuntime::replay_recorded` があります。`LocalProgramHost` が公開するのは `run`、`start`、`resume`、`fork` で、生のrecorded-bindingのコントロールプレーンは公開しません。Programの結果には、従来のハンドル、予算、権限のフィールドに加えて、ネイティブランタイムが提供する、型付きのプロバイダーの結果と失敗のエビデンスが残ります。

`ProgramResult.failure` は、辞書ではなく、読み取り専用の `ProgramFailure` の値か `None` です。`provider_outcome` と `provider_cause` はプロバイダーの失敗のエビデンスを保持し、`code`、`message`、`operation_id`、`core_node`、`attempts`、`witness` は失敗した操作を説明します。結果には、`bundle_id`、`operation_id`、`attempt`、`checkpoint`、`interrupt`、`provider_budget_authority` のフィールドもあります。`native_history_archive` のないホスト（既定のメモリ上のホストがそうです）では、`provider_outcome` は native 状態を除いたメッセージを持ち、`witness["provider_outcome_native_retained"]` は `False` になり、元の outcome は `provider_cause` に残ります。

インストールされているJavaScriptの語彙（vocabulary）は、辞書として正確に取得できます。

```python
manifest = ng.javascript_authoring_capability_manifest()
```

### モジュールの形と `callCore`

JavaScriptのProgramは、`define()` をエクスポートする必要があります。これはグラフビルダーを1つ返す関数です。そのグラフが、そのProgramで唯一のCoreです。`define()` がないと、コンパイルは `ProgramCompileError` と `P_JS_DEFINE_MISSING` で失敗します。つまり、`main()` だけをエクスポートするモジュールは拒否されます。`main()` は省略可能で、省略した場合、実行はCoreを直接動かします。

`ng.callCore(name, input, sourceSite)` には、`ng.graph(...)` に付けた名前で、そのCoreを指定する必要があります。上の例では `"main"` です。ほかの名前を指定すると、実行は `P_JS_CONTROL_COMMAND` で失敗します。別の手順を実行したい場合は、その手順をこのグラフの中に入れ、入力を変えて、もう一度呼びます。すべてのノードタイプは、`add_registered_node` に渡す前に、`@ng.node` か `ng.NodeFactory.register_type` で登録してください。登録されていないタイプを渡すと、`ng.ProgramRegistryError` が発生します。

`callCore` が完了すると、Coreの出力オブジェクトが `main()` に返されます。上のクイックスタートでは、これをそのまま `result.output` として返しています。

```json
{"channels": {"value": {"value": 1, "version": 1}}, "global_version": 1, "value": 1}
```

`channels` には、各チャネルの `value` と `version` が入ります。`version` は、すべてのチャネルが共有する書き込みカウンターが、そのチャネルへの最後の書き込み時点で持っていた値です。`global_version` は、実行の最後の書き込み後のカウンターです。どちらも書き込みの順序を表すだけで、チャネルが受けた書き込みの回数でもスーパーステップの番号でもありません。一度も書き込まれていないチャネルのバージョンは0です（[バージョン](concepts.md#versions)を参照）。各チャネルの値は、同名のキーがまだなければ、そのチャネル名のトップレベルのキーにもコピーされます。そのため、`main()` は `result.value` を読めます。`channels` または `global_version` という名前のチャネルには、`channels` の下からしか到達できません。バリアのリストは、ソートして保存されます。`graph.barrier("join", ["zeta", "alpha"])` は、`bundle.sealed_core_definitions` では `{"wait_for": ["alpha", "zeta"]}` になります。順序は動作に影響しません。

### コマンドが失敗すると実行が終わる

`main()` が再開されるのは、コマンドが完了したときだけです。yieldしたコマンドが完了しないことがあります。たとえば、`callCore` のノードが例外を送出した場合、Coreのステップ予算を使い切った場合、発火できなくなったバリアで止まった場合、実時間の上限を超えた場合です。このときは、`ProgramRuntime` がその結果を記録し、対応する `status` と `failure` を持つ、実行の最終結果にします。ジェネレーターは再開されません。`yield` を囲む `try`/`catch` は実行されず、それより後のJavaScriptも実行されません。failure policyに従って失敗したjoin（`ng.all`、`ng.race`、`ng.quorum`）も、同じように実行を終わらせます。失敗の処理は、Python側で `result.status` と `result.failure` を使って行ってください。

割り込まれたコマンドは扱いが異なります。実行は `Interrupted` で止まり、`resume()` がそのコマンドを続行します。joinのメンバーが割り込まれると、そのjoinは一時停止します。joinは、それ以上メンバーを起動せず、すでに動いているメンバーが終わるのを待ちます。それらの結果でjoinが決まる場合（fail-fastの失敗、raceの勝者、quorumの充足または到達不能）は、その結果でjoinが終わります。そうでない場合、実行は最初に一時停止したメンバーで `Interrupted` として止まります。`resume()` は、そのメンバーを続行し、終了済みのメンバーはコマンドジャーナルからリプレイします。終了済みのメンバーが再実行されることも、再課金されることもありません。

`ng.await(command, timeout_ms)` のデッドラインは絶対時刻です。コマンドジャーナルにはawaitを開始した時刻が記録されるので、再開した場合も、停止したプロセスを復旧した場合も、残りの時間しか使えません。デッドラインを過ぎてから再開すると、awaitは回答されたノードを実行せずに、すぐ実行を `TimedOut`、`P_AWAIT_TIMEOUT` で終わらせます（#391）。プランレベルの `await` オペレーションも、同じように、再開をまたいでデッドラインを維持します。

不確かな効果（payloadの `kind` が `tool_effect_reconciliation` の `NodeInterrupt`）で一時停止したメンバーは、Coreのステップと同じく、メンバーの失敗よりも優先されます（#392）。joinは、失敗せずにそのメンバーで一時停止し、失敗したメンバーを記録します。再開されると、その失敗で終わります。成功によって決まるjoinは、そのとおりに終わります（#397）。再開時には、答えが続行するメンバーが最後まで実行されてから、記録済みの失敗でjoinが終わります。これにより、答えがそのメンバーに届き、その効果が確定します（#400）。joinが一時停止するときは、宣言順にかかわらず、まず、不確かな効果で一時停止したメンバーで止まります（#401）。収集型の `ng.all`（`collect: true`）は、この優先順位の意図的な例外です。一時停止したメンバーを含め、すべてのメンバーを待ってから報告します。そのため、別のメンバーが通常の質問で一時停止している間は、メンバーが失敗してもjoinは終わりません。実行はそのメンバーで `Interrupted` として止まり、失敗は、すべてのメンバーが確定してから報告されます（#401）。

### グラフのチェックとバリアの停止

コンパイルでは、スケジューラーが実際にたどる経路に沿って、Coreグラフをチェックします。条件付きエッジを持つノードは、最初の条件付きエッジだけを通ってルーティングされます。JavaScriptのProgramはstrictなドキュメントなので、次のそれぞれが `ng.ProgramCompileError` を発生させます。

- `P_JS_GRAPH_ARGUMENT`：ノード名に `__start__` か `__end__` を指定した `.node()` の呼び出し
- `P_JS_GRAPH_ENTRY`：`define()` が、ノードはあるが `.entry()` のないグラフを返した場合
- `P_CORE_E13`：条件付きエッジも持つノードから出る静的エッジ。そのエッジは発火しないため
- `P_CORE_E14`：同じノードから出る2本目の条件付きエッジ。ルーティングに使われないため

`P_CORE_E15` は `bundle.compile_diagnostics` に出る警告です。バリアのメンバーに、joinでも `__end__` でもない場所へ向かうルートがあり、そのルートを通るとjoinが止まってしまう、という内容です。ループは有効です。プランやチェックリストなど、すべてのサイクルを `P_CORE_E16` で拒否したい場合は、`ProgramCompiler` または `LocalProgramHost` に `require_acyclic=True` を渡します。

バリアが発火するのは、`wait_for` のリストにあるすべてのノードが、そのバリアにルーティングし終えたときです。別の経路でjoinに到達したノード（たとえば、メンバーの失敗ルート上の後始末用ノード）は、そのメンバーの代わりにはなりません。実行する対象が残っていないのに、joinにシグナルが残っている場合、実行は完了しません。`P_CORE_BARRIER_STALLED` で `Failed` として終わります。`failure.core_node` にはそのjoinの名前が入り、`failure.witness["stalled_barriers"]` には、止まったjoinごとに `wait_for`、`received`、`missing` のノードが並びます。このような実行が正常に終わるのは、最後のスーパーステップで `__end__` への明示的なルートを通った場合だけです。`__end__` への静的エッジ（`.exit()` が書き込みます）、`__end__` への条件付きルート、`Command.goto("__end__")` が該当します。後続ノードがないだけでは終わりません。それより前のスーパーステップでの明示的な終了でも終わりません。同じ状態の `GraphEngine` の実行は `ng.RunStatus.BarrierStalled` を返し、`result.output["_neograph"]["barrier_stalled"]` に同じ詳細が入ります。

### エラーと診断

Pythonで発生するProgramのエラーは型付きで、診断を保持しています。

| 例外 | 発生元 | 継承元 |
|---|---|---|
| `ng.ProgramCompileError` | `ProgramCompiler.compile`、`LocalProgramHost.compile`、`compile_admit` | `RuntimeError` |
| `ng.ProgramAdmissionError` | `LocalProgramHost.admit` と `compile_admit`（例：`P_ADMIT_POLICY`、`P_ADMIT_PROVIDER`） | `RuntimeError` |
| `ng.ProgramDiagnosticError` | `run`、`start`、`start_active`、`resume`、`fork`、`replace`、`migrate_graph`、`prepare_child_synthesis`（例：`P_START_BUDGET`、`P_RUN_NOT_FOUND`、`P_REPLACEMENT_AUTHORITY`） | `RuntimeError` |
| `ng.ProgramRegistryError` | `ProgramRegistryBuilder.add_registered_node`、`add_registered_reducer`、`add_registered_condition` | `ValueError`、`RuntimeError` |

最初の3つの例外のメッセージは、`Program compilation failed`、`Program admission failed`、`Program operation failed` のいずれかで始まります。その後ろに、診断1件につき1行が続きます。内容は、コード、JSON pointer、メッセージで、ソース上の位置がある場合は `(source:line:col)` も付きます。`.diagnostics` は、`code`、`severity`、`phase`、`message`、`source_id`、`json_pointer`、`span`、`witness` を持つ辞書のリストです。

コンパイルに成功したバンドルは、2つのリストを持ちます。`bundle.compile_diagnostics` は、コンパイラーが返したものです。現行のルールによる警告が入り、JavaScriptの呼び出し位置は `span` に入ります。`P_JS_CONTROL_CHARACTER` のような、作成時の警告も含まれます。作成者には、こちらのリストを見せてください。`bundle.diagnostics` は、バンドルにシールされたコピーで、`bundle.id` の対象です。バンドルが最初にシールされたときのルールと文言のままなので、新しいコンパイラーでもバンドルIDは変わりません。そのため、内容が異なることがあります。spanはなく、現行のルールでは出なくなった警告が残っている場合もあります。たとえば、Pythonのノードだけが書き込むチャネルに対する `P_CORE_E6` です。コンパイラーも `LocalProgramHost` も、lintの結果を標準エラー出力には出力しません。

<a id="model-calling-nodes"></a>
### モデルを呼び出すノード

`LocalProgramHost` は、admissionしたProgramの組み込みの `llm_call` ノードに、プロバイダーを1つ結び付けます。レジストリは、プロバイダーを名前で宣言し、それを必要とするノードも宣言します。ホストは、プロバイダーのオブジェクト、モデル、リクエストのコントロールを、秘密情報を含まない `binding_label` の下で提供します。

```python
from neograph_engine.llm import SchemaProvider

registry = (
    ng.ProgramRegistryBuilder()
    .add_provider("openrouter", "1.0.0", "sha256:" + "3" * 64)
    .add_registered_node("llm_call", "1.0.0", "sha256:" + "4" * 64, provider="openrouter")
    .add_registered_reducer("append", "1.0.0", "sha256:" + "5" * 64)
    .build()
)
controls = ng.ProviderControls()
controls.max_output_tokens = 256
ceiling.model_tokens = 100_000
ceiling.monetary_microunits = 500_000
# Prices are host data: a catalogue layer over the embedded catalogue, in the
# provider's policy. This one prices one OpenRouter model and its route ceiling.
prices = {"version": 1, "models": [{
    "id": "deepseek/deepseek-v4-flash-0731", "origins": ["https://openrouter.ai"],
    "pricing": {"input": "0.000000018", "output": "0.00000064",
                "ceiling_input": "0.00000044", "ceiling_output": "0.00000132"}}]}
policy = ng.load_provider_policy(ng.provider_policy_json(), ng.provider_codec_defaults_json(),
                                 json.dumps(prices))
# descriptor_json: a descriptor as in "Descriptor and runtime policy" above, for
# https://openrouter.ai; options as there, with the OpenRouter key.
descriptor = ng.load_provider_descriptor(descriptor_json, policy)
host = ng.LocalProgramHost(
    registry, "tenant-a", ceiling,
    node_context=ng.NodeContext(provider=SchemaProvider(descriptor, options),
                                model="deepseek/deepseek-v4-flash-0731",
                                provider_controls=controls),
    binding_label="openrouter-prod")

source = ng.ProgramSource.from_javascript("chat.js", r'''
export function define() {
  const graph = ng.graph("chat");
  graph.channel("messages", {reducer: "append", initial: []});
  graph.node("llm", {type: "llm_call"});
  graph.entry("llm");
  graph.exit("llm");
  return graph;
}
''')
run_budget.model_tokens = 100_000
run_budget.monetary_microunits = 500_000  # 0.50 USD
version = host.compile_admit(source, run_budget)
result = host.run(version, {"messages": [{"role": "user", "content": "Hi"}]}, run_budget)
print(result.usage.monetary_microunits, [r.evidence for r in result.monetary_records])
```

`add_provider(name, semantic_version, implementation_digest)` は、プロバイダーのIDを登録します。`add_registered_node("llm_call", ..., provider=name)` は、そのIDを静的な要件として持つ組み込みノードを登録します。`provider=` で結び付けられるのは `llm_call` だけで、ほかのタイプや、未知のプロバイダーを指定すると `ValueError` になります。ホストは、`node_context` のプロバイダーを、ディスクリプターのIDにかかわらず、この名前に結び付けます。`NodeContext(provider_controls=...)` には、ノードが送る型付きの `ProviderControls`（たとえば `max_output_tokens`）を渡します。`node_context` に必要なのは、プロバイダーと `binding_label` で、ツールは不要です。

結び付けのうち、どのIDにも含まれるのはラベルだけです。プロバイダーを必要とするProgramのバージョンには、機能のレシートが付きます。このレシートのIDは、ラベルとプロバイダーのレジストリ上のIDのダイジェストです。プロバイダーのキー、エンドポイント、モデルは含まれません。したがって、モデルを変えてもバージョンIDは変わらず、ラベルを変えると変わります。プロバイダーを必要としないProgramにはレシートが付かず、バンドルIDとバージョンIDも変わりません。ラベルは、英小文字（`a-z`）、数字（`0-9`）、`.`、`_`、`-`、`:` からなる1〜64文字で、先頭は英字か数字にします。APIキーに見える文字列にしてはいけません。`node_context` がないと、プロバイダーを必要とするProgramは、admissionで `P_ADMIT_PROVIDER` により拒否されます。`provider=` なしで登録した `llm_call` ノードや `intent_classifier` ノードを持つProgramも同様です。

各呼び出しの料金は、プロバイダーのポリシーが持つモデルカタログだけを使って、実行の `monetary_microunits` に対して計算されます。組み込みのカタログには価格が載っていないので、ホストが上で示したように自分の価格を重ねます。項目の `pricing` は、単位あたりの10進 USD を与えます。`input`、`output`、キャッシュ・推論・画像の単位、OpenRouterのルートの上限（`ceiling_input`、`ceiling_output`）、service tier（`input/flex`）、long-context の帯（`input+200000`）、サーバーツールの料金（`tool:web_search`）です。リクエストを送る前に、その呼び出しの上限額が予約されます。OpenRouterの場合は、リクエストにもこの上限が `provider.max_price` として付きます。値は、ルートの上限の入力・出力の単価です。コントロールや `SchemaProviderDefaults` で、すでにこれより低い `max_price` を設定している場合は、そちらが使われます。これにより、OpenRouterが呼び出しを、より高価なエンドポイントに回すことはなくなります。料金の計算では、会話がユーザーメッセージ1件だけのプローブ用リクエストを準備しますが、送信はしません。そのため、Pythonのプロバイダーの `prepare` から、そのプローブが見えます。上限を決められない呼び出しや、予約額が残りの予算に収まらない呼び出しは、何も送信されないまま拒否され、実行は `P_MONEY_*` のコードで失敗します。上限を決められない呼び出しとは、カタログが価格を付けていないモデル、ルートの上限がないルーターのエイリアス、出力の上限がないリクエスト、料金も使用の上限もないサーバーツールのことです。`ProgramRunBudget` の他のフィールドと同じく、`monetary_microunits = 0` は何も許可しません。この場合、すべての呼び出しが拒否されます。応答の後、`result.monetary_records` には、呼び出しごとに1件の `ng.ProgramMonetaryRecord` が入り、`evidence` が付きます。その種類は次の3つです。`reported`（OpenRouterの `usage.cost`）、`estimated`（カタログの単価。`catalog_version` と `catalog_digest` でカタログを、`pricing` で項目と価格の行を特定します）、`held`（結果または使用量が不明なため、予約額が課金されます）。`served_by` は、OpenRouter が呼び出しを処理したと示すアップストリームで、応答が名前を示さない場合は `None` です。プロバイダーが完了した HTTP 4xx 応答（408、425、429 を除く。たとえば不正なキーや不正なリクエスト）で拒否した呼び出しと、送信されなかった呼び出しは、予約を解放し、記録を残さず、金額もモデルトークンも課金しません。429、5xx の応答、途中で切れた呼び出しは `held` のままです。`result.usage.monetary_microunits` は、これらの合計です。予約額を超えて課金された呼び出し（たとえば upstream がリクエストの出力 cap より多い出力を返した場合）は、実行のハンドルにも報告されます。`host.start(...)` がハンドルを返し、`wait()` の後に `handle.notices` へ、そのような呼び出しごとに `P_MONEY_OVER_HOLD` notice が入ります。witness は `call_id`、`operation_id`、`node`、`model`、`evidence`（`reported` または `estimated`）、`reserved_microunits`、`microunits`、`over_by` で、呼び出しの記録に `served_by` があればそれが加わります。実行は変わりません。呼び出しは報告どおりに課金され、status、予算、`P_MONEY_BUDGET` は notice がないときと同じです。コード、単位、上限、精算の規則は、[価格の証拠とモデルカタログ](reference-en.md#pricing-evidence-and-the-model-catalogue)にあります。

モデルを呼び出すPythonノードは、プロバイダーを自分で持ちます。そのような呼び出しは、Programの予算には反映されません。

### 子Program、世代置換、およびグラフマイグレーション

`LocalProgramHost` は、静的な子のリンク、子合成ゲートウェイ、保持された `ng.checkpoint` での世代置換、およびライブルート Core グラフマイグレーションを、同期版と `_async` 版の両方で公開します。バインディングは C++ ランタイムへ転送し、独自のポリシーは追加しません。

| 操作 | Python シグネチャ | 結果 / 拒否コード |
|---|---|---|
| 静的な子のバインド | `host.bind_child(parent, binding_name, child, max_budget)` | `ModuleLinkReceipt`；未リンクの `ng.spawn` は `P_CHILD_BINDING` で失敗 |
| 子の開始・復旧 | `host.start_child(parent, link, version, input, budget, child_run_id=None, trace_id="python-program-child")` / `start_child_async(...)`、`host.recover_children(parent_run_id)` / `recover_children_async(...)` | `ProgramHandle` / `list[ProgramHandle]` |
| 合成バリデーターの登録 | `host.register_synthesis_validator(validate)`（`validate(proposal, bundle, reservation)` は `ProgramSynthesisSemanticDecision(validator_identity, contract_identity, accepted, evidence=None)` を返す） | 合成前に必須；登録済みのバリデーターがない場合はすべての合成が拒否される |
| 子の合成 | `ProgramSynthesisProposal(...)`、`ProgramChildSynthesisGrant(owner_scope, parent_run_id, parent_version, expected_lineage_head_id, reviewed_source, child_budget_ceiling, template_identity, semantic_validator_identity, semantic_contract_identity, ...)`、`host.prepare_child_synthesis(parent, handoff, proposal, grant, binding_name)` / `prepare_child_synthesis_async(...)`、`host.recover_child_synthesis(parent_run_id, proposal_id)` / `recover_child_synthesis_async(...)`、`host.reserve_synthesis(source, handoff, proposal, expected_lineage_head=None)` / `reserve_synthesis_async(...)`、`host.synthesize(proposal)` | `ProgramChildSynthesisRecord`（`Bound`、または `P_CHILD_SYNTHESIS_SEMANTICS` を伴う `Failed`）；権限超過は `P_CHILD_SYNTHESIS_AUTHORITY` |
| チェックポイントの保持 | `handle.next_handoff()` / `next_handoff_async()`、`handle.latest_handoff()` / `latest_handoff_async()` | `ProgramHandoff` リース；`.wait()` / `.wait_async()` は `ExactProgramHandoff` を返す |
| 世代の置換 | `host.replace(source, target, input, budget, replacement_run_id=None, trace_id="python-program-replace")` / `replace_async(...)` | 新世代の `ProgramHandle`；拒否コードは `P_REPLACEMENT_AUTHORITY`、`P_REPLACEMENT_BUDGET`、`P_REPLACEMENT_TARGET`、`P_REPLACEMENT_INPUT`、`P_REPLACEMENT_HANDOFF` |
| ライブ Core グラフマイグレーション | `host.migrate_graph(source, target, semantic_adapter=None, requested_run_id=None)` / `migrate_graph_async(...)` | 承認済み互換バージョンの新世代の `ProgramHandle`；非互換グラフは `P_GRAPH_MIGRATION_INCOMPATIBLE` |
| 系譜とレコードの参照 | `host.run_record(run_id)`、`host.run_lineage(logical_run_id)`、`host.run_generation(logical_run_id, generation)`、`host.child_syntheses(parent_run_id)` | `ProgramRunRecord`、`ProgramRunLineage`、`ProgramRunGeneration`、`list[ProgramChildSynthesisRecord]` |

`ProgramChildSynthesisGrant(owner_scope, parent_run_id, parent_version, expected_lineage_head_id, reviewed_source, child_budget_ceiling, template_identity, semantic_validator_identity, semantic_contract_identity, ...)` では、`template_identity`、`semantic_validator_identity`、`semantic_contract_identity` の 3 つが必須の `sha256` ID（`sha256:` に続く 64 文字の小文字 16 進数）であり、任意のキーワード引数で `allowed_capabilities`、`allowed_effects`、`lineage_id`、`parent_policy_fingerprint`、`minimum_execution_guarantee`、`max_source_bytes`、`max_sealed_modules` を設定します。`ProgramSynthesisSemanticDecision(validator_identity, contract_identity, accepted, evidence=None)` は最初の 3 つの引数（`validator_identity`、`contract_identity`、`accepted`）にデフォルト値を持ちません。`LocalProgramHost.register_synthesis_validator(validate)` は `ProgramSynthesisSemanticDecision` を返すコールバック `validate(proposal, bundle, reservation)` を登録します。登録済みのバリデーターがない場合、すべての合成は拒否されます。ゲートウェイは、決定が grant のバリデーターと契約（`semantic_validator_identity` と `semantic_contract_identity`）を指定している場合にのみそれを受け入れ、永続的な子合成（`prepare_child_synthesis`）の決定が別のバリデーターまたは契約を指定した場合は `P_CHILD_SYNTHESIS_SEMANTICS` で `Failed` となります。

```python
# The host's reviewed identities ("sha256:" + 64 lowercase hex), from its own registry.
TEMPLATE_ID, VALIDATOR_ID, CONTRACT_ID = reviewed_identities("child-worker")

def validate_synthesis(proposal, bundle, reservation):
    # The host's own check of the compiled child; here: it asks for no capability.
    accepted = not proposal.requested_capabilities
    return ng.ProgramSynthesisSemanticDecision(
        VALIDATOR_ID,
        CONTRACT_ID,
        accepted,
        evidence={"bundle_id": bundle.id, "checked": "no capabilities"},
    )

host.register_synthesis_validator(validate_synthesis)
```

置換対象の権限は元の権限の減衰範囲内でなければならず（`P_REPLACEMENT_AUTHORITY`）、`budget` は元の系譜の権威ある `remaining_budget` と一致し（`P_REPLACEMENT_BUDGET`）、対象は JavaScript 制御を持ち（`P_REPLACEMENT_TARGET`）、`input` は `previous_run_id` と `handoff` を束縛し（`P_REPLACEMENT_INPUT`）、ハンドオフ参照は有効なチェックポイント先頭と一致する必要があります（`P_REPLACEMENT_HANDOFF`）。置換が拒否された場合、元の実行は現在の世代にとどまり、保持中の `ProgramHandoff` リースも消費されません。既存の子は新世代へ引き継がれ、一致する `ng.spawn` バインディングで再結合されます。`migrate_graph` は、ライブルート Core グラフの実行をスーパーステップ境界で承認済みの互換バージョンの新世代へ移行し、`ProgramGraphMigrationTarget` の `MigrationPlan` と任意の `GraphSemanticMigrationAdapter` を適用します。ライフサイクルの詳細は[再帰的なエージェント](PROGRAM_RECURSIVE_HARNESSES.md)を参照してください。

### 予算

`LocalProgramHost` に渡す `budget_ceiling` が、admissionポリシーの上限になります。`max_child_depth` と `max_total_children` を除くすべてのフィールドは、正の値にする必要があります。モデルを一切呼び出さないProgramでも、`model_tokens`、`monetary_microunits`、`max_dynamic_compiles` が対象です。そうでない場合、コンストラクターは、0になっているフィールドの名前を挙げて `ValueError` を送出します。子に関する2つのフィールドは、両方とも0か、両方とも正の値にします。`LocalProgramHost(..., max_supported_child_depth=8)` は内部コンパイラーと admission プロファイルの両方の子深さ上限（`PolicySnapshot`、モジュール解決、ランタイムに引き継がれます）を設定し、`ProgramCompiler(..., max_supported_child_depth=8)` は単体のコンパイラーの上限を設定します（既定値は `ng.MAX_SUPPORTED_CHILD_DEPTH = 8` で、最大 `ng.MAX_REPRESENTABLE_CHILD_DEPTH` まで指定でき、`0` または `ng.MAX_REPRESENTABLE_CHILD_DEPTH` を超える値は `ValueError` を送出します）。コンパイラーと admission プロファイルはそれぞれ独自の上限を強制するため、両者が異なる場合は小さい方が優先され、`P_BUDGET_INVALID`、`P_START_BUDGET`、`P_CHILD_DEPTH` は有効な上限を `witness["hard_ceiling"]` に報告します。

`compile_admit(source, budget)` と `compile(source, budget)` は、`budget` を、すべてのフィールドの最小値と最大値の両方としてバンドルにシールします。シールされた予算が上限を超えるバンドルは、admissionで拒否されます。そのバージョンに対する `run`、`start`、`start_active` の呼び出しでは、毎回、フィールドごとに同じ値を渡す必要があります。予算が大きくても小さくても、`ng.ProgramDiagnosticError` が `P_START_BUDGET` で発生します。メッセージには、最初に食い違ったフィールドが挙げられます。たとえば `Program invocation budget wall_time_ms=5000 is outside its admitted bounds [10000, 10000] (policy ceiling 10000)` です。別の予算を使うには、コンパイルとadmissionをやり直します。結果は新しい `ProgramVersion` になります。実行時の予算では、`wall_time_ms`、`max_concurrency`、`max_program_operations`、`max_core_steps` を正の値にする必要があります。`model_tokens` と `monetary_microunits` は0でもかまいません。0は何も許可しません。`model_tokens = 0` のまま、モデルのトークンを使う実行は `BudgetExhausted` で終わり、`monetary_microunits = 0` の場合は、料金のかかるプロバイダー呼び出しがすべて、送信前に拒否されます（[モデルを呼び出すノード](#model-calling-nodes)を参照）。モデルトークン予算が実行を止めたときは、応答が予算を超えた場合も、次の呼び出しが送信前に拒否された場合も、`status` は `BudgetExhausted`、`failure.code` は `NG.BudgetExhausted` です。`failure.witness["model_tokens"]` には `granted`、`used`、`over_by` が入ります。ほかの予算も同じ形で実行を終わらせます。実行が許可量を超えたまま終わった場合も同じです。金額は `P_MONEY_BUDGET` と `failure.witness["monetary"]`、Coreのステップは `P_CORE_STEP_BUDGET` と `failure.witness["core_steps"]`、wall time は `TimedOut`、`P_RUNTIME_TIMEOUT`、`failure.witness["wall_time_ms"]` で終わります。複数の予算を超えた実行は witness にそのすべてを並べ、ステータスとコードは wall time、モデルトークン、金額、Coreのステップのうち最初のものに従います。[Program ガイドの予算](program-guide.md#budgets)を参照してください。

### 失敗のルーティング

`retryPolicy(node, {...})` に `on_failure` を指定すると、ノードの最終的な失敗が、実行を失敗させる代わりに、ハンドラーにルーティングされます。ルールは[Programグラフの機能](program-graphs.md#failure-routing)にあります。Pythonのノードは、例外の属性で失敗を表します。文字列の `code`（`app.compile_error` のような作成者のコード）、真偽値の `retryable`、文字列の `public_message` です。`str(exc)` が記録に入ることはありません。`ng.ProgramDiagnosticError`、`ng.ProgramCompileError`、`ng.ProgramAdmissionError` を送出したノードは、設定上の判定（`NG.ConfigError`）を報告します。これは、リトライもルーティングもされません。ハンドラーに `interruptBefore` がある場合、実行は `Interrupted` で止まり、`result.interrupt.pending_input.core_interrupt_value` に `{"reason": "failure_route", "failure": record}` が入ります。`host.resume(run_id, value)` で、ハンドラーから続行します。failure exitでは、宣言したコードで実行が `Failed` として終わります。failure edgeのないノードが失敗した場合は、分類されたコード（`NG.TaskFailed`、作成者のコードなど）で `Failed` として終わります。

`ProgramRunBudget.max_node_retries`（デフォルトは `None` で、上限なし）は、実行全体でのノードのリトライ回数を制限します。`result.usage.node_retries` がその回数、`result.remaining_budget.max_node_retries` が残りです。`compile_admit` はこれをシールしないので、再コンパイルしなくても、実行時に設定できます。`error_text` を設定するノード向けのredactor（伏せ字処理の関数）は、`LocalProgramHost(..., failure_text_redactor=fn)` と `GraphEngine.compile(..., failure_text_redactor=fn)` で渡します。`fn` は `{"node", "code", "kind", "attempts", "retryable", "public_message"}` を受け取り、残すテキストか `None` を返します。redactorがなければ、テキストは書き込まれません。`GraphEngine` では、failure exitの後、`result.status` が `ng.RunStatus.FailureExit` になります。`engine.resolved_retry_policy(node)` は、そのノードが実際に使うリトライのタイミングを返します。

`retryPolicy(node, {attempt_timeout_ms: ...})` は、ノードの1回の試行ごとに制限をかけます（100〜3 600 000 ms）。タイムアウトした試行では、`input.ctx.cancel_token.is_cancelled()` が真になり、`NG.Timeout` で失敗します。他の失敗と同じように、リトライもルーティングもできます。タイムアウトした後に試行が返した値は、捨てられます。Pythonのノード本体は強制的に中断できません。スリープしたりブロックしたりするノードは、トークンをポーリングしてください。次の試行が始まるのは、タイムアウトした試行が戻ってからです。停止の期限（再びタイムアウトの値。ただし1秒以上）までに戻らない試行があると、実行は `NG.EffectUncertain` で失敗します。そのような本体はバックグラウンドで動き続け、インタープリターの終了時には、バインディングがその完了を待ちます。実行の `wall_time_ms` を超えてしまうバックオフがある場合、リトライは kind `deadline` で打ち切られます。`GraphEngine` では、`engine.set_node_attempt_timeout_ms(node, ms)` でホスト側のタイムアウトを設定します（`None` で解除。定義が持つタイムアウトは、短くする方向にしか変えられません）。`engine.resolved_attempt_timeout_ms(node)` は、そのノードが実際に使うタイムアウトを返します。詳しくは[Programグラフの機能](program-graphs.md#attempt-timeouts-and-the-deadline)を参照してください。

### スケジューラースレッド、ワーカー、タイムアウト

`LocalProgramHost(..., scheduler_threads=1, worker_count=1)` には、スレッドに関する設定が2つあります。`scheduler_threads` は、実行を進める `ProgramRuntime` のスケジューラープールのスレッド数です。このプールが、JavaScriptの制御、`callCore` のディスパッチ、join、デッドラインを担当します。1以上にする必要があります。`worker_count` は、1つのグラフ内でfan-outするための、各Coreエンジンのワーカープールのサイズです。admission済みのProgramの `max_concurrency` を超えてはいけません。デフォルトの1では、fan-outの分岐はスケジューラーのスレッド上で実行されます。

Programの実行中、Pythonのノード本体がスケジューラーのスレッドを占有することはありません。ノード本体は、バインディングが持つワーカープールで動きます。デフォルトは32スレッドで、変更するには、最初のProgramの実行より前に、`NEOGRAPH_PYTHON_NODE_THREADS` に1〜1024の値を設定します。スケジューラーのスレッドがデフォルトの1つでも、ホストの複数の実行、`ng.all`・`ng.race`・`ng.quorum` のメンバー、1つのCoreの実行のfan-outの分岐は、同時に進みます。ノードの実行中にデッドラインが発火することもあります。`ng.race` は最初に終わったメンバーを返し、`ng.quorum` は必要な数の成功がそろった時点で返ります。ただし、PythonのコードはGILの下で動くので、CPUを使うノードが並列に動くことはありません。ノードどうしが重なって動くのは、`time.sleep`、I/O、ネイティブ呼び出しなどでGILを解放している間です。通常の `GraphEngine` の実行では、Pythonのノード本体は、これまでどおり呼び出し元のスレッドで動きます。

どちらの制限も、実行中のPythonノードを中断させることはありません。キャンセルは協調的に行われます。実行の `wall_time_ms` が尽きると、ランタイムは実行をキャンセルします。結果が返るのは、実行中のノードが戻った後で、`status` は `TimedOut`、`failure.code` は `P_RUNTIME_TIMEOUT`、メッセージは `Program wall-time budget (N ms) expired` です。`failure.witness["wall_time_ms"]` には `granted`、`used`、`over_by` が入り、タイムアウトによって中断された操作のテキストは `failure.witness["cause_message"]` に入っています。`ng.await(command, timeoutMs, sourceSite)` のデッドラインが過ぎると、実行は `TimedOut` と `P_AWAIT_TIMEOUT` で終わります。デッドラインの後に届いた完了は受け付けられません。どちらの場合も、子のCoreの実行はキャンセルされます。`input.ctx.cancel_token.is_cancelled()` を確認しているノードは早めに止まります。確認しないノードは、戻るまで結果が遅れます。`TimedOut` と `Cancelled` の結果では、完了したCoreノードが `execution_trace` に並びます。

Programのエンジンは、lintの結果もfan-outに関する通知も、標準エラー出力に出しません。`EngineConfig::worker_count` を勧める通知が出るのは、自分で構築した `GraphEngine` の場合だけです。

### メモリ上の状態と再開

デフォルトでは、`LocalProgramHost` はバンドル、バージョン、アクティベーション、実行の記録、チェックポイントを、ホストのオブジェクトの中のメモリに保持し、ディスクには何も書き込みません。実行を再開できるのは、その実行を開始したホストのインスタンスが生きている間に、同じインスタンスを通してだけです。プロセスを再起動した後や、別の `LocalProgramHost` からは、`resume()` が `ng.ProgramDiagnosticError` を `P_RUN_NOT_FOUND` で送出します。メッセージは `run '<id>' was not found in this LocalProgramHost; runs are kept in memory by the host that started them` です。PostgreSQLのProgramストアは、C++のホスト用のAPIです。

`sqlite_directory=path` を指定すると、ホストはバンドル、バージョン、実行の記録、チェックポイントを、そのディレクトリ内のSQLiteファイル（`program.db`、`checkpoints.db`）に保存し、プロバイダー呼び出しは、プロバイダー呼び出しのジャーナル（`provider_calls.db`）に保存します。これにより、実行はプロセスが終わっても残ります。同じディレクトリを同じレジストリで開いたホストが、その実行を続行できます。`resume()` は、割り込まれた実行を続行します。`reconnect(run_id)` は、停止したプロセスが実行中のまま残した実行を続行します。復旧のとき、ジャーナルに記録済みのリクエストが再送されることはありません。ジャーナルが応答をリプレイし、その呼び出しは1回だけ課金されます。プロバイダーを結び付けるホストには、`native_history_archive=` も必要です（初回は `ng.NativeArchive.provision`、以後は `ng.NativeArchive.open`）。ジャーナルは、すべての応答をarchiveの保管の下に置くためです。子の予算を持たず、`define()` だけで書いたProgramの実行は、実時間の間、実行リース（execution lease）を保持します。所有者がまだ生きているかもしれない実行を、別のランタイムが続行しないようにするためです。リースが切れるまでは、`reconnect()` が、その理由を説明するメッセージ付きで `P_RUN_LEASE_HELD` を送出します。切れた後は、実行を `TimedOut` で終わらせ、ジャーナルに記録済みの呼び出しを課金します。`main()` を持つProgramや、子の予算（`max_child_depth` と `max_total_children` がどちらも 0 より大きい）付きで始めたProgramはリースを持たず、`reconnect()` は最後のチェックポイントからすぐに続行します。クラッシュからの復旧は、実行の最後の永続チェックポイント（たとえば割り込みで残ったもの）から再開します。Core の呼び出しは、それぞれ止まった場所から続きます（#402）。Core のチェックポイントを保存していた `callCore`（typed plan では `call_core`）は最新のチェックポイントから続くので、完了していた呼び出しは結果をリプレイし、一時停止していた呼び出しは質問をもう一度出します。最初のチェックポイントの前に止まった呼び出しは、痕跡を残さず開始していた可能性があります。そのため `reconnect()` は、JavaScript でも typed plan でも、その呼び出し（`root.javascript.<n>`、または plan の操作）を指す `result.interrupt.pending_effect` を付けて、実行を `Interrupted` で返します（#404）。C++ のホストは `ProgramRuntime::reconcile()` で答えます。`Unknown` は実行を `AmbiguousEffect` のままにします。呼び出しの結果を添えた `Completed` は実行を続行し、その呼び出しが予約していた実時間と Core ステップは実行に戻ります（#403）。呼び出しが予約したトークンと金額は、何を使ったか分からないため、消費されたままです。

`resume(run_id, value)` はハンドルを返します。結果は `.wait()` で受け取ります。`pending_id` を指定しない場合は、その実行で待機中の呼び出しが1つだけならそれを再開します。待機中の呼び出しがない場合や、複数ある場合は、`ValueError` が発生します。その場合は、`result.interrupt.pending_input.call_id` を `pending_id` に渡して、呼び出しを正確に指定してください。`fork(run_id, value, version, budget)` は、その実行の正確なチェックポイント（または `checkpoint_id=`）から、`version` の新しい実行を開始し、保留中の呼び出しに `value` で答えます。`budget` は、コピー元の実行の `remaining_budget` から切り分けたフォークの取り分です。`wall_time_ms`、`model_tokens`、`monetary_microunits`、`max_program_operations`、`max_core_steps`、`max_dynamic_compiles` は量なので、フォークがコピー元に残っている量から取り、残りはコピー元が持ちます。`max_concurrency` は上限なので、フォークはコピー元の上限以下で自分の上限を持ち、コピー元は自分の上限をそのまま保ちます。コピー元の残りより大きいフォークは `P_FORK_BUDGET` を送出し、コピー元の `wall_time_ms` または `max_core_steps` を 0 にするフォークは `P_FORK_SOURCE_BUDGET` を送出します。どちらの場合も、コピー元は変わりません。`pending_id=` で呼び出しを指定する方法は、`resume()` と同じです。フォークは、`resume()` と同じように、一時停止した実行が終えていた処理をリプレイします。一時停止したステップで終了したノードや Send は、再実行も再課金もされず、そのプロバイダー呼び出しも再送されません（#390）。一時停止していたノードのように、フォークで再び実行されるノードや Send は、質問を出す前に行ったプロバイダー呼び出しを、コピー元の実行のジャーナルからリプレイします。その呼び出しはフォークに結び直されるので、再送も再課金もされません（#405）。この再生には永続的なプロバイダー呼び出しのジャーナルが必要で、`sqlite_directory` を指定したホストにはそれがあります。インメモリのホストにはないため、そこではフォークで再び実行されるノードが、質問を出す前に行ったプロバイダー呼び出しを再送し、再課金します。フォークはそのことを知らせます。`fork()` が返すハンドルの `notices` は、`ng.ProgramNotice` オブジェクト（`code`、`message`、`witness`）のリストです。一時停止した試行が、止まっているオペレーションでプロバイダー呼び出しを記録していた場合、`notices` にはコードが `P_FORK_PROVIDER_CALLS_NOT_JOURNALED` の通知が1つ入ります。`witness` には、`source_run_id`、`source_attempt`、`operation_id`、`calls`（各項目に `call_id`、`node`、`model`、`microunits`）と、それらの呼び出しの合計 `microunits` が入ります。通知はエラーではなく、フォークが送るものも課金するものも変えません。列挙された呼び出しのうちどれをフォークが再実行するかは、通知からは分かりません。一時停止したステップで終わったノードの呼び出しは、そのステップに保存された書き込みからリプレイされ、再送されないからです（#390）。ジャーナルのあるホストでのフォークや、一時停止した試行が止まっているオペレーションで呼び出しを記録していないフォークでは、`notices` は空のリストです。このリストは `fork()` が返したハンドルに属し、実行とともには保存されません。そのため、`reconnect()` が返すものを含め、ほかのハンドルのリストは空です。

Coreのステップが複数の割り込みで一時停止している場合、`result.interrupt.pending_input.core_interrupt_id`（または `pending_effect.core_interrupt_id`）が、この保留中の呼び出しが答えるCoreの割り込みを示します。`resume()` は、まさにその割り込みに答えます。ステップの他の割り込みは、保留中の呼び出しを1つずつ、あらためて質問します。Coreの割り込みが1つだけのときは、`""` になります（#388）。`fork()` も、同じ割り込みに答え、フォークした実行は、ステップの他の質問を再び出します（#394）。JavaScriptの `main()` をフォークした場合は、`resume()` と同じように、コピー元が一時停止したコマンドから続行します。`main()` は、コピー元の実行の入力でもう一度実行されます（フォーク側の `input` は使われません）。終了済みのコマンドは、コピー元のコマンドジャーナルからリプレイされ、実行も送信も課金も繰り返されません。一時停止していた `callCore` は、そのチェックポイントから `value` で再開されます（#395）。フォークがリプレイする記録済みの失敗は、プロバイダーのエビデンスを保持します。フォークは、そのエビデンスを、コピー元の実行の保管から自分の保管に結び付け直します。そのため、`result.failure.provider_outcome` は、コピー元と同じようにフォークでも復元されます（#398）。

`sqlite_directory` があると、resumeやフォークは、SchemaProviderのdescriptor policyが変わった後でも、ジャーナルに記録されたプロバイダー呼び出しを使います（#543）。ジャーナルは、呼び出しを、リクエストが求める内容で識別し、それを承認したpolicyでは識別せず、`wait()` の後に `handle.notices` で `P_PROVIDER_CALL_POLICY_CHANGED` を見られます。ソースの呼び出しを見つけても、0.15が書いたジャーナルのように照合できないフォークは、呼び出しを再送して再課金する代わりに `NG.ConfigError` で失敗します。`fork_may_resend_provider_calls=True` で開いたホストは、代わりにその呼び出しを送り、プロバイダーが再び課金することがあり、`handle.notices` に `P_FORK_PROVIDER_CALL_RESENT` が付きます。既定値は `False` です。

`resume()` が返す結果の `execution_trace` には、再開後に実行されたCoreノードだけが並びます。割り込みの前に実行されたノードは、割り込まれた結果に出ています。たとえば、Coreが `a → b → c` で、`b` の前で割り込まれた場合、割り込まれた結果は `["a"]`、再開後の結果は `["b", "c"]` になります。

## 必須のライフサイクルHook

Hookは、ホストのライフサイクルイベントによって起動されます。モデルがツールの呼び出しを決めて起動されるものではありません。

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

`FailClosed` では、コールバックが失敗すると、保護されたランタイム境界がブロックされます。`Continue` を使えるのは、観測データが失われてもかまわない場合だけです。

`HookBoundaryBlocked` は、ブロックされたHookの `invocation_id` と `state`（`"failed"`、`"timed_out"` など）を保持します。Pythonのツールは、[ツールの失敗に関する契約](reference-en.md#tool-failure-contract)に従います。ツールの中で（たとえば入れ子のエンジン実行によって）`HookBoundaryBlocked` が送出されると、実行が止まり、`NodeInterrupt` が送出されると、実行が一時停止します。それ以外の例外は、モデルが読むツールの結果になります。その結果に入るのは、`ValueError: bad input` のような例外の型とメッセージを `ToolExecutionPolicy.error_limit_bytes`（既定 2048 バイト）で切ったもので、トレースバックはプロバイダーへ送られず、`neograph_engine.tools` ロガーへ WARNING で記録されます。

## ランタイムコンテキスト、Skill、strictディスパッチ

このバインディングは、変更できないRAWの履歴レコード、コンテキストのアーティファクト、エポック、必須のSkill／制約、変換レシート、プロバイダーのディスパッチレシートを公開しています。

RAWの `RuntimeHistoryRecord` を作るときは、アシスタントのメッセージなら `RuntimeHistoryRecordData.trust` に `RuntimeTrustClass.ModelOutput` を設定します。デフォルトの `UntrustedInput` が受け付けるのはユーザーのメッセージだけで、アシスタントの出力に使うと拒否されます。これらのラベルは、メッセージの由来を表すものです。ツール実行、予算、コードの権限を与えるものではなく、本物のネイティブリプレイの保管の代わりにもなりません。

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

`ContextTransformReceipt` は、任意の派生エビデンスを許しますが、必須のアーティファクトはすべてバイト単位で同一のままであることを要求します。

strictの経路をすべて使うには、永続化されるSQLiteのストアを使います。

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

`max_input_tokens` は、トークナイザーで数えた値ではなく、エンコードしたリクエストのヒューリスティックな見積もり（バイト数を3で割った値）と比べます。[コンテキストの予算](STRICT_RUNTIME_INTERPOSITION.md#context-budget)を参照してください。

プロファイルは、C++と同じくホストがステップを進めます。ホストは、プロバイダーを呼び出すたびに、RAWレコードを追加し、新しいエポックをアクティブにします。エポックの最初のディスパッチ以降に追加されたアシスタントまたはツールのメッセージを提示する呼び出しは `RuntimeError` を送出し、メッセージがこの契約を明示します。`profile.attach(engine)` の後で、`engine.run()` 1回の中でツールループが回ると、まさにこれが起きます。[ホストがステップを進める契約](STRICT_RUNTIME_INTERPOSITION.md#host-stepped-contract)を参照してください。

## HITLと状態

静的な `interrupt_before` / `interrupt_after`、動的な `NodeInterrupt`、同期の `resume`、asyncioの `resume_async`、正確な指定の `resume_from` と `resume_from_async` には、チェックポイントストアが必要です。

`ng.Send` でディスパッチされたPythonノードが `ng.NodeInterrupt` を送出すると、他のノードと同じように実行が一時停止します。`run`、`run_stream`、`run_async`、`run_stream_async` は、チェックポイント付きで中断した `RunResult` を返します。`resume`、`resume_async`、`resume_from`、`resume_from_async` は、答えを渡してその Send を実行します。そのとき、Send を出力したノードと、終了済みの Send は、再実行されずにリプレイされます。1つのステップで複数のノードや Send が `ng.NodeInterrupt` を送出すると、`result.interrupts` に、それぞれが `{"id", "node", "value"}` として並び、`result.interrupt_id` は最初のものを示します。`resume`、`resume_async`、`resume_from`、`resume_from_async` に `interrupt_id=` を渡すと、その割り込みに答えます。渡さなければ最初の割り込みに答えます。その再開では、一時停止している他のノードや Send が、それぞれの質問をあらためて出し、次の再開でそれに答えます（#388）。保留中でない `interrupt_id` を渡すと、`ValueError` が発生します。`RunMetadata.resume_interrupt_id` も同じIDを保持します。

答えが届くのは、再開が再び入ったステップの中で、その答えを受けるノードや Send だけです。再開後の実行の、これより後のステップでは、`input.ctx.resume_value` と、ツールゲートの `ToolGateContext.resume_value` が `None` になります。そこで質問するノードやゲートは、実行をもう一度一時停止させます（#393）。`interrupt_before` の一時停止の後に再開すると、ゲートが止めていたノードに値が渡されます。`interrupt_after` の一時停止の後に再開すると、どのノードにも渡されません。1つのステップでゲート付きのノードが複数実行可能になった場合は、ゲートが1つずつ答えられます。ステップのすべてのゲートに答えるまで、実行は次のゲート付きノードの前で再び一時停止し、すべてに答えた後で、各ゲート付きノードが自分のゲートへの答えを受け取ります（#396）。1つのステップの中では、失敗したノードや Send が、質問を出したものより優先されます。順序は関係ありません。実行は失敗を送出し、割り込みは報告されません。失敗と並んで一時停止するのは、不確かな効果（payloadの `kind` が `tool_effect_reconciliation` の `NodeInterrupt`）だけで、照合できるようにするためです（#392）。

`input.ctx.resume_interrupt_id` は、`input.ctx.resume_value` がどの割り込みに答えているかをノードに伝えます（#406）。値がノード自身の `NodeInterrupt` への答えであれば、その割り込みの id です（`result.interrupt_id` が報告する `"s3:review"` のような値）。再開が `interrupt_id=` で指定した場合も、最初の割り込みに答えた場合も同じです。値がノードを止めていた `interrupt_before` ゲートの承認であれば `None` です。ゲートの後ろにあって自分の質問も出すノードは、`resume_interrupt_id` が `None` の間は `ng.NodeInterrupt` を送出します。そうすれば、実行の承認を自分の質問への答えとして受け取らずに済みます。このパターンは[コンセプトガイド](concepts.md)にあります。

Pythonで `CheckpointStore` のサブクラスを作る場合は、`requires_managed_budget(thread_id) -> bool` を実装できます。この同期の仮想メソッドは、永続化された管理対象バンクの拒否義務（denial obligation）を読み取ります。エンジンのネイティブな非同期ファサードがこのメソッドを呼び、バインディングは、Pythonのオーバーライドを呼ぶためにGILを取得します。チェックポイントの状態が除去または削除されていても、永続化されている義務を、そのまま正直に報告してください。オーバーライドを実装しなかった場合は、`False` を返すのではなく、ネイティブ側の「バックエンド未対応」という明示的なエラーになります。

このリーダーは、支出、復元、リースのいずれの権限も与えません。管理対象の上限付き実行には、これまでどおり、実際に対応しているネイティブの管理対象予算のリースが必要です。リーダーを実装しただけでは、それは得られません。

```python
if result.interrupted:
    result = engine.resume(result_thread_id, {"approved": True})
```

承認のあとに続くターンも、最初のターンと同じようにストリーミングできます。`resume`、`resume_from`、`resume_async`、`resume_from_async` は `callback=` を受け取り、`run_stream` が実行に対して行うのと同じように、再開したターンの `GraphEvent` を受け取ります。同期の呼び出しでは呼び出しを駆動するスレッドで、asyncioの呼び出しではループスレッドで受け取ります。コールバックが例外を送出しても、実行は止まりません。同期の呼び出しでは例外が `sys.unraisablehook` に渡され、デフォルトではstderrに出力されます。asyncioの呼び出しでは例外がループ上で送出され、ループの例外ハンドラーが報告します。`callback` は4つのメソッドすべてでキーワード専用で、`resume` と `resume_async` の `metadata` も同様です。

```python
async def approve(engine, thread_id, answer, on_event):
    # The resumed turn streams its tokens and carries a deadline of its own.
    metadata = ng.RunMetadata(timeout_ms=30_000, trace_id="trace-42")
    return await engine.resume_async(
        thread_id, answer, callback=on_event, metadata=metadata
    )
```

状態の確認とタイムトラベルには、`get_state_history`、`update_state`、`fork` を使います。`get_state_view()` は、Pydanticを使ったフラットなチャネルアクセスを提供します。`get_state()` は、正規の入れ子の表現を返します。

## 非同期とキャンセル

`run_async`、`run_stream_async`、`resume_async`、`resume_from_async` は、`asyncio.Future` オブジェクトを返します。これらのグラフのFutureをキャンセルすると、`CancelToken` を通じてキャンセルが要求されます。再開した実行も同じです。呼び出しが `cancel_token` を設定した `RunConfig` を受け取る場合、Futureをキャンセルするとそのトークンがキャンセルされます。操作が終わるのは、ネイティブのI/Oがキャンセル可能な境界に達してからです。グラフのストリーミングのコールバックは、呼び出し側のasyncioのループスレッドに戻ってきます。インタープリターが終了するとき、モジュールがインポート時に登録した `atexit` フックが、ファイナライズが始まる前に、これらのFutureを支えるスレッドと、Pythonのノードやツールの本体を実行するプールを止めます。実行中のPythonコードは終わるまで待ち、まだ待機中の実行は破棄します。その後の `run_async`、`run_stream_async`、`resume_async`、`resume_from_async` の呼び出しは `RuntimeError` を送出します。

プロバイダーの `invoke` と `dispatch` は、同期のPythonメソッドです。バインディングは、ネイティブの呼び出しが動いている間はGILを解放し、Pythonのプロバイダーのオーバーライドやイベントコールバックを呼ぶときにGILを取得します。イベントコールバックは、所有権を持つ `ProviderEvent` の値を受け取り、ネイティブのワーカースレッド上で動くことがあります。コールバックからasyncioが管理する状態を更新する必要がある場合は、`loop.call_soon_threadsafe` を使ってください。

オブザーバーは `request.on_event = callback` で設定し、`None` を代入すると解除します。`request.on_event` を読むと、元のPythonのcallableと同一のオブジェクトが返ります。`RunConfig.on_provider_event` も同じ規則です。各イベントには、文字列の `kind` と、型付きの `value` があります。`ProviderPartDelta` の値は、自分の `bytes` を所有しています。コールバックが終わった後にイベントを保持しても、借用しているSDKのテキストビューを保持することにはなりません。キャンセルを要求するには、`request.cancel_token` に設定したトークンの `token.cancel()` を呼びます。

同期のプロバイダー呼び出しをイベントループのスレッドから外すには、`asyncio.to_thread(provider.invoke, request)` を使います。そのawaitをキャンセルしただけでは、プロバイダーの呼び出しはキャンセルされません。`request.cancel_token` に `CancelToken` を設定し、キャンセルを明示的に要求してください。

有効な準備済みハンドルで、そのトークンがすでにキャンセル済みの場合、プロバイダーのディスパッチの事前チェックは、所有権を持つSDKの `Cancelled` の失敗を返します。同期のディスパッチも、同じネイティブの非同期プロバイダーの経路を最後まで実行します。グラフ、ホスト、コルーチンの入口でのキャンセルは別の境界で、`ProviderOutcome` が返る前に例外が発生することがあります。キャンセルを要求しても、すべての境界で型付きの `Cancelled` のデータが得られるとは限りません。また、キャンセルを要求したからといって、リクエストが送信されていないこと、リモートの処理が止まったこと、課金が発生しないことが保証されるわけではありません。

GILを解放することと、Pythonのコールバックのために再取得することが、バインディングの別々の責務である理由は、[pybind11のGILに関するドキュメント](https://pybind11.readthedocs.io/en/stable/advanced/misc.html#global-interpreter-lock-gil)で説明されています。

## プロトコルと可観測性

- MCPクライアントのツールは、ビルドされている場合、`neograph_engine.mcp` から使えます。
- A2Aクライアントの型は、ビルドされている場合、`neograph_engine.a2a` から使えます。
- `ProtocolHostAdapter` は、公式のPython向けA2A／ACPサーバーSDKを、NeoGraphのセッションの意味論に統合し、グラフのインタラプトも扱います([A2A と ACP の SDK の背後でエージェントをホストする](#hosting-an-agent-behind-the-a2a-and-acp-sdks)を参照)。
- `neograph_engine.tracing` と `neograph_engine.openinference` は、ベンダー中立なOTel／OpenInferenceのデータを出力します。送信先は、Phoenix、Langfuse、Arizeと、それらと互換性のあるバックエンドです。

<a id="hosting-an-agent-behind-the-a2a-and-acp-sdks"></a>
### A2A と ACP の SDK の背後でエージェントをホストする

Python パッケージには A2A や ACP のサーバーがありません。トランスポートとワイヤー形式は公式 SDK が担い、`ProtocolHostAdapter` がそのコールバックのためにエンジンを動かします。A2A のコンテキスト ID や ACP のセッション ID が NeoGraph の `thread_id` になり、チェックポイントが会話を引き継ぎ、`cancel(request_id)` は内側の `run_async()` をキャンセルし、`stream()` はトークンイベントに続けて最終イベントを 1 つ返します。[サンプル 27 と 28](../bindings/python/examples/README.md) は、同じデモグラフをそれぞれの SDK でホストします。

人を待って停止したグラフ(`interrupt_before`、`NodeInterrupt`、ツールゲート)はプロトコルのクライアントに報告され、クライアントの答えが、実行が停止したチェックポイントを再開します。

| アダプターの呼び出し | 動作 |
|---|---|
| `stream(payload, thread_id=..., interrupts=True)` | `final` イベントの代わりに `interrupt` イベント 1 つで終わります。`event.interrupt` は `ProtocolInterrupt` で、`reason`、`payload`、`node`、`checkpoint_id`、`interrupt_id` を持ちます。 |
| `resume_stream(answer, thread_id=..., interrupt=None)` | `resume_value(answer, interrupt)` で再開します。`token` イベントを返し、続けて `final` か、別の `interrupt` で終わります。 |
| `resume_payload(answer, thread_id=...)` | ストリーミングなしで同じことをし、応答テキストを返します。 |
| `pending_interrupt(thread_id)`、`discard_interrupt(thread_id)` | アダプターがスレッドについて覚えている停止と、それを忘れること(キャンセルされたタスク、キャンセルされた権限リクエスト)。 |

`ProtocolHostAdapter(..., resume_value=callable)` は、プロトコルの答えを、停止したノードが `input.ctx.resume_value` から読む値に変換します。既定は答えそのものです。A2A の後続メッセージのテキスト、または ACP の権限オプションの ID(`"approve"` か `"reject"`)で、ネイティブサーバーも同じです。`interrupts=True` なしでグラフが停止すると、`stream()`、`run()`、`run_payload()` は `ProtocolInterrupted` を送出します。以前は、状態の最後のメッセージを答えのように返していました。停止はスレッドごとにメモリに覚えられ、それを再開するにはエンジンにチェックポイントストアが必要です。再起動のあとは、保存しておいた ID から作った `ProtocolInterrupt` を `interrupt=` に渡してください(`checkpoint_id` があれば、そのチェックポイントを正確に再開します)。記録された停止も `interrupt=` もない場合、`resume_stream()` と `resume_payload()` は `LookupError` を送出します。停止していないスレッドを再開すると、最後のターンのテキストが返ってしまうためです。

A2A の実行器は停止を `input-required` タスクとして示し、そのタスクへの次のメッセージで再開します。ACP のエージェントは同じターンの中で `session/request_permission` を送ります。それぞれの対応は [A2A](a2a.md#interrupts-when-you-host-from-python) と [ACP](acp.md#permission-requests-when-you-host-from-python) にあります。ネイティブサーバーの NeoGraph コラボレーションエンベロープ、Program ベースの A2A タスク、テナント分離は、この経路には含まれません。

### ネイティブA2Aのdiscoveryとストリーミング

`a2a.WireDialect` は、`V1_0`（主）か `V0_3`（互換用）です。`client.wire_dialect()` は、クライアントの構築後、またはカードを強制的に取得した後、カードで選ばれたRPCか、プローブの成功があるまでは `None` を返します。`AgentCard.supported_interfaces` には、独立した `AgentInterface` レコードが入り、`url`、`protocol_binding`、`protocol_version`、`tenant` を持ちます。`card.raw` は、観測用のJSONです。ネイティブのクライアントは、最初の呼び出しの前にカードを取得し、互換性のあるJSONRPCインターフェースを選びます。このとき、正規化したベースURLのものを優先します。RPCの送信先は、そのインターフェースのURLがベースURLと同じoriginであれば、そのURLです。カードが別のoriginを指すことを許すには `client.set_allow_cross_origin_interfaces(True)` を使い、実際に使われているエンドポイントは `client.endpoint_url()` で確認できます。クライアントがプローブを行うのは、カードを取得できない場合だけです。その場合は1.0を先に試し、`-32601`、`-32009`、またはHTTP 400が返ったときに0.3へフォールバックします。カードで選ばれた呼び出しは、フォールバックしません。メッセージIDはランダムなUUIDです。`a2a.A2ARpcError` は、実際の整数の `.code` を保持します。

```python
from neograph_engine import a2a
from uuid import uuid4

client = a2a.A2AClient("http://127.0.0.1:8080")
card = client.fetch_agent_card()
for interface in card.supported_interfaces:
    print(interface.protocol_version, interface.tenant)

params = a2a.MessageSendParams()
params.message.message_id = str(uuid4())
params.message.role = a2a.Role.User
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

このサンプルを実行する前に、そのエンドポイントで実際のローカルA2Aサーバーを起動してください。Chatのループバックのピアとは別のものです。`send_message(params)` は、ストリーミングしないマルチパート版のオーバーロードです。テキスト用の簡易オーバーロードも残っています。認証情報は `client.set_authorization_header(authorization_header)` で明示的に設定し、その値をログに出さないでください。`Part.media_type`、`file`、`data`、`metadata`、メッセージの拡張／参照ID、タスクのアーティファクトは、型付きのデータとして観測した内容を保持します。プロバイダーのネイティブな権限を持つことはありません。

`params.message` と `Task.status` は、元のオブジェクトと直接つながったインラインのレコードです。省略可能な設定、ベクトル、イベントの子要素は、切り離されたスナップショットなので、入力を更新するには代入し直す必要があります。`StreamEvent.type` には `StreamEvent.Type.StatusUpdate/ArtifactUpdate/Task` を使います。`status_update`、`artifact_update`、`task`、`is_final()` を確認してください。V1の最初のタスクは、最終ではありません。ネイティブのSSEは、ステータスと、追記または置換のアーティファクト更新を組み立てます。ブロックする呼び出しはGILを解放します。コールバックはGILを取得し、GILに対して安全なオーナーを保持します。コールバックで配信したからといって、出力を見たうえでの再ディスパッチが許されるわけではありません。コールバックのスレッドがasyncioのループであるとは限りません。必要な場合は `loop.call_soon_threadsafe` を使ってください。


`OpenInferenceProvider` は `neograph_engine.openinference` からインポートします。コンストラクターは `OpenInferenceProvider(inner: Provider, tracer, *, span_name="llm.complete")` です。このファサードは、ネイティブのC++ラッパーに処理を委譲します。ラッパーは、型付きの `prepare(request)`、`dispatch(prepared)`、`invoke(request)` を継承します。デフォルトのspan名はラベルであり、`complete` メソッドが復活したわけではありません。`opentelemetry-api` と `opentelemetry-sdk` をインストールしてください。構築にはOTelのAPIが必要で、エクスポートには、設定済みのSDKのspanプロセッサーとエクスポーターが必要です。

準備、失敗したadmission、破棄では、LLMスパンは作られません。admission済みのディスパッチは、スパンを1つ開始し、完了、SDKの失敗、ディスパッチの例外のいずれかで終了させます。準備済みのオペレーションは、ラッパーや外部のトレーサーへの参照が回収された後も、Pythonのトレーサーアダプターを保持します。ブロックする `invoke` / `dispatch` はGILを解放します。OTelの呼び出しと、Pythonの参照の破棄では、GILを取得します。トレーサーの失敗は、ネイティブ側のベストエフォートの方針に従い、所有権を持つ結果、元のイベント、プロダクト側の例外を置き換えることはありません。

アダプターは、ディスパッチの時点でPythonのトレーサーの `start_span` を呼び出します。呼び出し元のコンテキストが、ネイティブのワーカースレッドに自動で引き継がれることはありません。意図する親のOTelコンテキストをディスパッチの前に取得し、下のサンプルのように、トレーサーアダプターを通して明示的に渡してください。グラフ／ノードを親にしたい場合は、グラフノード内のプロバイダーでも同じようにします。OTelのドキュメントに、[親コンテキストを明示的に選ぶ方法](https://opentelemetry-python.readthedocs.io/en/latest/api/trace.html#opentelemetry.trace.Tracer.start_span)があります。

サンプルの `ParentContextTracer` は、取得した親コンテキストを1つだけ保持します。論理的に別の親を使う場合は、新しくコンテキストを取得して、アダプターとプロバイダーのラッパーを作り直してください。古いアダプターを使い回すと、以前の親のままになります。

上のプロバイダーのサンプルで作った `provider` と `controls` を使います。この断片を実行する前に、1リクエスト用のピアを再起動してください。

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

コンソールエクスポーターには、`openinference.span.kind="LLM"` を持つ `llm.request` が表示され、その `parent_id` は `request` スパンの `span_id` と一致するはずです。リンク先のピアを使うと、表示される結果は、消費済みで、失敗がなく、`Hello.` を含みます。新しいリクエストには、`observed.invoke(request)` で同じ準備とディスパッチをまとめて行えます。消費済みのハンドルを、もう一度ディスパッチしてはいけません。

LLMスパンのフィールドには、admission済みのモデル、宣言したtemperatureと出力の上限、公開されているメッセージのロールと、見えるテキスト（`Text`）の内容、既知の入力・出力・合計の使用量が入ります。カウントが欠けている場合は、対応する属性を省きます。報告されたゼロは、ゼロのままです。ストリーミングのリクエストでは、見えるテキストのデルタに対してだけ `llm.token` イベントが追加され、テキストは `attributes["chunk"]` に入ります。SDKの失敗では、安全なメッセージを使ってERRORステータスを設定し、公開できる部分的な出力を保持します。正常に完了したときはOKを設定します。`request.on_event` は、これまでどおり元の型付きイベントを受け取ります。

ネイティブリプレイ、reasoningのパート、rawエンベロープ、rawイベントは、これらのLLMスパンのフィールドには入りません。結果は、これまでどおりネイティブのメッセージとエビデンスを保持します。トレースのエクスポートが、リプレイの保管権限や会計の権限を与えることはありません。公開されるテキストには、アプリケーションの機微なデータが含まれることがあります。プロンプトとエクスポーターは、それを踏まえて選んでください。`openinference_tracer(tracer, *, root_name="graph.run", node_span_prefix="node.", on_event=None)` は、グラフイベント用の別のコンテキストマネージャーとして、引き続き使えます。プロバイダーのLLMスパンの代わりにはなりません。

## オプションのコンポーネント

公開パッケージは、オプションのC++コンポーネントがあるかどうかを、正直に示します。

- `_HAVE_PROGRAM`、`_HAVE_SQLITE`、`_HAVE_POSTGRES`、`_HAVE_MCP`、`_HAVE_A2A`
- 存在しないコンポーネントは、Pythonでエミュレートされず、単に存在しません
- PyPIのwheelでは、Program/QuickJS、LLM、MCP、SQLiteが有効です。ソースビルドでは、CMakeのオプションに従います

## テストとサンプル

バインディングのテストスイートは、Coreの実行、カスタムコールバック、asyncio、キャンセル、Programのコンパイル／ランタイム、必須のHook、strictなコンテキスト、SQLiteの永続化、プロトコル、READMEのサンプルを対象にしています。

- [Pythonのサンプル](../bindings/python/examples/README.md)
- [C++のサンプル](../examples/README.md)
- [Program ガイド](program-guide.md)
- [Strictランタイムインターポジション](STRICT_RUNTIME_INTERPOSITION.md)
