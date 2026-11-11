cmake_minimum_required(VERSION 3.21)

project(qcnote VERSION 1.0.0 LANGUAGES C CXX)

# 静态 CRT（/MT，Debug 配置 /MTd）：单 exe 免安装、无 vcruntime/msvcp DLL 依赖（需求「部署」验收项）
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 单配置生成器（Ninja）默认 Release：本工具定位小体积绿色工具
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

# Release 优先小体积：把默认 /O2 换成 /O1（含 /Os，体积优先优化）
foreach(lang C CXX)
    string(REPLACE "/O2" "/O1" CMAKE_${lang}_FLAGS_RELEASE "${CMAKE_${lang}_FLAGS_RELEASE}")
endforeach()

# ---------------------------------------------------------------------------
# vendored 依赖（vendor/ 目录内容只读，禁止修改）
# ---------------------------------------------------------------------------

# WIL：微软官方 Win32 RAII 头文件库（header-only，INTERFACE 目标）
add_library(wil INTERFACE)
target_include_directories(wil INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/wil/include")

# Scintilla 5.6.9：源码静态编译进项目（官方 win32/makefile 的 LIBSCI 目标集合）
#   src/*.cxx 全部（含 ScintillaBase.cxx）
#   win32/ 六个 TU：PlatWin / ScintillaWin / HanjaDic / ListBox / SurfaceGDI / SurfaceD2D
#     （ListBoxX_Register 由 PlatWin 调用、Surface::Allocate 定义在 SurfaceD2D、
#       SurfaceGDI_Allocate 由 Allocate 调用——缺任一即链接失败）
#   排除 ScintillaDLL.cxx（DLL 入口，静态链接不需要）
# 注意：Scintilla TU 用 /std:c++20 单独编译（上游按 C++17+ 编写），应用层为 C++23；
#       两层间只有 extern "C" 接口（Scintilla_RegisterClasses），无 STL 类型跨界，无 ODR 问题。
file(GLOB SCINTILLA_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/scintilla/src/*.cxx")
add_library(scintilla_lib STATIC
    ${SCINTILLA_CORE_SOURCES}
    vendor/scintilla/win32/PlatWin.cxx
    vendor/scintilla/win32/ScintillaWin.cxx
    vendor/scintilla/win32/HanjaDic.cxx
    vendor/scintilla/win32/ListBox.cxx
    vendor/scintilla/win32/SurfaceGDI.cxx
    vendor/scintilla/win32/SurfaceD2D.cxx
)
# include/ 对外（Scintilla.h 注册 API）；src/ 仅 Scintilla 内部头文件自用
target_include_directories(scintilla_lib
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/scintilla/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/scintilla/src"
)
target_compile_definitions(scintilla_lib
    PRIVATE _CRT_SECURE_NO_DEPRECATE            # 官方 vcxproj 同款定义
    PRIVATE $<$<CONFIG:Debug>:DEBUG>            # 对齐官方 makefile 的 -DDEBUG（Debug 构建）
)
target_compile_options(scintilla_lib PRIVATE
    /std:c++20
    /utf-8                                      # 官方 vcxproj 用 /source-charset:utf-8，Scintilla 源码为无 BOM UTF-8
)
# Scintilla 平台层用到的系统库（官方 makefile LIBS 同款；D2D/DWrite 为运行时动态加载无需链接）。
# INTERFACE 使链接 scintilla_lib 的目标自动带上，应用层不必感知。
target_link_libraries(scintilla_lib INTERFACE gdi32 user32 imm32 ole32 oleaut32 uuid advapi32)

# Catch2 v3.8.1：vendor 快照缺少上游仓库的 CMake/ 脚本目录（CatchConfigOptions.cmake 等），
# 其自带 CMakeLists 无法 add_subdirectory；且 vendor/ 禁止修改、不能补文件。
# 处理：直接从 vendor/catch2/src 源码构建静态库，等价复刻官方 Catch2::Catch2 / Catch2::Catch2WithMain。
# 注：catch_test_macros.hpp 强制包含 <catch2/catch_user_config.hpp>，该头由 .in 模板 configure 生成到构建目录。
set(CATCH_CONFIG_CONSOLE_WIDTH 80)        # 官方默认值；不设会生成空值宏导致编译失败
set(CATCH_CONFIG_DEFAULT_REPORTER "console")  # 上游缺失的 CMake/ 脚本负责给默认值；空串会导致运行期报「No reporter registered with name: ''」
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/catch2/src/catch2/catch_user_config.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/catch2/catch_user_config.hpp"
    @ONLY)
# 注：catch2 v3 源码全部位于 src/catch2/ 子目录（src 顶层无 .cpp），RECURSE glob 已收齐
file(GLOB_RECURSE CATCH2_INTERNAL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/catch2/src/catch2/*.cpp")
list(FILTER CATCH2_INTERNAL_SOURCES EXCLUDE REGEX "catch_main\\.cpp$")
add_library(catch2_lib STATIC ${CATCH2_INTERNAL_SOURCES})
target_include_directories(catch2_lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/catch2/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
# Catch2 官方要求 C++17 起；随项目基线用 c++20
target_compile_options(catch2_lib PRIVATE /std:c++20 /utf-8)
# WithMain 变体：仅追加默认 main 入口的 TU（官方 Catch2WithMain 同构，避免整套源码编两遍）
add_library(catch2_with_main STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/catch2/src/catch2/internal/catch_main.cpp")
target_compile_options(catch2_with_main PRIVATE /std:c++20 /utf-8)
target_link_libraries(catch2_with_main PUBLIC catch2_lib)
# 对外提供与官方包同名的目标，后续里程碑与文档引用保持一致
add_library(Catch2::Catch2 ALIAS catch2_lib)
add_library(Catch2::Catch2WithMain ALIAS catch2_with_main)

# PCRE2 10.45（vendored，DEC-012）：8-bit 静态库、仅解释执行（不启用 JIT）。
# vendor/pcre2 的官方 CMakeLists 依赖未随快照提供的仓库根文件（configure.ac、
# cmake/ 等），不能 add_subdirectory（与 Catch2 同处理法）：按官方 CMakeLists
# 的 PCRE2_SOURCES 集合自建目标，仅编 8-bit 组件（无 pcre2posix* / pcre2grep /
# pcre2test / pcre2_dftables / pcre2_fuzzsupport；pcre2_ucptables.c 由
# pcre2_tables.c #include，不单独编译；pcre2_jit_compile.c 含无 JIT 时的公共
# API 桩函数，保留编译）。
# 生成物进构建目录（vendor/ 只读）：pcre2.h ← pcre2.h.generic（COPYONLY）、
# pcre2_chartables.c ← pcre2_chartables.c.dist（C locale 默认表）。
# 不定义 HAVE_CONFIG_H（不使用 config.h）：以 config.h.generic 的默认宏值经
# target_compile_definitions 提供——SUPPORT_UNICODE 开启 UTF/Unicode 支持
#（编译期开关，PCRE2_UTF 运行时可用），LINK_SIZE/NEWLINE_DEFAULT 等取 generic
# 默认值；PCRE2_EXPORT= 空导出修饰（静态链接，pcre2_internal.h 在 _WIN32 +
# PCRE2_STATIC 下引用该宏）。
set(PCRE2_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/pcre2")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/vendor/pcre2/src/pcre2.h.generic"
               "${PCRE2_GEN_DIR}/pcre2.h" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/vendor/pcre2/src/pcre2_chartables.c.dist"
               "${PCRE2_GEN_DIR}/pcre2_chartables.c" COPYONLY)
add_library(pcre2_lib STATIC
    "${PCRE2_GEN_DIR}/pcre2_chartables.c"
    vendor/pcre2/src/pcre2_auto_possess.c
    vendor/pcre2/src/pcre2_chkdint.c
    vendor/pcre2/src/pcre2_compile.c
    vendor/pcre2/src/pcre2_compile_class.c
    vendor/pcre2/src/pcre2_config.c
    vendor/pcre2/src/pcre2_context.c
    vendor/pcre2/src/pcre2_convert.c
    vendor/pcre2/src/pcre2_dfa_match.c
    vendor/pcre2/src/pcre2_error.c
    vendor/pcre2/src/pcre2_extuni.c
    vendor/pcre2/src/pcre2_find_bracket.c
    vendor/pcre2/src/pcre2_jit_compile.c
    vendor/pcre2/src/pcre2_maketables.c
    vendor/pcre2/src/pcre2_match.c
    vendor/pcre2/src/pcre2_match_data.c
    vendor/pcre2/src/pcre2_newline.c
    vendor/pcre2/src/pcre2_ord2utf.c
    vendor/pcre2/src/pcre2_pattern_info.c
    vendor/pcre2/src/pcre2_script_run.c
    vendor/pcre2/src/pcre2_serialize.c
    vendor/pcre2/src/pcre2_string_utils.c
    vendor/pcre2/src/pcre2_study.c
    vendor/pcre2/src/pcre2_substitute.c
    vendor/pcre2/src/pcre2_substring.c
    vendor/pcre2/src/pcre2_tables.c
    vendor/pcre2/src/pcre2_ucd.c
    vendor/pcre2/src/pcre2_valid_utf.c
    vendor/pcre2/src/pcre2_xclass.c
)
target_include_directories(pcre2_lib
    PUBLIC "${PCRE2_GEN_DIR}"                                # pcre2.h（消费者）
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/pcre2/src"   # 内部头（pcre2_internal.h 等）
)
target_compile_definitions(pcre2_lib
    PUBLIC PCRE2_CODE_UNIT_WIDTH=8 PCRE2_STATIC              # 消费者包含 pcre2.h 所需
    PRIVATE SUPPORT_UNICODE LINK_SIZE=2 HEAP_LIMIT=20000000
            MATCH_LIMIT=10000000 MATCH_LIMIT_DEPTH=10000000
            MAX_VARLOOKBEHIND=255 NEWLINE_DEFAULT=2 PARENS_NEST_LIMIT=250
            MAX_NAME_SIZE=128 MAX_NAME_COUNT=10000
            HAVE_MEMMOVE _CRT_SECURE_NO_DEPRECATE _CRT_SECURE_NO_WARNINGS
    PRIVATE PCRE2_EXPORT=                                    # 空导出修饰（静态链接）
)
# 上游 C 代码在 MSVC /W3 下有少量转换类告警，按任务口径放宽抑制
target_compile_options(pcre2_lib PRIVATE /wd4996 /wd4244 /wd4267 /wd4018)

# ---------------------------------------------------------------------------
# 应用（M0：Win32 空壳窗口 + Scintilla 静态链接验证）
# ---------------------------------------------------------------------------
add_executable(qcnote WIN32
    src/main.cpp
    src/platform/singleinstance.cpp
    src/platform/shellintegration.cpp
    src/ui/theme.cpp
    src/ui/titlebar.cpp
    src/ui/tabs.cpp
    src/ui/welcome.cpp
    src/ui/menu.cpp
    src/ui/mainmenu.cpp
    src/ui/toolbar.cpp
    src/ui/statusbar.cpp
    src/ui/tooltip.cpp
    src/ui/dialog.cpp
    src/ui/searchdialog.cpp
    src/editor/scintillaedit.cpp
    src/editor/multicursorcontroller.cpp
    src/editor/searchcontroller.cpp
    src/qcnote.rc
)
# MSVC 19.51（VS2026）无 /std:c++23 开关（仅 c++14/17/20/latest）；
# /std:c++latest 为 _MSVC_LANG=202400（C++26 草案模式，完整含 C++23 特性），以此达成 C++23 选型意图
target_compile_options(qcnote PRIVATE /std:c++latest /utf-8)
# WINVER/_WIN32_WINNT=Win10：WM_NCMOUSEWHEEL 等消息常量可用（目标平台即 Win10/11）
target_compile_definitions(qcnote PRIVATE UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)  # NOMINMAX：std::min/max/clamp 可用
# comctl32：Tab 条/工具栏/状态栏 tooltip 控件；comdlg32：打开/另存为系统对话框；
# shell32：CommandLineToArgvW（D2 启动参数）；qcnote_core：M2-A 纯逻辑核心层；
# advapi32：M5 shell 集成注册表读写（scintilla_lib INTERFACE 亦传递，显式声明
# 使 platform 层依赖意图自包含）；
# gdiplus：F2 工具栏图标 GDI+ 抗锯齿绘制（系统组件，无额外部署负担）
target_link_libraries(qcnote PRIVATE scintilla_lib wil dwmapi comctl32 comdlg32 shell32 advapi32 qcnote_core gdiplus)
# manifest 由 qcnote.rc 嵌入（PerMonitorV2 DPI 声明）；显式关闭链接器/mt 的
# manifest 生成，避免默认 manifest 覆盖 rc 嵌入的资源
target_link_options(qcnote PRIVATE /MANIFEST:NO)
# .rc 中图标相对路径（icons/qcnote.ico）依赖 rc.exe 的包含搜索路径：传入 src/ 目录
target_include_directories(qcnote PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ---------------------------------------------------------------------------
# M2-A 纯逻辑核心层（src/core/）：文档/格式/存储三层 + 编码/换行管线。
# 无 UI/Scintilla 依赖；qcnote 应用目标在 M2-B 接线时链接 qcnote_core。
# ---------------------------------------------------------------------------
add_library(qcnote_core STATIC
    src/core/textutil.cpp
    src/core/encoding.cpp
    src/core/storage.cpp
    src/core/localstorage.cpp
    src/core/format.cpp
    src/core/document.cpp
    src/core/documentmanager.cpp
    src/core/searchengine.cpp
    src/core/multicursor.cpp
    src/core/inifile.cpp
    src/core/sessionmanager.cpp
    src/core/draftcachemanager.cpp
)
# PUBLIC 传播到 qcnote 应用与测试目标（沿用应用层统一编译口径）
target_compile_options(qcnote_core PUBLIC /std:c++latest /utf-8)
target_compile_definitions(qcnote_core PUBLIC UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_include_directories(qcnote_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# bcrypt：SHA-256 内容指纹（BCryptHashData）；ole32：CoCreateGuid/StringFromGUID2（uid）+
# CoTaskMemFree（SHGetKnownFolderPath）；shell32：SHGetKnownFolderPath（会话目录解析）
target_link_libraries(qcnote_core PUBLIC bcrypt ole32 shell32)
# M3-A 查找引擎：PCRE2-8 静态库（仅 searchengine.cpp 使用，PRIVATE——头文件不
# 泄 PCRE2 类型；静态库链接依赖自动传播到 qcnote 应用与测试目标）
target_link_libraries(qcnote_core PRIVATE pcre2_lib)
# M4-A 草稿 JSON（vendored nlohmann v3.12.0）：仅 draftcachemanager.cpp 内部使用，
# PRIVATE——头文件不泄 nlohmann 类型，消费者无需 include 路径
target_include_directories(qcnote_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor")

# ---------------------------------------------------------------------------
# 测试（Catch2 v3；后续里程碑在 tests/ 下按模块重建单测）
# ---------------------------------------------------------------------------
enable_testing()
add_executable(test_smoke tests/test_smoke.cpp)
# Catch2WithMain 自带 main 入口；同样使用 /MT 静态 CRT（随 CMP0091 全局设置）
target_compile_options(test_smoke PRIVATE /std:c++latest /utf-8)
target_link_libraries(test_smoke PRIVATE Catch2::Catch2WithMain)
add_test(NAME smoke COMMAND test_smoke)

# M2-A U1 单测：编码识别/编解码（TC-U01~U07）、换行归一与 BOM 写回（TC-U08/U09）、
# 原子写、文档生命周期/遮蔽重载/编码转换/回调。直接链接 qcnote_core。
add_executable(test_core
    tests/test_core_encoding.cpp
    tests/test_core_storage.cpp
    tests/test_core_document.cpp
)
# _CRT_SECURE_NO_WARNINGS：getenv（测试临时目录环境变量）；CATCH_CONFIG_WCHAR：wstring 断言输出
target_compile_definitions(test_core PRIVATE _CRT_SECURE_NO_WARNINGS CATCH_CONFIG_WCHAR)
target_link_libraries(test_core PRIVATE Catch2::Catch2WithMain qcnote_core)
add_test(NAME core COMMAND test_core)
# 测试临时文件目录指向本任务临时区（缺失时测试内部回退 %TEMP%\qcnote-test-core）
set_tests_properties(core PROPERTIES ENVIRONMENT
    "QCNOTE_TEST_TMP=D:/temp/aiRunner/opencode/qcnote-win32-rewrite-20261110183000/test-core")

# M1 UI 基建单测：主题 token 数学（Scale/BlendOver）、标题栏布局、
# 按钮/边缘命中（含 DPI 200% 换算正确性的代码级验证）。
# M2-B 扩展：状态栏字段文本 + 对话框高度纯函数。
# M6 扩展（E5）：深色色板取值照抄断言 + SetTheme/CurrentPalette 分发 + 深色
# 半透明 token 的 BlendOver 预合成。
# 直接编译 src/ui 源码（纯函数部分不依赖窗口存在）；statusbar 依赖 tooltip
#（comctl32）、dialog 依赖 dwmapi（DWMWCP 圆角 + theme.cpp 的 DWM 暗色联动）；
# toolbar 依赖 theme/tooltip/commands.h（头文件即用）与 gdiplus（F2 图标
# GDI+ 路径；单测经真实弹出窗口宿主驱动 attach/交互）。
add_executable(test_ui tests/test_ui.cpp src/ui/theme.cpp src/ui/titlebar.cpp
                           src/ui/statusbar.cpp src/ui/tooltip.cpp src/ui/dialog.cpp
                           src/ui/searchdialog.cpp src/ui/toolbar.cpp)
target_compile_options(test_ui PRIVATE /std:c++latest /utf-8)
# NOMINMAX：statusbar/dialog 用 std::min/max；WINVER=Win10 同应用目标
target_compile_definitions(test_ui PRIVATE UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_include_directories(test_ui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(test_ui PRIVATE Catch2::Catch2WithMain gdi32 user32 dwmapi comctl32 gdiplus)
add_test(NAME ui COMMAND test_ui)

# M1-2 菜单系统单测：MenuModel 数据模型（结构/级联/勾选/enabled/动态条目）、
# ComputeMenuLayout 布局数学（列预留/DPI 换算/极值夹取/分隔行）、主菜单内容构建。
# menu.cpp 含弹出层窗口代码（链接需要 user32/gdi32/dwmapi），单测只调用纯逻辑部分。
add_executable(test_menu tests/test_menu.cpp src/ui/menu.cpp src/ui/mainmenu.cpp src/ui/theme.cpp)
target_compile_options(test_menu PRIVATE /std:c++latest /utf-8)
target_compile_definitions(test_menu PRIVATE UNICODE _UNICODE NOMINMAX)
target_include_directories(test_menu PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(test_menu PRIVATE Catch2::Catch2WithMain gdi32 user32 dwmapi)
add_test(NAME menu COMMAND test_menu)

# M1-3 Tab 条单测：宽度自适应算法（DistributeTabWidths 全分支 + 向下取整边界）、
# 几何与命中（ComputeTabStripGeometry/TabStripHitTest 纯函数）、右键菜单模型
# （A3 动态数量与置灰）、无障碍名称、GDI 中英文测量（自然宽数据来源，屏幕 DC）。
# tabs.cpp 引用 titlebar（tabArea 几何）与 menu（MenuModel）；tooltip 路径仅在
# attach 后可达，单测不需要窗口。
add_executable(test_tabs tests/test_tabs.cpp src/ui/tabs.cpp src/ui/menu.cpp
                          src/ui/theme.cpp src/ui/titlebar.cpp)
target_compile_options(test_tabs PRIVATE /std:c++latest /utf-8)
target_compile_definitions(test_tabs PRIVATE UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_include_directories(test_tabs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(test_tabs PRIVATE Catch2::Catch2WithMain gdi32 user32 dwmapi comctl32)
add_test(NAME tabs COMMAND test_tabs)

# M3-A U2/U4 单测：查找引擎（TC-U10~U17：纯文本/正则/无效模式/零长度跳过/
# >10000 截断/替换展开/全部替换/环绕导航）与多光标纯逻辑（TC-U27/U28 纯化
# 部分：Ctrl+D 严格重叠端点相接追加、Ctrl+Shift+L 全部实例、Ctrl+Alt+Up/Down
# 字符列保持、Shift+Alt+I 行尾集合、Esc 收缩、粘贴分发判定、wordRangeAt
# 中英词边界）。数据内联 UTF-8 字符串，直接链接 qcnote_core（含 PCRE2）。
add_executable(test_search tests/test_search.cpp)
# CATCH_CONFIG_WCHAR：errorMessage（wstring）断言输出
target_compile_definitions(test_search PRIVATE _CRT_SECURE_NO_WARNINGS CATCH_CONFIG_WCHAR)
target_link_libraries(test_search PRIVATE Catch2::Catch2WithMain qcnote_core)
add_test(NAME search COMMAND test_search)

# M4-A U3 单测：会话与草稿持久化层（TC-U18~U26）——INI 读写基建（QSettings
# IniFormat 兼容转义/原子写）、防抖器（时钟注入）、便携路径解析、会话序列化/
# 容错/几何校验、草稿条目序列化/恢复分类计划（RestoreOutcome，不操作管理器）/
# 同 URL 去重/清理管线/关闭现场还原稳定插入、baselineHash 稳定性。
# 依赖 qcnote_core（nlohmann 仅库内部 PRIVATE 使用，测试不需要其 include 路径）。
add_executable(test_session tests/test_session.cpp)
target_compile_definitions(test_session PRIVATE _CRT_SECURE_NO_WARNINGS CATCH_CONFIG_WCHAR)
target_link_libraries(test_session PRIVATE Catch2::Catch2WithMain qcnote_core)
add_test(NAME session COMMAND test_session)
# 测试临时目录指向本任务临时区（缺失时测试内部回退 %TEMP%\qcnote-test-core）
set_tests_properties(session PROPERTIES ENVIRONMENT
    "QCNOTE_TEST_TMP=D:/temp/aiRunner/opencode/qcnote-win32-rewrite-20261110183000/test-session")

# M5 平台层单测：单实例（TC-I04 降级路径的可测注入部分）与 shell 集成（TC-I11
# 单元级）——互斥量名派生（基础域全局唯一/userdir 注入域）、WM_COPYDATA 载荷
# 编解码（空列表/空格中文路径/坏帧拒绝）、主窗口域标签选择（枚举注入）、
# TryForwardToPrimary 真实降级契约、位置参数绝对化（D2）、shell 注册表键值
# 精确断言（注入随机子树，RAII 清理，全程不碰真实键）。
# 直接编译 src/platform 源码（user32：窗口枚举/消息；advapi32：注册表）。
add_executable(test_platform
    tests/test_platform.cpp
    src/platform/singleinstance.cpp
    src/platform/shellintegration.cpp
)
target_compile_options(test_platform PRIVATE /std:c++latest /utf-8)
target_compile_definitions(test_platform PRIVATE UNICODE _UNICODE NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00 CATCH_CONFIG_WCHAR)
target_include_directories(test_platform PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(test_platform PRIVATE Catch2::Catch2WithMain user32 advapi32)
add_test(NAME platform COMMAND test_platform)
