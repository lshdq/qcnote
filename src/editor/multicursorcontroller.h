// qcnote 多光标 Scintilla 交互映射层（M3-B，B4）。
//
// 职责边界（与 core/multicursor 纯逻辑的三层模式，对齐旧版
// MultiCursorController + MultiCursorEngine）：
//   - core/multicursor 只计算「目标光标/选区集合」（字节偏移）；
//   - 本层做交互映射：子类化 Scintilla 子窗口拦截 Alt 系鼠标与多光标
//     触发键，把核心层结果经 SCI_SETSELECTION/ADDSELECTION 应用到
//     Scintilla 原生多选区（渲染/同步编辑/撤销合并全由 Scintilla 承担）；
//   - 组装层（main.cpp）经 EditorHost 的 onWindowCreated/Destroyed 挂接。
//
// 旧语义 → Scintilla 映射（详表见 M3-B 报告）：
//   Alt+单击加光标      → 拦截 WM_LBUTTONDOWN（wParam MK_ALT，无 Shift）→
//                          SCI_POSITIONFROMPOINTCLOSE + 集合重建（去重后
//                          SETSELECTION+ADDSELECTION，主光标=位置最末，与
//                          旧版 primary 语义一致）
//   Alt+Shift+拖拽列选  → 放行原生（ButtonDownWithModifiers 对 Alt 键置
//                          selType=rectangle，拖拽/自动滚动/逐行视觉展开/
//                          短行钳制全原生；中文全角对齐由
//                          SetRectangularRange 的视觉 x 命中保证）；
//                          WM_LBUTTONUP 时把矩形选区转普通多选区
//                          （SETSELECTIONMODE(STREAM)+CANCEL，对齐旧版
//                          finishColumnSelect「定格为普通多选区」——否则原生
//                          Enter/方向键会 DropAdditionalRanges 塌缩）
//   Ctrl+Alt+↑/↓        → core::multicursorAddLine → 全量重设 selections
//   Ctrl+Alt+Shift+方向  → 键盘列选：↑/↓ 放行原生 SCI_LINE*RECTEXTEND
//                          （lastXChosen 即期望 x 粘性）；←/→ 自实现
//                          「字符步进不跨行」（原生 CharLeft/RightRectExtend
//                          会跨行），步进后 SETRECTANGULARSELECTIONCARET +
//                          把主选区指到活动角（使 CHOOSECARETX 更新的
//                          lastXChosen 取活动角 x，粘性正确传递给后续原生
//                          ↑/↓）+ SCI_SCROLLRANGE 保持两角可见
//   Shift+Alt+I         → core::multicursorLineEnds 行尾集合
//   Ctrl+D              → 无选区：每光标各自选词（wordRangeAt）；有选区：
//                          core::multicursorNextOccurrence 追加（环绕、严格
//                          重叠跳过）
//   Ctrl+Shift+L        → core::multicursorAllOccurrences
//   Esc（多光标激活）    → core::multicursorCollapse 收缩末光标
//   Ctrl+C/X（多选区）  → 自定义剪贴板：按位置排序后每选区一行 \n 拼接
//                          （空选区光标贡献空行；不写 cfColumnSelect 标记——
//                          原生复制矩形选区会标记列剪贴板，导致单光标粘贴
//                          触发 PasteRectangular，偏离旧版纯文本语义）
//   Ctrl+V（多选区）    → core::multicursorPasteDistributes 判定：行数==光标
//                          数逐行分发（剪贴板行拆分 + 每选区替换，单批撤销）；
//                          否则每光标整份替换（BEGINUNDOACTION 合并）
//   键入/Backspace/Delete/Enter/方向/Home/End → 原生（依赖
//                          SCI_SETADDITIONALSELECTIONTYPING(true)，逐光标
//                          执行且单批撤销；矩形选区例外：先转普通多选区）
//   IME 提交分发        → 原生：WM_IME_COMPOSITION(GCS_RESULTSTR) →
//                          AddWString → InsertCharacter 与键入同一代码路径，
//                          分发所有选区并单批撤销（源码级验证 + 自测注入
//                          WM_CHAR 等价路径断言）；组合窗/预编辑由原生
//                          inline IME 呈现
//
// 单批撤销：本层所有批量编辑（粘贴分发/剪切删除）显式包
// SCI_BEGINUNDOACTION/ENDUNDOACTION；键入/Enter 等原生路径由 Scintilla
// UndoGroup 内建合并——一次按键一步回滚（B4）。
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "core/multicursor.h"

namespace editor {

class MultiCursorHost {
public:
    MultiCursorHost() = default;
    ~MultiCursorHost();

    MultiCursorHost(const MultiCursorHost&) = delete;
    MultiCursorHost& operator=(const MultiCursorHost&) = delete;

    // 子窗口生命周期（EditorHost onWindowCreated/Destroyed 挂接）
    void attachWindow(HWND sci);
    void detachWindow(HWND sci);
    void detachAll();
    bool owns(HWND sci) const;

    // —— 交互入口（子类化消息的真实路径；mod 显式传入便于自测注入）——

    // 键盘拦截：返回 true = 已消费（不透传 Scintilla）
    bool handleKeyDown(HWND sci, UINT vk, bool ctrl, bool shift, bool alt);

    // 鼠标左键按下拦截（wParamFlags = WM_LBUTTONDOWN 的 wParam，真实与
    // 合成消息均携带 MK_ALT/MK_SHIFT）：返回 true = 已消费
    bool handleMouseButtonDown(HWND sci, UINT wParamFlags, POINT pt);

    // 鼠标左键抬起：矩形选区 → 普通多选区（列选定格，旧版语义）
    void handleMouseButtonUp(HWND sci);

    // DEC-008：打开查找对话框前退出多光标（收缩末光标，同 Esc）
    void collapseIfActive(HWND sci);

    // 多光标激活（选区数 > 1）
    static bool active(HWND sci);

    // —— 自动化测试直调入口（显式修饰键；与真实路径同一内部函数）——
    void testKeyDown(HWND sci, UINT vk, bool ctrl, bool shift, bool alt) {
        handleKeyDown(sci, vk, ctrl, shift, alt);
    }
    void testAltClick(HWND sci, POINT pt) {
        handleMouseButtonDown(sci, MK_ALT, pt);
        handleMouseButtonUp(sci);
    }
    void testPaste(HWND sci);  // Ctrl+V 多光标粘贴路径（读注入/系统剪贴板）

    // 剪贴板存取注入（自动化测试用内存实现规避系统剪贴板可用性抖动；
    // 空指针恢复系统实现——真实用户路径不变）
    void setClipboardAccessForTest(std::function<std::string(HWND)> read,
                                   std::function<bool(HWND, const std::string&)> write);

    // 状态栏刷新挂点（集合重建后调用；常规键入/移动经 SCN_UPDATEUI 已覆盖）
    std::function<void(HWND)> onSelectionsRebuilt;

private:
    std::vector<HWND> windows_;
    // 剪贴板存取（默认系统实现；测试可注入内存实现）
    std::function<std::string(HWND)> readClipboard_ = nullptr;
    std::function<bool(HWND, const std::string&)> writeClipboard_ = nullptr;

    std::string clipboardRead(HWND owner);
    bool clipboardWrite(HWND owner, const std::string& utf8);
};

// —— 共享工具（供查找控制器等复用）——

// 收集 Scintilla 全部选区（按 (caret, anchor) 升序；矩形选区取逐行展开结果）
std::vector<core::SelRange> CollectSelections(HWND sci);

// 按升序全量重设 selections（SETSELECTION+ADDSELECTION；先转普通流式
// 多选区并清 moveExtends；主光标 = 位置最末，对齐旧版 primary 语义）
void ApplySelections(HWND sci, const std::vector<core::SelRange>& selections);

// 活动文档当前不含换行的选区文本（无选区/多行选区返回空；供查找预填）
std::string SelectionTextForSearch(HWND sci);

}  // namespace editor
