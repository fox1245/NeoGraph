<!-- neograph-i18n: source=ROADMAP_v1.md locale=ja source_sha256=e5d4df28a5ba92ca1778fdff4fa741fc4c435c9dfba1fddbe8dacd31ca1f3121 -->
# NeoGraph v1.0 — 設計研ぎ澄ましロードマップ

**Languages:** [English](ROADMAP_v1.md) | [한국어](ROADMAP_v1.ko.md) | [日本語](ROADMAP_v1.ja.md) | [简体中文](ROADMAP_v1.zh-CN.md)

本ファイルは将来の v1.0 メジャーバンプを対象とする**アーキテクチャ上の**変更を
追跡します。これらは漸増的パッチではなく、各々が非推奨期間を必要とする
公開 API 破壊候補です。リビングドキュメントとして維持 — v0.3.x パッチ
シリーズが構造的な痛点を露呈した際にここに候補を追加し、着地時に削除。

## このファイルが存在する理由

v0.3.x キャンセル伝播シリーズ (5 ラウンド: v0.3.0 単一ノード、
v0.3.1 マルチ Send ポインタ、v0.3.1+ インプロセスポーリング、v0.3.2
Python 用フック、v0.3.2 C++ スコープ+リトライ+例外型付け) は、
**同じ横断的関心事 (キャンセル) が ~8 つのディスパッチエントリポイント +
2 つのエントリ言語 (C++/Python) を通じてスレッド化されなければならなかった**
ため、5 つのパッチを要した単一の論理的修正でした。各パッチが 1 つの
エントリパスをクローズする間、他を開いたままにしていました。

バグパターンはほぼ「アーキテクチャが間違っている」ではなく、
「正しいパターンが N 箇所のうち M 箇所に適用された」でした。
v0.3.x シリーズは例外によって*コア*設計 (Pregel BSP スーパーステップ、
チャネルリデューサ、Send/Command 動的ディスパッチ、全体を通じた asio
コルーチン) を検証しました — モデルを疑うことなくバグが捕捉されました。

このシリーズが実際に露呈したのは、現在の設計における 3 つの高認知負荷の
継ぎ目であり、N 箇所実装分布をエラーがちにしています。以下の各候補が
1 つの継ぎ目に対処します。

---

## Candidate 1 — タグベースルーティングによる単一ディスパッチエントリ

### 症状

`GraphNode` は 8 つの仮想メソッドを公開:

```
execute            execute_async
execute_full       execute_full_async
execute_stream     execute_stream_async
execute_full_stream execute_full_stream_async
```

これらは `(sync/async) × (writes/full) × (stream/non-stream)` の直積を形成。
デフォルトは互いにチェーン。優先順位は一貫していなければならない。
すべてのデフォルトチェーンホップがバグの隠れ場所:
- v0.3.1 #2: ヒントメッセージがストリーミングバリアントに言及せず。
- v0.3.2 #10 (Python): `PyGraphNode::execute_full_stream` が
  `execute_stream` 分岐をスキップ — ストリーミング専用ノードで
  `run_stream` が無効化。
- v0.3.2 #10 (C++): `GraphNode::execute_full_stream` デフォルトが
  最初に `execute_full` を呼び出し → `ExecuteDefaultGuard` 再帰が
  スロー → `execute_stream` に決して到達せず。
- `execute_full_stream_async` で GCC-13 コード生成回避策が必要 —
  `co_await` 周りの `catch(T&)` が暗黙に失敗するため。

### 研ぎ澄まし

単一仮想ディスパッチ:

```cpp
class GraphNode {
public:
    virtual NodeOutput run(const NodeInput& in) = 0;
};

struct NodeInput {
    const GraphState&    state;
    const RunContext&    ctx;          // see Candidate 2
    GraphStreamCallback  stream_cb;    // null if non-stream
    bool                 is_async;     // hint, not a hard contract
};

struct NodeOutput {
    std::vector<ChannelWrite> writes;
    std::optional<Command>    command;
    std::vector<Send>         sends;
};
```

ユーザーは 1 つのメソッドをオーバーライド。同期/非同期の区別はエンジンが処理
(エンジンが同期オーバーライドを run_sync でラップ、非同期オーバーライドは
直接 await — ただしこれはエンジンの関心事でありユーザーの関心事ではない)。
ストリーミングの区別: `stream_cb` 非 null = ストリーミング期待。ユーザーは
使用または無視。Command/Send: フィールドを設定するだけ。

移行: 8 仮想メソッドを 1 リリースの間非推奨の薄いシムとして保持。
新コードは `run()` をオーバーライド。トランポリン (`PyGraphNode`) は
ワンライナーに。

### コスト

- 公開 API 破壊 — 既存のすべての GraphNode サブクラスが `run()` 書き直しが必要。
- `RunContext` (Candidate 2) はハード前提条件。さもなければ `run()` が
  実行毎メタデータを運べない。
- エンジン内部ディスパッチロジックは単純化されるが、エンジンは
  実行時ヒントまたは規約に基づいて sync-vs-async を選択しなければならない。

---

## Candidate 2 — 実行毎メタデータ用の明示的 `RunContext`

### 症状

現在 `RunConfig::cancel_token` が呼出元が設定できる唯一の実行毎「メタデータ」。
エンジンは 2 つの機構でそれを密輸:

1. `GraphState::run_cancel_token_` — GraphState 内に存在するが
   **チャネルセット内にはない**メンバで、`serialize()` が失う。
   - v0.3.1 マルチ Send 修正: `init_state(send_state) +
     send_state.restore(snapshot)` がワーカー毎状態を再構築したが、
     チャネルセット外のため `run_cancel_token_` をドロップ。
     全 Send ファンアウトワーカーで明示的な
     `send_state.set_run_cancel_token(parent.run_cancel_token_shared())`
     が必要だった。
   - 次の実行毎フィールド (deadline? trace_id? メトリクスハンドル?) を
     追加する者はこのまったく同じバグに再び当たる。

2. `current_cancel_token()` thread_local — execute_full_async エントリで
   `CurrentCancelTokenScope` が設定。
   - v0.3.2 C++ 修正: PyGraphNode がスコープをインストール。ネイティブ
     C++ `GraphNode::execute_full_async` デフォルトは NOT で、
     マルチ Send C++ ワーカーの `Provider::complete` が null スレッド
     ローカルを見て run_sync がキャンセルバインディングなしで実行。
     7s コスト漏洩。
   - すべての新しいディスパッチエントリポイントがスコープを
     インストールすることを覚えておく必要がある。忘れ = 暗黙の機能破損。

両方の機構は実行毎メタデータのための第一級の場所がないために存在する。
これらは回避策。

### 研ぎ澄まし

すべてのディスパッチを通じて `GraphState` と共に運ばれる明示的
`RunContext`:

```cpp
struct RunContext {
    std::shared_ptr<CancelToken>  cancel_token;
    std::optional<Deadline>       deadline;
    std::string                   trace_id;
    std::string                   thread_id;
    int                           step;
    StreamMode                    stream_mode;
    // ... extension point for future cross-cutting concerns
};

class GraphNode {
public:
    virtual NodeOutput run(const NodeInput& in) = 0;
    // in.ctx is the RunContext — no thread_local, no
    // serialize-loses-it. Every dispatch path threads it explicitly.
};
```

`Provider::complete(params, ctx)` もコンテキストを受け取る。
thread_local なし。`current_cancel_token()` なし。Send ファンアウト
ワーカーは値をコピー (安価 — shared_ptr + 数個の文字列)。

### コスト

- 公開 API 破壊 — すべての Provider、すべての GraphNode、すべての Tool。
- 全体でより広いシグネチャ — `state, ctx` がいたるところに。
- しかし: 「キャンセル/トレース/デッドラインのスレッド化を忘れた」
  バグのクラス全体をクローズ。1 つのシグネチャ、新しいフィールドを
  追加する 1 つの場所、回避策なし。

### この候補が防いだであろう v0.3.x のバグ

- v0.3.1 マルチ Send ポインタドロップ: ctx は単なる明示的フィールドで、
  非直列化メンバに埋もれていない。
- v0.3.2 C++ thread_local 欠落: thread_local がまったくない。
- 将来の deadline / trace_id / メトリクス漏洩: 同じ形状、同じ予防カバレッジ。

---

## Candidate 3 — 階層的 / 利用者毎 CancelToken

### 症状

`CancelToken` は 1 つの `cancellation_signal sig_` + 1 つの `bind_executor`
スロットの周りに設計された。asio の `cancellation_slot` は単一ハンドラ —
最後の `bind_cancellation_slot` が勝つ。同時利用者 (各々が
Provider::complete → 内部 run_sync → bind_cancellation_slot を呼ぶ
マルチ Send ファンアウトワーカー) が互いのバインディングを暗黙に上書き。
最後にバインドされた HTTP のみがキャンセルされた。

v0.3.2 はこの単一シグナル設計の上に `add_cancel_hook` リストを接ぎ木し、
各ネストされた run_sync が親の `cancel()` がフック反復で発火する自身の
プライベートシグナルを所有するように。動作するが、「N 利用者コンテキストで
使用される単一利用者プリミティブの補償」と読める。加えて emit-vs-bind 競合:
add_cancel_hook が呼ばれた時点でキャンセルが既に設定されていた場合、
同期発火が co_spawn がスロットをバインドする前に emit を post し、
emit が失われる。v0.3.2 は run_sync エントリで熱心な
`is_cancelled()` 短絡を追加してこれをかわした — パッチの上のパッチ。

### 研ぎ澄まし

階層的キャンセル:

```cpp
class CancelToken {
public:
    /// Create a child token. Parent.cancel() cascades to child.
    /// Each child has its OWN cancellation_signal — no
    /// single-consumer assumption.
    std::shared_ptr<CancelToken> fork();

    /// Cancel this token (and recursively all children).
    void cancel();

    bool is_cancelled() const noexcept;
    asio::cancellation_slot slot();  // each token has its own
    void bind_executor(asio::any_io_executor ex);
};
```

各 `run_sync(aw, parent_token)` が:
```cpp
auto child = parent_token->fork();
child->bind_executor(io.get_executor());
asio::co_spawn(io, body(),
    asio::bind_cancellation_slot(child->slot(), asio::detached));
```

接ぎ木する add_cancel_hook リストなし。emit-vs-bind 競合なし
(子は新しく作成され、シグナルが最初にバインドされ、fork() が親状態の
スナップショット)。マルチ Send ファンアウト: 3 兄弟トークン、親が 3 つ
すべてをキャンセル。

借用元: Go の `context.Context` キャンセル、asio の
`asio::cancellation_state` / `make_cancellation_filter`
(asio が適切な API を獲得した場合)。パターンはよく知られている。

### コスト

- CancelToken への公開 API 変更 (追加的 — `fork()` は新規)。
  古い `add_cancel_hook` は非推奨化される。
- 内部: すべての `run_sync(aw, cancel)` が `run_sync(aw, cancel->fork())` に。
- ネット: 1 つのプリミティブが「単一シグナル + フックリスト +
  熱心キャンセル短絡 + 利用者毎競合ノート」を置換。

---

## 横断的観察

3 つの候補は構成される: Candidate 2 が Candidate 3 のトークンを
ディスパッチパスを通じて運ぶ。Candidate 1 の単一 `run()` は自然に
キャンセル子を含む `RunContext` を受け取る。

1 つだけ着地させるなら Candidate 2 を優先 — 再発バグの最大クラス
(全ディスパッチを通じてスレッド化される必要があるものすべて) を殺す。

追跡: 本ファイルは v0.3.x パッチラウンドが新しいアーキテクチャ上の
継ぎ目を露呈したとき、または候補が着地したとき (打ち消し線とマージ
コミットへのリンク) に更新される。

---

## パターン回顧 — 9 つの下流所見 (issue #36)

v0.5 → v0.8 期間にわたる ProjectDatePop の `cpp_backend` ストレステストが
9 つの NeoGraph 所見を着地させた。**これら 9 つのうち少なくとも 7 つが
同じ構造的パターン** に遡る — Candidates 1 + 6 が閉じるのは漸増的ではなく、
*パターンが再発しうる表面を排除することによって*。

### 統一パターン

> **「X は Y の場合にのみ安全」 — しかし Y の前提条件が docstring に
> 記載されておらず、コンパイル時に強制されず、違反時に実行時にも
> 表面化しない。デフォルトパスが暗黙に間違ったことを行い、多くの場合
> 入力直積の特定の隅でのみ発生する。**

| # | 隠れた条件付き不変条件 |
|---|---|
| #4 | `Provider::complete_stream_async` デフォルトブリッジは、ネイティブ同期 `complete_stream` がそれ自体 `run_sync` を使用しない場合**にのみ**安全 — `SchemaProvider` WS パスによって暗黙に違反 |
| #5 | `Provider` の 4 仮想直積は、選ばれたオーバーライド表面がたまたまブリッジネストを避ける場合**にのみ**安全 — 不変条件が `provider.h` から不可視 |
| #6 | `schema_mutex_` × on_chunk ロッキングは、ユーザーのコールバックが SchemaProvider に再入しない場合**にのみ**安全 — 修正前は文書化なし |
| #9 | C++ openinference パリティが必要だったのは、Python ラッパーが翻訳されないコールバックスレッド同一性に関する隠れた仮定を持っていたため |
| #16 | NeoGraph のバンドル cpp-httplib は、すべての利用者 TU が `CPPHTTPLIB_OPENSSL_SUPPORT` を定義する場合**にのみ**正しい — さもなければ暗黙の ODR 違反 |
| #34 | `extra_fields` は `params.tools` が非空の場合**にのみ**適用 — ツールなし呼出で推論フィールドが暗黙ドロップ |
| #35 | `temperature` は `params.temperature ≥ 0` の場合**にのみ**送信 — しかしスキーマに「このプロバイダは temperature を全く受け付けない」と宣言する手段がなく、全呼出サイトがデフォルトを否定することを強制 |

さらに 2 つの所見 (#17 ドキュメントギャップ、#33 呼出毎バインディングギャップ) は
隠れた不変条件トラップではなくギャップ報告。同じ根本診断 (抽象が静的表面を
宣言したが動的等価物を公開しなかった) が適用される。

### Candidates 1 + 6 が*クラス*を閉じ、インスタンスだけではない理由

上記の各所見は、誤動作した特定のオーバーライドサイトへの**標的パッチ**を
通じてクローズされた (PR #10、PR #11、PR #12、PR #19、PR #20、PR #37、
PR #37)。各パッチは*パターンを許した表面*を変更せずに残した: 8 GraphNode
仮想メソッド、4 Provider 仮想メソッド、schema build_body 分岐ツリー。
次の下流 — または次のベンダースキーマ、または 1 つのデフォルトを調整する
次のリファクタ — が、他の何らかの「X は Y の場合にのみ安全」が潜む同じ
直積の新しい隅を発見するだろう。

Candidates 1 と 6 はそれらの直積を**各 1 仮想メソッド**に集約する。
着地後:

- **Candidate 1** (GraphNode 8 → 1): 「8 仮想メソッドのうちどれを
  オーバーライドするかがブリッジの安全性を決定する」決定はもはや存在しない。
  ユーザーは `run(NodeInput)` をオーバーライド。Sync vs async、
  stream vs non-stream、writes vs full-result はすべて本体形状の選択 —
  仮想同一性に結びついた隠れた不変条件なし。
- **Candidate 6** (Provider 4 → 1): 新しい実装は
  `CompletionProvider::do_invoke()` を 1 つのオーバーライドポイントとして
  使用。既存の `Provider` 表面は互換性のため安定を保ち、新しいパスは
  1 つの明示的リクエストモードと 1 つのドレインパターンを持つ。

残る 2 つの所見 (#9 スレッド同一性、#16 ODR マクロ) は Candidates 1 + 6
では*修正されない* — それらは別の課題クラス (可観測性レイヤパリティ、
ビルドシステム規約)。#9 は既に解決済み (PR #12 + パリティテスト)。
#16 は現在コンパイル時ガード (v0.8.0 `api.h`)。

### この回顧で依然として重要なこと

9 つの所見は**プロジェクトの年齢に関わらず**表面化しただろう。どれも
長期実行本番デプロイメントやエキゾチックなベンダーを必要としなかった —
それらは ~3 週間にわたって現実的なエージェントフローを書いた単一の下流
利用者 (ProjectDatePop) から来た。Candidates 1 + 6 なしでは、同等の深さの
次の下流が同じ形状のさらに 5-10 の所見を着地させるだろう。それらがあれば、
クラスは閉じられる。

これは **v1.0 サイクルで Candidates 1 + 6 を優先する** という構造的議論 —
より表面的な v0.x クリーンアップに対して。新しい「X は Y の場合にのみ安全」
所見はそれぞれパッチ労力で元が取れたが、7 所見にわたる累積労力は既に
Candidates 1 + 6 が推定コストを超過している。

### v0.x 非推奨期間での緩和

Candidates 1 + 6 が着地するまで、不変条件を今日存在する場所に固定:

- `[[deprecated]]` on legacy 8 virtuals + `docs/migration-v0.4-to-v1.0.md`
  — v0.4 / v0.8 着地。
- すべての「Y の場合にのみ安全」前提条件を持つオーバーライドポイントに
  `@warning` ブロック (例: `Tracer::start_span`、
  `OpenInferenceTracerSession::close`)。
- 言語が表現できる TU 間不変条件にコンパイル時 `#error` ガード
  (例: `CPPHTTPLIB_OPENSSL_SUPPORT` マクロ一貫性 — v0.8 着地)。
- 違反時に不変条件を名前付けするフレンドリーな実行時エラー
  (例: `Unknown reducer: 'foo'. Available: ...` — v0.8 着地)。

これらはパターンが噛み付く時間窓を狭めるが、クラスを閉じない。
Candidate 1 + 6 が閉じる。

---

## ステータス

| # | 候補 | ステータス | トリガーラウンド / 課題 |
|---|---|---|---|
| 1 | 単一 `run()` ディスパッチ + タグ | **v0.9.0 で着地。** `run(NodeInput)` が純粋仮想。レガシー 8 仮想メソッドとフォールバックチェーンは消滅。 | v0.3.1 #2, v0.3.2 #10 (×2 言語); #36 によりパターン補強 (9 下流所見) |
| 2 | 明示的 `RunContext` 引数 | **v0.4–v0.8 で着地** (`RunContext::store` フィールド v0.8 #27 追加) | v0.3.1 マルチ Send, v0.3.2 C++ スコープ |
| 3 | 階層的 CancelToken | **v0.4 で着地** (`CancelToken::fork()` + カスケード) | v0.3.2 フック, v0.3.2 emit-vs-bind |
| 4 | 自己進化グラフ実行時フック | リサーチ | TODO_v0.3.md #8 |
| 5 | pgvector RAG サンプル | クックブック | TODO_v0.3.md #9 |
| 6 | Provider 単一ディスパッチ | **削除なしで着地。** `CompletionProvider::do_invoke()` が推奨される 1 オーバーライドパス。既存の `Provider::complete*` メソッドはサポート継続。非推奨警告は撤回され削除計画なし。 | #4 (v0.7 クローズ), #5 (互換性ポリシー), #36 によりパターン補強 |

---

# 実行計画

> **ステータス:** Candidate 1 は完了。以下の計画は v0.4.0 から破壊的
> v0.9.0 v1 準備リリースを通じて移行がどのように段階化されたかを記録。
> 歴史的コンテキストであり、残作業ではない。

## ユーザー向け動機

バグクラスフレーミングはしばらく忘れてほしい。**今日 README を開く新規ユーザー**
から見ると、表面は断片化して見える:

  - 「ノードをどう書くか？」 — 8 仮想メソッド (`execute` / `execute_async`
    / `execute_full` / `execute_full_async` / `execute_stream` /
    `execute_stream_async` / `execute_full_stream` /
    `execute_full_stream_async`)。どれを選ぶか？答えは「Send/Command、
    sync/async、ストリーミング/非ストリーミングに依存」 — ユーザーが
    前もって推論しなければならない 3 つの直交軸。
  - 「キャンセルをどうやるか？」 — `RunConfig::cancel_token` は存在するが、
    キャンセルが LLM に到達するにはさらに必要なもの: (a) エンジンが
    thread_local スコープをインストール、(b) Provider::complete がそれを
    読み取る、(c) run_sync がフックを登録、(d) ワーカーがリトライしない。
    これらはどれも読むべき 1 か所にない。
  - 「状態をどう更新するか？」 — v0.3.2 では `dict | list[ChannelWrite]`。
    それ以前は README が 1 つの形状を文書化し、バインディングが他方で
    暗黙の no-op。新規ユーザーは「なぜ書き込みが適用されない？」に当たり
    デバッグを強いられる。
  - 「状態をどう読むか？」 — ネストされた `state["channels"][name]["value"]`
    OR フラットな `engine.get_state_view(thread_id).<channel>` OR 型付き
    Pydantic サブクラス。3 つの有効な答え。単一の正規のものはない。
  - 「グラフをどう実行するか？」 — `run` (sync) vs `run_async` vs
    `run_stream` vs `run_stream_async` vs `resume` vs `resume_async`。
    6 エントリポイント、再び多軸マトリックス。

**個々の追加はそれぞれ正当化された** (resume_if_exists は実際のチャット
セマンティクス、StateView は実際のエルゴノミクス上の勝利、等)。しかし
**累積効果は、1 つのことを行うのにドキュメント、サンプル、バインディング
コードに散らばった 2-4 通りの方法がある表面**。v0.3.x パッチは積み重なり
続けた。v0.3.x キャンセルラウンド (5 回) は、この断片化がバグの隠れ場所
でもあることを可視化した — 「正しい方法」が N か所のうち M か所にあるとき、
N+1 か所での省略が暗黙の no-op / 忘れられたパターンのバグ。

アーキテクチャ上の研ぎ澄まし (Candidates 1-3) はこれを以下に集約:

  - **ノードを書く 1 つの方法** (`run(NodeInput) -> NodeOutput` + タグ)。
  - **実行毎メタデータをスレッド化する 1 つの方法** (`RunContext` 引数)。
  - **キャンセルする 1 つの方法** (ネスト操作に `token->fork()`、親が
    すべてをキャンセル)。
  - **状態を読む 1 つの方法** (StateView が正規。生辞書はエスケープハッチ)。
  - **実行する 1 つの方法** (run / run_async 等をストリームコールバックを
    取るかイテレータを返す 1 つのメソッドに集約)。

これが v1.0 契約 — ドキュメントページが再び短く読める。

## バージョニング戦略

| バージョン | 範囲 | 公開 API |
|---|---|---|
| **v0.4.x** | RunContext が*新規*パラメータとして着地、古いメソッドは非推奨だが依然動作。CancelToken が追加的 `fork()` を獲得。新しい `run(NodeInput)` が追加的着地。 | 両方の API が呼出可能。非推奨警告。 |
| **v0.5.x** | サンプルと pybind バインディングが新 API に移行。古い API は非推奨のまま。 | 両方の API が呼出可能。より重い非推奨警告 + ドキュメントが新方式に誘導。 |
| **v1.0.0** | 古い API (8 仮想メソッド、thread_local スコープ、単一ハンドラ CancelToken signal-on-self) を削除。 | 単一正規 API。 |

根拠: **v0.4 → v1.0 への飛躍なし。** 2 リリースの非推奨期間が下流利用者
(neoclaw、NeoProtocol Executor、WASM スパイク、このリポジトリ外のあらゆるもの)
が 1 コンポーネントずつ移行することを可能にする。cibuildwheel マトリックスは
期間中無傷のまま — リリースパスごとに 20 wheel 変更なし、古いメソッドへの
依存が徐々に減少するだけ。

移行が予想より長引く場合 (例: サードパーティ C++ GraphNode サブクラスが
一般的)、v0.5 が v0.5.x に延長非推奨となり、v0.6 が期間を伸ばす。
非推奨警告が 1 リリースの間静かになってから古い API を削除。

## PR 順序付け

各行は 1 つのマージ可能 PR。順に着地、すべて master 上
(長寿命 feature branch なし — プロジェクトのコミット履歴はストレートラインで、
非推奨戦略は各 PR が v0.4.0+i, v0.4.0+(i+1) 等として PyPI に独立して
出荷可能であることを意味)。

| # | PR | 範囲 | 着地先 |
|---|---|---|---|
| 1 ✓ | **`RunContext` 配管 (内部)** — landed `a473f0e` | `struct RunContext` を `engine.h` に追加。エンジンの `execute_graph_async` が構築しスレッド化。NodeExecutor が `execute_full_async` に渡す。Pybind がラップ。**公開向け変更なし** — 古いメソッドは依然 `state` のみを受け取る。新しい `ctx` がディスパッチパス内に並存。ctest 442/442 + pytest 96/96 グリーン。ベンチ中央値 5.365 µs (BASE 5.285 µs, +1.5%) — WSL2 ~3% ノイズフロア内、5.185 µs ベースラインから ±5% バンド内。Pybind wrap は PR 7 (バインディング移行) に延期 (PR 1 は pybind diff ゼロのため)。 | v0.4.0 |
| 2 ✓ | **`GraphNode::run(NodeInput) -> NodeOutput`** — landed `607ce66` | GraphNode 上の新仮想メソッド。デフォルト実装が古い 8 仮想メソッドに委譲 (優先順位保持)。エンジンの優先ディスパッチエントリとして登録。既存 C++ サブクラスはデフォルトフォールバック経由で依然コンパイル+動作。ctest 442 → 445 (3 新 NodeRunDispatch テスト) + pytest 96/96 + 5 ライブ LLM/WS グリーン。ベンチ中央値 6.122 µs vs PR1 BASE 6.160 µs (Δ -0.6%) on A/B 10 rounds (ホストが今日ノイジー、PR1 BASE が昨日の 5.285 → 6.160 にドリフト — 同じコード、WSL2 ジッタ。A/B 比較がホストドリフトを相殺)。**捕捉されたトラップ**: ``run(const NodeInput&)`` が pybind 非同期パス下の asio エグゼキュータ内で SEGV (コルーチン参照パラメータ UAF、v0.2.0 RunConfig クラッシュ形状)。修正: ``NodeInput`` を値渡しに。node.h に文書化。 | v0.4.0 |
| 3 ✓ | **CancelToken `fork()` 追加** — landed `897645c` | `std::shared_ptr<CancelToken> CancelToken::fork()` 追加。親 `cancel()` が子にカスケード。`add_cancel_hook` は動作継続 (非推奨。`[[deprecated]]` 注釈は PR 4 で着地)。`run_sync(aw, cancel)` が `cancel->fork()` に切替。単一シグナル `slot()` API はエンジンの外部 co_spawn 用に残存。ctest 445 → 452 (7 新 CancelTokenFork テスト) + pytest 96/96 + 5 ライブ LLM/WS グリーン。ベンチ A/B 20 rounds (両方向インタリーブ): Δ min +1.0%, Δ median +1.5% — ±5% バンド内。ベンチパスに `cancel_token` がないため `fork()` にヒットせず、小さなデルタはバイナリレイアウトノイズ (PR3 bench binary が PR2 より 3.7KB 小さい、レイアウトが異なる)。 | v0.4.0 |
| 4 ✓ | **非推奨注釈** — landed `35a4517` | 8 古い仮想メソッド + `add_cancel_hook` に `[[deprecated]]` 追加 (Hook returned by it は間接的に非推奨)。トランポリンスコープ (`CurrentCancelTokenScope` / `current_cancel_token()`) は延期 — PR 7 (バインディング移行) が `ctx.cancel_token` 読み取りで置換する密輸チャネルのため、今それを非推奨にするとすべての密輸サイトで明確な移行パスなしに抑制が必要になる。内部呼出サイト (graph_node.cpp デフォルトチェーン、デフォルト `run()` 転送) は新しい `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` マクロ (`api.h` — GCC/clang/MSVC ポータブル) でブラケット。非推奨仮想メソッドをオーバーライドするか `add_cancel_hook` を呼び出すユーザーコードは移行警告を表示。エンジン内部はクリーン。ctest 452/452 + pytest 96/96 + 5 ライブ LLM/WS グリーン。ベンチ A/B 10 rounds: Δ median +0.3%, min +0.8% — 純粋な属性変更、レイアウトノイズ。`-Werror=deprecated-declarations` は有効化せず (CI がこれまで `-Werror` を持ったことがない。警告は非推奨期間中情報提供のまま)。 | v0.4.0 |
| 5 ✓ | **StateView 正規化、生 dict 非推奨** — landed `f31aa53` | `engine.get_state(thread_id) -> dict` を pybind docstring でソフト非推奨としてマーク。新しい正規 = `get_state_view(thread_id) -> StateView` (v0.3.2 に既に存在)。`DeprecationWarning` emit なし、`[[deprecated]]` 注釈なし — 生 dict は正当な用途 (チャネル毎 `version` アクセス、スナップショット直列化) を持つ。v1.0 はソフト非推奨が大きなフィードバックを生成しない限りエスケープハッチとして保持。行動変更ゼロ。ctest 452/452 + pytest 96/96 グリーン。 | v0.4.0 |
| 6 ✓ | **サンプル移行** — landed `a2a24ef` (PR 6a, C++) + `0a76e3a` (PR 6b, Python) | 7 C++ + 19 Python サンプル (合計 44 GraphNode サブクラス) が統一 ``run(NodeInput)`` API に切り替え。PR 6a は手動移行。PR 6b は AST スコープヘルパーを使用して安全にバッチ書き換え。スモーク実行が v0.3.2 出力とビット単位で一致。ctest 452/452 + pytest 96/96 グリーン。 | v0.4.x (6a + 6b に分割) |
| 7 ✓ | **Pybind バインディング移行** — landed `4e186a5` | ``PyGraphNodeOwner`` が ``GraphNode::run(NodeInput)`` をオーバーライドし、``has_user_method`` MRO ウォーク経由で Python ユーザーの ``run`` メソッドにディスパッチ。存在しない場合はレガシーチェーンにフォールスルー。``RunContext`` / ``NodeInput`` / ``CancelToken`` を Python にバインド (パッケージから再エクスポート)。密輸 ``CurrentCancelTokenScope`` は STAYS — レガシーチェーンが未移行ユーザーコードのために依然インストール。PR 9 がレガシー 8 仮想メソッドと共に削除。ctest 452/452 + pytest 96/96 + 5 ライブ LLM/WS グリーン。新しい ``run(input)`` API がエンドツーエンドで行使。 | v0.4.x |
| 8 ✓ | **ドキュメント書き直し** — landed `519a00b` | `docs/reference-en.md` §6 GraphNode を単一の `run()` 仮想メソッドに集約。新しい RunContext + CancelToken (`fork()` 例付き) サブセクションを §7 の下に。README "Differences from LangGraph" が `run(input)` を指す "One node method" エントリを取得。`@ng.node` デコレータの内部 `_DecoratorNode` が現在 `run()` を使用するため、5 秒デモが新しいパスを通じて実行。concepts.md / troubleshooting.md スイープは PR 9 に延期 (レガシーチェーン削除時にそれらが明らかに古くなる)。 | v0.5.0 |
| 9 ✓ | **古い API 削除** — ビルトイン移行 `d1070dc`。レガシー GraphNode チェーン `19819d8`。キャンセルフック `1d786a5`。スレッドローカル/Python レガシーブリッジ `9e8e956`。廃止 Python テスト `4392fbb`。 | v0.9.0 |

## 完了した v0.4.0 以降の計画 (歴史的)

v0.4.0 が 2026-05-05 に出荷 (`4cae42c`, tag `v0.4.0`)。以下に記述された
観察期間と破壊的削除は両方とも完了。v0.9.0 が 2026-05-14 に削除を出荷。

### Phase A — 非推奨期間 (完了)

期間: 数週間 ~ 1 マイナーサイクル。エンジンコード変更なし。このフェーズは
v1.0 が基盤コードを削除する前に非推奨警告が実際の下流破損を表面化する時間を
持つために存在。

監視項目:

  1. **非推奨可視性** — ユーザーは実際に 8 レガシー仮想メソッド +
     `add_cancel_hook` の `[[deprecated]]` 警告を見ているか？
     PR 4 (`35a4517`) が内部呼出サイトを
     `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` 下に置いたため、警告はユーザー
     オーバーライドサイトからのみ来るべき。課題トラッカー / ディスカッション /
     直接フィードバックチャネルで "what's this warning?" の言及を監視。
  2. **レガシーチェーン回帰** — レガシー 8 仮想デフォルトチェーンが破損する
     新たに発見されたケース (暗黙の no-op、忘れられたスコープ等)。
     v0.3.x にはこれらが 5 ラウンドあった。もう 1 つは妥当。
  3. **下流利用者破損** — `GraphNode` のサードパーティ C++ サブクラス。
     このリポジトリの軌道上の既知の利用者:
     - `neoclaw` — `src/neoclaw_nodes.cpp:94` が依然
       `std::vector<ChannelWrite> execute(const GraphState&) override`
       を持つ。v1.0 出荷前に `run(NodeInput)` に自己移行しなければ
       neoclaw が v1.0 wheel で破損。
     - `NeoProtocol` Executor runtime — NeoGraph WASM ビルドを使用。
       v0.4.0 バインディングテスト推奨。
     - WASM スパイク — engine-zero-diff パスは v0.3.x ベースラインだった。
       v0.4 run() 追加は追加的のためおそらく問題ないが、検証。
  4. **新人モードトラップ表面** — `ee11ed6` 新人スイープがチャットボット
     デモセッションから 5 つのトラップをクローズ。ストリーミング / MCP /
     非同期ファンアウト / HITL resume はそのデモが触れなかったパス。
     同様のトラップ密度が可能。新鮮な `cibuildwheel` + 初回ユーザー
     シミュレーションで表面化、または別セッションプライミング経由。
  5. **オプションパッチリリース** — Phase A が実際のバグを表面化した場合、
     v0.4.x パッチを出荷。v1.0 前に新しい機能が真に必要な場合、
     v0.5.0 マイナーを出荷 (依然非推奨期間内)。

終了基準: Phase A は非推奨警告が「1 リリースの間静か」になった時に終了 —
具体的には、レガシーパスに結びついたユーザー可視破損ゼロで 1 完全マイナー
サイクル (例: v0.5.0 出荷)。

### Phase B — 破壊的削除 (v0.9.0 で完了)

サブ PR は以下に独立して着地し、各ステップが単独でレビュー・復帰可能:

| サブ PR | 範囲 | リスク | 触れるファイル |
|---|---|---|---|
| **9b** | `graph_node.cpp` レガシーデフォルトチェーン (`ExecuteDefaultGuard` 再帰検出付き 8 仮想クロスルーティングロジック) を削除。`node.h` から 8 仮想宣言を削除。`src/core/deep_research_graph.cpp` (5+ サブクラス) と `src/core/plan_execute_graph.cpp` (3+ サブクラス) の内部ノードを `execute()` / `execute_full()` オーバーライドから `run(NodeInput)` に移行。 | **高** — すべての内部 GraphNode サブクラスが 1 PR で移行しなければならない。ビルトインノードは既に PR 9a (`d1070dc`) で移行済み。これら 2 つのグラフファクトリはノードが `nodes/` 内ではなくファイルローカルだったため最後のホールドアウト。 | `node.h`, `graph_node.cpp`, `deep_research_graph.cpp`, `plan_execute_graph.cpp` |
| **9c** | `add_cancel_hook` + `Hook` RAII クラス + `hooks_` メンバ + `hooks_mu_` + `cancel()` のフック反復ループを削除。`cancel.h` が `fork()` + `cancel()` + `is_cancelled()` + `slot()` + `bind_executor()` のみに縮小。 | **中** — `fork()` が正規の置換であり、7 CancelTokenFork ctest で行使。失敗モードは依然 `add_cancel_hook` を呼び出すユーザーコードでのリンクエラー (コンパイル時に捕捉、暗黙ではない)。 | `cancel.h` のみ (実装はヘッダオンリー) |
| **9d** | `CurrentCancelTokenScope` (ヘッダ + 実装) + `current_cancel_token()` thread_local アクセサ + `state.run_cancel_token_` メンバ + `set_run_cancel_token` / `run_cancel_token` / `run_cancel_token_shared` アクセサを削除。`cancel.cpp` が空に (ファイル削除可能)。`RunContext::cancel_token` が現在唯一のパス。 | **中** — すべての内部密輸サイトが既に `ctx.cancel_token` を読まなければならない (PR 7 バインディング完了。プロバイダ側読み取りは監査が必要)。失敗モード: 依然 `current_cancel_token()` を読むプロバイダが null を返す → キャンセルが LLM HTTP に伝播しない。 | `cancel.h`, `cancel.cpp` (削除), `state.h`, `graph_state.cpp`, plus `provider/*` にわたる監査スイープ |
| **9e** | `PyGraphNodeOwner` の 6 レガシー GraphNode オーバーライド (`execute(GraphState&)`, `execute_full`, `execute_full_async`, `execute_stream`, `execute_full_stream`, `execute_full_stream_async`) を削除 — `run(NodeInput)` + `get_name()` + dtor のみ残す。`tests/test_node_default_dispatch.cpp` + `tests/test_node_async_default.cpp` + それらの CMakeLists エントリを削除。 | **低** — 純粋な引き算、破壊するロジックなし。失敗モード: `execute()` のみを定義する (no `run()`) ユーザー Python クラスがディスパッチ時に NotImplementedError に当たる。Phase A がこれらを表面化しているはず。 | `bindings/python/src/bind_node.cpp`, `tests/CMakeLists.txt`, 2 テストファイル |

9b–e 着地後:

  - **SOVERSION 導入** ("bump" ではない — 現在どの neograph_* lib にも
    `set_target_properties(... VERSION ... SOVERSION ...)` が存在しない)。
    v1.0.0 は `libneograph_core` / `_llm` / `_postgres` / `_sqlite` /
    `_mcp` / `_a2a` / `_acp` にわたって SOVERSION 1 を導入する自然な
    瞬間。cibuildwheel マトリックスを検証 (manylinux soname suffix,
    macOS install_name, Windows DLL — 各々が SOVERSION を異なる方法で処理。
    "CMake property = it works" と仮定せず、ベンチスタイル検証として扱う)。
  - **ドキュメントスイープ** — `docs/concepts.md` "8 dispatch entry points"
    段落が 1 つに集約。`docs/troubleshooting.md` がレガシーチェーン
    エントリを削除。README "Differences from LangGraph" が "How NeoGraph
    thinks" に (ほとんどの LG-デルタエントリがギャップ解消により適用
    されなくなったため)。
  - **v1.0.0 タグ → PyPI 公開** — 最終ステップ。ロールバックコストが高い
    (PyPI リリースの取り下げ + タグ復帰) ため、タグをプッシュする前に
    完全な ctest + pytest + 5 ライブ LLM + cibuildwheel 20 wheel マトリックス
    グリーンを検証。

### このロードマップへの v0.4.0 以降の小さな修正

監査が捕捉した以前のドラフトの 2 つの小さな不正確さ:

  - **PyGraphNodeOwner レガシーオーバーライド数は 7 ではなく 6。** 以前の
    ノートは「7 overrides remove, run() only remains」と述べていた。実際の
    `bind_node.cpp:183` の `PyGraphNodeOwner` の GraphNode 派生オーバーライドは
    6 (8 GraphNode 仮想メソッドから決してオーバーライドされなかった
    `execute_async` と `execute_stream_async` を除く — デフォルトチェーンが
    処理)。9e 後: `run()` + `get_name()` + dtor が残り、`run()` だけではない。
  - **SOVERSION は "bump" ではなく "introduce"。** `CMakeLists.txt:5` の
    コメントが SOVERSION に言及しているが、実際の
    `set_target_properties(... SOVERSION ...)` 呼出は存在しない。v1.0
    が最初に設定するバージョン。含意: cibuildwheel マトリックス実行が
    SOVERSION サフィックスが Linux .so / macOS dylib install_name に現れた
    ときに wheel レイアウトが回帰しないことを検証する必要がある。

### 歴史的反事実: 「決して削除しなかったら？」

Phase B が決して着地しない場合 (v1.0+ にレガシーが残存)、システムは
**破損しない** — すべての現在のシナリオが動作継続、全 452 ctest がパス、
非推奨警告はユーザーオーバーライドサイトでのみ発火。コストは急性的ではなく
構造的:

  - **バグクラス繁殖地が開いたまま。** v0.3.x の 5 ラウンドキャンセル伝播
    パッチシリーズは、同じパターンが 8 ディスパッチエントリポイント ×
    2 言語を通じてスレッド化されなければならなかったために発生。レガシー
    チェーンを残すことは M-of-N 省略バグを次の横断的関心事 (deadline /
    trace_id / メトリクスハンドル / 可観測性トレーシング) に利用可能に
    保つ。
  - **`state.run_cancel_token_` 非チャネルセットメンバ** が明示的に転送
    されない限りすべてのマルチ Send ファンアウトでドロップ。ここに追加
    された新しい実行毎フィールドは v0.3.1 ポインタドロップバグを繰り返す。
  - **ドキュメント内の 2 つの API 表面** — 新人はソースを読まずに
    `execute` vs `run` を区別できない。`ee11ed6` 新人スイープの 5 トラップ
    はまさにこのドキュメントギャップ形状だった。
  - **SOVERSION がきれいに導入されない。** ディストロパッケージャ
    (Debian, Homebrew, conda-forge) は SOVERSION のないライブラリを
    上流管理不備と扱う。
  - **警告疲れ。** 恒久的な非推奨警告がユーザーを無視するよう訓練し、
    次の実際の非推奨が埋もれる。

これらは今日 v0.4.0 を破損しない。将来のすべての進化をより遅くバグがちに
する。v1.0 の「単一正規の方法」という約束がこれら 5 つすべてに一度に答える。

## PR 毎契約

各 PR は以下を満たすこと:

  - **マージ時に ctest 442/442 + pytest 96/96 を破損しない** (ビルド内の
    非推奨警告は許可、エラーは不可)。
  - **ベンチを回帰させない** (`bench_neograph` seq パス上の中央値 µs/iter、
    `feedback_wsl2_bench_isolation.md` に従って測定 — 新鮮な worktree、
    taskset+chrt)。
  - **最大で次のいずれかに触れる**: ヘッダ表面 OR エンジン内部 OR
    バインディング OR サンプル。混在 PR はレビューを困難にし復帰を
    高コストに。
  - **マージ時にこのテーブルに行を追加** — 提案行を打ち消し線に、マージ
    コミットにリンク、スコープドリフトがあれば注記。

## パフォーマンス回顧 — `b59444f` 18 日潜在 par 回帰

v1.0 サイクルの終わり近く、README の「エンジンオーバーヘッド」自慢
(par 11.8 µs) が破損していることが明らかに。測定 + 並列 bisect 結果:
単一コミット `b59444f` が 18 日間 (2026-04-26 → 2026-05-13) 潜在して
いた回帰だった。

### 何が起きたか

- `b59444f` が `GraphEngine::compile()` のデフォルトワーカー数を
  `1` から `std::thread::hardware_concurrency()` に変更。意図: ファンアウト
  ノードが明示的設定なしで実際の並列実行を受け取る。
- 副作用: 1 ノード逐次 + 5 ノードファンアウトマイクロベンチが反復あたり
  **~75 µs/iter の追加スレッド間サブミットコスト** を拾った。11.8 µs →
  283 µs (24×)。
- 2026-04-27 の perf 監査 (`project_perf_audit_2026-04-27.md`) は
  `fd60aab` を「修正」として記録しているが、それは別の回帰 (タイミング
  測定パターン) でありワーカー数デフォルトを変更せずに残した。par
  マイクロベンチ自体が "default=hardware_concurrency" モードで測定されて
  いたため数値的に正常に見えたが、README の実際の 11.8 µs 主張は
  pre-`b59444f` の値だった。
- v1.0 サイクルの PR 毎契約は「ベンチを回帰させない」ことを要求するが、
  当時のベンチは同じ (回帰した) ベースラインに対して測定されていたため、
  ±5% バンド内に収まり暗黙にパス。18 日間潜在。
- 2026-05-13、コミット毎並列 bisect (`git worktree add` で 11 worktree 並列、
  taskset+chrt 測定) が par 11.8 µs → 283 µs ジャンプの責任を負う単一
  コミットとして `b59444f` を確認。復帰 (`e5ecb08`) が 11.8 → 12.2 µs
  に復元。

### トレードオフ — default=1 が正しい理由

`asio::thread_pool` スレッド間サブミットはタスクあたり約 75 µs のコスト。
単一ノードの実行時間が ms 単位 (LLM 呼出、HTTP 等) なら、そのコストは
ノイズに消える — しかし NeoGraph の称賛される「エンジンオーバーヘッド
直列/並列 µs スケール」パス上では同じ桁数であり直接現れる。

- **CPU 微小 / 逐次ノード (マイクロベンチ、バリデータチェーン等)** —
  default=1 が圧倒的に良い。ワーカープールなしの単一 io_context スレッド上で
  逐次。
- **真のファンアウト意図 (sleep-bound sims、別プロセス呼出、同期 HTTP)** —
  ユーザーは明示的に `engine->set_worker_count_auto()` または
  `set_worker_count(N)` を呼び出す必要がある。1 行。

このトレードオフを明示的にするため、`e5ecb08` のコミットメッセージ +
以下のファンアウトサンプル 5 サイト (10/14/21/36 +
`deep_research_graph` ビルダー) が明示的 `set_worker_count_auto()` 呼出を
追加し、移行ドキュメントの Migration 3 セクションが強化された。

### PR 毎契約の補強 (次の回帰を防ぐ)

`bench_neograph par` マイクロベンチがベースラインの ±5% 以内であることの
確認だけでは不十分だった — ベースライン自体が回帰していると、一緒に
スライドダウンする。フォローアップパッチで:

  -  ベンチ回帰 CI が **README 記載の絶対値** (`seq ≈ 5.0 µs`、
    `par ≈ 12 µs` 等) を第 2 の wall-time-anchor ゲートとして使用。
    ベースライン自体の回帰を捕捉。
  -  または GitHub Actions cron を master → master 7 日回帰測定用に追加
    (nightly-soak スタイルパターン)。
  -  PR 毎 diff が `GraphEngine::compile()` または `set_worker_count` に
    触れる場合、PR 本文に「別途マイクロベンチ測定結果」を含めなければ
    ならない (CODEOWNERS フックで自動化)。

3 つすべてがフォローアップ作業。v1.0 リリース前に少なくとも 1 つが着地
しなければならない。

### 学んだこと

1. **「デフォルト値変更」は機能的意味がなくてもパフォーマンス上重要な契約に
   なりうる。** README の自慢数値が「デフォルト」パスから来ているなら、
   デフォルト変更 = README 変更。
2. **回帰測定のベースライン自体が回帰しうる。** ±band 比較だけをするな。
   絶対値アンカーも設定せよ。
3. **並列 bisect (11 並列 worktree + 結果集約) が 18 日潜在回帰を 30 分で
   特定。** 線形 bisect よりはるかに速い — master が長く成長したときの
   デフォルトツール。

## リファクタ中に避けるべき v0.3.x トラップの記憶

ビルド/リリースパイプラインは v0.1.x → v0.3.x から地雷を蓄積している。
各々がメモリエントリを持つ — このテーブルが関連領域に触れる際のチェックリスト:

| トラップ | 噛み付く場所 | メモリエントリ |
|---|---|---|
| すべての公開クラス + 自由関数に `NEOGRAPH_API` マクロ | 新しいエンジンサブライブラリ (postgres / sqlite / mcp / a2a / acp)。Windows DLL 境界。 | `feedback_neograph_api_discipline.md` |
| ブランチ間 stale .so 汚染 | ブランチ間で使用される `BUILD_SHARED_LIBS=ON` build/ → compile() での ABI 不一致 SEGV | `feedback_cross_branch_stale_so_trap.md` |
| ベンチ測定時のビルドディレクトリ汚染 | 長寿命 build/ dirs が新鮮な worktree ビルドより遅いバイナリを生成 (+0.4 µs/iter false signal) | `feedback_bench_build_dir_contamination.md` |
| WSL2 測定ジッタ | プレーンな「多数反復 + 中央値」は収束しない — taskset + chrt FIFO 99 が必要 | `feedback_wsl2_bench_isolation.md` |
| Doxygen `/*` ワイルドカード in comments | `/**` 内の `fs/*` / `terminal/*` がネストされたコメントを開き後続の診断を抑制。`&#42;` HTML エンティティを使用。 | `feedback_doxygen_slash_star_trap.md` |
| ASan `__cxa_throw` インターセプタ CHECK | pybind 境界を越える C++ 例外が `LD_PRELOAD libasan.so` 下でインターセプタを発動。CI でキーワードにより選択解除。キャンセル/throw 正しさは TSan + ライブ LLM テストで行使。 | (this session — add note in feedback) |
| TSan eptr ライフタイム競合 | NodeInterrupt の exception_ptr が co_await 境界を越えると libstdc++ `__exception_ptr::_M_release` を発動。修正: 理由を `std::string` として抽出しメインスレッドで新規スロー。 | `feedback_parallel_group_eptr_race.md` |
| MSVC は明示的 `<array>` / `<algorithm>` が必要 | libstdc++ はこれらを推移的に引き込む。MSVC v143 は引き込まない。`std::array` 等を使用するテストファイルが Windows CI を暗黙に破損。 | (this session — add note in feedback) |
| scikit-build-core 0.12.2 Windows single_config | `-G` フラグが検出され、env-var が無視 — Windows wheel が SQLite=OFF 上書きを失う。`[[tool.scikit-build.overrides]]` + `cmake.define` を使用。 | `feedback_libcurl_unconditional_dep.md` |
| Wheel OpenSSL CA パス | manylinux libssl が Ubuntu 上に存在しない AlmaLinux パスを使用。`__init__.py` が certifi から `SSL_CERT_FILE` を自動設定。 | `feedback_wheel_openssl_ca.md` |
| pyproject.toml ランタイム依存が CI の PYTHONPATH フローで自動インストールされない | `pip install --quiet pytest` 行が pyproject.toml の `dependencies = [...]` をミラーしなければならない。v0.3.2 で pydantic についてこれを失った。 | (this session — add note in feedback) |
| `compile()` デフォルトワーカー数回帰 | `b59444f` がデフォルトを `1 → hardware_concurrency` に変更、潜在 par マイクロベンチ 11.8 → 283 µs (24×)。ベースライン自体が回帰するパターン。修正は `e5ecb08`。 | "Perf retrospective" セクション (上記) |

リファクタ PR が新しいサブライブラリ、新しい公開クラス、新しい
ランタイム依存、pybind を越えて throw する新しいテストパターン、新しい
wheel プラットフォームを追加する場合 — まずこのテーブルを開くこと。
v0.3.x パッチシリーズの半分は既にこのリストにある項目の再発見だった。

## ドキュメント影響マップ

リファクタが着地したとき、これらのページが編集を必要とする:

  - **`README.md`** — "Python Binding" セクションの RunConfig テーブル、
    "Differences from LangGraph" デルタ (ほとんどのエントリが時代遅れに
    なり編集ではなく削除されるべき)、"What's covered by the binding"
    表面リスト。
  - **`docs/reference-en.md`** (1622 行) — GraphNode / Node / Provider /
    CancelToken / RunConfig セクション。約 30-40% 書き直し。
    ナラティブツアー構造は残る。API 表面は縮小。
  - **`docs/concepts.md`** — 531 行の概念的ナラティブ。"8 dispatch entry
    points" 段落が 1 つに集約。キャンセル伝播段落がクリーンアップ。
  - **`docs/troubleshooting.md`** — ほとんどの v0.3.x エントリが時代遅れに。
    "silent no-op" / "forgot to override" / "thread_local missing" エントリは
    削除可能。
  - **`bindings/python/examples/`** — すべてのサンプル (22 Python + 30 C++)
    が更新。
  - **`Doxyfile`** — 変更なし。PROJECT_NUMBER が pyproject.toml から読み取る
    ため v1.0.0 が自動伝播。
  - **`ROADMAP_v1.md`** (本ファイル) — 着地した候補を打ち消し線に、
    予想より難しかった/簡単だったことの事後分析を追加。

## v1.0 の完了定義

  1. 次の各々を行う単一の正規の方法: ノード作成、実行のキャンセル、
     状態の読み取り、状態の更新、グラフの実行。
  2. README の "Python Binding" セクションが新規ユーザーに 5 分未満で読める。
  3. `docs/reference-en.md` GraphNode セクションが 8 つではなく 1 メソッド。
  4. v0.3.x 非推奨警告が最終削除前に少なくとも 1 リリースの間静かだった。
  5. ctest / pytest / ライブ LLM / Valgrind / Doxygen すべてが v1.0 タグで
     グリーン。

---

# リサーチトラック (上記 v1.0 研ぎ澄ましより重要度が低い)

## Candidate 4 — 自己進化 JSON エージェント v2 (リサーチ)

### コンテキスト

`bindings/python/examples/22_self_evolving_graph.py` がループが閉じることを
証明: LLM 修正子が実行中グラフへの JSON 編集を提案し、エンジンが再コンパイルし、
新しいグラフが実行される。PoC は動作するが、LLM が編集提案時にチャネル
データフローについて推論するのに苦労 — どのノードがどのチャネルを
読み取り/書き込みするかを「見て」いないため、その提案が頻繁に間違った
ワイヤを通じてデータをルーティングする。

### リサーチ方向

チャネルトポロジを修正子プロンプトに明示的に公開。調査すべき 2 つの形式:

1. **プロンプト内のトポロジサマリ** — エンジンがコンパイルされた
   チャネルアクセスパターンから導出された ``"node X reads {a,b}, writes {c}"``
   のようなノード毎仕様を出力。修正子プロンプトが JSON 定義と共に
   これを受け取る。

2. **ステージ毎チャネル提案** — 修正子がフラットなセットではなく
   *ステージ毎* (split / synthesize / etc.) にチャネルを提案。エンジンが
   各提案ステージのチャネルセットが上流/下流ステージと一貫していることを
   構成チェック。

### なぜ v1.0 必須でないか

- 出荷済みエンジンのユーザーブロッカーではない — PoC のギャップは
  *プロンプトエンジニアリング*にあり、エンジンにはない。
- エンジン側表面変更が正当化される前に LLM 評価ハーネス (トポロジバリアント
  毎の正解率、コスト、収束までの編集サイクル) が必要。
- 評価が LLM が実際に使用するイントロスペクションを示せば、より広い
  「グラフイントロスペクション API」v1.x 機能に折り畳まれる可能性。

### コスト

- リサーチが検証すればエンジン表面追加 (トポロジアクセサ) は小さい。
- 作業のほとんどはこのリポジトリのホットパス外の LLM 側実験。

### トリガーラウンド

TODO_v0.3.md item #8 — ユーザーブロッカーではなくリサーチとして v0.3.x から
延期。

## Candidate 5 — クックブックトラック: pgvector RAG サンプル

### コンテキスト

`bindings/python/examples/` (23 サンプル) は ReAct、HITL、インテント
ルーティング、マルチエージェント討論、深層リサーチ (web crawl / web search)、
自己進化グラフ等をカバー — しかし **ベクトル検索 / RAG** サンプルがない。
16/17 に折り畳まれていないことを確認: それらは埋め込みベースの検索ではなく
Web リサーチ。

RAG は最も一般的な LLM パターンの 1 つ。欠如は NeoGraph を評価する
ユーザーにとって実際の発見可能性ギャップ。

### なぜエンジン関心事ではなくクックブックエントリか

エンジン表面は現状で十分 — `PostgresCheckpointStore` が既に接続プール/設定
ストーリーを持ち、埋め込み + pgvector ノードが再利用可能。エンジン API
追加は不要。作業は純粋な動作サンプル (~150-200 行):

  - `EmbeddingNode` — OpenAI 埋め込みまたはローカルモデルを呼び出す。
  - `RetrieveNode` — 事前投入テーブルに対する pgvector 類似度クエリ。
  - `RAGCallNode` — 検索されたコンテキストを連結した LLM 呼出。
  - ワンタイムインデックス設定スクリプト (サンプル本体とは別で、
    サンプルが毎回再インデックスしないように)。

### なぜ v0.3.x から延期されたか

v0.3.x シリーズは FastAPI SSE チャットデモフィードバックによって露呈した
エンジンのバグ/エルゴノミクスを範囲としていた。RAG はエンジンのバグでは
ない。「一般的なパターンに動作サンプルが必要」。各エントリが実際の
レシピである別のクックブックドラムビートに属し、v-bump ではない。

### トリガーラウンド

TODO_v0.3.md item #9 — クックブック素材として確認 (エンジンギャップなし)、
将来の「サンプルトラック」スイープに延期。

---

## Candidate 6 — Provider 単一ディスパッチ

### 症状

`Provider` は 4 つの仮想メソッドを公開 (GraphNode の 8 つより 1 次元小さい):

```
complete           complete_async
complete_stream    complete_stream_async
```

`(sync/async) × (stream/non-stream)` — Candidate 1 と同じ形状、
同じ「N のうち少なくとも 1 つをオーバーライド」契約、同じトラップ。
非ストリーミングペアは安全 (1 ブリッジステップ深さ)。ストリーミングペアは
#10 以前は安全でなかった: `complete_stream` は同期 httplib、デフォルト
`complete_stream_async` ブリッジがそれをインラインでラップ (そして
WebSocket Responses パスがエンジンの io_context ワーカーの上に `run_sync`
をネスト)、`GraphEngine::run_stream_async` 内から呼ばれたときに断続的
segfault を生成 (issue #4、PR #10 のワーカースレッドブリッジ +
`SchemaProvider` ネイティブオーバーライドにより修正)。

### なぜこれはブロッカーではなくクリーンアップか

具体的なクラッシュ (#4) はクローズ — PR #10 が同期 `complete_stream` 用に
専用ワーカースレッドを生成し、トークンを awaiter のエグゼキュータに
ディスパッチバック。`SchemaProvider` が WS パスに対してワーカースレッド
さえもスキップするようオーバーライド。`OpenAIProvider` と `SchemaProvider`
HTTP/SSE パスは安全なデフォルトを継承するため、4 仮想直積はもはや
クラッシュしがちではない。残るのはアーキテクチャ上のイボ: オーバーライド
表面が必要以上に広く、ブリッジの安全性が直積のどの隅をオーバーライドするか
に依存する (コンパイル時に何も固定しない不変条件)。

### 着地方向

1 オーバーライドパスは置換ではなく追加的:

```cpp
class CompletionProvider : public Provider {
public:
    asio::awaitable<ChatCompletion>
    invoke_request(CompletionRequest request);

protected:
    virtual asio::awaitable<ChatCompletion>
    do_invoke(CompletionRequest request) = 0;
};
```

`CompletionRequest` がコールバックの存在から collect モードまたは stream
モードを明示的に選択し、トランスポートをコールバック存在から推論しない。
Final アダプタが既存のすべての `Provider` エントリポイントを保持。
これにより新しい実装に 1 つのオーバーライドを与えつつ、既存のソースと
バイナリ契約を無傷のままにする。

### 隣接 — `schema_provider.cpp` 分割

`schema_provider.cpp` はマルチベンダースキーママッピング + HTTP/SSE ワイヤ +
body-build + 応答解析を集中させる ~1,800 LoC。単一ディスパッチ書き直しは
`SchemaParser` / `SchemaWireBuilder` / `SchemaProviderImpl` に分割する
自然な瞬間。別の ROADMAP エントリを必要としないようここに言及。作業が
異なる PR で発生する場合は分割可能。

### トリガーラウンド

Issue #5 — #4 のデバッグ中に表面化。具体的なクラッシュは PR #10 で
クローズ。アーキテクチャ上のクリーンアップは追加的 `CompletionProvider`
パスと恒久的互換性ポリシーを通じて着地。

### 着地ログ (v0.9.0 候補サイクル)

5 PR が master に順次着地、2026-05-13 中盤:

| PR | 範囲 | 着地先 |
|---|---|---|
| **#40 (PR1)** | 新しい仮想メソッド `Provider::invoke(params, on_chunk = nullptr)` を追加。デフォルト実装が 4 レガシー仮想チェーンに転送 (既存の全 Provider サブクラスは動作変更なし)。6 新 ctest。 | v0.9.0 |
| **#41+#42 (PR2)** | エンジンビルトイン LLM ノード (`LLMCallNode`、`IntentClassifierNode`) が `provider->invoke(params, on_chunk)` 経由でディスパッチ。PR #41 は積み上げベースにのみマージされ、その後 PR #42 経由で master に再適用。 | v0.9.0 |
| **#43 (PR3)** | すべてのエンジン内部同期 LLM 呼出サイトを移行 — `agent.cpp` (5 サイト)、`deep_research_graph.cpp` (6 サイト)。`Provider::invoke()` デフォルトにスレッドローカルキャンセル伝播パリティを追加 (レガシー `complete()` の `current_cancel_token()` 動作を再現)。3 新 ctest。 | v0.9.0 |
| **#44 (PR4)** | 全 4 レガシー仮想メソッドに `[[deprecated]]` をマーク。`plan_execute_graph.cpp` の 3 サイトを `invoke()` に移行。`OpenInferenceProvider` と `RateLimitedProvider` (デコレータ) の 4 仮想オーバーライドブロックを `NEOGRAPH_PUSH/POP_IGNORE_DEPRECATED` でラップ — 内部転送警告をブロック。ユーザー向けオーバーライド/呼出サイトのみが警告。 | v0.9.0 |
| **#45 (PR5)** | C++ サンプル移行 (`31_local_transformer.cpp`、`cookbook/ai-assembly/member_server.cpp`)。 | v0.9.0 |

### 追加的互換性パス (2026-07 改訂)

既存の `Provider` vtable を保持し、別の `CompletionProvider` を追加。
新しい実装は明示的な `CompletionRequest` を受け取り `do_invoke()` のみを
オーバーライド。既存の 4 仮想メソッドとコールバックベースの `invoke()` は
final アダプタを通じて新しい実装に配線。既存の Provider サブクラスと
Python トランポリンは現状のまま保持。

既存の仮想メソッドは削除計画のない安定 API として引き続きサポート。
互換性とセキュリティ修正は継続適用されるが、既存の 4 仮想メソッドへの
全新機能のバックポート義務はない。新しい実装と新しい直接呼出はそれぞれ
`do_invoke()` と `invoke_request()` を使用すべき。このポリシーは既存の
公開シグネチャ、仮想メソッド順序、オブジェクトサイズ、vtable を変更
しない。#127 のネイティブ非同期トランスポートと操作所有権はこの API
ポリシーとは別。

フォローアップ:

  - **6b**: 新しい Providers が `CompletionProvider` に対して記述される。
    既存ビルトインの即時継承は ABI 影響のため変更しない。
  - **6c**: ネイティブ非同期トランスポートとリクエスト所有のキャンセル/
    ライフタイムを完了。
  - **隣接**: `schema_provider.cpp` (1800 LoC) を
    `SchemaParser` / `SchemaWireBuilder` / `SchemaProviderImpl` に分割
    (上記 6b と同じ PR または別 — 実装時に決定)。

---

## Candidate 7 — gRPC トランスポート (オプトインコンポーネント)

### コンテキスト

HasMCP コールドメール (2026-05-15) はトリガーではないが、そのメールは
「gRPC が次のトランスポート方向」という無料の業界シグナルを与えた。
MCP コミュニティでは [gRPC を標準トランスポートとして追加すること](https://github.com/modelcontextprotocol/modelcontextprotocol/issues/966) が議論されており、
Google は gRPC-as-native-MCP-transport に取り組んでいる。gRPC は
NeoGraph の 4 軸ナラティブのほぼすべての軸に整合 — protobuf バイナリ
シリアライゼーション (パフォーマンス / 軽量)、HTTP/2 多重化 (マルチ
テナント接続コスト)、ネイティブ双方向ストリーミング (トークン / イベント)、
スキーマ強制 + 小ワイヤ (組み込み)。

### 判断 (2026-05-15)

- **社内実装なし。** 通信プロトコルは大幅な車輪の再発明リスクを伴う —
  標準の `grpc++` + `protoc` を使用。
- **オプトインのみ。** `NEOGRAPH_BUILD_GRPC` オプション、**デフォルト OFF**。
  grpc++ は protobuf + abseil + c-ares + re2 + zlib (数十 MB 推移的) を
  引き込み、「2 deps / libc.so.6 only / 1.2 MB binary」軽量軸を破壊。
  デフォルト OFF がそれを止める唯一の方法であり、
  `cmake-option-default-flip-trap` (EDDSkills、このセッションで新規追加)
  の規律を適用: `find_package(Protobuf/gRPC)` はオプションゲート内のみ。
  デフォルト反転なし。
- **NeoGraph ネイティブ API、MCP 標準から独立。**
  `proto/neograph.proto` = `GraphService { RunGraph(unary) /
  RunGraphStream(server-stream) / Health }`。ペイロードは JSON 文字列
  (graph-as-data プロパティを保持 — 強く型付けされた proto メッセージで
  モデル化するとユーザーグラフ変更ごとに .proto を再生成することになる)。
  MCP-over-gRPC 標準が確定したら、このサービスの隣に MCP 形状のサービスを
  追加 (このサービスは変更なし)。

### 着地 (v0.9.x サイクル、スキャフォールド)

- `NEOGRAPH_BUILD_GRPC=OFF` オプション + 条件付き `find_package` +
  fatal guard。
- `proto/neograph.proto`、`src/grpc/graph_service.cpp` (ハッシュキー付き
  コンパイルキャッシュ — multi_tenant_chatbot クックブックパターンを再利用)、
  `include/neograph/grpc/graph_service.h` (`NEOGRAPH_HAVE_GRPC` ガード)、
  `examples/52_grpc_server.cpp`。

### 検証 — 最初の grpc++ 装備ビルド (2026-05-16)

apt `libgrpc++-dev protobuf-compiler-grpc` (1.51.1) + protoc 3.21.12 を
インストール後、`-DNEOGRAPH_BUILD_GRPC=ON` でビルドしエンドツーエンドパス:
  - `neograph_grpc` / `example_grpc_server` / `example_grpc_client`
    すべてがコンパイル・リンク OK。
  - C++ client → server: **Health** (ok/version/default_graph)、
    **RunGraph** unary (`{"text":"hello from grpc"}` →
    `"HELLO FROM GRPC"`, trace=[upper])、**RunGraphStream**
    (5 events, FINAL payload, status OK)。`RESULT: PASS (failures=0)`。
  - protoc コード生成パス (生 `add_custom_command`) が動作。**1 バグ修正**:
    VERBATIM モードで `ARGS --proto_path="${dir}"` の引用符が文字通り
    渡され、protoc が ``"…/proto"`` (引用符を含む) をディレクトリとして
    見る → "directory does not exist"。引用符の除去 (`--proto_path=${dir}`)
    でクローズ。

### WSL Windows-PATH リークトラップ (再現可能 — ビルド環境警告)

この環境 (WSL2、大規模 Windows PATH リーク) で grpc++ ON でビルドした際に
2 つの汚染が捕捉された。クリーンな Linux ホスト / CI では現れないが、
WSL 開発者はこれらに当たる:

  1. **anaconda re2** — `gRPCConfig.cmake` が `find_package(re2)` を行う際、
     システム re2 cmake config が存在しない場合 (apt `libre2-dev` が
     インストールされていない)、PATH から `/mnt/c/ProgramData/anaconda3/
     Library/lib/cmake/re2/re2Targets.cmake` (Windows) を拾い
     `set_target_properties` でエラー。修正:
     `-DCMAKE_IGNORE_PREFIX_PATH=/mnt/c;…` +
     `-DCMAKE_IGNORE_PATH=…/anaconda3/Library/lib/cmake;…` → grpc が
     システム pkg-config re2 にフォールバック ("Found RE2 via pkg-config")。
  2. **ZLIB include** — `FindZLIB` がライブラリはシステム
     (`/usr/lib/.../libz.so`) から拾うが `ZLIB_INCLUDE_DIR` は PATH 内の
     `/mnt/c/gtk/include` (Windows zlib.h) から → `-isystem
     /mnt/c/gtk/include` がすべての grpc リンクターゲットにリーク →
     `/mnt/c/gtk/include/libintl.h` が `printf` を `libintl_printf` マクロ
     として書き換え → `std::printf` コンパイルエラー。修正: 明示的に
     `-DZLIB_INCLUDE_DIR=/usr/include
     -DZLIB_LIBRARY=/usr/lib/x86_64-linux-gnu/libz.so` を設定。

  → 両方とも `cmake-option-default-flip-trap` のいとこ (環境リークが
  `find_package` を間違ったプレフィックスに引きずる)。EDDSkills SKILL
  `wsl-windows-path-cmake-find-leak` を追加 (2026-05-16)。

### NexaGraph 先行分析 — gRPC-MCP の真の ROI はチェックポイント

NeoGraph の前身 NexaGraph (`/root/Coding/NexaGraph`) は gRPC-MCP を
既に早期に実装・運用していた。調査所見 (Explore, 2026-05-16):

- **実装実体**: `proto/rag_service.proto` (RAGService, 11 unary RPC —
  vector_search / graph_search / ingest / chat history / image task /
  **graph checkpoint** 5 RPC)、`src/nexagraph/grpc_client.cpp` が完全実装、
  api_server.cpp から `GRPC_TARGET` env 経由で本番統合。サーバーは
  デュアルトランスポート (HTTP JSON-RPC + gRPC 50051)。ストリーミングなし
  (全 unary)。
- **オーバーヘッド削減主張** (`DOCS/grpc-client-plan.md`):
  シリアライゼーション 1ms→0.01ms、埋め込み 1536d 15KB→6KB、
  リクエスト毎新規接続 → HTTP/2 多重化。**測定なし — 設計根拠のみ。**
- **正直な評価**: 典型的な MCP ツール呼出では、LLM 推論 (数百 ms) が支配的
  なため、1ms シリアライゼーション節約はノイズ。gRPC の利得が*実際に存在する*
  領域は **大規模構造化ペイロード** — 埋め込みベクトル、RAG インジェスト、
  特に **グラフチェックポイント** (各ステップで `channel_values_json` +
  `channel_versions_json` が大規模化)。小さなツールメタデータ/文字列クエリは
  <1% (認知複雑性に見合わない)。言い換えれば「MCP 全般が高速」ではなく、
  「大規模ペイロード MCP」に限定。

**主要所見 — NeoGraph 導入時の優先順位付け直し:**

1. **gRPC CheckpointStore = 真の ROI (最優先候補)。**
   NexaGraph の `grpc_checkpoint.cpp` は既に
   **`neograph::graph::CheckpointStore` から継承** — その時点から NeoGraph の
   チェックポイント抽象を使用していた。言い換えれば、
   NeoGraph の `Postgres/Sqlite CheckpointStore` の隣に `GrpcCheckpointStore`
   を追加する形でほぼドロップイン移植 (~150 LoC)。大ペイロード +
   (MCP #966) 標準から独立 + 構築されたばかりの `neograph::grpc`
   コンポーネント内部に自然に収まる。チェックポイントは各ステップで
   大きな JSON ブロブであり、gRPC バイナリ利得が実際に測定可能な唯一の
   ホットパス。
2. **MCP-over-gRPC トランスポート (一般ツール呼出) = 低優先度。**
   LLM 支配的なため利得は小さく + MCP-over-gRPC 標準がまだ確定していない
   (#966)。標準確定後も、RAG / 埋め込みのような大規模ペイロードツールの
   場合のみ。

### GrpcCheckpointStore — 着地 + 測定 (2026-05-16)

`neograph::grpc` に追加: `GrpcCheckpointStore` (クライアント、
`CheckpointStore` から継承 — NexaGraph と同じ) +
`CheckpointServiceImpl`+`run_checkpoint_server` (サーバー、任意の
`CheckpointStore` バックエンドをラップ) + `checkpoint_to/from_json`
ヘルパー。`CheckpointService` proto に 5 RPC。NexaGraph のフラット
マッピングでは処理できなかった NeoGraph のリッチフィールド (next_nodes
vector / `CheckpointPhase` enum / `barrier_state` nested map /
`schema_version`) の往復保持 — サンプル 54 正しさ PASS。

**測定結果 (example_grpc_checkpoint, 1536-d 埋め込み + 12 ターン、200 iters、
localhost loopback) — 「もっともらしいが未証明」をクローズ。正直に言って
半分は却下:**

| メトリック | 値 |
|---|---|
| JSON (checkpoint_json) | 29 080 B |
| Protobuf wire (CheckpointBlob) | 29 131 B |
| 想定 JSON-RPC エンベロープ | 29 155 B |
| protobuf / JSON-RPC payload | **99.9%** |
| InMemory in-process | save 27 µs / load 36 µs |
| gRPC round-trip | save 720 µs / load 755 µs |
| gRPC ネットワークオーバーヘッド | **+693 µs save / +719 µs load** |

**正直な結論 — NexaGraph の「シリアライゼーション 15KB→6KB バイナリ圧縮」
主張は NeoGraph の JSON-in-proto 設計では未達成 (ペイロード 99.9% 同一)。**
理由: graph-as-data の堅牢性のため、チェックポイント全体が単一の proto
string フィールドとしてパック → protobuf フィールドレベル圧縮が適用されない。
NexaGraph はフィールド毎メンバ proto を持っていたため圧縮されたが、
すべてのチェックポイント形式ドリフト (`next_nodes` / `barrier_state` /
`schema_version` の追加ごと) が proto 再生成を要求。言い換えれば、
**このトレードオフで圧縮よりスキーマ安定性を意図的に選択したため、
ペイロード利得は本当に 0 (証明済み: 設計上利益なし)。**

gRPC の*実際の*利得はトランスポートのみ — HTTP/2 接続再利用
(JSON-RPC / HTTP1.1 の呼出毎接続を排除)。単一の loopback 往復 +700 µs は
これを示さない (デルタは負荷下 / リモート RTT でのみ現れる)。言い換えれば、
**トランスポート利得は依然として負荷テスト依存 — 単一測定では証明できない。**

→ 優先順位再確認:
  - **GrpcCheckpointStore の真の価値 =「型付き RPC によるリモート
    チェックポイント + HTTP/2 接続再利用」+「多言語: 任意の言語サーバーが
    `CheckpointService` を実装可能」**。NexaGraph が宣伝したペイロード
    圧縮ではない。クックブックとして出荷するが、正直なセールスポイントは
    「圧縮」ではなく「型付きリモートチェックポイント、エージェント
    プロセスに DB ドライバゼロ」。
  - **MCP-over-gRPC トランスポート (一般ツール呼出) = 保留。** JSON-in-proto
    ではチェックポイント測定で確認された通りペイロード圧縮が適用されない
    ため、ツール呼出が同じ設計に従うなら圧縮利得は 0 + LLM 支配的。
    大きなバイナリツール (生埋め込み等) でのみ再考。標準 (#966) が確定し
    フィールド毎メンバが正当化される場合。
  - 残る検証: 負荷下 (N 同時チェックポイント保存) で HTTP/2 多重化が
    呼出毎接続に対して実際にデルタを生むか — ベンチジョブ候補 (持続的、
    単発ではない)。

### ToolCalling: JSON-RPC vs gRPC ヘッドツーヘッド (2026-05-16)

ユーザーリクエスト — チェックポイントではなく、*ツール呼出*自体、
両トランスポートに対して実サーバー上でヘッドツーヘッド。`proto` が
`ToolService.CallTool` を取得、サンプル 55 が **両方で同じ compute fn を
起動 (a) httplib JSON-RPC 2.0 `tools/call` (MCP 形状、HTTP/1.1 keep-alive)
(b) gRPC ToolService (HTTP/2)** し同じものを測定。

**正直さインシデント — 「gRPC 70x faster」は測定アーティファクトだった。**
初回実行: JSON-RPC p50 43 ms (ペイロードに関わらず定数)。43 ms =
TCP delayed-ACK タイマーの教科書的シグネチャ。原因: `CPPHTTPLIB_TCP_NODELAY`
デフォルト **false** → Nagle on、gRPC は TCP_NODELAY デフォルト on →
不公平。「gRPC 70x」をそのままコミットするのは嘘だっただろう。
両側に `Server/Client::set_tcp_nodelay(true)` を適用し再測定。

**公正条件結果 (loopback、両側 keep-alive + NODELAY、N=300 p50、2 再現):**

| ペイロード | gRPC p50 | JSON-RPC p50 | 比 |
|---|---|---|---|
| tiny args (~30 B) | 433 / 448 µs | 436 / 410 µs | **0.99–1.09× (引き分け)** |
| 1536-float (~12 KB) | 655 / 680 µs | 1079 / 1016 µs | **0.61–0.67× (gRPC ~1.5×)** |
| args wire (tiny) | 42 B | 118 B | エンベロープオーバーヘッド |
| args wire (12 KB) | 12025 B | 12100 B | **99% (圧縮 0)** |

**真実:**
- **小さなツール呼出 (実際のツール呼出の大多数): JSON-RPC ≈ gRPC 引き分け。**
  トランスポート切替 ROI ≈ 0。
- **大規模ペイロードツール呼出 (~12 KB+, 埋め込み / RAG チャンク返却):
  gRPC ~1.5×。** NexaGraph が言及した領域だが、70× ではなく 1.5×。
- ペイロード圧縮は依然 0 (JSON-in-proto、チェックポイント測定と一貫)。
- Loopback 天井 — 実ネットワークでは RTT が両側に等しく加算され比は
  さらに 1 に収束。1.5× は最良ケース。

**Candidate 7 最終評決:**
- gRPC の ROI は (1) **多言語サイドカー / リモート型付き RPC** (言語境界)、
  (2) **大規模ペイロードツール / チェックポイントで ~1.5×**。一般ツール
  呼出の大量移行は無価値 (引き分け + 標準 #966 未確定)。
- MCP-over-gRPC トランスポート: **保留、確認済み。**「一般 MCP ツール呼出が
  高速化」は測定で反証 (引き分け)。標準確定後、かつ埋め込み多用ツールの
  場合のみ。
- Nagle インシデント → EDDSkills SKILL 候補 `bench-shock-number-nagle-first`
  (衝撃的なトランスポートベンチ数値 = TCP_NODELAY / Nagle / delayed-ACK を
  まず疑え。`perf-regression-bench-bisect` のいとこ)。ユーザー承認後に追加。

### NeoGraph JSON-RPC が gRPC と引き分ける理由 — yyjson (証明済み)

ユーザー洞察: 「JSON-RPC は yyjson でパースするから速い。構造的に gRPC が
勝たなければならない。」サンプル 55 にトランスポート除去された純粋コーデック
マイクロベンチを追加して検証:

| 12 KB payload, codec only, 5000 iters | µs |
|---|---|
| yyjson parse+dump | **38.9** |
| protobuf ser+parse | **1.75** |
| → yyjson / protobuf | **22.3× 遅い** |

**ユーザーは完全に正しい。** protobuf は構造的に 22× 高速なコーデック。
しかし往復では 12 KB で差が 1.5× に希釈 — シリアライゼーションギャップ
~37 µs は完全往復 692–1096 µs の小さな断片 (残りはソケット I/O / syscall /
HTTP フレーミング)。**ツール呼出ホットパスがコーデックではなくソケット
I/O に支配されていることの定量的証拠。**

重要な含意 — **NeoGraph の JSON-RPC が gRPC と引き分けるのは yyjson の
おかげであり、JSON-RPC プロトコルが速いからではない。** 典型的なスタック
(Python の `json` は yyjson より ~50× 遅く、12 KB で ~2 ms) では、
コーデックが往復を支配 → そこでは gRPC が構造的に支配する。NeoGraph だけが
yyjson を使用し、したがってそのトラップを回避する。

→ これは隠れたセールスポイントであり、Candidate 7 を保留する*最終的*正当化:
「他のフレームワークは JSON パースがボトルネックなので gRPC トランスポートが
重要だが、NeoGraph の MCP / JSON-RPC は yyjson のためそうではない。」
特に NeoGraph にとって、MCP-over-gRPC はさらに ROI が低い (コーデック
アドバンテージが既に yyjson で相殺されている)。gRPC は多言語/リモート境界 +
大規模ペイロード目的で ~1.5× のみ — 確認済み。

### NexaGraph 第 2 収穫 — 履歴圧縮 + GrpcRemoteTool (2026-05-16)

完全な NexaGraph 調査の後、既に移植された `GrpcCheckpointStore` に加えて、
3 つの追加の*汎用的、非重複、NeoGraph にまだない*項目をさらに移植。
(RAG アプリ固有の stdio / HTTP MCP in `proto/rag_mcp_server/backend` は
NeoGraph が既に持つかアプリ固有のため除外。`DOCS/graph-engine-design.md` は
実際 NeoGraph の設計祖先であり、「移植」対象ではない。)

1. **`neograph::history` (新コアユーティリティ、追加的)** — NexaGraph の
   CAF `compress_history` アクターからアクターシェルを剥いだコアのみを移植:
   - `compact_history(messages, Provider&, model, max_tokens=12000,
     recent_keep=6) -> awaitable<CompactedHistory>` — トークン推定が
     予算を超える場合、(system 1 + last N) の間のセクションを 1 回の
     LLM 呼出で要約し、system-summary メッセージに置換。
     `co_await provider.invoke()` (非推奨の `complete()` は使用しない、
     非同期ライブラリ依存ゼロ — コア内部は既にコルーチン使用)。
   - `sanitize_tool_calls(messages&)` — NeoGraph が**完全に欠いていた**防御:
     切り詰めにより破損した OpenAI tool-pairs (応答のない assistant
     `tool_call` / 呼出のない tool メッセージ) の 2 パス除去、冪等。
     `compact_history` が出力に内部適用 → 圧縮結果が決して 400 を
     生成しない。
   - `estimate_tokens` — 保守的な ~3 chars/tok 推定 (混合 KO / EN)。
   - サンプル 56 `history_compaction` (オフライン MockProvider、キー不要) —
     sanitize 3→1、compact 29 msgs/975 tok → 6 msgs/208 tok、
     元の変更なし検証 PASS。`src/core/history.cpp` が全設定で
     `neograph_core` にビルド — 496/497 ctest PASS (1 失敗 =
     既存 `pybind_smoke` openinference モジュール欠落、無関係)。

2. **`neograph::grpc::GrpcRemoteTool`** — サンプル 55 は gRPC 経由でツールを
   *エクスポート*する側 (`run_tool_server`)、これはその鏡像 —
   リモートの `ToolService.CallTool` を通常の `neograph::Tool` として
   *インポート*する側。NexaGraph の `GrpcTool` アダプタを移植。
   pimpl (公開ヘッダは grpc++ フリー、`GrpcCheckpointStore` と同じ姿勢)。
   単純な proto に list-tools RPC がないため、定義は ctor 経由で注入。
   サーバーエラー → `runtime_error` として再スロー (ツールエラー、
   トランスポートエラーではない — ローカル Tool と同じ契約)。
   サンプル 57 `grpc_remote_tool` — サーバースレッド + `Tool&` 多相呼出 +
   エラーパス PASS。**gRPC の ROI #1 (多言語リモート型付き RPC) の
   利用者側実体化** — エージェントの視点から、プロセス境界ツールは
   呼出サイトでローカルツールと区別不能。

### 残件 (依然オープン)

  - CI に `grpc-build` ジョブを追加 (apt deps + ON build +
    `example_grpc_client` / `server` smoke — クリーンな ubuntu runner では
    上記 WSL トラップは適用されない)。
  - `RunGraphStream` の `ServerWriter::Write` がストリーミングノード
    コールバック内で呼ばれる — 現在は単一スーパーステップループスレッドを
    仮定。マルチワーカーファンアウトグラフでコールバックがワーカー
    スレッドで呼び出される場合、`ServerWriter` 同期が必要 (gRPC
    `ServerWriter` はスレッドセーフではない)。現在のサンプルは単一
    ノードのため、これは露呈していない。
  - TLS / auth: `run_server` の安全でないデフォルトの代わりにユーザー
    配線を文書化。
