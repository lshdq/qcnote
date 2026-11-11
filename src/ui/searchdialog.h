// qcnote 查找替换对话框（M3-B，B5）：基于 DialogHost 的视觉/模态模式扩展的
// 专用输入对话框（E4：顶部对齐 y≈96、可拖动把手、边界约束）。
//
// 与 DialogHost 的关系：DialogHost 是「消息 + 按钮行」通用弹窗；本对话框
// 需要输入控件（EDIT 子类化换肤）、复选框、状态行与常驻动作按钮（查找类
// 对话框惯例：查找下一个/替换/全部替换不关闭对话框），交互模型不同，故
// 复用 DialogHost 的窗口/模态循环/拖动/边界约束骨架（同视觉语言），自成
// 窗口类。
//
// 控件方案（决策）：
// - 输入框 = 原生 EDIT 子控件（WS_CHILD|ES_AUTOHSCROLL，无 WS_BORDER），
//   边框/焦点环由父窗口在 EDIT 外圈自绘（1px border / accent 焦点环），
//   文本色/背景经 WM_CTLCOLOREDIT 换肤；中文 IME、光标、选择、Ctrl+V 等
//   编辑行为全部原生（自绘输入框需重做 IME/光标，工作量大且风险高——
//   「IME 天然支持」是选原生 EDIT 的决定性理由）。Enter/Shift+Enter/Esc/
//   F3/Tab 经子类化拦截转发对话框动作。
// - 复选框 = 自绘（勾选框图形 + 文字；正则开启时「全字匹配」置灰禁用）。
// - 状态行文本 = 纯函数 BuildSearchStatusText（优先级：错误 → 已替换 N 处
//   → 无匹配 → >10000 → 第 N 个 / 共 M 个；错误文本另以 error 色着色，
//   颜色仅辅助线索）。
//
// 布局（参照旧 SearchDialog.qml：width 520，顶部对齐 defaultY=96）：
//   标题「查找与替换」把手 / 查找行（标签+输入框）/ 替换行 / 选项行（三复选
//   ）/ 状态行 / 按钮行（查找下一个[默认] 上一个 替换 全部替换 关闭）
//
// 键盘流转（对齐旧版焦点流转）：
//   Tab/Shift+Tab 循环：查找框 → 替换框 → 三复选 → 五按钮；
//   Enter = 查找下一个（焦点在按钮上则触发该按钮；输入框内 Shift+Enter =
//   上一个）；Esc/「关闭」= 关闭；F3/Shift+F3 = 下一个/上一个；
//   Space 切换聚焦的复选/触发聚焦的按钮。
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace ui {

namespace searchdialog_metrics {

constexpr int Width = 520;        // 旧 QML SearchDialog width
constexpr int TopAlignY = 96;     // 顶部对齐 y（DEC-002：避免遮挡编辑区）
constexpr int PaddingH = 24;
constexpr int PaddingTop = 20;
constexpr int TitlePixelSize = 14;
constexpr int BodyPixelSize = 12;
constexpr int EditHeight = 28;
constexpr int RowSpacing = 8;
constexpr int SectionSpacing = 12;
constexpr int CheckBoxBox = 16;   // 勾选框边长
constexpr int CheckBoxSpacing = 24;
constexpr int StatusHeight = 18;
constexpr int ButtonHeight = 32;
constexpr int ButtonMinWidth = 72;
constexpr int ButtonSpacing = 8;
constexpr int ButtonsToBottom = 20;
constexpr int KeepWidth = 80;     // 拖动边界约束（把手最小保留宽）
constexpr int LabelWidth = 52;    // 「查找：」「替换为：」标签列宽

// 按钮序（0 起从左到右）
constexpr int ButtonFindNext = 0;
constexpr int ButtonFindPrev = 1;
constexpr int ButtonReplace = 2;
constexpr int ButtonReplaceAll = 3;
constexpr int ButtonClose = 4;
constexpr int ButtonCount = 5;
// 复选序
constexpr int CheckCase = 0;
constexpr int CheckWholeWord = 1;
constexpr int CheckRegex = 2;

}  // namespace searchdialog_metrics

// 控制器状态快照（供状态行文本与按钮使能）
struct SearchDialogState {
    std::wstring errorMessage;  // 无效正则等（空 = 无错误）
    int matchCount = 0;
    int currentIndex = -1;
    bool overflowed = false;
    int generation = 0;         // 全新查找会话代数（pattern/选项/重绑 +1）
};

// 状态行文本（纯函数，单测覆盖；优先级见文件头注释）
std::wstring BuildSearchStatusText(const SearchDialogState& state, int lastReplacedCount);

// 对话框动作契约：组装层在 controller + 编辑器之上实现（main.cpp 适配器）
class SearchDialogClient {
public:
    virtual ~SearchDialogClient() = default;
    virtual std::wstring GetPattern() = 0;               // UTF-16
    virtual void SetPattern(const std::wstring&) = 0;    // 触发重算 + 高亮
    virtual std::wstring GetReplacement() = 0;
    virtual void SetReplacement(const std::wstring&) = 0;
    virtual bool GetOption(int idx) = 0;                 // 0 大小写/1 全字/2 正则
    virtual void SetOption(int idx, bool on) = 0;
    virtual SearchDialogState GetState() = 0;
    virtual void FindNext() = 0;                         // 环绕推进 + 原生选区
    virtual void FindPrev() = 0;
    virtual void SelectCurrent() = 0;                    // 定位当前匹配（不推进）
    virtual bool SelectionIsCurrentMatch() = 0;          // 全新会话首跳判定
    virtual bool ReplaceCurrent() = 0;
    virtual int ReplaceAll() = 0;
    virtual void OnClosed() = 0;                         // 清高亮 + 焦点回编辑器
};

class SearchDialog {
public:
    SearchDialog() = default;
    ~SearchDialog();

    SearchDialog(const SearchDialog&) = delete;
    SearchDialog& operator=(const SearchDialog&) = delete;

    // 客户端在启动时接线一次（对话框开/关态均可用——F3 关闭态跳转共用出口）
    void setClient(SearchDialogClient* client) { client_ = client; }

    // 模态打开（预填由调用方先经客户端写入 pattern；此处读取并同步输入框）。
    // focusReplace：Ctrl+H 聚焦替换框，Ctrl+F 聚焦查找框并全选
    void open(HWND owner, bool focusReplace);

    bool active() const { return active_; }

    // 跳转统一出口（对话框内 Enter/按钮/F3 与关闭态 F3/菜单共用；DEC-013）
    void doFindNext();
    void doFindPrev();

    // 对话框打开期间重复 Ctrl+F / Ctrl+H（旧版 openSearchDialog visible 分支）
    void refocusFind();      // 聚焦查找框并全选
    void refocusReplace();   // 聚焦替换框

    // 控制器状态变化刷新（内容变化/替换后；仅对话框打开时有可见效果）
    void refreshState();

    // —— 测试钩子 ——
    HWND dialogWindowForTest() const { return hwnd_; }
    HWND findEditForTest() const { return findEdit_; }
    HWND replaceEditForTest() const { return replaceEdit_; }
    RECT buttonRectForTest(int index) const;      // searchdialog_metrics::Button*
    RECT checkBoxRectForTest(int index) const;    // searchdialog_metrics::Check*
    const std::wstring& statusTextForTest() const { return statusText_; }
    std::function<void()> onOpened;               // 自动化注入（打开后、循环前）

    // EDIT 子类化过程需要触发关闭/焦点循环（见 searchdialog.cpp）
    friend LRESULT CALLBACK SearchEditSubclassProc(HWND hwnd, UINT msg, WPARAM wp,
                                                   LPARAM lp, UINT_PTR id, DWORD_PTR ref);

private:
    static LRESULT CALLBACK DialogWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void buildLayout();
    void paint();
    void close();
    void endDrag();
    RECT clampDialogRect(RECT r) const;
    void syncInputsFromClient();          // 打开时：输入框/复选 ← 客户端
    void onFindEdited();                  // 查找框 EN_CHANGE
    void onReplaceEdited();               // 替换框 EN_CHANGE
    void toggleCheckBox(int idx);
    void activateButton(int idx);         // 按钮/键盘触发统一出口
    void setFocusItem(int idx, bool showFocusVisual);
    void moveFocus(int delta);
    void refreshStatusText();
    bool buttonEnabled(int idx) const;
    bool checkBoxEnabled(int idx) const;

    SearchDialogClient* client_ = nullptr;
    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND findEdit_ = nullptr;
    HWND replaceEdit_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HBRUSH editBkBrush_ = nullptr;
    UINT dpi_ = 96;
    bool active_ = false;
    bool done_ = false;
    bool dragging_ = false;
    POINT dragOrigin_{};
    int hoverItem_ = -1;   // -1 无；0..4 复选、5..9 按钮
    int pressedItem_ = -1;
    int focusItem_ = 0;    // 0/1 输入框；2..4 复选；5..9 按钮
    bool focusVisible_ = false;
    // 布局（设备像素）
    RECT header_{}, findLabel_{}, findEditRect_{}, replaceLabel_{}, replaceEditRect_{};
    RECT checkBoxes_[3]{}, statusRect_{}, buttons_[5]{};
    int width_ = 0, height_ = 0;
    // 会话显示状态
    std::wstring statusText_;
    int lastReplacedCount_ = -1;  // -1 = 不显示（到下一次查找/替换操作前显示）
    int lastGeneration_ = -1;     // 全新会话判定（对应旧版 hasJumped 重置源）
    bool hasJumped_ = false;      // 全新会话首跳先定位当前匹配
};

}  // namespace ui
