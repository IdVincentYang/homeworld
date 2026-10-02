# Homeworld task 系统：协作调度与上下文切换

本文按“调度器解决什么问题 → 任务怎样运行 → 调度器怎样安排任务”介绍 `task.c` / `task.h`。核心是任务注册、按时调度和 yield 后恢复；暂停与 freeze all 是配套的生命周期控制功能。源码路径相对仓库根目录。

先记住一句话：**这是运行在主线程上的协作式任务调度器。任务主动调用 `taskYield()` 交回控制权，调度器在以后恢复它；它不是线程池，也不会强行抢占正在运行的任务。**

## 1. task 系统解决什么问题

游戏主循环需要推进许多不同节奏的工作：每轮处理输入和界面、定时更新游戏世界、周期处理网络状态。若把所有工作都塞进一个大循环，职责和频率会纠缠在一起；若每个模块自己写独立循环，又难以统一暂停和驱动。

Homeworld 把这些长期工作的入口注册到同一个调度器：

```text
Win32 消息循环空闲
  → utyTasksDispatch() 计算经过的 ticks
    → taskExecuteAllPending(ticks)
      → 逐个检查任务是否到期
        → 恢复任务，从上次 taskYield() 之后继续
```

驱动入口在 `src/Win32/main.c` 的消息循环和 `src/Win32/utility.c` 的 `utyTasksDispatch()`；调度决策集中在 `src/Game/task.c`。

## 2. 心智模型：不是线程，而是可暂停的函数

一个 task 有两个方面：

1. **调度信息**：何时运行、是否暂停、每次调用间隔。
2. **暂停现场**：任务上次 yield 时的执行位置、寄存器和需要保留的栈内容。

调度器在同一条主线程上依次运行任务。运行中的任务如果不调用 `taskYield()`，调度器就无法切换到下一个任务；因此长时间不 yield 会卡住同批任务和主循环。

```c
void exampleTask(void)
{
    taskYield(0);        // 首次运行时让 taskStart() 完成初始化

  while (!workIsComplete())
    {
        doOnePieceOfWork();
        taskYield(0);    // 保存现场，交回调度器
    }

  taskExit();          // 主动结束任务并交回调度器回收
}
```

这是现有 API 的用法：任务体通常自己写循环，并在适当位置 yield。任务完成后用 `taskExit()` 主动结束；如果由别的模块强制终止，则由调用方调用 `taskStop(handle)`。普通 `return` 不是这类任务的正常结束方式。

## 3. 数据结构：调度器记住什么

### 4.1 任务槽位与句柄

`taskData` 是最多 32 个指针的槽位数组（`TSK_NumberTasks`）；`taskhandle` 实际是槽位索引。空槽为 `NULL`。`taskMaxTask` 是遍历时使用的上界，不代表所有小于它的槽位都在使用。

### 4.2 每个槽位指向 `taskdata`

`taskdata` 定义在 `src/Game/task.h`。初读时可把字段分为两组：

| 字段 | 读法 |
| --- | --- |
| `flags` | 任务控制状态：已分配、暂停、逐帧运行等 |
| `function` | 任务入口函数 |
| `ticks` | 尚未凑够一次运行间隔的剩余 ticks |
| `ticksPerCall` | 定时任务每次运行所需的 ticks |
| `ebx`、`ecx`、`edi`、`esi`、`esp`、`ebp`、`eip` | 任务让出时保存的 x86 执行现场 |
| `nBytesStack` | 需要额外保存的任务栈字节数（启用 `TASK_STACK_SAVE` 时） |

这组寄存器字段是理解实现的关键，但不必一开始背偏移：调度器恢复它们后跳回 `eip`；任务 yield 后又把新的现场写回。该实现依赖 32 位 x86 和编译器/栈帧约定，不是跨平台的通用 C 协程实现。

任务没有独立分配一整块栈。它在调度器使用的栈上运行，系统保存并恢复任务需要跨 yield 保留的栈内容；因此 `taskdata` 除了调度字段，也承担了上下文存储的角色。

### 4.3 调度器的全局状态

- `taskCurrentTask`：当前正在运行的任务句柄；没有正在运行的任务时为 `-1`。
- `taskFrequency`：基础 tick 频率，由 `taskStartup()` 设置。
- `taskTimeDelta` / `taskTimeElapsed`：本次调度推进的秒数，以及累计运行时间。
- `taskNumberCalls`：当前任务本轮需要执行的次数。

这些是调度器共享状态，不是每个任务私有的字段。

## 4. 一个任务从创建到结束

### 第一步：启动调度器

`taskStartup(frequency)` 清空任务槽位并记录基础频率。启动代码在 `src/Win32/utility.c` 调用它。之后平台层才会持续把经过的 ticks 交给调度器。

### 第二步：注册任务

模块调用：

```c
taskhandle handle = taskStart(myTask, period, flags);
```

`taskStart()` 分配一个槽位，设置任务入口和调度间隔，并立即进入任务函数。任务函数通常在开头调用一次 `taskYield(0)`：这会把初始执行现场交回 `taskStart()`，供它保存起来。因此首次 yield 之前的代码会在 `taskStart()` 调用期间同步执行，不应放耗时工作。

### 第三步：调度并恢复

每次 `taskExecuteAllPending(ticks)` 遍历任务槽位，跳过空槽和暂停任务，计算每个任务本轮要运行几次。若任务到期，调度器恢复现场并跳回上次 yield 的位置。

### 第四步：主动让出或退出

- `taskYield(0)`：保存任务现场，回到调度器；以后从 yield 之后继续。
- `taskExit()`：结束任务，调度器走退出路径并释放其槽位。
- `taskPause(handle)` / `taskResume(handle)`：让调度器暂时跳过或重新调度该任务。
- `taskStop(handle)`：直接释放任务；调用方应确保不会在该任务正在执行时不安全地销毁它。

示例任务可对照 `src/Game/region.c` 的 `regProcessTask()` 或 `src/Game/ping.c` 的 `pingUpdateTask()`：它们先 yield，随后反复工作并 yield。

## 5. 两种调度节奏

### 定时任务：按经过的时间运行

不带 `TF_OncePerFrame` 时，`period` 表示秒数。`taskStart()` 用 `period * taskFrequency` 算出 `ticksPerCall`。调度时把本轮 ticks 与上次剩余 ticks 相加，整除得到调用次数，余数留到下轮。

例如基础频率约为 240 ticks/秒，周期设为 `1.0f / 16.0f`，一次调用约需要 15 ticks，即约 16 次/秒。单次调度中，定时任务的调用次数最多限制为 `TSK_MaxTicks`（8），以限制单轮补跑量。

项目例子：`universeUpdateTask` 使用定时周期；`pingUpdateTask` 也按定时周期工作。

### 逐帧任务：每次调度运行一次

带 `TF_OncePerFrame` 时，调度器每次执行 `taskExecuteAllPending()` 都把本任务安排运行一次，不按 `period` 累计 ticks。这里的“帧”更准确说是**一次调度器执行**，受 Win32 消息循环影响，不保证等于显示器的一次垂直同步。

项目例子：`rndRenderTask` 和 `regProcessTask` 使用该标志。

| 对比 | 定时任务 | `TF_OncePerFrame` |
| --- | --- | --- |
| 依据 | 累积 ticks 达到周期 | 调度器每运行一轮 |
| `period` 的作用 | 决定 `ticksPerCall` | 不决定隔几帧运行 |
| 适合 | 固定时间间隔的更新、服务轮询 | 渲染和逐轮 UI 处理 |

## 6. 暂停与 freeze：调度器的管理能力

`taskPause()` / `taskResume()` 管理单个任务；`taskFreezeAll()` / `taskResumeAll()` 批量管理所有任务。这些 API 不负责安排日常工作，它们只改变调度器是否跳过任务。

本项目使用 freeze all 的实际场景是**窗口失去激活状态**，例如玩家 Alt-Tab。在 `src/Win32/main.c` 的 `DeactivateMe()` 中，单机且允许 Alt-Tab 暂停时会冻结全部任务，返回游戏后由 `ActivateMe()` 恢复。

联机游戏或配置为 Alt-Tab 时不暂停时，程序不冻结整个任务集：它保存当前暂停状态，只暂停渲染和 region 任务，让其他后台工作继续。冻结前保存暂停状态，是为了恢复时不意外启动原本就暂停的任务。

冻结不会销毁任务，也不会停止全局计时；任务的执行现场仍保留。`taskResumeAll()` 的源码注释特别要求调用方避免恢复后的首次调度补跑暂停期间积累的 ticks。失活路径会调用 `utyTaskTimerClear()`，但是否有 ticks 积压仍取决于窗口失活期间调度器是否持续派发。

## 7. 易踩的边界

- **忘记 yield：**任务占着主线程不放，调度器不能抢占它。把大工作拆成有界的小步骤，并定期 yield。
- **误以为 task 是并行线程：**任务之间不会并行；共享数据没有因此自动获得线程同步保护。
- **误解首次运行：**`taskStart()` 会同步运行任务体直到第一次 yield。
- **误解逐帧：**`TF_OncePerFrame` 是每次调度器调用一次，不是独立的显示器刷新回调。
- **恢复时补算时间：**暂停任务不等于暂停时钟。`taskResumeAll()` 的调用方必须考虑暂停区间是否应计入后续任务时间。
- **把平台实现当成可移植协程：**保存 x86 寄存器和栈现场的汇编实现限制了架构和编译器适用范围。

## 8. 建议的源码阅读顺序

1. `src/Game/task.h`：先看 `taskdata`、标志位和公开 API。
2. `src/Game/task.c` 的 `taskStartup()`、`taskStart()`：看调度器初始化和任务首次运行。
3. `src/Game/task.c` 的 `taskExecuteAllPending()`：看 ticks 如何决定调用次数，以及现场如何恢复。
4. `src/Game/region.c` 的 `regProcessTask()`：用一个真实任务体串起 yield、工作和下一轮恢复。
5. `src/Win32/utility.c` 的 `utyTasksDispatch()`：看平台如何测量 ticks 并驱动调度器。
6. `src/Win32/main.c` 的 `DeactivateMe()` / `ActivateMe()`：最后再看暂停和 freeze 的应用场景。

读到 `taskYield()` 后，最值得继续追的细节是：任务第一次 yield 时如何建立初始上下文，以及局部栈内容如何保存在 `taskdata` 后面。先理解生命周期和调度节奏，再读那些汇编偏移会轻松很多。
