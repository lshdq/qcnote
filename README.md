# qcnote

轻量级 Windows 纯文本编辑器，版本 1.0.0。

纯 Win32 API + Scintilla 5.6.9 静态链接 + C++（MSVC，`/std:c++latest` 即 C++23 语义，`/MT` 静态 CRT），产物为**单个 exe（约 1.7 MB）**：免安装、无任何外部 DLL 依赖、空闲内存 6 MB、热启动 35 ms。由 Qt 6 Quick/QML 版全量重写而来（动机与指标对照见下文；历史源码在 git 历史）。

## 功能特性

完整需求与对等语义见 `docs/需求文档/需求-Win32全量重写.md`。

### 文档与标签

- 多标签：新建 Ctrl+N（未命名-N 递增）/ 打开 Ctrl+O（多选、重复路径激活已有标签）/ 保存 Ctrl+S / 另存为 Ctrl+Shift+S / 关闭标签 Ctrl+W / Ctrl+Tab、Ctrl+Shift+Tab 循环切换
- Win11 连通式标签条：活动 Tab 顶圆角卡片与工具栏同色、非活动悬停高亮、竖分隔线；未保存 `●` 标记；超长省略 + tooltip 完整名；溢出横向滚动；键盘导航（方向键 / Enter / Menu）
- 标签右键菜单：关闭当前 / 右边所有 / 左边所有 / 其他文件（右键不激活标签，批量关闭后回到锚点标签）
- 关闭三选确认：保存 / 不保存（同时清理草稿）/ 取消
- 欢迎页：无文档时显示，含「打开文件…」按钮

### 编辑

- 编码与换行：UTF-8（读容忍 BOM，写回原样）与 GBK（CP936）自动识别（UTF-8 BOM → 严格 UTF-8 校验 → GBK）；「通过编码重新加载 / 通过编码保存」一步转换；GBK 不可编码字符保存前报错（含字符与行号）；LF / CRLF 换行风格按文档记忆并在保存时写回，新文档 LF
- 自动换行：视图菜单开关；关闭时长行横向滚动
- 多光标列编辑：Ctrl+Alt+↑/↓ 添加光标、Alt+单击、Alt+Shift+拖拽矩形列选、Ctrl+Alt+Shift+方向键盘列选、Shift+Alt+I 选中行行尾插光标、Ctrl+D 依次选中相同词、Ctrl+Shift+L 全部相同；全光标编辑合并为单次撤销；Esc 退出保留最末光标；状态栏显示「光标 ×N」；与查找互斥
- 查找替换：Ctrl+F / Ctrl+H 同一模态对话框；实时全部匹配高亮与「第 N 个 / 共 M 个」计数；三开关（区分大小写 / 全字匹配 / 正则表达式，正则为 PCRE 语义）；Enter / Shift+Enter 环绕跳转；F3 / Shift+F3 关闭态沿用参数；替换当前后自动跳下一个；全部替换单次撤销；捕获组 `$1..$99` / `\1..\9`、`$$` 字面 `$`；零长度匹配自动跳过

> 快捷键备注：Windows 显卡驱动可能占用 `Ctrl+Alt+方向键`（如 Intel 核显的旋转屏幕热键），导致添加光标快捷键无效；这属于系统级热键冲突，可在显卡控制面板中关闭对应热键后正常使用。

### 会话与草稿

- 会话恢复：文档列表（顺序 / 路径 / 光标 / 编码）+ 活动标签 + 窗口几何，500 ms 防抖写盘；启动按原顺序恢复，缺失条目静默跳过
- 关闭现场还原：未命名文档按标签栏位置恢复（空标签不恢复）
- 草稿缓存：脏文档内容 1 s 防抖落盘 + 失焦即写；恢复时磁盘哈希一致则静默应用为未保存修改，不符则弹出冲突汇总对话框（恢复 / 丢弃）
- 热退出：X / Alt+F4 / 菜单「退出」直接退出免提示，未保存修改经草稿保留，下次启动恢复

### 平台集成

- 单实例：二次启动将命令行参数转发已运行实例（`WM_COPYDATA`）后退出；IPC 不可用时自动降级多实例
- 命令行位置参数：启动时打开指定文件（可多个、可含空格/中文）
- shell 集成：资源管理器右键「用 qcnote 打开」（注册表 `"<exe>" "%1"`）
- 便携模式：exe 旁放置 `qcnote.portable` 标记文件，会话 ini 与 drafts 目录均存 exe 旁
- 应用图标：exe 嵌入多尺寸 ico（16/24/32/48/64/256）

### 外观

- 一体化无边框标题栏：菜单按钮 → Tab 条 → 最小化/最大化/关闭；拖动移动、双击最大化、最大化时拖拽还原（光标比例锚定）、8 向边缘缩放；Win+方向贴靠、Aero Snap、Win11 Snap Layouts（最大化按钮悬停）；Per-Monitor V2 DPI；右键标题栏系统菜单
- 窗口边界线：活动窗口四边 1px 描边（非活动变淡），最大化时隐藏
- 工具栏（新建/打开/保存）与状态栏（光标行列、选中/总字符数、编码、换行风格、多光标 ×N、文件完整路径 tooltip）
- 弹窗可拖动：应用内自绘弹窗标题栏把手拖动、边界约束、默认位置复位
- 浅色 / 深色双主题：视图菜单「深色主题」即时生效；非客户区（标题栏 / 滚动条 / IME 候选窗）经 DWM 联动暗色

### 架构扩展点（本期不实现，接口已预留）

- **format 层**：文档格式抽象 + 纯文本默认兜底；未来 Markdown / JSON 在同层新增实现注册
- **storage 层**：存储抽象按 URL scheme 分发；`file://` 由本地存储实现，未来网络存储同层新增

## 性能指标

数据来自 `docs/测试报告/指标终验报告-Win32全量重写-20261111.md`（口径：任务管理器默认「内存」列 = 私有工作集）：

| 指标 | 目标 | 实测（M6 终验） | 结论 |
|------|------|----------------|------|
| 空闲内存（空壳，静置 5s） | < 10 MB | **6 MB** | ✅ |
| 空闲内存（开 3 个中文笔记） | ≤ 30 MB | **6.4 MB** | ✅ |
| 启动时间（热启动中值） | < 200 ms | **35 ms**（5 次：39/31/35/38/23） | ✅ |
| 启动时间（冷启动） | < 200 ms | 常态远低于 200 ms（重启后首次启动含杀软扫描为人工项） | 人工项 |
| 部署 | 单 exe 免安装 | `dumpbin /dependents` 全部系统 DLL；`/MT` 静态 CRT | ✅ |
| 产物大小 | — | **1,706.5 KB**（Release `/O1`） | — |

对照 Qt Quick 旧版：空闲内存 60~100 MB → 6 MB（约 1/10~1/16）；启动 0.5 s+ → 35 ms（约 1/14）；无 GPU 场景图依赖（纯 GDI/GDI+ + DWM 合成）。

## 构建指南

### 环境要求

- Windows 10/11 x64
- VS Build Tools 2026（C++ 工具链 + Windows SDK；本项目实例安装于 `d:\Program Files\Microsoft Visual Studio\18\BuildTools`）
- CMake ≥ 3.21 与 Ninja（本项目复用 `D:\Program\Qt\Tools\CMake_64` 与 `D:\Program\Qt\Tools\Ninja`）

### 构建步骤（pwsh）

```powershell
# 1) 导入 MSVC 环境（vcvars64 环境变量导入当前 pwsh 会话）
cmd /c '"d:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && set' | ForEach-Object {
    if ($_ -match '^(.*?)=(.*)$') { Set-Content -Path "env:\$($Matches[1])" -Value $Matches[2] }
}
$env:PATH = "D:\Program\Qt\Tools\CMake_64\bin;D:\Program\Qt\Tools\Ninja;" + $env:PATH

# 2) 配置 + 构建（Release；产物 build-win\qcnote.exe）
cmake -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win

# 3) 单元测试（8 套 Catch2）
ctest --test-dir build-win --output-on-failure
```

运行：

```powershell
.\build-win\qcnote.exe [文件路径...]
```

## 测试

### 单元测试（ctest）

8 套 Catch2 测试套：`smoke` / `core`（document / encoding / storage）/ `ui` / `menu` / `tabs` / `search` / `session` / `platform`。

### selftest / e2e 脚本（pwsh）

进程级界面验证，默认使用 `build-win\qcnote.exe`：

```powershell
pwsh -NoProfile -File tests\verify-menu-selftest.ps1          # 菜单系统（三级级联/键盘导航/焦点）
pwsh -NoProfile -File tests\verify-tabs-selftest.ps1          # 标签条（宽度自适应/溢出/键盘）
pwsh -NoProfile -File tests\verify-doc-selftest.ps1           # 文档全链路（打开/保存/编码/换行）
pwsh -NoProfile -File tests\verify-search-selftest.ps1        # 查找替换（22 场景）
pwsh -NoProfile -File tests\verify-theme-selftest.ps1         # 双主题（色板/切换/非客户区）
pwsh -NoProfile -File tests\verify-instance-shell-e2e.ps1     # 单实例 + shell 集成 e2e
pwsh -NoProfile -File tests\verify-session-draft-e2e.ps1      # 会话/草稿/热退出 e2e
```

### 冒烟与指标

```powershell
pwsh -NoProfile -File tools\verify-smoke.ps1                                # 启动/退出冒烟
pwsh -NoProfile -File tools\verify-metrics.ps1 -ExePath build-win\qcnote.exe # 空壳内存 + 热启动
```

### 测试防扰约定（重要）

自动化测试与测量**一律不把窗口置于前台、不抢占用户焦点**：

- selftest / e2e / 冒烟脚本均以 `--headless` 启动被测程序：窗口完全不显示、无任务栏项、不激活
- 指标测量用 `--noactivate`（`SW_SHOWNOACTIVATE`）：窗口可见以保证计时真实，但不抢前台焦点
- 仅当用户明确要求肉眼验证界面时，才正常可见启动

## 目录结构

```
src/
├── main.cpp            # WinMain 入口：窗口/DPI/消息循环/命令行/生命周期编排
├── core/               # 纯逻辑层（不依赖 UI）：document / documentmanager / storage /
│                       # encoding / format / textutil / searchengine / multicursor /
│                       # sessionmanager / draftcachemanager / inifile
├── editor/             # Scintilla 编辑层：scintillaedit（每文档一实例）、
│                       # multicursorcontroller、searchcontroller
├── ui/                 # 纯 Win32 自绘 UI：titlebar / menu / mainmenu / tabs / welcome /
│                       # toolbar / statusbar / dialog / searchdialog / tooltip / theme
├── platform/           # 平台集成：singleinstance（WM_COPYDATA 转发）、shellintegration
└── icons/              # 应用图标资产（qcnote.ico / qcnote.svg）
vendor/                 # vendored 第三方源码（见下表）
tests/                  # 8 套 Catch2 单元测试 + 7 个 verify-*.ps1 selftest/e2e 脚本
tools/                  # verify-smoke.ps1 / verify-metrics.ps1
build-win/              # MSVC/Ninja 构建目录（产物 qcnote.exe）
build/                  # 旧 Qt 版历史构建产物（保留不动）
docs/                   # 需求文档 / 测试用例 / 决策项 / 审查记录 / 测试报告
```

## 第三方依赖（vendored）

全部依赖以源码形式入库（`vendor/`），不引入包管理器，无运行时外部 DLL：

| 依赖 | 版本 / 快照 | 来源 | 许可 | 用途 |
|------|------------|------|------|------|
| Scintilla | 5.6.9（version.txt=569） | Notepad++ GitHub 仓库稀疏检出，2026-11-10 master 快照（commit `61e17dde`） | Scintilla 历史许可（见 `vendor/scintilla/License.txt`） | 编辑组件（静态链接，排除 DLL 入口） |
| WIL | commit `abb1bd35` | microsoft/wil | MIT | Win32 RAII 头文件库（header-only） |
| Catch2 | v3.8.1 | catchorg/Catch2 | BSL-1.0 | 单元测试框架 |
| PCRE2 | 10.45 | pcre2project/pcre2 | BSD-3-Clause | 正则查找替换（UTF 模式） |
| nlohmann/json | v3.12.0 | nlohmann/json | MIT | 草稿 JSON 序列化（单头） |

## 数据落点与便携模式

### 安装模式（默认）

| 数据 | 位置 |
|------|------|
| 会话 | `%LOCALAPPDATA%\qcnote\qcnote.ini`（500 ms 防抖写盘） |
| 草稿 | `%LOCALAPPDATA%\qcnote\drafts\<uid>.json`（每文档一条，1 s 防抖 + 失焦即写） |

### 便携模式

exe 旁放置空标记文件 `qcnote.portable`：`qcnote.ini` 与 `drafts\` 均存 exe 所在目录。便携/安装模式切换后旧数据自然失联，不做自动迁移。

### 从旧 Qt 版迁移（DEC-014）

| 数据 | 旧 Qt 版位置 | 新版位置 | 迁移方法 |
|------|-------------|---------|---------|
| 会话 ini | `%APPDATA%\qcnote\qcnote.ini`（QSettings UserScope = Roaming） | `%LOCALAPPDATA%\qcnote\qcnote.ini` | INI 格式**字节级兼容**（键转义 / `file:///` URL 双形态读取），手动复制 `qcnote.ini` 到新位置即恢复会话（标签顺序 / 光标 / 窗口几何 / 编码记忆） |
| 草稿 | `%LOCALAPPDATA%\qcnote\qcnote\drafts\`（Qt AppLocalDataLocation 多一层 org\app） | `%LOCALAPPDATA%\qcnote\drafts\` | 目录层级不同，**不自动继承**；手动复制 `*.json` 条目到新目录即可恢复未保存草稿（JSON 字段 / version 1 / 基线哈希算法完全兼容） |

两种数据的手动复制互不影响：只复制 ini 恢复会话，草稿缺失视为无草稿正常启动。

## 已知限制

- Win10 1809（build 17763）之前的系统 DWM 暗色属性不存在：深色主题下系统标题栏 / 滚动条 / IME 候选窗不联动变暗（应用内自绘部分正常；实现先试属性 id 20 再回退 19，更旧系统静默无视觉影响）
- 多光标时 IME 预编辑（组合中字符串）在**所有光标处**显示（旧 Qt 版仅主光标处）；属视觉差异非功能缺陷——提交后每个光标正确上屏，单光标行为与旧版一致
- Tab 超长省略的 tooltip 边框为系统绘制的 1 px 细线，不可定制（背景 / 文字颜色已随主题定制）
- 开启「自动换行」后矩形列选行为退化（Scintilla 矩形选区不支持折行显示；默认 NoWrap 无影响）
- 大文件（> 10 MB）不做性能优化，允许卡顿，不作为缺陷
- 编码仅 UTF-8 / GBK（CP936），暂无其他编码
- 用户可见字符串直接写中文，不做 i18n 抽取

## 历史：Qt 6 时代版本

1.0.0 之前 qcnote 为 Qt 6.11 Quick/QML 实现（MinGW 64-bit），因 QML 引擎 + GPU 场景图的固有基线成本（空闲 60~100 MB、启动 0.5 s+）全量重写为纯 Win32 栈。历史源码完整保留在 git 历史（重写起点为 commit `4e313f0` 及之前）；根目录 `build/` 为旧 Qt 构建产物、Qt 工具链（`D:\Program\Qt\6.11.2\mingw_64`）保留未动，如需考古或对比可 checkout 历史 commit 后用 Qt 工具链重新构建。
