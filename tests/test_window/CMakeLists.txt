# 窗口关闭/退出流程 QML 集成测试目标（offscreen）：实例化完整 Main.qml
# （ApplicationWindow + DocumentManager 单例 + 未保存确认框接线），
# 以 QWindow::close() 等价驱动 onClosing 四分支与确认框各分支
# （TC-001 至 TC-009 的可自动化部分）。
qt_add_executable(test_window
    tst_windowclose.cpp
)

# qcnote_coreplugin：静态 QML 模块插件（import qcnote 所需）
target_link_libraries(test_window PRIVATE
    Qt6::Test
    Qt6::Quick
    qcnote_core
    qcnote_coreplugin
)

qt_import_qml_plugins(test_window)

add_test(NAME test_window COMMAND test_window)
# GUI 红线：自动化验证一律 offscreen，无可见窗口
set_tests_properties(test_window PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# 会话恢复 QML 集成测试目标（offscreen，架构同上：单 main）：
# main 中 QSettings::setPath 重定向会话文件到临时目录（不污染真实用户
# 配置），实例化 Main.qml 验证启动恢复链路（TC-011/TC-012）、编辑器光标
# 双向同步与退出路径会话落盘。
qt_add_executable(test_sessionrestore
    tst_sessionrestore.cpp
)

target_link_libraries(test_sessionrestore PRIVATE
    Qt6::Test
    Qt6::Quick
    qcnote_core
    qcnote_coreplugin
)

qt_import_qml_plugins(test_sessionrestore)

add_test(NAME test_sessionrestore COMMAND test_sessionrestore)
# GUI 红线：自动化验证一律 offscreen，无可见窗口
set_tests_properties(test_sessionrestore PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# 启动参数打开 + 单实例转发 QML 集成测试目标（offscreen，架构同上）：
# 复刻 main.cpp 启动序列（引擎加载前 SingleInstance::create + initialize
# 启动参数），实例化 Main.qml 验证恢复会话后叠加打开（TC-007）、IPC 转发
# 叠加（TC-008/TC-014）。main 中注入随机 USERNAME 隔离命名管道服务名，
# 会话重定向到临时目录，QCNOTE_TEST_SHELL_NOOP 隔离首启提示。
qt_add_executable(test_startupargs
    tst_startupargs.cpp
)

target_link_libraries(test_startupargs PRIVATE
    Qt6::Test
    Qt6::Quick
    qcnote_core
    qcnote_coreplugin
)

qt_import_qml_plugins(test_startupargs)

add_test(NAME test_startupargs COMMAND test_startupargs)
# GUI 红线：自动化验证一律 offscreen，无可见窗口
set_tests_properties(test_startupargs PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# 对话框样式与弹窗拖动 QML 集成测试目标（offscreen，架构同上：
# 单 main）：按 QML 类型短名 MovableDialog + dialogTitle 属性定位
# Main.qml 四个对话框，断言默认位置居中（未拖动 resize 跟随）、
# header 把手合成鼠标拖动精确位移、四方向超界 clamp（把手不丢失）、
# 关闭重开复位、拖动后窗口 resize 重新 clamp、SearchDialog 拖动后
# 功能正常，以及 closePolicy 仅 Esc、footer WinLayout/右对齐/按钮
# 命名与 primary、正文文案与错误框排队链路（《测试用例-对话框居中与
# 按钮Windows化》TC-001/002/004/005/006/007/008 与《测试用例-弹窗
# 拖动》TC-002/003/004/006/009 的可自动化部分）。
qt_add_executable(test_dialogstyle
    tst_dialogstyle.cpp
)

target_link_libraries(test_dialogstyle PRIVATE
    Qt6::Test
    Qt6::Quick
    qcnote_core
    qcnote_coreplugin
)

qt_import_qml_plugins(test_dialogstyle)

add_test(NAME test_dialogstyle COMMAND test_dialogstyle)
# GUI 红线：自动化验证一律 offscreen，无可见窗口
set_tests_properties(test_dialogstyle PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# GBK 与 UTF-8 编码支持 QML 集成测试目标（offscreen，架构同上：单 main）：
# 按标题定位「编码」子菜单与按编码重载确认框，直发菜单项 triggered()
# 驱动 TitleBar → Main → DocumentManager 信号链，断言菜单启用状态、
# 打勾标识、状态栏编码文本、确认框取消/确认/不弹框三分支、通过编码
# 保存的转换/转另存为/不可编码错误链路（《测试用例-GBK与UTF8编码支持》
# TC-017/TC-018 的可自动化部分）。
qt_add_executable(test_encodingmenu
    tst_encodingmenu.cpp
)

target_link_libraries(test_encodingmenu PRIVATE
    Qt6::Test
    Qt6::Quick
    qcnote_core
    qcnote_coreplugin
)

qt_import_qml_plugins(test_encodingmenu)

add_test(NAME test_encodingmenu COMMAND test_encodingmenu)
# GUI 红线：自动化验证一律 offscreen，无可见窗口
set_tests_properties(test_encodingmenu PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# 查找替换菜单入口 QML 集成测试目标（offscreen，架构同上：单 main）：
# 按标题定位 文件/搜索/视图 子菜单与查找对话框，对菜单项直发 triggered()
# 驱动 TitleBar → Main → SearchDialog/SearchController 信号链，断言主菜单
# 重组结构、搜索四项置灰条件、查找…/替换…入口（预填/焦点/高亮）、
# 下一个/上一个关闭态跳转（环绕、不恢复高亮）与无匹配无操作，附文件/
# 编码/视图/退出代表性回归（《测试用例-查找替换菜单入口》TC-001 至
# TC-006、TC-008 的可自动化部分）。
qt_add_executable(test_searchmenu
    tst_searchmenu.cpp
)

target_link_libraries(test_searchmenu PRIVATE
    Qt6::Test
    Qt6::Quick
    qcnote_core
    qcnote_coreplugin
)

qt_import_qml_plugins(test_searchmenu)

add_test(NAME test_searchmenu COMMAND test_searchmenu)
# GUI 红线：自动化验证一律 offscreen，无可见窗口
set_tests_properties(test_searchmenu PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# 草稿缓存 QML 单例链路回归测试目标（offscreen，架构同上：单 main）：
# 《退出免提示与草稿缓存单例修复》DEC-005——加载真实 Main.qml，
# 经引擎拿 DocumentManager 单例后：tc002 真实防抖计时器到期断言条目
# 落盘（TC-002，修复前红/修复后绿，QML 单例分裂根因的直接回归锁），
# tc003 预放置未命名条目断言恢复且激活（TC-003，DEC-002 激活规则）。
# 会话经 QSettings::setPath 重定向临时目录，缓存目录按本 exe 名派生并
# 预清理/结束清理，不污染真实用户数据。
qt_add_executable(test_draftsingleton
    tst_draftsingleton.cpp
)

target_link_libraries(test_draftsingleton PRIVATE
    Qt6::Test
    Qt6::Quick
    qcnote_core
    qcnote_coreplugin
)

qt_import_qml_plugins(test_draftsingleton)

add_test(NAME test_draftsingleton COMMAND test_draftsingleton)
# GUI 红线：自动化验证一律 offscreen，无可见窗口
set_tests_properties(test_draftsingleton PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# 端到端草稿链路驱动 harness（不注册 ctest，手动/编排脚本运行）：
# 《测试用例-退出免提示与草稿缓存单例修复》TC-010/TC-011/TC-012 的进程侧
# 执行器，由 tests/verify-hotexit-draft-e2e.ps1 启动。加载真实 Main.qml，
# 经 QML 动作入口（doNewDocument/doQuit）与活动 TextArea append() 等价驱动
# 用户输入链路（EditorArea onTextChanged → doc.content 回写 → 真实防抖），
# 支持热退出/待强杀/失焦强杀/恢复断言四模式。输出目录指到 build 根：与
# qcnote.exe 同目录共享便携标记（qcnote.portable → build\drafts）。
qt_add_executable(e2e_draft_driver
    e2e_draft_driver.cpp
)
set_target_properties(e2e_draft_driver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
target_link_libraries(e2e_draft_driver PRIVATE
    Qt6::Quick
    qcnote_core
    qcnote_coreplugin
)
qt_import_qml_plugins(e2e_draft_driver)

# 标签页右键菜单 QML 集成测试目标（offscreen，架构同上：单 main）：
# 按 QML 类型短名定位 DocumentTabs，经「关闭右边所有」等文本反查其根级
# 右键菜单，程序化设置 contextIndex 后 open()，对菜单项发送真实鼠标事件
# （区别于直发 triggered()——批量关闭入口 closeRangeWithConfirm 带 uiLocked
# 防重入检查，其结果与菜单可见性时序相关），断言置灰条件、关闭右边/左边/
# 其他（锚点激活）、逐个三选确认顺序（从左到右）与取消中止，附直发
# triggered() 对照组（《测试用例-标签页右键菜单》TC-002/003/005/006/007/010
# 的可自动化部分）。
qt_add_executable(test_tabcontextmenu
    tst_tabcontextmenu.cpp
)

target_link_libraries(test_tabcontextmenu PRIVATE
    Qt6::Test
    Qt6::Quick
    qcnote_core
    qcnote_coreplugin
)

qt_import_qml_plugins(test_tabcontextmenu)

add_test(NAME test_tabcontextmenu COMMAND test_tabcontextmenu)
# GUI 红线：自动化验证一律 offscreen，无可见窗口
set_tests_properties(test_tabcontextmenu PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# 窗口边界线 QML 集成测试目标（offscreen，架构同上：单 main）：
# 按 特征（QQuickRectangle + z:999 + border.width:1 + 透明填充）定位
# Main.qml 的 1px 窗口边界描边层，断言铺满窗口、最大化隐藏/还原恢复、
# 颜色随 active 状态取 windowBorder/windowBorderInactive、无输入接收且
# z 低于全部 8 个边缘缩放命中区（《测试用例-窗口边界线》TC-003/004/
# 005/006 的可自动化部分；视觉观感与交互手感需肉眼验收）。
qt_add_executable(test_windowborder
    tst_windowborder.cpp
)

target_link_libraries(test_windowborder PRIVATE
    Qt6::Test
    Qt6::Quick
    qcnote_core
    qcnote_coreplugin
)

qt_import_qml_plugins(test_windowborder)

add_test(NAME test_windowborder COMMAND test_windowborder)
# GUI 红线：自动化验证一律 offscreen，无可见窗口
set_tests_properties(test_windowborder PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# 标签页 Win11 风格改造 QML 集成测试目标（offscreen，架构同上：单 main）：
# 按 QML 类型短名定位 TitleBar/DocumentTabs/其内 ListView，经 itemAtIndex
# 取 delegate 后按特征定位背景结构（radius 5 主背景/底部补角矩形/radius 4
# 悬停高亮/border 2 焦点环），断言活动标签连通式（token 引用 toolbarBackground、
# 同色顶圆角无描边、贴底高度 34）、非活动透明 + 合成悬停高亮（深浅两主题，
# DEC-002）、关闭按钮移除（无 Canvas/无多余 Text，TC-005）、关闭入口
# （主菜单直发 triggered/Ctrl+W 合成按键，TC-006）、未保存 ● 标记（TC-007）、
# 自然宽度公式 + 等比收缩 + 饱和溢出（TC-008）、溢出 interactive 联动
# （TC-009）、键盘导航与焦点环（TC-010）、文字右边距与省略（TC-011）
# （《测试用例-标签页Win11风格改造》的可自动化部分）。
qt_add_executable(test_win11tabs
    tst_win11tabs.cpp
)

target_link_libraries(test_win11tabs PRIVATE
    Qt6::Test
    Qt6::Quick
    qcnote_core
    qcnote_coreplugin
)

qt_import_qml_plugins(test_win11tabs)

add_test(NAME test_win11tabs COMMAND test_win11tabs)
# GUI 红线：自动化验证一律 offscreen，无可见窗口
set_tests_properties(test_win11tabs PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# 标签页 Win11 风格改造截图取证 harness（不注册 ctest，手动/编排脚本运行）：
# 《测试用例-标签页Win11风格改造》TC-002/003/004/008 视觉证据与 DEC-002/
# DEC-004 预约验证点的截图侧执行器，由 tests\verify-win11tabs.ps1 启动。
# 加载真实 Main.qml，按场景（浅/深 × 单标签/多标签悬停/收缩/溢出/焦点环）
# 构造后 grabWindow() 落 PNG（整窗 + 标题栏 2x 放大）到指定输出目录。
qt_add_executable(e2e_tabshot_driver
    e2e_tabshot_driver.cpp
)
set_target_properties(e2e_tabshot_driver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

target_link_libraries(e2e_tabshot_driver PRIVATE
    Qt6::Quick
    qcnote_core
    qcnote_coreplugin
)

qt_import_qml_plugins(e2e_tabshot_driver)
