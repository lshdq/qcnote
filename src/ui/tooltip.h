// qcnote 手动 TRACK 模式 tooltip（M2-B）：comctl32 tooltip 控件封装。
//
// 与 TabStrip（M1-3）相同的做法抽出的共享实现：悬停延迟由宿主计时（WM_TIMER），
// 到时经 show() 激活 TRACK tooltip；文本/位置随时可更新。WS_EX_NOACTIVATE
// 不夺焦点。toolbar（按钮 tooltip）与 statusbar（路径完整名 tooltip）复用。
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <string>

namespace ui {

class ManualTooltip {
public:
    ManualTooltip() = default;
    ~ManualTooltip();

    ManualTooltip(const ManualTooltip&) = delete;
    ManualTooltip& operator=(const ManualTooltip&) = delete;

    void create(HWND hostWindow);  // hostWindow：tooltip 归属窗口（销毁上下文）
    void destroy();
    bool valid() const { return tooltip_ != nullptr; }

    // 显示/更新 tooltip（screenPt 为屏幕坐标，通常为光标位置）
    void show(const std::wstring& text, POINT screenPt);
    void hide();

    // 按当前主题设置 tip 底色/文字色（E5：系统 tooltip 控件本身无 token 概念，
    // 显式经 TTM_SETTIPBKCOLOR/TTM_SETTIPTEXTCOLOR 接入；边框仍系统绘制）。
    // create() 时与主题切换时（组装层转发）调用；未创建时为空操作
    void applyThemeColors();

private:
    TOOLINFO makeToolInfo() const;

    HWND host_ = nullptr;
    HWND tooltip_ = nullptr;
    std::wstring text_;  // 控件持有指针期间须存活的文本缓冲
};

}  // namespace ui
