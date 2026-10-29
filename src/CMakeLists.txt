# ------------------------------------------------------------------
# 核心静态库 + QML 模块（URI qcnote）
# 测试与可执行文件共用这一份核心代码，不重复编译源文件
# ------------------------------------------------------------------
qt_add_library(qcnote_core STATIC
    document/document.h
    document/document.cpp
    document/documentlistmodel.h
    document/documentlistmodel.cpp
    document/documentmanager.h
    document/documentmanager.cpp
    format/documentformat.h
    format/plaintextformat.h
    format/plaintextformat.cpp
    storage/documentstorage.h
    storage/localstorage.h
    storage/localstorage.cpp
)

target_include_directories(qcnote_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# QML 类型注册生成文件（qcnote_core_qmltyperegistrations.cpp）按 moc 记录的
# 裸文件名包含 QML_ELEMENT 头（如 <document.h>），因此将头文件子目录加入
# 库内部 include 路径（PRIVATE：不对外暴露扁平命名空间）
target_include_directories(qcnote_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/document
    ${CMAKE_CURRENT_SOURCE_DIR}/format
    ${CMAKE_CURRENT_SOURCE_DIR}/storage
)

target_link_libraries(qcnote_core PUBLIC Qt6::Quick Qt6::QuickControls2 Qt6::QuickDialogs2)

# QML 文件放在 qml/ 子目录，QTP0004 NEW 自动生成各子目录的 qmldir
qt_policy(SET QTP0004 NEW)
# 采用默认资源前缀 :/qt/qml/（Qt 推荐约定）
qt_policy(SET QTP0001 NEW)

# 模块输出目录需以目标路径（qcnote）结尾，保证 qmllint 等工具可用
set(QT_QML_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# AppTheme 注册为 QML 单例（pragma Singleton + qmldir singleton 声明）
# 注意：QT_QML_SINGLETON_TYPE 是源文件属性，必须在 qt_add_qml_module 之前设置
set_source_files_properties(qml/AppTheme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(qcnote_core
    URI qcnote
    VERSION 1.0
    QML_FILES
        qml/AppTheme.qml
        qml/CaptionButton.qml
        qml/DocumentTabs.qml
        qml/EditorArea.qml
        qml/Main.qml
        qml/StatusBar.qml
        qml/TitleBar.qml
        qml/Toolbar.qml
        qml/WelcomePage.qml
    RESOURCES
        qml/icons/new.svg
        qml/icons/open.svg
        qml/icons/save.svg
)

# ------------------------------------------------------------------
# 可执行目标（WIN32 子系统，无控制台窗口）
# 静态 QML 模块需链接自动生成的插件目标并显式导入
# ------------------------------------------------------------------
qt_add_executable(qcnote WIN32
    main.cpp
)

target_link_libraries(qcnote PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    qcnote_coreplugin
)

qt_import_qml_plugins(qcnote)
