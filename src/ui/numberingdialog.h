// qcnote 插入编号对话框（列模式填充编号，任务B）：复用 SearchDialog 的
// 窗口/模态循环/拖动/边界约束/DPI/主题换肤骨架（DEC-003）精简而成的参数
// 输入对话框。
//
// 控件方案（与 SearchDialog 同源）：
// - 输入框 = 原生 EDIT 子控件（WS_CHILD|ES_AUTOHSCROLL，无 WS_BORDER；
//   不用 ES_NUMBER——需允许负号），边框/焦点环由父窗口在 EDIT 外圈自绘
//   （1px border / accent 焦点环），文字色/背景经 WM_CTLCOLOREDIT 换肤；
//   中文 IME、光标、选择、Ctrl+V 等编辑行为全部原生。Enter/Esc/Tab 经
//   子类化拦截转发对话框动作。
// - 复选框 = 自绘（勾选框图形 + 文字；勾选为形状线索非颜色）。
// - 状态行 = 确定时校验的错误文本（error 色；文本自身即用户可读的错误
//   消息——非颜色线索）。
//
// 布局（width 380 逻辑像素，宿主居中——对话框惯例，无强制顶部对齐）：
//   标题「插入编号」把手 / 起始值行（标签+输入框）/ 增量行 / 复选框行
//   （补零（自动位数））/ 状态行 / 按钮行（确定[默认] 取消）
//
// 键盘流转（对齐 SearchDialog）：
//   Tab/Shift+Tab 循环（起始值→增量→补零→确定→取消）；Enter = 确定
//   （焦点在按钮上则触发该按钮）；Esc = 取消；Space 切换复选/触发聚焦
//   按钮；输入框内编辑行为全原生（IME 支持）。
//
// 参数记忆（DEC-005）：确定成功后进程内记住三参数（文件级静态，不落盘），
// 下次打开预填；取消/Esc 不记忆中间态；重启回默认 1/1/关。
//
// 校验（确定时；非法 → 状态行错误文本 + 不关闭 + 焦点回首个非法输入框）：
//   合法十进制整数（可选前导 '-' + 至少一位数字，无空白/小数点/其他字符）、
//   增量 ≠ 0、|起始值| ≤ 10^15、|增量| ≤ 10^6（DEC-007）。
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace ui {

namespace numberingdialog_metrics {

constexpr int Width = 380;        // 逻辑宽（对齐 DialogHost 默认宽）
constexpr int PaddingH = 24;
constexpr int PaddingTop = 20;
constexpr int TitlePixelSize = 14;
constexpr int BodyPixelSize = 12;
constexpr int EditHeight = 28;
constexpr int RowSpacing = 8;
constexpr int SectionSpacing = 12;
constexpr int CheckBoxBox = 16;   // 勾选框边长
constexpr int StatusHeight = 18;
constexpr int ButtonHeight = 32;
constexpr int ButtonMinWidth = 88;
constexpr int ButtonSpacing = 8;
constexpr int ButtonsToBottom = 20;
constexpr int KeepWidth = 80;     // 拖动边界约束（把手最小保留宽）
constexpr int LabelWidth = 52;    // 「起始值：」「增量：」标签列宽

// 按钮序（0 起从左到右）
constexpr int ButtonOk = 0;      // 确定（默认按钮）
constexpr int ButtonCancel = 1;  // 取消
constexpr int ButtonCount = 2;

}  // namespace numberingdialog_metrics

// 校验范围常量（DEC-007：配合选区数上限保证 long long 不溢出）
constexpr long long kStartAbsMax = 1000000000000000LL;  // 10^15
constexpr long long kStepAbsMax = 1000000LL;            // 10^6

struct NumberingParams {
    long long start = 1;
    long long step = 1;
    bool padZero = false;
};

// 解析+校验两输入框文本；成功返回 true 并写 out.start/out.step（padZero
// 不在解析范围，保持调用方原值）；失败返回 false 且 errorText 为用户可读
// 中文错误（错误优先级：起始值语法 → 起始值范围 → 增量语法 → 增量非 0 →
// 增量范围——与「焦点回首个非法输入框」口径一致）。纯函数，不依赖窗口。
bool ParseNumberingParams(const std::wstring& startText, const std::wstring& stepText,
                          NumberingParams& out, std::wstring& errorText);

class NumberingDialog {
public:
    NumberingDialog() = default;
    ~NumberingDialog();

    NumberingDialog(const NumberingDialog&) = delete;
    NumberingDialog& operator=(const NumberingDialog&) = delete;

    // 模态显示；true = 确定（params 写回用户值 + 进程内记忆），false = 取消/
    // Esc（params 不被写回、不记忆中间态）
    bool showModal(HWND owner, NumberingParams& params);

    bool active() const { return active_; }

    // —— 测试钩子（对齐 SearchDialog 命名惯例）——
    HWND dialogWindowForTest() const { return hwnd_; }
    HWND startEditForTest() const { return startEdit_; }
    HWND stepEditForTest() const { return stepEdit_; }
    RECT buttonRectForTest(int index) const;   // 0=确定 1=取消
    RECT checkBoxRectForTest() const;
    const std::wstring& statusTextForTest() const { return statusText_; }
    std::function<void()> onOpened;            // 自动化注入（打开后、循环前）

    // EDIT 子类化过程需要触发确定/关闭/焦点循环（见 numberingdialog.cpp）
    friend LRESULT CALLBACK NumberingEditSubclassProc(HWND hwnd, UINT msg, WPARAM wp,
                                                      LPARAM lp, UINT_PTR id,
                                                      DWORD_PTR ref);

private:
    static LRESULT CALLBACK DialogWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void buildLayout();
    void paint();
    void close();
    void endDrag();
    RECT clampDialogRect(RECT r) const;
    void accept();               // 确定：校验 → 写回+记忆关闭 / 报错不关闭
    void toggleCheckBox();
    void activateButton(int idx);  // 按钮/键盘触发统一出口
    void setFocusItem(int idx, bool showFocusVisual);
    void moveFocus(int delta);

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND startEdit_ = nullptr;
    HWND stepEdit_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HBRUSH editBkBrush_ = nullptr;
    UINT dpi_ = 96;
    bool active_ = false;
    bool done_ = false;
    bool result_ = false;       // showModal 返回值（确定 true）
    bool dragging_ = false;
    POINT dragOrigin_{};
    int hoverItem_ = -1;    // -1 无；2 复选、3/4 按钮（与 focusItem 同轴）
    int pressedItem_ = -1;
    int focusItem_ = 0;     // 0/1 输入框；2 复选；3/4 按钮
    bool focusVisible_ = false;
    bool padZero_ = false;      // 复选框状态（打开时自记忆预填）
    bool statusIsError_ = false;
    NumberingParams accepted_{};  // 确定时暂存（showModal 返回前写回调用方 params）
    // 布局（设备像素）
    RECT header_{}, startLabel_{}, startEditRect_{}, stepLabel_{}, stepEditRect_{};
    RECT checkBox_{}, statusRect_{}, buttons_[2]{};
    int width_ = 0, height_ = 0;
    // 会话显示状态
    std::wstring statusText_;
};

}  // namespace ui
