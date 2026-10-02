# Homeworld 1 源码学习文档

这组文档从源码出发，梳理 Homeworld 1 的运行结构、数据流和设计取舍。建议先读本页，再按 [学习路线](learning_guide.md) 跟着代码走；需要查某个子系统时，再打开对应模块深读。

## 建议顺序

| 顺序 | 文档 | 读完后应能回答 |
| :-- | :-- | :-- |
| 1 | [源码架构地图](analysis.md) | 程序有哪些主要层次，关卡、资源、模拟和渲染如何连接？ |
| 2 | [源码学习路线](learning_guide.md) | 从哪个入口开始读，怎样沿调用链跟到对象和数据？ |
| 3 | [协作式任务调度](modules/task_scheduler_overview.md) | 主循环如何驱动每帧任务和定频逻辑？ |
| 4 | [用 C++20 协程表达 task](modules/task_coroutine_migration.md) | taskdata、pause/resume 和补跑调用如何映射到 coroutine frame 与 handle？ |
| 5 | [对象模型](modules/spaceobj_overview.md) | 静态舰船定义与运行中的舰船如何分开管理？ |
| 6 | [资源加载](modules/resource_loading_overview.md) 与 [statscript 数据绑定](modules/statscript_overview.md) | 关卡需要的文件怎样变成可用的静态数据？ |
| 7 | [KAS 脚本](modules/kas_script_overview.md) 与 [AI 决策和团队执行](modules/ai_features_overview.md) | 任务脚本和电脑玩家 AI 怎样通过共享的团队 move 执行命令？ |
| 8 | [LOD 系统](modules/lod_overview.md) 与 [渲染管线](modules/render_pipeline_overview.md) | 对象怎样选择细节并经渲染接口变成像素？ |
| 9 | [UI 系统](modules/ui_system_overview.md) | 布局、控件、输入事件与业务界面怎样分层？ |

## 协程设计参考

- [用 C++20 协程表达 task](modules/task_coroutine_migration.md) 与 [独立原型代码](modules/task_coroutine_prototype.cpp)：把旧调度器映射到 promise、coroutine handle 和协程帧。
- [用 C++20 协程设计 Go 风格 goroutine 运行时](modules/cpp20_goroutine_runtime_design.md)：分析协程帧、独立栈、跨线程恢复和 M:N 调度所需的运行时部件。

## 按兴趣跳读

- 想理解一帧发生什么：先看 `src/Win32/main.c` 的 `WinMain`，再看 `utyTasksDispatch`、`taskExecuteAllPending` 和 `universeUpdateTask`。
- 想理解舰船从数据到对象：从 `src/Game/levelload.c` 进入，接着读 `file.c`、`statscript.c`、`spaceobj.h` 和 `universe.c`。
- 想理解任务行为：从 `src/SinglePlayer/Mission01.kas` 开始，追到 KAS 生成入口、`KAS.c`、`KASFunc.c`，再追到 `AITeam.c` 和 `AIFleetMan.c`。
- 想理解画面如何生成：从 `src/Game/univupdate.c` 的渲染列表更新开始，继续到 `mesh.c`、`LOD.c` 和 `src/rgl/`。

## 文档范围

`analysis.md` 提供整体结构和数据流；`learning_guide.md` 提供带问题的阅读路线；`modules/` 下的文档深入讲解单个子系统。实现事实以仓库里的 `.c`、`.h`、脚本和随附格式文档为准。涉及作者意图或架构优劣的判断会标为推断或设计观察。

`BuildingHomeworld.doc` 主要讲构建和发布流程，不是完整的架构说明。格式细节可继续查 `documents/FormatsReleasedToPublic/`，KAS 与 AI 的语义可查 `documents/technical/`。
