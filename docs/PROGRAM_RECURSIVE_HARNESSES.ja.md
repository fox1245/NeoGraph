<!-- neograph-i18n: source=docs/PROGRAM_RECURSIVE_HARNESSES.md locale=ja source_sha256=66abaa1eb78c67c16cf2ef0e6f42ad71632806116dc0b71dd952e8433d4b4970 -->
# 独立した Harness generation を持つ再帰エージェント

**Languages:** [English](PROGRAM_RECURSIVE_HARNESSES.md) | [한국어](PROGRAM_RECURSIVE_HARNESSES.ko.md) | [日本語](PROGRAM_RECURSIVE_HARNESSES.ja.md) | [简体中文](PROGRAM_RECURSIVE_HARNESSES.zh-CN.md)

検証済みシナリオは、一つの owner の下で、単一の論理 session を親・子・孫のツリーとして実行する。各エージェントは独自の ProgramVersion、Core topology、state、budget、lineage を持つ。子は、主 orchestrator と同じホスト所有の合成境界を通じ、自分の子の JavaScript を提案できる。

参照テストは四つの異なる Core plan を実行する。主 Harness は一ノード、元の子は二ノード、置換後の子は三ノード、孫は四ノードである。孫がまだ生存している間に子を置換する。主 orchestrator は既存の await を通じて、置換後の子の結果を受け取る。SQLite と PostgreSQL の process-exit テストは、ネストした置換のコミット後にツリー全体を再オープンし、完了済み作業を再実行せずに完了する。

## 識別情報とストレージ

`ProgramRunRecord::logical_run_id()` と `ProgramHandle::logical_run_id()` は、安定したエージェント識別情報を公開する。置換のたびに、異なる physical run と不変の ProgramVersion を作成する点は変わらない。lineage が有効な generation を選択する。その generation の handle を取得するには、`reconnect(owner, logical_run_id)` を使う。

新しい run-record schema 4 は、logical run が physical run と異なる場合に `logical_run_id` を持つ。child relation は引き続き元の child 識別情報を参照し、最初の承認済み link と invocation を保持する。置換後の実行が終端結果を返す場合、relation は、その結果の実際の run/version 識別情報に加えて `terminal_generation` を記録する。transition backend はこれを、コミット済みの child lineage、generation、終端 run record と照合する。

既存の schema 2/3 レコードは読み取り可能であり、新しいフィールドを持たない通常のレコードは schema 3 表現を維持する。古い Program binary を新しい C++ record layout と混在させてはならない。Program consumer をまとめて再ビルドすること。

## 子孫を伴う置換

生存している family の置換には、その所有 runtime と、そのエージェントの generator 内で完了済みの、保持されたトップレベル checkpoint を使う。この checkpoint はネストしたエージェントのものでもよい。置換先はすでに承認済みでなければならない。

同じトランザクションで新しい generation を公開し、既存の child relation とコミット済み子孫 budget をコピーする。親識別情報と絶対的な child depth は変更できない。置換先は子の能力・副作用 grant と保証下限を保持しなければならない。ネストした置換は親の結果契約を維持しなければならない。保持する子の binding 名は一意である必要がある。中断した子には、親の置換前に明示的な処置が必要である。

旧実行は、論理エージェントの terminal hook を発行せず、移管した子をキャンセルせずに退役する。論理 child-concurrency と quota の cleanup は後継へ移り、attempt 固有の cleanup は引き続き実行される。子をすでに待っている親は後継の結果を追い、その child handle を通じたキャンセルも同じ連鎖を追う。root-generation handle は既存の generation semantics を維持する。有効な root には、返された replacement handle を使うか、logical identity で再接続する。

不変な generation 作成公開に存在する子だけが、継承 binding になる。既存の子は、正確に一致する binding と input を使って再 join できる。

```javascript
// Original child Harness:
const worker = yield ng.spawn("grandchild", {}, "child:spawn");
yield ng.checkpoint(
  {child: worker.child_run_id, replacement: "child-v2.json"},
  "child:swap"
);
```

```javascript
// Replacement Harness: this resolves the inherited child invocation.
const result = yield ng.await(
  ng.spawn("grandchild", {}, "replacement:join"),
  30000,
  "replacement:await"
);
return {generation: 2, result};
```

置換は、その子を再作成せず、invocation を書き換えず、別の child grant を取得しない。旧 generation の未添付の合成 binding は新しい権限にならない。新しい binding は引き続き通常の合成・承認経路を使う。ソース提案は、独立にレビュー済みのホスト template instance と一致しなければならない。参照ホストは、エージェントが checkpoint に含めたというだけではソースを承認しない。

## プロセス喪失後の承認済みの子の復旧

承認済み child binding を復元するホストは、未完了の JavaScript `spawn`/`await` コマンドが公開済みの子へ再接続することを、明示的に許可できる。

```cpp
config.recover_existing_child_commands = true; // default: false
```

これは合成 gateway なしで、通常の static binding と置換によって継承された子の両方に使える。parent relation、link receipt、owner、正確な invocation、child depth、永続化された child run が一致しなければならない。static command は記録された command coordinate から同じ child ID を導出しなければならない。継承された子は generation 作成公開で承認された ID を保持する。過去の Program version とホスト grant は、引き続き利用可能でなければならない。ホスト admission resolver を設定している場合、通常どおり child attempt を承認する。

復旧は専用の reconnect 経路を使う。検査後に子が消えても、新しい子の作成へフォールスルーできない。run の欠落、input の変更、binding の利用不能がある場合、既存コマンドを照合待ちの保留状態に残す。コマンド予約と子孫会計は復旧をまたいで維持する。このフラグは、未知の provider、tool、native、その他の外部副作用の replay を認可しない。別途 grant を要する合成復旧プロトコルは変わらない。

このフラグとは独立に、実行中 attempt の replay は、未完了の封印済み `ng.checkpoint` を、正確な journaled value と元の command reservation から完了する。外部 dispatch は行わない。他の未知の副作用については、引き続き family を停止して照合を待つ。中断した子は dispatch 済みで再開可能な relation に留まり、親は relation をキャンセルしたり予約を二重返却したりせず、進行中 command reservation を保持する。

ストレージが利用可能なまま結果公開が失敗した場合、dispatch 済みコマンドの結果と pending effect の両方を `Ambiguous` として公開する。キャンセルでその不確実性を消すことはできない。楽観的 command-head 読み取りの競合は再試行する一方、親 head が変わらないまま恒久的に拒否されても、子の完了通知を無期限に阻止することはできない。

`*StaticChild*` テストは、SQLite と PostgreSQL の process exit、static と継承された再帰 child、不変の child ID と receipt、正確に 64 journal entry を生成する孫の 32 checkpoint を対象とする。opt-in の欠如、run/binding の欠落、input/receipt の改変、復旧承認と dispatch の間で run が消えるケースも実行する。

## JSON 成果物と会計

JSON はシリアライズされ、検証された Program bundle である。異なる JSON の読み込みは、不変 version の候補を選択する。ファイル変更は稼働エンジンを変更しない。ホストが version を承認し、保持した checkpoint でエージェントの generation を切り替える。generator の heap/stack state は移植しない。アプリケーション state は明示的な `handoff` JSON を通じて渡し、child relation は永続ツリーに残る。

例の replacement bundle は、session 開始前にコンパイル・承認する。主エージェントと子エージェントは実行中に子孫の JavaScript を提案し、それらのコンパイルは各エージェントの grant を消費する。新たに生成した自己置換を準備することは、別のコンパイル・承認操作のままである。`replace` API 自体は、承認済みの置換先を選択する。

委譲済み compile budget は、親の別の合成要求には使えない。置換は正確な残 budget とコミット済み子孫を移管する。child recovery は初回 attempt の deadline も保持する。SQLite Catalog は現在、transition store と同様に、同時 agent publication とデータベースを共有できるよう、上限付き busy timeout を使う。

## 参照実装の実行

Program と QuickJS をビルドし、各ケースには両方の永続化 backend を有効にする。`neograph_program_tests --gtest_filter="*Recursive*"` を実行する。PostgreSQL ケースは既存の使い捨て `NEOGRAPH_TEST_POSTGRES_URL` fixture と CTest database resource lock を使う。この構成には native WSL Docker を利用できる。

成功する topology テストの実行時に `NEOGRAPH_RECURSIVE_ARTIFACT_DIR` を設定すると、`main.json`、`child-v1.json`、`child-v2.json`、`grandchild.json`、`session.json` を出力する。これらはテストの registry/compiler 識別情報を使うレビュー用 fixture であり、単独の本番構成ではない。

この検証は三階層ツリー、ネストした checkpoint 置換、生存する子孫の保持とキャンセル、generation-result の完全性、budget の縮小、SQLite/PostgreSQL での session 全体のプロセス復旧を対象とする。LLM ソース生成サービス、Python/transport facade、owner 間の session 共有ポリシー、任意の Core のインプレース変更を導入するものではない。native Core migration は既存の境界を維持する。複数ホスト間での独立した live ownership transfer と、context/hook 障害の全組み合わせは、別途検証が必要である。

## 対話型 chatbot の例

[進化する Harness chatbot](../examples/cookbook/self_evolving_chatbot/README.md) は、分離した二 tenant、OpenRouter adapter、browser inspector、ターンごとのレビュー済み template proposal、runtime successor compilation、SQLite または PostgreSQL の chat/provider ledger を加えて参照実装を拡張する。assistant は置換後に reviewer を合成でき、その間も元の orchestrator は同じ logical assistant を待ち続ける。

`RuntimeConfig::checkpoint_handler` により、ホストは永続公開後に handle と move-only checkpoint lease をキューへ入れられる。lease を保持すると、scheduler thread をブロックせずに generator を一時停止する。コールバックは速やかに戻らなければならない。コンパイルと置換はホスト worker 上で実行する。再接続時には、最新の完了済み checkpoint の replay も通知される。明示的な `next_handoff` 要求が優先する。

`ProgramRuntime::reserve_synthesis` は、想定した lineage head に対して未割当の動的コンパイル一回分を差し引き、保持中 lease の journal reference を更新する。checkpoint 識別情報とシリアライズされた handoff value は変わらない。古い head、別の owner/runtime、期限切れの wall budget、子孫へ割り当て済みの compile budget は拒否する。ホストは予約前に intent を永続化し、結果を記録しなければならない。不確実な確認応答は無料の再試行を認可できない。chatbot は通常の `ProgramSynthesisGateway` でこの予約を使い、承認済み置換先を `replace` に渡す。

chatbot の template gate は回答品質の証明ではなく、モデル呼び出しと generator control は `Unmanaged` 保証を維持する。プロセス喪失後の pending provider effect には、自動再 dispatch ではなく照合が必要である。
