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
//   Alt+单击加光标      → 拦截 WM_LBUTTONDOWN（Alt 读物理键盘状态
//                          GetKeyState(VK_MENU)，DEC-007：真实消息 wParam
//                          无 Alt 位——0x20 是 MK_XBUTTON1；无 Shift，Shift
//                          仍读 wParam 合法标志）→
//                          SCI_POSITIONFROMPOINTCLOSE + 集合重建（去重后
//                          SETSELECTION+ADDSELECTION，主光标=位置最末，与
//                          旧版 primary 语义一致）
//   Alt+Shift+拖拽列选  → 放行原生，但按下转发前剥掉 wParam 的 MK_SHIFT
//                          （NativeMouseDownFlags）——本层与 Scintilla 的 Alt
//                          修饰键均从物理键盘状态读取（GetKeyState(VK_MENU)，
//                          ScintillaWin.cxx MouseModifiers 同此；DEC-007），
//                          Ctrl/Shift 读 wParam（真实消息合法标志），剥 Shift
//                          后矩形模式仍生效，且
//                          ButtonDownWithModifiers 走「新建空矩形（锚=游标=
//                          按下点，Editor.cxx ~4974-4997）」分支，对齐旧版
//                          columnDragBeginAt（锚=游标=按下点），不再执行普通
//                          Shift+click 的 SetSelection(newPos) 扩展语义；
//                          拖动路径 ButtonMoveWithModifiers 不读 Shift，拖拽/
//                          自动滚动/逐行视觉展开全原生；中文全角对齐由
//                          SetRectangularRange 的视觉 x 命中保证；空行/短行
//                          终点跟随鼠标 x 走矩形虚拟列（SCVS_RECTANGULAR-
//                          SELECTION，见 scintillaedit.cpp configure），定格
//                          后虚拟列保留；WM_LBUTTONUP 先放行原生 ButtonUp
//                          （矩形态下主选区重写/全行重建带虚拟列）再把矩形
//                          选区转普通多选区（SETSELECTIONMODE(STREAM)+CANCEL，
//                          对齐旧版 finishColumnSelect「定格为普通多选区」——
//                          否则原生 Enter/方向键会 DropAdditionalRanges 塌缩）
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

#include "Scintilla.h"
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

    // 鼠标左键按下拦截（wParamFlags = 消息 wParam；MK_CONTROL/MK_SHIFT 为
    // 真实消息合法标志照常解析，Alt 不经 wParam——wParam 的 0x20 位实为
    // MK_XBUTTON1 侧键（WinUser.h:2735），MK_ALT 属 OLE 拖放协议常量
    //（oleidl.h），Alt 一律读物理键盘状态 GetKeyState(VK_MENU)（MSDN
    // WM_LBUTTONDOWN Remarks，DEC-007））：返回 true = 已消费
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
        // DEC-007：Alt 经物理键状态判定——钩子内以 GetKeyboardState/
        // SetKeyboardState 置本线程键状态表 Alt down（不注入系统输入流，
        // 防扰约定；main.cpp 列选自测同款先例），调用后恢复原状态，使直调
        // 路径与真实 Alt+单击输入形态重合；wParamFlags 用真实形态 MK_LBUTTON
        //（无 0x20——真实消息该位是 MK_XBUTTON1）
        BYTE state[256] = {}, saved[256] = {};
        ::GetKeyboardState(saved);
        for (int i = 0; i < 256; ++i) state[i] = saved[i];
        state[VK_MENU] |= 0x80;
        ::SetKeyboardState(state);
        handleMouseButtonDown(sci, MK_LBUTTON, pt);
        handleMouseButtonUp(sci);
        ::SetKeyboardState(saved);
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

// 虚拟空间选项唯一口径（C-1）：scintillaedit.cpp configure 与键盘列选瞬态
// 恢复点（multicursorcontroller.cpp handleKeyDown ↑/↓ 分支）共用同一常量，
// 防两处恢复值漂移（DEC-003 只启用矩形选区虚拟空间；DEC-006 瞬态
// USERACCESSIBLE 用后必须恢复为本值）
constexpr int kVirtualSpaceOptions = SCVS_RECTANGULARSELECTION;

// 列选拖拽按下转发前的 wParam 调整（纯函数，不读键状态，自测断言覆盖；
// alt/ctrl 由调用方传入——alt 为物理键盘状态判定结果，DEC-007）：
// (alt && !ctrl) 时返回 wParamFlags & ~MK_SHIFT（其余位保留），否则原样返回。
// 为什么只剥 wParam 的 MK_SHIFT：Scintilla 的 MouseModifiers 中 Shift/Ctrl
// 读 wParam、Alt 读物理键盘状态（ScintillaWin.cxx:1142
// KeyboardIsKeyDown(VK_MENU)），剥掉 wParam 的 MK_SHIFT 后矩形模式仍生效，
// 且 ButtonDownWithModifiers（Editor.cxx ~4974-4997）走「新建空矩形（锚=
// 游标=按下点）」分支——不再执行普通 Shift+click 的 SetSelection(newPos)
// 扩展语义（按下即拉出旧选区的根因）。已知权衡（DEC-001）：Alt+Shift+click
// 落在已有非空选区内时原生走选区拖放分支而非新建矩形（罕见路径，接受）；
// Ctrl+Alt+Shift 属未定义手势不剥（DEC-002，行为与修复前一致）。
UINT NativeMouseDownFlags(UINT wParamFlags, bool alt, bool ctrl);

// 收集 Scintilla 全部选区（按 (caret, anchor) 升序；矩形选区取逐行展开结果）
std::vector<core::SelRange> CollectSelections(HWND sci);

// 按升序全量重设 selections（SETSELECTION+ADDSELECTION；先转普通流式
// 多选区并清 moveExtends；主光标 = 位置最末，对齐旧版 primary 语义）
void ApplySelections(HWND sci, const std::vector<core::SelRange>& selections);

// 活动文档当前不含换行的选区文本（无选区/多行选区返回空；供查找预填）
std::string SelectionTextForSearch(HWND sci);

}  // namespace editor
