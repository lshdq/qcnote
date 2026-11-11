// qcnote Scintilla 编辑器组件（M2-B）：每 Document 一个 Scintilla 子窗口实例。
//
// 方案选型（对比单实例+状态序列化，任务 M2-B 决策）：
//   A. 每 Document 一个 Scintilla 子窗口，切换=ShowWindow 显隐——撤销历史、
//      滚动位置、光标、选区全部天然按文档保留（Notepad++ 同款多实例做法；
//      撤销栈跨文档克隆在 Scintilla 中不可行，SCI_SAVEPOINT/undo 无法复制）。
//      空窗口开销约 100~300KB/实例，多笔记场景可接受（验收：10 文档 ≤30MB）。
//   B. 单实例+切换时序列化内容/光标/滚动——撤销历史丢失，违背旧版
//      「切换标签不丢失未保存内容 + 撤销栈」语义。
//   选 A。旧 QML 版为每文档一个 TextArea（delegate 不销毁），语义一致。
//
// 内容同步（DEC-010：UTF-8 字节流直通，SC_CP_UTF8 下 Scintilla 位置即文档
// 字节偏移，零换算）：
//   打开/重载/遮蔽重载 → SCI_SETTEXT（loading 抑制回写）；
//   用户编辑 SCN_MODIFIED（字符级 insert/delete）→ 增量回写 Document 内容
//   （insert 用通知的 text/length，delete 按 position+length 删除；避免大文档
//   全量 SCI_GETTEXT 回写）；
//   光标移动 SCN_UPDATEUI → doc->setCursorPosition 回写（会话数据源）+ 状态栏刷新。
//   防环：applying_ 标志标识「本次 Document 变更源自编辑器」，组装层据此跳过
//   documentContentChanged 的反向回灌（仅 saveAs 遮蔽/编码重载等外部变化才回灌）。
//
// 字体：Consolas 11pt（旧 QML AppTheme.fontFamilyMono + font.pointSize 11 同值）。
// Scintilla 的 pt→px 换算用 DpiForWindow（SurfaceGDI::Init），PMv2 下自动随
// 窗口 DPI 缩放，WM_DPICHANGED_AFTERPARENT 由 Scintilla 自行重测字体——本层
// 不做手动字体换算。中文经 GDI font-linking 回退（技术保持默认 GDI）。
//
// 换行：全局开关（旧 QML SessionManager.wordWrap 全局、所有文档共享；
// EditorArea.qml 注释「跟随全局设置」实证），新实例继承当前模式。
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "core/document.h"

namespace editor {

// —— 尺寸/配置 token（逻辑单位；字体 pt 由 Scintilla 按 DPI 自换算）——
namespace editor_metrics {

constexpr int FontPt = 11;      // Consolas 11pt（旧 QML 同值）
constexpr int TabWidth = 4;     // Tab 宽 4 等宽字符（旧 QML tabStopDistance=advance("0000")）
constexpr int CaretSlopX = 20;  // 光标水平边缘留白（CARET_SLOP 距离，逻辑像素）

}  // namespace editor_metrics

// —— 查找高亮 indicator 序号（容器区 8..31；0..7 留给词法器、32..35 为
// Scintilla IME 内部使用，均避开）——
constexpr int kIndicatorSearchMatch = 8;    // 全部匹配底色（ROUNDBOX + alpha）
constexpr int kIndicatorSearchCurrent = 9;  // 当前匹配描边（BOX）

// SetWindowSubclass 子类 id（进程内唯一即可）：编辑器子窗口边缘命中透传
//（F6）。与 multicursorcontroller.cpp 的 kSubclassId（0x51431101 'QC1\1'）
// 区分，两套子类化在同一 Scintilla 子窗口上并存。
constexpr UINT_PTR kEdgeHitSubclassId = 0x51431102;  // 'QC1\2'

// 状态栏采样（活动文档的编辑器几何状态）
struct EditorStatus {
    int line = 0;               // 1 起（SCN 通知后采样）
    int column = 0;             // 1 起，字符列（非字节列；中文 1 字符 = 1 列）
    long long selectedChars = 0; // 选区字符数（0 = 无选区；多选区 = 各选区合计，B4）
    long long totalChars = 0;   // 文档总字符数
    int cursorCount = 1;        // 光标数（B4：SCI_GETSELECTIONS，>1 状态栏「光标 ×N」）
};

class EditorHost {
public:
    EditorHost() = default;
    ~EditorHost();

    EditorHost(const EditorHost&) = delete;
    EditorHost& operator=(const EditorHost&) = delete;

    void attach(HWND parent);
    void detach();  // 销毁全部子窗口（须在 DocumentManager 析构前调用）

    // —— 文档生命周期（组装层在 documentsChanged/activeDocumentChanged 驱动）——
    void addDocument(core::Document* doc);        // 创建实例并装载当前内容（幂等）
    void removeDocument(core::Document* doc);     // 销毁实例
    // 实例集合与管理器文档列表对齐（新增建实例、消失销毁；活动文档显隐不变）
    void syncToDocuments(const std::vector<core::Document*>& docs);
    void setActiveDocument(core::Document* doc);  // 显隐切换 + 焦点移交

    // 外部内容变化（saveAs 遮蔽重载/按编码重载）→ 重装文本（撤销栈按外部
    // 覆盖语义清空），光标取 doc->cursorPosition 钳制到新长度
    void reloadFromDocument(core::Document* doc);

    void markSaved(core::Document* doc);  // 保存成功：SCI_SETSAVEPOINT（辅助语义）

    // 本次 Document 内容变化是否源自编辑器回写（组装层防回灌判定）
    bool isApplying(core::Document* doc) const { return applying_ == doc; }

    // —— 视图 ——
    void setWordWrap(bool wrap);  // 全局（旧版语义：所有文档共享）
    bool wordWrap() const { return wordWrap_; }

    // —— 主题（E5）：全部已创建实例按 CurrentPalette 重设配色并重绘 ——
    // 滚动条暗色：实例窗口切换 DarkMode_Explorer 视觉样式类（Win11 深色滚动
    // 条；Win10 无该类时静默回退浅色滚动条）+ FRAMECHANGED 触发非客户区重绘
    void retheme();

    // —— 布局 ——
    void layout(const RECT& contentRect);  // WM_SIZE/DPI 变化后重排全部实例

    void focusActive();                     // 焦点到活动编辑器（无则不动）
    HWND activeWindow() const;              // 活动编辑器窗口（无文档为 nullptr）

    // —— 通知（父窗口 WM_NOTIFY 转发；返回是否为本组件实例的消息）——
    bool handleNotify(const NMHDR* hdr);

    // 状态采样（doc 的编辑器当前光标/选区/字符数；无实例返回零值）
    EditorStatus statusOf(const core::Document* doc) const;

    // 测试钩子：文档对应实例窗口（自动化驱动 WM_CHAR/SCI 消息）
    HWND windowForTest(const core::Document* doc) const;

    // —— 挂接点（组装层）——
    std::function<void(core::Document*)> onUserEdited;   // 用户编辑已回写 Document
    std::function<void(core::Document*)> onCursorChanged; // 光标/选区变化（状态栏刷新）
    // 实例窗口创建/销毁（M3-B：组装层挂多光标子类化等按窗口处理的组件）
    std::function<void(HWND)> onWindowCreated;
    std::function<void(HWND)> onWindowDestroyed;

private:
    struct Entry {
        core::Document* doc = nullptr;
        HWND hwnd = nullptr;
        bool loading = false;  // 程序化装载期间抑制 SCN_MODIFIED 回写
    };

    Entry* find(core::Document* doc);
    Entry* find(HWND hwnd);
    const Entry* find(const core::Document* doc) const;
    HWND createEditor();
    void configure(HWND sci, UINT dpi) const;
    // 主题相关配色（configure 与 retheme 共用；字体/策略等与主题无关的项不重设）
    static void applyThemeColors(HWND sci);
    void loadFromDocument(Entry& e);

    HWND parent_ = nullptr;
    std::vector<Entry> entries_;
    core::Document* active_ = nullptr;
    core::Document* applying_ = nullptr;  // 编辑器回写进行中（防回灌标志）
    bool wordWrap_ = false;
    RECT contentRect_{};
};

}  // namespace editor
