# vividcode 源码使用指南

更新日期：2026-11-04。

## 本机 PI 辅助

打开项目后，在画布底部切换到“PI 辅助”，点击“连接 Pi”，输入要求后点击发送或按 Ctrl+Enter。Pi 沿用自身模型、登录和工具配置，可以修改当前项目文件；“停止”终止任务但保留已写入内容。官方默认安装可自动检测，找不到时在“Pi 设置…”选择 Pi / Node 路径。

“每次任务结束后更新代码关系和 AI 解释”默认关闭，勾选后保存。开启时先更新关系图，再用现有 AI 解析接口更新变更文件及受影响解释，可能产生接口费用；无源码变化不请求解释。关闭时用 F5 和顶部 AI 解析手动更新。“图谱信息”页签保留颜色与节点/关系说明；拖动分隔条调整底部高度。详见 [PI 辅助与 Pi 接入](../Document/AI助手与Pi接入.md)。

## AI 解析

最右侧“AI 设置…”填写 OpenAI 兼容 Base URL（保留 `/v1` 等前缀）、模型名和 Key，选择“跟随界面 / 中文 / English”和“初学者 / 进阶 / 专业”（默认进阶），保存后点击顶部“AI 解析”。单击画布文件、类、函数/方法、变量或展开容器，弹出只包含所在文件 AI 解析的卡片，可滚动/选中文字，点空白处收起，不使用 Esc 或关闭按钮。悬停仍是普通符号信息；左侧树不显示解析。单击不请求 API，源码不改动。三种程度各有缓存，原缓存沿用为进阶基线。首次逐文件解析，后续只请求新建、哈希变化及 AI 判断受依赖影响的文件；失败保留、取消和重试继续支持。“AI 任务…”查看进度，Key 使用 Windows 用户加密保存。

源码会发送给你填写的服务，可能产生费用；单文件默认发送上限 256 KiB，可在 AI 设置调整。新文件要在 F5 更新语义图后才能出现在画布。循环影响可能标记为下次继续更新；功能、限制与验证见 [AI 文件解释](../Document/AI文件解释.md)。

vividcode 使用 C++ / Qt 展示源码结构与关系。C++、C#（含 Unity）、Python、JavaScript/TypeScript 已有独立分析器；Java 使用本机 JDK，支持显式工程配置和关系分析。

## 文件、函数与变量

画布文件节点按语言显示不同颜色，下方有“文件颜色”图例：C/C++ 蓝、C# 紫、Python 绿、JavaScript 黄、TypeScript 青、Java 橙。普通概览、展开文件分组与双击后的关联文件使用同一套配色，左侧树沿用原样式。

打开项目并等待分析，在左侧“结构与关系”展开文件和所属类/函数，可看到全局变量、成员、参数和局部变量。双击名称只在画布显示关联文件；右键文件/连线选择具体源码位置，“显示全部工程关系”返回。升级后打开项目或 F5 会自动重抽取旧分析结果，无需删除缓存。没有跨文件引用的局部变量通常只有自己的文件；动态引用与画布上限仍有边界，详见[基础文件浏览](../Document/基础文件浏览.md)。

## 构建和安装

当前开发环境为 Windows x64、Visual Studio 2022 C++ 工具链及 Qt 6.11.2 MSVC 64 位。默认只构建/安装核心及 Qt，按需选择解析组件；Node、Python、.NET SDK、LLVM、JDK 使用本机安装。

在本目录 PowerShell 执行：

```powershell
# 核心
.\build.ps1
.\install-command.ps1
# 以 JS/TS 为例，首次准备解析库需明确允许获取依赖
.\build.ps1 -Languages @('javascript') -DownloadDependencies
.\install-command.ps1 -Languages @('javascript')
# 需要运行检查时添加 -RunTests
.\build.ps1 -Languages @('javascript') -RunTests
```

Languages 可选 cpp/csharp/python/javascript/java，是本次安装的完整列表；重装未选组件移除，环境设置和缓存保留。多语言示例为 `@('cpp','csharp','python','javascript','java')`。-QtRoot/-CMake 可覆盖工具路径。脚本策略阻止时可使用 `powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1`。

本机正式安装已更新至当前 Release，包含 cpp/csharp/python/javascript/java，普通 PowerShell 可直接使用 vividcode。后续功能更新通过验证后直接更新正式安装；本机安装验证流程见构建文档。

| 解析组件 | 本机要求 |
| --- | --- |
| cpp | Python 3.12+ x64、LLVM/libclang、内置头文件、真实 compile_commands.json 与项目依赖 |
| csharp | .NET SDK、目标工程工作负载/引用；Unity 需实际生成工程与 DLL |
| python | Python 3.12+ x64（含 sqlite3） |
| javascript | Node.js 22/24 x64；TypeScript 5.9.3 随所选组件提供 |
| java | 完整 JDK 17+；显式配置源码根/classpath/modulePath/release，禁用注解处理器 |

右键“分析环境…”配置 exe、libclang.dll 和可选 resource 目录；留空自动查找。显式错误路径不回退，缺依赖只影响对应语言。普通 UTF-8/UTF-16 文件浏览不要求语言环境；Python 特殊编码声明读取需本机 Python 及 python 组件。

## 运行与操作

安装后重新打开终端，进入目标项目根目录：

```powershell
vividcode -g
vividcode -sg    # 强制读取，解除数量/单文件限制；谨慎使用
vividcode -c     # 按提示输入全局源码文件数量上限
vividcode -n     # 只保留最新成功文件级记录
vividcode -d     # 删除当前项目生成目录和集中缓存，先关闭查看器
vividcode -help
```

-g 生成 .vividcode/vividcode-viewer.exe 小启动器、project.json 和 records 中的 index.json。双击启动器打开该项目；各项目共用 %LOCALAPPDATA%/vividcode/runtime。更新共享安装后已有启动器直接调用新版。

记录包含路径、大小、SHA-256，不是源码备份。源码变化时 -g 或已初始化项目打开/F5 创建新记录，内容不变不重复。-n 不改人工 Document 文档或语义索引，-d 保留共享程序及其它项目。

生成中只在当前窗口输入 stop 并回车。新项目取消后没有 .vividcode；已有项目取消后原文件、历史和语义索引不修改，不新增本次记录。

-c 设置所有项目共用的源码文件数量（正整数，默认 20000），不清理历史。-sg 仍排除 build/node_modules 等目录及链接，成功后该项目保持强制模式，普通 -g 恢复普通模式。强制读取可能大量占用 CPU、内存、磁盘；图形/结果/后端累计资源限制仍保留。

终端回应已简化为英文。-g/-sg 在同一行刷新 `Generating: 已完成/总数`，成功统一 `Refreshed.`，不输出记录 ID/路径或 -sg 单独警告；有变化和没有变化的成功文案相同。-c 只提示 `Enter file limit: `，成功 `File limit set to 数量.`，错误 `Enter an integer from 1 to 2147483647.`；-n 输出 `Old data cleared.`，-d 输出 `Project cleared.`。stop 取消输出 `Cancelled.`。重定向时仅保留开始/最终计数，不逐文件输出。错误原因和涉及路径仍会保留，完整说明见控制台文档。
1. Ctrl+O 打开项目；左侧切换“项目文件”或“结构与关系”，按路径筛选文件。
2. 自动分析完成后查看语言分区图。顶部筛选关系类型，范围下拉框选择全部已加载范围或具体工程/TU。
3. 选择类/方法/成员查看相关关系；右键节点查看定义，右键连线查看发生处。源码只读，点击窗外、Esc 或 × 关闭。
4. 关系图默认按类/文件依赖从左到右分层，循环依赖组在同一层标记，无关联区域分开摆放。单击节点突出直接相关的节点/连线，单击连线突出两端，空白处取消高亮；选中不移动节点。滚轮 5%–300% 缩放，空白处平移，拖动节点。缩放只改变比例，文件/类默认缩略；右键“显示状态 → 显示详情”或双击缩略节点才展开，右键“显示缩略”收起。“适应画布”保留展开状态。右键“全部简化”将全部已加载分组设为缩略，保持当前缩放比例，之后可单独展开。所有箭头从被引用者右侧指向引用者左侧，例如 A 调用 B 时显示 B → A。布局尚不保存，展开/筛选/刷新可能重新排列。
5. 保存源码后 F5/右键刷新。打开先显示成功缓存再后台检查，取消/失败保留旧图，右键可取消、导入配置、重选入口、适应画布。

C++ 自动查找已导入数据库或根目录/build 的 compile_commands.json，缺配置时右键导入，不猜编译参数。C# 只查根层 .sln/.csproj，同语言多个入口列表选择；配置/目标框架改变后自动刷新。Python、JS/TS、Java 根据源码自动加入队列。Java 配置读取根目录 vividcode.java.json，见 [Java 语义索引](../Document/Java语义索引.md)。

Unity 先在对应编辑器完成编译并生成 .sln/.csproj；改脚本集合、asmdef、包或目标后先重生成工程，再刷新。Python 支持项目根和 src 的导入；JS/TS 支持 JSX/TSX、tsconfig/jsconfig、paths/references。

开发版也可运行：

```powershell
& .\build\Release\vividcode-viewer.exe 'E:\你的项目'
```

## 原始 Visual Studio 工程

VividCode.sln 有 vividcode-viewer、vividcode、vividcode-launcher，选择 Release / x64 输出 x64/Release。控制台入口在 vividcode → Source Files → cli_main.cpp；调试窗口选择 vividcode-viewer，调试 CLI 选择 vividcode，默认参数 -help。

VS 不自动准备全部语言组件，所需 prepare 脚本应显式执行，安装可指定 -RuntimeDirectory .\x64\Release。CMake 生成工程为 build/vividcode.sln，产物 build/Release；不要编辑生成文件。

## 代码阅读入口

| 文件 | 职责 |
| --- | --- |
| main.cpp、VividCode.h/.cpp | 图形入口、主窗口、文件树与源码导航 |
| cli_main.cpp、ProjectStore.h/.cpp | 用户命令、JSON 历史、生成/清理 |
| ProjectLauncher.cpp、SharedRuntime.h | 原生启动器、共享程序与项目缓存 |
| ProjectScanner.h | 扩展名、排除、迭代扫描 |
| AnalysisEnvironment.h | 本机工具查找与环境设置 |
| ProjectDetection.h、SemanticPanel.h/.cpp | 自动发现、队列、配置、状态与结果汇总 |
| RelationGraph.h/.cpp、RelationLayout.h、SourcePopup.h | 真实图模型、依赖分层/循环组、选中高亮、证据与源码工具窗口 |
| UnifiedGraphStore.h | 各语言成功图的 SQLite 事务 |
| analyzer/、csharp-analyzer/、javascript-analyzer/、java-analyzer/ | 各语言解析与缓存 |
| CMakeLists.txt、build.ps1、tests/ | 构建、部署和检查 |

初学者讲解见 [vividcode代码解析.txt](../Document/vividcode代码解析.txt)，详细交接见 [文档索引](../Document/README.md)。

## 当前限制和验证

默认扫描 20,000 源码文件/50,000 条目，-c 调整全局文件数量，目录上限随之增加；-sg 解除这两项限制。不跟随链接，保留目录排除，扫描仍在 UI 线程。普通源码速览 2 MiB，强制项目解除单文件大小限制，UTF-8/UTF-16 BOM 或无 BOM UTF-8；Python 合法编码声明支持 GBK。

各语言同画布分区，每语言最多 81 节点/160 条聚合连线；配置并集不代表所有构建配置一致。动态/反射等目标保守处理。Python 复用语法事实后重解析全范围关系；JS/TS、Java 变化时整工程重建。没有 Maven/Gradle 自动导入、跨语言调用、变量读写细分、自动监控、代码编辑、语法高亮、行号栏、布局持久化或完整英文界面。AI 文件解释及其边界见前文和独立功能文档。

2026-10-24 源码清理版 CMake Release 与原始 VS Release/x64 查看器工程构建成功；核心、C++/Python/JS 后端、五语言界面及组件隔离共十组检查全部通过（163.91 秒），正式安装已更新并核验程序/分析源码和数据保留。主窗口直接由 C++ 建立，空 Designer 表单和 uic 依赖已移除。此前真实 C++、普通 .NET/WinForms/WPF、用户 Unity 项目及 Python/JS 源码副本已有验证。Debug、大型工程、完整桌面人工交互和干净机器未在本轮验收。详细证据见 [构建环境与运行](../Document/构建环境与运行.md)。

## 生成安装 exe

仅增加打包能力，复用现有安装脚本；不自动下载语言运行环境。先构建 Release，再安装开发工具 Inno Setup 6，然后在本目录执行：

```powershell
.\package.ps1 -Version 0.1.0
```

版本号由发布者填写，产物位于 `dist/vividcode-Setup-0.1.0-x64.exe`。默认携带五种分析组件；`-Languages` 指定包内组件。编译器未发现时使用 `-Compiler '完整路径\ISCC.exe'`，`-StageOnly` 只整理载荷。包只封装原有当前用户安装流程和 PATH 注册，没有新增卸载器或快捷方式。参数、升级规则与已执行验证见 `../Document/构建环境与运行.md` 的生成安装 EXE 章节。

### 自选安装目录与再次安装

0.1.1 示例包起提供目录选择，默认显示记录中的现有安装位置。保持该目录为升级；改选另一个空目录为迁移，保留组件、设置和缓存，校验成功后清理旧安装。新目录不能包含无关文件，也不能与旧目录相互嵌套。安装前关闭程序，完成后重开终端/IDE。用户共享根记录在 HKCU\Software\vividcode\InstallHome，并注册 VIVIDCODE_HOME 与 PATH；测试仍可显式覆盖 VIVIDCODE_HOME。打包命令可使用 `./package.ps1 -Version 0.1.1`。
