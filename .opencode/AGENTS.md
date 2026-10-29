# qcnote 项目指令

## 项目概述

Qt 6 + QML 桌面文本编辑器。演进路线：纯文本（当前）→ Markdown/JSON 格式扩展；本地文件（当前）→ 网络存储扩展。

**禁止在项目根目录创建 AGENTS.md；本文件（.opencode/AGENTS.md）是唯一项目指令文件。**

## 构建与测试（pwsh）

```powershell
$env:PATH = "D:\Program\Qt\Tools\CMake_64\bin;D:\Program\Qt\Tools\Ninja;" + $env:PATH
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=D:/Program/Qt/6.11.2/mingw_64 -DCMAKE_C_COMPILER=D:/Program/Qt/Tools/mingw1310_64/bin/gcc.exe -DCMAKE_CXX_COMPILER=D:/Program/Qt/Tools/mingw1310_64/bin/g++.exe
cmake --build build
ctest --test-dir build --output-on-failure
```

运行（自动化验证**仅用 offscreen**，不产生任何可见窗口、不抢占焦点；**禁止**在自动化验证/冒烟中启动真实窗口——即使最小化也会在任务栏闪现并可能干扰用户；仅当用户明确要求肉眼验证界面时，经用户同意后再最小化启动并尽快关闭）：

```powershell
$env:PATH = "D:\Program\Qt\6.11.2\mingw_64\bin;D:\Program\Qt\Tools\mingw1310_64\bin;" + $env:PATH
$env:QT_QPA_PLATFORM = "offscreen"
Start-Process -FilePath "E:\works\qcnote\build\qcnote.exe"
# 用后结束进程：Get-Process qcnote | Stop-Process -Force
```

## 目录结构与职责

```
src/
├── main.cpp                 # 入口：QGuiApplication + QQuickStyle("Material")
├── document/                # Document（文档对象）、DocumentManager（QML 单例，文档生命周期）
├── format/                  # DocumentFormat 抽象 + PlainTextFormat
├── storage/                 # DocumentStorage 抽象 + LocalStorage（file://）
└── qml/                     # QML 界面；AppTheme.qml 为语义主题 token 单例
tests/test_core/             # QtTest 单元测试（C++ 核心）
docs/                        # 需求文档 / 测试用例 / 决策项 / 审查记录
```

## 扩展约定

- **新增格式**（如 Markdown）：在 `src/format/` 新增 `DocumentFormat` 派生类，并在 `DocumentManager` 注册；未知扩展名兜底为 PlainTextFormat，不得移除该兜底行为。
- **新增存储**（如网络）：在 `src/storage/` 新增 `DocumentStorage` 派生类并按 URL scheme 注册；`file://` 始终由 LocalStorage 处理。
- 换行/编码约定：内容在内存中一律 LF、无 BOM；Document 记录原文件换行风格，LocalStorage 保存时写回；编码当前仅 UTF-8（读容忍 BOM）。

## 编码约定

- C++20；类名 PascalCase，成员函数 camelCase；头文件成对放同名目录。
- C++ 类型通过 `QML_ELEMENT` / `QML_SINGLETON` 注册进 `qcnote` QML 模块；QML 中禁止直接访问 C++ 内部成员，走属性与 `Q_INVOKABLE`。
- QML 颜色一律取自 `AppTheme` 语义 token，禁止散落硬编码色值；状态表达必须有非颜色线索。
- 用户可见字符串 MVP 阶段直接写中文，不做 i18n 抽取。
- UI 结构遵循 desktop-ui-design skill：顶部窗口栏（菜单按钮 → Tab 区 → 窗口控制）、工具栏、内容区、状态栏。

## 提交约定

- 提交信息用中文；前缀：feat / fix / docs / chore / test / refactor。
