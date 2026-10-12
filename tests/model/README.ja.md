<!-- neograph-i18n: source=tests/model/README.md locale=ja source_sha256=4ab02b678a96ff4f83b64df76fa5e351f8972eadfb5f9fbdf138149dd53a24cb -->
# Join モデル (#363)

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`BarrierJoin.tla` は、BSP の super-step（スーパーステップ）と一つの AND join をモデル化します。この join の
`wait_for` slot は複数の `any_of` 代替（alternative）を持つことができ、これが #363 で承認された範囲です
（join のモードは先送りされています）。`tests/test_join_exhaustive.cpp` は同じ規則を参照インタープリターとして
記述し、より大きなグラフで実際の `Scheduler` と `GraphEngine` をそれと照合して検査します。セマンティクスを
変更するときは、両者を一致させてください。

## 規則

- slot は、その代替のいずれかが join にシグナル（signal）を送ると埋まります。join はすべての slot が
  埋まると発火（fire）し、その後、蓄積されたすべてのシグナルをクリアします。
- メンバー以外からのシグナルは記録されますが、決してカウントされません。
- 条件付きルートは、非決定的なラベルの選択です。
- `Command.goto(__end__)` は、その super-step のすべてのルートに優先します。
- 準備完了のものがなくなると、実行（run）は終了します。join がまだシグナルを保持している場合（メンバーからか
  どうかを問わず）、実行は停滞（stall）しており `Failed` になります。ただし、最後の super-step（それ以降に
  準備完了のものがない super-step）が明示的なルートで `__end__` に到達した場合は除きます。明示的なルートとは、
  `__end__` への静的 edge（`.exit()` が書き込むもの）、`__end__` への条件付きルート、または
  `Command.goto(__end__)` です。暗黙の終了（後続ノードのないノード）は停滞を決して抑制せず、それより前の
  super-step での明示的な終了も抑制しません。

したがって、残った `any_of` 代替が join が exit する super-step に到着すれば実行は完了し、一つ後の
super-step に到着して、ほかに準備完了のものがなければ実行は失敗します。

C++ のハーネスは失敗ルート(#364)も扱います。failure edge に沿って失敗したノードはその edge の対象にだけルーティングされ、`Command.goto` は引き続きそれより優先されます。失敗の対象は join や `__end__` にはなれないため、上の規則はそのまま成り立ち、この TLA+ モデルは failure edge を含みません。

## 不変条件

| 名前 | 意味 |
|---|---|
| `NoDoubleFirePerRound`, `JoinRunsOnlyWhenFired`, `FireClearsSignals` | 各発火は、直前の発火以降に到着してすべての slot を埋めたシグナルを消費します。join は発火の直後にのみ実行され、発火はすべてのシグナルをクリアします。 |
| `StallImpliesFailed`, `FailedOnlyOnStall` | join でシグナルが待機したまま終了する実行は失敗し、そのような実行だけが失敗します。 |
| `OnlyExplicitEndSuppressesStall` | シグナルが待機したまま完了する実行は、最後の super-step で明示的に `__end__` に到達しています。 |
| `LeftoverAlternativeStalls` | join の発火後に到着した代替は、後続の発火がなく、最後の super-step に明示的な終了もない場合、実行を失敗させます（fail-closed）。 |
| `NamedOutcomes` | 互いに排他的でない #357 の二つの変種です。残った代替が exit と同時に到着すれば完了し、一つ後の super-step に到着すれば失敗します。 |
| `LegacyEquivalence` | slot がすべて単一要素（singleton）の場合、slot の規則は現在の barrier と同じです。つまり、`wait_for` が受信したシグナルの部分集合であることです。 |
| `Termination`（プロパティ） | すべての実行は、完了するか、失敗するか、step の上限に到達します。 |

## TLC の実行

TLC は CI に含まれていません。Java 11 以降と、TLA+ のリリース（<https://github.com/tlaplus/tlaplus/releases>）
にある `tla2tools.jar` が必要です。

```sh
cd tests/model
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC \
    -workers auto -config BarrierJoin.cfg BarrierJoin.tla
```

`BarrierJoin.cfg` は、ノードが最大三つのすべてのグラフを join のメンバーのすべての `any_of` 分割とともに検査し、
さらに spec で名前を付けた #357 のグラフ（legacy barrier、`any_of` slot、互いに排他的でない二つの変種、
`Command.goto` による明示的な終了）も検査します。TLC は 335,521 個の異なる状態を見つけ、エラーは
ありません。8 worker で約 2 分かかる時間の大半は、21,707 個の初期状態の計算に費やされます。
`MaxNodes = 4` は TLC では手が届きません。C++ harness はノード五つまでをカバーします。

不変条件が実際に違反を検出することを確かめるには、暗黙の終了が停滞を抑制するようにする（`StallImpliesFailed`
が失敗します）か、任意のメンバーのシグナルで join を発火させてください（`NoDoubleFirePerRound` が失敗します）。

## 再開モデル

`tests/resume_model.h` と `tests/test_resume_model.cpp` は、join ハーネスが join を検査するように、
`GraphEngine` の割り込み・再開・再生の契約を参照インタプリタと照合して検査します。
`tests/test_program_resume_model.cpp` は Program ランタイムについて同じことを行います。参照
インタプリタは文書化された契約（`docs/concepts.md`）を述べます。super-step は準備できたノードを
実行し、次にそれらの Send を実行します。呼び出しが尋ねる step はそのすべてで一時停止し、終わったもの
は保持します。再開は指定した割り込み、または最初の割り込みに答え、その呼び出しだけが値を受け取り
ます（#388、#393）。`interrupt_before` の承認はそのゲートが止めたノードに届き、step の各ゲートは
個別に答えられ、答えられたゲートだけが通過します（#382、#396）。失敗は同じ step の通常の割り込み
より優先されます（#392）。

### Core ハーネス

- **グラフ族。** 主ノード `n0`..`n{N-1}` は前向きの辺（ノードごとに後続ノードは最大 2 つ）でつながり、
  すべてのノードに到達できます。ノードは `Ok`、`Ask`（答えを受けるまで `NodeInterrupt` を送出し、
  後続ノードが 2 つなら答えに応じて経路を選べます）、`Fail`（失敗辺あり、またはなし、#364）、
  `Flaky`（失敗した後、自分の attempt 名前空間での再試行が成功する）、または `Map`（項目が `Ok`、
  `Ask`、`Fail` の worker への Send 1 つまたは 2 つ）です。静的な先行ノードが 2 つある最後のノードは
  barrier join にでき、`Ok` か `Ask` のノード 1 つに `interrupt_before` か `interrupt_after` を付け
  られます（拡張範囲では `interrupt_before` ゲート 2 つ）。
- **スケジュール。** 参照インタプリタのすべての回答順序：保留中の各質問に割り込み id で、また既定で
  答え、答えに応じて経路を選ぶ `Ask` には両方のラベルを与えます。各スケジュールは、すべての永続
  操作（チェックポイントの `save` と `put_writes` のそれぞれ。各操作の前と最後の操作の後、拡張範囲では
  各操作の前後で、復旧中に 2 回目のクラッシュも起こします）でのクラッシュと、すべての一時停止での
  フォークでも実行します。クラッシュはプロセスを止めます。ストアは以降の操作をすべて拒否し、ホストは
  実際のホストのように最新のチェックポイントから復旧します。そのチェックポイントが自分の答えていた
  ものなら実行中だった再開をやり直し、見ていない `interrupt_before` や `interrupt_after` の一時停止は
  質問として示し、それ以外は値なしで再開し、チェックポイントのないスレッドは再実行します。すべての
  スケジュールは `InMemoryCheckpointStore` で実行し、粗い時計（すべてのチェックポイントを同じ
  タイムスタンプで保存）の `SqliteCheckpointStore` ではクラッシュなしで実行します（ノード 2 つ、拡張
  範囲では 3 つまでのグラフはクラッシュも起こします）。
- **Ask。** すべてのスケジュールを 2 回実行します。自分の呼び出し id 宛ての答えだけを受け取る Ask と、
  承認ノードのようにどんな再開値でも受け取る Ask です。
- **性質。** P1 ちょうど 1 回：完了が永続化された後（保留中の書き込み、またはその step のコミット）、
  ノード本体が再び完了することはありません。本体が再実行されるのは、クラッシュがその完了を失った
  後だけです。P2 すべての質問を尋ねる：一時停止と保留中の質問、その payload が参照と等しく、各
  `Ask` は与えられた答えをちょうど消費します。P3 同じ結果：状態、ログ、失敗記録、経路が参照と等しい。
  P4 クラッシュ透過性と P5 フォーク等価性：すべてのクラッシュとフォークのスケジュールで P1-P3 が
  成り立ちます。

### Program ハーネス

JavaScript の `main()` プログラム（`callCore`、メンバー 2 つを一緒にまたは 1 つずつ実行する
`ng.all`、`ng.race`、`ng.quorum`、そして `ng.await`）と型付き plan（`parallel`、3 つのうち 2 つの
`quorum`、`sequence`）を、バインドされた provider を呼び出し質問できるノード 1 つの Core 上で実行
します。ホストは `LocalProgramHost` と同じ構成です。プロセス内の価格付きブローカーを使うメモリの
ホスト、またはすべてのストアを 1 つの SQLite ファイルに置き SQLite provider ジャーナルを使うホスト
です。provider は、送ったリクエストを数えるローカルトランスポート上の実際の SDK クライアントです。
P1 は「1 回送り 1 回課金する」も扱います。リクエスト数と、run に課金されたトークンと金額が参照と
等しくなります（プロセス内ブローカーでは、文書どおり再実行されたノードは再び呼び出して再び支払い
ます）。クラッシュは、一度に呼び出しを 1 つ実行するプログラムについて SQLite ホストで起こします。
データベースをすべてのチェックポイント保存、`put_writes`、Program の遷移、provider の dispatch、
ジャーナルに記録された呼び出しでコピーし、コピー上の新しいランタイムが run に再接続します。

ハーネスは、P4 や P5 が扱わない文書化された振る舞いに従うスケジュールを失敗とせずに数え、それが
契約に投げかける問いを報告します。JavaScript のコマンド内のクラッシュは、調整のために run を返し
ます。最初の Core チェックポイントの前に止まった plan は復旧されません（`P_RUN_RECOVERY_BLOCKED`）。
リクエストが出ている間のクラッシュは呼び出しを保留し、その呼び出しは二度と送られません。一時停止
した run のフォークは、一時停止した呼び出しが質問する前に行った provider 呼び出しを再び送り再び
課金しますが、元の run の再開はその呼び出しを再生します。

### 範囲と変数

| | CI 範囲 | `NEOGRAPH_RESUME_MODEL_BOUND=extended` |
|---|---|---|
| 主ノード、特殊ノード | 3、2 | 4（ノード 3 つまでは特殊 3、4 つでは 2） |
| スケジュールあたりの回答、クラッシュ | 4、1 | 6、2 |
| Send の項目 | `ok`、`ask`、`ask ok`、`ask ask`、`ask fail` | 1 つまたは 2 つのすべてのパターン |
| ゲート | `Ok` か `Ask` に 1 つ | どのノードにも 1 つ、または `interrupt_before` 2 つ |

CI 範囲では、Core ハーネスはグラフ 2,196 個と Ask の種類ごとに約 6,100 の回答スケジュール、それぞれ
約 83,000 回の実行を扱います。Program ハーネスは JavaScript プログラム 112 個と plan 18 個、約 750 の
スケジュールを実行します。各テストはワーカー 4 つで 1 分以内に終わります。

変数：`NEOGRAPH_RESUME_MODEL_NODES`、`_SPECIAL`、`_SPECIAL_LARGEST`、`_ANSWERS`、`_CRASHES` は個々の
上限を上書きします。`NEOGRAPH_RESUME_MODEL_SQLITE=0` は SQLite を外し、
`NEOGRAPH_RESUME_MODEL_THREADS` はワーカー数を決めます（既定 4）。Program ハーネスでは
`NEOGRAPH_PROGRAM_RESUME_MODEL_BOUND=extended` がコマンド 2 つのプログラム、すべてのフォーク、すべての
操作の後のクラッシュを加えます。`_ONLY` は説明にその文字列を含むプログラムだけを残し、`_CRASHES=0`
はクラッシュを外し、`_THREADS` はワーカー数を決め（既定 4）、`_DEBUG` はすべての結果と永続操作を
出力します。
