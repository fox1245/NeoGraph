<!-- neograph-i18n: source=docs/PROGRAM_CHILD_SYNTHESIS_RECOVERY.md locale=ja source_sha256=6ed07e73d44459a4ff1885cb7f07064be6994987113c36f0c7bae8c9c79d76ed -->
# 子合成の復旧検証

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_RECOVERY.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_RECOVERY.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_RECOVERY.zh-CN.md)

N3 は、レビュー済みソースを使う単一の子と checkpoint のシナリオを、SQLite と PostgreSQL で検証する。復旧では、記録済みの子操作と未分類の外部副作用を区別するようになった。ただし Core dispatch に永続 checkpoint も結果もない場合は、引き続き明示的な照合を要求する。

## 復旧規則

復旧が dispatch を検討する前に、親の command journal が承認済み Program、command ordinal、payload、effect identity に一致しなければならない。この復旧経路に入れるのは `spawn`、または合成された `spawn` に終わる `await` 連鎖だけである。ホストは grant、元の generation、実際のコンパイル差し引き、生成済み binding を再検証する。記録済み dispatch については、正確な child run ID と正規 input も一致しなければならない。

復旧はコマンドの既存リソース予約と操作差し引きを再利用する。同じコマンドに再課金したり、別の動的コンパイルを許可したりしない。子予約の再構築には保留中コマンドが保持するリソースも含む。この再構築が変えるのはプロセス内の会計上の見え方であり、永続 budget ではない。既存の子レコードと終端結果が引き続き最終的な根拠となる。

同一の `Compiling` または `Validating` 公開がすでに存在しても、新たな呼び出し元がその作業を所有する証拠にはならない。`Published` を受け取った writer だけが、確保した段階を開始できる。同時 cold recovery テストでは、この公開の前に独立した二つのプロセスを同期させ、worker 一つの成功、競合一つ、意味検証一回を要求する。

## プロセス終了マトリクス

各ケースは、選択した公開の後にデストラクタを呼ばずプロセスを終了し、新しい Catalog、transition、checkpoint store を開く。両データベースで同じケースを実行する。

| 中断箇所 | 必要な復旧 |
|---|---|
| `Reserved` | 既存の予約から一度だけコンパイルする |
| `Compiling` | 差し引きを保持し、照合を要求する |
| `Compiled` | bundle を再利用する |
| `Validating` | 差し引きを保持し、照合を要求する |
| `Validated` | 意味検証の証拠を再利用する |
| `Admitting` | 凍結済み承認を再利用する |
| `Admitted` | 承認済み version をバインドする |
| `Bound` | 記録済みの親コマンドを実行する |
| `Dispatching` | child 識別情報と input を再利用する |
| `Spawned` | 既存の子を復旧するか、不確実な Core dispatch を明示的に照合する |
| 子 relation の `Publishing` | 既存の子の初回公開を完了する |
| 子 relation の `Dispatched` | replay 非安全な Core 作業に checkpoint/result がなければ照合する |
| 子の終端結果 | 結果を再利用し、完了済みの子を再実行しない |
| 親への子結果の添付 | 既存の join 結果を再利用する |
| 親のコマンド結果 | 記録済みコマンド結果を replay する |
| 親の終端結果 | 同一の終端結果識別情報を返す |

データベースごとに 16 の process-exit 境界と、二つのプロセスによる復旧競合一件がある。テストは、元の child 識別情報、永続 child relation 一つ、コンパイル差し引き一回、維持された command-operation 数も検証する。実行マーカーは、完了済みの子作業がプロセス終了をまたいで繰り返されないことを検証する。

## 不確実な子の実行

`Dispatched` relation だけでは、プロセスが消える前に Core 作業が実行されたかを確定できない。子がまだ実行中として記録され、正確な checkpoint がなく、その plan が checkpoint なしの replay に対して安全でない場合、親の再接続は安全側に失敗する。`recover_child_synthesis` は child ID と input を保持したまま、`P_CHILD_SYNTHESIS_CHILD_UNCERTAIN` を伴う `ReconciliationRequired` を記録する。

この非稼働の親に対する復旧検査は、稼働中の親への再接続を実行喪失と誤分類しない。稼働中の再接続は既存の attempt を返す。

合成レコードがすでに `Spawned` でも、この不確実性の処置を適用する。レコードは過去の成果物を変更せずに、その照合処置を追記できる。代わりの子を発行したり、成功結果を捏造したりしない。この変更では、自動照合承認 API は提供しない。

## 追加の検査

- 予約の確認応答を失った後、合成履歴が読めなくなっても、コンパイル一単位を返却することはできない。読み取りが復旧したら元の要求を再利用する。
- PostgreSQL テストは予約トランザクションの内部でデータベース接続を切断する。新しい接続では元の親が見え、部分的な予約は見えない。その後の attempt は一度だけ差し引く。
- 意味検証中のキャンセルは、承認や子 dispatch に進むことができない。
- Catalog activation は、生成済み binding を別の version へ向け直せない。
- Retention pin は必要な version を保持する。ホストが子 version を削除した場合、キャッシュ済み合成証拠ではその version を実行できない。すべての合成履歴から retention root を自動収集する責任は、引き続きホストにある。
- Program replacement はコンパイル差し引きを保持し、旧 run の未添付の生成済み binding を後継 generation へ移さない。添付済み子孫は、[再帰 Harness 契約](PROGRAM_RECURSIVE_HARNESSES.md)を通じて保持できるようになった。
- PostgreSQL テストは、process-exit マトリクスと接続切断ケースを含め、既存の CTest database resource lock を共有する。

## この検証の限界

これは、記載した一連のシナリオに対するプロセス障害と接続障害の検証である。外部副作用の普遍的な exactly-once、電源断、ホスト間 database failover、不確実な Core/provider 結果の自動解決を検証したとは主張しない。ホスト副作用が混在する一般的な構造化 join は、既存の照合動作を維持する。複数の生成済み子ツリー、任意の外部 validator 中のキャンセル、graph 移行の全組み合わせ、retention root の自動発見、合成性能や storage 増加の検証には、別の対象範囲が必要である。

ホスト統合は[永続化契約](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)、変更されていないレビュー済みソースと権限の境界は[認可契約](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md)を参照。
