<!-- neograph-i18n: source=docs/PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md locale=ja source_sha256=7a6203d69da6c363bf3c4c5b6aa5b1b1fcd2f36c656c6f22bffc36cddeeb1e72 -->
# 子合成の永続化

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.zh-CN.md)

状態: インメモリ参照ストア、SQLite、PostgreSQL 向けの N2 は実装済み。単一の子シナリオに対する N3 復旧検証は、[復旧マトリクス](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md)に記載する。ホストエントリポイントは、既存のトップレベル checkpoint と既存の `ng.spawn` / `ng.await` 子ライフサイクルを使う。これは範囲を限定したレビュー済みソースの経路であり、モデル generator、テンプレート renderer、新しい DSL コマンドを実装するものではない。

## ホスト統合

`RuntimeConfig::child_synthesis_gateway` と `child_synthesis_grant_resolver` を設定する。リゾルバは `(owner_scope, parent_run_id, grant_id)` を使い、信頼済みホストポリシーから正確な grant を選択しなければならない。合成レコードから grant を読み込んでも、その権限は確立されない。

1. 親の次のトップレベル checkpoint で `ProgramHandoff` を取得できるようにする。
2. 親 run、有効な lineage、generation を読み込み、[認可契約](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md)に従って、レビュー済みソースの具体化結果と grant を選択する。
3. handoff を保持したまま、`ProgramRuntime::prepare_child_synthesis(owner, parent, handoff, proposal, grant, binding_name)` を呼ぶ。
4. 返されたレコードが `Bound` に達したら handoff を解放する。
5. 親は通常の `ng.spawn` / `ng.await` 経路でその binding 名を使う。

たとえば、承認済みの親は次を yield できる。

```javascript
yield ng.checkpoint({request: "reviewed-child"}, "synthesis:request");
return yield ng.await(
  ng.spawn("generated-child", input.childInput, "generated:spawn"),
  5000,
  "generated:await"
);
```

checkpoint 要求をレビュー済みソースとポリシーへどう対応付けるかはホストが決定する。checkpoint payload は、自分自身にコンパイル権限や実行権限を与えられない。永続経路ではランタイムトランザクションが予約を管理し、gateway の単独 `reserve` / `reserve_child` コールバックは呼ばない。

## 原子的な公開

`ProgramTransitionPublication::child_synthesis_records` は、親の run snapshot、journal head、lineage と同じトランザクションで、不変レコードを一つ追加する。最初の `Reserved` レコードは、正確な直前の親 snapshot、元の lineage、結果の lineage、動的コンパイル一単位の差し引きに結び付かなければならない。競合する要求が同一の budget 遷移を再利用し、別の binding を賄うことはできない。同一の公開の再試行は `AlreadyPresent` を返す。

SQLite は既存の `BEGIN IMMEDIATE` トランザクションを使う。PostgreSQL は既存のトランザクションと owner advisory lock を使う。両者とも合成ログへ追記し、追記順で検証済み request head を再構築する。書き込みが失敗すると、親、lineage、合成レコードをまとめてロールバックする。インメモリ参照実装は強い例外保証を維持する。

合成レコードを持つ公開は storage schema 6 を使う。通常の公開は引き続き schema 5 としてシリアライズし、reader は schema 1–5 の対応を維持する。`load_child_syntheses(owner, parent_run)` は現在の request head を返す。この履歴に対応しないカスタムストアでは、基底実装は安全側に失敗する。動的 budget を持つ run の復旧には、この読み取り機能が必要である。

レコードの上限は 8 MiB と 16 revision である。proposal、元のホスト grant と親 context、reservation、累積した段階出力を保持する。正規識別情報と段階検証は、過去の成果物の変更、異なる generation、binding の改名、証拠の改変を拒否する。binding 名は一つの parent run 内で一意である。SQL ログは追記専用であり、大きな履歴の保持方針と圧縮は今後の作業として残る。

## 段階と復旧

| 永続状態 | 復旧後の次の操作 |
|---|---|
| `Reserved` | 実行権を確保し、一度だけコンパイルする |
| `Compiling` | `ReconciliationRequired` とする。不確実なコンパイルを再実行しない |
| `Compiled` | 保存済み bundle を再利用し、意味検証の実行権を確保する |
| `Validating` | `ReconciliationRequired` とする。不確実な validator を再実行しない |
| `Validated` | 受理済みの受領記録と証拠を再利用し、承認ポリシーを選ぶ |
| `Admitting` | 凍結済み承認と冪等な Catalog 承認を再利用する |
| `Admitted` | スコープ付き module 受領記録を使い、正確な承認済み version をリンクする |
| `Bound` | 親の通常の child command を再開する |
| `Dispatching` | 記録済み child ID と input を `start_child` で再利用する |
| `Spawned` | 子・結果を再利用するか、永続 checkpoint のない Core dispatch を照合する |
| `Failed` / `ReconciliationRequired` | 結果を保持し、親の自動 replay を禁止する |

未完了の合成を持つ非稼働の親を再接続する前に、`recover_child_synthesis(owner, parent_run, proposal_id)` を呼ぶ。稼働中の親には、保持した checkpoint API が必要である。復旧のたびにホスト権限を選び直し、元の generation を検査する。完了済みの意味検証と凍結済み承認は再利用する。意味検証による拒否は受領記録と証拠を保持し、承認へ進むことはない。

生成された binding は、静的リゾルバの parent-version スコープではなく、実際の parent run と generation を使って解決する。dispatch は `start_child` の前に安定した child ID と input を記録する。一つの grant で二つ目の呼び出しを作ることはできない。通常の子 budget、公開、実行、join が引き続き最終的な根拠となる。

完了、再試行、復旧、およびコミット確認応答の喪失時にも、親はコンパイル差し引き分を保持する。合成は経過 wall time を差し引く。復旧した attempt でも元の合成 deadline を保持する。spawn 状態の公開は、既存コマンドの進行中予約を保持する。

## 検証と残る作業

backend 適合性テストは、spawn/join 成功、正規形式の往復、owner/run 分離、重複要求と同時要求、backend 書き込み失敗時のロールバック、コミット済み段階の再利用、意味検証の拒否、grant 失効、不確実な compile/validation 実行権の確保を対象とする。Linux の process-exit テストは、承認の凍結後にデストラクタを呼ばず終了し、SQLite と PostgreSQL の両方で Program store、transition store、checkpoint store を再オープンしてから、子の join に成功する。

[N3 復旧マトリクス](PROGRAM_CHILD_SYNTHESIS_RECOVERY.md)は、データベースごとの 16 process-exit 境界、複数プロセスによる同時復旧、確認応答の喪失、実際の PostgreSQL 接続切断、キャンセル、version 保持、activation、Program replacement まで対象を広げる。その制限は明示したままである。自動照合 authorizer はなく、一般的な混合副作用 join、電源断、ホスト間 failover、graph 移行・保持のすべての組み合わせは、単一の子シナリオの対象外である。

この変更は公開 C++ 型、virtual method、構成フィールドを追加する。すべての Program consumer を再ビルドし、古い object を新しい library と混在させてはならない。Core-only の依存や native control C ABI の変更は追加しない。
