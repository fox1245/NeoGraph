<!-- neograph-i18n: source=examples/cookbook/minimal-mcp/README.md locale=ja source_sha256=aabe3dcc4da8e46fba45ac72d14b3c3a206736c485bd63248eb40c1abf57404e -->
# 最小限のMCP — fastmcpなし、SDKなし、APIキーなし

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

このリポジトリの他のMCPの例（03 / 20 / 21 / 22）はすべて、固定のOpenRouter DeepSeekモデルを使用するReActループ内にMCPクライアントをラップするものであり、また、ほとんどのMCPチュートリアルは、`pip install fastmcp`（約60パッケージをpullする）をサーバー側に導入することを前提としています。これによって、便利な事実が隠されています:

> **NeoGraphの組み込みMCPクライアントは、ピア側にはワイヤプロトコルを話すプロセス以外に何も必要としない。**
> ワイヤプロトコルを話すプロセスであり、自身の側には何も必要としません。
> を除く（`libneograph_mcp` には既に含まれている）。**

このクックブックは、最小限の構成でそれを証明する：

- **サーバー**: [`min_stdio_server.py`](min_stdio_server.py) — 約60行の純粋なstdlib Pythonスクリプト。`fastmcp` なし、`mcp` SDKなし、pip installなし。stdin/stdout上の改行区切りJSON-RPCを話し、3つのツール（`get_current_time`、`calculate`、`get_weather`）を公開します。
- **クライアント**: [`client_harness.cpp`](client_harness.cpp) — サーバーをサブプロセスとして起動し、`initialize` → `tools/list` → `tools/call` を実行して、結果を出力します。**LLMなし、APIキーなし。**

## 実行してください

ビルドディレクトリから（`-DNEOGRAPH_BUILD_MCP=ON` でビルド済み。これは例ではデフォルトでオン）：

```bash
./cookbook_minimal_mcp python3 ../examples/cookbook/minimal-mcp/min_stdio_server.py
```

期待される出力：

```
[*] Spawning stdio MCP server: python3 .../min_stdio_server.py
[*] initialize OK
[*] tools/list -> 3 tools:
    - get_current_time: Get the current UTC date and time (ISO format).
    - calculate: Evaluate a simple math expression (+ - * / ** % and parens).
    - get_weather: Return deterministic demo weather for a city.

[*] tools/call round-trips:
    get_current_time({"timezone":"UTC"}) -> 2026-05-31 12:00:00 (UTC)
    calculate({"expression":"2 ** 16 + 1"}) -> 65537
    get_weather({"city":"Tokyo"}) -> Tokyo: 22C, clear (demo)

[*] 3/3 MCP tool calls succeeded (no LLM, no fastmcp)
```

`65537` は、呼び出しが実際にサーバーに到達し、そこで評価されたことを証明します — それは固定文字列ではありません。

## これが重要な理由

- **軽量、両面とも。**「バッテリー同梱」という主張は現実的です：NeoGraphはMCPを静的にリンクするため、別途インストールするパッケージもなく、ドリフトし得る依存関係もありません。*ピア*サーバーはstdlibが許可する限り小さくできます — エッジデバイス、CI、またはフレームワークなしでローカルツールをいくつか公開したい場合に便利です。
- **ピア非依存。** `min_stdio_server.py`を、stdio上でMCPを話す任意の実行可能ファイル（Goバイナリ、Rustサーバー、fastmcp、公式SDK）に置き換えます。C++側は決して変更されません。
- **キーフリープロトコルテスト。** ループ内にLLMがないため、これはエージェントに配線する前に、MCPサーバーの `tools/list` と `tools/call` の形状が正しいことをスモークテストする最も速い方法でもあります。

## エージェントへの組み込み

ラウンドトリップが動作したら、`client.get_tools()`をグラフノード（ツールは通常の`neograph::Tool`インスタンスです）に渡して、LLMが ReAct ループを介して呼び出せるようにします — そのステップについては[`examples/03_mcp_agent.cpp`](../../03_mcp_agent.cpp)を参照してください。
