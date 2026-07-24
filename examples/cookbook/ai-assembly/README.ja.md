<!-- neograph-i18n: source=examples/cookbook/ai-assembly/README.md locale=ja source_sha256=828f35d27b957d55c8c766d3ce714ae4094397f9f2d4f0cabea710750619cb9a -->
# AI国会

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

**新しい NeoGraph ユーザー**として構築されたおもちゃのデモ — すべての API の選択は
公開ドキュメント (README、github の例、Doxygen) を読んで作成されました。
NeoGraph のソースを開く必要はありません。ポイントは 2 つあります。
A2A が実際の複数ペルソナのシナリオで機能することを証明し、
新人の C++ 開発者は途中でぶつかる摩擦。

## 何をするのか

国会議員4名がそれぞれ異なる港に座っており、
それぞれが、個別のペルソナ プロンプトによってサポートされる A2A エンドポイントであり、
同じ OpenAI モデル (`gpt-5.4-mini`)。議長（国会議長）は別人である
を介してすべてのメンバーに請求書を並行してブロードキャストするプログラム
NeoGraph の `A2AClient` は、返信から各メンバーの投票を解析し、
結果を宣言します。

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
       (PersonaNode → OpenAI gpt-5.4-mini, persona-specific system prompt)
```

各メンバーは 1 ノードの NeoGraph (`__start__ → persona → __end__`) です。
`a2a::A2AServer` の背後で提供されます。グラフは `prompt` チャネルを読み取り、
`response` チャネルを書き込みます。 A2Aサーバーのデフォルト
`GraphAgentAdapter` は、JSON-RPC 経由でそれらを表示します。

## ライブトランスクリプト (gpt-5.4-mini、2026-04-29)

ビル: [`bills/basic_income.txt`](bills/basic_income.txt) — ユニバーサル
ベーシックインカム、月額50万ウォン、財源は土地+炭素+累進税。

```
[Speaker of the National Assembly] Bill submission: [National Basic Income Law]

[Progress Kim Jinbo]   Protecting socially vulnerable groups + asset/carbon taxation = alignment        → Support
[Conservative Park Bosu]   200 trillion mandatory spending + market distortion + real estate shock    → Oppose
[Center Jung Jungdo]   Acknowledging intent but excessive amount; suggests phased reduction amendment  → Oppose
[Green Na Noksaek]   Carbon tax + unearned income taxation + equitable distribution                    → Support

[Speaker of the National Assembly] Vote result:  2 in favor  /  2 opposed  /  0 abstention
[Speaker of the National Assembly] Tie vote — the bill is rejected (custom).
```

各ペルソナの推論は、当事者が表明した価値観を真に追跡します。
それはフレームワークが行っていることではありません。OpenAI が個別の機能を尊重しているだけです。
システム プロンプトは表示されますが、アセンブリの仕組み (並行 A2A、投票集計、
Discovery) は純粋な NeoGraph です。

## ビルド + 実行 (NeoGraph ツリー内)

```bash
# from NeoGraph repo root
cmake --build build-pybind --target \
    cookbook_ai_assembly_member cookbook_ai_assembly_speaker -j4

echo 'OPENAI_API_KEY=sk-...' > .env

bash examples/cookbook/ai-assembly/scripts/run_session.sh
```

## Python スピーカー バリアント (v0.2.1+、クロスランゲージ A2A)

同じスピーカーのロジックを Python の約 100 行で、同じものに対して
C++ メンバー サーバー — A2A プロトコル ブリッジ言語を証明します
きれいに：

```bash
pip install neograph-engine          # >= 0.2.1
# (start the C++ members in another terminal as above)
PYTHONPATH=build-pybind python3 examples/cookbook/ai-assembly/speaker.py \
    examples/cookbook/ai-assembly/bills/basic_income.txt \
    http://127.0.0.1:8101 http://127.0.0.1:8102 \
    http://127.0.0.1:8103 http://127.0.0.1:8104
```

Python A2A バインディング (`neograph_engine.a2a`) は v0.2.1 で出荷されます。
サーバー側 (A2A エンドポイントとしてのグラフ) は、現時点では C++ のみのままです。

## 摩擦ジャーナル — 新人 NeoGraph ユーザーがつまずいたもの

これらは、これを構築中に発見された大まかなエッジです。 **4つすべて
v0.2.1** で修正されました — 記録としてここに残しました。

### 1. A2A は C++ のみでした — Python バインディングはそれを公開しませんでした (v0.2.1 で修正)

`pip install neograph-engine` は動作しますが、v0.2.1 より前の `neograph_engine`
`A2AClient` / `AgentCard` はエクスポートされませんでした。 v0.2.1 では、
`neograph_engine.a2a` サブモジュール (クライアント + AgentCard + タスク/メッセージ/
Part/TaskState/Role) — 上記の Python スピーカーのバリアントを参照してください。

**サーバー側バインディングは依然として C++ のみです**。 A2Aサーバーには
v0.3 のフォローアップである GIL 対応のライフサイクル コントラクト。

### 2. システムがインストールされていない / ホイールにヘッダーがない (README v0.2.1 で修正)

README に「CMake プロジェクトから NeoGraph を使用する」セクションが追加されました。
`FetchContent_Declare` パターンを示しています。この料理本も生きています
NeoGraph ツリー内にあるため、何もせずに直接 `add_executable` できます。
外部依存関係 — スタンドアロン バリアントでは FetchContent が使用されます。

### 3. `OpenAIProvider::create()` `unique_ptr` 対 `shared_ptr` (v0.2.1 で修正)

`OpenAIProvider::create_shared(cfg)` が追加されました — を返します
`shared_ptr<Provider>` を直接使用するため、きれいにキャプチャされます。
`NodeFactory` クロージャ。クックブックの ~133 行目で使用されています。
`member_server.cpp`。

### 4. `.env` 自動ロードは A2A 子プロセスに伝播しません (v0.2.1 で文書化)

`cppdotenv::auto_load_dotenv()` は、を呼び出すバイナリ内で動作します。
ただし、子サーバーをフォークするランチャー スクリプトは `source .env` でなければなりません
まず親シェル内で。現在文書化されているのは、
[`docs/troubleshooting.md`](../../../docs/troubleshooting.md)以下
「ソースからビルド」。

### 5. スムーズに機能したこと (肯定的なメモ)

- `A2AServer::start_async` + 自動ポート (`port=0`) は痛みがありませんでした。
- AgentCard 検出 (`fetch_agent_card`) は正常に動作しました - マニュアルはありません
  HTTPが必要です。
- `std::async` 先物からの同時 `send_message_sync` — いいえ
  クライアント側のロック、共有セッション状態なし。 A2A仕様 /
  NeoGraph はどちらも並列クライアント要求を完全に処理します。
  箱。
- 自由形式の韓国語テキストの `parse_vote` 正規表現は、モデルが次のように機能するため機能します。
  尋ねられた場合は、確実に `vote: support/oppose/abstain` を尊重します。ペルソナの出力
  フォーマット内にとどまるため、これは 5 行の集計関数になりました。
- ビルドはクリーンでした — FetchContent は v0.2.0 をプルしましたが、手動によるデプロイはありませんでした
  インストール。標準の Ubuntu 上の OpenSSL/CURL で十分でした。

## ファイル

```
ai-national-assembly/
├── CMakeLists.txt              # FetchContent NeoGraph v0.2.0
├── src/
│   ├── member_server.cpp       # one binary, configurable persona
│   └── speaker.cpp             # orchestrator, broadcasts bill, tallies
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

## ライセンス

MIT、NeoGraphと同じ。
