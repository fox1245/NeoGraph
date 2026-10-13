<!-- neograph-i18n: source=docs/program-guide.md locale=zh-CN source_sha256=f6877fa2edea4b8e4bebe30ec03143b3d82f0d970761d08e32839b95ea0a8daa -->
# Program 指南：协调图的 JavaScript

**Languages:** [English](program-guide.md) | [한국어](program-guide.ko.md) | [日本語](program-guide.ja.md) | [简体中文](program-guide.zh-CN.md)

图的形状是固定的：节点、边和通道在编译时就确定了。Program 是一个小型 JavaScript 模块，由宿主编译、准入(admission)并运行，位于图的上一层。它导出 `define()`，返回它所拥有的那张图；还可以导出 `main()`，这是一个生成器，决定如何运行这张图：同时跑几份、如何合并它们的结果、预算耗尽时怎么办。运行时会把 `main()` 的每一步记入日志，所以一次运行可以暂停等人，也可以停下来再继续。

本页说明这一模型，并完整演示一个 Program。规则在 [Python 绑定指南](python-binding.md#program-and-quickjs)中，模块可以写什么、不能写什么在[编写规则](program-graphs.md#authoring-rules)中，图的功能在 [Program 图功能](program-graphs.md)中，错误码在[诊断指南](diagnostics.md)中。

## 什么时候用 Program

如果一张图加上它的检查点就够用，就用普通的图。Program 用于图以外的工作：并行运行多份并按规则（`ng.all`、`ng.race`、`ng.quorum`）合并，对整次运行施加预算，记录运行以便在另一个进程中恢复，以及让宿主准入由模型或用户编写的代码。这段 JavaScript 不能访问文件系统、网络、时钟、随机数、`eval`、动态 `import`、`Promise` 和 `async`，只能构建图并向运行时 `yield` 命令。它引用的每个节点类型、条件和归约器，都必须在宿主构建的注册表里，所以生成的 Program 无法运行宿主没有注册的代码。

## 前提条件

Program 层需要包含 Program 和 QuickJS 控制的构建：`pip install neograph-engine` 已经包含，源码构建需要 `-DNEOGRAPH_BUILD_PYBIND=ON -DNEOGRAPH_BUILD_PROGRAM=ON -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON`（然后执行 `pip install pydantic certifi`，并把构建目录加入 `PYTHONPATH`）。

下面的示例用到了失败路由、`fork` 和 SQLite 状态目录。PyPI 上的 0.14.0 wheel 有这些，0.13.1 wheel 没有，所以使用更早的 wheel 时，请用当前源码构建的版本来运行；用到这些功能的示例（Python 32、35 和 36）也一样。想从更小的示例开始，可以运行 [`29_program_hello.py`](../bindings/python/examples/29_program_hello.py)，它用不到一百行完成一个 Program 的编译、准入(admission)和运行。在 C++ 中，请从 [`65_program_javascript.cpp`](../examples/65_program_javascript.cpp) 入手。

## 完整示例：发布检查

一个发布作业并行检查三个服务。`payments` 的检查失败了。作业没有整体失败，而是把失败送到评审步骤，作业在那里等待人工处理。人批准发布后，其他服务不用重新检查，作业随即完成。同一个作业在预算之内运行。

示例由三个 Python 文件组成。`release_program.py` 放每次运行都不变的全部内容，`run_release.py` 和 `restart.py` 使用它。

下面的代码把失败路由、供审阅的暂停、预算和重启放进同一个故事。仓库里为其中每一部分都有一个可运行的单独示例：[`32_program_failure_routing.py`](../bindings/python/examples/32_program_failure_routing.py)（失败路由，以及操作员的 `retry` 或 `abort`）、[`34_program_pause_resume.py`](../bindings/python/examples/34_program_pause_resume.py)（`interruptBefore` 和 `host.resume`）、[`33_program_budgets.py`](../bindings/python/examples/33_program_budgets.py)、[`35_program_fork.py`](../bindings/python/examples/35_program_fork.py) 和 [`36_program_crash_recovery.py`](../bindings/python/examples/36_program_crash_recovery.py)。与之对应的 C++ 示例是 [66](../examples/66_program_failure_routing.cpp) 到 [69](../examples/69_program_crash_recovery.cpp)。

### 1. 节点代码

节点是普通的 Python。`check` 对 `payments` 会失败。失败通过异常的 `code`、`public_message` 和 `retryable` 属性来说明自己；异常的文本不会离开节点。`Review` 在有人回答之后运行，这个回答就是它的恢复值。

```python
import hashlib

import neograph_engine as ng


# 1. The node code. Programs refer to these by name; they stay ordinary Python.

CHECKED = []   # which services ran their checks, to show that nothing runs twice

class ChecksFailed(Exception):
    code = "release.checks_failed"        # an author code: lower-case, dotted
    public_message = "the release checks failed"
    retryable = True


@ng.node("release.check")
def check(state):
    service = state.get("service")
    CHECKED.append(service)
    if service == "payments":
        raise ChecksFailed(f"{service}: 2 tests failed")
    return [ng.ChannelWrite("report", [f"{service}: checks passed"])]


class Review(ng.GraphNode):
    """Runs after a person answered; the answer is the resume value."""

    def __init__(self, name):
        super().__init__()
        self._name = name

    def get_name(self):
        return self._name

    def run(self, input):
        answer = input.ctx.resume_value or {}
        return [ng.ChannelWrite("decision", answer.get("decision", "hold"))]


@ng.node("release.ship")
def ship(state):
    return [ng.ChannelWrite("report", [f"{state.get('service')}: shipped"])]


@ng.node("release.hold")
def hold(state):
    return [ng.ChannelWrite("report", [f"{state.get('service')}: held"])]


ng.NodeFactory.register_type("release.review", lambda name, config, ctx: Review(name))
ng.ConditionRegistry.register_condition(
    "release.decision", lambda state: state.get("decision") or "hold")
```

### 2. 注册表

注册表是 Program 可以使用的可执行名称的列表，每个名称带有版本和摘要。宿主用已经在当前进程中注册的节点、条件和归约器来构建它。

```python
# 2. The registry: the only executable names a Program may use.

def digest(label):
    return "sha256:" + hashlib.sha256(label.encode()).hexdigest()


def build_registry():
    builder = ng.ProgramRegistryBuilder()
    for node_type in ("release.check", "release.review", "release.ship", "release.hold"):
        builder.add_registered_node(node_type, "1.0.0", digest(node_type))
    builder.add_registered_condition("release.decision", "1.0.0", digest("release.decision"))
    for reducer in ("append", "overwrite"):
        builder.add_registered_reducer(reducer, "1.0.0", digest(reducer))
    return builder.build()
```

### 3. Program

`define()` 描述图。`check` 会尝试两次（`max_retries: 1`）。如果仍然失败，不终止运行，而是把失败送到 `review`，并在 `failures` 通道追加一条失败记录。`interruptBefore` 拦住了 `review`，所以运行停在那里等待回答。回答到来后，条件 `release.decision` 把运行引向 `ship` 或 `hold`。`hold` 是失败退出节点：它以错误码 `release.held` 让运行以 `Failed` 结束。

`main()` 用 `ng.all` 对每个服务运行一次这张图，同时最多三个，并按服务的顺序返回报告。

```python
# 3. The Program: define() describes the graph, main() coordinates runs of it.

SOURCE = r'''
export function define() {
  const g = ng.graph("release");
  g.channel("service", {reducer: "overwrite", initial: ""});
  g.channel("report", {reducer: "append", initial: []});
  g.channel("failures", {reducer: "append", initial: []});
  g.channel("decision", {reducer: "overwrite", initial: ""});
  g.node("check", {type: "release.check"});
  g.node("review", {type: "release.review"});
  g.node("ship", {type: "release.ship"});
  g.node("hold", {type: "release.hold"});
  g.entry("check");
  g.edge("check", "ship");
  g.exit("ship");
  // Two attempts. Then the failure goes to "review" instead of failing the run.
  g.retryPolicy("check", {max_retries: 1, initial_delay_ms: 10, backoff_multiplier: 1,
                          max_delay_ms: 10, on_failure: "review", error_channel: "failures"});
  g.interruptBefore("review");               // a person answers before "review" runs
  g.conditionalEdge("review", "release.decision", {approve: "ship", hold: "hold"});
  g.failureExit("hold", {code: "release.held"});
  return g;
}

export function* main(input) {
  const results = yield ng.all(
    input.services.map((name) => ng.callCore("release", {service: name}, "release:" + name)),
    {max_in_flight: 3}, "fan-out");
  return results.map((result) => result.report);
}
'''
```

有两条规则要记住。`ng.callCore` 只能指定 `define()` 返回的那张图，名称就是传给 `ng.graph()` 的名称。另外，失败的命令会终止运行：生成器不会恢复执行，所以包在 `yield` 外面的 `try`/`catch` 永远不会执行。请像这里的 `retryPolicy` 一样在图里处理失败，或者在宿主中读取 `result.status` 和 `result.failure`。

### 4. 预算与宿主

宿主为它准入的每个 Program 设置上限。Program 与一份运行预算一同准入，该版本的每次运行都必须使用完全相同的预算。上限的各字段，除两个子程序限制外，即使 Program 从不调用模型也必须为正数。

```python
# 4. The budgets. The host ceiling bounds every Program; the run budget bounds one run.

def budget(**fields):
    value = ng.ProgramRunBudget()
    for name, number in fields.items():
        setattr(value, name, number)
    return value


ceiling = budget(wall_time_ms=120_000, model_tokens=1, monetary_microunits=1,
                 max_concurrency=8, max_program_operations=64, max_core_steps=2_000,
                 max_dynamic_compiles=1)
run_budget = budget(wall_time_ms=10_000, max_concurrency=6, max_program_operations=16,
                    max_core_steps=50)


def admit(host, run_budget):
    """Compile the Program, check it against the host's ceiling and return its version."""
    source = ng.ProgramSource.from_javascript("release.js", SOURCE)
    return host.compile_admit(source, run_budget)


def make_host(state_dir=None):
    host = ng.LocalProgramHost(build_registry(), "tenant:docs", ceiling,
                               sqlite_directory=state_dir)
    return host, admit(host, run_budget)
```

`compile_admit` 编译源码，检查构建产物是否超出上限，并返回 `ProgramVersion`。源码、注册表和预算相同，则在另一个进程中得到相同的版本 ID；测试会跨进程检查这一点。

### 5. 运行并暂停

创建 `run_release.py`：

```python
from release_program import CHECKED, admit, budget, make_host, run_budget

host, version = make_host()

# Run: "payments" fails its checks, so the run pauses for a person.

services = {"services": ["api", "payments", "web"]}
paused = host.run(version, services, run_budget)
print(paused.status)
print(paused.interrupt.core_node, paused.interrupt.value["failure"]["code"])
```

```text
ProgramTerminalStatus.Interrupted
review release.checks_failed
```

`api` 和 `web` 完成了检查。`payments` 失败了两次，所以它的失败路由到了 `review`，运行以状态 `Interrupted` 停下。`result.interrupt.core_node` 指出在等待的节点，`result.interrupt.value` 中是 `{"reason": "failure_route", "failure": {...}}`，其中失败记录包含 `node`、`kind`（这里是 `exhausted`）、`code`、`attempts`、`retryable`、`step`、`routed_to` 和 `effects`。`api` 和 `web` 的报告已经在这次运行里了。

### 6. 分叉出去试另一个回答

```python
# Fork the paused run to see what "hold" would do. The original stays paused.
# The fork takes a share of the paused run's remaining budget.

fork_budget = budget(wall_time_ms=2_000, max_concurrency=3, max_program_operations=8,
                     max_core_steps=20)
forked = host.fork(paused.run_id, {"decision": "hold"}, version, fork_budget).wait()
print(forked.status, forked.failure.code)
```

```text
ProgramTerminalStatus.Failed release.held
```

`fork` 从暂停运行的检查点开始一次新的运行，并用 `"hold"` 回答那个问题。分叉以失败退出的错误码 `Failed` 结束。原来的运行仍处于暂停状态。已经完成的工作会被重放，不会重新运行。

分叉的预算是原运行剩余预算中切出的一份。`wall_time_ms`、`model_tokens`、`monetary_microunits`、`max_program_operations`、`max_core_steps` 和 `max_dynamic_compiles` 是量：分叉从原运行剩下的量里取走自己的份额，其余仍归原运行。这个例子中，原运行保留剩余的 `wall_time_ms` 减去 2,000，以及剩余的 `max_core_steps` 减去 20。`max_concurrency` 是上限，不是量：分叉有自己的上限，不超过原运行的，原运行保留自己的，所以分叉的 3 不会占用原运行的并发。如果分叉会让原运行的 `wall_time_ms` 或 `max_core_steps` 变成 0，`fork` 调用会以 `P_FORK_SOURCE_BUDGET` 拒绝它；比原运行剩余量更大的分叉则以 `P_FORK_BUDGET` 拒绝。两种情况都不会改变原运行。

在 fork 中再次运行的节点（例如提问的那个）会重复它在提问之前发出的 provider 调用。有持久的 provider 调用 journal 时，fork 会重放这些调用，所以不会再次发送，也不会再次计费；`LocalProgramHost` 在设置了 `sqlite_directory` 时有这样的 journal。全部放在内存里的宿主没有 journal，所以那里的 fork 会把这些调用再发送一次、再计一次费。这不是错误，fork 发送和计费的内容也不变，但 fork 会告知这一点：如果暂停的那次尝试在暂停的操作里记录了 provider 调用，`fork` 返回的句柄上的 `handle.notices` 就有一条代码为 `P_FORK_PROVIDER_CALLS_NOT_JOURNALED` 的通知，它的 `witness` 列出这些调用（`call_id`、`node`、`model`、`microunits`）及其合计。通知无法说明其中哪些调用会在 fork 中再次运行：在暂停那一步中已完成的节点所发出的调用，会从该步保存的写入重放，不会再次发送。在有 journal 的宿主上做的 fork，或者暂停的那次尝试没有记录这类调用的 fork，没有通知。通知不随运行一起保存，所以 `reconnect` 返回的句柄上没有。详情见 [Python 绑定指南](python-binding.md#in-memory-state-and-resume)和[定价证据与价格目录](reference-en.md#pricing-evidence-and-the-price-catalog)。

### 7. 用回答恢复运行

```python
# Resume the original with "approve". The other services do not run again.

done = host.resume(paused.run_id, {"decision": "approve"}).wait()
print(done.status)
print(done.output)
print(sorted(CHECKED))
```

```text
ProgramTerminalStatus.Completed
[['api: checks passed', 'api: shipped'], ['payments: shipped'], ['web: checks passed', 'web: shipped']]
['api', 'payments', 'payments', 'web']
```

`resume` 回答待决的问题，并继续同一次运行。因为人批准了，`payments` 跳过检查直接发布。最后一行显示没有任何东西运行两次：`api` 和 `web` 各出现一次，`payments` 出现两次（它两次失败的尝试）。

### 8. 太小的预算

```python
# A budget that is too small refuses the join before any member starts.

small = budget(wall_time_ms=10_000, max_concurrency=6, max_program_operations=2,
               max_core_steps=50)
refused = host.run(admit(host, small), services, small)
print(refused.status, refused.failure.code)
```

```text
ProgramTerminalStatus.BudgetExhausted P_PROGRAM_OPERATION_BUDGET
```

汇合需要的 Program 操作次数超过了这份预算（`max_program_operations=2`）允许的数量。运行时在启动任何成员之前就拒绝整个汇合，所以没有任何服务得到检查。换预算就要重新 `compile_admit`，得到新的版本；用与已准入预算不同的预算调用 `host.run` 会抛出 `P_START_BUDGET`。

### 9. 经受住重启

默认情况下，`LocalProgramHost` 把一切放在内存里，`resume` 只能通过启动该运行的宿主对象工作。指定 `sqlite_directory` 后，宿主把运行、版本和检查点保存在 SQLite 文件中，新的进程就可以继续这次运行。它必须使用相同的注册表，并准入相同的 Program。

创建 `restart.py`：

```python
import sys

from release_program import make_host, run_budget

state_dir, step = sys.argv[1], sys.argv[2]
host, version = make_host(state_dir)

if step == "start":
    paused = host.run(version, {"services": ["api", "payments", "web"]}, run_budget)
    print(paused.status, paused.run_id)
else:
    done = host.resume(sys.argv[3], {"decision": "approve"}).wait()
    print(done.status, done.output)
```

```bash
python restart.py state start
python restart.py state resume run-...
```

第一条命令打印状态 `Interrupted` 和运行 ID；把该 ID 传给第二条命令，它会打印 `Completed` 和报告。目录 `state` 中有 `program.db`、`checkpoints.db` 和 `provider_calls.db`。

进程停止时仍处于运行状态（而不是暂停）的运行，用 `host.reconnect(run_id)` 继续，具体怎么继续取决于 Program。没有子预算（`max_child_depth` 和 `max_total_children` 都是 0，这是默认值）的、只有 `define()` 的 Program，会在其 `wall_time_ms` 内持有执行租约，以免别的进程接着运行一个所有者可能仍然存活的运行。租约到期前，`reconnect` 会抛出 `P_RUN_LEASE_HELD`；到期后，它让运行以 `TimedOut` 结束，而不会继续运行。带有 `main()` 的 Program，或带子预算（两者都大于 0）启动的 Program，不持有租约，`reconnect` 会立即从最近一个检查点继续，重放已记入 journal 的模型调用，并只计一次费（[`36_program_crash_recovery.py`](../bindings/python/examples/36_program_crash_recovery.py)）。

## `define()` 能表达什么

`ng.graph(name)` 返回一个构建器。所有方法都返回该构建器。

| 方法 | 含义 |
|---|---|
| `channel(name, {reducer, initial})` | 带归约器（`overwrite`、`append` 或已注册的归约器）的状态通道。 |
| `node(name, {type, ...config})` | 已注册类型的节点；其他键是它的配置。 |
| `edge(from, to)`、`conditionalEdge(from, condition, routes)` | 固定边和带分支的边。条件是已注册的名称。 |
| `entry(node)`、`exit(node)` | 连接 `__start__` 和 `__end__`。 |
| `barrier(node, waitFor)` | 等待所列全部节点或 `{any_of: [...]}` 槽位的汇合。 |
| `interruptBefore(...)`、`interruptAfter(...)` | 在节点之前或之后暂停运行，等待回答。 |
| `retryPolicy(node, {...})` | 重试、超时，以及带 `on_failure` 时的失败边。 |
| `failureExit(node, {code})` | 以该错误码让运行以 `Failed` 结束的节点。 |
| `metadata({...})` | 关于图的声明，不授予任何权限。 |

`ng.javascript_authoring_capability_manifest()` 返回当前构建接受的确切词汇表，TypeScript 声明在 `dts_prelude` 中。不太常用的键见 [Program 图功能](program-graphs.md)。

## `main()` 能 `yield` 什么

每个 `yield` 向运行时交出一个命令，命令完成后收到它的结果。

| 命令 | 结果 |
|---|---|
| `ng.callCore(name, input, site)` | 运行图并返回它的输出：`channels` 中是每个通道的 `value` 和 `version`，每个通道的值也以通道名为键直接给出。 |
| `ng.all(members, options, site)`（`ng.parallel` 相同） | 并发运行各成员命令，每次最多 `max_in_flight` 个，并按给定顺序返回结果。任一成员失败则整个运行失败。 |
| `ng.race(members, options, site)` | 返回最先完成的成员的结果。 |
| `ng.quorum(members, required, site)` | 达到所需数量的成员成功后返回。 |
| `ng.await(command, timeoutMs, site)` | 等待一个命令，直到截止时间。截止时间一过，运行以 `TimedOut` 结束，错误码为 `P_AWAIT_TIMEOUT`。 |
| `ng.spawn(binding, input, site)` | 启动子 Program。需要带子程序绑定解析器的 C++ 宿主；`LocalProgramHost` 没有，会以 `P_CHILD_BINDING` 失败。 |
| `ng.emit(value, site)`、`ng.checkpoint(value, site)` | 把 `value` 记入运行日志并原样返回。各计为一次 Program 操作。 |
| `ng.cancelScope(scope, reason, site)` | 取消运行（`scope` 为 `"current"`）；运行以 `Cancelled` 结束，错误码为 `P_RUNTIME_CANCELLED`。 |

每个命令的最后一个参数是调用位置的标签。`ng.all` 的某个成员暂停，整个运行就暂停：其余成员会完成，运行变为 `Interrupted`，由 `resume` 回答暂停的那个成员。

## 状态

`ProgramResult.status` 是 `ProgramTerminalStatus`。状态不是 `Completed` 时，请读取 `result.failure`（`code`、`message`、`core_node`、`witness`）。

| 状态 | 含义 |
|---|---|
| `Completed` | `main()` 返回了值，或图到达了出口。`result.output` 中是该值。 |
| `Interrupted` | 运行在等待回答；`result.interrupt` 指出位置。用 `resume` 或 `fork` 继续。 |
| `Failed` | 图没有路由的失败、失败退出，或 `main()` 抛出了异常。`result.failure.code` 给出具体原因。 |
| `BudgetExhausted` | 某项有预算的资源耗尽，例如 `P_PROGRAM_OPERATION_BUDGET`、`P_CORE_STEP_BUDGET` 或 `P_CONCURRENCY_BUDGET`。模型 token 无论是因某次响应使运行超出预算，还是因下一次调用在发送前被拒绝，都以 `NG.BudgetExhausted` 结束。金额和 Core 步骤会带着一条说明运行超出多少的记录结束（见[预算](#budgets)），运行在超出预算后才结束时也是如此。 |
| `TimedOut` | 运行的 `wall_time_ms` 或某个 `ng.await` 的截止时间已过。在 `wall_time_ms` 之后才完成的运行同样是 `TimedOut`（见[预算](#budgets)）。 |
| `Cancelled` | `ng.cancelScope` 或句柄的 `cancel()` 停止了运行。 |

另外还有 `AmbiguousEffect` 和 `CheckpointIncompatible`，见 `include/neograph/program/result.h`。

<a id="budgets"></a>
## 预算

`ProgramRunBudget` 有十个字段：`wall_time_ms`、`model_tokens`、`monetary_microunits`、`max_concurrency`、`max_program_operations`、`max_core_steps`、`max_dynamic_compiles`、`max_child_depth`、`max_total_children` 和 `max_node_retries`。`result.usage` 统计一次运行用掉的量，`result.remaining_budget` 是剩余的量。零表示什么都不授予：`model_tokens = 0` 的运行如果调用模型，会以 `BudgetExhausted` 结束。上限、已准入的预算以及模型成本规则，见 [Python 绑定指南](python-binding.md#budgets)。

运行超出预算而结束时，会带有一条指明是哪项预算的 failure 记录，witness 则说明超出了多少。`failure.witness` 在该预算的键下有一项，含 `granted`、`used` 和 `over_by`；运行恰好停在额度上，或模型调用在发送前被拒绝时，`over_by` 为 0。

| 预算 | 状态 | `failure.code` | witness 键 |
|---|---|---|---|
| `wall_time_ms` | `TimedOut` | `P_RUNTIME_TIMEOUT` | `wall_time_ms` |
| `model_tokens` | `BudgetExhausted` | `NG.BudgetExhausted` | `model_tokens` |
| `monetary_microunits` | `BudgetExhausted` | `P_MONEY_BUDGET` | `monetary` |
| `max_core_steps` | `BudgetExhausted` | `P_CORE_STEP_BUDGET` | `core_steps` |

无论是预算在中途停下了运行，还是运行在超出预算后才结束，记录都是一样的。用量在 plan 完成之后才结算，所以 plan 可能超出额度才结束：响应的费用可能高于为该调用预留的金额，忽略取消的节点可能在截止时间之后才返回，子图节点的步骤会计入运行的 Core 步骤，却不会让包含它的图停下来。运行同时超出多项预算时，witness 为每一项各有一条，status 和错误码取自表中排在最前的那一项（即预算字段的顺序）。无论运行还用了什么，截止时间都会使运行以 `TimedOut` 结束，所以 wall time 排在最前。需要计价的调用在发送前被拒绝时，拒绝的证据保存在 `monetary` 之下：预留额、`ceiling` 和 `used`。

## 行为在哪里测试

示例的每一步都对应 `bindings/python/tests/program_e2e` 中的场景：`test_07_main_programs.py`（`ng.all`、`race`、`quorum`、`await`）、`test_10_failure_routing.py`（失败路由与批准）、`test_02_hitl.py`（中断与恢复，包括在内存中和重启之后）、`test_04_budgets.py`（预算），以及 `test_13_fork_replays_the_paused_step.py` 和 `test_20_javascript_fork.py`（分叉）。[该目录的 README](../bindings/python/tests/program_e2e/README.md) 列出了全部场景。示例没有涉及的 Hook、provider 调用和子 Program，见 [Python 绑定指南](python-binding.md)和[递归 Harness](PROGRAM_RECURSIVE_HARNESSES.md)。
