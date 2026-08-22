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
cmake -S . -B build-cookbook \
    -DNEOGRAPH_BUILD_EXAMPLES=ON \
    -DNEOGRAPH_BUILD_PROGRAM=ON \
    -DNEOGRAPH_BUILD_A2A=ON \
    -DNEOGRAPH_BUILD_LLM=ON
cmake --build build-cookbook --target \
    cookbook_ai_assembly_member cookbook_ai_assembly_speaker -j4

echo 'OPENROUTER_API_KEY=sk-or-...' > .env

bash examples/cookbook/ai-assembly/scripts/run_session.sh
```

メンバーサーバーはライブのOpenRouter呼び出しを行います。`OPENROUTER_API_KEY`とネットワークアクセスが必要です。コンパイル自体はオフラインです。

## Pythonスピーカーバリアント（v0.2.1+、クロス言語A2A）

同じスピーカーロジックを、約100行のPythonで、同じC++メンバーサーバーに対して実行—これにより、A2Aプロトコルが言語をクリーンに橋渡しすることが証明されます。

```bash
pip install 'neograph-engine>=0.2.1'
# (start the C++ members in another terminal as above)
PYTHONPATH=build-cookbook python3 examples/cookbook/ai-assembly/speaker.py \
    examples/cookbook/ai-assembly/bills/basic_income.txt \
    http://127.0.0.1:8101 http://127.0.0.1:8102 \
    http://127.0.0.1:8103 http://127.0.0.1:8104
```

Python A2A バインディング（`neograph_engine.a2a`）は v0.2.1 で提供される。サーバー側（graph-as-A2A-endpoint）は今のところ C++ のみのままである。

## 摩擦ジャーナル — 新しい NeoGraph ユーザーがつまずいた点


ビルド中に判明した粗雑なエッジを以下に示す。**4 件すべて v0.2.1 で修正** — 記録としてここに残しておく。

### 1. A2AはC++専用だった — Pythonバインディングがそれを公開していなかった（v0.2.1で修正済み）

`pip install neograph-engine` は動作しますが、pre-v0.2.1の`neograph_engine`は`A2AClient` / `AgentCard`をエクスポートしていませんでした。v0.2.1では`neograph_engine.a2a`サブモジュール（client + AgentCard + Task/Message/Part/TaskState/Role）を追加しています — 上記のPythonスピーカー変種を参照してください。

**サーバーサイドのバインディングはまだC++専用です**。A2AServerにはGIL対応のライフサイクルコントラクトが必要であり、これはv0.3のフォローアップです。

### 2. システムインストールなし／ホイール内にヘッダーなし（README v0.2.1で修正済み）

READMEに「Using NeoGraph from your CMake project」というセクションが追加され、`FetchContent_Declare`パターンを示しています。このクックブックはNeoGraphツリー内にも存在するため、外部依存なしで`add_executable`を直接実行できます。スタンドアロン版はFetchContentを使用します。

### 3. `OpenAIProvider::create()` `unique_ptr` vs `shared_ptr` (v0.2.1で修正)

`OpenAIProvider::create_shared(cfg)` が追加されました — 直接 `shared_ptr<Provider>` を返すため、`NodeFactory` クロージャへきれいに取り込まれます。クックブックは `member_server.cpp` の約133行目でこれを使用しています。

### 4. `.env` autoload が A2A 子プロセスに伝播しない（v0.2.1 で文書化済み）

`cppdotenv::auto_load_dotenv()`はそれを呼び出すバイナリ内で動作しますが、子サーバーをフォークするランチャースクリプトは、最初に親シェルで`source .env`を実行する必要があります。現在は[`docs/troubleshooting.md`](../../../docs/troubleshooting.md)の「Build from source」に記載されています。

### 5. スムーズに機能した点（肯定的なメモ）

- `A2AServer::start_async` + auto-port (`port=0`) は問題ありませんでした。
- AgentCardの発見（`fetch_agent_card`）は、手動のHTTPを必要とせずにそのまま機能しました。
- 並行 `send_message_sync` からの `std::async` フューチャー — クライアント側のロックなし、共有セッション状態なし。A2A仕様 / NeoGraph はどちらも並行クライアントリクエストを追加設定なしでクリーンに処理します。
- `parse_vote`正規は自由形式韓国語テキストで機能します。モデルが要求された際に`vote: support/oppose/abstain`を確実に尊重するためです。パーソナの出力がフォーマット内に収まることで、5行の集計関数となりました。
- ツリー内CMakeビルドは自己完結型です。上記のように`NEOGRAPH_BUILD_A2A=ON`と`NEOGRAPH_BUILD_LLM=ON`で設定してください。

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
