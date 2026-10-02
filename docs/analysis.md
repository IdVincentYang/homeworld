# Homeworld 1 源码架构地图

> 本文是源码阅读的总入口：先建立模块边界和关键数据模型，再沿启动、关卡加载、模拟、绘制几条主链路阅读。
> 范围覆盖本仓库中的游戏客户端源码、脚本运行时、资源管线、渲染库与工具。仓库依赖的发行版资源和部分第三方库不在此范围内。
> 文中把源码或随仓库文档可直接核验的内容称为“实现事实”；“设计观察”是基于实现作出的学习总结，不等同于作者的原始设计意图。
> 建议搭配[源码学习路线](learning_guide.md)和[文档索引](README.md)使用。

## 1. 功能目标

这张架构地图希望回答四个问题：

1. 程序从哪里进入，逻辑更新和画面绘制如何被驱动？
2. 一关的布局、舰船定义和资源文件如何变成运行中的对象？
3. 关卡脚本、常规 AI、对象行为和渲染分别负责什么？
4. 从 Homeworld 1 的实现中，哪些设计值得学习，哪些约束来自当时的平台和工具？

**项目定位（实现事实）**：这是以 C 为主的 Win32 实时策略游戏源码。核心代码集中在 `src/Game/`；平台适配在 `src/Win32/`；软件、Direct3D 和 3dfx 图形后端在 `src/rgl/`。单人任务脚本、AI、舰船专属回调和自研资源格式也一并包含在仓库中。

**阅读边界**：源码快照不是一个现代、平台无关的引擎 SDK。它依赖 Win32/DirectX 与历史构建工具，且部分美术、音频和发布资源不随源码完整提供。`BuildingHomeworld.doc` 主要说明构建环境和发布流程；格式与子系统设计资料分散在 `documents/`。

## 2. 整体分层

| 层 | 主要职责 | 代码入口 |
| :-- | :-- | :-- |
| Win32 宿主 | 窗口、消息泵、输入和平台服务 | [main.c](../src/Win32/main.c)、`src/Win32/` |
| 任务调度 | 在空闲循环中按 tick 驱动协作式任务 | [task.c](../src/Game/task.c)、[utility.c](../src/Win32/utility.c) |
| 世界与模拟 | Universe、空间对象、关卡布局、更新与碰撞 | [universe.c](../src/Game/universe.c)、[univupdate.c](../src/Game/univupdate.c)、[levelload.c](../src/Game/levelload.c) |
| 行为 | KAS 任务脚本、AI 玩家与舰队/资源/防御管理器 | [KAS.c](../src/Game/KAS.c)、[AIPlayer.c](../src/Game/AIPlayer.c)、`src/Game/AI*Man.c` |
| 内容与资源 | 统一文件访问、BIG 归档、格式解析和静态数据绑定 | [file.c](../src/Game/file.c)、[bigfile.c](../src/Game/bigfile.c)、[statscript.c](../src/Game/statscript.c) |
| 前端 UI | Region 树、控件、屏幕流和绘制回调 | [region.c](../src/Game/region.c)、[FeFlow.c](../src/Game/FeFlow.c)、[UIcontrols.c](../src/Game/UIcontrols.c) |
| 场景渲染 | 可见对象遍历、LOD、网格提交 | [render.c](../src/Win32/render.c)、[LOD.c](../src/Game/LOD.c)、[mesh.c](../src/Game/mesh.c) |
| 图形后端 | GL 风格子集接口及软件、D3D、3dfx 实现 | [rglext.h](../src/rgl/rglext.h)、`src/rgl/swdriver.c`、`src/rgl/d3driver.cpp`、`src/rgl/fxdriver.c` |
| 内容构建工具 | KAS 编译器、资源导出/打包及格式资料 | `tools/`、`documents/`、`src/SinglePlayer/` |

### 2.1 核心概念对齐

| 概念 | 它代表什么 | 主要关联 |
| :-- | :-- | :-- |
| Task | 可让出执行权、由调度器按频率恢复的工作单元 | `taskStart`、`taskYield`、`taskExecuteAllPending` |
| Universe | 当前战局的对象集合、静态信息和空间组织 | `universe`、MissionSphere、更新/渲染列表 |
| SpaceObj 与 StaticInfo | 动态对象实例与同类型共享定义分离 | `Ship` 等实例引用 `ShipStaticInfo`、LOD 和网格数据 |
| KAS Mission/FSM/State | 单人任务脚本的任务、团队状态机和具体状态 | 生成的脚本函数由 `KAS.c` 运行时推进 |
| AIPlayer/AITeam | 常规电脑玩家策略与具体舰队团队执行 | AI 管理器下发目标；团队执行 move |
| Region/FeFlow | UI 布局树、事件处理和屏幕流程 | `.fib` 布局、控件回调、Region 绘制队列 |
| rgl | 游戏绘制代码与图形设备后端之间的接口层 | GL 风格函数表、软件/D3D/3dfx 后端 |
| file/BIG | 逻辑文件名到磁盘散文件或 BIG 归档的统一访问 | 格式解析器再解释文件内容 |

可按[对象模型说明](modules/spaceobj_overview.md)、[任务调度说明](modules/task_scheduler_overview.md)、[KAS 说明](modules/kas_script_overview.md)和[UI 系统说明](modules/ui_system_overview.md)逐个展开。

### 2.2 系统架构与数据流

~~~mermaid
flowchart TD
    WIN["Win32 消息泵<br/>main.c"] --> DISPATCH["utyTasksDispatch<br/>taskExecuteAllPending"]
    DISPATCH --> SIM["Universe 更新任务"]
    DISPATCH --> REGION["Region 事件处理任务"]
    DISPATCH --> DRAW["渲染任务"]

    LEVEL[".level / .missphere"] --> LOAD["levelload.c"]
    LOAD --> WORLD["Universe 与对象实例"]
    FILE["磁盘文件 / .big"] --> PARSE["格式解析与 statscript 绑定"]
    PARSE --> STATIC["StaticInfo / 网格 / LOD"]
    STATIC --> WORLD

    SIM --> BEHAVIOR["KAS 与 AI"]
    BEHAVIOR --> WORLD
    WORLD --> RLIST["Universe 渲染列表"]
    REGION --> QUEUE["UI 与场景绘制回调队列"]
    DRAW --> QUEUE
    RLIST --> VIEW["render.c 主视口"]
    VIEW --> LOD["LOD 选择"]
    LOD --> MESH["mesh.c"]
    MESH --> RGL["rgl 接口"]
    RGL --> BACKEND["软件 / D3D / 3dfx"]
    UI["FeFlow / UIcontrols"] --> REGION

    KASFILE[".kas"] --> KAS2C["kas2c"]
    KAS2C --> GENERATED["生成的 C 函数"]
    GENERATED --> BEHAVIOR
~~~

图中有两种不同路径：

- **运行时路径**：平台循环驱动调度器；Universe 更新、Region 处理和绘制任务各自运行，再通过对象状态、渲染列表和回调队列衔接。
- **内容构建路径**：KAS 源文件先由 `kas2c` 转为 C；关卡、模型、纹理等内容由相应工具和解析器处理。`file.c` 统一文件来源，但不取代各格式解析器。

## 3. 运行时数据模型

理解对象与数据的所有权，是阅读后续流程的关键。

### 3.1 静态定义与动态实例

`spaceobj.h` 用一组具有共同前缀布局的 C 结构体表达空间对象类型。运行中的 `Ship`、`Missile`、`Bullet`、`Effect`、`Resource` 等保存位置、朝向、健康值和当前状态；舰船种类对应的 `ShipStaticInfo` 则保存可共享的定义数据，例如碰撞、网格/LOD、炮塔、停靠点和舰船回调。

这不是 C++ 虚函数式继承，而是靠结构体布局、显式类型/标志和函数表协作。`src/Ships/` 中按舰船类型实现的 `CustShipHeader` 回调，为通用对象生命周期补充舰船特化行为。

| 数据 | 生命周期与用途 | 入口 |
| :-- | :-- | :-- |
| `StaticHeader` / `ShipStaticInfo` | 按对象类型共享，提供资源和静态规则 | [spaceobj.h](../src/Game/spaceobj.h)、[statscript.c](../src/Game/statscript.c) |
| `SpaceObj` 派生实例 | 随战局创建、更新、绘制和销毁 | [universe.c](../src/Game/universe.c)、[univupdate.c](../src/Game/univupdate.c) |
| Universe 对象列表 | 管理当前战局对象及不同更新/绘制集合 | [universe.c](../src/Game/universe.c)、[univupdate.c](../src/Game/univupdate.c) |
| AI 团队与 KAS 状态 | 描述由谁控制团队，以及下一步行为 | [AITeam.c](../src/Game/AITeam.c)、[KAS.c](../src/Game/KAS.c) |

进一步阅读：[SpaceObj 对象模型](modules/spaceobj_overview.md)。

### 3.2 行为所有权

KAS 和 AI 不应被理解为两套互不相干的“舰船行为系统”。KAS 描述战役节奏、条件和团队状态；常规 AI 管理器负责资源、攻击、防御等策略；AITeam 和 move 逻辑落实具体团队命令。脚本可以创建 FSM 控制团队，也能通过 `TeamGiveToAI` / `kasfTeamGiveToAI` 把团队交回常规 AI。

这条控制权边界是重要阅读线索：跟踪一条脚本命令时，要从 KAS 宿主函数继续追到 AI 团队的实际执行处，而不要在脚本调用点就认为命令已经完成。

## 4. 内容模型与资源管线

### 4.1 从关卡布局到战局对象

关卡布局描述 MissionSphere、舰船、资源、光照和背景等内容；关卡加载代码解析布局并创建对象。需要的舰船静态信息再按需加载，配置字段由 `statscript` 写入相应静态结构。

| 阶段 | 工作 | 主要入口 |
| :-- | :-- | :-- |
| 关卡布局 | 读取 `.level`、`.missphere` 及相关分布文件 | [levelload.c](../src/Game/levelload.c) |
| 对象创建 | 按布局创建 Ship、Asteroid 等战局实例 | [universe.c](../src/Game/universe.c) |
| 文件定位 | 在磁盘散文件和 `.big` 归档中读取数据 | [file.c](../src/Game/file.c)、[bigfile.c](../src/Game/bigfile.c) |
| 格式解析 | 将 `.geo`、`.lif`、`.shp` 等字节解码为引擎数据 | [mesh.c](../src/Game/mesh.c)、[bmp.c](../src/Game/bmp.c)、[statscript.c](../src/Game/statscript.c) |
| 静态绑定 | 把文本字段填入对应类型的静态信息结构 | [statscript.c](../src/Game/statscript.c) |
| 运行期使用 | 对象实例引用静态数据，供 AI、模拟和绘制使用 | [spaceobj.h](../src/Game/spaceobj.h)、[render.c](../src/Win32/render.c) |

### 4.2 统一文件访问与具体格式

`file.c` / `bigfile.c` 解决的是“从哪里读”和“如何表现为文件流”的问题；`.geo`、`.lif`、`.etg`、`.btg` 等解析器解决的是“读出的字节代表什么”。把这两个边界分开看，可以避免把 BIG 归档误认为一个通用资源对象系统。

- **文件来源**：逻辑文件访问可落到磁盘文件或 `.big`；开发时散文件可覆盖归档中的同名内容。`FF_IgnoreBIG` 可以跳过归档；`FF_IgnoreDisk` 虽在头文件中定义，但本快照中没有使用点。
- **内容解释**：模型、图像、效果、背景和舰船参数分别由对应解析代码或 statscript 读取。
- **静态绑定**：statscript 通过字段名、回调和结构体地址/偏移完成配置写入；配置表与目标结构体间存在显式约定。

格式资料位于 `documents/FormatsReleasedToPublic/`，关卡格式说明位于 `documents/MissionMan/`。资源访问细节见[资源加载说明](modules/resource_loading_overview.md)和[statscript 说明](modules/statscript_overview.md)。

## 5. 从启动到画面的主流程

### 5.1 程序入口与任务分发

Win32 消息泵位于 `src/Win32/main.c`。当 Windows 没有待处理消息时，程序进入 `utyTasksDispatch`，测量经过的 tick 并调用 `taskExecuteAllPending`。调度器按任务配置恢复执行相应工作。

任务调度是协作式的：任务通过 `taskYield` 让出执行权；这不是抢占式线程池。Universe 更新以独立的定频任务推进（源码配置约为 16 Hz），Region 处理和渲染任务则按帧工作。不要把模拟频率与显示帧率视为同一个时钟。

相关实现：[Win32 入口](../src/Win32/main.c)、[调度器](../src/Game/task.c)、[utility 任务启动与派发](../src/Win32/utility.c)。

### 5.2 关卡与静态资源加载

~~~mermaid
flowchart LR
    LAYOUT[".level / .missphere"] --> LEVELLOAD["levelInit / levelload.c"]
    LEVELLOAD --> NEED["标记本关所需的静态类型"]
    NEED --> IO["fileOpen / fileLoadAlloc"]
    IO --> SOURCE["磁盘散文件或 .big"]
    SOURCE --> PARSER["模型、图像、文本等格式解析器"]
    PARSER --> STATS["statscript 字段绑定"]
    STATS --> STATIC["StaticInfo、网格、LOD 等静态数据"]
    LEVELLOAD --> INSTANCE["创建 MissionSphere 与对象实例"]
    STATIC --> INSTANCE
    INSTANCE --> WORLD["Universe 中的运行时对象"]
~~~

加载设计的关键不是某个单独格式，而是把**布局解析、文件定位、格式解析、静态数据绑定和对象实例化**拆成连续的职责。按需标记静态类型使关卡可以避免加载不相关的舰船数据；文件抽象则让内容能够以散文件或归档形式部署。

### 5.3 模拟、行为与绘制

~~~mermaid
flowchart TD
    TICK["Win32 空闲循环 / tick"] --> SCHED["taskExecuteAllPending"]
    SCHED --> UPDATE["universeUpdateTask"]
    UPDATE --> KAS["KAS Mission / FSM / State"]
    UPDATE --> AI["AIPlayer 与管理器"]
    KAS --> TEAM["AITeam 命令与状态"]
    AI --> TEAM
    TEAM --> OBJECTS["SpaceObj 状态更新"]
    OBJECTS --> RLIST["univUpdateRenderList"]

    SCHED --> REG["regProcessTask<br/>输入、Region 事件与绘制队列"]
    SCHED --> RENDER["rndRenderTask"]
    REG --> QUEUE["regFunctionsDraw"]
    RENDER --> QUEUE
    QUEUE --> VIEW["render.c 主游戏视口"]
    VIEW --> RLIST
    VIEW --> LOD["lodLevelGet"]
    LOD --> MESH["meshRender / rglMeshRender"]
    MESH --> BACKEND["软件 / D3D / 3dfx"]
~~~

实现上，`regProcessTask` 负责处理 Region 事件并准备绘制回调；`rndRenderTask` 通过 `regFunctionsDraw` 消费回调。主游戏视口在 `src/Win32/render.c` 遍历 Universe 的渲染列表，按对象与相机状态选择 LOD，再交给 mesh/rgl。Sensors Manager 在 `sensors.c` 有独立视图路径，但会复用底层对象、LOD 和绘制能力。

## 6. 核心运行时子系统

### 6.1 协作式任务调度

`task.c` 集中管理任务登记、到期计算、上下文切换和让出执行。任务上下文结构直接保存 x86 寄存器与栈信息，是这份旧式 Win32 源码中很有代表性的实现：它提供轻量协程式工作流，但对处理器、编译器和调用约定有强依赖。

阅读入口：[task.c](../src/Game/task.c)、[task.h](../src/Game/task.h)；扩展说明：[任务调度器](modules/task_scheduler_overview.md)。

### 6.2 Universe 与空间对象

Universe 负责战局范围内的对象和静态信息；各对象类型使用公共结构体前缀和显式回调形成多态行为。游戏逻辑代码可按对象类型组织在 `src/Ships/`，而对象的公共生命周期与空间更新仍由 `src/Game/` 中的通用代码驱动。

值得关注的连接点包括 `univAddShip` 一类创建接口、`CustShipHeader` 回调，以及静态信息加载后由实例引用的方式。它们展示了在没有现代组件框架时，如何组合通用生命周期与舰船特化逻辑。

### 6.3 KAS 与 AI

KAS 的层级是 Mission、FSM、State。任务脚本由 `tools/win32/KAS/` 中的 flex/bison 编译器 `kas2c` 转换为 C；运行时 `KAS.c` 调用相应 init/watch 函数推进脚本。脚本通过 `KASFunc.c` 提供的宿主函数访问游戏对象和 AI 能力。

AI 能力以特征位组织，AIPlayer 持有策略管理器，AITeam 承接具体团队命令。这里体现了“任务编排”与“通用执行策略”分离的好处：剧本可以控制关键节奏，同时仍能复用常规 AI 行为。

阅读入口：[KAS 运行时](../src/Game/KAS.c)、[KAS 宿主函数](../src/Game/KASFunc.c)、[AI 特征位](../src/Game/AIFeatures.h)、[AI 玩家](../src/Game/AIPlayer.c)、[KAS 专题](modules/kas_script_overview.md)、[AI 专题](modules/ai_features_overview.md)。

### 6.4 场景渲染与 LOD

渲染链分为游戏对象选择、模型层和图形设备接口：Universe 维护绘制列表；`src/Win32/render.c` 处理主视口；`LOD.c` 选择网格、微型精灵、子像素、函数或不绘制等层级；`mesh.c` 组织网格和材质提交；`rgl` 后端完成实际光栅化。

这种分层让游戏侧有机会按距离减少绘制工作，并让多个后端共享一套上层绘制调用。不过 rgl 只是图形接口抽象，资源格式、状态和历史后端差异仍会渗透到调用端。

阅读入口：[渲染管线专题](modules/render_pipeline_overview.md)、[LOD 专题](modules/lod_overview.md)、[render.c](../src/Win32/render.c)、[mesh.c](../src/Game/mesh.c)、[rgl 接口](../src/rgl/rglext.h)。

### 6.5 前端 UI

UI 采用 Region 树与回调驱动的运行时：布局/屏幕流、控件行为、事件处理和实际绘制相互衔接；`regProcessTask` 处理事件并构建绘制工作，绘制任务随后执行回调。`.fib` 等布局资源与 `FeFlow`、`UIcontrols`、Region 和字体/注册表代码一起构成前端。

读 UI 时可先从一个屏幕进入，沿屏幕流找到布局文件，再追控件创建、事件回调、业务动作和绘制回调。总览不在这里重复每种控件的细节，详见[UI 系统设计](modules/ui_system_overview.md)。

## 7. 脚本与内容构建链

脚本和资源都不是运行时代码凭空生成的；它们有各自的源格式、转换工具和加载入口。

| 输入 | 构建/加载环节 | 运行时落点 |
| :-- | :-- | :-- |
| `src/SinglePlayer/*.kas` | `tools/win32/KAS/` 的 `kas2c` 编译成 C | `KAS.c` 执行生成的 Mission/FSM/State 函数 |
| `.level` / `.missphere` | `levelload.c` 解析布局 | Universe 创建对象与 MissionSphere |
| `.geo` / `.lif` 等 | 对应格式解析器读取 | 网格、纹理等静态数据被渲染使用 |
| `.shp` 等文本配置 | `statscript.c` 字段/回调绑定 | 舰船或其他类型的静态信息结构 |
| 散文件 / `.big` | `file.c`、`bigfile.c` 统一 IO | 各解析器获得数据流或整块数据 |

工具源码与格式文档也属于学习项目架构的一部分。看一项功能时，沿着“内容作者输入 -> 工具转换 -> 文件部署 -> 运行时解析 -> 游戏系统使用”走完整条链，通常比只读运行时代码更容易理解约定从何而来。

## 8. 平台与图形后端边界

| 边界 | 实现 | 学习时的判断 |
| :-- | :-- | :-- |
| 操作系统 | `src/Win32/` | 主消息循环、输入和平台服务绑定 Win32；不能把游戏逻辑可读误当成跨平台。 |
| 调度上下文 | `src/Game/task.c` | 保存 x86 寄存器/栈的实现依赖旧平台与编译器约定。 |
| 图形设备 | `src/rgl/` | 软件、D3D、3dfx 后端共享 GL 风格接口，但上层与后端并非完全无差异。 |
| 第三方能力 | `src/Titan/`、`src/Sigs/`、预编译库 | 网络、音视频等部分能力依赖外部库或服务，源码快照不保证单独即可重建完整发行体验。 |
| 构建环境 | `BuildingHomeworld.doc`、`tools/` | 历史编译器、批处理与资源工具是工程系统的一部分，不只是代码之外的附属物。 |

## 9. 值得学习的设计

| 设计做法 | 值得学习的地方 | 代价或限制 |
| :-- | :-- | :-- |
| 将模拟更新与逐帧工作放进同一调度框架 | 任务频率统一管理，逻辑更新不必跟显示帧率绑定 | 协作式任务必须主动让出；调度实现依赖平台细节 |
| 静态定义与运行实例分离 | 同类型对象共享配置和资源，实例保留动态状态 | 结构体前缀继承和裸指针使类型/所有权约束不够显式 |
| 统一文件访问、独立格式解析 | 部署位置与文件格式相对解耦，开发期可使用散文件覆盖 | BIG、路径前缀和覆盖顺序是隐式约定，诊断能力受限 |
| 数据表、绑定回调和类型专属函数表 | 可扩展舰船差异与文本配置，避免所有逻辑挤进一个巨大分支 | 名称、表项、结构体布局和回调签名必须保持同步 |
| KAS 编排与 AI 团队执行分工 | 关卡脚本描述目标和节奏，AI 复用团队级执行逻辑 | 控制权跨脚本/AI 切换，调试时需追完整条调用链 |
| 距离驱动的 LOD 与多图形后端 | 在大量空间对象场景下控制绘制成本，并适配不同设备路线 | LOD 效果依赖内容配置；后端抽象没有抹平所有兼容性差异 |
| Region、布局流与控件逻辑分层 | UI 事件、屏幕导航和绘制职责可分别追踪 | 布局资源与控件/回调名称之间存在运行时约定，布局适配能力有限 |

这套代码最值得带走的不是某个孤立技巧，而是**围绕生命周期建立边界**：内容先变成静态定义，再产生运行实例；任务脚本决定目标，AI 执行团队动作；对象先进入渲染集合，再经 LOD、网格层和后端提交。阅读时沿这些边界追踪，能看清系统为什么如此组织。

## 10. 当前实现约束与阅读注意

- **单平台假设**：主程序与部分基础设施绑定 Win32、x86 和历史编译工具。
- **全局状态较多**：Universe、渲染、输入和子系统共享全局状态，跨模块追数据所有权时要沿初始化与销毁路径核对。
- **隐式布局约定**：结构体公共前缀、配置字段名、回调表和资源命名共同构成接口；单看某个头文件不一定能看出完整契约。
- **协作式运行**：任务只在显式让出点切换，长时间不 yield 的代码会影响其他任务推进。
- **内容不完整**：源码仓库不包含全部商业资源；即使源码可读，也不代表可仅凭此仓库复现原版内容与运行环境。
- **资料类型需区分**：`BuildingHomeworld.doc` 是构建/发布指南；关卡、几何、纹理和 KAS 的规范分散于 `documents/` 中。

## 11. 核心源码索引

### 11.1 程序与运行时

| 主题 | 从这里开始 |
| :-- | :-- |
| Win32 程序入口 | [src/Win32/main.c](../src/Win32/main.c)、[src/Win32/utility.c](../src/Win32/utility.c) |
| 任务调度 | [src/Game/task.c](../src/Game/task.c)、[任务调度专题](modules/task_scheduler_overview.md) |
| Universe 更新与对象集合 | [src/Game/universe.c](../src/Game/universe.c)、[src/Game/univupdate.c](../src/Game/univupdate.c) |
| 空间对象模型 | [src/Game/spaceobj.h](../src/Game/spaceobj.h)、[对象模型专题](modules/spaceobj_overview.md) |
| 关卡加载 | [src/Game/levelload.c](../src/Game/levelload.c) |

### 11.2 行为、内容与 UI

| 主题 | 从这里开始 |
| :-- | :-- |
| KAS 运行时与宿主函数 | [src/Game/KAS.c](../src/Game/KAS.c)、[src/Game/KASFunc.c](../src/Game/KASFunc.c)、[KAS 专题](modules/kas_script_overview.md) |
| AI 能力与策略 | [src/Game/AIFeatures.h](../src/Game/AIFeatures.h)、[src/Game/AIPlayer.c](../src/Game/AIPlayer.c)、[AI 专题](modules/ai_features_overview.md) |
| 文件与 BIG | [src/Game/file.c](../src/Game/file.c)、[src/Game/bigfile.c](../src/Game/bigfile.c)、[资源加载专题](modules/resource_loading_overview.md) |
| 静态配置绑定 | [src/Game/statscript.c](../src/Game/statscript.c)、[statscript 专题](modules/statscript_overview.md) |
| UI 前端 | [src/Game/region.c](../src/Game/region.c)、[src/Game/FeFlow.c](../src/Game/FeFlow.c)、[UI 专题](modules/ui_system_overview.md) |

### 11.3 渲染与工具

| 主题 | 从这里开始 |
| :-- | :-- |
| 主场景绘制 | [src/Win32/render.c](../src/Win32/render.c)、[渲染管线专题](modules/render_pipeline_overview.md) |
| LOD 选择 | [src/Game/LOD.c](../src/Game/LOD.c)、[LOD 专题](modules/lod_overview.md) |
| 网格与图形接口 | [src/Game/mesh.c](../src/Game/mesh.c)、[src/rgl/rglext.h](../src/rgl/rglext.h) |
| KAS 编译器 | `tools/win32/KAS/`、`src/SinglePlayer/` |
| 格式与构建资料 | `documents/FormatsReleasedToPublic/`、`documents/technical/`、`BuildingHomeworld.doc` |

需要循序渐进的阅读安排与练习题，见[源码学习路线](learning_guide.md)。
