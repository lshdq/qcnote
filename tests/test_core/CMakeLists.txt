# 单元测试目标（控制台程序）：链接同一份核心静态库 qcnote_core，
# 不重复编译核心源文件。
qt_add_executable(test_core
    main.cpp
    tst_document.cpp
    tst_documentmanager.cpp
    tst_encodingdetector.cpp
    tst_gbkcodec.cpp
    tst_localstorage.cpp
    tst_multicursorengine.cpp
    tst_sessionmanager.cpp
    tst_shellintegration.cpp
    tst_singleinstance.cpp
)

target_link_libraries(test_core PRIVATE
    Qt6::Test
    qcnote_core
)

add_test(NAME test_core COMMAND test_core)
# 构造级用例需在 build/ 根（与 qcnote.exe 及其他测试可执行文件同目录）短暂放置
# qcnote.portable 标记，ctest -j 并行调度下可能与 test_window/test_sessionrestore
# （经 QML 单例空路径构造 SessionManager）时间重叠导致其误入便携模式，故串行执行。
set_tests_properties(test_core PROPERTIES RUN_SERIAL TRUE)
