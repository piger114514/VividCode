# C++ 语义索引

更新日期：2026-10-02。

## 2026-10-02：共用 C++ / C# 界面

C++ 分析器、编译数据库与增量 SQLite 缓存保留，界面自动按语言检测工程；C++ 与 C# 分别分析后把成功结果写入同项目 `graph.db`，默认全范围在同一画布按语言分区显示。左侧“结构与关系”与“项目文件”共用页签，没有语言/符号选择下拉框；顶部为关系过滤和 C# 配置。刷新、导入配置、取消与源码速览改为右键。完整行为及限制见 [多语言分析与共享图数据库](多语言分析与共享图数据库.md)。

## 用户已确认的范围

- 独立 Python 进程调用 LLVM/libclang，C++ / Qt 查看器展示结构与关系列表并定位源码。
- 提取类、成员、函数、声明与定义位置、继承、直接调用和成员引用，SQLite 存储；符号使用独立 ID，关系携带发生位置。
- 导入现有 `compile_commands.json`；缺失时提示配置，不自行猜测参数。
- Python 与 libclang 随运行包部署，纳入 `.vividcode` 的 `-g/-d` 生命周期。
- 用户随后确认真实关系图和地图式类/成员层级：缩小时看类，放大看函数、成员；最新确认按语言独立分析并共用图数据库，同画布分区显示。具体范围可选 TU/配置，也可显示当前已导出各配置关系并集；Python/JS/TS/Java 仍仅文件浏览。

## 当前实现与操作

已实现独立 Python/libclang C++ 分析器，Qt 默认显示“真实关系图”，另保留“结构与关系列表”。支持命名空间、类/结构体、成员、函数/方法、声明定义位置，以及继承、直接调用、成员引用、普通引用和覆写；虚方法调用保守标记为“可能调用”，成员引用暂不细分读写。C# 已单独接入 Roslyn；Python/JS/TS/Java 仍为文件浏览。

1. 在项目目录执行 `vividcode -g`，打开生成的 `.vividcode/vividcode-viewer.exe`；也可用开发版查看器打开项目。
2. 自动发现已导入的有效数据库，或根目录 / `build/compile_commands.json` 并分析。缺失时右键“导入工程配置…”选择真实构建系统导出的数据库。未初始化项目会创建启动器/JSON，需要共享运行环境。
3. 默认显示全部已加载范围，也可选择具体 TU/配置。左侧“结构与关系”树或图中双击选择类/函数/成员；右键“查看源代码”打开只读小窗口，节点定位定义/声明，连线定位发生处。单击只选中。
4. 打开时先读已有成功结果再后台检查；F5 或右键刷新检查修改，右键可取消。失败保留旧图及明确状态，C# 失败也不删除 C++ 结果。

## 真实关系图

- 数据完全来自当前索引快照的符号 ID、parent_id 和关系，不根据名称或示例推断连线。同名重载通过 ID 区分。方法焦点显示一层入边/出边，类焦点覆盖当前 TU 中属于该类的成员关系；成员层保留递归/自环。
- 地图式缩放：初始为 65% 概览；放大至 90% 进入成员层，缩小到 75% 以下收起，使用两个阈值避免反复切换。概览通过真实 parent_id 收起类内符号并隐藏内部连线，跨范围连线按类型/置信度汇总；展开后恢复原始端点、方法和成员。类容器的包围框表达真实归属，不添加虚构关系线。
- 全局符号按实际声明文件归入“文件范围”；未记录所属类的项目外符号归入明确标注的“项目外依赖”，不猜测其类名。直接成员即使没有当前筛选类型的边，展开时也保留声明；邻居容器只展示当前相关符号，不宣称该 TU 包含全项目全部成员。
- 箭头方向为起点 → 终点。直接调用橙色，继承蓝色，成员引用绿色，覆写紫色，普通引用灰色；连线同时有中文标签，“可能调用”用虚线并说明运行时目标未确定。
- 单击节点/连线选中并显示证据，右键“查看源代码”定位定义（缺失则声明）或发生处。外部/缺失端点不可定位时明确提示。同端点/类型/置信度的多处关系合并，位置下拉框保留证据，源码菜单提供各处位置；重复证据去重。
- 双击类可展开并选中，双击具体符号切换焦点并同步结构树。拖动更新连线与类容器。滚轮缩放、空白处平移；旧按钮行已移除，适应画布在右键菜单中。无焦点时初始适配全范围。
- 图顶部显示已选范围、层级、节点/连线及证据数量、收起的内部关系数。汇总线保留原始端点与位置。默认合并当前全部已加载工程/TU，也可选择具体范围；不表示所有构建配置或全部运行时调用，无关系提示只对当前范围/筛选成立。
- 每种语言最多 81 节点、160 条合并线，达到限制提示，完整列表可访问。默认全范围合并当前已导出 TU 的关系证据；并集不代表不同宏配置语义一致。布局随焦点/范围/筛选/层级重建，尚无拖动持久化或更多嵌套层级。
- 成功时空诊断区自动收起；出现诊断时显示。左侧文件树继续浏览多语言源码，原文件卡片画布已移除。

不猜测缺失的编译参数，不执行数据库里的编译命令。支持 Windows arguments 数组、command 字符串、常见 clang++ 和 MSVC/clang-cl 参数、相对 include 和强制包含。首版明确拒绝响应文件 `@file` 和预编译头参数，需从构建系统导出不使用这些参数的命令。

本项目真实编译数据库：`VividCode/build/semantic-validation/compile_commands.json`。它来自独立 Ninja 构建，保留原始 VS 和常规 CMake 构建入口。其它 CMake 项目需用支持编译数据库的生成器（例如 Ninja），启用 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`；VS 生成器本身不导出该文件。

## 缓存与部署

- `%LOCALAPPDATA%/vividcode/runtime/analyzer/`：Python 3.13.7 embeddable x64、分析器源码、libclang 23.1.2、Clang 23 内置头文件、许可证和摘要。不需另装 Python/LLVM；项目自己的 SDK、Qt 和第三方头文件仍须可访问。
- `%LOCALAPPDATA%/vividcode/semantic/<项目路径哈希>/config.json`：编译数据库绝对路径、状态、诊断、解析与复用数量；项目移动后需重新导入有效数据库。
- `%LOCALAPPDATA%/vividcode/semantic/<项目路径哈希>/index.db`：SQLite schema 2，`units` 表按编译命令哈希保存命令、依赖哈希及符号/关系 JSON；`meta` 表记录解析器版本。符号采用 `c++:` + USR，同文件多配置独立保存并在界面分别选择。
- 根据 TU 的依赖内容 SHA-256、参数、分析器/DLL 版本、项目与显式包含目录文件清单及相关环境变量决定复用。头文件变化重解析受影响 TU；新增文件保守失效缓存，删除命令清除其结果。
- 一轮全部成功后通过 SQLite 单事务提交；语法错误、缺头文件、非法数据库或取消不替换上次完整快照。取消时 config 可保留 running，界面将其视为未完成/旧结果，下次可恢复运行。schema 不兼容时拒绝，无自动迁移。
- 分析/导出与 ProjectStore 共用根目录 `.vividcode-semantic.lock` 的 Windows 独占句柄，退出自动释放并删除锁文件。`-g/-d` 不与活动分析并发；`-d` 删除所有运行文件与索引，`-c` 只清理文件级历史。

## 源码入口和运行

| 文件（相对 VividCode） | 职责 |
| --- | --- |
| analyzer/clang_bridge.py | ctypes 绑定 libclang 公开 C API，字符串和游标生命周期 |
| analyzer/extractor.py | AST 遍历、USR、位置、结构与关系；跳过外部声明子树，补充必要关系终点 |
| analyzer/analyzer.py | 编译命令、路径检查、互斥、增量、SQLite 事务和 JSONL 协议 |
| SemanticPanel.h/.cpp、ProjectDetection.h、UnifiedGraphStore.h | 自动发现、语言调度、共享成功快照、范围/结构/类型与列表同步、状态和取消 |
| RelationGraph.h/.cpp | buildRelationMap 构建类/成员层模型；证据合并、箭头、容器、缩放阈值、拖动及源码联动 |
| VividCode.cpp | 页签与项目内源码定位 |
| ProjectStore.cpp | 共享路径/旧索引迁移与生命周期互斥 |
| prepare-analyzer.ps1 | 下载固定 Python 包并校验固定 SHA-256，复制本机 LLVM DLL/内置头文件及许可证 |
| tests/test_analyzer.py、SemanticSmokeTest.h、RelationGraphTest.h | 后端真实解析、关系模型与界面端到端测试；可选真实项目验收 |

后端命令（GUI 已封装）：

```powershell
& "$env:LOCALAPPDATA/vividcode/runtime/analyzer/python/python.exe" "$env:LOCALAPPDATA/vividcode/runtime/analyzer/analyzer.py" index --project '项目根目录' --compile-commands '数据库路径'
& "$env:LOCALAPPDATA/vividcode/runtime/analyzer/python/python.exe" "$env:LOCALAPPDATA/vividcode/runtime/analyzer/analyzer.py" export --project '项目根目录'
```

index 输出 JSONL 协议 1（progress/diagnostic/result），失败退出非零；export 输出各 TU 配置与快照的单个 JSON。开发测试可显式设置 VIVIDCODE_LIBCLANG；GUI 清除此覆盖，使用共享安装中的 DLL。

## 已验证

2026-10-02 最新界面：CMake 构建与 prototype_smoke、cli_lifecycle、semantic_ui、csharp_ui 四项相关检查通过。C++ 真正的 libclang 结果与 C# Roslyn 结果共同写入 graph.db，分区同时显示；C++ 关系模型全范围、结构树、源码右键定位、失败不替换后端数据库及切换项目停止进程继续通过。未重跑不变后端的全套解析检查，详见多语言分析与共享图数据库.md。

2026-10-02 共享部署：SemanticPanel 从共享运行包启动分析器，缓存以规范项目路径哈希隔离，project.json 校验归属。旧项目 -g 先逐文件校验迁移旧 SQLite/配置，再移除项目内 analyzer/semantic；两份索引冲突时保留并报错。项目移动后的新路径使用新缓存，旧路径缓存暂不自动回收。CMake Release 构建与四组 CTest 通过；七项真实后端检查含两个项目分别索引与归属错误拒绝访问。当前用户共享安装已完成，本工程旧语义数据库迁移前后摘要一致，10 个 TU 可从共享后端导出；本轮不将缓存读取当作新的真实工程全量索引验收。部署细节见 [控制台与记录管理](控制台与记录管理.md)。

2026-09-30 命名迁移：主窗口为 VividCodeWindow，源码为 VividCode.cpp/.h。本次 CMake 四组 CTest 全部通过，原始 VS 产物的语义界面测试通过；新路径的 Ninja 编译数据库已实际编译验证。旧 .vividcode/semantic 备份后重新生成，新索引 10 个 TU 全部成功，真实关系图中 VividCodeWindow::openProject → scanProject 的调用处和头文件定义定位通过。旧编译数据库与旧索引不再作为当前数据使用。

2026-09-27 关系图阶段：基础关系图版本的四组 CTest 已通过，真实工程 10 个 TU 索引成功，已核对 openProject → scanProject 的调用处和头文件定义。地图式类/成员层级的中间版本通过两组界面检查，之后补充了类双击展开及原始证据说明。最终版本已完成编译；用户明确要求完成后结束、不再检验，未对最后改动追加测试或真实工程重验。不要把此前检查写成最终版本已经完整验收。

- Release x64 构建，CTest 四项 `prototype_smoke`、`cli_lifecycle`、`semantic_backend`、`semantic_ui` 全部通过。
- 六项后端真实 libclang 测试覆盖：中文/空格路径、重载、跨文件关系、继承、字段引用、虚调用、Windows command、强制包含、多配置及重排、缓存复用、共享头文件更新、删除 TU、失败保留旧结果、取消与锁释放、新头文件遮蔽。
- 界面测试覆盖真实后端、结构/关系列表、关系定位源码行、失败时数据库字节不变、切换项目停止旧进程。已检查中文离屏截图 `build/Release/semantic-preview.png`。
- 独立 Ninja 实际 Qt 项目构建成功，真实项目最终 TU 数量与实测耗时见项目档案。
- 原始 VS `.vcxproj/.filters` 已同步新源码及部署入口；本轮未重新执行原始 VS 解决方案构建，不以 CMake 通过替代该验收。

## 当前限制

面向 Windows x64；模板、宏、动态分派及函数指针完整覆盖未验收，函数指针不标为确定调用。默认全范围是当前导出各 TU/配置的关系并集；具体范围仍可选择，尚无不同宏配置的一致性判定。Python 串行解析、Qt 异步运行，结果 128 MiB 与源码 2 MiB 上限保留。Unreal/超大工程和干净电脑未验收。

C# 已接入，C++ / C# 的共享展示已实现；Python/JS/TS/Java 语义分析、跨语言证据及更广泛工程支持等待后续需求，不由路线图自动授权。
