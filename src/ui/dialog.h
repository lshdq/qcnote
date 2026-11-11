// qcnote 自绘基础对话框（M2-B，E4）：后续一切应用内模态弹窗的统一基座——
// M2 用例：打开/保存失败错误框、另存为遮蔽重载失败提示；
// M3 复用：查找替换对话框（顶部对齐 y≈96 经 defaultPos 覆写）；
// M4 复用：未保存三选确认、编码重载确认、草稿冲突汇总。
//
// 模态语义（参照 MenuHost 模式）：showModal 内自有消息循环，期间禁用宿主窗口
// （EnableWindow(FALSE)），键盘由对话框窗口接收——宿主快捷键自然屏蔽；返回
// 所选按钮 id（Esc 返回 spec.escId，可配置；Enter 触发默认按钮）。
//
// E4 可拖动基础语义（完整版 M3/M4 随查找对话框细化的 clamp/复位策略在此定形）：
//   - 默认位置：宿主客户区居中（defaultPos 覆写，屏幕坐标）；
//   - 每次打开复位默认位置（旧 MovableDialog onAboutToShow 复位语义）；
//   - header 标题条为拖动把手：左键按住移动窗口；
//   - 边界约束（把手不丢失）：对话框左上角 clamp 在宿主窗口矩形内
//     [owner.left, owner.right - keepWidth] × [owner.top, owner.bottom - headerH]
//     ——把手整体始终留在宿主窗口内可抓回，弹窗身体允许右/下越界。
//
// 视觉：contentBackground 卡片 + 1px border 边线 + 圆角/投影
// （DWMWCP_ROUNDSMALL + CS_DROPSHADOW，同 MenuHost）；
// 按钮：默认（accent 底 + 白字）/普通（border 描边 + textPrimary），hover/pressed。
#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ui {

namespace dialog_metrics {

constexpr int WidthDefault = 380;     // 逻辑宽（旧 QML MovableDialog width 380/440）
constexpr int PaddingH = 24;          // 卡片水平内边距（旧 Material padding 24）
constexpr int PaddingTop = 20;        // 标题区顶留白
constexpr int TitlePixelSize = 14;    // 标题字号（粗体）
constexpr int MessagePixelSize = 12;  // 正文字号（9pt）
constexpr int TitleToMessageGap = 12;
constexpr int MessageToButtonsGap = 20;
constexpr int ButtonHeight = 32;
constexpr int ButtonMinWidth = 88;
constexpr int ButtonSpacing = 8;
constexpr int ButtonsToBottom = 20;
constexpr int KeepWidth = 80;         // 拖动边界约束：把手最小保留宽（旧 QML keepWidth）

}  // namespace dialog_metrics

// 按钮描述：id 由调用方定义（如 1=确定 / 2=取消 / 3=不保存），返回值即所选 id
struct DialogButtonSpec {
    std::wstring text;
    int id = 0;
    bool isDefault = false;  // Enter 触发 + accent 主按钮视觉
};

struct DialogSpec {
    std::wstring title;       // header 标题（拖动把手文本）
    std::wstring message;     // 正文（\n 换行；自动按宽折行）
    std::vector<DialogButtonSpec> buttons;  // 至少 1 个；从左到右排布
    int escId = 0;            // Esc 关闭返回值（通常 = 取消类按钮 id；0 也合法）
    std::optional<POINT> defaultPos;  // 默认位置覆写（屏幕坐标；空 = 宿主居中）
    int widthLogical = dialog_metrics::WidthDefault;
};

// 对话框尺寸计算（纯函数，单测覆盖）：按消息文本测量高度
// （messageTextLines 为 DrawText 实测折行后的行数，由组件测量传入）。
int ComputeDialogHeight(int lineCount, UINT dpi);

class DialogHost {
public:
    DialogHost() = default;
    ~DialogHost();

    DialogHost(const DialogHost&) = delete;
    DialogHost& operator=(const DialogHost&) = delete;

    // 模态显示（阻塞至按钮选择/Esc 关闭）；返回所选按钮 id 或 escId。
    // owner 禁用期间窗口不可交互；返回前恢复启用并归还焦点。
    int showModal(HWND owner, const DialogSpec& spec);

    bool active() const { return active_; }

    // 测试/自动化注入口：窗口创建并显示后、消息循环开始前触发
    //（可向 dialogWindowForTest() 投递键盘消息驱动）
    std::function<void()> onOpened;

    HWND dialogWindowForTest() const { return hwnd_; }

private:
    struct Layout {
        int width = 0, height = 0;
        RECT header{};                 // 拖动把手（标题文本区）
        RECT message{};
        std::vector<RECT> buttons;
        int defaultButton = -1;
    };

    static LRESULT CALLBACK DialogWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void buildLayout();
    void paint();
    void close(int result);
    void endDrag();
    RECT clampDialogRect(RECT r) const;

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    DialogSpec spec_;
    Layout layout_;
    HFONT titleFont_ = nullptr;
    HFONT messageFont_ = nullptr;
    UINT dpi_ = 96;
    int result_ = 0;
    bool active_ = false;
    bool done_ = false;
    bool dragging_ = false;      // header 拖动中（捕获状态）
    POINT dragOrigin_{};         // 拖动起点（屏幕坐标：光标 + 窗口左上）
    int hoverButton_ = -1;
    int pressedButton_ = -1;
    int focusButton_ = 0;        // 键盘焦点按钮（Tab 循环）
    bool focusVisible_ = false;  // 焦点环显示（Tab 按下后）
};

}  // namespace ui
