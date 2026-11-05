# 正则搜索与替换 QML 集成测试目标（offscreen）：实例化完整 Main.qml
# （SearchDialog + EditorArea 会话级 SearchController + SearchHighlightLayer
# 完整接线），经 QMetaObject 调用 Main QML function 与合成键盘事件驱动
# 控制器层 TC-101~TC-103 与人工交互用例 TC-201~TC-206 的可自动化部分。
qt_add_executable(test_searchui
    tst_searchuiintegration.cpp
)

# qcnote_coreplugin：静态 QML 模块插件（import qcnote 所需）
target_link_libraries(test_searchui PRIVATE
    Qt6::Test
    Qt6::Quick
    qcnote_core
    qcnote_coreplugin
)

qt_import_qml_plugins(test_searchui)

add_test(NAME test_searchui COMMAND test_searchui)
# GUI 红线：自动化验证一律 offscreen，无可见窗口
set_tests_properties(test_searchui PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
