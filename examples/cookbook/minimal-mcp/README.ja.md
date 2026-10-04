<!-- neograph-i18n: source=examples/cookbook/minimal-mcp/README.md locale=ja source_sha256=a7376fb45ce9458c71f7360172bb26947a1694fa341962f3490ecc028390d1a4 -->
# Minimal MCP — fastmcp と API key なし

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

このリポジトリの他のMCPの例（03 / 20 / 21 / 22）はすべて、固定のOpenRouter DeepSeekモデルを使用するReActループ内にMCPクライアントをラップするものであり、また、ほとんどのMCPチュートリアルは、`pip install fastmcp`（約60パッケージをpullする）をサーバー側に導入することを前提としています。これによって、便利な事実が隠されています:

> peer は stdio MCP protocol を実装すれば十分です。C++ client は
> `libneograph_mcp` と推移的な native 依存をリンクします。peer に Python MCP
> package は不要です。

このクックブックは、最小限の構成でそれを証明する：

- **サーバー**: [`min_stdio_server.py`](min_stdio_server.py) — 約60行の純粋なstdlib Pythonスクリプト。`fastmcp` なし、`mcp` SDKなし、pip installなし。stdin/stdout上の改行区切りJSON-RPCを話し、3つのツール（`get_current_time`、`calculate`、`get_weather`）を公開します。
- **クライアント**: [`client_harness.cpp`](client_harness.cpp) — サーバーをサブプロセスとして起動し、`initialize` → `tools/list` → `tools/call` を実行して、結果を出力します。**LLMなし、APIキーなし。**

## 実行してください

`-DNEOGRAPH_BUILD_EXAMPLES=ON` と `-DNEOGRAPH_BUILD_MCP=ON` で構成した build directory から実行します。モデルを呼ばなくても SchemaProvider は必要です。`CMAKE_PREFIX_PATH` で installed prefix、または `NEOGRAPH_SCHEMAPROVIDER_SOURCE_DIR` で SDK source を指定してください：

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

- **小さな peer。** server は Python 標準 library のみを使います。client には SchemaProvider を含む native build 依存があります。MCP を binary に含めてもこの依存はなくなりません。
- **ピア非依存。** `min_stdio_server.py`を、stdio上でMCPを話す任意の実行可能ファイル（Goバイナリ、Rustサーバー、fastmcp、公式SDK）に置き換えます。C++側は決して変更されません。
- **キーフリープロトコルテスト。** ループ内にLLMがないため、これはエージェントに配線する前に、MCPサーバーの `tools/list` と `tools/call` の形状が正しいことをスモークテストする最も速い方法でもあります。

## エージェントへの組み込み

ラウンドトリップが動作したら、`client.get_tools()`をグラフノード（ツールは通常の`neograph::Tool`インスタンスです）に渡して、LLMが ReAct ループを介して呼び出せるようにします — そのステップについては[`examples/03_mcp_agent.cpp`](../../03_mcp_agent.cpp)を参照してください。
