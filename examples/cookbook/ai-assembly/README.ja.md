<!-- neograph-i18n: source=examples/cookbook/ai-assembly/README.md locale=ja source_sha256=4922ec93b98cf57b8a7fc967e471974122e6b7608a53fc5f1b826cb01f3fd9b8 -->
# AI国民議会

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**NeoGraphの新規ユーザーとして**構築されたおもちゃのデモ — すべてのAPI選択は、NeoGraphのソースを開くことなく公開ドキュメント（README、githubの例、Doxygen）を読むことで行われました。目的は2つあります：A2Aが実際のマルチペルソナシナリオで機能することを証明すること、そして、まったく新しいC++開発者が途中で直面する摩擦を浮き彫りにすることです。

## 動作

国民議会の4人の議員が別々のポートに座り、それぞれが個別のペルソナプロンプトと、固定されたDeepSeekモデル用の同じOpenRouterルートに支えられたA2Aエンドポイントです。議長（国民議会議長）は独立したプログラムであり、NeoGraphの`A2AClient`を介して法案をすべての議員に並行してブロードキャストし、各議員の返信から投票を解析し、結果を宣言します。

```
                          ┌──────────────────┐
                          │  Speaker         │
                          │   A2AClient ×4   │
                          └─────────┬────────┘
                fetch_agent_card +    send_message_sync
            ┌──────────┬───────────┴───────────┬──────────┐
            ▼          ▼                       ▼          ▼
       :8101 Progress    :8102 Conservative  :8103 Center  :8104 Green
       Kim Jinbo         Park Bosu           Jung Jungdo   Na Noksaek
       (PersonaNode → OpenRouter DeepSeek, persona-specific system prompt)
```

各メンバーは、`__start__ → persona → __end__`の背後で提供される1ノードのNeoGraph（`a2a::A2AServer`）です。このグラフは`prompt`チャネルを読み取り、`response`チャネルに書き込みます。A2Aサーバーのデフォルトの`GraphAgentAdapter`は、これらをJSON-RPC経由で公開します。

## ライブ議事録（OpenRouter経由のDeepSeek、2026-04-29）

ビル：[`bills/basic_income.txt`](bills/basic_income.txt) — ユニバーサルベーシックインカム、月50万ウォン、土地・炭素・累進課税で資金調達。

```
[Speaker of the National Assembly] Bill submission: [National Basic Income Law]

[Progress Kim Jinbo]   Protecting socially vulnerable groups + asset/carbon taxation = alignment        → Support
[Conservative Park Bosu]   200 trillion mandatory spending + market distortion + real estate shock    → Oppose
[Center Jung Jungdo]   Acknowledging intent but excessive amount; suggests phased reduction amendment  → Oppose
[Green Na Noksaek]   Carbon tax + unearned income taxation + equitable distribution                    → Support

[Speaker of the National Assembly] Vote result:  2 in favor  /  2 opposed  /  0 abstention
[Speaker of the National Assembly] Tie vote — the bill is rejected (custom).
```

各ペルソナの推論は、自分の政党の表明された価値観を真に追跡します。それはフレームワークの仕業ではなく、ピン留めされたモデルがそれぞれ異なるシステムプロンプトに従うためです。ただし、アセンブリのメカニクス（並列A2A、投票集計、発見）は純粋なNeoGraphです。

## ビルド＋実行（NeoGraphツリー内で）

```bash
# from NeoGraph repo root; A2A and LLM are optional build components
export SCHEMAPROVIDER_PREFIX="/absolute/path/to/installed/schemaprovider"
cmake -S . -B build-cookbook \
    -DCMAKE_PREFIX_PATH="$SCHEMAPROVIDER_PREFIX" \
    -DNEOGRAPH_BUILD_EXAMPLES=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_A2A=ON \
    -DNEOGRAPH_BUILD_LLM=ON
cmake --build build-cookbook --target \
    cookbook_ai_assembly_member cookbook_ai_assembly_speaker -j4

# オフラインfixture：.env読み込み/provider通信なし
NEOGRAPH_BUILD_DIR="$PWD/build-cookbook" \
  bash examples/cookbook/ai-assembly/scripts/run_session.sh --mock
# live：環境または.envで鍵を非公開設定
NEOGRAPH_BUILD_DIR="$PWD/build-cookbook" \
  bash examples/cookbook/ai-assembly/scripts/run_session.sh
```

typed SchemaProvider CMakeパッケージとビルド依存を先にインストールします。統合NeoGraphターゲットでありstandaloneプロジェクトはありません。`NEOGRAPH_BUILD_DIR`でバイナリを選び、未設定なら`build-pybind`、`build`、recipeの`build`を順に探します。`--mock`は合成棄権でモデル判断/使用量ではありません。liveは法案/ペルソナのプロンプトをOpenRouterへ送り、有効な鍵、ネットワーク、クレジットが必要です。4人の呼び出しは有料で固定料金を保証しません。鍵、`.env`、プロンプト、出力記録を非公開にし、raw native記録を公開しないでください。A2Aにはportable応答だけを渡します。C++はtyped `ProviderRequest`/SDKイベントと完全な不変`sp::Outcome`を使い、projectionはnative replay権限ではありません。ソース移行の記録で、新しい実行検証ではありません。

## Pythonスピーカーバリアント（v0.2.1+、クロス言語A2A）

Pythonバインディングはこの切り替えでは**deferred（延期）**です。`speaker.py`には別途互換`neograph_engine.a2a`が必要で、C++移行完了は保証しません。以下は歴史的使用例です。

```bash
pip install 'neograph-engine>=0.2.1'
# (start the C++ members in another terminal as above)
PYTHONPATH=build-cookbook python3 examples/cookbook/ai-assembly/speaker.py \
    examples/cookbook/ai-assembly/bills/basic_income.txt \
    http://127.0.0.1:8101 http://127.0.0.1:8102 \
    http://127.0.0.1:8103 http://127.0.0.1:8104
```

v0.2.1バインディングは歴史的リリース結果で現在の検証ではありません。A2A wire client/protocolは変更されていません。

## 摩擦ジャーナル — 新しい NeoGraph ユーザーがつまずいた点


切り替え前の歴史的な摩擦記録で、現在のlegacy API提供の主張ではありません。上のlive記録も歴史的記録です。

### 1. A2AはC++専用だった — Pythonバインディングがそれを公開していなかった（v0.2.1で修正済み）

過去のv0.2.1でPython A2A clientが追加されました。現在のバインディングは延期され、将来リリースの提供を約束しません。

### 2. システムインストールなし／ホイール内にヘッダーなし（README v0.2.1で修正済み）

過去のREADMEはFetchContentを説明しました。このrecipeには統合ターゲットだけがありstandalone CMakeプロジェクトはありません。SchemaProviderが必要です。

### 3. `OpenAIProvider::create()` `unique_ptr` vs `shared_ptr` (v0.2.1で修正)

過去の`create_shared`は旧所有権問題を解決しました。現在は`examples::make_openrouter_provider`とtyped outcomeを使います。

### 4. `.env` autoload が A2A 子プロセスに伝播しない（v0.2.1 で文書化済み）

`cppdotenv::auto_load_dotenv()`はそれを呼び出すバイナリ内で動作しますが、子サーバーをフォークするランチャースクリプトは、最初に親シェルで`source .env`を実行する必要があります。現在は[`docs/troubleshooting.md`](../../../docs/troubleshooting.md)の「Build from source」に記載されています。

### 5. スムーズに機能した点（肯定的なメモ）

- `A2AServer::start_async` + auto-port (`port=0`) は問題ありませんでした。
- AgentCardの発見（`fetch_agent_card`）は、手動のHTTPを必要とせずにそのまま機能しました。
- 並行 `send_message_sync` からの `std::async` フューチャー — クライアント側のロックなし、共有セッション状態なし。A2A仕様 / NeoGraph はどちらも並行クライアントリクエストを追加設定なしでクリーンに処理します。
- `parse_vote`正規は自由形式韓国語テキストで機能します。モデルが要求された際に`vote: support/oppose/abstain`を確実に尊重するためです。パーソナの出力がフォーマット内に収まることで、5行の集計関数となりました。
- 歴史的in-treeビルド経験です。現在の前提は上記を参照してください。

## Files

```
ai-assembly/
├── member_server.cpp           # one configurable persona server
├── speaker.cpp                 # orchestrator, broadcasts bill, tallies
├── speaker.py                  # Python A2A client variant
├── prompts/
│   ├── jinbo.txt               # Kim Jinbo (Progress)
│   ├── bosu.txt                # Park Bosu (Conservative)
│   ├── jungdo.txt              # Jung Jungdo (Center)
│   └── nokdang.txt             # Na Noksaek (Green)
├── bills/
│   └── basic_income.txt        # sample bill: National Basic Income Law
└── scripts/
    └── run_session.sh          # spin up 4 members + run speaker
```

## License

MIT、NeoGraphと同様。
