<!-- neograph-i18n: source=docs/PROGRAM_CHILD_SYNTHESIS_CONTRACT.md locale=ja source_sha256=b4350a0091100913423075c5ccded5a70741490d738e4bfaabaa0e42158b2c9e -->
# 子 Program の合成: ホスト認可契約

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_CONTRACT.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_CONTRACT.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_CONTRACT.zh-CN.md)

状態: N1 のホスト境界と N2 の永続ランタイム統合は実装済み。[SQLite/PostgreSQL の永続化と復旧](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)を参照。Program 側の専用提出経路とモデルによる生成は、今後の作業として残る。

## エントリと所有権

`ProgramSynthesisGateway::synthesize_child(proposal, grant, parent)` は、別途選択した `ProgramChildSynthesisGrant` の下でのみ proposal を受け入れる。ホストは親の version、run record、lineage head、generation を読み込み、`ProgramChildSynthesisParent` に格納する。これらのスナップショットと grant は、ホスト自身のストアとレビュー済みテンプレートポリシーから取得しなければならない。要求元の Program から取得してはならない。

最初の境界は、レビュー済みテンプレートの正確な**具体化結果**を承認する。grant は `template_identity` と `program_synthesis_source_identity(source)` を結び付ける。後者は import の識別情報、封印された module 本体、ソース座標、runtime/profile の識別情報を含む、完全な正規ソースエンベロープを対象とする。ホストは grant を発行する前に具体化結果を描画してレビューする。この API は、テンプレート描画やそのパラメータスキーマを実装しない。grant は意味検証器とタスク契約の識別情報も固定する。子のコンパイルには親の封印済み registry フィンガープリントを使わなければならない。この初回提供の範囲では、同じソーステキストから異なる登録済み実装を選ぶことはできない。

保存済み grant のハッシュは完全性を証明するが、発行者は証明しない。呼び出し元が自作した grant を解析できても、その使用許可にはならない。gateway は、ホスト grant と親スナップショットの代わりにシリアライズ済み認可受領記録を受け付けない。

## 予約前の検査

純粋操作 `authorize_program_child_synthesis` は次を検査する。

- owner、parent run、親の ProgramVersion と bundle、親ポリシーのフィンガープリント、lineage 識別情報、有効な generation、正確な record/journal/head の組み合わせ。
- 親スナップショットが実行中であり、終端状態ではないこと。
- レビュー済みソースの正確な識別情報、正規ソースエンベロープのバイト列、封印済み module 数。
- 要求した能力・副作用がホスト grant と親ポリシーの両方に収まり、import した module の識別情報が親ポリシーに収まること。
- 子 budget の九つの全次元がホスト上限と親の利用可能な残量に収まること。親の進行中予約から借りてはならない。
- コンパイル一回、子一つ、子孫深度一段分の余裕。
- 親の実行保証を弱めない、子の保証下限。

これらの検査はリソースを予約しない。不変で読み取り専用の `ProgramChildSynthesisAuthorization` 証拠を生成する。他の子の予約や容量は、通常の `ProgramRuntime::start_child` 境界で引き続き検査しなければならない。同時に得た事前検査結果は、それぞれ独立した budget grant ではない。

## 予約とコンパイル

子エントリには `ProgramSynthesisGatewayConfig::reserve_child` が必須である。そのシグネチャは proposal と authorization の両方を受け取り、compare-and-swap の対象となる正確な `source_lineage_head_id` と `parent_remaining` も含む。その head に対してコンパイル一回分を原子的に差し引くか、別の generation に課金せず失敗しなければならない。新しい head を再読み込みし、黙ってその head に対する予約へ切り替えてはならない。ホストは実行所有権、キャンセル、現在有効な deadline も検査し、予約の精算時に経過 wall time を課金する。保存済みスナップショットは、その wall-time 割当が現在も利用可能だという証拠にはならない。

返される `ProgramSynthesisReservation` は、同じ proposal、lineage、source head、開始 budget に結び付かなければならない。gateway は compiler 評価の前に、開始 budget の変更、誤った head、予約後 budget の不足、子・深度容量の喪失を拒否する。予約の構築自体も独立して、コンパイルの差し引きが正確に一回分であることを要求し、budget の増加を禁止する。

子のコンパイルには、親のより大きい残 budget ではなく、要求した子の budget 上限を使う。コンパイルはソース識別情報、要求した能力・副作用の閉包、認可済み保証下限を保持しなければならない。必須のホスト意味検証器は grant の validator と contract の識別情報に一致し、Catalog 承認前に実行する。生成された子ポリシーには、要求を超える能力・副作用の許可や、提出ソースの import に含まれない module の許可を持たせてはならない。意味検証で拒否されても、先に差し引いたコンパイル分は戻さない。再試行には、特定 head に対応する有効な予約が必要である。古い事前検査証拠を再利用して budget を補充してはならない。

子専用 gateway は汎用 `reserve` コールバックを省略できる。その場合、汎用 `synthesize()` エントリを呼ぶと失敗する。逆に、既存の後継合成 gateway は、`reserve` を設定しただけでは子合成の許可を得ない。いずれのエントリも他方へフォールバックしない。

## JavaScript の budget と保証の境界

Generator Program は、ホスト所有の `ProgramBudgetBounds` と Catalog ポリシーを通じて、ゼロでない動的コンパイル上限を受け取れるようになった。デフォルトのコンパイルは引き続き動的コンパイルをゼロ回とする。呼び出し元が `start` 時にその上限を増やすことはできない。宣言専用 JavaScript と、`expand_task_graph` を含まない通常の C++ plan は、動的コンパイルをゼロとする構造規則を維持する。

この budget 変更は、JavaScript コマンドや暗黙の compiler アクセスを追加しない。`ng.hostCapability` は既存の trusted-native インターフェースのままであり、N1 はそこへ合成プラグインを導入せず、native C ABI も変更しない。

現在の compiler は generator control を保守的に `Unmanaged` と分類する。制限された generator 言語 profile を、自動的に `Strict` 実行保証を与えるものとして示してはならない。N1 はこの分類を維持する。レビュー済みの宣言専用の子は、Core 閉包が Strict なら Strict grant を満たせる。generator の子は proposal で要求しただけでは、その保証下限を通過できない。より弱い子を承認するには、明示的に互換なホスト grant と親保証が必要であり、コンパイル中に黙って下限を下げてはならない。

## 診断

| コード | 境界 |
|---|---|
| `P_CHILD_SYNTHESIS_OWNER` | Owner の不一致 |
| `P_CHILD_SYNTHESIS_GENERATION` | 誤った run/version/policy、古いまたは不整合な head、無効な generation |
| `P_CHILD_SYNTHESIS_SOURCE` | 未レビューの具体化結果、または source/module のサイズ制限 |
| `P_CHILD_SYNTHESIS_REGISTRY` | コンパイル済みの子が異なる封印済み registry を使う |
| `P_CHILD_SYNTHESIS_SEMANTICS` | Validator または task-contract の識別情報が変わった |
| `P_CHILD_SYNTHESIS_AUTHORITY` | 要求または子の承認ポリシーが権限を拡張する |
| `P_CHILD_SYNTHESIS_BUDGET` | 無効な実行 budget、または compile/child/depth/host 容量の不足 |
| `P_CHILD_SYNTHESIS_RESERVATION` | 予約が認可済み head と残 budget に結び付いていない |
| `P_CHILD_SYNTHESIS_GUARANTEE` | Grant またはコンパイル済みの子が保証下限に違反する |

`ProgramSynthesisValidationError` は、引き続き意味検証の拒否証拠を保持する。コンパイルエラーは既存の compiler 診断を維持する。保存 budget のデコーダは、正規識別情報の検証前に負数、小数、範囲外の整数を拒否する。無効な数値がラップアラウンドし、それ以外は有効な保存 ID に化けることはできない。

## 永続統合と残る検証範囲

単独の `synthesize_child` API は承認で終わる。N2 runtime API は、SQLite と PostgreSQL において、原子的予約、永続的な段階別結果、parent-run/generation スコープの binding、既存の子ライフサイクルによる dispatch を追加する。構成、復旧動作、N3 障害マトリクスと明示された制限については[永続化契約](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)を参照。authorization 値は証拠のままであり、新たな認可と誤認され得る公開の保存値コンストラクタはない。

この C++ API 追加に伴い、Program consumer の再ビルドが必要である。既存の後継合成結果形式、JavaScript コマンドプロトコル、native control C ABI、Core-only の依存境界は変わらない。
