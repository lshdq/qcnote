# qcnote 项目指令

## 项目概述

纯 Win32 + Scintilla 5.6.9 静态链接 + C++23（MSVC `/MT`）的 Windows 单 exe 文本编辑器，版本 1.0.0。由 Qt 6 Quick/QML 版全量重写而来（旧源码已删除，在 git 历史可追溯）。演进路线：纯文本（当前）→ Markdown/JSON 格式扩展；本地文件（当前）→ 网络存储扩展。

**禁止在项目根目录创建 AGENTS.md；本文件（.opencode/AGENTS.md）是唯一项目指令文件。**

## 构建与测试（pwsh）

```powershell
# 1) 导入 MSVC 环境（vcvars64 环境变量导入当前 pwsh 会话）
cmd /c '"d:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && set' | ForEach-Object {
    if ($_ -match '^(.*?)=(.*)$') { Set-Content -Path "env:\$($Matches[1])" -Value $Matches[2] }
}
$env:PATH = "D:\Program\Qt\Tools\CMake_64\bin;D:\Program\Qt\Tools\Ninja;" + $env:PATH

# 2) 配置 + 构建（Release；产物 build-win\qcnote.exe）
cmake -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win

# 3) 单元测试（8 套 Catch2：smoke/core/ui/menu/tabs/search/session/platform）
ctest --test-dir build-win --output-on-failure
```

selftest / e2e / 冒烟 / 指标脚本：

```powershell
pwsh -NoProfile -File tests\verify-menu-selftest.ps1           # 菜单
pwsh -NoProfile -File tests\verify-tabs-selftest.ps1           # 标签条
pwsh -NoProfile -File tests\verify-doc-selftest.ps1            # 文档全链路
pwsh -NoProfile -File tests\verify-search-selftest.ps1         # 查找替换
pwsh -NoProfile -File tests\verify-theme-selftest.ps1          # 双主题
pwsh -NoProfile -File tests\verify-instance-shell-e2e.ps1      # 单实例/shell e2e
pwsh -NoProfile -File tests\verify-session-draft-e2e.ps1       # 会话/草稿 e2e
pwsh -NoProfile -File tools\verify-smoke.ps1                   # 冒烟
pwsh -NoProfile -File tools\verify-metrics.ps1 -ExePath build-win\qcnote.exe  # 指标
```

## 测试防扰约定（必须遵守）

自动化测试与测量**一律不把窗口置于前台、不抢占用户焦点**：

- selftest / e2e / 冒烟脚本均以 `--headless` 启动被测程序：窗口不显示、无任务栏项、不激活（exe 已内置该测试钩子）
- 指标测量用 `--noactivate`（`SW_SHOWNOACTIVATE`）：窗口可见保证计时真实，但不抢前台焦点
- **禁止**在自动化验证/冒烟中可见启动真实窗口——即使最小化也会在任务栏闪现并可能干扰用户
- 新增自动化验证脚本必须沿用上述钩子；仅当用户明确要求肉眼验证界面时才可见启动，并尽快关闭

## 目录结构与职责

```
src/
├── main.cpp            # WinMain 入口：窗口/DPI/消息循环/命令行/生命周期编排
├── core/               # 纯逻辑层（不依赖 UI/Win32）：document、documentmanager、
│                       # storage（DocumentStorage 抽象 + localstorage）、encoding、
│                       # format（DocumentFormat 抽象 + PlainTextFormat 兜底）、
│                       # textutil、searchengine、multicursor、sessionmanager、
│                       # draftcachemanager、inifile
├── editor/             # Scintilla 编辑层：scintillaedit（每文档一实例，切换=显示切换，
│                       # 保留撤销历史）、multicursorcontroller、searchcontroller
├── ui/                 # 纯 Win32 自绘 UI：titlebar、menu/mainmenu、tabs、welcome、
│                       # toolbar、statusbar、dialog、searchdialog、tooltip、theme
├── platform/           # 平台集成：singleinstance（WM_COPYDATA 参数转发）、
│                       # shellintegration（资源管理器右键）
└── icons/              # 应用图标资产
vendor/                 # vendored 第三方源码：scintilla 5.6.9、wil、catch2 v3.8.1、
│                       # pcre2 10.45、nlohmann/json v3.12.0（版本与许可见 README）
tests/                  # 8 套 Catch2 单元测试 + 7 个 verify-*.ps1 selftest/e2e 脚本
tools/                  # verify-smoke.ps1 / verify-metrics.ps1
build-win/              # MSVC/Ninja 构建目录（当前产物）
build/                  # 旧 Qt 版历史构建产物（保留不动）
docs/                   # 需求文档 / 测试用例 / 决策项 / 审查记录 / 测试报告
```

## 扩展约定

- **新增格式**（如 Markdown）：在 `src/core/format` 新增 `DocumentFormat` 派生类，并在 `DocumentManager` 注册；未知扩展名兜底为 `PlainTextFormat`，不得移除该兜底行为。
- **新增存储**（如网络）：在 `src/core/storage` 新增 `DocumentStorage` 派生类并按 URL scheme 分发；`file://` 始终由 `LocalStorage` 处理。
- **主题**：`src/ui/theme.h` 定义两套色板（`LightPalette()` / `DarkPalette()`）与 `CurrentPalette()` / `SetTheme()`；QML 时代 AppTheme 语义 token 已移植为 C++ 常量表。颜色一律经 `CurrentPalette()` 取语义 token，禁止散落硬编码色值；状态表达必须有非颜色线索。
- 换行/编码约定：文档内容在内存中一律 UTF-8 `std::string`、LF、无 BOM；`Document` 记录原文件换行风格（CRLF/LF），保存时写回；读 UTF-8 容忍 BOM；GBK（CP936）仅在磁盘读写边界经 UTF-16 一次性转换。会话 ini 落 `%LOCALAPPDATA%\qcnote\`（便携模式为 exe 旁），格式对齐 QSettings 字节级兼容。

## 编码约定

- C++23：项目 TU 用 `/std:c++latest`（本机 cl 19.51 无 `/std:c++23` 开关，latest 实测 `_MSVC_LANG=202400`，完整涵盖 C++23）；Scintilla TU 保持 `/std:c++20` 混编（边界仅 `extern "C"`，无 ODR 问题）；代码避免使用 C++26 独有特性。
- 类名 PascalCase，成员函数 camelCase；头文件成对放同名目录。
- 分层依赖方向：`core`（纯逻辑，可测）← `editor`（Scintilla 封装）← `ui` / `platform` / `main`（Win32）；`core` 不得反向包含 UI/Win32 头。
- 用户可见字符串直接写中文，不做 i18n 抽取。
- UI 结构遵循 desktop-ui-design skill：顶部窗口栏（菜单按钮 → Tab 区 → 窗口控制）、工具栏、内容区、状态栏。

## 提交约定

- 提交信息用中文；前缀：feat / fix / docs / chore / test / refactor。
