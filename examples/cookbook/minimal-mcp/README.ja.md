<!-- neograph-i18n: source=examples/cookbook/minimal-mcp/README.md locale=ja source_sha256=018efba21b0004352a4b23c8947e0d18299157eb31070d941304799863f60d82 -->
# 最小限の MCP — fastmcp、SDK、API キーなし

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

このリポジトリ (03 / 20 / 21 / 22) 内の他のすべての MCP サンプルは MCP をラップしています
`OPENAI_API_KEY` とほとんどの MCP を必要とする ReAct ループ内のクライアント
チュートリアルでは、`pip install fastmcp` (約 60 個のパッケージをプルします) を前提としています。
サーバー側で。これには有益な事実が隠されています。

> **NeoGraph の組み込み MCP クライアントは、ピア側には何も必要ありません。
> ワイヤープロトコルを話すプロセス - それ自身の側には何もありません
> `libneograph_mcp` を除く (すでにバイナリ内にあります)。**

このクックブックでは、可能な限り最小のセットアップでそれを証明しています。

- **サーバー**: [`min_stdio_server.py`](min_stdio_server.py) — 約 60 行
  pure-stdlib Python スクリプト。 `fastmcp`、`mcp` SDK、pip インストールはありません。
  改行区切りの JSON-RPC を stdin/stdout 上で通信し、公開します。
  3 つのツール (`get_current_time`、`calculate`、`get_weather`)。
- **クライアント**: [`client_harness.cpp`](client_harness.cpp) — を生成します
  サーバーをサブプロセスとして実行し、`initialize` → `tools/list` →
  `tools/call`、結果を出力します。 **LLM も API キーもありません。**

## 実行してください

ビルド ディレクトリから (`-DNEOGRAPH_BUILD_MCP=ON` でビルドされます。
例としてデフォルトでオンになっています):

```bash
./cookbook_minimal_mcp python3 ../examples/cookbook/minimal-mcp/min_stdio_server.py
```

期待される出力:

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

`65537` は、呼び出しが実際にサーバーに到達し、評価されたことを証明します。
それは缶詰の文字列ではありません。

## なぜこれが重要なのか

- **軽量、両面。** 「バッテリーが付属」という主張は真実です。
  NeoGraph は MCP を静的にリンクするため、個別のパッケージはありません。
  インストールし、ドリフトする可能性のある依存関係はありません。 *ピア* サーバーは次のようになります。
  標準ライブラリが許す限り小さい - エッジデバイス、CI、または次の場合に便利です。
  フレームワークを使用せずに、いくつかのローカル ツールを公開したいだけです。
- **ピアに依存しない。** `min_stdio_server.py` を任意の実行可能ファイルに置き換えます。
  標準入出力 (Go バイナリ、Rust サーバー、fastmcp、
  公式SDK）。 C++ 側は決して変わりません。
- **キーフリー プロトコル テスト。** ループ内に LLM がないため、これは
  これは、MCP サーバーのスモークテストを行う最速の方法でもあります。
  配線前の `tools/list` および `tools/call` の形状は正しい
  エージェントに。

## エージェントに接続する

ラウンドトリップが機能したら、`client.get_tools()` をグラフ ノードに渡します。
(ツールは通常の `neograph::Tool` インスタンスです) そのため、LLM は以下を呼び出すことができます。
ReAct ループを介してそれらを実行します — [`examples/03_mcp_agent.cpp`](../../03_mcp_agent.cpp) を参照
そのステップのために。
