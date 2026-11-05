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
