<!-- neograph-i18n: source=examples/cookbook/jarvis/bench/pybind/README.md locale=ja source_sha256=097ce5a3394ec214d04a7245b7e4e7e6d937754bb1c18e589a850dc1bc41f54d -->
# Python モード ベンチマーク — NeoGraph-from-Python と LangGraph

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

主な質問: **Python から pybind (ノード本体も Python) 経由で NeoGraph を使用すると、問題は解決しますか?
スタンドアロン C++ の利点 (起動、RSS、スループット)?**

回答: **いいえ。** 肥大化は Python インタープリターによるものではなく、LangChain インポート ツリーによるものです。
NeoGraph-from-Python = リーン Python (10MB/30ms) + 単一のコンパイル済み .so。

## 測定結果 (2026-07-05、WSL2、python3.12)

```bash
cd <neograph>/build-pybind
LD=$(pwd)
PYTHONPATH="$LD" LD_LIBRARY_PATH="$LD" python3 <bench>/pybind/startup_rss.py neograph
PYTHONPATH="$LD" LD_LIBRARY_PATH="$LD" python3 <bench>/pybind/perturn.py   neograph 5000
python3 <bench>/pybind/startup_rss.py langgraph          # bare
python3 <bench>/pybind/startup_rss.py langgraph_openai   # actual chatbot stack
python3 <bench>/pybind/perturn.py   langgraph 5000
```

|メトリック (すべての Python プロセス) | NeoGraph から Python |ランググラフ |利点 |
|---|---|---|---|
|ターンごと (5 つの Python 呼び出し可能ノード、GIL を含む) | **0.38ms · ~2620 回転/秒** | 0.93ms · ~1075 | 2.4倍 |
|起動（インポート→コンパイル） | **40ms** | 462ms (裸) / 2977ms (+langchain_openai) | 11–73× |
| RSS | **36MB** | 61MB (ベア) / 561MB (+langchain_openai) | 1.7～15× |
| (参考) ベア Python3 RSS | — | 9.9MB | |

## Python モードでも高速な理由

- **ターンごと**: BSP エンジン (スーパーステップ ループ、スケジューラー、チャネル リダクション、ルーティング、チェックポイント)
  オーバーヘッド) は C++ で実行され、**ノード本体のみが Python** です。 LangGraph のエンジンは純粋な Python Pregel です。
  GIL は両方のノードの実行中に保持されますが、NeoGraph のノード間でのオーケストレーションは C++ であるため、より高速です。
  pybind/GIL 境界コストはほぼゼロであるため、スタンドアロン C++ モック (9 ノード 0.38 ミリ秒) と Python 5 ノードは
  効果的に結びついた。
- **スタートアップ/RSS**: `import neograph_engine` は単一の .so をロードします。 LangGraph の 462ms/
  61MB は langgraph+langchain-core インポート ツリーで、langchain_openai を使用すると最大 2977ms/561MB になります。 NeoGraph にはそのようなツリーはありません。

## 意味するところ

Python から NeoGraph を使用すると、**Python エコシステム全体 (HF、OpenAI SDK、pandas など) が提供されます。
ノード本体のインライン) + 起動、RSS、スループットの利点を同時に実現**。
つまり、「パフォーマンスは C++ スタンドアロン、エコシステムは Python」という二項対立は間違っています。
Python モードでは両方が可能です。スタンドアロン C++ はさらに一歩先へ (起動 8ms ・ RSS
7.5MB) ただし、ノードが C++ であるか、HTTP 経由で呼び出されるツールの場合に限ります。

注意: ノードが torch/HF をインポートする場合、RSS はそのライブラリによって支配されます。
（エンジン音がうるさい）。これはワークロードのプロパティであり、フレームワークではありません。どちらも同じです。
