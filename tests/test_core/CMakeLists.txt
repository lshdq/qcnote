# 单元测试目标（控制台程序）：链接同一份核心静态库 qcnote_core，
# 不重复编译核心源文件。
qt_add_executable(test_core
    main.cpp
    tst_document.cpp
    tst_documentmanager.cpp
    tst_localstorage.cpp
    tst_multicursorengine.cpp
)

target_link_libraries(test_core PRIVATE
    Qt6::Test
    qcnote_core
)

add_test(NAME test_core COMMAND test_core)
