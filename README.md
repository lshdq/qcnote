# qcnote

Qt 6 + QML 的 Windows 桌面文本编辑器。当前为**纯文本 MVP**，按扩展点设计逐步演进。

## 功能

- 多标签文档：新建 / 打开 / 保存 / 另存为 / 关闭标签，快捷键 Ctrl+N / O / S、Ctrl+Shift+S、Ctrl+W、Ctrl+Tab
- 未保存保护：Tab `●` 标记 + 标题 `*` 标记；关闭标签 / 退出时三选确认（保存 / 放弃 / 取消）
- 纯文本编辑：等宽字体、Tab 缩进、撤销/重做
- 编码与换行：UTF-8（读取容忍 BOM，写出无 BOM）；自动识别并保留文件原换行风格（LF / CRLF）
- 自绘 Frameless 标题栏：主菜单 + 文档 Tab 区 + 窗口控制按钮；拖拽移动、双击最大化、边缘缩放，保留系统贴靠（Win+方向）
- 主题：AppTheme 语义 token，浅色 / 深色一键切换（菜单 → 查看 → 深色主题）
- 状态栏：光标行列、选中/总字符数、编码、换行风格、文件路径

## 构建与运行

环境：Qt 6.11.2（MinGW 64-bit）、CMake、Ninja（详细路径见 `.opencode/AGENTS.md`）。

```powershell
$env:PATH = "D:\Program\Qt\Tools\CMake_64\bin;D:\Program\Qt\Tools\Ninja;" + $env:PATH
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=D:/Program/Qt/6.11.2/mingw_64 -DCMAKE_C_COMPILER=D:/Program/Qt/Tools/mingw1310_64/bin/gcc.exe -DCMAKE_CXX_COMPILER=D:/Program/Qt/Tools/mingw1310_64/bin/g++.exe
cmake --build build
ctest --test-dir build --output-on-failure
```

运行：

```powershell
$env:PATH = "D:\Program\Qt\6.11.2\mingw_64\bin;D:\Program\Qt\Tools\mingw1310_64\bin;" + $env:PATH
.\build\qcnote.exe
```

## 目录结构

```
src/
├── main.cpp            # 入口（Material 风格）
├── document/           # Document、DocumentManager（QML 单例）
├── format/             # DocumentFormat 抽象 + PlainTextFormat（格式扩展点）
├── storage/            # DocumentStorage 抽象 + LocalStorage（存储扩展点）
└── qml/                # QML 界面与 AppTheme 主题 token
tests/test_core/        # QtTest 单元测试（C++ 核心）
docs/                   # 需求 / 测试用例 / 决策 / 审查记录
```

## 扩展点（演进路线）

- **新格式**（Markdown、JSON…）：在 `src/format/` 新增 `DocumentFormat` 派生类并注册到 `DocumentManager`；未知扩展名自动兜底纯文本
- **新存储**（网络保存 / 获取）：在 `src/storage/` 新增 `DocumentStorage` 派生类并按 URL scheme 注册；`file://` 始终走本地

## 已知限制（MVP）

- Win11 悬停最大化按钮不弹出 Snap Layouts（自绘按钮限制；Win+方向贴靠可用）
- Frameless 窗口无系统阴影
- 仅 UTF-8（暂无 GBK 探测）；\>10MB 大文件无性能优化
