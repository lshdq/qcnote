# 测试报告：显示行号（DPI 修复后回归复测）

- 日期：2026-11-19
- 被测制品：`build-win\qcnote.exe`（基线 90de232b + 未提交「显示行号」全部改动 10 文件 +160/−11，叠加 DPI 修复：`scintillaedit` 新增 `refreshLineNumberWidths()`、`main.cpp` WM_DPICHANGED 段经 `kMsgRefreshLineNumbers=WM_APP+11` 延迟一拍调用）
- 场景：业务代码回归复测（Auto 模式，全部 headless，遵守测试防扰约定）
- 本轮不新增用例，仅复测

## 单元测试

- ctest 8 套 Catch2：**8/8 通过，0 失败**（smoke/core/ui/menu/tabs/search/session/platform，总耗时 2.91s）

## 专项与回归 selftest/e2e（均 headless）

| 用例/脚本 | 结果 | 备注 |
|----------|------|------|
| tests\verify-linenumbers-selftest.ps1 | 通过 | 31/31（含 TC-01~07：默认显示、菜单开关、全局生效、宽度自适应、会话持久化两态、旧 ini 前向兼容、双主题配色） |
| tests\verify-menu-selftest.ps1 | 通过 | 全场景 PASS |
| tests\verify-doc-selftest.ps1 | 通过 | 全场景 PASS |
| tests\verify-theme-selftest.ps1 | 通过 | 双主题 + ini 记录/重启恢复 PASS |
| tests\verify-session-draft-e2e.ps1 | 通过 | 26/26 |
| tests\verify-edge-hittest-e2e.ps1 | 通过 | 含 wordWrap 重启恢复 PASS |
| tests\verify-instance-shell-e2e.ps1 | 通过 | 25/25，单实例/WM_COPYDATA 参数转发不受 WM_APP+11 新占用影响 |

## DPI 修复专项验证（本轮补充，inline headless 驱动）

- 方法：`--headless` 启动 → 枚举 `QcnoteMainWindow` → 经 `kMsgSelftestCommand(0x8007)` 发 `NewDocument(=1)` 获取 Scintilla 子窗口 → 向主窗口连续投递 `kMsgRefreshLineNumbers(0x800B)` ×10 → 复查。
- 结果：投递前 type=1(SC_MARGIN_NUMBER) width=24；投递后进程存活、type=1 width=24，**无崩溃、状态稳定 → PASS**。
- 说明：WM_DPICHANGED 本身无法在 headless 自动化中安全触发（需真实 DPI 迁移窗口），仅验证刷新消息路径安全幂等。

## 测试代码变更

- 无（未新增/修改任何测试文件；DPI 专项验证为一次性 inline 驱动，复用既有 headless 钩子，未改动生产代码与测试代码）。

## 总结

- 通过率：100%（8 套单测 + 7 个 selftest/e2e + DPI 专项验证全部通过）
- 失败用例：无
- 阻塞项：无
