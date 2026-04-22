# NeoGraph API Reference

**NeoGraph** is a C++ graph agent engine library -- a C++ alternative to LangGraph.
It provides a modular architecture for building LLM-powered agent workflows as
directed graphs with state management, checkpointing, streaming, and tool integration.

**Modules:**

| Module | Namespace | Description |
|--------|-----------|-------------|
| Core | `neograph` | Foundation types, Provider and Tool interfaces |
| Graph | `neograph::graph` | Graph engine, nodes, state, checkpointing, store |
| LLM | `neograph::llm` | LLM provider implementations and Agent |
| MCP | `neograph::mcp` | Model Context Protocol client |
| Util | `neograph::util` | Concurrency utilities |

**Convenience header:** `#include <neograph/neograph.h>` includes the full core + graph engine API.

---

## Table of Contents

- [1. Foundation Types](#1-foundation-types)
  - [ToolCall](#toolcall)
  - [ChatMessage](#chatmessage)
  - [ChatTool](#chattool)
  - [ChatCompletion](#chatcompletion)
  - [Helper Functions](#helper-functions)
  - [ADL Serialization](#adl-serialization)
- [2. Provider Interface](#2-provider-interface)
  - [StreamCallback](#streamcallback)
  - [CompletionParams](#completionparams)
  - [Provider](#provider)
- [3. Tool Interface](#3-tool-interface)
  - [Tool](#tool)
- [4. Graph Types](#4-graph-types)
  - [ReducerType](#reducertype)
  - [ReducerFn](#reducerfn)
  - [Channel](#channel)
  - [ChannelWrite](#channelwrite)
  - [NodeInterrupt](#nodeinterrupt)
  - [Send](#send)
  - [Command](#command)
  - [RetryPolicy](#retrypolicy)
  - [StreamMode](#streammode)
  - [Edge](#edge)
  - [ConditionalEdge](#conditionaledge)
  - [NodeContext](#nodecontext)
  - [GraphEvent](#graphevent)
  - [GraphStreamCallback](#graphstreamcallback)
  - [NodeResult](#noderesult)
  - [ConditionFn](#conditionfn)
  - [Constants](#constants)
- [5. GraphState](#5-graphstate)
- [6. GraphNode](#6-graphnode)
  - [GraphNode (abstract)](#graphnode-abstract)
  - [LLMCallNode](#llmcallnode)
  - [ToolDispatchNode](#tooldispatchnode)
  - [IntentClassifierNode](#intentclassifiernode)
  - [SubgraphNode](#subgraphnode)
- [7. GraphEngine](#7-graphengine)
  - [RunConfig](#runconfig)
  - [RunResult](#runresult)
  - [GraphEngine](#graphengine-1)
- [7b. Engine Internals](#7b-engine-internals)
  - [GraphCompiler](#graphcompiler)
  - [Scheduler](#scheduler)
  - [CheckpointCoordinator](#checkpointcoordinator)
  - [NodeExecutor](#nodeexecutor)
- [8. Checkpoint](#8-checkpoint)
  - [Checkpoint (struct)](#checkpoint-struct)
  - [CheckpointStore](#checkpointstore)
  - [InMemoryCheckpointStore](#inmemorycheckpointstore)
- [9. Store](#9-store)
  - [Namespace](#namespace)
  - [StoreItem](#storeitem)
  - [Store (abstract)](#store-abstract)
  - [InMemoryStore](#inmemorystore)
- [10. Loader](#10-loader)
  - [ReducerRegistry](#reducerregistry)
  - [ConditionRegistry](#conditionregistry)
  - [NodeFactory](#nodefactory)
  - [Built-in Registrations](#built-in-registrations)
- [11. React Graph](#11-react-graph)
- [12. LLM Module](#12-llm-module)
  - [OpenAIProvider](#openaiprovider)
  - [SchemaProvider](#schemaprovider)
  - [Agent](#agent)
  - [json_path Utilities](#json_path-utilities)
- [13. MCP Module](#13-mcp-module)
  - [MCPTool](#mcptool)
  - [MCPClient](#mcpclient)
- [14. Util Module](#14-util-module)
  - [RequestQueue](#requestqueue)
- [Usage Examples](#usage-examples)
  - [Minimal ReAct Agent](#minimal-react-agent)
  - [Custom Graph with Conditional Routing](#custom-graph-with-conditional-routing)
  - [Human-in-the-Loop with Checkpointing](#human-in-the-loop-with-checkpointing)
  - [Dynamic Fan-Out with Send](#dynamic-fan-out-with-send)
  - [Routing Override with Command](#routing-override-with-command)
  - [SchemaProvider Multi-LLM Support](#schemaprovider-multi-llm-support)
  - [MCP Tool Integration](#mcp-tool-integration)

---

## 1. Foundation Types

**Header:** `<neograph/types.h>`
**Namespace:** `neograph`

Core data types shared across all modules. These model the LLM chat protocol:
messages, tool calls, completions, and their JSON serialization.

### ToolCall

Represents a single tool invocation requested by the LLM.

```cpp
struct ToolCall {
    std::string id;         // Unique identifier assigned by the LLM
    std::string name;       // Name of the tool to call
    std::string arguments;  // JSON-encoded string of arguments
};
```

| Field | Type | Description |
|-------|------|-------------|
| `id` | `std::string` | Unique identifier for this tool call (assigned by the LLM) |
| `name` | `std::string` | Name of the tool function to invoke |
| `arguments` | `std::string` | JSON-encoded string containing the call arguments |

### ChatMessage

A single message in a conversation. Covers all roles: system, user, assistant, and tool.

```cpp
struct ChatMessage {
    std::string role;                    // "system", "user", "assistant", or "tool"
    std::string content;                 // Text content of the message
    std::vector<ToolCall> tool_calls;    // Tool calls (assistant messages only)
    std::string tool_call_id;           // ID of the tool call this responds to (tool messages)
    std::string tool_name;              // Name of the tool (tool messages)
    std::vector<std::string> image_urls; // base64 data URLs or HTTP URLs for Vision
};
```

| Field | Type | Description |
|-------|------|-------------|
| `role` | `std::string` | Message role: `"system"`, `"user"`, `"assistant"`, or `"tool"` |
| `content` | `std::string` | Text content of the message |
| `tool_calls` | `std::vector<ToolCall>` | Tool calls requested by the assistant (empty for non-assistant messages) |
| `tool_call_id` | `std::string` | ID linking this tool result to its originating tool call |
| `tool_name` | `std::string` | Name of the tool that produced this result |
| `image_urls` | `std::vector<std::string>` | Image URLs for multi-modal/vision messages. Accepts `data:image/...;base64,...` or `https://...` |

### ChatTool

Defines a tool available to the LLM.

```cpp
struct ChatTool {
    std::string name;        // Tool name (unique identifier)
    std::string description; // Human-readable description for the LLM
    json parameters;         // JSON Schema describing the tool's parameters
};
```

| Field | Type | Description |
|-------|------|-------------|
| `name` | `std::string` | Unique tool name |
| `description` | `std::string` | Description shown to the LLM to explain the tool's purpose |
| `parameters` | `json` | JSON Schema object describing accepted parameters |

### ChatCompletion

Result of a single LLM completion call.

```cpp
struct ChatCompletion {
    ChatMessage message;   // The assistant's response message
    struct Usage {
        int prompt_tokens = 0;      // Tokens in the prompt
        int completion_tokens = 0;  // Tokens in the completion
        int total_tokens = 0;       // Total tokens used
    } usage;
};
```

| Field | Type | Description |
|-------|------|-------------|
| `message` | `ChatMessage` | The assistant's response (may include tool calls) |
| `usage.prompt_tokens` | `int` | Number of tokens in the input prompt |
| `usage.completion_tokens` | `int` | Number of tokens in the generated completion |
| `usage.total_tokens` | `int` | Total tokens consumed (prompt + completion) |

### Helper Functions

#### `messages_to_json`

Converts a message vector to the OpenAI-compatible JSON wire format. Handles tool call
messages, tool result messages, and multi-modal (vision) messages with appropriate structure.

```cpp
json messages_to_json(const std::vector<ChatMessage>& messages);
```

**Returns:** A `json` array where each element is a properly formatted message object.

#### `tools_to_json`

Converts tool definitions to the OpenAI-compatible JSON wire format, wrapping each tool
in a `{type: "function", function: {...}}` envelope.

```cpp
json tools_to_json(const std::vector<ChatTool>& tools);
```

**Returns:** A `json` array of tool definitions.

#### `parse_response_message`

Parses a single choice object from an OpenAI-format API response into a `ChatMessage`.
Extracts the assistant's content and any tool calls from the `message` field.

```cpp
ChatMessage parse_response_message(const json& choice);
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `choice` | `const json&` | A single element from the `choices` array (must contain a `message` field) |

**Returns:** A `ChatMessage` with role, content, and any tool calls populated.

### ADL Serialization

Argument-Dependent Lookup (ADL) serialization functions for nlohmann/json integration.
These allow direct use with `json j = my_tool_call;` and `my_tool_call = j.get<ToolCall>()`.

```cpp
void to_json(json& j, const ToolCall& tc);
void from_json(const json& j, ToolCall& tc);

void to_json(json& j, const ChatMessage& msg);
void from_json(const json& j, ChatMessage& msg);
```

All fields use `value()` with empty-string defaults, making deserialization tolerant
of missing fields.

---

## 2. Provider Interface

**Header:** `<neograph/provider.h>`
**Namespace:** `neograph`

The abstract interface for LLM backends. Implement this to add support for any LLM API.

> **Writing a custom Provider subclass?** See
> [`ASYNC_GUIDE.md` §9.3](ASYNC_GUIDE.md#93-provider) for the
> decision matrix on whether to override `complete()`,
> `complete_async()`, or both.

### StreamCallback

Type alias for the streaming token callback.

```cpp
using StreamCallback = std::function<void(const std::string& chunk)>;
```

Called once per token (or chunk) during streaming completion. The `chunk` parameter
contains the incremental text fragment.

### CompletionParams

Parameters for a single LLM completion request.

```cpp
struct CompletionParams {
    std::string model;                // Model identifier (e.g. "gpt-4o")
    std::vector<ChatMessage> messages; // Conversation history
    std::vector<ChatTool> tools;      // Available tools (empty = no tool use)
    float temperature = 0.7f;         // Sampling temperature
    int max_tokens = -1;              // Max tokens to generate (-1 = provider default)
};
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `model` | `std::string` | `""` | Model to use. If empty, the provider's default model is used |
| `messages` | `std::vector<ChatMessage>` | | Conversation messages in chronological order |
| `tools` | `std::vector<ChatTool>` | `{}` | Tools available for the LLM to call. Empty disables tool use |
| `temperature` | `float` | `0.7f` | Sampling temperature (0.0 = deterministic, higher = more random) |
| `max_tokens` | `int` | `-1` | Maximum tokens to generate. `-1` lets the provider decide |

### Provider

Abstract base class for LLM providers. All LLM backends must implement this interface.

```cpp
class Provider {
public:
    virtual ~Provider() = default;

    // Synchronous completion
    virtual ChatCompletion complete(const CompletionParams& params) = 0;

    // Streaming completion: calls on_chunk per token, returns full result when done
    virtual ChatCompletion complete_stream(const CompletionParams& params,
                                           const StreamCallback& on_chunk) = 0;

    // Provider name (e.g. "openai", "claude")
    virtual std::string get_name() const = 0;
};
```

| Method | Description |
|--------|-------------|
| `complete(params)` | Perform a blocking completion. Returns the full `ChatCompletion` |
| `complete_stream(params, on_chunk)` | Streaming completion. Calls `on_chunk` for each token as it arrives, then returns the assembled `ChatCompletion` |
| `get_name()` | Returns a human-readable provider identifier |

---

## 3. Tool Interface

**Header:** `<neograph/tool.h>`
**Namespace:** `neograph`

Abstract interface for tools that LLMs can call. Implement this to expose functions
to the agent.

> **Writing a custom Tool subclass?** See
> [`ASYNC_GUIDE.md` §9.6](ASYNC_GUIDE.md#96-tool-vs-asynctool) for
> when to inherit `Tool` (sync) vs `AsyncTool` (async). The two are
> mutually exclusive — pick one.

### Tool

```cpp
class Tool {
public:
    virtual ~Tool() = default;

    // Returns the tool's definition (name, description, parameter schema)
    virtual ChatTool get_definition() const = 0;

    // Executes the tool with the given arguments, returns result as string
    virtual std::string execute(const json& arguments) = 0;

    // Returns the tool's unique name
    virtual std::string get_name() const = 0;
};
```

| Method | Returns | Description |
|--------|---------|-------------|
| `get_definition()` | `ChatTool` | Returns the tool's metadata including JSON Schema for parameters |
| `execute(arguments)` | `std::string` | Runs the tool with parsed JSON arguments. Returns the result as a string that will be sent back to the LLM |
| `get_name()` | `std::string` | Unique identifier for this tool |

**Example implementation:**

```cpp
class WeatherTool : public neograph::Tool {
public:
    ChatTool get_definition() const override {
        return {"get_weather", "Get current weather for a city", json::parse(R"({
            "type": "object",
            "properties": {
                "city": {"type": "string", "description": "City name"}
            },
            "required": ["city"]
        })")};
    }

    std::string execute(const json& args) override {
        std::string city = args.at("city");
        return "Weather in " + city + ": 22C, sunny";
    }

    std::string get_name() const override { return "get_weather"; }
};
```

---

## 4. Graph Types

**Header:** `<neograph/graph/types.h>`
**Namespace:** `neograph::graph`

Core types for the graph engine: channels, edges, events, and control-flow primitives.

### ReducerType

Determines how channel values are merged when written by multiple nodes.

```cpp
enum class ReducerType {
    OVERWRITE,  // New value replaces old value
    APPEND,     // New value is appended (for array channels)
    CUSTOM      // User-defined reducer function
};
```

### ReducerFn

Signature for custom reducer functions.

```cpp
using ReducerFn = std::function<json(const json& current, const json& incoming)>;
```

| Parameter | Description |
|-----------|-------------|
| `current` | The current channel value |
| `incoming` | The new value being written |

**Returns:** The merged result that becomes the new channel value.

### Channel

Internal representation of a named, versioned state channel with an associated reducer.

```cpp
struct Channel {
    std::string name;                              // Channel name
    ReducerType reducer_type = ReducerType::OVERWRITE; // Merge strategy
    ReducerFn   reducer;                           // Custom reducer (when type == CUSTOM)
    json        value;                             // Current value
    uint64_t    version = 0;                       // Write counter
};
```

### ChannelWrite

A single write operation targeting a named channel. Nodes return vectors of these.

```cpp
struct ChannelWrite {
    std::string channel;  // Target channel name
    json        value;    // Value to write (merged via the channel's reducer)
};
```

### NodeInterrupt

Exception type thrown from within a node to trigger a dynamic breakpoint (human-in-the-loop).
When thrown, execution pauses, a checkpoint is saved, and the interrupt can be resumed later.

```cpp
class NodeInterrupt : public std::runtime_error {
public:
    explicit NodeInterrupt(const std::string& reason);
    const std::string& reason() const;
};
```

| Method | Returns | Description |
|--------|---------|-------------|
| `reason()` | `const std::string&` | The reason string passed to the constructor |

**Usage inside a node:**

```cpp
std::vector<ChannelWrite> execute(const GraphState& state) override {
    auto input = state.get("user_input");
    if (needs_approval(input)) {
        throw NodeInterrupt("Requires human approval");
    }
    // ... normal execution
}
```

### Send

Represents a dynamic fan-out request. A node can return `Send` objects to dispatch
one or more nodes with different inputs, enabling map-reduce patterns.

```cpp
struct Send {
    std::string target_node;  // Node to dispatch
    json        input;        // Channel writes for that invocation
};
```

The engine executes each `Send` target with its own input, then continues the graph
after all sends complete. Multiple sends to the same node run in sequence.

### Command

Combined routing override and state update. A node returns a `Command` to simultaneously
write state updates AND redirect execution to a specific next node, bypassing normal
edge routing.

```cpp
struct Command {
    std::string               goto_node;  // Next node (overrides edge routing)
    std::vector<ChannelWrite> updates;    // State updates to apply
};
```

| Field | Type | Description |
|-------|------|-------------|
| `goto_node` | `std::string` | Name of the node to execute next. Overrides normal edge resolution |
| `updates` | `std::vector<ChannelWrite>` | Channel writes to apply before routing |

### RetryPolicy

Configures automatic retry behavior for node execution failures.

```cpp
struct RetryPolicy {
    int   max_retries        = 0;      // 0 = no retry
    int   initial_delay_ms   = 100;    // First retry delay in milliseconds
    float backoff_multiplier = 2.0f;   // Exponential backoff factor
    int   max_delay_ms       = 5000;   // Maximum delay cap in milliseconds
};
```

Delay for retry `n` is `min(initial_delay_ms * backoff_multiplier^n, max_delay_ms)`.

### StreamMode

Bitfield flags controlling which events are emitted during streaming execution.

```cpp
enum class StreamMode : uint8_t {
    EVENTS  = 0x01,  // NODE_START, NODE_END, INTERRUPT, ERROR
    TOKENS  = 0x02,  // LLM_TOKEN (individual tokens from streaming LLM calls)
    VALUES  = 0x04,  // Full state snapshot after each step
    UPDATES = 0x08,  // Channel write deltas per node
    DEBUG   = 0x10,  // Internal debug info (retry attempts, routing decisions)
    ALL     = 0xFF   // All event types
};
```

Combine flags with bitwise OR:

```cpp
StreamMode mode = StreamMode::EVENTS | StreamMode::TOKENS;
```

**Operators:**

```cpp
StreamMode operator|(StreamMode a, StreamMode b);  // Combine flags
StreamMode operator&(StreamMode a, StreamMode b);  // Mask flags
bool has_mode(StreamMode flags, StreamMode test);   // Test if flag is set
```

### Edge

A static directed edge between two nodes.

```cpp
struct Edge {
    std::string from;  // Source node name
    std::string to;    // Target node name
};
```

Use the special constants `START_NODE` and `END_NODE` for graph entry and exit points.

### ConditionalEdge

A dynamic edge whose target is determined at runtime by a named condition function.

```cpp
struct ConditionalEdge {
    std::string from;                              // Source node name
    std::string condition;                         // Name in ConditionRegistry
    std::map<std::string, std::string> routes;     // condition_result -> target node name
};
```

At runtime, the engine calls the condition function (looked up by name in `ConditionRegistry`).
The function's return value is used as a key into the `routes` map to determine the next node.

### NodeContext

Dependency injection container passed to node constructors. Provides access to the
LLM provider, tools, and configuration.

```cpp
struct NodeContext {
    std::shared_ptr<Provider> provider;   // LLM provider
    std::vector<Tool*>        tools;      // Available tools (non-owning; engine owns the unique_ptrs)
    std::string               model;      // Model override (empty = provider default)
    std::string               instructions; // System prompt / instructions
    json                      extra_config; // Additional configuration (node-type-specific)
};
```

### GraphEvent

Event emitted during streaming graph execution.

```cpp
struct GraphEvent {
    enum class Type {
        NODE_START,     // A node is about to execute
        NODE_END,       // A node has finished executing
        LLM_TOKEN,      // A single token from a streaming LLM call
        CHANNEL_WRITE,  // A channel value was updated
        INTERRUPT,      // Execution paused (NodeInterrupt or configured breakpoint)
        ERROR           // An error occurred during execution
    };

    Type        type;       // Event type
    std::string node_name;  // Name of the node that produced this event
    json        data;       // Event payload (varies by type)
};
```

**Event data payloads:**

| Type | `data` contents |
|------|-----------------|
| `NODE_START` | `{}` or node metadata |
| `NODE_END` | Channel writes produced by the node |
| `LLM_TOKEN` | `{"token": "..."}` |
| `CHANNEL_WRITE` | `{"channel": "...", "value": ...}` |
| `INTERRUPT` | `{"reason": "...", "node": "..."}` |
| `ERROR` | `{"error": "...", "node": "..."}` |

### GraphStreamCallback

Type alias for the graph event callback used in streaming execution.

```cpp
using GraphStreamCallback = std::function<void(const GraphEvent&)>;
```

### NodeResult

Extended return type from node execution. Wraps channel writes with optional
`Command` and `Send` directives for advanced control flow.

```cpp
struct NodeResult {
    std::vector<ChannelWrite> writes;           // Channel updates
    std::optional<Command>    command;           // Routing override (if set)
    std::vector<Send>         sends;             // Dynamic fan-out targets

    NodeResult() = default;
    NodeResult(std::vector<ChannelWrite> w);     // Implicit from plain writes
};
```

When `command` is set, normal edge routing is bypassed and execution jumps to
`command->goto_node`. When `sends` is non-empty, the engine performs dynamic
fan-out to the specified targets.

### ConditionFn

Signature for condition functions used in conditional edges.

```cpp
using ConditionFn = std::function<std::string(const GraphState&)>;
```

The function inspects the current graph state and returns a string key. This key is
looked up in the `ConditionalEdge::routes` map to determine the next node.

### Constants

```cpp
constexpr const char* START_NODE = "__start__";  // Graph entry point
constexpr const char* END_NODE   = "__end__";    // Graph termination
```

These are used in edge definitions to mark graph entry and exit:

```cpp
Edge{START_NODE, "my_first_node"}
Edge{"my_last_node", END_NODE}
```

---

## 5. GraphState

**Header:** `<neograph/graph/state.h>`
**Namespace:** `neograph::graph`

Thread-safe, versioned key-value state container for the graph. Each entry is a
named channel with an associated reducer that controls how values are merged.

```cpp
class GraphState {
public:
    void init_channel(const std::string& name,
                      ReducerType type,
                      ReducerFn reducer,
                      const json& initial_value = json());

    json get(const std::string& channel) const;
    std::vector<ChatMessage> get_messages() const;

    void write(const std::string& channel, const json& value);
    void apply_writes(const std::vector<ChannelWrite>& writes);

    uint64_t channel_version(const std::string& channel) const;
    uint64_t global_version() const;

    json serialize() const;
    void restore(const json& data);

    std::vector<std::string> channel_names() const;
};
```

| Method | Description |
|--------|-------------|
| `init_channel(name, type, reducer, initial_value)` | Register a channel with its reducer and optional initial value. Must be called before any read/write to that channel |
| `get(channel)` | Read the current value of a channel. Thread-safe (shared lock) |
| `get_messages()` | Convenience method: reads the `"messages"` channel and deserializes it as `std::vector<ChatMessage>` |
| `write(channel, value)` | Write a value to a single channel through its reducer. Thread-safe (exclusive lock) |
| `apply_writes(writes)` | Atomically apply a batch of `ChannelWrite` operations. All writes are applied under a single exclusive lock |
| `channel_version(channel)` | Returns the write counter for a specific channel |
| `global_version()` | Returns the global version counter (incremented on every write to any channel) |
| `serialize()` | Serializes all channel values and versions to JSON (for checkpointing) |
| `restore(data)` | Restores channel values and versions from serialized JSON |
| `channel_names()` | Returns the names of all initialized channels |

---

## 6. GraphNode

**Header:** `<neograph/graph/node.h>`
**Namespace:** `neograph::graph`

Nodes are the computational units of a graph. The library provides an abstract base
class and four built-in node types.

> **Writing a custom GraphNode subclass?** There are four `execute*`
> virtuals and their four async peers (8 total). Picking the wrong
> one silently drops `Command` / `Send`, freezes the event loop, or
> infinite-recurses. See
> [`ASYNC_GUIDE.md` §9.2](ASYNC_GUIDE.md#92-graphnode--the-four-quadrant-matrix)
> for the full decision matrix and the known pitfalls.

### GraphNode (abstract)

Base class for all graph nodes.

```cpp
class GraphNode {
public:
    virtual ~GraphNode() = default;

    // Basic execution: read state, return channel writes
    virtual std::vector<ChannelWrite> execute(const GraphState& state) = 0;

    // Streaming variant (default: delegates to execute)
    virtual std::vector<ChannelWrite> execute_stream(
        const GraphState& state, const GraphStreamCallback& cb);

    // Extended execute: returns NodeResult with optional Command/Send
    // Default: wraps execute() result into NodeResult.writes
    virtual NodeResult execute_full(const GraphState& state);

    // Extended streaming variant
    virtual NodeResult execute_full_stream(
        const GraphState& state, const GraphStreamCallback& cb);

    virtual std::string get_name() const = 0;
};
```

| Method | Description |
|--------|-------------|
| `execute(state)` | Core execution. Read from state, perform computation, return channel writes |
| `execute_stream(state, cb)` | Streaming variant. Emits `GraphEvent`s via `cb` during execution. Default implementation delegates to `execute()` |
| `execute_full(state)` | Extended execution returning `NodeResult`. Override this to use `Command` or `Send`. Default wraps `execute()` output |
| `execute_full_stream(state, cb)` | Streaming extended execution. Default delegates to `execute_stream()` and wraps result |
| `get_name()` | Returns the node's unique name within the graph |

To support `Send` or `Command`, override `execute_full()` (or `execute_full_stream()`
for streaming). The engine always calls the `execute_full*` variants internally.

### LLMCallNode

Calls the LLM with the current conversation state. Reads from the `"messages"` channel,
sends a completion request to the provider, and writes the assistant's response back
to the `"messages"` channel.

```cpp
class LLMCallNode : public GraphNode {
public:
    LLMCallNode(const std::string& name, const NodeContext& ctx);

    std::vector<ChannelWrite> execute(const GraphState& state) override;
    std::vector<ChannelWrite> execute_stream(
        const GraphState& state, const GraphStreamCallback& cb) override;
    std::string get_name() const override;

private:
    CompletionParams build_params(const GraphState& state) const;
};
```

| Constructor Parameter | Description |
|-----------------------|-------------|
| `name` | Node name |
| `ctx` | Node context providing the LLM provider, tools, model, and instructions |

The `execute_stream` override emits `LLM_TOKEN` events for each streamed token.

### ToolDispatchNode

Dispatches tool calls from the latest assistant message. Reads pending tool calls from
the `"messages"` channel, executes each tool, and writes tool result messages back.

```cpp
class ToolDispatchNode : public GraphNode {
public:
    ToolDispatchNode(const std::string& name, const NodeContext& ctx);

    std::vector<ChannelWrite> execute(const GraphState& state) override;
    std::string get_name() const override;
};
```

| Constructor Parameter | Description |
|-----------------------|-------------|
| `name` | Node name |
| `ctx` | Node context (uses `ctx.tools` to look up and execute tools) |

### IntentClassifierNode

Uses the LLM to classify user intent, then writes the classification result to the
`"__route__"` channel. Designed for use with the `"route_channel"` built-in condition
to enable dynamic intent-based routing.

```cpp
class IntentClassifierNode : public GraphNode {
public:
    IntentClassifierNode(const std::string& name, const NodeContext& ctx,
                         const std::string& prompt,
                         std::vector<std::string> valid_routes);

    std::vector<ChannelWrite> execute(const GraphState& state) override;
    std::string get_name() const override;
};
```

| Constructor Parameter | Type | Description |
|-----------------------|------|-------------|
| `name` | `std::string` | Node name |
| `ctx` | `NodeContext` | Provider and model for the classification LLM call |
| `prompt` | `std::string` | Classification prompt template |
| `valid_routes` | `std::vector<std::string>` | Allowed classification values. The LLM output is validated against these |

### SubgraphNode

Wraps a compiled `GraphEngine` as a single node, enabling hierarchical graph composition
(supervisor pattern, nested workflows). Channel mappings control data flow between
parent and child graphs.

```cpp
class SubgraphNode : public GraphNode {
public:
    SubgraphNode(const std::string& name,
                 std::shared_ptr<GraphEngine> subgraph,
                 std::map<std::string, std::string> input_map = {},
                 std::map<std::string, std::string> output_map = {});

    std::vector<ChannelWrite> execute(const GraphState& state) override;
    std::vector<ChannelWrite> execute_stream(
        const GraphState& state, const GraphStreamCallback& cb) override;
    std::string get_name() const override;
};
```

| Constructor Parameter | Type | Description |
|-----------------------|------|-------------|
| `name` | `std::string` | Node name in the parent graph |
| `subgraph` | `std::shared_ptr<GraphEngine>` | The compiled child graph engine |
| `input_map` | `std::map<std::string, std::string>` | `parent_channel -> child_channel` mapping. Read from parent, write to child input |
| `output_map` | `std::map<std::string, std::string>` | `child_channel -> parent_channel` mapping. Read from child result, write to parent |

If the maps are empty, channels are mapped by name (identity mapping).

---

## 7. GraphEngine

**Header:** `<neograph/graph/engine.h>`
**Namespace:** `neograph::graph`

The core execution engine. Compiles graph definitions, manages state transitions,
and orchestrates node execution through a super-step loop.

### RunConfig

Configuration for a single graph execution run.

```cpp
struct RunConfig {
    std::string thread_id;                          // Checkpoint association key
    json        input;                              // Initial channel writes
    int         max_steps    = 50;                  // Safety limit for loops
    StreamMode  stream_mode  = StreamMode::ALL;     // Which events to emit
};
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `thread_id` | `std::string` | `""` | Identifies the conversation/session for checkpointing |
| `input` | `json` | `{}` | Initial values written to channels before execution starts. Typically `{"messages": [...]}` |
| `max_steps` | `int` | `50` | Maximum number of super-steps before forced termination (prevents infinite loops) |
| `stream_mode` | `StreamMode` | `ALL` | Bitfield controlling which event types are emitted during streaming |

### RunResult

Result returned after graph execution completes or is interrupted.

```cpp
struct RunResult {
    json        output;                          // Final serialized state
    bool        interrupted       = false;       // True if execution was paused (HITL)
    std::string interrupt_node;                  // Node that caused the interrupt
    json        interrupt_value;                 // Value associated with the interrupt
    std::string checkpoint_id;                   // ID of the last checkpoint saved
    std::vector<std::string> execution_trace;    // Ordered list of executed node names
};
```

| Field | Type | Description |
|-------|------|-------------|
| `output` | `json` | Serialized final state of all channels |
| `interrupted` | `bool` | `true` if execution was paused by an interrupt (HITL) |
| `interrupt_node` | `std::string` | Name of the node that triggered the interrupt |
| `interrupt_value` | `json` | Reason or payload from the interrupt |
| `checkpoint_id` | `std::string` | UUID of the last saved checkpoint |
| `execution_trace` | `std::vector<std::string>` | Ordered list of node names in execution order |

### GraphEngine

The main engine class. Created via the static `compile()` method.

```cpp
class GraphEngine {
public:
    // ---- Construction ----

    static std::unique_ptr<GraphEngine> compile(
        const json& definition,
        const NodeContext& default_context,
        std::shared_ptr<CheckpointStore> store = nullptr);

    // ---- Execution (sync) ----

    RunResult run(const RunConfig& config);

    RunResult run_stream(const RunConfig& config,
                         const GraphStreamCallback& cb);

    RunResult resume(const std::string& thread_id,
                     const json& resume_value = json(),
                     const GraphStreamCallback& cb = nullptr);

    // ---- Execution (async, 3.0) ----

    asio::awaitable<RunResult> run_async(const RunConfig& config);

    asio::awaitable<RunResult> run_stream_async(
        const RunConfig& config, const GraphStreamCallback& cb);

    asio::awaitable<RunResult> resume_async(
        const std::string& thread_id,
        const json& resume_value = json(),
        const GraphStreamCallback& cb = nullptr);

    // ---- State Inspection & Manipulation ----

    std::optional<json> get_state(const std::string& thread_id) const;

    std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                              int limit = 100) const;

    void update_state(const std::string& thread_id,
                      const json& channel_writes,
                      const std::string& as_node = "");

    std::string fork(const std::string& source_thread_id,
                     const std::string& new_thread_id,
                     const std::string& checkpoint_id = "");

    // ---- Configuration ----

    void own_tools(std::vector<std::unique_ptr<Tool>> tools);
    void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);
    void set_store(std::shared_ptr<Store> store);
    std::shared_ptr<Store> get_store() const;
    void set_retry_policy(const RetryPolicy& policy);
    void set_node_retry_policy(const std::string& node_name, const RetryPolicy& policy);
    void set_worker_count(std::size_t n);  // 3.0: opt-in fan-out pool
    const std::string& get_graph_name() const;
};
```

#### `compile`

```cpp
static std::unique_ptr<GraphEngine> compile(
    const json& definition,
    const NodeContext& default_context,
    std::shared_ptr<CheckpointStore> store = nullptr);
```

Compiles a graph from a JSON definition and returns an engine ready for execution.

| Parameter | Type | Description |
|-----------|------|-------------|
| `definition` | `const json&` | Graph definition in JSON format (see below) |
| `default_context` | `const NodeContext&` | Default context injected into all nodes |
| `store` | `std::shared_ptr<CheckpointStore>` | Optional checkpoint store for persistence |

**Graph definition JSON schema:**

```json
{
  "name": "my_graph",
  "channels": [
    {"name": "messages", "type": "append"},
    {"name": "status", "type": "overwrite", "initial": "idle"}
  ],
  "nodes": [
    {"name": "llm", "type": "llm_call"},
    {"name": "tools", "type": "tool_dispatch"}
  ],
  "edges": [
    {"from": "__start__", "to": "llm"},
    {"from": "tools", "to": "llm"}
  ],
  "conditional_edges": [
    {
      "from": "llm",
      "condition": "has_tool_calls",
      "routes": {"yes": "tools", "no": "__end__"}
    }
  ],
  "interrupt_before": [],
  "interrupt_after": ["tools"]
}
```

##### Barrier nodes (AND-join opt-in)

A node declaration may include a `barrier` field to opt into AND-join
semantics for that specific node. Under the default signal-dispatch
model, a node fires every super-step that any upstream routes to it
— which double-fires join nodes on asymmetric serial fan-in (paths
of different lengths). A barrier gates the node until **all** listed
upstreams have signaled at least once (across any number of
super-steps):

```json
"join": {
  "type": "my_join",
  "barrier": {"wait_for": ["a", "s2"]}
}
```

Fires once when both `a` and `s2` have signaled. State resets on
fire, so loops through the barrier collect fresh signals each round.

**Persistence:** since `CHECKPOINT_SCHEMA_VERSION = 2`, the barrier
accumulator is persisted on every checkpoint (`Checkpoint::barrier_state`,
a `map<string, set<string>>`) and restored on resume. Interrupts that
land mid-accumulation are therefore safe — the partial upstream set
survives the pause and the barrier fires as soon as the remaining
signals arrive. v1 blobs deserialize with an empty `barrier_state`,
matching pre-v2 behavior for those stored checkpoints.

#### `run`

```cpp
RunResult run(const RunConfig& config);
```

Executes the graph synchronously (blocking). Starts from `START_NODE`, follows edges
until `END_NODE` is reached or `max_steps` is exceeded.

#### `run_stream`

```cpp
RunResult run_stream(const RunConfig& config,
                     const GraphStreamCallback& cb);
```

Executes the graph with streaming events. The callback `cb` is invoked for each event
matching the `config.stream_mode` filter.

#### `resume`

```cpp
RunResult resume(const std::string& thread_id,
                 const json& resume_value = json(),
                 const GraphStreamCallback& cb = nullptr);
```

Resumes execution from a previously interrupted checkpoint (human-in-the-loop).

| Parameter | Type | Description |
|-----------|------|-------------|
| `thread_id` | `std::string` | Thread ID to resume |
| `resume_value` | `json` | Optional value to inject before resuming (e.g., human approval) |
| `cb` | `GraphStreamCallback` | Optional streaming callback. Pass `nullptr` for non-streaming resume |

#### `get_state`

```cpp
std::optional<json> get_state(const std::string& thread_id) const;
```

Returns the latest state for a thread, or `std::nullopt` if no checkpoint exists.

#### `get_state_history`

```cpp
std::vector<Checkpoint> get_state_history(const std::string& thread_id,
                                          int limit = 100) const;
```

Returns the checkpoint history for a thread, ordered by timestamp (newest first).

#### `update_state`

```cpp
void update_state(const std::string& thread_id,
                  const json& channel_writes,
                  const std::string& as_node = "");
```

Manually updates the state for a thread by applying channel writes. Creates a new
checkpoint with the updated state.

| Parameter | Type | Description |
|-----------|------|-------------|
| `thread_id` | `std::string` | Target thread |
| `channel_writes` | `json` | Object of `{channel: value}` pairs to apply |
| `as_node` | `std::string` | Optional: record these writes as if from a specific node |

#### `fork`

```cpp
std::string fork(const std::string& source_thread_id,
                 const std::string& new_thread_id,
                 const std::string& checkpoint_id = "");
```

Creates a copy of a thread's state as a new thread. Useful for branching conversations
or creating what-if scenarios.

| Parameter | Type | Description |
|-----------|------|-------------|
| `source_thread_id` | `std::string` | Thread to copy from |
| `new_thread_id` | `std::string` | New thread identifier |
| `checkpoint_id` | `std::string` | Optional: fork from a specific checkpoint (default: latest) |

**Returns:** The checkpoint ID of the new forked state.

#### `own_tools`

```cpp
void own_tools(std::vector<std::unique_ptr<Tool>> tools);
```

Transfers tool ownership to the engine. The engine stores them and keeps raw pointers
valid for the lifetime of all `NodeContext.tools` references.

#### `set_checkpoint_store`

```cpp
void set_checkpoint_store(std::shared_ptr<CheckpointStore> store);
```

Attaches a checkpoint store. Required for `resume()`, `get_state()`, `fork()`, and
all state inspection methods.

#### `set_store`

```cpp
void set_store(std::shared_ptr<Store> store);
```

Attaches a cross-thread shared memory store (see [Store](#9-store)).

#### `get_store`

```cpp
std::shared_ptr<Store> get_store() const;
```

Returns the attached shared memory store, or `nullptr` if none is set.

#### `set_retry_policy`

```cpp
void set_retry_policy(const RetryPolicy& policy);
```

Sets the default retry policy for all nodes. Nodes without a specific policy
will use this one.

#### `set_node_retry_policy`

```cpp
void set_node_retry_policy(const std::string& node_name, const RetryPolicy& policy);
```

Sets a retry policy for a specific node, overriding the default.

#### `get_graph_name`

```cpp
const std::string& get_graph_name() const;
```

Returns the name of the graph as specified in the definition.

---

## 7b. Engine Internals

`GraphEngine` is a thin orchestrator that delegates to four purpose-built
classes. Users typically never touch them directly — they are instantiated
inside `GraphEngine::compile()` and driven from `execute_graph()` — but
they are public so advanced callers can build without JSON, drive custom
checkpoint flows, or stub pieces in tests.

| Class | Header | Responsibility |
|-------|--------|----------------|
| [`GraphCompiler`](#graphcompiler) | `<neograph/graph/compiler.h>` | Parses JSON → `CompiledGraph` |
| [`Scheduler`](#scheduler) | `<neograph/graph/scheduler.h>` | Routing decisions (signal dispatch + barriers) |
| [`CheckpointCoordinator`](#checkpointcoordinator) | `<neograph/graph/coordinator.h>` | Per-run checkpoint lifecycle |
| [`NodeExecutor`](#nodeexecutor) | `<neograph/graph/executor.h>` | Retry, parallel fan-out, Send dispatch |

### GraphCompiler

**Header:** `<neograph/graph/compiler.h>`

Pure JSON → value-type translation. No runtime dependencies — the
resulting `CompiledGraph` is a movable bundle you can inspect or
construct by hand in tests.

```cpp
namespace neograph::graph {

struct ChannelDef {
    std::string  name;
    ReducerType  type = ReducerType::OVERWRITE;
    std::string  reducer_name = "overwrite";
    json         initial_value;
};

struct CompiledGraph {
    std::string name;
    std::vector<ChannelDef> channel_defs;
    std::map<std::string, std::unique_ptr<GraphNode>> nodes;
    std::vector<Edge> edges;
    std::vector<ConditionalEdge> conditional_edges;
    BarrierSpecs barrier_specs;
    std::set<std::string> interrupt_before;
    std::set<std::string> interrupt_after;
    std::optional<RetryPolicy> retry_policy;
};

class GraphCompiler {
public:
    static CompiledGraph compile(const json& definition,
                                 const NodeContext& default_context);
};

} // namespace neograph::graph
```

`GraphEngine::compile()` calls `GraphCompiler::compile()` then moves
every field of the result into the engine's runtime home. Parsing
failures (malformed edge, unknown node type) surface here before any
runtime state is built.

### Scheduler

**Header:** `<neograph/graph/scheduler.h>`

Owns the graph topology and computes each super-step's ready set from
routing signals emitted by the previous step. No knowledge of
threading, checkpointing, retries, or HITL — those stay in the engine.

```cpp
namespace neograph::graph {

struct StepRouting {
    std::string node_name;
    std::optional<std::string> command_goto;
};

struct NextStepPlan {
    std::vector<std::string> ready;
    bool hit_end = false;
    std::optional<std::string> winning_command_goto;
};

using BarrierSpecs = std::map<std::string, std::set<std::string>>;
using BarrierState = std::map<std::string, std::set<std::string>>;

class Scheduler {
public:
    Scheduler(const std::vector<Edge>& edges,
              const std::vector<ConditionalEdge>& conditional_edges,
              BarrierSpecs barrier_specs = {});

    std::vector<std::string> plan_start_step() const;

    NextStepPlan plan_next_step(
        const std::vector<std::string>& just_ran,
        const std::vector<NodeResult>& results,
        const GraphState& state,
        BarrierState& barrier_state) const;

    std::vector<std::string> resolve_next_nodes(
        const std::string& current,
        const GraphState& state) const;

    const BarrierSpecs& barrier_specs() const;
};

} // namespace neograph::graph
```

**Semantics:**

- **Signal dispatch**: a node becomes ready in super-step S+1 iff some
  node in step S explicitly routed to it (regular edge, conditional
  edge branch, `Command::goto_node`, or Send). No static predecessor
  map — that would conflate XOR routing with AND fan-in.
- **Pairing invariant**: the caller must pass `just_ran` and `results`
  with `just_ran[i] ↔ results[i]`. Enforced by the two-argument
  overload's type signature so callers cannot desynchronize them.
- **Barriers**: nodes declared with `"barrier": {"wait_for": [...]}`
  gate on ALL listed upstreams having signaled, accumulated across
  super-steps via the mutable `BarrierState` map. Fires reset the
  entry so loops through the barrier work correctly.

### CheckpointCoordinator

**Header:** `<neograph/graph/coordinator.h>`

Per-run wrapper over `(CheckpointStore, thread_id)`. Every method is a
safe no-op when the store is null or thread_id is empty, so call sites
never need to guard.

```cpp
namespace neograph::graph {

struct ResumeContext {
    bool have_cp = false;
    std::string checkpoint_id;
    json channel_values;
    int start_step = 0;  // Phase-adjusted
    CheckpointPhase phase = CheckpointPhase::Completed;
    std::vector<std::string> next_nodes;
    std::unordered_map<std::string, NodeResult> replay_results;
    BarrierState barrier_state;
};

class CheckpointCoordinator {
public:
    CheckpointCoordinator(std::shared_ptr<CheckpointStore> store,
                          std::string thread_id);

    bool enabled() const noexcept;

    std::string save_super_step(
        const GraphState& state,
        const std::string& current_node,
        const std::vector<std::string>& next_nodes,
        CheckpointPhase phase,
        int step,
        const std::string& parent_id,
        const BarrierState& barrier_state) const;

    ResumeContext load_for_resume() const;

    void record_pending_write(
        const std::string& parent_cp_id,
        const std::string& task_id,
        const std::string& task_path,
        const std::string& node_name,
        const NodeResult& nr,
        int step) const;

    void clear_pending_writes(const std::string& parent_cp_id) const;
};

} // namespace neograph::graph
```

**Phase-aware step offset:** `load_for_resume()` reads the latest
checkpoint's `interrupt_phase` and sets `start_step` accordingly —
`Before` / `NodeInterrupt` re-enter at `cp.step`, `After` / `Completed` /
`Updated` advance by +1. The engine's resume path never repeats this
logic.

### NodeExecutor

**Header:** `<neograph/graph/executor.h>`

Owns per-super-step node invocation: retry loop, replay lookup,
pending-write recording, parallel fan-out via
`asio::experimental::make_parallel_group`, and Send dispatch. 3.0
removed the sync `run_one` / `run_parallel` / `run_sends` twins;
callers use the `_async` peers.

```cpp
namespace neograph::graph {

class NodeExecutor {
public:
    using RetryPolicyLookup = std::function<RetryPolicy(const std::string&)>;

    NodeExecutor(
        const std::map<std::string, std::unique_ptr<GraphNode>>& nodes,
        const std::vector<ChannelDef>& channel_defs,
        RetryPolicyLookup retry_policy_for,
        asio::thread_pool* fan_out_pool = nullptr);

    asio::awaitable<NodeResult> run_one_async(
        const std::string& node_name, int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        const BarrierState& barrier_state,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb, StreamMode stream_mode);

    asio::awaitable<std::vector<NodeResult>> run_parallel_async(
        const std::vector<std::string>& ready, int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        const BarrierState& barrier_state,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb, StreamMode stream_mode);

    asio::awaitable<void> run_sends_async(
        const std::vector<Send>& sends, int step,
        GraphState& state,
        const std::unordered_map<std::string, NodeResult>& replay,
        CheckpointCoordinator& coord,
        const std::string& parent_cp_id,
        std::vector<std::string>& trace,
        const GraphStreamCallback& cb, StreamMode stream_mode);

    asio::awaitable<NodeResult> execute_node_with_retry_async(
        const std::string& node_name,
        GraphState& state,
        const GraphStreamCallback& cb, StreamMode stream_mode);
};

} // namespace neograph::graph
```

**Invariants:**

- `run_one_async` and `run_parallel_async` both save a
  `phase=NodeInterrupt` checkpoint scoped to the interrupting node
  before rethrowing `NodeInterrupt`, so resume re-enters just that
  node (sibling writes are already in `pending_writes` and replay via
  the map).
- `run_parallel_async` applies writes + `Command.updates` in `ready`
  order so `ready[i] ↔ results[i]` pairing holds for the subsequent
  Scheduler call.
- `run_sends_async`: single Send runs on the shared state with retry;
  multi Send gives each target an isolated state copy (init + restore
  + apply input) without retry — preserves pre-3.0 semantics.
- `fan_out_pool` (optional) determines where parallel branches
  dispatch. When null, branches run on `co_await asio::this_coro::
  executor` — fine for single-thread async callers, but CPU-bound
  fan-out serializes. When non-null, `run_parallel_async` and the
  multi-Send branch `co_spawn` onto `pool->get_executor()` for real
  thread parallelism. `GraphEngine::set_worker_count(N)` installs the
  pool for sync `run()` callers.
- `execute_node_with_retry_async` is the inner retry loop: backoff
  uses an `asio::steady_timer` so the executor isn't frozen during
  retry waits.

---

## 8. Checkpoint

**Header:** `<neograph/graph/checkpoint.h>`
**Namespace:** `neograph::graph`

Checkpointing enables persistence, time-travel debugging, and human-in-the-loop
workflows by saving and restoring graph execution state.

### Checkpoint (struct)

A serialized snapshot of graph execution state at a point in time.

```cpp
struct Checkpoint {
    std::string id;                // UUID v4
    std::string thread_id;         // Conversation/session identifier
    json        channel_values;    // Serialized channel data
    json        channel_versions;  // Per-channel version counters
    std::string parent_id;         // Previous checkpoint ID (for time-travel chain)
    std::string current_node;      // Node that was active at checkpoint time
    std::vector<std::string> next_nodes;  // Nodes to execute on resume
    CheckpointPhase interrupt_phase;  // Before | After | Completed | NodeInterrupt | Updated
    std::map<std::string, std::set<std::string>> barrier_state;  // v2+: in-flight barrier accumulators
    json        metadata;          // User-defined metadata
    int64_t     step;              // Super-step number
    int64_t     timestamp;         // Unix epoch milliseconds
    int         schema_version = CHECKPOINT_SCHEMA_VERSION;  // Layout version

    static std::string generate_id();  // Generate UUID v4
};
```

| Field | Type | Description |
|-------|------|-------------|
| `id` | `std::string` | Unique identifier (UUID v4) |
| `thread_id` | `std::string` | Groups checkpoints by conversation/session |
| `channel_values` | `json` | Serialized state of all channels |
| `channel_versions` | `json` | Version counter for each channel |
| `parent_id` | `std::string` | ID of the preceding checkpoint (forms a linked list for time-travel) |
| `current_node` | `std::string` | Node that was executing when the checkpoint was taken |
| `next_nodes` | `std::vector<std::string>` | All nodes scheduled for the next super-step (used by `resume()`). Under signal dispatch a super-step can leave several nodes simultaneously ready (parallel fan-out, conditional branches activating together), and every one of them must be persisted — storing a single node would silently drop siblings across a crash |
| `interrupt_phase` | `CheckpointPhase` | Enum: `Before` (interrupt_before fired), `After` (interrupt_after fired), `Completed` (normal super-step cadence), `NodeInterrupt` (node threw `NodeInterrupt` mid-execution), `Updated` (external `update_state()` injection). `to_string()` and `parse_checkpoint_phase()` give a stable wire/log encoding |
| `barrier_state` | `map<string, set<string>>` | Per-barrier accumulator of upstreams that have signaled so far. Entries only exist for barriers that are in-flight (not yet fired) — the Scheduler clears an entry when its barrier fires. Shape matches `BarrierState` from `scheduler.h`. Present since schema v2; v1 blobs deserialize with an empty map, which matches their pre-v2 behavior |
| `metadata` | `json` | Arbitrary user-defined data |
| `step` | `int64_t` | Super-step counter |
| `timestamp` | `int64_t` | Creation time in Unix epoch milliseconds |
| `schema_version` | `int` | On-wire layout version (see `CHECKPOINT_SCHEMA_VERSION`, currently `2`). Fresh checkpoints from the engine always carry the current version. Persistent `CheckpointStore` implementations should serialize it and treat `0` on a deserialized blob as "pre-versioned" (e.g. the field was absent — migration is the caller's responsibility) |

### CheckpointStore

Abstract interface for checkpoint persistence. Implement this to store checkpoints
in a database, file system, or any other backend.

> **Writing a custom store?** 8 sync methods have 8 async peers.
> See [`ASYNC_GUIDE.md` §9.4](ASYNC_GUIDE.md#94-checkpointstore) —
> override all-sync or all-async (not mixed), depending on whether
> your backend is blocking or async-capable.

```cpp
class CheckpointStore {
public:
    virtual ~CheckpointStore() = default;

    virtual void save(const Checkpoint& cp) = 0;

    virtual std::optional<Checkpoint> load_latest(const std::string& thread_id) = 0;

    virtual std::optional<Checkpoint> load_by_id(const std::string& id) = 0;

    virtual std::vector<Checkpoint> list(const std::string& thread_id,
                                          int limit = 100) = 0;

    virtual void delete_thread(const std::string& thread_id) = 0;
};
```

| Method | Description |
|--------|-------------|
| `save(cp)` | Persist a checkpoint |
| `load_latest(thread_id)` | Load the most recent checkpoint for a thread |
| `load_by_id(id)` | Load a specific checkpoint by UUID |
| `list(thread_id, limit)` | List checkpoints for a thread, newest first, up to `limit` |
| `delete_thread(thread_id)` | Delete all checkpoints for a thread |

### InMemoryCheckpointStore

Thread-safe in-memory implementation suitable for testing and single-process applications.

```cpp
class InMemoryCheckpointStore : public CheckpointStore {
public:
    void save(const Checkpoint& cp) override;
    std::optional<Checkpoint> load_latest(const std::string& thread_id) override;
    std::optional<Checkpoint> load_by_id(const std::string& id) override;
    std::vector<Checkpoint> list(const std::string& thread_id,
                                  int limit = 100) override;
    void delete_thread(const std::string& thread_id) override;

    size_t size() const;  // Total number of stored checkpoints
};
```

---

## 9. Store

**Header:** `<neograph/graph/store.h>`
**Namespace:** `neograph::graph`

Cross-thread shared memory store. Provides namespaced key-value storage that persists
across threads and graph executions. Use cases include long-term user preferences,
shared knowledge bases, and agent memory.

### Namespace

A hierarchical path represented as a vector of strings.

```cpp
using Namespace = std::vector<std::string>;
```

Example: `{"users", "user123", "preferences"}` represents the path `users/user123/preferences`.

### StoreItem

A single item in the store.

```cpp
struct StoreItem {
    Namespace   ns;          // Namespace path
    std::string key;         // Item key within the namespace
    json        value;       // Stored value
    int64_t     created_at;  // Creation timestamp (Unix epoch millis)
    int64_t     updated_at;  // Last update timestamp (Unix epoch millis)
};
```

### Store (abstract)

Abstract interface for cross-thread shared memory.

```cpp
class Store {
public:
    virtual ~Store() = default;

    // Put a value (create or update)
    virtual void put(const Namespace& ns, const std::string& key,
                     const json& value) = 0;

    // Get a single item
    virtual std::optional<StoreItem> get(const Namespace& ns,
                                         const std::string& key) const = 0;

    // Search items under a namespace prefix
    virtual std::vector<StoreItem> search(const Namespace& ns_prefix,
                                           int limit = 100) const = 0;

    // Delete an item
    virtual void delete_item(const Namespace& ns, const std::string& key) = 0;

    // List namespaces under a prefix
    virtual std::vector<Namespace> list_namespaces(
        const Namespace& prefix = {}) const = 0;
};
```

| Method | Description |
|--------|-------------|
| `put(ns, key, value)` | Insert or update a value. Updates `updated_at` if the item already exists |
| `get(ns, key)` | Retrieve a single item. Returns `std::nullopt` if not found |
| `search(ns_prefix, limit)` | Find all items whose namespace starts with the given prefix |
| `delete_item(ns, key)` | Remove an item from the store |
| `list_namespaces(prefix)` | List all unique namespaces that start with the given prefix |

### InMemoryStore

Thread-safe in-memory implementation for testing and single-process use.

```cpp
class InMemoryStore : public Store {
public:
    void put(const Namespace& ns, const std::string& key,
             const json& value) override;
    std::optional<StoreItem> get(const Namespace& ns,
                                 const std::string& key) const override;
    std::vector<StoreItem> search(const Namespace& ns_prefix,
                                   int limit = 100) const override;
    void delete_item(const Namespace& ns, const std::string& key) override;
    std::vector<Namespace> list_namespaces(
        const Namespace& prefix = {}) const override;

    size_t size() const;  // Total number of stored items
};
```

---

## 10. Loader

**Header:** `<neograph/graph/loader.h>`
**Namespace:** `neograph::graph`

Singleton registries for reducers, conditions, and node types. These enable
JSON-driven graph construction: the `GraphEngine::compile()` method looks up
components by name from these registries.

### ReducerRegistry

Singleton registry mapping string names to `ReducerFn` implementations.

```cpp
class ReducerRegistry {
public:
    static ReducerRegistry& instance();

    void register_reducer(const std::string& name, ReducerFn fn);
    ReducerFn get(const std::string& name) const;
};
```

| Method | Description |
|--------|-------------|
| `instance()` | Returns the singleton instance |
| `register_reducer(name, fn)` | Registers a custom reducer function |
| `get(name)` | Looks up a reducer by name. Throws if not found |

### ConditionRegistry

Singleton registry mapping string names to `ConditionFn` implementations.

```cpp
class ConditionRegistry {
public:
    static ConditionRegistry& instance();

    void register_condition(const std::string& name, ConditionFn fn);
    ConditionFn get(const std::string& name) const;
};
```

| Method | Description |
|--------|-------------|
| `instance()` | Returns the singleton instance |
| `register_condition(name, fn)` | Registers a custom condition function |
| `get(name)` | Looks up a condition by name. Throws if not found |

### NodeFactory

Singleton factory for creating `GraphNode` instances from JSON configuration.

```cpp
using NodeFactoryFn = std::function<std::unique_ptr<GraphNode>(
    const std::string& name,
    const json& config,
    const NodeContext& ctx)>;

class NodeFactory {
public:
    static NodeFactory& instance();

    void register_type(const std::string& type, NodeFactoryFn fn);
    std::unique_ptr<GraphNode> create(const std::string& type,
                                       const std::string& name,
                                       const json& config,
                                       const NodeContext& ctx) const;
};
```

| Method | Description |
|--------|-------------|
| `instance()` | Returns the singleton instance |
| `register_type(type, fn)` | Registers a node factory function for the given type string |
| `create(type, name, config, ctx)` | Creates a node of the given type. Throws if the type is not registered |

### Built-in Registrations

The library pre-registers the following components:

**Reducers:**

| Name | Behavior |
|------|----------|
| `"overwrite"` | Replaces the current value with the incoming value |
| `"append"` | Appends the incoming value to the current array. If the incoming value is an array, its elements are concatenated |

**Conditions:**

| Name | Behavior |
|------|----------|
| `"has_tool_calls"` | Inspects the last message in the `"messages"` channel. Returns `"yes"` if it contains tool calls, `"no"` otherwise |
| `"route_channel"` | Reads the `"__route__"` channel and returns its string value. Used with `IntentClassifierNode` |

**Node types:**

| Type | Class | Description |
|------|-------|-------------|
| `"llm_call"` | `LLMCallNode` | Calls the LLM with current conversation state |
| `"tool_dispatch"` | `ToolDispatchNode` | Dispatches tool calls from the latest assistant message |
| `"intent_classifier"` | `IntentClassifierNode` | LLM-based intent classification. Reads `prompt` and `valid_routes` from `config` |
| `"subgraph"` | `SubgraphNode` | Runs a compiled subgraph. Reads `input_map` and `output_map` from `config` |

---

## 11. React Graph

**Header:** `<neograph/graph/react_graph.h>`
**Namespace:** `neograph::graph`

Convenience function that creates a standard ReAct (Reason + Act) agent as a two-node
graph: `llm_call -> tool_dispatch -> (loop back if tool calls, else end)`.

```cpp
std::unique_ptr<GraphEngine> create_react_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& instructions = "",
    const std::string& model = "");
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `provider` | `std::shared_ptr<Provider>` | LLM provider |
| `tools` | `std::vector<std::unique_ptr<Tool>>` | Tools available to the agent (ownership transferred) |
| `instructions` | `std::string` | System prompt / instructions |
| `model` | `std::string` | Model override (empty uses provider default) |

**Returns:** A compiled `GraphEngine` ready to run.

This is functionally equivalent to using `Agent::run()` but as a graph engine, giving
you access to checkpointing, streaming events, state inspection, and all other graph
engine features.

---

## 11b. Plan-and-Execute Graph

**Header:** `<neograph/graph/plan_execute_graph.h>`
**Namespace:** `neograph::graph`

Convenience factory for the Plan-and-Execute pattern: a planner emits a JSON
array of steps, an executor consumes them one-by-one via an inner ReAct loop,
and a responder composes the final answer from `past_steps`.

```
__start__ → planner → [plan_empty? responder : executor]
                      executor → [plan_empty? responder : executor]
                      responder → __end__
```

```cpp
std::unique_ptr<GraphEngine> create_plan_execute_graph(
    std::shared_ptr<Provider> provider,
    std::vector<std::unique_ptr<Tool>> tools,
    const std::string& planner_prompt,
    const std::string& executor_prompt,
    const std::string& responder_prompt,
    const std::string& model = "",
    int max_step_iterations = 5);
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `provider` | `std::shared_ptr<Provider>` | LLM provider shared by every phase |
| `tools` | `std::vector<std::unique_ptr<Tool>>` | Tools the executor may invoke (ownership transferred) |
| `planner_prompt` | `std::string` | System prompt for the planner; must instruct the model to reply with a JSON array of steps (fenced ```json blocks and leading prose are tolerated) |
| `executor_prompt` | `std::string` | System prompt for the single-step executor (inner ReAct loop) |
| `responder_prompt` | `std::string` | System prompt for the final synthesis phase |
| `model` | `std::string` | Model override (empty uses provider default) |
| `max_step_iterations` | `int` | Upper bound on tool-call iterations inside the executor per step |

**Channels populated:** `plan`, `past_steps`, `final_response`, `messages`.

**Returns:** A compiled `GraphEngine` ready to run. The factory registers its
three custom node types and the `plan_empty` condition on first call
(idempotent via `std::call_once`).

See `examples/14_plan_executor.cpp` for a Send-fan-out variant with crash /
resume via pending-writes.

---

## 12. LLM Module

### OpenAIProvider

**Header:** `<neograph/llm/openai_provider.h>`
**Namespace:** `neograph::llm`

Provider implementation for the OpenAI API and OpenAI-compatible endpoints.

```cpp
class OpenAIProvider : public Provider {
public:
    struct Config {
        std::string api_key;                          // API key
        std::string base_url = "https://api.openai.com"; // API base URL
        std::string default_model = "gpt-4o-mini";    // Default model
        int timeout_seconds = 60;                     // HTTP timeout
    };

    static std::unique_ptr<OpenAIProvider> create(const Config& config);

    ChatCompletion complete(const CompletionParams& params) override;
    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override;
    std::string get_name() const override;  // Returns "openai"
};
```

**Config fields:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `api_key` | `std::string` | | OpenAI API key |
| `base_url` | `std::string` | `"https://api.openai.com"` | Base URL. Override for Azure, local models, or compatible APIs |
| `default_model` | `std::string` | `"gpt-4o-mini"` | Model used when `CompletionParams::model` is empty |
| `timeout_seconds` | `int` | `60` | HTTP request timeout |

**Usage:**

```cpp
auto provider = neograph::llm::OpenAIProvider::create({
    .api_key = "sk-...",
    .default_model = "gpt-4o"
});
```

### SchemaProvider

**Header:** `<neograph/llm/schema_provider.h>`
**Namespace:** `neograph::llm`

A schema-driven provider that supports multiple LLM APIs through JSON configuration
files. Instead of hardcoding API-specific logic, `SchemaProvider` reads a schema that
describes how to format requests, parse responses, and handle streaming for any API.

```cpp
class SchemaProvider : public Provider {
public:
    struct Config {
        std::string schema_path;     // Schema name or file path
        std::string api_key;         // API key (overrides env var)
        std::string default_model = "gpt-4o-mini";
        int timeout_seconds = 60;
    };

    static std::unique_ptr<SchemaProvider> create(const Config& config);

    ChatCompletion complete(const CompletionParams& params) override;
    ChatCompletion complete_stream(const CompletionParams& params,
                                   const StreamCallback& on_chunk) override;
    std::string get_name() const override;
};
```

**Config fields:**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `schema_path` | `std::string` | | Built-in schema name or path to a custom schema JSON file |
| `api_key` | `std::string` | | API key. If empty, falls back to the env var specified in the schema |
| `default_model` | `std::string` | `"gpt-4o-mini"` | Default model identifier |
| `timeout_seconds` | `int` | `60` | HTTP timeout |

**Built-in schemas:**

| Name | API | Notes |
|------|-----|-------|
| `"openai"` | OpenAI | Same behavior as `OpenAIProvider` |
| `"claude"` | Anthropic Claude | Uses SSE event-based streaming |
| `"gemini"` | Google Gemini | Uses function declarations format |

**Custom schemas:** Pass a file path to `schema_path` to load a custom schema JSON file
describing any API's request/response format.

**Usage:**

```cpp
// Using a built-in schema
auto claude = neograph::llm::SchemaProvider::create({
    .schema_path = "claude",
    .api_key = "sk-ant-...",
    .default_model = "claude-sonnet-4-20250514"
});

// Using a custom schema file
auto custom = neograph::llm::SchemaProvider::create({
    .schema_path = "/path/to/my_provider.json",
    .api_key = "...",
    .default_model = "my-model-v1"
});
```

**Internal strategy enums** (documented for custom schema authors):

The schema file configures the following strategies:

| Strategy | Options | Description |
|----------|---------|-------------|
| System prompt | `IN_MESSAGES`, `TOP_LEVEL`, `TOP_LEVEL_PARTS` | How the system prompt is placed in the request |
| Tool calls | `TOOL_CALLS_ARRAY`, `CONTENT_ARRAY`, `PARTS_ARRAY` | How tool calls appear in assistant messages |
| Tool results | `FLAT`, `CONTENT_ARRAY`, `PARTS_ARRAY` | How tool results are formatted |
| Tool defs | `FUNCTION`, `NONE`, `FUNCTION_DECLARATIONS` | How tool definitions are wrapped |
| Response | `CHOICES_MESSAGE`, `CONTENT_ARRAY`, `CANDIDATES_PARTS` | How responses are parsed |
| Streaming | `SSE_DATA`, `SSE_EVENTS` | Streaming format |

### Agent

**Header:** `<neograph/llm/agent.h>`
**Namespace:** `neograph::llm`

A simple agent that runs an LLM tool-use loop: call the LLM, execute any tool calls,
feed results back, and repeat until the LLM responds with text only.

```cpp
class Agent {
public:
    Agent(std::shared_ptr<Provider> provider,
          std::vector<std::unique_ptr<Tool>> tools,
          const std::string& instructions = "",
          const std::string& model = "");

    // Run the tool loop, returns the final text response
    std::string run(std::vector<ChatMessage>& messages,
                    int max_iterations = 10);

    // Streaming variant: streams final response tokens
    std::string run_stream(std::vector<ChatMessage>& messages,
                           const StreamCallback& on_chunk,
                           int max_iterations = 10);

    // Single completion (no tool loop)
    ChatCompletion complete(const std::vector<ChatMessage>& messages);
};
```

| Constructor Parameter | Type | Description |
|-----------------------|------|-------------|
| `provider` | `std::shared_ptr<Provider>` | LLM provider to use |
| `tools` | `std::vector<std::unique_ptr<Tool>>` | Tools available to the agent (ownership transferred) |
| `instructions` | `std::string` | System prompt prepended to messages |
| `model` | `std::string` | Model override (empty uses provider default) |

| Method | Description |
|--------|-------------|
| `run(messages, max_iterations)` | Runs the full tool-use loop. Mutates `messages` in place with the full conversation. Returns the final assistant text response |
| `run_stream(messages, on_chunk, max_iterations)` | Same as `run()` but streams the final response tokens via `on_chunk`. Tool-use iterations are not streamed |
| `complete(messages)` | Single LLM call without tool loop. Useful for one-shot completions |

**Usage:**

```cpp
auto provider = neograph::llm::OpenAIProvider::create({.api_key = "sk-..."});

std::vector<std::unique_ptr<neograph::Tool>> tools;
tools.push_back(std::make_unique<WeatherTool>());

neograph::llm::Agent agent(provider, std::move(tools),
                            "You are a helpful weather assistant.");

std::vector<neograph::ChatMessage> messages;
messages.push_back({"user", "What's the weather in Seoul?"});

std::string response = agent.run(messages);
```

### json_path Utilities

**Header:** `<neograph/llm/json_path.h>`
**Namespace:** `neograph::llm::json_path`

Utility functions for navigating and manipulating JSON values using dot-separated
path strings. Used internally by `SchemaProvider` but available for general use.

```cpp
namespace json_path {
    std::vector<std::string> split_path(const std::string& path);
    const json* at_path(const json& root, const std::string& path);
    json* at_path_mut(json& root, const std::string& path);
    bool has_path(const json& root, const std::string& path);

    template<typename T>
    T get_path(const json& root, const std::string& path, const T& default_val);

    void set_path(json& root, const std::string& path, const json& value);
}
```

| Function | Description |
|----------|-------------|
| `split_path(path)` | Splits a dot-path string into segments. Example: `"choices.0.message"` becomes `["choices", "0", "message"]` |
| `at_path(root, path)` | Navigates into a JSON value by dot-path. Numeric segments index into arrays. Returns `nullptr` if the path does not exist |
| `at_path_mut(root, path)` | Mutable version of `at_path` |
| `has_path(root, path)` | Returns `true` if the dot-path exists in the JSON value |
| `get_path<T>(root, path, default_val)` | Returns the value at the path converted to type `T`, or `default_val` if the path does not exist or conversion fails |
| `set_path(root, path, value)` | Sets a value at a dot-path, creating intermediate objects as needed |

**Examples:**

```cpp
using namespace neograph::llm::json_path;

json data = json::parse(R"({"choices": [{"message": {"content": "Hello"}}]})");

// Navigate
const json* msg = at_path(data, "choices.0.message.content");
// *msg == "Hello"

// Check existence
bool exists = has_path(data, "choices.0.message.role");
// exists == false

// Get with default
std::string role = get_path<std::string>(data, "choices.0.message.role", "assistant");
// role == "assistant"

// Set value (creates intermediates)
set_path(data, "metadata.version", 2);
```

---

## 13. MCP Module

**Header:** `<neograph/mcp/client.h>`
**Namespace:** `neograph::mcp`

Model Context Protocol (MCP) client implementation. Connects to MCP servers, discovers
available tools, and wraps them as NeoGraph `Tool` instances.

Two transports are available:

- **HTTP** — `MCPClient("http://host:port")`. Each tool call opens a short-lived
  session against the server (Streamable HTTP transport, `Mcp-Session-Id` header
  echoed per-request).
- **stdio** — `MCPClient({"python", "server.py"})`. The client `fork`+`execvp`s the
  subprocess, wires bidirectional pipes, and exchanges newline-delimited JSON-RPC
  over the child's stdin/stdout. The subprocess lives as long as the
  `MCPClient` *or any `MCPTool`* it produced; destruction sends SIGTERM and
  reaps via `waitpid` (SIGKILL fallback after ~500 ms).

### MCPTool

Wraps a single MCP server tool as a local `Tool` implementation. Two
constructors, one per transport; `MCPClient::get_tools()` picks the right one.

```cpp
class MCPTool : public Tool {
public:
    // HTTP mode — each execute() opens a fresh session against server_url.
    MCPTool(const std::string& server_url,
            const std::string& name,
            const std::string& description,
            const json& input_schema);

    // stdio mode — tool holds a shared_ptr back-ref to the subprocess
    // session, keeping it alive as long as any tool is reachable.
    MCPTool(std::shared_ptr<detail::StdioSession> session,
            const std::string& name,
            const std::string& description,
            const json& input_schema);

    ChatTool   get_definition() const override;
    std::string execute(const json& arguments) override;
    std::string get_name() const override;
};
```

Usually you do not construct `MCPTool` directly — `MCPClient::get_tools()`
discovers and wraps them.

### MCPClient

Client that connects to an MCP server, performs the initialization handshake, and
provides methods to discover and invoke tools.

> `MCPClient` is not designed to be subclassed — you use it as-is.
> `rpc_call_async()` is the real implementation and `rpc_call()` is
> a thin sync facade. See [`ASYNC_GUIDE.md` §9.5](ASYNC_GUIDE.md#95-mcpclient).

```cpp
class MCPClient {
public:
    // HTTP transport.
    explicit MCPClient(const std::string& server_url);

    // stdio transport — fork+exec the subprocess.
    explicit MCPClient(std::vector<std::string> argv);

    bool initialize(const std::string& client_name = "neograph");
    std::vector<std::unique_ptr<Tool>> get_tools();
    json call_tool(const std::string& name, const json& arguments);
};
```

| Method | Description |
|--------|-------------|
| `MCPClient(url)` | Construct an HTTP-mode client |
| `MCPClient(argv)` | Spawn a subprocess and construct a stdio-mode client. `argv[0]` is resolved via `PATH` (execvp). Throws on fork/exec failure |
| `initialize(client_name)` | Perform the MCP initialization handshake. Returns `true` on success. Must be called before `get_tools()` or `call_tool()` |
| `get_tools()` | Discovers tools from the server and returns them as `std::unique_ptr<Tool>` instances ready for use with `Agent` or `GraphEngine` |
| `call_tool(name, arguments)` | Invokes a tool by name with the given arguments. Returns the raw JSON response |

**HTTP usage:**

```cpp
neograph::mcp::MCPClient client("http://localhost:8000");
client.initialize();
auto tools = client.get_tools();
```

**stdio usage:**

```cpp
// argv[0] is resolved via PATH; pipe fds are closed in the child before execvp.
neograph::mcp::MCPClient client({"python", "/path/to/server.py"});
client.initialize();
auto tools = client.get_tools();   // MCPTools hold shared_ptr<StdioSession>
```

---

## 14. Util Module

**Header:** `<neograph/util/request_queue.h>`
**Namespace:** `neograph::util`

### RequestQueue

Lock-free request queue with a worker thread pool and backpressure support.
Decouples HTTP connection acceptance from LLM call concurrency in server applications.

```cpp
class RequestQueue {
public:
    struct Stats {
        size_t pending;        // Tasks waiting in queue
        size_t active;         // Tasks currently executing
        size_t completed;      // Total completed tasks
        size_t rejected;       // Tasks rejected due to full queue
        size_t num_workers;    // Number of worker threads
        size_t max_queue_size; // Maximum queue capacity
    };

    RequestQueue(size_t num_workers = 128, size_t max_queue_size = 10000);
    ~RequestQueue();

    // Non-copyable
    RequestQueue(const RequestQueue&) = delete;
    RequestQueue& operator=(const RequestQueue&) = delete;

    // Submit a task. Returns {accepted, future}.
    // If the queue is full, returns {false, invalid_future}.
    template<typename F>
    std::pair<bool, std::future<void>> submit(F&& task);

    // Get current queue statistics
    Stats stats() const;
};
```

| Constructor Parameter | Type | Default | Description |
|-----------------------|------|---------|-------------|
| `num_workers` | `size_t` | `128` | Number of worker threads in the pool |
| `max_queue_size` | `size_t` | `10000` | Maximum number of pending tasks. Tasks beyond this limit are rejected |

| Method | Description |
|--------|-------------|
| `submit(task)` | Enqueues a callable. Returns a pair: `first` is `true` if accepted (or `false` if the queue is full -- backpressure), `second` is a `std::future<void>` that resolves when the task completes or propagates exceptions |
| `stats()` | Returns a snapshot of current queue statistics |

The queue uses `moodycamel::ConcurrentQueue` internally for lock-free enqueue/dequeue
and a condition variable to wake idle workers.

**Usage:**

```cpp
neograph::util::RequestQueue queue(4, 100);  // 4 workers, max 100 pending

auto [accepted, future] = queue.submit([&] {
    // Handle an incoming HTTP request
    auto result = engine->run(config);
    send_response(result);
});

if (!accepted) {
    send_503_service_unavailable();
}
```

---

## Usage Examples

### Minimal ReAct Agent

The simplest way to use NeoGraph -- a ReAct agent with tools:

```cpp
#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/graph/react_graph.h>

int main() {
    auto provider = neograph::llm::OpenAIProvider::create({
        .api_key = std::getenv("OPENAI_API_KEY"),
        .default_model = "gpt-4o"
    });

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<WeatherTool>());

    auto engine = neograph::graph::create_react_graph(
        provider, std::move(tools),
        "You are a helpful assistant with access to weather data."
    );

    neograph::graph::RunConfig config;
    config.input = {{"messages", json::array({
        {{"role", "user"}, {"content", "What's the weather in Tokyo?"}}
    })}};

    auto result = engine->run(config);
    // result.output contains the final state with all messages
}
```

### Custom Graph with Conditional Routing

Building a graph with conditional edges:

```cpp
#include <neograph/neograph.h>
#include <neograph/llm/openai_provider.h>

using namespace neograph::graph;
using json = nlohmann::json;

int main() {
    auto provider = neograph::llm::OpenAIProvider::create({
        .api_key = std::getenv("OPENAI_API_KEY")
    });

    std::vector<std::unique_ptr<neograph::Tool>> tools;
    tools.push_back(std::make_unique<SearchTool>());
    tools.push_back(std::make_unique<CalculatorTool>());

    // Build tool pointer list for NodeContext
    std::vector<neograph::Tool*> tool_ptrs;
    for (auto& t : tools) tool_ptrs.push_back(t.get());

    NodeContext ctx{provider, tool_ptrs, "gpt-4o", "You are a helpful assistant."};

    json definition = {
        {"name", "assistant_graph"},
        {"channels", json::array({
            {{"name", "messages"}, {"type", "append"}},
            {{"name", "status"},   {"type", "overwrite"}, {"initial", "idle"}}
        })},
        {"nodes", json::array({
            {{"name", "llm"},   {"type", "llm_call"}},
            {{"name", "tools"}, {"type", "tool_dispatch"}}
        })},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "llm"}},
            {{"from", "tools"},     {"to", "llm"}}
        })},
        {"conditional_edges", json::array({
            {{"from", "llm"},
             {"condition", "has_tool_calls"},
             {"routes", {{"yes", "tools"}, {"no", "__end__"}}}}
        })}
    };

    auto store = std::make_shared<InMemoryCheckpointStore>();
    auto engine = GraphEngine::compile(definition, ctx, store);
    engine->own_tools(std::move(tools));

    RunConfig config;
    config.thread_id = "session-1";
    config.input = {{"messages", json::array({
        {{"role", "user"}, {"content", "Search for NeoGraph C++ library"}}
    })}};

    auto result = engine->run(config);

    // Inspect execution trace
    for (const auto& node : result.execution_trace) {
        std::cout << "Executed: " << node << "\n";
    }
}
```

### Human-in-the-Loop with Checkpointing

Using interrupts for human approval:

```cpp
auto store = std::make_shared<InMemoryCheckpointStore>();
auto engine = GraphEngine::compile(definition, ctx, store);

// Configure interrupt after the "tools" node
// (set "interrupt_after": ["tools"] in the JSON definition)

RunConfig config;
config.thread_id = "approval-session";
config.input = {{"messages", json::array({
    {{"role", "user"}, {"content", "Delete all files in /tmp"}}
})}};

auto result = engine->run(config);

if (result.interrupted) {
    std::cout << "Interrupted at: " << result.interrupt_node << "\n";
    std::cout << "Reason: " << result.interrupt_value.dump() << "\n";

    // Get human input...
    std::string approval = get_human_approval();

    // Resume with the human's decision
    auto resumed = engine->resume(
        "approval-session",
        {{"approved", approval == "yes"}}
    );
}
```

### Dynamic Fan-Out with Send

Using `Send` for map-reduce patterns:

```cpp
class FanOutNode : public GraphNode {
public:
    std::string get_name() const override { return "fan_out"; }

    // Override execute_full to return Send directives
    NodeResult execute_full(const GraphState& state) override {
        auto items = state.get("items");
        NodeResult result;
        for (const auto& item : items) {
            result.sends.push_back(Send{
                "process_item",       // target node
                {{"item", item}}      // input for that invocation
            });
        }
        return result;
    }
};
```

Each `Send` dispatches the `"process_item"` node with a different input. The engine
executes all sends, collecting their channel writes, before proceeding to the next
edge in the graph.

### Routing Override with Command

Using `Command` to simultaneously update state and control routing:

```cpp
class RouterNode : public GraphNode {
public:
    std::string get_name() const override { return "router"; }

    NodeResult execute_full(const GraphState& state) override {
        auto messages = state.get_messages();
        auto last = messages.back().content;

        NodeResult result;

        if (last.find("urgent") != std::string::npos) {
            result.command = Command{
                "urgent_handler",                          // goto node
                {{{"channel", "priority"}, {"value", "high"}}} // state updates
            };
        } else {
            result.command = Command{
                "normal_handler",
                {{{"channel", "priority"}, {"value", "normal"}}}
            };
        }

        return result;
    }
};
```

When `Command` is returned, its `updates` are applied to the state and execution
jumps directly to the specified `goto_node`, bypassing normal edge routing.

### SchemaProvider Multi-LLM Support

Using `SchemaProvider` to switch between LLM providers:

```cpp
#include <neograph/llm/schema_provider.h>

// OpenAI
auto openai = neograph::llm::SchemaProvider::create({
    .schema_path = "openai",
    .api_key = std::getenv("OPENAI_API_KEY"),
    .default_model = "gpt-4o"
});

// Anthropic Claude
auto claude = neograph::llm::SchemaProvider::create({
    .schema_path = "claude",
    .api_key = std::getenv("ANTHROPIC_API_KEY"),
    .default_model = "claude-sonnet-4-20250514"
});

// Google Gemini
auto gemini = neograph::llm::SchemaProvider::create({
    .schema_path = "gemini",
    .api_key = std::getenv("GEMINI_API_KEY"),
    .default_model = "gemini-2.0-flash"
});

// All three implement the same Provider interface
// Use any of them interchangeably with Agent or GraphEngine
neograph::llm::Agent agent(claude, std::move(tools), "You are helpful.");
```

### MCP Tool Integration

Connecting to an MCP server and using its tools:

```cpp
#include <neograph/mcp/client.h>
#include <neograph/llm/openai_provider.h>
#include <neograph/llm/agent.h>

int main() {
    // Connect to MCP server
    neograph::mcp::MCPClient mcp("http://localhost:3000");
    if (!mcp.initialize()) {
        std::cerr << "Failed to connect to MCP server\n";
        return 1;
    }

    // Discover tools from server
    auto tools = mcp.get_tools();
    std::cout << "Discovered " << tools.size() << " tools\n";

    // Use discovered tools with an Agent
    auto provider = neograph::llm::OpenAIProvider::create({
        .api_key = std::getenv("OPENAI_API_KEY")
    });

    neograph::llm::Agent agent(provider, std::move(tools),
                                "You have access to remote tools via MCP.");

    std::vector<neograph::ChatMessage> messages;
    messages.push_back({"user", "Use the available tools to help me."});

    std::string response = agent.run(messages);
    std::cout << response << "\n";
}
```
