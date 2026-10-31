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
