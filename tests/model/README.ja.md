<!-- neograph-i18n: source=tests/model/README.md locale=ja source_sha256=39036d07e843a8120cb5ee7f36521353e0c42c468b608c4b0f7909b22286dac0 -->
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
