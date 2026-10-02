// Standalone C++20 design prototype for the Homeworld task scheduler.
// This file is documentation code; it is not wired into the legacy C build.

#include <algorithm>
#include <array>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <utility>

namespace homeworld::task_proto {

constexpr std::size_t kMaxTasks = 32;
constexpr std::uint32_t kMaxCallsPerDispatch = 8;

enum TaskFlags : std::uint32_t {
    OncePerFrame = 1u << 0,
};

class TaskRoutine {
public:
    struct promise_type;
    using handle_type = std::coroutine_handle<promise_type>;

    TaskRoutine() noexcept = default;
    explicit TaskRoutine(handle_type handle) noexcept : handle_(handle) {}

    TaskRoutine(const TaskRoutine&) = delete;
    TaskRoutine& operator=(const TaskRoutine&) = delete;

    TaskRoutine(TaskRoutine&& other) noexcept
        : handle_(std::exchange(other.handle_, handle_type{})) {}

    TaskRoutine& operator=(TaskRoutine&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, handle_type{});
        }
        return *this;
    }

    ~TaskRoutine() { reset(); }

    bool valid() const noexcept { return static_cast<bool>(handle_); }
    bool done() const noexcept { return !handle_ || handle_.done(); }

    void resume() const { handle_.resume(); }
    promise_type& promise() const { return handle_.promise(); }

    void reset() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }

    struct promise_type {
        bool reachedSchedulerYield = false;

        TaskRoutine get_return_object() noexcept {
            return TaskRoutine{handle_type::from_promise(*this)};
        }

        // Match taskStart(): enter the body now and run to its first task yield.
        std::suspend_never initial_suspend() noexcept { return {}; }

        // Keep the frame alive until the scheduler observes completion and destroys it.
        std::suspend_always final_suspend() noexcept { return {}; }

        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };

private:
    handle_type handle_{};
};

struct SchedulerYield {
    bool await_ready() const noexcept { return false; }

    void await_suspend(TaskRoutine::handle_type handle) const noexcept {
        handle.promise().reachedSchedulerYield = true;
    }

    void await_resume() const noexcept {}
};

class Scheduler;

struct TaskContext {
    Scheduler* scheduler = nullptr;
    int handle = -1;

    SchedulerYield yield() const noexcept { return {}; }
    bool pause(int target) const noexcept;
    bool resume(int target) const noexcept;
};

using TaskEntry = TaskRoutine (*)(TaskContext&);

struct TaskControlBlock {
    bool allocated = false;
    bool paused = false;
    bool pauseBeforeFreeze = false;
    bool oncePerFrame = false;
    bool running = false;
    std::uint32_t ticksPerCall = 1;
    std::uint32_t tickRemainder = 0;
    TaskEntry entry = nullptr;
    TaskContext context{};
    std::optional<TaskRoutine> routine;
};

class Scheduler {
public:
    // periodTicks is the legacy period converted to base ticks:
    // periodTicks = periodSeconds * taskFrequency.
    int start(TaskEntry entry, std::uint32_t periodTicks, std::uint32_t flags = 0) {
        if (entry == nullptr || ((flags & OncePerFrame) == 0 && periodTicks == 0)) {
            return -1;
        }

        for (std::size_t i = 0; i < slots_.size(); ++i) {
            TaskControlBlock& slot = slots_[i];
            if (slot.allocated) {
                continue;
            }

            slot.allocated = true;
            slot.paused = false;
            slot.pauseBeforeFreeze = false;
            slot.oncePerFrame = (flags & OncePerFrame) != 0;
            slot.running = true;
            slot.ticksPerCall = std::max<std::uint32_t>(periodTicks, 1);
            slot.tickRemainder = 0;
            slot.entry = entry;
            slot.context.scheduler = this;
            slot.context.handle = static_cast<int>(i);

            // initial_suspend() is suspend_never, so this call runs synchronously
            // until entry first awaits context.yield(), like legacy taskStart().
            slot.routine.emplace(entry(slot.context));
            slot.running = false;

            if (!slot.routine->valid() || slot.routine->done() ||
                !slot.routine->promise().reachedSchedulerYield) {
                stop(static_cast<int>(i));
                return -1;
            }
            return static_cast<int>(i);
        }
        return -1;
    }

    void dispatch(std::uint32_t elapsedTicks) {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            TaskControlBlock& slot = slots_[i];
            if (!slot.allocated || slot.paused) {
                continue; // paused time is not accumulated, matching task.c
            }

            std::uint32_t calls = 0;
            if (slot.oncePerFrame) {
                calls = 1;
            } else {
                const std::uint64_t total =
                    static_cast<std::uint64_t>(elapsedTicks) + slot.tickRemainder;
                slot.tickRemainder = static_cast<std::uint32_t>(total % slot.ticksPerCall);
                calls = static_cast<std::uint32_t>(total / slot.ticksPerCall);
                calls = std::min(calls, kMaxCallsPerDispatch);
            }

            // Each resume runs from the previous co_await ctx.yield() to the next
            // one. This is the C++ equivalent of one taskNumberCalls iteration.
            for (std::uint32_t call = 0; call < calls; ++call) {
                if (!slot.allocated || !slot.routine) {
                    break;
                }

                TaskRoutine& routine = *slot.routine;
                routine.promise().reachedSchedulerYield = false;
                slot.running = true;
                routine.resume();
                slot.running = false;

                if (routine.done()) {
                    stop(static_cast<int>(i));
                    break;
                }

                // This prototype supports scheduler-yield suspension only.
                // An I/O/timer awaiter needs a separate Waiting state and wake path.
                if (!routine.promise().reachedSchedulerYield) {
                    stop(static_cast<int>(i));
                    break;
                }
            }
        }
    }

    bool pause(int handle) {
        TaskControlBlock* slot = find(handle);
        if (slot == nullptr) {
            return false;
        }
        slot->paused = true;
        return true;
    }

    bool resume(int handle) {
        TaskControlBlock* slot = find(handle);
        if (slot == nullptr) {
            return false;
        }
        slot->paused = false;
        return true;
    }

    void freezeAll() {
        for (TaskControlBlock& slot : slots_) {
            if (!slot.allocated) {
                continue;
            }
            slot.pauseBeforeFreeze = slot.paused;
            slot.paused = true;
        }
    }

    void resumeAll() {
        for (TaskControlBlock& slot : slots_) {
            if (slot.allocated) {
                slot.paused = slot.pauseBeforeFreeze;
            }
        }
    }

    bool stop(int handle) {
        TaskControlBlock* slot = find(handle);
        if (slot == nullptr || slot->running) {
            return false;
        }
        slot->routine.reset();
        slot->allocated = false;
        slot->paused = false;
        slot->pauseBeforeFreeze = false;
        slot->entry = nullptr;
        slot->context.scheduler = nullptr;
        slot->context.handle = -1;
        slot->tickRemainder = 0;
        return true;
    }

private:
    TaskControlBlock* find(int handle) noexcept {
        if (handle < 0 || static_cast<std::size_t>(handle) >= slots_.size()) {
            return nullptr;
        }
        TaskControlBlock& slot = slots_[static_cast<std::size_t>(handle)];
        return slot.allocated ? &slot : nullptr;
    }

    std::array<TaskControlBlock, kMaxTasks> slots_{};
};

inline bool TaskContext::pause(int target) const noexcept {
    return scheduler != nullptr && scheduler->pause(target);
}

inline bool TaskContext::resume(int target) const noexcept {
    return scheduler != nullptr && scheduler->resume(target);
}

// Example shape. Local variables such as i live in the coroutine frame across yield.
TaskRoutine exampleTask(TaskContext& ctx) {
    co_await ctx.yield(); // taskStart returns after reaching this first yield

    int i = 0;
    while (i < 10) {
        ++i;               // one logical legacy task invocation
        co_await ctx.yield();
    }

    co_return;             // equivalent to taskExit() for this routine
}

} // namespace homeworld::task_proto
