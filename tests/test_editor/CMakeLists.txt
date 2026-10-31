# QML 集成测试目标（offscreen）：实例化真实 EditorArea 组件
# （TextArea + MultiCursorController + MultiCursorLayer 完整接线），
# 以合成 key/mouse/InputMethod 事件驱动列式编辑交互链路
# （TC-101 至 TC-113 的可自动化部分）。
qt_add_executable(test_editor
    tst_editorintegration.cpp
)

# qcnote_coreplugin：静态 QML 模块插件（import qcnote 所需）
target_link_libraries(test_editor PRIVATE
    Qt6::Test
    Qt6::Quick
    qcnote_core
    qcnote_coreplugin
)

qt_import_qml_plugins(test_editor)

add_test(NAME test_editor COMMAND test_editor)
# GUI 红线：自动化验证一律 offscreen，无可见窗口
set_tests_properties(test_editor PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
