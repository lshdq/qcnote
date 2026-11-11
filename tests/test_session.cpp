// M4-A U3 单测：会话与草稿持久化层（TC-U18~U26）。
//
// 语义基准：docs/需求文档/需求-Win32全量重写.md C1~C4/D5 + 旧版实现
//（git HEAD src/document/sessionmanager / draftcachemanager）与决策记录
// DEC-001~009（关闭现场还原 / 未保存修改临时缓存 / 退出免提示）。
// 覆盖：INI 读写基建（QSettings IniFormat 兼容转义）、防抖器、便携路径解析、
// 会话序列化/容错/几何校验、草稿条目序列化/恢复分类/去重/清理、关闭现场
// 还原算法、baselineHash 稳定性。全部临时目录 RAII（test_core_common）+
// 磁盘状态/时钟/屏幕注入，不碰真实用户目录。
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/documentmanager.h"
#include "core/draftcachemanager.h"
#include "core/inifile.h"
#include "core/sessionmanager.h"
#include "test_core_common.h"

using namespace core;

namespace {

// —— 磁盘状态 mock：url → 归一化内容哈希（不在表 = 缺失/不可读）——
using DiskMap = std::map<std::filesystem::path, std::string>;

DiskHashQuery diskQuery(const DiskMap& map) {
    return [&map](const std::filesystem::path& url) -> std::optional<std::string> {
        const auto it = map.find(url);
        if (it == map.end())
            return std::nullopt;
        return it->second;
    };
}

// —— 会话条目构造 ——
SessionDocumentEntry urlEntry(std::filesystem::path url, int position, int cursor = 0,
                              std::optional<TextEncoding> encoding = std::nullopt) {
    SessionDocumentEntry e;
    e.url = std::move(url);
    e.position = position;
    e.cursor = cursor;
    e.encoding = encoding;
    return e;
}

SessionDocumentEntry uidEntry(std::string uid, int position) {
    SessionDocumentEntry e;
    e.uid = std::move(uid);
    e.position = position;
    return e;
}

// —— 草稿条目构造/落盘 ——
DraftEntry makeEntry(std::string uid, std::filesystem::path url, std::wstring name,
                     std::string content, int cursor, long long savedAt,
                     std::string baselineHash, LineEnding le = LineEnding::Lf,
                     TextEncoding enc = TextEncoding::Utf8, bool bom = false) {
    DraftEntry e;
    e.uid = std::move(uid);
    e.url = std::move(url);
    e.untitledName = std::move(name);
    e.content = std::move(content);
    e.cursor = cursor;
    e.savedAt = savedAt;
    e.baselineHash = std::move(baselineHash);
    e.lineEnding = le;
    e.encoding = enc;
    e.hasBom = bom;
    return e;
}

void writeEntryFile(const std::filesystem::path& dir, const DraftEntry& e) {
    REQUIRE(DraftCacheManager::persist(dir, e));
}

std::filesystem::path entryFile(const std::filesystem::path& dir, const std::string& uid) {
    return DraftCacheManager::entryPath(dir, uid);
}

bool fileExists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

RECT rect(LONG l, LONG t, LONG r, LONG b) { return RECT{l, t, r, b}; }

} // namespace

// ===========================================================================
// INI 基建
// ===========================================================================

TEST_CASE("IniFile：键值转义纯函数（QSettings IniFormat 对齐）", "[session][ini]") {
    // 键：'/'→'\'（旧版 [documents] 1/url 落盘即 "1\url"）；安全字符原样
    REQUIRE(IniFile::escapeKey("1/url") == "1\\url");
    REQUIRE(IniFile::escapeKey("documents") == "documents");
    REQUIRE(IniFile::escapeKey("a.b-c_d9") == "a.b-c_d9");
    // ≤U+00FF → %XX 大写；>U+00FF → %UXXXX
    REQUIRE(IniFile::escapeKey("a;b") == "a%3Bb");
    REQUIRE(IniFile::escapeKey("中") == "%U4E2D");
    REQUIRE(IniFile::unescapeKey("1\\url") == "1/url");
    REQUIRE(IniFile::unescapeKey("a%3Bb") == "a;b");
    REQUIRE(IniFile::unescapeKey("%U4E2D") == "中");
    REQUIRE(IniFile::unescapeKey("documents") == "documents");
    // 读侧对未转义 '/' 键同样兼容（新旧文件两读）
    REQUIRE(IniFile::unescapeKey("1/url") == "1/url");

    // 值：含 ;,= → 引号包裹；首尾空格 → 引号包裹；反斜杠助记转义
    REQUIRE(IniFile::escapeValue("a=b;c,d") == "\"a=b;c,d\"");
    REQUIRE(IniFile::escapeValue(" spaced ") == "\" spaced \"");
    REQUIRE(IniFile::escapeValue("plain") == "plain");
    REQUIRE(IniFile::escapeValue("line1\nline2") == "line1\\nline2");
    REQUIRE(IniFile::escapeValue("a\"b\\c") == "a\\\"b\\\\c");
    REQUIRE(IniFile::escapeValue("tab\there") == "tab\\there");
    // 控制字符 \xHH（小写不补零）；后跟十六进制字符强制再转义（读侧消歧）
    REQUIRE(IniFile::escapeValue("\x01") == "\\x1");
    REQUIRE(IniFile::escapeValue("\x1A" "b") == "\\x1a\\x62");
    REQUIRE(IniFile::unescapeValue("\\x1a\\x62") == "\x1A" "b");
    REQUIRE(IniFile::unescapeValue("\"a=b;c,d\"") == "a=b;c,d");
    REQUIRE(IniFile::unescapeValue("line1\\nline2") == "line1\nline2");
    REQUIRE(IniFile::unescapeValue("a\\\"b\\\\c") == "a\"b\\c");
    // 未引号值尾部空白截断；引导空白不属于值
    REQUIRE(IniFile::unescapeValue("plain  ") == "plain");
    REQUIRE(IniFile::unescapeValue("  lead") == "lead");

    // 往返电池（含中文/特殊文件名字符/控制字符/引号/反斜杠）
    const std::vector<std::string> battery = {
        "C:/a b/中;文.txt=x,y", "file:///C:/Users/%E4%B8%AD.txt", "a\\b\\c",
        "\"quoted\"", "  edge  ", "ctl\x1f\x02x", "未命名-1", "100%", "a,b;c=d",
    };
    for (const std::string& s : battery) {
        INFO("roundtrip: " << s);
        REQUIRE(IniFile::unescapeValue(IniFile::escapeValue(s)) == s);
        REQUIRE(IniFile::unescapeKey(IniFile::escapeKey(s)) == s);
    }
}

TEST_CASE("IniFile：读写往返与文件形态", "[session][ini]") {
    testutil::TempDir tmp(L"ini-io");
    const std::filesystem::path ini = tmp.file(L"qcnote.ini");

    IniData data;
    data.setValue("documents", "size", "2");
    data.setValue("documents", "1/url", "C:/a b/中;文.txt");
    data.setValue("documents", "1/cursor", "42");
    data.setValue("documents", "1/encoding", "utf8");
    data.setValue("documents", "2/uid", "11111111-2222-3333-4444-555555555555");
    data.setValue("documents", "activeUid", "11111111-2222-3333-4444-555555555555");
    data.setValue("window", "maximized", "true");
    REQUIRE(IniFile::write(ini, data));

    // 文件形态：UTF-8 无 BOM、CRLF、键中 '/' 落盘为 '\'、值含 ;= 引号包裹
    const std::string raw = testutil::readBytes(ini);
    REQUIRE(raw.substr(0, 1) == "[");
    REQUIRE(raw.find("\r\n") != std::string::npos);
    REQUIRE(raw.find("1\\url=\"C:/a b/中;文.txt\"") != std::string::npos);
    REQUIRE(raw.find("1\\cursor=42") != std::string::npos);
    REQUIRE(raw.find("size=2") != std::string::npos);

    auto back = IniFile::read(ini);
    REQUIRE(back.has_value());
    REQUIRE(back->value("documents", "1/url") == "C:/a b/中;文.txt");
    REQUIRE(back->value("documents", "1/cursor") == "42");
    REQUIRE(back->value("documents", "activeUid") == "11111111-2222-3333-4444-555555555555");
    // 查找 ASCII 大小写不敏感（QSettings IniCaseSensitivity）
    REQUIRE(back->value("DOCUMENTS", "1/URL") == "C:/a b/中;文.txt");
    REQUIRE(back->value("documents", "SIZE") == "2");
    REQUIRE(back->value("window", "maximized") == "true");
    REQUIRE(back->value("documents", "active") == std::nullopt);
    REQUIRE(back->value("nope", "key") == std::nullopt);

    // 不存在的文件 → nullopt（与空文件区分）
    REQUIRE_FALSE(IniFile::read(tmp.file(L"absent.ini")).has_value());

    // 父目录自动创建（用户目录首启）
    const std::filesystem::path nested = tmp.path / L"sub" / L"dir" / L"qcnote.ini";
    IniData editorOnly;
    editorOnly.setValue("editor", "wordWrap", "false");
    REQUIRE(IniFile::write(nested, editorOnly));
    REQUIRE(fileExists(nested));

    // 原子替换：旧内容被整体替换，无残留键
    IniData replace;
    replace.setValue("documents", "size", "0");
    REQUIRE(IniFile::write(ini, replace));
    auto after = IniFile::read(ini);
    REQUIRE(after->value("documents", "size") == "0");
    REQUIRE(after->findSection("window") == nullptr); // 旧节被替换掉
}

TEST_CASE("IniFile：读侧宽容（注释/续行/重复键/General/无等号行）", "[session][ini]") {
    testutil::TempDir tmp(L"ini-tolerant");
    const std::filesystem::path ini = tmp.file(L"qcnote.ini");
    testutil::writeBytes(ini,
        "; 整行注释\r\n"
        "[documents]\r\n"
        "size=2\r\n"
        "1\\url=C:/a.txt ; 行内注释\r\n"
        "2\\url=C:/long-\\\r\n"          // 行尾反斜杠续行
        "path/b.txt\r\n"
        "垃圾行没有等号\r\n"
        "dup=first\r\n"
        "dup=second\r\n"
        "[General]\r\n"
        "top=1\r\n"
        "[%General]\r\n"
        "literal=2\r\n");
    auto data = IniFile::read(ini);
    REQUIRE(data.has_value());
    REQUIRE(data->value("documents", "size") == "2");
    REQUIRE(data->value("documents", "1/url") == "C:/a.txt"); // 行内注释截断+尾空格清理
    REQUIRE(data->value("documents", "2/url") == "C:/long-path/b.txt"); // 续行拼接
    REQUIRE(data->value("documents", "dup") == "second");               // 重复键后者覆盖
    REQUIRE(data->value("", "top") == "1");          // [General] → 空节名
    REQUIRE(data->value("general", "literal") == "2"); // [%General] → 字面 general 节
    // BOM 容忍
    testutil::writeBytes(tmp.file(L"bom.ini"), "\xEF\xBB\xBF[a]\r\nk=v\r\n");
    auto bomData = IniFile::read(tmp.file(L"bom.ini"));
    REQUIRE(bomData->value("a", "k") == "v");
}

TEST_CASE("IniFile：QSettings 旧资产兼容样例（Qt 版写出的 qcnote.ini）", "[session][ini]") {
    testutil::TempDir tmp(L"ini-qt");
    const std::filesystem::path ini = tmp.file(L"qcnote.ini");
    // 旧 sessionmanager 经 QSettings IniFormat 写出的形态：CRLF、键 "N\url"、
    // url 值为 QUrl::toString()（file:/// + 百分号编码）、bool 小写 true/false
    testutil::writeBytes(ini,
        "[documents]\r\n"
        "size=2\r\n"
        "1\\url=file:///C:/Users/%E4%B8%AD%E6%96%87/a%20b.txt\r\n"
        "1\\cursor=42\r\n"
        "1\\encoding=gbk\r\n"
        "2\\uid=11111111-2222-3333-4444-555555555555\r\n"
        "activeUid=11111111-2222-3333-4444-555555555555\r\n"
        "[window]\r\n"
        "x=-1920\r\n"
        "y=100\r\n"
        "width=1000\r\n"
        "height=640\r\n"
        "maximized=false\r\n"
        "[editor]\r\n"
        "wordWrap=true\r\n");

    const SessionData parsed = SessionManager::parseSession(ini);
    REQUIRE(parsed.documents.size() == 2);
    // file:/// + 百分号解码 → 本地路径（空格/中文还原）
    REQUIRE(parsed.documents[0].url == std::filesystem::path(L"C:/Users/中文/a b.txt"));
    REQUIRE(parsed.documents[0].cursor == 42);
    REQUIRE(parsed.documents[0].encoding == TextEncoding::Gbk);
    REQUIRE(parsed.documents[1].uid == "11111111-2222-3333-4444-555555555555");
    REQUIRE(parsed.documents[1].position == 1);
    REQUIRE(parsed.activeUid == "11111111-2222-3333-4444-555555555555");
    REQUIRE(parsed.activeUrl == std::nullopt);
    REQUIRE(parsed.window.has_value());
    REQUIRE(*parsed.window == WindowGeometry{-1920, 100, 1000, 640, false}); // 多屏负坐标
    REQUIRE(parsed.wordWrap == true);
}

// ===========================================================================
// 防抖器与路径解析
// ===========================================================================

TEST_CASE("Debouncer：防抖触发语义（时钟注入）", "[session][debounce]") {
    Debouncer d(500);
    REQUIRE_FALSE(d.pending());
    REQUIRE_FALSE(d.flushDue(1000)); // 无挂起不触发

    d.trigger(1000);
    REQUIRE(d.pending());
    REQUIRE(d.dueAt() == 1500);
    REQUIRE_FALSE(d.flushDue(1499)); // 未到期
    REQUIRE(d.pending());            // 未到期不清除
    REQUIRE(d.flushDue(1500));       // 到期触发
    REQUIRE_FALSE(d.pending());      // 触发后清除
    REQUIRE_FALSE(d.flushDue(2000)); // 一次性

    // restart 语义：窗口内再次触发顺延
    d.trigger(2000);
    d.trigger(2500);
    REQUIRE(d.dueAt() == 3000);
    REQUIRE_FALSE(d.flushDue(2999));
    REQUIRE(d.flushDue(3000));

    d.trigger(4000);
    REQUIRE(d.pending());
    d.cancel();
    REQUIRE_FALSE(d.pending());
    REQUIRE_FALSE(d.flushDue(9999));

    // 防御：非单调时钟（now 早于上次）不误触发
    d.trigger(10000);
    REQUIRE(d.dueAt() == 10500);
    REQUIRE_FALSE(d.flushDue(100));
}

TEST_CASE("resolvePaths：便携/安装模式路径解析（注入 exeDir/标记文件）", "[session][paths]") {
    testutil::TempDir tmp(L"paths");
    const std::filesystem::path exeDir = tmp.path / L"app";
    const std::filesystem::path userBase = tmp.path / L"user";
    std::filesystem::create_directories(exeDir);

    // 无标记 → 安装模式：ini 与 drafts 同落用户根（D5：%LOCALAPPDATA%\qcnote）
    auto installed = SessionManager::resolvePaths(exeDir, userBase);
    REQUIRE_FALSE(installed.portable);
    REQUIRE(installed.iniPath == userBase / L"qcnote.ini");
    REQUIRE(installed.draftsDir == userBase / L"drafts");

    // 标记是目录 → 不算便携
    std::filesystem::create_directories(exeDir / L"qcnote.portable");
    auto markerDir = SessionManager::resolvePaths(exeDir, userBase);
    REQUIRE_FALSE(markerDir.portable);

    // 常规文件标记 → 便携：ini 与 drafts 落 exe 旁
    std::filesystem::remove(exeDir / L"qcnote.portable");
    testutil::writeBytes(exeDir / L"qcnote.portable", "");
    auto portable = SessionManager::resolvePaths(exeDir, userBase);
    REQUIRE(portable.portable);
    REQUIRE(portable.iniPath == exeDir / L"qcnote.ini");
    REQUIRE(portable.draftsDir == exeDir / L"drafts");

    // 空 exeDir → 不以 CWD 为基准，直接安装模式
    auto noExe = SessionManager::resolvePaths(std::filesystem::path(), userBase);
    REQUIRE_FALSE(noExe.portable);
    REQUIRE(noExe.iniPath == userBase / L"qcnote.ini");

    // 生产重载（真实 %LOCALAPPDATA%）：只校验形态，不写入
    wchar_t exePath[MAX_PATH] = {};
    REQUIRE(GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0);
    const std::filesystem::path realExeDir =
        std::filesystem::path(exePath).parent_path();
    auto real = SessionManager::resolvePaths(realExeDir);
    REQUIRE_FALSE(real.portable); // 构建目录无标记
    REQUIRE(real.iniPath.filename() == L"qcnote.ini");
    REQUIRE(real.draftsDir.filename() == L"drafts");
    REQUIRE_FALSE(real.iniPath.empty());
}

// ===========================================================================
// TC-U18 会话序列化往返
// ===========================================================================

TEST_CASE("TC-U18：会话序列化往返（uid 位置/编码键/active 互斥）", "[session][u18]") {
    testutil::TempDir tmp(L"u18");
    testutil::writeBytes(tmp.file(L"a.txt"), "AAA");
    testutil::writeBytes(tmp.file(L"b.txt"), "BBB");
    testutil::writeBytes(tmp.file(L"a;b=c,d.txt"), "special"); // 文件名特殊字符触发值引号

    DocumentManager mgr;
    REQUIRE(mgr.openPaths({tmp.file(L"a.txt"), tmp.file(L"b.txt"), tmp.file(L"a;b=c,d.txt")})
                .empty());
    mgr.documentAt(0)->setCursorPosition(7);
    mgr.documentAt(1)->setEncoding(TextEncoding::Gbk); // 编码一律写（含 gbk）
    Document* untitled = mgr.newDocument();            // 未命名-1，激活
    REQUIRE(untitled->url().empty());

    const SessionData data = SessionManager::buildSessionData(mgr);
    REQUIRE(data.documents.size() == 4);
    REQUIRE(data.documents[0].url == tmp.file(L"a.txt"));
    REQUIRE(data.documents[0].cursor == 7);
    REQUIRE(data.documents[0].encoding == TextEncoding::Utf8);
    REQUIRE(data.documents[0].position == 0);
    REQUIRE(data.documents[1].encoding == TextEncoding::Gbk);
    REQUIRE(data.documents[2].url == tmp.file(L"a;b=c,d.txt"));
    REQUIRE(data.documents[3].uid == untitled->uid()); // uid 即标签位置记录
    REQUIRE(data.documents[3].url.empty());
    REQUIRE(data.documents[3].position == 3);
    REQUIRE(data.activeUid == untitled->uid()); // 活动为未命名 → activeUid
    REQUIRE(data.activeUrl == std::nullopt);

    const std::filesystem::path ini = tmp.file(L"qcnote.ini");
    REQUIRE(SessionManager::persistNow(ini, data));

    // active 与 activeUid 互斥（文件级断言：activeUid 存在时 active 键不存在）
    auto raw = IniFile::read(ini);
    REQUIRE(raw.has_value());
    REQUIRE(raw->value("documents", "active") == std::nullopt);
    REQUIRE(raw->value("documents", "activeUid") == untitled->uid());

    const SessionData parsed = SessionManager::parseSession(ini);
    REQUIRE(parsed == data); // 完整往返（含位置/光标/编码/特殊字符 url）

    // 活动为文件 → active 键、无 activeUid
    mgr.setActiveDocument(mgr.documentAt(1));
    const SessionData data2 = SessionManager::buildSessionData(mgr);
    REQUIRE(data2.activeUrl == tmp.file(L"b.txt"));
    REQUIRE(data2.activeUid == std::nullopt);
    REQUIRE(SessionManager::persistNow(ini, data2));
    auto raw2 = IniFile::read(ini);
    REQUIRE(raw2->value("documents", "active") == SessionManager::pathToUrlValue(tmp.file(L"b.txt")));
    REQUIRE(raw2->value("documents", "activeUid") == std::nullopt);
    REQUIRE(SessionManager::parseSession(ini) == data2);

    // 窗口几何 + wordWrap/darkTheme 往返
    SessionData withWindow;
    withWindow.documents = data.documents;
    withWindow.window = WindowGeometry{-1920, 50, 1280, 720, true};
    withWindow.wordWrap = false;
    withWindow.darkTheme = true;  // E5：显式 true 落盘
    REQUIRE(SessionManager::persistNow(ini, withWindow));
    {
        auto raw = IniFile::read(ini);
        REQUIRE(raw->value("editor", "darkTheme") == "true");
    }
    const SessionData parsedWindow = SessionManager::parseSession(ini);
    REQUIRE(parsedWindow.window.has_value());
    REQUIRE(*parsedWindow.window == WindowGeometry{-1920, 50, 1280, 720, true});
    REQUIRE(parsedWindow.wordWrap == false);
    REQUIRE(parsedWindow.darkTheme == true);

    // 显式 false 同样落盘（阻却旧会话残留 true；切回浅色后重启仍浅色）
    SessionData lightTheme = withWindow;
    lightTheme.darkTheme = false;
    REQUIRE(SessionManager::persistNow(ini, lightTheme));
    {
        auto raw = IniFile::read(ini);
        REQUIRE(raw->value("editor", "darkTheme") == "false");
    }
    REQUIRE(SessionManager::parseSession(ini).darkTheme == false);

    // 空会话（无文档）：size=0、无 active 键
    SessionData empty;
    REQUIRE(SessionManager::persistNow(ini, empty));
    const SessionData parsedEmpty = SessionManager::parseSession(ini);
    REQUIRE(parsedEmpty.documents.empty());
    REQUIRE(parsedEmpty.activeUrl == std::nullopt);
    REQUIRE(parsedEmpty.activeUid == std::nullopt);
    // 无会话文件：空会话（首启）
    REQUIRE(SessionManager::parseSession(tmp.file(L"none.ini")) == SessionData{});
}

// ===========================================================================
// TC-U19 会话容错
// ===========================================================================

TEST_CASE("TC-U19：会话容错（坏条目/跳号/均空/旧格式/非法值）", "[session][u19]") {
    testutil::TempDir tmp(L"u19");
    const std::filesystem::path ini = tmp.file(L"qcnote.ini");
    testutil::writeBytes(ini,
        "[documents]\r\n"
        "size=6\r\n"
        "1\\url=C:/exists/a.txt\r\n"
        "1\\cursor=10\r\n"
        "1\\encoding=gbk\r\n"
        "2\\cursor=5\r\n"                 // 跳号坏条目：无 url 无 uid → 丢弃
        "3\\uid=UID3\r\n"                 // 未命名占位（绝对位置 2）
        "4\\url=C:/missing.txt\r\n"       // 预检丢弃（缺失）
        "5\\url=\r\n"                     // 旧格式无 uid 的未命名条目 → 丢弃
        "6\\url=C:/exists/b.txt\r\n"
        "6\\cursor=-7\r\n"                // 负值 → 0
        "6\\encoding=big5\r\n"            // 非法编码 → 自动识别
        "active=C:/missing.txt\r\n"       // active 指向丢失文件：保留（恢复侧回退）
        "[window]\r\n"
        "x=10\r\n"
        "y=20\r\n"
        "maximized=true\r\n"              // 缺 width/height → 整节无效
        "[editor]\r\n"
        "wordWrap=1\r\n");

    SessionManager::FileProbe probe = [](const std::filesystem::path& p) {
        return p.wstring().find(L"C:/exists/") == 0;
    };
    const SessionData parsed = SessionManager::parseSession(ini, probe);
    REQUIRE(parsed.documents.size() == 3);
    REQUIRE(parsed.documents[0].url == std::filesystem::path(L"C:/exists/a.txt"));
    REQUIRE(parsed.documents[0].cursor == 10);
    REQUIRE(parsed.documents[0].encoding == TextEncoding::Gbk);
    REQUIRE(parsed.documents[0].position == 0);
    REQUIRE(parsed.documents[1].uid == "UID3");
    REQUIRE(parsed.documents[1].position == 2); // 编号跳孔后绝对位置不回填
    REQUIRE(parsed.documents[2].url == std::filesystem::path(L"C:/exists/b.txt"));
    REQUIRE(parsed.documents[2].cursor == 0);
    REQUIRE(parsed.documents[2].encoding == std::nullopt);
    REQUIRE(parsed.documents[2].position == 5);
    REQUIRE(parsed.activeUrl == std::filesystem::path(L"C:/missing.txt"));
    REQUIRE(parsed.activeUid == std::nullopt);
    REQUIRE_FALSE(parsed.window.has_value());
    REQUIRE(parsed.wordWrap == true);
    // darkTheme：无键 → nullopt（保持浅色默认，前向兼容旧会话）
    REQUIRE(parsed.darkTheme == std::nullopt);

    // darkTheme 宽容解析（与 wordWrap 同款 "1" 形态；写侧只产 true/false）
    testutil::writeBytes(tmp.file(L"editor-dark.ini"),
        "[editor]\r\n"
        "darkTheme=1\r\n");
    REQUIRE(SessionManager::parseSession(tmp.file(L"editor-dark.ini")).darkTheme == true);
    testutil::writeBytes(tmp.file(L"editor-light.ini"),
        "[editor]\r\n"
        "darkTheme=0\r\n");
    REQUIRE(SessionManager::parseSession(tmp.file(L"editor-light.ini")).darkTheme == false);

    // 无预检（纯数据解析）：缺失文件条目保留（B 层打开时再跳过）
    const SessionData noProbe = SessionManager::parseSession(ini);
    REQUIRE(noProbe.documents.size() == 4);

    // size 异常：非数字/负数/超出键数上界 → 按 0/键数截断（防长循环）
    {
        IniData d;
        d.setValue("documents", "size", "abc");
        d.setValue("documents", "1/url", "C:/x.txt");
        IniFile::write(ini, d);
        REQUIRE(SessionManager::parseSession(ini).documents.empty());
    }
    {
        IniData d;
        d.setValue("documents", "size", "-5");
        IniFile::write(ini, d);
        REQUIRE(SessionManager::parseSession(ini).documents.empty());
    }
    {
        IniData d;
        d.setValue("documents", "size", "999999999"); // 超出实际键数 → 截断
        d.setValue("documents", "1/url", "C:/x.txt");
        IniFile::write(ini, d);
        const SessionData s = SessionManager::parseSession(ini);
        REQUIRE(s.documents.size() == 1);
        REQUIRE(s.documents[0].url == std::filesystem::path(L"C:/x.txt"));
    }
    // 无 size 键 → 空文档列表（[window] 仍独立有效）
    {
        IniData d;
        d.setValue("window", "x", "1");
        d.setValue("window", "y", "2");
        d.setValue("window", "width", "3");
        d.setValue("window", "height", "4");
        d.setValue("window", "maximized", "false");
        IniFile::write(ini, d);
        const SessionData s = SessionManager::parseSession(ini);
        REQUIRE(s.documents.empty());
        REQUIRE(s.window.has_value());
    }
}

// ===========================================================================
// TC-U20 窗口几何校验
// ===========================================================================

TEST_CASE("TC-U20：窗口几何校验（钳制/越界回退/多屏 mock）", "[session][u20]") {
    const std::vector<RECT> screens = {rect(0, 0, 1920, 1040), rect(1920, 0, 3200, 720)};

    // 合法几何原样通过
    auto ok = SessionManager::validateGeometry(WindowGeometry{100, 50, 1200, 800, false}, screens);
    REQUIRE(ok.has_value());
    REQUIRE(*ok == WindowGeometry{100, 50, 1200, 800, false});

    // 宽高下限钳制（产品最小尺寸 800×500）
    auto clampedMin =
        SessionManager::validateGeometry(WindowGeometry{100, 50, 400, 300, false}, screens);
    REQUIRE(clampedMin.has_value());
    REQUIRE(clampedMin->width == 800);
    REQUIRE(clampedMin->height == 500);

    // 上限钳制到各屏最大可用宽高
    auto big = SessionManager::validateGeometry(WindowGeometry{0, 0, 5000, 4000, false}, screens);
    REQUIRE(big.has_value());
    REQUIRE(big->width == 1920);
    REQUIRE(big->height == 1040);

    // 恰在边界：800×500 不动
    auto edge = SessionManager::validateGeometry(WindowGeometry{0, 0, 800, 500, true}, screens);
    REQUIRE(edge.has_value());
    REQUIRE(edge->width == 800);
    REQUIRE(edge->height == 500);
    REQUIRE(edge->maximized == true); // 最大化标志透传

    // 位置与任何屏不相交 → nullopt（显示器变更回退默认几何）
    REQUIRE_FALSE(
        SessionManager::validateGeometry(WindowGeometry{99999, 50, 800, 500, false}, screens)
            .has_value());
    REQUIRE_FALSE(
        SessionManager::validateGeometry(WindowGeometry{100, 99999, 800, 500, false}, screens)
            .has_value());

    // 副屏上合法（含贴边相交）
    auto second = SessionManager::validateGeometry(WindowGeometry{1920, 0, 800, 500, false}, screens);
    REQUIRE(second.has_value());
    // 负坐标（主屏左侧副屏）
    const std::vector<RECT> leftScreens = {rect(-1280, 0, 0, 1024), rect(0, 0, 1920, 1040)};
    auto negative =
        SessionManager::validateGeometry(WindowGeometry{-1280, 0, 800, 500, false}, leftScreens);
    REQUIRE(negative.has_value());
    REQUIRE(negative->x == -1280);

    // 屏幕过小：下限优先（宁可超出小屏也不破坏最小尺寸保证）
    const std::vector<RECT> tinyScreens = {rect(0, 0, 640, 480)};
    auto tiny = SessionManager::validateGeometry(WindowGeometry{0, 0, 400, 300, false}, tinyScreens);
    REQUIRE(tiny.has_value());
    REQUIRE(tiny->width == 800);
    REQUIRE(tiny->height == 500);

    // 空屏幕列表：仅下限钳制、不校验位置
    auto noScreens =
        SessionManager::validateGeometry(WindowGeometry{99999, 99999, 400, 300, false}, {});
    REQUIRE(noScreens.has_value());
    REQUIRE(noScreens->width == 800);
    REQUIRE(noScreens->height == 500);
}

// ===========================================================================
// TC-U21 草稿条目序列化
// ===========================================================================

TEST_CASE("TC-U21：草稿条目序列化往返 + 损坏/版本不符静默删", "[session][u21]") {
    testutil::TempDir tmp(L"u21");
    const std::filesystem::path dir = tmp.path / L"drafts";

    // entryFor 字段全量 + 光标钳制
    Document doc;
    doc.applyLoadedState(std::filesystem::path(L"C:/t/中文.txt"), "ABC中文", LineEnding::Crlf,
                         TextEncoding::Gbk, true);
    doc.setContent("ABC中文XYZ");
    doc.setCursorPosition(3);
    const DraftEntry entry = DraftCacheManager::entryFor(doc, 12345);
    REQUIRE(entry.uid == doc.uid());
    REQUIRE(entry.url == std::filesystem::path(L"C:/t/中文.txt"));
    REQUIRE(entry.untitledName == L"中文.txt");
    REQUIRE(entry.content == "ABC中文XYZ");
    REQUIRE(entry.cursor == 3);
    REQUIRE(entry.lineEnding == LineEnding::Crlf);
    REQUIRE(entry.encoding == TextEncoding::Gbk);
    REQUIRE(entry.hasBom);
    REQUIRE(entry.baselineHash == doc.baselineHash());
    REQUIRE(entry.savedAt == 12345);

    doc.setCursorPosition(9999); // 光标越界 → 钳到内容字节长度
    const DraftEntry clamped = DraftCacheManager::entryFor(doc, 1);
    REQUIRE(clamped.cursor == static_cast<int>(std::string("ABC中文XYZ").size()));

    // persist → readEntry 完整往返（原子写；目录自动创建）
    REQUIRE(DraftCacheManager::persist(dir, entry));
    const auto back = DraftCacheManager::readEntry(entryFile(dir, entry.uid));
    REQUIRE(back.has_value());
    REQUIRE(*back == entry);

    // 未命名条目：url 空、沿用未命名显示名
    Document untitled(L"未命名-3");
    untitled.setContent("草稿内容");
    const DraftEntry untitledEntry = DraftCacheManager::entryFor(untitled, 7);
    REQUIRE(untitledEntry.url.empty());
    REQUIRE(untitledEntry.untitledName == L"未命名-3");
    REQUIRE(DraftCacheManager::persist(dir, untitledEntry));
    const auto untitledBack = DraftCacheManager::readEntry(entryFile(dir, untitledEntry.uid));
    REQUIRE(untitledBack.has_value());
    REQUIRE(*untitledBack == untitledEntry);

    // 损坏 JSON → readEntry nullopt；restore 静默删文件
    testutil::writeBytes(entryFile(dir, "bad1"), "{ 这不是 json");
    REQUIRE_FALSE(DraftCacheManager::readEntry(entryFile(dir, "bad1")).has_value());
    // 版本不符（version=2）→ nullopt
    testutil::writeBytes(entryFile(dir, "bad2"), R"({"version":2,"uid":"bad2","content":"x"})");
    REQUIRE_FALSE(DraftCacheManager::readEntry(entryFile(dir, "bad2")).has_value());
    const RestoreOutcome outcome =
        DraftCacheManager::restore(dir, SessionData{}, DiskHashQuery());
    // 损坏/版本不符条目被静默删除；既有条目正常参与分类（磁盘查询 null →
    // url 条目按缺失归冲突；未命名条目重建）
    REQUIRE_FALSE(fileExists(entryFile(dir, "bad1")));
    REQUIRE_FALSE(fileExists(entryFile(dir, "bad2")));
    REQUIRE(outcome.restored.size() == 1);
    REQUIRE(outcome.restored[0].entry.uid == untitledEntry.uid);
    REQUIRE(outcome.conflicts.size() == 1);
    REQUIRE(outcome.conflicts[0].entry.uid == entry.uid);
    REQUIRE(outcome.conflicts[0].missing);
    // 目录中只剩待决策的冲突条目文件
    REQUIRE(testutil::entryCount(dir) == 1);

    // 旧字段名 "name"（Qt 版资产）宽容读取
    testutil::writeBytes(entryFile(dir, "old1"),
                         R"({"version":1,"uid":"old1","url":"","name":"旧名","content":"c",)"
                         R"("cursor":1,"lineEnding":"crlf","encoding":"gbk","hasBom":true,)"
                         R"("baselineHash":"h","savedAt":9})");
    const auto old1 = DraftCacheManager::readEntry(entryFile(dir, "old1"));
    REQUIRE(old1.has_value());
    REQUIRE(old1->untitledName == L"旧名");
    REQUIRE(old1->savedAt == 9);
    REQUIRE(old1->cursor == 1);
    REQUIRE(old1->lineEnding == LineEnding::Crlf);
    REQUIRE(old1->encoding == TextEncoding::Gbk);
    REQUIRE(old1->hasBom);

    // 非 .json 扩展名文件不参与（不读不删）
    testutil::writeBytes(dir / L"notes.txt", "junk");
    const RestoreOutcome outcome2 =
        DraftCacheManager::restore(dir, SessionData{}, DiskHashQuery());
    REQUIRE(outcome2.restored.size() == 1); // old1 → 未命名重建计划
    REQUIRE(fileExists(dir / L"notes.txt"));
}

// ===========================================================================
// TC-U22 草稿恢复分类全分支
// ===========================================================================

TEST_CASE("TC-U22：恢复分类（静默/冲突/未命名重建/光标钳制/应用后删条目）", "[session][u22]") {
    testutil::TempDir tmp(L"u22");
    const std::filesystem::path dir = tmp.path / L"drafts";
    const std::filesystem::path urlA = L"C:/works/a.txt";
    const std::filesystem::path urlB = L"C:/works/b.txt";
    const std::filesystem::path urlGone = L"C:/works/gone.txt";

    SessionData session;
    session.documents = {urlEntry(urlA, 0), uidEntry("UID-U", 1), urlEntry(urlB, 2)};
    session.activeUrl = urlA;

    // 磁盘 mock：A/B 存在（A 哈希与草稿一致 → 静默；B 不一致 → 冲突）；gone 缺失
    const DiskMap disk = {{urlA, "hash-a"}, {urlB, "hash-b-disk"}};
    const DiskHashQuery query = diskQuery(disk);

    writeEntryFile(dir, makeEntry("e1", urlA, L"a.txt", "A草稿", 2, 100, "hash-a"));
    writeEntryFile(dir, makeEntry("e2", urlB, L"b.txt", "B草稿", 0, 100, "hash-b-draft"));
    writeEntryFile(dir, makeEntry("e3", urlGone, L"gone.txt", "G草稿", 0, 100, "hash-g"));
    writeEntryFile(dir,
                   makeEntry("UID-U", std::filesystem::path(), L"未命名-1", "abc", 99, 100, ""));

    const RestoreOutcome outcome = DraftCacheManager::restore(dir, session, query);

    // url 条目先行（文件名序 e1→e2），未命名随后
    REQUIRE(outcome.restored.size() == 2);
    REQUIRE(outcome.restored[0].target == DraftRestorePlan::Target::ApplyToUrlDoc);
    REQUIRE(outcome.restored[0].url == urlA);
    REQUIRE(outcome.restored[0].entry.content == "A草稿");
    REQUIRE(outcome.restored[1].target == DraftRestorePlan::Target::NewUntitled);
    REQUIRE(outcome.restored[1].entry.uid == "UID-U");
    REQUIRE(outcome.restored[1].insertIndex == 1); // 会话绝对位置
    REQUIRE(outcome.restored[1].entry.cursor == 3); // 光标钳制到内容长度（99→3）
    REQUIRE(outcome.restored[1].entry.untitledName == L"未命名-1");

    // 冲突：B 哈希不符（missing=false）/ gone 缺失（missing=true）
    REQUIRE(outcome.conflicts.size() == 2);
    REQUIRE(outcome.conflicts[0].entry.uid == "e2");
    REQUIRE_FALSE(outcome.conflicts[0].missing);
    REQUIRE(outcome.conflicts[0].displayName == L"b.txt");
    REQUIRE(outcome.conflicts[1].entry.uid == "e3");
    REQUIRE(outcome.conflicts[1].missing);
    REQUIRE(outcome.conflicts[1].displayName == L"gone.txt");

    // 静默恢复与未命名重建的条目文件已删；冲突条目文件保留待决策
    REQUIRE_FALSE(fileExists(entryFile(dir, "e1")));
    REQUIRE_FALSE(fileExists(entryFile(dir, "UID-U")));
    REQUIRE(fileExists(entryFile(dir, "e2")));
    REQUIRE(fileExists(entryFile(dir, "e3")));

    // 激活：无 activeUid、会话文件可打开（A/B 在盘）→ 维持会话活动标签
    REQUIRE(outcome.activatedRestoredIndex == -1);

    // activeUid 匹配（活动未命名标签关闭现场还原一级激活）
    {
        SessionData s = session;
        s.activeUrl = std::nullopt;
        s.activeUid = "UID-U";
        writeEntryFile(dir, makeEntry("UID-U", std::filesystem::path(), L"未命名-1", "abc", 1, 100, ""));
        const auto out2 = DraftCacheManager::restore(dir, s, query);
        REQUIRE(out2.activatedRestoredIndex == 0); // restored[0] 即该未命名条目
    }

    // 空场景（会话文件全部丢失）：激活首个恢复文档
    {
        SessionData s;
        s.documents = {uidEntry("UID-U", 0)};
        writeEntryFile(dir, makeEntry("UID-U", std::filesystem::path(), L"未命名-1", "abc", 1, 100, ""));
        const DiskHashQuery emptyDisk = diskQuery({});
        const auto out3 = DraftCacheManager::restore(dir, s, emptyDisk);
        REQUIRE(out3.restored.size() == 1);
        REQUIRE(out3.activatedRestoredIndex == 0);
    }

    // 冲突解决：恢复 → B 应用到 url 文档（磁盘仍在）；gone 转未命名重建；
    // 应用后删条目文件（DEC-007）；已有活动文档不抢占
    std::vector<DraftConflict> conflicts = outcome.conflicts;
    const ConflictResolution resolved =
        DraftCacheManager::resolveConflicts(conflicts, /*restore=*/true,
                                            /*hasActiveDocument=*/true, query);
    REQUIRE(conflicts.empty()); // 决策后清空
    REQUIRE(resolved.plans.size() == 2);
    REQUIRE(resolved.plans[0].target == DraftRestorePlan::Target::ApplyToUrlDoc);
    REQUIRE(resolved.plans[0].url == urlB);
    REQUIRE(resolved.plans[0].entry.content == "B草稿");
    REQUIRE(resolved.plans[1].target == DraftRestorePlan::Target::NewUntitled);
    REQUIRE(resolved.plans[1].entry.untitledName == L"gone.txt"); // 缺失 → 未命名重建
    REQUIRE(resolved.plans[1].insertIndex == -1);
    REQUIRE_FALSE(resolved.activateFirst); // 已有活动文档
    REQUIRE_FALSE(fileExists(entryFile(dir, "e2"))); // 应用后删条目
    REQUIRE_FALSE(fileExists(entryFile(dir, "e3")));

    // 丢弃分支：不产计划、删条目、不激活
    {
        writeEntryFile(dir, makeEntry("c1", urlB, L"b.txt", "x", 0, 1, "h"));
        std::vector<DraftConflict> cs{DraftConflict{makeEntry("c1", urlB, L"b.txt", "x", 0, 1, "h"),
                                                   entryFile(dir, "c1"), false, L"b.txt"}};
        const auto dropped = DraftCacheManager::resolveConflicts(cs, false, false, query);
        REQUIRE(cs.empty());
        REQUIRE(dropped.plans.empty());
        REQUIRE_FALSE(dropped.activateFirst);
        REQUIRE_FALSE(fileExists(entryFile(dir, "c1"))); // 丢弃也删条目
    }
    // 恢复分支 + 无活动文档：激活首个恢复（DEC-006 对齐）
    {
        writeEntryFile(dir, makeEntry("c2", urlB, L"b.txt", "x", 0, 1, "h"));
        std::vector<DraftConflict> cs{DraftConflict{makeEntry("c2", urlB, L"b.txt", "x", 0, 1, "h"),
                                                   entryFile(dir, "c2"), false, L"b.txt"}};
        const auto activated = DraftCacheManager::resolveConflicts(cs, true, false, query);
        REQUIRE(activated.activateFirst);
    }
}

// ===========================================================================
// TC-U23 同 URL 去重
// ===========================================================================

TEST_CASE("TC-U23：同 URL 去重（savedAt 最新/缺失=0/平局 uid 字典序/落选删文件）",
          "[session][u23]") {
    const std::filesystem::path url = L"C:/works/same.txt";

    auto scenario = [&](std::vector<DraftEntry> entries, const std::string& expectWinner) {
        testutil::TempDir tmp(L"u23");
        const std::filesystem::path dir = tmp.path / L"drafts";
        for (const auto& e : entries)
            writeEntryFile(dir, e);
        const DiskMap disk = {{url, "hash-ok"}};
        const RestoreOutcome out =
            DraftCacheManager::restore(dir, SessionData{}, diskQuery(disk));
        REQUIRE(out.conflicts.empty());
        REQUIRE(out.restored.size() == 1); // 同 URL 仅留一条
        REQUIRE(out.restored[0].entry.uid == expectWinner);
        // 落选条目文件立即删除（目录只剩非条目文件，应为 0）
        REQUIRE(testutil::entryCount(dir) == 0);
    };

    // savedAt 最新者胜
    scenario({makeEntry("aaa", url, L"n", "old", 0, 100, "hash-ok"),
              makeEntry("bbb", url, L"n", "new", 0, 200, "hash-ok")},
             "bbb");
    // savedAt 缺失（=0 最旧）→ 有时间戳者胜
    scenario({makeEntry("zzz", url, L"n", "old", 0, 0, "hash-ok"),
              makeEntry("aaa", url, L"n", "new", 0, 5, "hash-ok")},
             "aaa");
    // 完全平局 → uid 字典序保留较大者（确定性，不依赖枚举顺序）
    scenario({makeEntry("aaa", url, L"n", "a", 0, 100, "hash-ok"),
              makeEntry("zzz", url, L"n", "z", 0, 100, "hash-ok")},
             "zzz");
    // 三条目：中间者最新
    scenario({makeEntry("a1", url, L"n", "x", 0, 10, "hash-ok"),
              makeEntry("m1", url, L"n", "x", 0, 200, "hash-ok"),
              makeEntry("z1", url, L"n", "x", 0, 100, "hash-ok")},
             "m1");

    // 未命名条目（url 空）不参与去重：各自独立恢复
    {
        testutil::TempDir tmp(L"u23-unnamed");
        const std::filesystem::path dir = tmp.path / L"drafts";
        writeEntryFile(dir, makeEntry("u1", std::filesystem::path(), L"未命名-1", "x", 0, 100, ""));
        writeEntryFile(dir, makeEntry("u2", std::filesystem::path(), L"未命名-2", "y", 0, 200, ""));
        const RestoreOutcome out =
            DraftCacheManager::restore(dir, SessionData{}, DiskHashQuery());
        REQUIRE(out.restored.size() == 2);
        REQUIRE(testutil::entryCount(dir) == 0);
    }
}

// ===========================================================================
// TC-U24 清理语义与防抖管线
// ===========================================================================

TEST_CASE("TC-U24：清理语义（回基线删/关闭脏删/干净关无条目/放弃路径）", "[session][u24]") {
    testutil::TempDir tmp(L"u24");
    const std::filesystem::path dir = tmp.path / L"drafts";
    long long fakeNow = 0;
    DraftCacheManager dcm(dir, [&fakeNow] { return fakeNow; });

    testutil::writeBytes(tmp.file(L"f.txt"), "ABC");
    testutil::writeBytes(tmp.file(L"g.txt"), "GGG");
    DocumentManager mgr;
    Document* fdoc = mgr.openPath(tmp.file(L"f.txt")).value();

    // —— 防抖：1000ms 到期写出（未到期不写）——
    fdoc->setContent("ABCD"); // 置脏
    dcm.onContentChanged(fdoc, 1000);
    REQUIRE(dcm.hasPending());
    REQUIRE_FALSE(fileExists(entryFile(dir, fdoc->uid())));
    REQUIRE_FALSE(dcm.flushDue(1999)); // 999ms：未到期
    REQUIRE(dcm.hasPending());
    REQUIRE(dcm.flushDue(2000)); // 到期：写出（只重写变化条目）
    REQUIRE_FALSE(dcm.hasPending());
    REQUIRE(fileExists(entryFile(dir, fdoc->uid())));

    // —— 防抖 restart：窗口内再次变化顺延到期 ——
    fdoc->setContent("ABCDE");
    dcm.onContentChanged(fdoc, 3000);
    dcm.onContentChanged(fdoc, 3500); // restart → due 4500
    REQUIRE_FALSE(dcm.flushDue(4499));
    REQUIRE(dcm.flushDue(4500));

    // —— 内容回基线（撤销回原样）→ 立即删条目 ——
    fdoc->setContent("ABC"); // 与基线一致 → modified false
    REQUIRE_FALSE(fdoc->modified());
    dcm.onContentChanged(fdoc, 5000);
    REQUIRE_FALSE(fileExists(entryFile(dir, fdoc->uid())));
    REQUIRE_FALSE(dcm.hasPending());

    // —— 保存路径（markSaved → modifiedChanged false）→ 删条目 ——
    fdoc->setContent("ABCX");
    dcm.onContentChanged(fdoc, 6000);
    dcm.flushNow(); // 失焦即写/退出兜底：无视到期立即写出
    REQUIRE(fileExists(entryFile(dir, fdoc->uid())));
    fdoc->markSaved();
    dcm.onModifiedCleared(fdoc);
    REQUIRE_FALSE(fileExists(entryFile(dir, fdoc->uid())));

    // —— 关闭仍脏（「不保存」放弃路径）→ 删条目（放弃即真丢）——
    fdoc->setContent("ABCY");
    dcm.onContentChanged(fdoc, 7000);
    dcm.flushNow();
    REQUIRE(fileExists(entryFile(dir, fdoc->uid())));
    REQUIRE(fdoc->modified());
    dcm.onDocumentClosed(fdoc); // documentClosed 时序：仍可读 uid/modified
    REQUIRE_FALSE(fileExists(entryFile(dir, fdoc->uid())));

    // —— 干净关闭：无条目产生（干净文档本不落盘）——
    Document* gdoc = mgr.openPath(tmp.file(L"g.txt")).value();
    REQUIRE_FALSE(gdoc->modified());
    dcm.onDocumentClosed(gdoc);
    REQUIRE_FALSE(fileExists(entryFile(dir, gdoc->uid())));
    REQUIRE(testutil::entryCount(dir) == 0);

    // —— 未命名文档同样入管线（uid 主键，无 URL 可缓存）——
    Document* untitled = mgr.newDocument();
    untitled->setContent("未命名草稿");
    dcm.onContentChanged(untitled, 8000);
    dcm.flushNow();
    REQUIRE(fileExists(entryFile(dir, untitled->uid())));
}

// ===========================================================================
// TC-U25 关闭现场还原算法
// ===========================================================================

TEST_CASE("TC-U25：关闭现场还原算法（稳定插入/激活优先级/无位置排后/钳制）",
          "[session][u25]") {
    const std::filesystem::path urlA = L"C:/works/a.txt";
    const std::filesystem::path urlB = L"C:/works/b.txt";

    // 场景 1：[文件A, 未命名1(有内容), 文件B]、活动=未命名1 →
    //        未命名按会话位置 1 插入；activeUid 一级激活
    {
        testutil::TempDir tmp(L"u25-1");
        const std::filesystem::path dir = tmp.path / L"drafts";
        SessionData session;
        session.documents = {urlEntry(urlA, 0), uidEntry("UID-1", 1), urlEntry(urlB, 2)};
        session.activeUid = "UID-1";
        writeEntryFile(dir, makeEntry("UID-1", std::filesystem::path(), L"未命名-1", "内容", 2, 100, ""));
        const DiskMap disk = {{urlA, "ha"}, {urlB, "hb"}};
        const RestoreOutcome out = DraftCacheManager::restore(dir, session, diskQuery(disk));
        REQUIRE(out.restored.size() == 1);
        REQUIRE(out.restored[0].target == DraftRestorePlan::Target::NewUntitled);
        REQUIRE(out.restored[0].insertIndex == 1); // = 关闭时位置（A 后、B 前）
        REQUIRE(out.activatedRestoredIndex == 0);  // activeUid > active URL > 首个
    }

    // 场景 2：活动为文件（active URL）且会话文件可开 → 维持会话活动（不抢占）
    {
        testutil::TempDir tmp(L"u25-2");
        const std::filesystem::path dir = tmp.path / L"drafts";
        SessionData session;
        session.documents = {urlEntry(urlA, 0), uidEntry("UID-1", 1), urlEntry(urlB, 2)};
        session.activeUrl = urlA;
        writeEntryFile(dir, makeEntry("UID-1", std::filesystem::path(), L"未命名-1", "内容", 0, 100, ""));
        const DiskMap disk = {{urlA, "ha"}, {urlB, "hb"}};
        const RestoreOutcome out = DraftCacheManager::restore(dir, session, diskQuery(disk));
        REQUIRE(out.activatedRestoredIndex == -1);
    }

    // 场景 3：会话文件全部丢失（activeBefore 为空）→ 激活首个恢复文档
    {
        testutil::TempDir tmp(L"u25-3");
        const std::filesystem::path dir = tmp.path / L"drafts";
        SessionData session;
        session.documents = {urlEntry(urlA, 0), uidEntry("UID-1", 1)};
        writeEntryFile(dir, makeEntry("UID-1", std::filesystem::path(), L"未命名-1", "内容", 0, 100, ""));
        const RestoreOutcome out =
            DraftCacheManager::restore(dir, session, diskQuery(DiskMap{}));
        REQUIRE(out.restored.size() == 1);
        REQUIRE(out.activatedRestoredIndex == 0);
    }

    // 场景 4：无位置记录条目排最后（追加）；有位置条目按位置先行（文件名序不干扰）
    {
        testutil::TempDir tmp(L"u25-4");
        const std::filesystem::path dir = tmp.path / L"drafts";
        SessionData session;
        session.documents = {urlEntry(urlA, 0), uidEntry("UID-P", 2)}; // uidN 不在会话
        writeEntryFile(dir, makeEntry("UID-N", std::filesystem::path(), L"未命名-9", "n", 0, 100, ""));
        writeEntryFile(dir, makeEntry("UID-P", std::filesystem::path(), L"未命名-2", "p", 0, 100, ""));
        const DiskMap disk = {{urlA, "ha"}};
        const RestoreOutcome out = DraftCacheManager::restore(dir, session, diskQuery(disk));
        REQUIRE(out.restored.size() == 2);
        REQUIRE(out.restored[0].entry.uid == "UID-P"); // 有位置（2）先行
        REQUIRE(out.restored[0].insertIndex == 2);
        REQUIRE(out.restored[1].entry.uid == "UID-N"); // 无位置排最后
        REQUIRE(out.restored[1].insertIndex == -1);    // -1 = 追加尾部
    }

    // 场景 5：无内容未命名标签不恢复（空位消除）；其余未命名绝对位置不变
    {
        testutil::TempDir tmp(L"u25-5");
        const std::filesystem::path dir = tmp.path / L"drafts";
        SessionData session;
        session.documents = {urlEntry(urlA, 0), uidEntry("UID-EMPTY", 1), uidEntry("UID-F", 2)};
        // 仅 UID-F 有草稿；UID-EMPTY 无内容未命名标签（无条目）
        writeEntryFile(dir, makeEntry("UID-F", std::filesystem::path(), L"未命名-3", "f", 0, 100, ""));
        const RestoreOutcome out =
            DraftCacheManager::restore(dir, session, diskQuery(DiskMap{}));
        REQUIRE(out.restored.size() == 1); // UID-EMPTY 无计划（空位自然消除）
        REQUIRE(out.restored[0].entry.uid == "UID-F");
        REQUIRE(out.restored[0].insertIndex == 2); // 绝对位置保留（空位不回填）
    }

    // 场景 6：url 草稿 + 未命名草稿混排 → url 先行、未命名按位随后
    {
        testutil::TempDir tmp(L"u25-6");
        const std::filesystem::path dir = tmp.path / L"drafts";
        SessionData session;
        session.documents = {uidEntry("UID-B", 0), urlEntry(urlA, 1), uidEntry("UID-A", 2)};
        writeEntryFile(dir, makeEntry("UID-A", std::filesystem::path(), L"未命名-1", "a", 0, 100, ""));
        writeEntryFile(dir, makeEntry("aa", urlA, L"a.txt", "draft", 0, 100, "ha"));
        writeEntryFile(dir, makeEntry("UID-B", std::filesystem::path(), L"未命名-2", "b", 0, 100, ""));
        const DiskMap disk = {{urlA, "ha"}};
        const RestoreOutcome out = DraftCacheManager::restore(dir, session, diskQuery(disk));
        REQUIRE(out.restored.size() == 3);
        REQUIRE(out.restored[0].target == DraftRestorePlan::Target::ApplyToUrlDoc); // url 先行
        REQUIRE(out.restored[1].target == DraftRestorePlan::Target::NewUntitled);
        REQUIRE(out.restored[1].entry.uid == "UID-B"); // 位置 0 先于位置 2
        REQUIRE(out.restored[1].insertIndex == 0);
        REQUIRE(out.restored[2].entry.uid == "UID-A");
        REQUIRE(out.restored[2].insertIndex == 2);
    }

    // 场景 7：插入位置越界钳制（B 层应用时的钳制规则，纯函数锁定）
    REQUIRE(DraftCacheManager::clampInsertIndex(-1, 3) == 3); // -1 = 追加尾部
    REQUIRE(DraftCacheManager::clampInsertIndex(5, 2) == 2);  // 越界 → 钳到末尾
    REQUIRE(DraftCacheManager::clampInsertIndex(1, 3) == 1);  // 界内原样
    REQUIRE(DraftCacheManager::clampInsertIndex(0, 0) == 0);
    REQUIRE(DraftCacheManager::clampInsertIndex(-1, 0) == 0);
}

// ===========================================================================
// TC-U26 baselineHash 稳定性
// ===========================================================================

TEST_CASE("TC-U26：baselineHash 稳定性（同内容同哈希/改一字不同）", "[session][u26]") {
    testutil::TempDir tmp(L"u26");
    testutil::writeBytes(tmp.file(L"same1.txt"), "相同内容\n第二行");
    testutil::writeBytes(tmp.file(L"same2.txt"), "相同内容\n第二行");
    testutil::writeBytes(tmp.file(L"diff.txt"), "相同内容\n第二行X"); // 一字之差

    DocumentManager mgr;
    Document* d1 = mgr.openPath(tmp.file(L"same1.txt")).value();
    Document* d2 = mgr.openPath(tmp.file(L"same2.txt")).value();
    Document* d3 = mgr.openPath(tmp.file(L"diff.txt")).value();
    REQUIRE(d1->baselineHash() == d2->baselineHash()); // 同内容 → 同指纹
    REQUIRE(d1->baselineHash() != d3->baselineHash()); // 改一字 → 不同
    REQUIRE(d1->baselineHash().size() == 64);          // SHA-256 hex 小写
    REQUIRE(d1->baselineHash() == sha256Hex("相同内容\n第二行"));

    // 保存后基线前移：哈希随基线更新（草稿冲突检测的事实之源）
    d1->setContent("新内容");
    d1->markSaved();
    REQUIRE(d1->baselineHash() == sha256Hex("新内容"));
}

TEST_CASE("TC-U27：restoreUntitledDraft 未命名带位置插入（M4-B 新增接口）",
          "[session][u27]") {
    testutil::TempDir tmp(L"u27");
    testutil::writeBytes(tmp.file(L"a.txt"), "AAA");
    testutil::writeBytes(tmp.file(L"b.txt"), "BBB");

    DocumentManager mgr;
    REQUIRE(mgr.openPaths({tmp.file(L"a.txt"), tmp.file(L"b.txt")}).empty());
    REQUIRE(mgr.activeDocument() == mgr.documentAt(1)); // 打开即激活末文档

    // 场景 1：按记录名与位置插入（A 与 B 之间）；不激活（活动仍指 B 文档）
    Document* u1 = mgr.restoreUntitledDraft(L"未命名-1", 1);
    REQUIRE(mgr.documentCount() == 3);
    REQUIRE(mgr.documentAt(0)->displayName() == L"a.txt");
    REQUIRE(mgr.documentAt(1) == u1);
    REQUIRE(u1->displayName() == L"未命名-1");
    REQUIRE(u1->url().empty());
    REQUIRE(mgr.activeDocument()->displayName() == L"b.txt"); // 插入不抢占活动
    u1->setCursorPosition(2);
    u1->setContent("内容"); // 调用方随后写内容（M4-B 时序：先入列再写）
    REQUIRE(u1->modified());

    // 场景 2：同名冲突（未命名-1 已在列）→ 编号器递增生成新名
    Document* u2 = mgr.restoreUntitledDraft(L"未命名-1", -1); // -1 追加尾部
    REQUIRE(u2 != u1);
    REQUIRE(u2->displayName() == L"未命名-2");

    // 场景 3：记录名推进编号器——后续 newDocument 跳过占用号
    Document* u5 = mgr.restoreUntitledDraft(L"未命名-5", 99); // 越界位置钳制追加
    REQUIRE(u5->displayName() == L"未命名-5");
    REQUIRE(mgr.documentCount() == 5); // [a, u1, b, u2, u5]（99 钳到 4）
    Document* u6 = mgr.newDocument();  // 编号器已到 5 → 下一个 6
    REQUIRE(u6->displayName() == L"未命名-6");

    // 场景 4：空名按编号器生成；非编号名（另存为冲突回退名）原样沿用
    Document* u7 = mgr.restoreUntitledDraft(L"", -1);
    REQUIRE(u7->displayName() == L"未命名-7");
    Document* u8 = mgr.restoreUntitledDraft(L"我的笔记", 0);
    REQUIRE(u8->displayName() == L"我的笔记");
    REQUIRE(mgr.documentAt(0) == u8);

    // 场景 5：非哨兵负位置（防御）钳到 0；构建会话数据记录完整顺序与 uid
    Document* u0 = mgr.restoreUntitledDraft(L"未命名-0", -3); // -3 ≠ -1 哨兵 → 钳 0
    REQUIRE(mgr.documentAt(0) == u0);
    const SessionData data = SessionManager::buildSessionData(mgr);
    REQUIRE(data.documents.size() == mgr.documentCount());
    REQUIRE(data.documents[0].uid == u0->uid());
}
