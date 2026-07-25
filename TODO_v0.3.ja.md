<!-- neograph-i18n: source=TODO_v0.3.md locale=ja source_sha256=ee4fb3a3df1268f70a1cf98004b3825f972ee0f84ad3de77e0887a39e7bb80c5 -->
# v0.3 フォローアップ

**Languages:** [English](TODO_v0.3.md) | [한국어](TODO_v0.3.ko.md) | [日本語](TODO_v0.3.ja.md) | [简体中文](TODO_v0.3.zh-CN.md)

元は FastAPI SSE チャットデモのフィードバックより (2026-05-04)。
v0.3.0 でキャンセル伝播がリリースされました。本ファイルは残るメンタルモデルと
エルゴノミクスのギャップを追跡します。

## ✅ v0.3.1 でクローズ (2026-05-04, セッション 2)

1. **同一 `thread_id` での自動チェックポイント再開** — オプトインの
   `RunConfig.resume_if_exists` が導入されました。True かつチェックポイント
   ストアが設定されている場合、`engine.run/run_async/run_stream` は
   `thread_id` の最新チェックポイントを読み込み、その上にチャネルリデューサー経由で
   `input` を適用します（これにより APPEND リデュースされた `messages` が
   新しいターンとともに成長します）。デフォルト `False` は既存の新規開始動作を維持します。
   テスト: `tests/test_resume_if_exists.cpp` (6) +
   `bindings/python/tests/test_resume_if_exists.py` (6)。
2. **ストリーミング専用ノードのエラーメッセージ改善** —
   `GraphNode.execute()` (Python ベース) がサブクラスの MRO を走査して
   `execute_stream` / `execute_full_stream` を探し、いずれかが定義されている場合、
   `NotImplementedError` に `engine.run_stream() / run_stream_async()` を
   指し示すヒントを含めます。テスト:
   `bindings/python/tests/test_streaming_only_error_hint.py` (4)。
3. **トークン送出ヘルパー** —
   `from neograph_engine.streaming import emit_token` が 4 行の `GraphEvent`
   構築儀式を `emit_token(cb, self._name, token)` に集約します。テスト:
   `bindings/python/tests/test_emit_token_helper.py` (5)。
4. **README「LangGraph との違い」セクション** — Python Binding セクションの
   下に追加。以下の項目を明示: オプトインのマルチターン再開、
   `update_state(channel_writes)` の形状、`get_state` のネストされた辞書、
   Python `Provider.complete` のみ、ストリーミング専用ノードには
   `run_stream*` が必要、新しい `emit_token` ヘルパー。さらに
   `resume_if_exists` を `RunConfig` テーブルに追加。
7. **並列 / Send ブランチのキャンセル伝播** — 静的並列を共有親状態経由で検証
   （v0.3.0 で既に正しい）。マルチ Send のギャップを発見・修正: 動的ファンアウト
   ワーカーが `serialize/restore` から分離された `GraphState` を構築していたが、
   `run_cancel_token_` はチャネルセットの外側に存在しドロップされていた —
   そのためキャンセルされた実行でも Send 分岐でコストが漏洩していた。
   `GraphState::run_cancel_token_shared()` を追加し、
   `NodeExecutor::run_sends_async` が分離された各 `send_state` に転送するように
   なりました。テスト:
   `tests/test_cancel_token_propagation.cpp` (3 — 静的並列、マルチ Send、
   ファンアウト途中中断)。

## ステータス: v0.3.x フィードバッククローズ

v0.3.x フィードバックバッチ（FastAPI SSE チャットデモ + エルゴノミクスラウンド）の
エンジンに影響する全項目が着地しました。残る項目 #9（pgvector RAG サンプル）は
純粋な動作サンプルであり、エンジンギャップはなく、将来のクックブックトラックとして
`ROADMAP_v1.md` の Candidate 5 に記録されています。v0.3.x シリーズはクローズされ、
以降のエンジン作業は v0.4 / v1.0 を対象とします。

## ✅ v0.3.2 でクローズ

9. **pgvector RAG サンプル → ROADMAP クックブックトラック** — エンジンギャップ
   なしを確認（既存の `PostgresCheckpointStore` インフラで十分。RAG ノードは
   純粋なユーザーコード）。`ROADMAP_v1.md` の Candidate 5 として、
   #8 と同じ Research/Cookbook セクションに記録。エンジンのバージョンアップ
   シリーズではなく、将来のクックブックのドラムビートに属します。

6. **`get_state` 辞書形状用のフラット `StateView` ヘルパー** —
   `engine.get_state_view(thread_id)` が Pydantic v2 の ``StateView`` を返し、
   チャネルがトップレベル属性としてアクセス可能になります
   （``state["channels"]["messages"]["value"]`` の代わりに ``view.messages``）。
   基底クラスは ``extra="allow"`` により任意のチャネル名を許容し、
   ユーザー宣言モデルなしの任意のグラフで動作します。``StateView`` を
   サブクラス化して型付きアクセス用のフィールドを宣言可能。不一致は pydantic の
   ``ValidationError`` を発生させ、暗黙の型強制は行いません。
   ``view.raw`` はバージョン/メタデータが必要な呼び出し元のために
   フラット化されていない辞書を保持します。

   Pydantic v2 をハード依存として追加（現代の LLM Python における必須要素 —
   FastAPI、LangChain、データモデルライブラリはすべて使用）。

   テスト: ``bindings/python/tests/test_state_view.py`` (12)。

8. **自己進化グラフ v2 → `ROADMAP_v1.md` のリサーチトラックへ** —
   トポロジー認識の修正プロンプト方向性はリサーチ候補 #4 として記録。
   エンジン側の変更は、LLM 評価でどのイントロスペクションが実際に効果を
   もたらすか判明すれば小規模になると見込まれます。出荷済みエンジンの
   ユーザーブロッカーではないため v0.3.x から延期。

5. **`update_state` が dict と `list[ChannelWrite]` の両方を受け付ける** —
   v0.3.1 README の説明（"channel_writes は ChannelWrite のリスト"）は
   実際には誤りでした: エンジンは JSON オブジェクトのみを受け付けていたため、
   リストを渡すと **暗黙の no-op** になっていました（C++ の `is_object()` チェックが
   拒否）。Pybind バインディングが入力形状に応じてディスパッチするようになりました:
   - `dict` `{channel: value}` → 既存パス（LangGraph の `values={...}` 形状、
     kwarg 名は異なる）。
   - `list[ChannelWrite]` → dict にリデュース（チャネルごとに最後の書き込みが
     勝つ）。ダックタイプの `.channel`/`.value` オブジェクトも受け付け。
   - その他の型は `TypeError` を発生させ、暗黙の no-op トラップが再発しないように。

   README の `Differences from LangGraph` セクションを修正。
   テスト: `bindings/python/tests/test_update_state_shapes.py` (11)。

10. **`execute_stream` 専用ノードが `run_stream` 経由でディスパッチされる** —
    Python バインディングと C++ エンジンレベルの両方で修正。

    **Python**: `PyGraphNode::execute_full_stream` が `execute_full` に
    フォールバックする前に `execute_stream` を参照するようになり、
    `execute_stream(state, cb)` のみをオーバーライドする Python ノードが
    `engine.run_stream()` / `run_stream_async()` で正しく動作します。
    v0.3.1 の `GraphNode.execute()` のヒントメッセージは誤誘導しなくなりました。

    **C++** (姉妹修正): `execute_stream` のみをオーバーライドする C++ サブクラスが
    同じ問題に直面していました — デフォルトの `GraphNode::execute_full_stream` が
    最初に `execute_full` を呼び出し、それが `execute` / `execute_async` の
    デフォルトチェーンを通じて `ExecuteDefaultGuard` の再帰チェックを発動。
    スローされた `runtime_error` は `result.writes = execute_stream(state, cb)` が
    実行される前に脱出していました。`GraphNodeMissingOverride`（後方互換のため
    `runtime_error` のサブクラス）を導入して修正 — デフォルト再帰ガードがこの
    専用型をスローし、`execute_full_stream{,_async}` の両デフォルトが*この型のみ*を
    キャッチして `execute_stream{,_async}` にフォールスルーします。
    実際のユーザースローエラーはそのまま伝播します。

    優先順位（両言語で維持）: execute_full_stream → execute_stream →
    execute_full → execute。

    テスト:
    `bindings/python/tests/test_execute_stream_dispatch.py` (5)、
    `tests/test_execute_stream_only_dispatch.cpp` (2)。
