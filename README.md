# vividcode

面向代码理解的桌面分析与可视化工具。使用 C++ / Qt 构建界面，分别用 Python/libclang、Roslyn、Python AST、TypeScript Compiler API 和 JDK 编译器接口分析 C++、C#、Python、JavaScript/TypeScript、Java。

## 当前能力

- 顶部“语言 / Language”可选择简体中文或 English，自动保存，下次打开程序生效；AI 讲解可跟随界面或单独指定语言。
- 浏览 C/C++、C#、Python、JS/TS、Java 源码，文件树按路径筛选，右键只读速览。
- C++ 导入 compile_commands.json，提取类、成员、函数、继承、调用、引用和覆写。
- C# 加载 .csproj/.sln，分析跨文件/跨项目关系、接口实现与覆写；支持 Unity 生成工程。
- Python 分析类、普通/异步函数、导入、继承、跨文件调用和成员引用，支持源码编码声明。
- JS/TS 支持 JSX/TSX、模块、类、函数/箭头/匿名函数、调用和成员关系，读取 tsconfig/jsconfig、paths/references。
- 自动按语言分析，共享 graph.db，同画布分区；支持关系类型/范围筛选、焦点图、类/文件展开、缩放、平移和拖动。
- 右键节点/连线定位定义或发生处；缓存复用、后台检查、取消、失败保留旧图和 F5 更新。
- -g 普通生成，-sg 强制读取全部支持源码并解除数量/单文件限制；目录排除保留。两者只显示文件完成数字进度和英文 Refreshed.，强制模式不额外打印警告，仍需留意资源消耗。
- -c 交互设置所有项目共用的源码文件数量上限，-n 保留最新记录，-d 删除当前项目数据。
- 生成时在当前窗口输入 stop 并回车；新项目清理本次输出，已有项目和语义索引保持不变。
- 按语言安装解析组件，右键“分析环境…”设置用户本机路径。

Java 已支持类/接口、方法/字段、继承/实现/覆写和跨文件调用；动态目标保守显示可能调用或未解析。AI 解析支持 OpenAI 兼容接口、Windows 加密 Key、文件哈希增量和依赖影响更新；单击画布文件/类/函数/变量显示纯解析卡片，点空白收起，不使用 Esc 或关闭按钮。讲解程度为初学者/进阶/专业，默认进阶，语言可跟随界面/中文/English，见 [AI 解析](Document/AI文件解释.md)。尚无跨语言调用、源码编辑、布局持久化、变量读写细分。当前范围与准确性见 [功能文档](Document/README.md)。

## 构建与运行

Windows x64、Visual Studio 2022 C++ 工具链、Qt 6.11.2 MSVC 64 位。默认只构建/安装核心与 Qt，通过 -Languages 选择 cpp/csharp/python/javascript/java；Node/Python/.NET/LLVM/JDK 由用户准备。

```powershell
cd VividCode
.\build.ps1 -Languages @('javascript') -DownloadDependencies
.\install-command.ps1 -Languages @('javascript')
```

Languages 是每次安装的完整列表；更新时明确指定全部所需组件。首次解析库准备需要显式下载/还原开关；客户端不自动安装运行时。测试需显式添加 -RunTests。

共享程序在 %LOCALAPPDATA%/vividcode/runtime，缓存按项目路径保存。2026-11-04 本机正式安装已更新为新增 AI 文件解释的 Release，保留 cpp/csharp/python/javascript/java 五种解析组件及既有设置/缓存，普通 PowerShell 的命令解析、程序/Network/TLS/分析器物理路径和构建哈希已核验。进入目标项目运行 vividcode -g，双击 .vividcode/vividcode-viewer.exe。

工具路径可配置，详见 [源码使用指南](VividCode/README.md) 和 [构建环境与运行](Document/构建环境与运行.md)。本轮 Release、十组相关回归及最后四组针对性复验通过；真实付费 AI 服务、大型工程与干净机器分发未验收。

## 目录

| 路径 | 用途 |
| --- | --- |
| VividCode/ | 现有 C++/Qt 工程、分析器、测试与构建脚本 |
| Document/ | 当前功能、运行、验证及 AI 交接 |
| PROJECT_BRIEF.md | 产品背景、已确定方向和当前状态 |
| AGENTS.md | 协作与文档维护规则 |

原始解决方案为 VividCode/VividCode.sln，主窗口为 VividCodeWindow；正式产品名及命令为 vividcode。本仓库不包含预编译程序、Qt DLL、本机运行时或索引。

远端为 [piger114514/VividCode](https://github.com/piger114514/VividCode)。用户要求当前私人开发、准备完成后公开，许可证未定；托管记录见 [GitHub 托管](Document/GitHub托管.md)。

2026-11-15 英文版 0.1.2：主界面、关系图、AI/Pi 设置、五种语言分析器、启动器与内置错误提示统一为英文；AI 的 auto 跟随英文界面，仍可显式选择中文解释并复用原中文缓存。补齐 C# errors 字段的失败状态展示，构建时部署 Visual C++ app-local DLL。Windows x64 安装包位于 VividCode/dist/english/vividcode-Setup-0.1.2-x64.exe，英文指南见 VividCode/README.en.md，交付与验证见 Document/英文版与打包.md。
