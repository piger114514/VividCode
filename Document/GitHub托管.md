# GitHub 托管

更新日期：2026-09-30。

## 范围与入口

- 目标远端：`https://github.com/piger114514/VividCode.git`。
- 本地仓库根目录为课程项目根目录，包含 `VividCode/`、`Document/`、`PROJECT_BRIEF.md`、`AGENTS.md` 与规划文档。
- 根目录 `README.md` 为仓库首页；源码详细使用指南仍为 `VividCode/README.md`。
- 根目录 `.gitignore` 与现有 `VividCode/.gitignore` 共同排除编译输出、IDE 个人配置、Python 缓存、`.work`、`.codegraph` 和 `.vividcode`。

## 用户意图与当前状态

用户要求目前私人托管，后续准备完成后公开开源；尚未确定许可证。不要擅自公开仓库或选择许可证。

已在根目录初始化 Git，分支为 `main`，`origin` 指向目标远端。GitHub API 已确认仓库为 `private`，当前账户具有推送权限。首次提交 `4273fd5` 已推送成功，并通过 `git ls-remote` 确认远端 main 与本地提交一致；本地 main 已跟踪 origin/main。

本次由沙箱建立 `.git`，与 Windows 用户身份不同。已在用户 Git 配置中仅将本项目绝对路径加入 `safe.directory`，便于后续正常使用 Git，未设置通配信任。

## 日常操作

在仓库根目录执行 `git status` 查看修改，核对文件后按路径使用 `git add`、`git commit` 和 `git push`。`.gitignore` 仅影响未跟踪文件，已经提交的文件不会因新增忽略规则自动退出版本管理。

首次推送后，其他机器可克隆远端并参照 `构建环境与运行.md` 准备依赖。仓库不保存可执行文件和依赖运行包；克隆后需构建，原电脑已有输出不会被忽略规则删除。

## 公开前

检查文件及提交历史、选择合适的开源许可证，并核对所分发第三方依赖的许可要求。公开仓库的贡献一般通过 Fork 和 Pull Request 完成，是否合并由维护者决定。

## 验证与限制

本轮只调整托管配置与文档，未修改产品逻辑，未重新构建或运行产品测试。

- 上传清单包含 52 个文件，约 322 KB；源码、分析器、测试、构建脚本与人工文档均在清单中。
- 已通过 `git check-ignore` 核验 `.work`、`.codegraph`、build、x64、`.vs`、`.vividcode`、Python 缓存和 `.vcxproj.user` 被排除。
- 对待上传文件扫描常见 GitHub/API 令牌和私钥头格式，未发现匹配；这不等同于全面的秘密信息审计。
- `git diff --cached --check` 发现原有文件末尾额外空行；保留原文件，禁用该项格式告警后的检查通过，没有为托管任务改动产品源码。
- GitHub 认证已完成，API 确认 `private: true`、`permissions.push: true`，`git ls-remote origin` 确认首次推送前远端无分支。
- 首次推送后，本地 HEAD 与远端 main 同为 `4273fd5e999d0cf1734c060c90e236656698a645`，工作区干净；随后以文档提交记录本次验收结果。
