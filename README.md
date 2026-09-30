# vividcode

面向代码理解的桌面分析与可视化工具，使用 C++ / Qt 构建界面，使用独立 Python / libclang 分析进程解析 C++。

## 当前能力

- 浏览 C/C++、C#、Python、JavaScript/TypeScript 和 Java 源文件。
- 导入 C++ 编译数据库，查看类、成员、函数、继承、直接调用和成员引用。
- 在关系图中缩放展开类成员，点击符号或关系位置定位源码。
- 使用 `vividcode --g` 初始化项目运行目录、`--c` 清理旧记录、`--d` 删除生成目录。

其它语言目前仅支持文件浏览。C++ 关系图按编译单元和配置展示局部关系；全项目关系图、其它语言语义分析及 AI 修改功能尚未实现。

## 构建与运行

当前主要开发环境为 Windows、Visual Studio 2022 C++ 工具链、Qt 6.11.2 MSVC 64 位和 LLVM/libclang。分析运行包准备脚本使用 Python 3.13.7 embeddable；首次准备需要联网获取 Python，并读取本机 LLVM 文件。

在 PowerShell 中进入源码目录后构建：

```powershell
cd CUGB_Graph
.\build.ps1
```

构建后运行 `CUGB_Graph/build/Release/vividcode-viewer.exe`。本仓库不包含预编译程序、Qt DLL、Python 运行包或本机索引。

构建脚本含本机默认依赖路径，其他电脑需按实际安装位置配置；详细说明见 [源码使用指南](CUGB_Graph/README.md) 与 [构建环境与运行](Document/构建环境与运行.md)。干净机器上的完整构建与分发尚未验证。

## 目录与文档

| 路径 | 用途 |
| --- | --- |
| `CUGB_Graph/` | 已有源码工程、分析器、测试、演示与构建脚本 |
| `Document/` | 功能说明、验证记录与开发交接 |
| `PROJECT_BRIEF.md` | 项目背景、范围与阶段状态 |
| `AGENTS.md` | AI 协作规则 |

完整交接入口为 [文档索引](Document/README.md)。保留历史源码目录名 `CUGB_Graph`，正式产品名为 vividcode。

## 托管与开源计划

远端仓库为 [piger114514/VividCode](https://github.com/piger114514/VividCode)。用户计划当前私人开发，后续准备完成后公开。开源许可证尚未确定；本次未添加许可证。托管操作与验证状态见 [GitHub 托管](Document/GitHub托管.md)。
