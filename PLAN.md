# 计划：让 vividcode 读取 C# 和 Python 源码（文件级）

> 历史专项计划：下文“现状”指该改动实施前的状态，不是当前待办。文件级扩展已在项目档案记录完成；其中其他语言分析器的提及仅为当时设想，不代表技术选型已定。2026-09-27 的长期目标、允许混合语言及最新环境状态以 PROJECT_BRIEF.md 和 Document/开发讨论.txt 为准。本文件不作为当前整体开发路线。

## Context

- ividcode 能浏览 C/C++ C# Python JavaScript Java等主流编程语言的源码文件。
- 当前阶段是**文件级浏览**：扫描 → 目录树/卡片 → 只读源码 → 文件级记录（路径/大小/SHA-256）。**尚未**做任何语言的语义解析（类/函数/关系）。

## 现状（已核实）

- `CUGB_Graph/ProjectScanner.h` 第 18 行 `extensions` 只含 `c/cc/cpp/cxx/h/hh/hpp/hxx/inl/ipp`，是“扫哪些文件”的唯一入口。
- 同文件第 19–20 行 `excluded` 只有 C/C++ 生成目录，缺 C#（`bin`/`obj`）和 Python（`__pycache__`/venv/缓存）目录。
- `CUGB_Graph/CUGB_Graph.cpp` 有几处写死 “C++” 的界面文字：窗口标题、初始提示、文件对话框标题、统计标签、卡片“源文件”。
- demo 目录只有三份 C++ 文件；CMake 用 `copy_directory` 把 `demo/` 整体复制到产物目录，新增文件会自动带上。
- `SmokeTest.h` 的断言不受影响：它只创建 `.cpp/.hpp/.h/.txt`，新增扩展名不会改变其结果（`notes.txt` 仍非源码、`build`/`.git` 仍被排除）。

## Approach（本次改动，全部为文件级）

1. `ProjectScanner.h`：扩展名集合加入 `cs`、`py`、`pyw`、`pyi`（不区分大小写）。
2. `ProjectScanner.h`：排除目录加入 C#/Python 常见生成与缓存目录：`bin`、`obj`、`__pycache__`、`.venv`、`venv`、`env`、`.pytest_cache`、`.mypy_cache`、`.tox`、`.idea`。
3. `demo/` 新增 `Greeter.cs` 与 `Greeter.py`，让“打开演示”即可看到效果。
4. 文档同步：`CUGB_Graph/README.md`、`Document/基础文件浏览.md`、`Document/vividcode代码解析.txt`（扩展名与“找源码文件”表述）、`PROJECT_BRIEF.md` 当前状态。`Document/README.md` 索引表无需改。

## Files to modify

- `CUGB_Graph/ProjectScanner.h`
- `CUGB_Graph/CUGB_Graph.cpp`
- `CUGB_Graph/demo/Greeter.cs`（新增）
- `CUGB_Graph/demo/Greeter.py`（新增）
- `CUGB_Graph/README.md`
- `Document/基础文件浏览.md`
- `Document/vividcode代码解析.txt`
- `PROJECT_BRIEF.md`

## Reuse（复用现有实现，不新建结构）

- `scanProject()` 已按扩展名集合 + 排除目录集合工作，只需改集合内容。
- 卡片配色逻辑无需改：`.cs/.py` 自然落入蓝色“源码文件”分支；C++ 头文件仍为绿色。
- CMake `copy_directory` 已自动搬运 `demo/`，无需改 `CMakeLists.txt`。

## Steps

- [x] 1. 改 `ProjectScanner.h`：扩展名加 `cs/py/pyw/pyi`；排除目录加 C#/Python 目录
- [x] 2. 改 `CUGB_Graph.cpp`：5 处界面文字中性化
- [x] 3. 新增 `demo/Greeter.cs`、`demo/Greeter.py`
- [x] 4. 更新 `README.md`、`Document/基础文件浏览.md`、`Document/vividcode代码解析.txt`、`PROJECT_BRIEF.md`
- [x] 5. 构建与验证（见下）

## Verification

- 运行 `CUGB_Graph/build.ps1`（或 CMake 构建）确认 Release x64 编译通过。
- `ctest` 跑 `prototype_smoke` 与 `cli_lifecycle`，确认两组仍通过（SmokeTest 断言不因新增扩展名改变）。
- 手动：运行 `build/Release/vividcode-viewer.exe`，点“打开演示”，应看到 `Greeter.cs`、`Greeter.py` 与三份 C++ 文件；点击任意文件能显示源码。
- 手动：新建一个含 `.cs`/`.py`/`__pycache__`/`bin` 的临时目录并“打开项目”，确认 `.cs`/`.py` 出现在树中、`__pycache__`/`bin` 被排除、统计数字正确。
- 若本机构建环境（Windows SDK）受限导致无法编译，将明确记录“未实际构建”，并把已完成/未完成分清楚。
