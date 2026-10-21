<!-- neograph-i18n: source=benchmarks/prompt_cache/README.md locale=ja source_sha256=606a79f539c2aa4251858cf8bd9238efa4b6b14a5b4024faf240476f774b4114 -->
# プロンプトキャッシュ接頭辞プローブ

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

プロバイダーのプロンプトキャッシュは、先頭の1バイト目から以前のリクエストと同一である、リクエストの前の部分を再利用します。NeoGraph のランタイムは、固定のコンテキストアーティファクトの集合と、伸びていく RAW 履歴から各リクエストを組み立てます（[`docs/STRICT_RUNTIME_INTERPOSITION.md`](../../docs/STRICT_RUNTIME_INTERPOSITION.md) の「Artifact placement」を参照）。このプローブは、ターンごとに変わる小さなブロックをどこに置くかで、プロバイダーが各リクエストのどれだけを再利用できるかがどう変わるかを測ります（issue #329）。

## 内容

`prompt_cache_probe.py` は、12〜20 ターンの会話を `RuntimeTurnAssembler` と `ControlledProvider` で実行します。

- 長い固定ブロック: プロバイダーのキャッシュ最小長を超えるよう、4,500〜5,000 トークンの `RequiredSkill` アーティファクト（`--bible-entries`、90〜115）。
- ターンごとに質問と返答が1つずつ増える RAW 履歴。
- ターンごとに変わり、シナリオが配置を決める約 400 トークンの動的ブロック（`--dynamic-lines`）。

プロバイダー呼び出しごとに、プロバイダーが報告した使用量（入力、キャッシュ読み取り、キャッシュ書き込み、キャッシュされなかったトークン）と、エンコードされたリクエストのうち直前のリクエストと同一の部分を記録します。その部分は、ワイヤ本文のバイト数と、読まれる順序（ツール、システム、メッセージ）で見たプロンプトの文字数で測ります。

シナリオ名は動的ブロックの配置で、末尾に `+compact` を付けると `--compact-at` で圧縮が加わります。最も古いターンが RAW ウィンドウから外れ、それらの要約がアーティファクトとして追加されます。配置は `none`、`default`（`BeforeLatestUser`）、`before-history`、`after-history`、`raw-append`（ブロックをアーティファクトにせず RAW フィードへ追記する）、`system-tail`、`system-head`、`tie-break` です。要約はプローブが記録した質問と返答から自分で書くもので、`compact_history` は呼びません。

## 実行

Python 拡張をビルドしてから実行します。

```bash
# No network and no key: only the request-prefix columns are filled.
PYTHONPATH=<build dir> python benchmarks/prompt_cache/prompt_cache_probe.py --offline \
    --preset anthropic.messages --model any-name --turns 14 --compact-at 10 \
    --scenarios none,default,before-history,after-history,raw-append,compaction

# Live: the key is read from the environment variable of the preset.
OPENAI_API_KEY=... PYTHONPATH=<build dir> python benchmarks/prompt_cache/prompt_cache_probe.py \
    --preset openai.chat --model <model> --rates input=...,cache_read=...,output=... \
    --budget-usd 0.05 --turns 16 --scenarios before-history,after-history,compaction

# Where does a provider's cache stop? Live only: four requests per level share the fixed block and that
# many turns of history, and differ after it.
OPENAI_API_KEY=... PYTHONPATH=<build dir> python benchmarks/prompt_cache/prompt_cache_probe.py \
    --preset openai.chat --model <model> --rates input=...,cache_read=...,output=... \
    --budget-usd 0.02 --prefix-ladder 1,2,4 --control reasoning_effort=low
```

- **プリセット。** `openai.chat`、`anthropic.messages`、`google.generate`、`openrouter.chat`。`ProviderControls` のフィールドは `--control NAME=VALUE` で指定し、family のネストはドットで区切ります。Messages のリクエスト単位のキャッシュ制御は `--cache-control 5m` で送ります。プローブは返答を移植可能なテキストとして記録するため、Gemini には `--control generate.history_mode=GeminiHistoryMode.PortableForeign` が必要です。
- **コスト。** ライブ実行には、支出を抑えて報告するための `--rates`（100 万トークンあたりの USD）が必要です。キャッシュヒットが一切ない最悪の場合が `--budget-usd` を超えるなら開始を拒否し、支出がそれを超えたら止まります。DeepSeek のようにバックグラウンドでキャッシュを作るプロバイダーでは、ターンの間に数秒の `--pause-ms` が必要です。
- **キー。** プローブは環境変数からキーを読み、出力も保存もしません。
- **プレフィックスの梯子。** `--prefix-ladder 1,2,4` はシナリオの代わりに動きます。各段階（共有するターン数）で、4 つのリクエストが固定ブロックとその分の履歴を共有し、その後ろのブロックだけが異なります。X、Y、Z、そして対照としての X の再送です。固定ブロックを変えた較正リクエストが、固定ブロックと質問 1 つのトークン数を教えてくれます。共有した履歴の末尾まで読むリクエストは最長の共通プレフィックスに従うキャッシュを、固定ブロックまでしか読まないリクエストは固定ブロックと以前のリクエストの末尾の間に何も保持しないキャッシュを示します。費用は数千分の 1 ドル程度です。
- **出力。** シナリオごとの表と要約が出力され、`--out` を付けると呼び出しごとの記録が JSON で保存されます。`--show-order N` は N 番目の呼び出しのメッセージ順序を出力します。
