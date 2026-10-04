<!-- neograph-i18n: source=examples/cookbook/jarvis/bench/pybind/README.md locale=ja source_sha256=721dbef65598b467d85737ce2bfa971f2362af4c95c8f724b9310338a1b887e9 -->
# Python グラフベンチマーク: NeoGraph と LangGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

これらのスクリプトは Python グラフの実行とプロセス起動を測定する。モデルプロバイダーを呼び出さず、API キーも不要である。型付きプロバイダーのリクエスト、イベント、結果のバインディングを検証するものではない。以下のコマンドは現行パッケージをインストールした環境向けであり、今回の更新ではソースを照合しただけで実行していない。

## 再現

現行の `neograph-engine` wheel をインストールした Python 環境を使う。比較用に `langgraph`、大きな import スタックの測定用に `langchain-openai` をインストールする。リポジトリのルートで実行する。

```bash
python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py neograph
python3 examples/cookbook/jarvis/bench/pybind/perturn.py neograph 5000
python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py langgraph
python3 examples/cookbook/jarvis/bench/pybind/startup_rss.py langgraph_openai
python3 examples/cookbook/jarvis/bench/pybind/perturn.py langgraph 5000
```

`perturn.py` はウォームアップ後に五つのノードのチェーンを繰り返す。各 Python ノードは `v` チャネルを一つ増やし、各測定実行はゼロから始まる。平均、p50、p90、毎秒の実行回数を表示する。実行回数には正の値を指定する。

`startup_rss.py` は新しいプロセスで起動時間（ms）と最大 RSS を表示する。NeoGraph 分岐はパッケージを import して三つのグラフシンボルを参照するだけで、グラフを compile しない。LangGraph 分岐はパッケージを import し、一つのノードのグラフを compile する。作業範囲が異なるため、時間比率をグラフ compile の高速化と解釈してはならない。RSS 換算は Linux の `ru_maxrss` 単位 KiB を前提とする。macOS では同じ換算をそのまま使わない。Windows には Python の `resource` モジュールがない。

## 過去の測定

リポジトリは以前、以下の値を報告した。現行の移行ビルドの測定値ではなく、今回の更新では再実行していない。

| 指標 | Python の NeoGraph | LangGraph |
|---|---|---|
| 実行ごとに五つの Python ノード | 0.38 ms; 約 2620 回/s | 0.93 ms; 約 1075 回/s |
| 上記の異なる範囲での起動 | 40 ms | 462 ms; `langchain_openai` を含む場合は 2977 ms |
| 最大 RSS | 36 MB | 61 MB; `langchain_openai` を含む場合は 561 MB |

NeoGraph はグラフスケジューラーとチャネル reduction を C++ で実行し、Python ノード本体は GIL を取得する。この測定は GIL 境界の費用を分離せず、他のワークロードの性能を証明するものでもない。ノード内で PyTorch などを import すると、そのメモリもプロセスの使用量に加わる。
