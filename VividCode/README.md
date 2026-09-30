# vividcode 基础运行版

正式名称为 vividcode，使用 C++ 和 Qt Widgets 浏览真实项目中的文件与源码，并通过控制台生成项目查看器、管理文件级代码文档记录。

已实现 C++ 语义索引与真实关系图：独立 Python/libclang 分析，SQLite 缓存，Qt 展示类、函数、成员和真实关系并定位源码。界面采用黑色/深灰背景与浅色文字；缩小时看类，放大后展开内部符号及相关关系。其它语言暂为文件浏览。运行方式与限制见 `../Document/C++语义索引.md`。

打开项目后在顶部导入构建系统生成的 `compile_commands.json`。后台完成后选择编译单元/配置及符号，在“真实关系图”查看关系，也可切换到“C++ 结构与关系列表”。点击节点或关系定位源码；点击“更新 C++ 索引”或 F5 检查修改。已有配置下次打开时自动检查。缺数据库需先从构建系统导出，程序不猜测编译参数。

本项目可使用 `build/semantic-validation/compile_commands.json`；已完成本项目索引。`--g` 部署 `analyzer/` 下的独立 Python、libclang 和内置头文件，语义缓存位于 `.vividcode/semantic/`。项目自身 SDK/Qt/第三方头文件仍须可访问。`build.ps1` 首次准备分析运行包需联网，默认读取本机 `D:/LLVM`；运行查看器不会下载依赖。

## 控制台使用

本机已将 `build/Release` 注册到当前用户 PATH。在新终端中切换到目标项目目录后运行：

```powershell
vividcode --g
```

它会生成当前目录的 `.vividcode`，里面有 `vividcode-viewer.exe`、Qt 运行依赖、`project.json` 和 `records/`。双击生成的查看器即可打开这个项目。代码有变化时，重新 `--g` 或在查看器中按 F5 会保存新的文件级记录；无变化不重复创建记录。

```powershell
vividcode --c     # 清理旧记录，只保留最新成功保存的一份
vividcode --d     # 删除当前目录的整个 .vividcode（先关闭查看器）
vividcode --help  # 查看帮助
```

`--c` 不会改动源码或人工维护的 `Document` 文档。当前记录为文件路径、大小、SHA-256 和 Markdown 概览，不是完整源码备份，也不是类/函数语义文档。最新记录位置由 `.vividcode/project.json` 中的 `latest` 指定，对应 `records/<记录ID>/code.md`。

如果旧终端找不到命令，请重开终端，必要时重启其宿主 IDE；也可以在本目录运行 `./install-command.ps1`，使当前 PowerShell 立即获得命令路径。工作区移动后需要重新注册路径。

开发交接和记录格式见 `../Document/控制台与记录管理.md`。

## 先运行，再阅读代码

1. 双击 `build/Release/vividcode-viewer.exe`。
2. 点击工具栏的“打开演示”，程序会打开附带示例源码：三份 C++ 文件，以及 `Greeter.cs`（C#）、`Greeter.py`（Python）、`Greeter.js`（JavaScript）、`Greeter.java`（Java）各一份。
3. 点击左侧 `Greeter.h`，查看类的声明；点击 `Greeter.cpp`，查看函数实现；点击 `Greeter.cs`、`Greeter.py`、`Greeter.js`、`Greeter.java` 查看其它语言的源码。
4. 导入 C++ 编译数据库并完成索引后，选择编译单元和符号查看关系图。滚轮放大到 90% 后展开类中的函数、成员及关系，缩小到 75% 以下收起；双击类也能展开，点击节点或关系位置查看对应源码。未配置索引时仍可浏览文件树。
5. 点击“打开项目…”选择自己的源码目录；输入文件名或部分路径进行筛选。
6. 用 Visual Studio 修改源码并保存，再返回查看器按 F5。新增、修改、删除会反映到当前视图中。

源码区域只读。分隔线可以拖动改变区域大小；工具栏支持放大、缩小和适应画布；按住画布空白位置可拖动画布。

`build/Release` 中的 DLL、platforms 文件夹和 demo 文件夹应与 exe 放在一起，不能只复制 exe。此目录已在本机部署 Qt 运行依赖，未在另一台干净 Windows 电脑验证；另一台机器可能需要安装 Microsoft Visual C++ 2015–2022 x64 运行库。

也可以将项目目录作为唯一命令行参数传入查看器，例如：

```powershell
& .\build\Release\vividcode-viewer.exe 'E:\你的项目'
```

## 从源码构建

本机路径默认是 Qt 6.11.2 MSVC 64 位、Qt 自带 CMake、Visual Studio 2022 的 C++ 工具链。

在本目录打开 PowerShell：

```powershell
.\build.ps1
```

若 PowerShell 的本地脚本策略阻止执行，可以仅对本次进程使用：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

脚本依次配置、编译 Release、部署 Qt DLL/插件、运行自动检查。Qt 安装位置不同时，使用 `-QtRoot` 参数；CMake 位置不同时，使用 `-CMake` 参数。本机 Qt 自带 CMake 曾在读取缓存时崩溃，脚本采用 `--fresh` 重新配置以避开该问题。

原始 `VividCode.sln` 现包含 `vividcode-viewer` 和独立控制台项目 `vividcode`，依赖 Qt VS Tools 提供的 QtMsBuild。重新加载解决方案后，在 `vividcode → Source Files` 下可找到 `cli_main.cpp`。选择 **Release / x64** 生成解决方案，输出在 `x64/Release`；此构建及产物的 CLI 生命周期、GUI 冒烟检查已通过，Debug 尚未验证。调试窗口时将 vividcode-viewer 设为启动项目；调试控制台时选择 vividcode，默认参数为 --help。

CMake 的 `build.ps1` 入口仍可用，生成的解决方案在 `build/vividcode.sln`，输出在 `build/Release`。不要编辑 CMake 生成的工程文件。现有用户 PATH 仍指向这套 CMake 输出；验证原始 VS 新产物时使用 `x64/Release` 中 exe 的完整路径。

首次构建后可运行 `./install-command.ps1` 将 Release 目录注册到当前用户 PATH。本机已执行完成。

## 应该从哪里学起

| 文件 | 先理解什么 |
| --- | --- |
| `main.cpp` | 创建 QApplication、创建窗口、进入事件循环 |
| `cli_main.cpp` | 控制台参数解析，分派 --g/--d/--c |
| `ProjectStore.h/.cpp` | 项目运行文件生成、记录保存、历史清理和限定目录删除 |
| `VividCode.h` | 主窗口有哪些控件和状态，以及对外提供的打开项目函数 |
| `VividCode.cpp` | 组装布局，使用 connect 连接按钮、文件选择与处理逻辑 |
| `ProjectScanner.h` | 遍历文件夹、筛选扩展名、排除生成目录，返回相对路径 |
| `SemanticPanel.h/.cpp` | 导入索引、选择编译单元和符号、共享关系图与列表的筛选条件 |
| `RelationGraph.h/.cpp` | 根据真实符号 ID 和 parent_id 建立分层关系图，绘制节点与箭头，处理缩放与源码导航 |
| `VividCode.ui` | 原始 Qt Designer 主窗口外壳；基础版主要布局在 C++ 中创建 |
| `SmokeTest.h` | 在临时目录中验证扫描、选中、刷新和异常输入 |
| `CMakeLists.txt` | 指定源文件，启用 Qt 的 moc/uic/rcc，链接 Widgets |

建议先在 `VividCode.cpp` 中修改窗口标题或按钮文字，执行 `build.ps1`，亲自走一遍“修改 → 编译 → 运行”。随后再研究 `openProject → scanProject → rebuildViews → showFile` 这条流程。

## 当前范围与限制

- 支持 C/C++ 的 `.c/.cc/.cpp/.cxx/.h/.hh/.hpp/.hxx/.inl/.ipp`、C# 的 `.cs`、Python 的 `.py/.pyw/.pyi`、JavaScript/TypeScript 的 `.js/.jsx/.mjs/.cjs/.ts/.tsx`、Java 的 `.java`，均不区分扩展名大小写。
- 默认排除 `.git/.vs/.codegraph/.codemap/.vividcode/build/out/x64/x86/debug/release/node_modules/vendor/third_party/_deps/CMakeFiles`，以及 JS/TS 的 `dist/.next/.nuxt/.cache/.parcel-cache/coverage`、Java 的 `target/.gradle/.mvn`，还有 `build-*`、`cmake-build-*` 目录，不跟随符号链接。排除列表目前写在 `ProjectScanner.h` 中。CodeGraph 是独立开发辅助工具，保留其 `.codegraph` 名称。
- 扫描在主线程完成，适用于可读的小型本地项目；大型目录可能暂时无响应。达到 5,000 个源码文件或 50,000 个目录条目时停止并提示；无权限的子目录可能无法列出。
- 原文件卡片画布已由真实关系图替换；文件树保留全部已扫描文件。图按当前编译单元和符号展示局部关系，最多 81 个节点、160 条合并连线，超出范围会提示；列表保留当前筛选的关系证据。
- 只读源码按 UTF-8 解码；GBK 等其他编码暂不自动识别。超过 2 MiB 的文件不加载。
- 每次打开或刷新重新扫描；已初始化项目会更新文件级历史记录。C++ 语义索引通过 SQLite 缓存，后台检查编译配置和文件变化；没有后台文件自动监控、布局持久化或最近项目恢复。
- C++ 支持类、成员、函数、继承、直接调用及成员引用；关系图基于实际索引，外部符号缺少归属信息时明确归入“项目外依赖”。变量读写细分、其它语言语义分析、全项目关系图和 AI 接口尚未实现。

最新构建：CMake 和原始 VS Release x64 均编译成功。基础关系图此前四组 CTest 通过，本工程 10 个真实编译单元解析及源码定位验证通过；中间分层版本两组界面检查通过。最后交互调整按用户要求不再追加检验，不将此前结果当作最终版本完整验收。2026-09-30 已将源码目录改为 VividCode，主窗口类改为 VividCodeWindow，源码文件改为 VividCode.h/.cpp；本次验证见构建环境与运行文档。

用户已确定当前先实现真实关系及地图式类成员展开，本轮功能已完成。后续扩展以新的需求为准。

2026-09-30 命名迁移验证：CMake/原始 VS/Ninja Release 构建成功；四组 CTest、原始 VS 产物 CLI 和界面检查通过；新路径的 10 个 TU 真实索引与调用关系源码定位通过。当前命令 PATH 指向 VividCode/build/Release，旧终端需重开。
