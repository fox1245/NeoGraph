<!-- neograph-i18n: source=SECURITY.md locale=ja source_sha256=1846450e26db18b21a90809734487fbc5fef135a3425887bd70fae57a20beb4e -->
# セキュリティ ポリシー

**Languages:** [English](SECURITY.md) | [한국어](SECURITY.ko.md) | [日本語](SECURITY.ja.md) | [简体中文](SECURITY.zh-CN.md)

## サポート対象のバージョン

NeoGraph は 1.0 より前の段階です。セキュリティ修正はまず `master` に入り、
現在のマイナー ラインの次のリリースで配布されます。古いマイナー ラインには
パッチを当てないため、最新リリースへ更新してください。

| バージョン | サポート |
| ---------- | -------- |
| `master` | はい、修正が最初に入る |
| 0.13.x (最新リリース) | はい |
| < 0.13 | いいえ |

## 脆弱性の報告

セキュリティの問題を公開 issue にしないでください。GitHub で非公開に報告して
ください。リポジトリの **Security** タブで **Report a vulnerability** を選びます
(<https://github.com/fox1245/NeoGraph/security/advisories/new>)。

可能であれば次を含めてください。

- 影響を受けるバージョンまたはコミット
- コンポーネント: コア エンジン、Program ランタイム、Python バインディング、
  MCP・A2A・ACP・gRPC のサーバーまたはクライアント、チェックポイント ストア、
  SchemaProvider との連携
- 最小限の再現手順と観測した影響

報告に実際の API キー、認証情報、個人データを含めないでください。

対応の目安: 7 日以内に受領を確認し、14 日以内に初回の評価をお伝えします。
修正の日程は深刻度に応じて決めます。修正は、パッチを当てたリリースの公開後に
GitHub Security Advisory で開示し、該当する場合は CVE も発行します。

## 対象範囲

対象: このリポジトリの NeoGraph のソース、配布済みの wheel、そして NeoGraph が
同梱する形の vendored サードパーティ コード (例: QuickJS、cpp-httplib) のうち
NeoGraph を通じて到達できる問題。

対象外: NeoGraph が公開しないサードパーティ依存の問題 (上流に報告してください)、
`docs/promo` 以下のプロモーション動画ツール、そして使い方を示すための例 (強化
された運用構成ではありません)。

## セキュリティ モデルの要点

- Program のソースとグラフ定義はコードと同じように扱ってください。信頼できる
  作成者から受け取るか admission ポリシーを通して承認し、実行予算でコストを
  制限してください。
- ツールの結果とモデルの出力は信頼できないデータです。モデルが注入された指示を
  拒むことに頼らず、効果のあるツールは ToolGate と Hook で制御してください。
- API キーは環境変数かホストのコールバックで渡し、Program やグラフの JSON に
  入れないでください。NeoGraph はキーをログ、イベント、ワイヤ上のエラーに
  書きません。
- NeoGraph と SchemaProvider transport の外向き HTTPS は TLS 1.2 以上を
  必要とします。
