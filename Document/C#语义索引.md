# C# 语义索引与关系图

更新日期：2026-10-02。

## 本轮范围

用户确认：像已有 C++ 功能一样，分析 C# 类、方法调用和成员引用，在现有关系图中连线，并从节点及关系发生处定位源码。优先普通 .NET / WinForms / WPF 项目，有 `.csproj` 或 `.sln`。本轮没有增加源码标识符点击跳转功能，也没有实现 Unity 支持。

复用 C++ / Qt 查看器和现有 `RelationGraph`，增加独立 C# / Roslyn 分析器。Roslyn 4.14.0 与 MSBuild Locator 1.7.8 的依赖版本和 SHA-512 完整性摘要登记在 `VividCode/csharp-analyzer/packages.lock.json`。C# 不交给 libclang，也不通过名称或正则猜测重载目标。

## 当前行为和操作

1. 在工程或解决方案的根目录运行 `vividcode -g`，打开生成的 `.vividcode/vividcode-viewer.exe`。也可使用开发版查看器打开该目录。
2. 自动识别 C# 工程，没有手动语言选择。只查找项目目录第一层 `.sln`（大小写不敏感、不跟随链接、不搜索子目录）；C++ 也存在时分别调度两种语言。
3. 一个解决方案直接分析，多个弹出列表单击或键盘确认。没有 `.sln` 时检测根目录 `.csproj` 或已有导入入口；缺配置可右键“导入工程配置…”。Debug/Release 和目标框架位于顶部，已有缓存恢复保存值；改配置自动刷新。同项目 F5 记住入口，右键“重新选择分析工程…”可重选；取消不会覆盖缓存。导入 `.csproj` 会加载项目引用，`.sln` 加载其中 C# 项目。
4. 默认显示全部已加载工程关系，可用分析范围下拉框缩小范围。左侧切换“结构与关系”并选择类/方法/成员，或在图中双击选择焦点；“选择符号”下拉行已移除。
5. 类/成员缩放、关系类型过滤、节点拖动继续可用。普通单击选中，右键“查看源代码”在小窗口定位定义或关系发生位置；点击窗外、× 或 Esc 关闭。底部嵌入源码区已移除。
6. 打开时先显示缓存并后台更新；F5 或右键刷新检查改动，右键可取消。两种语言结果写入同项目 graph.db，在同一画布分区排列，不制造跨语言连线。完整调度、共享数据及边界见 [多语言分析与共享图数据库](多语言分析与共享图数据库.md)。

需要可用的 .NET SDK、工程所需的工作负载/目标框架及 NuGet 依赖。分析器执行 MSBuild 设计时工程加载，运行目录设为导入工程所在目录，以便 SDK 选择考虑该位置的 `global.json`。不会替用户执行 `dotnet restore` 或安装 SDK；加载/编译失败时显示诊断，可先在工程目录执行 `dotnet restore`，必要时完成正常构建。

## 符号和关系

- 支持命名空间、类/record、结构体、接口、枚举、委托、方法、构造/析构、字段、属性和事件。普通符号用程序集对应的项目身份、配置与 Roslyn 文档符号 ID 构造 `c#:` 哈希；同名重载、泛型原始定义、扩展方法与 partial 类按语义符号处理。局部函数的身份含声明位置，前方编辑可能改变该 ID。
- `parent_id` 来自 Roslyn 所属符号；属性访问器归于对应属性。类/结构体/接口/枚举/委托可作为关系图的容器。元数据依赖标注为外部符号，不编造本地定义路径。
- 提取直接调用 `calls`、可能调用 `possible_call`、继承 `inherits`、接口实现 `implements`、覆写 `overrides`、成员引用 `member_ref` 和类型/方法等普通引用 `references`。
- 继承及接口实现为蓝色，调用为橙色，可能调用显示虚线，成员引用为绿色，覆写为紫色，普通引用为灰色。线型、中文标签、箭头和源码位置共同说明关系。
- 接口调用、非密封虚方法调用以及委托 Invoke 保守标为可能调用；不推断最终运行时对象。显式 `base` 调用按确定的基类目标处理。`dynamic` 和反射无法解析出可靠目标时不生成确定连线。
- 关系保存 `source_id`、`target_id`、类型、置信度及实际文件/行/列；一个关系多处出现时画布合并连线但保留每处证据。位置采用实际源码行号，不采用 `#line` 虚拟映射。

## 缓存、失败与生命周期

共享部署：`%LOCALAPPDATA%/vividcode/runtime/csharp-analyzer/`。分析器是面向 .NET 8 的框架依赖程序，允许使用更高主要版本的已安装运行时；不额外打包第二套 .NET SDK。目标项目仍只保存启动器和 JSON 历史。

C# 缓存位于 `%LOCALAPPDATA%/vividcode/semantic/<项目规范路径 SHA-256>/csharp/`：

- `config.json`：工程入口、配置、目标框架、状态、失败原因、新索引/复用数量。
- `index.json`：schema 1 的上次完整分析快照，按项目保存符号/关系和指纹，原子替换。C++ 的 `config.json/index.db` 仍在上级目录，分析缓存互不覆盖；成功的统一导出另进入上级 `graph.db`，提供跨语言的统一持久化和展示。共同 `project.json` 校验归属。

项目指纹包括源码/附加文件/分析配置文件、工程内容、相关 props/targets、NuGet assets、元数据与分析器 DLL、编译选项、Roslyn/SDK 路径及 vividcode 分析器 DLL 内容。项目引用指纹递归包含依赖项目，源码改变时失效该项目及其依赖者；独立项目继续复用。每次仍需加载 Workspace 和建立 Compilation，复用数量表示跳过诊断与关系重抽取，不表示完全不启动 MSBuild/Roslyn。

含显式自定义 MSBuild `Import` 的工程及其依赖者保守禁用复用，避免对外部导入或属性展开作未经验证的猜测。目录中的构建配置变化可保守影响所有项目。当前每种语言只保留一份最新完整语义快照；切换 Debug/Release/目标框架会重新分析相应配置，不同时保留多份 C# 配置历史。文件级 JSON 历史仍由原有记录模块管理。

所有已捕获输入在提交前再次校验内容；失败不替换完整 `index.json`。取消可能留下 running 状态，界面明确显示结果未更新，下次刷新可恢复。索引与导出复用根目录 `.vividcode-semantic.lock` 的 Windows 独占句柄，进程退出自动释放；分析器 Job Object 确保 Qt 取消后 MSBuild BuildHost 子进程退出。`-d` 沿用原有整项目缓存删除，包含 C++ 和 C#；`-c` 保留两种语义缓存。

## 源码入口与构建

| 文件（相对 VividCode） | 职责 |
| --- | --- |
| csharp-analyzer/vividcode-csharp.csproj | 独立分析程序与固定微软依赖 |
| csharp-analyzer/Program.cs | 参数、MSBuild 工程加载、编译诊断、依赖指纹、快照和全工程导出 |
| csharp-analyzer/Extractor.cs | Roslyn 语义符号、稳定 ID、归属、位置及关系抽取 |
| csharp-analyzer/Store.cs | 共享路径/归属、链接检查、原子 JSON、生命周期锁和子进程取消边界 |
| SemanticPanel.h/.cpp、ProjectDetection.h | setProject 语言分组、selectEntry 列表选择、beginNextLanguage 顺序分析、importCSharpProject 和统一展示 |
| UnifiedGraphStore.h、SourcePopup.h | 同项目 graph.db 与右键源码工具窗口 |
| RelationGraph.cpp | 共享图模型，接口/属性/事件等标签及接口实现关系 |
| VividCode.cpp | 节点/连线信号转到只读源码，限定项目根目录 |
| prepare-csharp-analyzer.ps1 | locked-mode 还原与框架依赖 publish，复制依赖锁及第三方说明 |
| build.ps1、vividcode.vcxproj | CMake/原始 VS 构建的 C# 运行包准备入口 |
| install-runtime.ps1 | 验证并共享安装 C# 分析器和完整 BuildHost 依赖 |
| tests/test_csharp_analyzer.py、CSharpSmokeTest.h | 后端真实语义检查、桌面工程及 Qt 导航检查 |

在 `VividCode` 下运行 `./build.ps1` 完成全部构建和六组 CTest，再 `./install-command.ps1` 更新共享环境。也可独立运行：

```powershell
./prepare-csharp-analyzer.ps1
# 原始 VS 的运行包也可单独准备：
./prepare-csharp-analyzer.ps1 -Destination ./x64/Release/csharp-analyzer
```

后端命令（GUI 已封装）：

```powershell
& "$env:LOCALAPPDATA/vividcode/runtime/csharp-analyzer/vividcode-csharp.exe" index --project '工程根目录' --project-file '工程完整路径.csproj' --configuration Debug
& "$env:LOCALAPPDATA/vividcode/runtime/csharp-analyzer/vividcode-csharp.exe" export --project '工程根目录'
```

`index` 输出 protocol 1 JSONL（progress/diagnostic/result），失败返回非零；`export` 输出现有 Qt 接口的 `state/units/symbols/relations` JSON。

## 已执行验证

- 2026-10-02 最新界面：CMake 构建成功，prototype_smoke、cli_lifecycle、semantic_ui、csharp_ui 四项相关检查通过。真实 C++/C# 混合临时工程确认同项目 graph.db 两份快照、同画布不相交分区、两种语言结构树、Release 自动刷新、C# 失败后 C++ 和 C# 成功结果仍在；源码菜单/行定位、稳定小窗口与窗外关闭通过。分区/源码截图已目视检查，完整证据及运行边界见多语言功能文档。下列条目记录前一阶段，当时的语言/更新按钮现已移除。

- 2026-10-02 自动发现调整：CMake Release x64 构建通过；prototype_smoke、semantic_ui、csharp_ui 三项相关界面检查均通过。CSharpSmokeTest 用真实 `.sln` 与 Roslyn 验证单个自动加载、扩展名大小写、只查根目录、多个方案实际鼠标点击、已有缓存时选择不同入口、取消不改配置、更新按钮重试、F5 沿用选择、无 `.sln` 提示，以及原有图/源码导航、失败保留和语言隔离。检查失败时输出源码行号；最初发现的 Windows 正反斜杠比较问题已在验证代码中修正。没有重跑本轮未修改的 C# 后端五项检查。
- 已目视检查 `build/Release/csharp-relation-preview.png`（顶部按钮行消失）及 `csharp-solution-preview.png`（可点击文件名和取消）。构建及依赖还原需要读取 SDK/NuGet 用户配置，沙箱内被拒绝后获准在沙箱外执行；相关测试安装仅写隔离测试环境，不代表用户普通终端的共享版本已经更新。

- C# Release publish 成功，无编译警告；Qt CMake Release x64 构建成功。
- 六组 CTest 首轮 6/6 通过，58.34 秒，包含原有 C++ 四组回归和新增 C# 后端/界面。后端四项真实 Roslyn 检查覆盖中文空格路径、跨文件/跨项目关系、重载、partial、继承、接口实现、覆写、字段/属性、可能调用、稳定 ID、依赖驱动复用、配置切换、删除、失败保留、锁、取消和恢复。
- 补充实际构建 WinForms、WPF 的 net9.0-windows 测试工程，继承和控件属性引用检查通过，8.32 秒。没有用此结果承诺全部 XAML、Designer 或第三方业务工程都已验收。
- Qt 测试实际选中调用线和方法节点，分别定位 Use.cs 调用语句和 Worker.cs 定义；语言切换清空旧图，错误后快照字节不变，整项目缓存删除通过。
- SDK 选择与分析器 DLL 指纹的最终修改后，两组 C# 检查 2/2 通过（51.44 秒），后端包含新增的五项真实解析检查。
- 原始 VS Release x64 编译和双分析运行包部署成功；当前用户共享 runtime 已更新。从安装的共享分析器对自身实际工程完整副本验收：467 个符号（含外部端点）、1,163 处关系；Program.Index → Extractor.Extract 在 Program.cs:174 的调用与 Extractor.cs:131 的定义位置准确，再次索引复用 1/1。该验证不等于用户业务工程已验收。
- 已保留并目视检查 build/Release/csharp-relation-preview.png。项目档案和构建文档包含环境问题及安装状态。

## 当前限制及待完成

Windows x64。当前 SDK 风格普通 .NET/WinForms/WPF 测试通过；用户未指定业务工程路径，尚未验收其真实业务项目。Unity、旧 .NET Framework 工程、自定义复杂 MSBuild 工程、混合语言 `.sln`、`.slnx`、大型工程和干净电脑未验收。

多目标框架只分析当前加载/指定配置，不同时合并所有目标框架；整个解决方案的显式 framework 参数需对各项目有效。无法定位的元数据、内存生成源码或项目根目录外文件显示不可用；WPF XAML 本身和事件连线尚未单独分析，生成代码依赖工程设计时加载/正常构建产物，source generator 的完整覆盖未验收。

引用暂不区分读/写。动态分派、反射、运行时委托目标和指针/别名流不做完整推断。项目加载仍串行；图每种语言最多 81 节点/160 条合并线，导出展示上限 128 MiB，源码上限 2 MiB。布局不持久化。新增且尚未加载的文件需要下一次刷新确认，尚无后台监控。共享数据库和 C++/C# 混合文件夹已经接入，但同一混合 .sln 的本机 C++ MSBuild 自动转换、跨语言调用、所有解决方案同时分析仍未实现。
