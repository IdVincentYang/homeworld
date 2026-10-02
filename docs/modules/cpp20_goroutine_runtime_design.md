# 用 C++20 协程设计 Go 风格 goroutine 运行时

本文给出一套从小到大的设计骨架，并用代码说明关键机制。示例用于讲解接口和并发约束；`Runtime` 的线程池、ready queue、timer wheel 等仍需按项目实现，代码不是可直接链接的完整库。

C++20 提供协程帧、`co_await` / `co_yield` 和 `coroutine_handle`，不提供 goroutine 调度器、通道、I/O poller、可增长本机栈或 worker pool。Go 运行时把 G（goroutine）、M（OS thread）和 P（执行许可/调度资源）分开管理；G 有自己的用户栈，栈可增长和收缩。[Go runtime HACKING：G、M 与栈](https://go.dev/src/runtime/HACKING) [Go stack 实现](https://go.dev/src/runtime/stack.go)

## 1. 先确定栈模型

若“独立栈”指跨挂起点保留局部变量，C++ 协程帧可以满足：编译器把跨 `co_await` 仍存活的值放进协程帧。若它指每个 G 有独立的本机 SP，且任意普通 C/C++ 函数深处都能暂停，单用 C++20 协程不够；需要 stackful context / Fiber 后端，或把可能挂起的整个调用链改为协程。

```cpp
// 普通函数调用链不能在中间被 C++20 协程自动冻结：
void syncA() { syncB(); }   // syncB 里没有 co_await 边界

// 可暂停的调用链要显式传播 await：
Task<> asyncA() {
    co_await asyncB();      // asyncB 挂起时，asyncA 也保存为协程帧
}
```

`co_await` 因此保存的是一串协程帧和 continuation，不是一整块可自由展开的同步栈。不要在协程帧仍活动时任意搬动它；用户可能保存了指向帧内对象的指针。帧通常比给每个 G 预留大块栈节省空间，但深层 `co_await` 可能产生多帧和分配。可按测量结果加入帧池或 promise 自定义分配器。[C++20 协程帧与分配规则：N4860](https://isocpp.org/files/papers/N4860.pdf)

## 2. 把一个 G 表示为控制块和协程帧

控制块保存运行时需要的身份、状态和策略；协程帧保存 promise、恢复位置、await 状态和跨挂起点存活的局部变量。

```cpp
enum class GState : unsigned char { New, Runnable, Running, Waiting, Done };

struct GControl {
    Runtime* runtime = nullptr;
    std::coroutine_handle<> coroutine{}; // GControl 持有唯一销毁权
    std::mutex resumeGate;                // 同一 G 不会并发执行/销毁
    std::atomic<GState> state{GState::New};
    std::atomic<bool> cancelRequested{false};
    std::exception_ptr error;             // Done 发布后读取
    LogicalContext context;               // trace、请求上下文等
    std::optional<WorkerId> affinity;      // 需要线程亲和时设置
};
```

协程返回类型的 promise 把帧和对应的 G 联系起来。这里让新协程先暂停，交给运行时排队后才开始执行；完成时也先停在 final suspend，由持有者收尾并销毁帧。

```cpp
struct GTask {
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;
    handle_type handle{}; // 示例中的轻量返回 token；生产代码应封装为 move-only owner

    struct promise_type {
        GControl* owner;
        explicit promise_type(GControl& g) noexcept : owner(&g) {}

        GTask get_return_object() noexcept {
            return {handle_type::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {
            owner->error = std::current_exception();
        }
    };
};

GTask workerMain(GControl& g) {
    while (!g.cancelRequested.load(std::memory_order_acquire)) {
        doOneUnitOfWork();
        co_await yield(g);
    }
}
```

创建时先初始化控制块，再创建协程帧，并把 handle 的唯一所有权交给运行时：

```cpp
void Runtime::spawn(GControl& g) {
    g.runtime = this;
    GTask task = workerMain(g);        // initial_suspend 阻止函数体先行执行
    g.coroutine = task.handle;
    task.handle = {};                  // 所有权转给 GControl
    g.state.store(GState::Runnable, std::memory_order_release);
    enqueue(g);                         // 只排队，不在此处内联 resume
}
```

示例省略了返回值模板、`join()` 和 move-only handle owner；生产实现应让唯一的运行时所有者负责 `destroy()`，避免复制裸 handle 后重复销毁。`promise_type` 的异常策略也可以改成错误码或 `exception_ptr` 结果；异常不能越过 worker 线程边界逃逸。

## 3. yield、ready queue 与唯一 resume 所有权

`co_await yield(g)` 把当前 G 从 `Running` 转成 `Runnable` 并放入队列。事件通知也只做状态转换和入队；真正调用 `resume()` 的是 worker。语言规则会在调用 `await_suspend()` 前把协程视为已挂起，所以发布 handle 后，另一个线程可能很快就恢复它；awaiter 不能在发布后继续读写自身或协程帧。[`await_suspend` 规则](https://eel.is/c++draft/expr.await)

```cpp
struct YieldAwaiter {
    GControl& g;
    bool await_ready() const noexcept { return false; }

    void await_suspend(GTask::handle_type) const noexcept {
        GState expected = GState::Running;
        if (!g.state.compare_exchange_strong(
                expected, GState::Runnable,
                std::memory_order_acq_rel)) {
            std::terminate(); // 状态不符合协议，不能继续排队
        }
        g.runtime->enqueue(g);
        // 入队后可能马上被别的 worker 取走；此后不再访问协程帧/awaiter。
    }

    void await_resume() const noexcept {}
};

YieldAwaiter yield(GControl& g) noexcept { return {g}; }
```

worker 对同一个 G 用 `resumeGate` 串行化 `resume()` 和销毁。await_suspend 可以把 G 发布给其他 worker；新 worker 即使立刻拿到队列项，也要等旧的 `resume()` 返回并释放 gate。这个 gate 由 worker 外层持有，并由同一个 worker 在 `resume()` 返回后释放；协程自己的局部锁不能这样跨挂起点持有。旧 worker 返回后只检查完成状态，不会再与新 worker 同时执行协程帧。

```cpp
void Runtime::runOne(GControl& g) {
    bool completed = false;
    {
        std::lock_guard gate(g.resumeGate);
        GState expected = GState::Runnable;
        if (!g.state.compare_exchange_strong(
                expected, GState::Running,
                std::memory_order_acq_rel)) {
            return; // 过期/重复队列项
        }

        auto h = g.coroutine;
        h.resume();

        // 若协程 yield，它已自行改为 Runnable 并入队；若等待则为 Waiting。
        // 两种情况都不能在这里覆盖它的新状态。
        if (h.done()) {
            g.coroutine = {};
            h.destroy();
            g.state.store(GState::Done, std::memory_order_release);
            completed = true;
        }
    }
    if (completed) notifyJoiners(g);
}
```

`enqueue()` 必须是线程安全且不会失败的发布操作，不能直接在调用线程内递归执行 `runOne()`。`await_suspend()` 是 `noexcept` 时尤其如此：队列应预留容量/使用侵入式节点，或把失败转换为可观察的错误，不能让分配异常穿过 `noexcept`。对一个 G 的状态转换应使用 CAS 或等价锁保护：`Runnable → Running` 只允许一个 worker 成功；多个事件同时唤醒时，只允许一个事件完成 `Waiting → Runnable`。队列锁或 release/acquire 原子操作还要让 awaiter 写入的结果先于恢复后的读取。

上述串行 gate 是示意性实现，吞吐量优化时可换成明确的 resume lease / 执行权状态机。关键不变量是：**一个 G 同时最多有一个执行者；`resume()` 返回后若 G 未完成，旧执行者不再修改其状态；销毁前没有 worker 正在执行它。**

## 4. timer、I/O 和跨线程唤醒

`await_suspend()` 注册等待条件并返回；timer 或 I/O 线程只唤醒 G、把它放回 ready queue，不直接 `resume()`。因此任意 OS 回调线程都能完成通知，协程本身仍由 runtime 的 `std::thread` worker 执行。

```cpp
struct SleepAwaiter {
    Runtime& runtime;
    GControl& g;
    Clock::time_point deadline;

    bool await_ready() const noexcept { return Clock::now() >= deadline; }

    void await_suspend(GTask::handle_type) const noexcept {
        GState expected = GState::Running;
        if (!g.state.compare_exchange_strong(
                expected, GState::Waiting,
                std::memory_order_acq_rel)) {
            std::terminate();
        }
        runtime.armTimer(deadline, [&runtime = runtime, &g = g] {
            runtime.wake(g); // callback 只入队；由 worker 调用 resume()
        });
        // 注册之后可能立即触发 timer；不要再访问协程帧中的 awaiter 状态。
    }

    void await_resume() const {
        if (g.cancelRequested.load(std::memory_order_acquire)) {
            throw Cancelled{}; // 也可返回错误值
        }
    }
};

void Runtime::wake(GControl& g) {
    GState expected = GState::Waiting;
    if (g.state.compare_exchange_strong(
            expected, GState::Runnable,
            std::memory_order_acq_rel)) {
        enqueue(g); // CAS 失败代表另一个事件/取消已经赢得唤醒
    }
}
```

真实 timer API 的 callback 可能比 `await_suspend()` 返回得更早；示例的 gate 让新 worker 等待旧 `resume()` 完成挂起交接。I/O poller 也采用同样的入队路径。阻塞系统调用放到专用 blocking pool，否则少数慢调用就能占满所有 coroutine worker。

示例把 `await_suspend()` 标为 `noexcept`，因此 `armTimer()` 也必须保证不抛异常；若注册可能失败，应在 awaiter 内捕获失败、保存错误结果并安排 G 恢复，让错误由 `await_resume()` 返回或抛出。

timer、I/O 和 channel waiter 必须在回调彻底注销前保持 G 控制块有效。生产实现通常让事件注册持有带 generation 的 G 引用/弱引用；不能只捕获可能被释放或复用的裸地址。销毁前要撤销注册，或让迟到回调通过 generation 检查后安全丢弃。

C++ 当前协程恢复规则规定：在不同 execution agent 上恢复通常是 implementation-defined；当 agent 是 `std::thread` / `std::jthread` 实例或执行 `main` 的线程时适用例外。可移植设计应由标准线程 worker 恢复，避免直接从任意 OS callback 线程恢复。同一 coroutine 的并发恢复可能产生数据竞争；跨线程后也不能假设 `thread_local` 值或线程身份不变。[协程恢复规则](https://eel.is/c++draft/coroutine.handle.resumption) [C++20 草案 N4860](https://isocpp.org/files/papers/N4860.pdf)

## 5. channel 等待队列怎样唤醒接收者

通道的接收 awaiter 先检查缓冲区，再在 `await_suspend()` 里加锁复查，避免“检查为空后、登记 waiter 前，发送方已经送达”的丢唤醒。发送方先把数据写入挂起接收者的结果槽，再解锁并唤醒 G。

```cpp
template<class T>
struct ReceiverWaiter {
    GControl* g;
    std::optional<T>* result; // 指向挂起协程帧里的接收 awaiter
};

template<class T>
struct ReceiveAwaiter {
    Channel<T>& channel;
    GControl& g;
    std::optional<T> result;

    bool await_ready() {
        std::lock_guard lock(channel.mutex);
        if (channel.buffer.empty()) return false;
        result.emplace(popFront(channel.buffer));
        return true;
    }

    bool await_suspend(GTask::handle_type) {
        std::lock_guard lock(channel.mutex);
        if (!channel.buffer.empty()) { // 与 await_ready 之间可能有 sender
            result.emplace(popFront(channel.buffer));
            return false;              // 不挂起，直接进入 await_resume
        }
        setState(g, GState::Running, GState::Waiting);
        channel.receivers.push_back({&g, &result});
        return true;
    }

    T await_resume() { return std::move(result.value()); }
};
```

发送路径的关键顺序如下。省略了关闭、容量、sender 队列和错误传播：

```cpp
void Channel<T>::send(T value) {
    GControl* receiver = nullptr;
    {
        std::lock_guard lock(mutex);
        if (receivers.empty()) {
            buffer.push_back(std::move(value));
        } else {
            auto waiter = popFront(receivers);
            waiter.result->emplace(std::move(value));
            receiver = waiter.g;
        }
    }
    if (receiver) runtime.wake(*receiver); // 解通道锁后再入 ready queue
}
```

多个完成源（发送、超时、取消）可能竞争同一个 G；用 `Waiting → Runnable` 的单次 CAS 决定胜者。胜出后要取消其他注册，或让它们成为可识别的过期 waiter。生产通道还需处理关闭语义、背压、`select` 同时等待多个通道、发送者与接收者公平性，以及 waiter 与协程帧的生命周期。这里的片段展示协议，不是完整 channel 实现。

## 6. 取消、线程亲和与逻辑上下文

取消应先设置请求位。若 G 正在等待，runtime 可尝试唤醒它；awaitable 恢复后检查取消并执行协程内清理。不能从另一个线程直接 destroy 一个正在运行的协程。

```cpp
void Runtime::requestCancel(GControl& g) {
    g.cancelRequested.store(true, std::memory_order_release);
    if (g.state.load(std::memory_order_acquire) == GState::Waiting) {
        wake(g); // CAS 处理与 I/O/timer 完成同时发生的情况
    }
}

GTask download(GControl& g) {
    Resource resource = acquireResource();
    while (!finished()) {
        if (g.cancelRequested.load(std::memory_order_acquire)) co_return;
        co_await waitForNetwork(g);
        consumeChunk();
    }
}
```

跨 worker 迁移后，OS `thread_local` 属于 worker，不属于 G。需要 goroutine-local 语义的数据应放进 `GControl::context`，在每次 resume 前安装、resume 返回后恢复 worker 原来的上下文：

```cpp
auto previous = currentLogicalContext;
currentLogicalContext = g.context;
h.resume();                         // 可能在另一个 worker 上继续
g.context = currentLogicalContext; // 保存协程刚更新的逻辑上下文
currentLogicalContext = previous;
```

如果 G 使用 UI、COM、图形设备等线程亲和 API，为它设置 `affinity` 并只在指定 worker 执行；也可以在 `co_await` 前后显式切换到目标 executor。不要把线程绑定 mutex、线程 ID 或必须在同一 OS 线程释放的资源跨挂起点带走。

## 7. 还要补齐的运行时部件

| 技术点 | 最小实现方向 | 主要边界 |
| --- | --- | --- |
| worker pool / G:M:N | 每 worker 本地队列、全局注入队列、work stealing；限制同时运行数。 | 关闭、唤醒、队列饥饿和 worker 休眠。 |
| 状态和 handle 所有权 | `New/Runnable/Running/Waiting/Done` 状态机；单一 owner 负责 destroy。 | 重复唤醒、重复 resume、停止中的 G 和复用 ID。 |
| `join` 与父子任务 | promise 保存结果；父 G 等待子 G 完成后由 continuation 唤醒。 | 异常传播、父取消子、detached G 的最终回收。 |
| channel / semaphore / select | 锁保护队列，多个完成源用一次性状态转换选出胜者。 | 关闭、背压、公平性、取消时清除 waiter。 |
| timer / I/O | timer heap 或 wheel；socket 使用非阻塞 I/O + OS poller。 | callback 只通知 runtime；不要直接在 poller 线程 resume。 |
| 阻塞调用 | 专用 blocking pool 或异步替代 API。 | 不能让阻塞调用耗尽 coroutine worker。 |
| 公平与抢占 | 长循环显式 `co_await yield()` 或检查预算/取消标志。 | `co_await` 是协作式；无挂起点的 CPU 任务不能被安全抢占。 |
| 结果与异常 | promise 存结果或 `exception_ptr`；`join` 时观察。 | 异常不能逃出 worker 线程入口。 |
| 分配与观测 | promise allocator / 帧池；统计帧字节、队列等待和迁移。 | 池化需覆盖异常创建、取消及最终销毁。 |
| 调试与追踪 | G ID、等待原因、continuation 链和逻辑栈回溯。 | 普通 native backtrace 看不到完整异步调用链。 |

## 8. 推荐的实现顺序

1. 单线程 ready queue：实现根协程、`yield`、完成和 `join`，先固定 handle 的所有权规则。
2. 等待原语：增加 timer、semaphore 和 channel；做“事件先完成/后完成”与取消竞争检查。
3. 多个 `std::thread` worker：加入线程安全队列、状态 CAS、resume gate 和线程亲和。
4. I/O 与阻塞池：让 poller 只发布完成事件，由 worker 恢复 coroutine。
5. 再做 work stealing、帧池、公平性、逻辑栈追踪和压力测量。

Go 运行时还负责可增长用户栈、M/P 调度、网络轮询和抢占等。C++20 `co_xxx` 给 runtime 的是显式挂起点和协程状态帧；如果项目必须兼容“普通同步函数任意深度挂起”，应把 stackful Fiber/context 作为底层，再决定是否在其上提供协程 API。[Go runtime HACKING](https://go.dev/src/runtime/HACKING) [Go runtime FAQ](https://go.dev/doc/faq)
