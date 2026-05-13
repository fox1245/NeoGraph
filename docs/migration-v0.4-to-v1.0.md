# Migration: 옛 8-virtual chain → 새 `run(NodeInput)` (v0.4 → v1.0)

NeoGraph v0.4 가 노드의 dispatch 진입점을 `run(NodeInput) ->
awaitable<NodeOutput>` 하나로 통합했습니다. 옛 8개 virtual
(`execute` / `execute_async` / `execute_stream` /
`execute_stream_async` 와 그 `_full` 짝꿍) 은 `[[deprecated]]` 마킹돼
있고 v1.0 에서 삭제됩니다. 이 문서가 옮기는 절차.

> v0.4 ~ v0.x 동안에는 옛 virtual 들이 그대로 동작합니다 — 한 번에
> 다 옮길 필요 없고, 새 노드만 새 모양으로 짜도 무방. 이 문서는
> v1.0 직전 (또는 v0.x 사이클에서 컴파일러 경고가 거슬리기 시작할
> 때) 보면 됩니다.

## 왜 옮기나

옛 모양 — `(sync/async) × (writes/full) × (stream/non-stream)` =
8 virtual cross-product. 어느 하나만 override 하면 다른 7 개에서
default 가 fallback chain 을 타고 호출됨. 안전한 조합도 있고
런타임 함정 (예: sync `execute_full` + async dispatch → nested
`run_sync` race) 도 있어서 사용자가 어느 함수를 override 해야 하는지
명확하지 않음.

새 모양 — `run(NodeInput) -> awaitable<NodeOutput>` 한 개. 하나만
override. sync vs async 는 호출자 안 신경 씀 (사용자가 코루틴 안에
`co_await` 으로 async 호출하든, 그냥 동기 코드 쓰든 자유). Command /
Send 는 `NodeOutput` 에 들어 있어서 추가 virtual 필요 없음. 스트리밍
콜백은 `NodeInput::stream_cb` (포인터, null 가능) 로 들어옴.

## 8 virtual → 새 `run()` 매핑표

| 옛 virtual | 옮긴 모양 |
|---|---|
| `execute(state)` | `co_return NodeOutput{ .writes = {...} };` (sync 본문) |
| `execute_async(state)` | `co_return co_await provider->complete_async(...);` 같은 native async |
| `execute_stream(state, cb)` | `if (in.stream_cb) (*in.stream_cb)(event); co_return NodeOutput{...};` |
| `execute_stream_async(state, cb)` | 위 + native async (`co_await ...`) |
| `execute_full(state)` | `NodeOutput out; out.writes=...; out.command=...; co_return out;` |
| `execute_full_async(state)` | 위 + native async |
| `execute_full_stream(state, cb)` | `execute_full` + `in.stream_cb` 사용 |
| `execute_full_stream_async(state, cb)` | 위 + native async |

핵심: **8 갈래가 `NodeOutput` 의 어느 필드를 채우느냐 + `in.stream_cb`
사용 유무 + `co_await` 사용 유무 의 조합**으로 표현 가능. virtual 은 1개.

## 케이스별 변환 예제

### 케이스 1 — 가장 단순한 sync 노드

**옛:**
```cpp
class MyNode : public GraphNode {
public:
    std::vector<ChannelWrite> execute(const GraphState& state) override {
        int n = state.get("counter").get<int>();
        return {ChannelWrite{"counter", json(n + 1)}};
    }
    std::string get_name() const override { return "my_node"; }
};
```

**새:**
```cpp
class MyNode : public GraphNode {
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        int n = in.state.get("counter").get<int>();
        NodeOutput out;
        out.writes.push_back({"counter", json(n + 1)});
        co_return out;
    }
    std::string get_name() const override { return "my_node"; }
};
```

차이:
- `state` → `in.state`
- 반환값을 `NodeOutput` 에 담음 (`writes` 필드)
- 함수가 `asio::awaitable<NodeOutput>` 이고 마지막에 `co_return`

### 케이스 2 — async LLM 노드 (`execute_async` 옮기기)

**옛:**
```cpp
class TalkNode : public GraphNode {
    std::shared_ptr<Provider> prov_;
public:
    asio::awaitable<std::vector<ChannelWrite>>
    execute_async(const GraphState& state) override {
        auto reply = co_await prov_->complete_async({
            .messages = state.get_messages(),
            .model    = "gpt-mock",
        });
        co_return std::vector<ChannelWrite>{
            {"reply", json(reply.message.content)}
        };
    }
    std::string get_name() const override { return "talk"; }
};
```

**새:**
```cpp
class TalkNode : public GraphNode {
    std::shared_ptr<Provider> prov_;
public:
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto reply = co_await prov_->complete_async({
            .messages = in.state.get_messages(),
            .model    = "gpt-mock",
        });
        NodeOutput out;
        out.writes.push_back({"reply", json(reply.message.content)});
        co_return out;
    }
    std::string get_name() const override { return "talk"; }
};
```

### 케이스 3 — 스트리밍 노드 (`execute_stream` 옮기기)

**옛:**
```cpp
std::vector<ChannelWrite>
execute_stream(const GraphState& state, const GraphStreamCallback& cb) override {
    auto reply = prov_->complete_stream(params, [&](const std::string& chunk) {
        cb({GraphEvent::Type::LLM_TOKEN, "talk", json(chunk)});
    });
    return {ChannelWrite{"reply", json(reply.message.content)}};
}
```

**새:**
```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    // in.stream_cb 는 포인터 — null 이면 호출자가 streaming 안 원함.
    auto on_chunk = [&](const std::string& chunk) {
        if (in.stream_cb) {
            (*in.stream_cb)({GraphEvent::Type::LLM_TOKEN, "talk", json(chunk)});
        }
    };
    auto reply = prov_->complete_stream(params, on_chunk);
    NodeOutput out;
    out.writes.push_back({"reply", json(reply.message.content)});
    co_return out;
}
```

### 케이스 4 — Command / Send 쓰는 노드 (`execute_full` 옮기기)

**옛:**
```cpp
NodeResult execute_full(const GraphState& state) override {
    NodeResult r;
    r.writes.push_back({"step", json("dispatched")});
    r.command = Command{.goto_node = "next_router"};   // 라우팅 강제
    return r;
}
```

**새:**
```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    NodeOutput out;   // NodeOutput == NodeResult — 같은 타입의 alias
    out.writes.push_back({"step", json("dispatched")});
    out.command = Command{.goto_node = "next_router"};
    co_return out;
}
```

`NodeOutput` 은 `NodeResult` 의 별명 — 옛 코드의 `NodeResult` 도
그대로 컴파일됩니다.

## 자주 하는 실수

### `NodeInput in` 은 by-value

```cpp
// ❌ 잘못 — 코루틴 ref-param UAF, pybind async path 에서 SEGV
asio::awaitable<NodeOutput> run(const NodeInput& in) override { ... }

// ✅ 맞음
asio::awaitable<NodeOutput> run(NodeInput in) override { ... }
```

이유: 코루틴 frame 이 인자 사본을 가져야 안전. 참조로 받으면
호출자 stack frame 이 사라진 뒤 `in.state` 가 dangling. PR 2 작업
중 실제로 발생한 버그.

### cancel / store / stream_cb 는 모두 `in.ctx` 에서

옛 노드는 cancel token 을 `state.run_cancel_token_` 같은 smuggling
채널로 받았는데, v0.4 부터는 `RunContext` 가 정식 plumbing:

```cpp
asio::awaitable<NodeOutput> run(NodeInput in) override {
    // 취소 신호 확인
    if (in.ctx.cancel_token && in.ctx.cancel_token->is_cancelled()) {
        throw CancelledException("user cancelled");
    }

    // Store 접근 (issue #27)
    if (in.ctx.store) {
        auto user_pref = in.ctx.store->get({"users", in.ctx.thread_id}, "lang");
        // ...
    }

    // Streaming sink (null 가능)
    if (in.stream_cb) {
        (*in.stream_cb)({GraphEvent::Type::NODE_END, "my_node", json(...)});
    }

    co_return NodeOutput{};
}
```

`in.ctx` 의 다른 필드: `deadline`, `trace_id`, `thread_id`, `step`,
`stream_mode` — 노드가 자기 컨텍스트를 알고 행동을 바꿀 때 활용.

### `_full` virtual 옮기는 사람 — `co_return out;` 한 줄로 끝

옛 `execute_full` 사용자가 가장 자주 헤매는 부분:
"`NodeResult` 는 옛 타입인데 `NodeOutput` 을 반환해야 하나?"
→ 둘은 같은 타입의 별명입니다. `NodeOutput out; out.writes=...;
out.command=...; out.sends=...; co_return out;` 만 하면 됨.

### 옛 virtual 도 같이 두는 transitional 패턴

마이그레이션 중간에 새 `run()` 도 박고 옛 `execute()` 도 그대로
두면, **새 `run()` 이 우선**되어서 호출됩니다 (engine 의 PR 2
dispatch). 옛 본문은 dead code — 안전하게 지우면 됩니다.

```cpp
class MyNode : public GraphNode {
public:
    // 새 진입점 — 우선됨
    asio::awaitable<NodeOutput> run(NodeInput in) override { ... }

    // 옛 진입점 — 호출 안 됨 (dead). 지워도 됨.
    std::vector<ChannelWrite> execute(const GraphState&) override {
        // ...
    }
};
```

## 마이그레이션 안 하면

v0.4 ~ v0.x 동안:
- 옛 virtual override 는 그대로 동작
- `[[deprecated]]` 컴파일러 경고가 뜸 (`-Wdeprecated-declarations`)
- 컴파일 / 런타임 모두 멀쩡 — 그냥 시끄러울 뿐

v1.0 에서:
- 옛 8 virtual 전부 삭제
- 옛 모양 노드는 컴파일 안 됨 (`'execute' marked override but doesn't
  override anything in the base class`)
- 그 시점에 옮겨야 함

선제 마이그레이션 추천 — 노드 개수가 많을수록 v1.0 직후 한꺼번에
옮기는 게 부담. 새 노드부터 새 모양으로 짜고, 옛 노드는 시간 날 때
하나씩.

## 일괄 옮기는 스크립트가 있나?

없습니다 — virtual 시그니처가 8 가지로 다양해서 정규식 변환이 깔끔하지
않습니다. 케이스별 변환 (위 4 가지 예제) 을 사용자가 보고 손으로
옮기는 게 안전.

가장 자주 쓰이는 패턴 (`execute(state)` 만 override) 의 경우 다음
정도의 sed/awk 한 줄이면 1차 변환 가능 — 검토는 사람이 해야 합니다:

```bash
# 매우 거친 1차 변환 — 한 줄짜리 execute 만 override 하는 노드 한정.
# 절대 -i 없이 dry-run 부터.
grep -lE 'execute\(const GraphState' src/**/*.cpp
# 결과 파일 하나씩 열어서 새 모양으로 손편집.
```

복잡한 노드 (`execute_full`, `execute_stream_async` 등) 는 무조건
손편집. 쇼트컷 없습니다.

## 관련 문서 / 이슈

- [`include/neograph/graph/node.h`](../include/neograph/graph/node.h) —
  새 `run(NodeInput)` virtual 의 인라인 docstring (예제 포함)
- [ROADMAP_v1.md](../ROADMAP_v1.md) — Candidate 1 (GraphNode 8-virtual
  flattening) 의 상세 설계 메모
- [troubleshooting.md](troubleshooting.md) — 실제 옮기다 부딪치는
  컴파일 에러 / 런타임 차이 정리
- [Issue #5](https://github.com/fox1245/NeoGraph/issues/5) — 같은 패턴
  을 `Provider` 에도 적용하는 v1.0 Candidate 6 추적용
