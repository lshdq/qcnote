// M2-A U1 单测：文档层（Document 元数据/基线语义 + DocumentManager 生命周期/
// 遮蔽重载/编码转换/回调）。TC-U09 文档侧（BOM 历史值）亦在此。
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "core/document.h"
#include "core/documentmanager.h"
#include "test_core_common.h"

using namespace core;
using namespace std::string_view_literals;

constexpr auto kUtf8NiHao = "\xE4\xBD\xA0\xE5\xA5\xBD"sv; // “你好” UTF-8
constexpr auto kGbkNiHao = "\xC4\xE3\xBA\xC3"sv;          // “你好” CP936
constexpr auto kBom = "\xEF\xBB\xBF"sv;
constexpr auto kEmojiGrin = "\xF0\x9F\x98\x80"sv;
constexpr auto kReplacement = "\xEF\xBF\xBD"sv;

static std::string cat(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (const auto p : parts)
        out.append(p);
    return out;
}

// ===========================================================================
// Document：元数据、uid、显示名
// ===========================================================================
TEST_CASE("Document 元数据默认值与 uid", "[core][doc]") {
    SECTION("新文档默认：UTF-8、无 BOM、LF、空内容、不脏") {
        Document doc(L"未命名-7");
        REQUIRE(doc.encoding() == TextEncoding::Utf8);
        REQUIRE_FALSE(doc.hasBom());
        REQUIRE(doc.lineEnding() == LineEnding::Lf);
        REQUIRE(doc.url().empty());
        REQUIRE(doc.content().empty());
        REQUIRE_FALSE(doc.modified());
        REQUIRE(doc.cursorPosition() == 0);
        REQUIRE(doc.displayName() == L"未命名-7");
    }
    SECTION("空未命名名默认「未命名」") {
        Document doc;
        REQUIRE(doc.displayName() == L"未命名");
    }
    SECTION("uid：非空、36 字符、构造后不可变") {
        Document doc;
        REQUIRE(doc.uid().size() == 36); // GUID 无大括号形式
        const std::string snapshot = doc.uid();
        doc.setContent("changed");
        doc.setUrl(L"D:/x/a.txt");
        REQUIRE(doc.uid() == snapshot);
    }
    SECTION("uid 全局唯一（连续构造不重复）") {
        Document a, b;
        REQUIRE(a.uid() != b.uid());
    }
    SECTION("displayName 随 url 文件名更新，回到未命名名") {
        Document doc(L"草稿");
        doc.setUrl(std::filesystem::path(L"D:/工作/读我.txt"));
        REQUIRE(doc.displayName() == L"读我.txt");
    }
}

// ===========================================================================
// Document：修改标记（基线比较）、markSaved、applyLoadedState、光标
// ===========================================================================
TEST_CASE("Document 修改标记与基线语义", "[core][doc]") {
    Document doc;
    int contentEvents = 0, modifiedEvents = 0;
    doc.onContentChanged = [&] { ++contentEvents; };
    doc.onModifiedChanged = [&] { ++modifiedEvents; };

    SECTION("setContent 越基线置脏（翻转发一次通知）") {
        doc.setContent("a");
        REQUIRE(doc.modified());
        REQUIRE(contentEvents == 1);
        REQUIRE(modifiedEvents == 1);
    }
    SECTION("脏值之间切换：内容通知但不翻转脏通知") {
        doc.setContent("a");
        doc.setContent("ab");
        REQUIRE(doc.modified());
        REQUIRE(contentEvents == 2);
        REQUIRE(modifiedEvents == 1); // 仍脏，无翻转
    }
    SECTION("回到基线自动清脏（撤消回原样场景）") {
        doc.setContent("a");
        doc.setContent("");
        REQUIRE_FALSE(doc.modified());
        REQUIRE(modifiedEvents == 2); // 脏→干净翻转
    }
    SECTION("同值 setContent 不置脏不通知") {
        doc.setContent("");
        REQUIRE(contentEvents == 0);
        REQUIRE_FALSE(doc.modified());
    }
    SECTION("markSaved 重置基线；干净态调用无通知") {
        doc.setContent("b");
        doc.markSaved();
        REQUIRE_FALSE(doc.modified());
        REQUIRE(modifiedEvents == 2); // 置脏 + 清脏
        const int before = modifiedEvents;
        doc.markSaved();
        REQUIRE(modifiedEvents == before); // 已干净：不重复通知
        doc.setContent("b2");
        REQUIRE(doc.modified());
        doc.setContent("b"); // 回到基线 → 清脏
        REQUIRE_FALSE(doc.modified());
    }
    SECTION("基线哈希 = 基线内容 SHA-256（不随当前编辑变化）") {
        doc.applyLoadedState(std::filesystem::path(L"D:/t/a.txt"), "base", LineEnding::Lf,
                             TextEncoding::Utf8, false);
        REQUIRE(doc.baselineHash() == sha256Hex("base"));
        doc.setContent("edited");
        REQUIRE(doc.baselineHash() == sha256Hex("base")); // 基线不动
        doc.markSaved();
        REQUIRE(doc.baselineHash() == sha256Hex("edited"));
    }
    SECTION("applyLoadedState：全元数据刷新、基线重置、不动光标") {
        doc.setContent("dirty");
        doc.setCursorPosition(9);
        doc.applyLoadedState(std::filesystem::path(L"D:/t/b.txt"), "loaded\ncontent",
                             LineEnding::Crlf, TextEncoding::Gbk, false);
        REQUIRE(doc.content() == "loaded\ncontent");
        REQUIRE_FALSE(doc.modified());
        REQUIRE(doc.lineEnding() == LineEnding::Crlf);
        REQUIRE(doc.encoding() == TextEncoding::Gbk);
        REQUIRE(doc.cursorPosition() == 9); // 不动光标（钳制归 DocumentManager）
    }
    SECTION("光标负值钳为 0；同值不通知") {
        int metaEvents = 0;
        doc.onMetadataChanged = [&] { ++metaEvents; };
        doc.setCursorPosition(5);
        REQUIRE(doc.cursorPosition() == 5);
        REQUIRE(metaEvents == 1);
        doc.setCursorPosition(-3); // 负值钳为 0（与当前 5 不同 → 通知）
        REQUIRE(doc.cursorPosition() == 0);
        REQUIRE(metaEvents == 2);
        doc.setCursorPosition(0); // 同值
        REQUIRE(metaEvents == 2);
        doc.setCursorPosition(-1); // 负值钳为 0 = 当前值 → 同值不通知
        REQUIRE(metaEvents == 2);
    }
    SECTION("编码/换行元数据变化不置脏") {
        doc.applyLoadedState({}, "x", LineEnding::Lf, TextEncoding::Utf8, false);
        doc.setEncoding(TextEncoding::Gbk);
        doc.setLineEnding(LineEnding::Crlf);
        REQUIRE_FALSE(doc.modified());
    }
}

// ===========================================================================
// DocumentManager：未命名编号（递增、跨关闭不复位、跳过占用）
// ===========================================================================
TEST_CASE("DocumentManager 未命名编号", "[core][dm]") {
    DocumentManager m;

    SECTION("连续新建：未命名-1/2/3") {
        REQUIRE(m.newDocument()->displayName() == L"未命名-1");
        REQUIRE(m.newDocument()->displayName() == L"未命名-2");
        REQUIRE(m.newDocument()->displayName() == L"未命名-3");
        REQUIRE(m.documentCount() == 3);
    }
    SECTION("关闭后编号不复位（单调递增）") {
        m.newDocument();
        m.newDocument();
        m.close(0);
        REQUIRE(m.newDocument()->displayName() == L"未命名-3");
    }
    SECTION("跳过被占用的显示名（文件名恰为 未命名-N 的已打开文档）") {
        testutil::TempDir dir(L"untitled");
        m.newDocument(); // 未命名-1
        // 创建名为「未命名-2」的文件并打开（displayName 占用 未命名-2）
        const auto file = dir.file(L"未命名-2");
        testutil::writeBytes(file, "x");
        REQUIRE(m.openPath(file).has_value());
        REQUIRE(m.documentAt(1)->displayName() == L"未命名-2");
        // 下一个新建跳过 2 → 未命名-3
        REQUIRE(m.newDocument()->displayName() == L"未命名-3");
    }
}

// ===========================================================================
// DocumentManager：openPaths（去重激活、失败列表、空路径跳过）
// ===========================================================================
TEST_CASE("DocumentManager openPaths 去重与失败收集", "[core][dm]") {
    testutil::TempDir dir(L"open");
    DocumentManager m;
    const auto a = dir.file(L"a.txt");
    const auto b = dir.file(L"b.txt");
    testutil::writeBytes(a, "aaa");
    testutil::writeBytes(b, "bbb");

    SECTION("批量打开两个文件，活动为最后一个") {
        const auto failures = m.openPaths({a, b});
        REQUIRE(failures.empty());
        REQUIRE(m.documentCount() == 2);
        REQUIRE(m.activeDocument()->displayName() == L"b.txt");
    }
    SECTION("重复路径：激活已有，不新建") {
        m.openPaths({a, b});
        const auto failures = m.openPaths({a});
        REQUIRE(failures.empty());
        REQUIRE(m.documentCount() == 2);
        REQUIRE(m.activeDocument() == m.documentAt(0));
        REQUIRE(m.activeDocument()->displayName() == L"a.txt");
    }
    SECTION("失败返回错误列表且不中断后续路径") {
        const auto missing = dir.file(L"missing.txt");
        const auto failures = m.openPaths({missing, a});
        REQUIRE(failures.size() == 1);
        REQUIRE(failures[0].path == missing);
        REQUIRE(failures[0].message.find(L"文件不存在") != std::wstring::npos);
        REQUIRE(m.documentCount() == 1); // a 仍被打开
        REQUIRE(m.activeDocument()->displayName() == L"a.txt");
    }
    SECTION("目录路径报错（isFile 预检）") {
        const auto failures = m.openPaths({dir.path});
        REQUIRE(failures.size() == 1);
        REQUIRE(failures[0].message.find(L"目录") != std::wstring::npos);
    }
    SECTION("空路径跳过（无错误无文档）") {
        const auto failures = m.openPaths({{}});
        REQUIRE(failures.empty());
        REQUIRE(m.documentCount() == 0);
    }
}

// ===========================================================================
// DocumentManager：save / saveAs（含遮蔽重载 DEC-008）
// ===========================================================================
TEST_CASE("DocumentManager save 与 saveAs", "[core][dm]") {
    testutil::TempDir dir(L"save");
    DocumentManager m;
    const auto a = dir.file(L"a.txt");
    const auto b = dir.file(L"b.txt");
    testutil::writeBytes(a, "AAA");
    testutil::writeBytes(b, "BBB");

    SECTION("未命名文档保存 → InvalidUrl（UI 捕获转另存为）") {
        Document* doc = m.newDocument();
        const auto r = m.save(doc);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().kind == StorageErrorKind::InvalidUrl);
    }
    SECTION("保存写出当前内容并清脏") {
        auto opened = m.openPath(a);
        REQUIRE(opened.has_value());
        Document* doc = *opened;
        doc->setContent("new content");
        REQUIRE(doc->modified());
        REQUIRE(m.save(doc).has_value());
        REQUIRE_FALSE(doc->modified());
        REQUIRE(testutil::readBytes(a) == "new content");
    }
    SECTION("另存为：url/显示名更新、清脏、磁盘就位") {
        Document* doc = m.newDocument();
        doc->setContent("saved as");
        const SaveAsResult r = m.saveAs(doc, b);
        REQUIRE(r.written);
        REQUIRE_FALSE(r.error.has_value());
        REQUIRE(doc->url() == b);
        REQUIRE(doc->displayName() == L"b.txt");
        REQUIRE_FALSE(doc->modified());
        REQUIRE(testutil::readBytes(b) == "saved as");
    }
    SECTION("遮蔽重载（DEC-008）：另存到其他已打开文档路径，对方从磁盘重载") {
        Document* docA = *m.openPath(a); // 内容 AAA
        Document* docB = *m.openPath(b); // 内容 BBB
        docA->setCursorPosition(2);
        docB->setContent("shadow written");
        const SaveAsResult r = m.saveAs(docB, a);
        REQUIRE(r.written);
        REQUIRE_FALSE(r.error.has_value());
        REQUIRE(testutil::readBytes(a) == "shadow written");
        // docB：url 更新 + 清脏
        REQUIRE(docB->url() == a);
        REQUIRE_FALSE(docB->modified());
        // docA（被遮蔽）：内容重载为磁盘新内容、清脏、光标不动、url 不变
        REQUIRE(docA->content() == "shadow written");
        REQUIRE_FALSE(docA->modified());
        REQUIRE(docA->cursorPosition() == 2);
        REQUIRE(docA->url() == a);
    }
    SECTION("另存为写盘失败：文档状态不变（脏保留）") {
        Document* doc = m.newDocument();
        doc->setContent("will fail");
        const SaveAsResult r = m.saveAs(doc, dir.path / L"no-dir" / L"x.txt");
        REQUIRE_FALSE(r.written);
        REQUIRE(r.error.has_value());
        REQUIRE(doc->url().empty());
        REQUIRE(doc->modified()); // 脏标记保留（保存未发生）
    }
    SECTION("GBK 不可编码另存为：写盘前失败、错误含行号") {
        Document* doc = m.newDocument();
        doc->setContent(cat({"你\n好\n", kEmojiGrin}));
        doc->setEncoding(TextEncoding::Gbk);
        const SaveAsResult r = m.saveAs(doc, dir.file(L"g.txt"));
        REQUIRE_FALSE(r.written);
        REQUIRE(r.error.has_value());
        REQUIRE(r.error->kind == StorageErrorKind::EncodingFailed);
        REQUIRE(r.error->message.find(L"第 3 行") != std::wstring::npos);
    }
}

// ===========================================================================
// reloadWithEncoding：换行重探测、光标钳制、不置脏
// ===========================================================================
TEST_CASE("DocumentManager reloadWithEncoding", "[core][dm]") {
    testutil::TempDir dir(L"reload");
    DocumentManager m;
    const auto file = dir.file(L"g.txt");
    // GBK “你\r\n好”：自动识别为 GBK、CRLF
    testutil::writeBytes(file, cat({kGbkNiHao.substr(0, 2), "\r\n", kGbkNiHao.substr(2)}));

    Document* doc = *m.openPath(file);
    REQUIRE(doc->encoding() == TextEncoding::Gbk);
    REQUIRE(doc->content() == "你\n好");
    REQUIRE(doc->lineEnding() == LineEnding::Crlf);

    SECTION("强制 UTF-8 重载：乱码呈现、不置脏、光标钳制、换行重探测") {
        doc->setContent("编辑过的内容");
        REQUIRE(doc->modified());
        doc->setCursorPosition(100);
        const auto r = m.reloadWithEncoding(doc, TextEncoding::Utf8);
        REQUIRE(r.has_value());
        // C4 E3 0D 0A BA C3 按 UTF-8：4 个无效首字节 → 4 个 U+FFFD + CRLF
        REQUIRE(doc->content() == cat({kReplacement, kReplacement, "\n", kReplacement,
                                       kReplacement}));
        REQUIRE(doc->encoding() == TextEncoding::Utf8);
        REQUIRE(doc->lineEnding() == LineEnding::Crlf); // 重探测（\r\n 保留）
        REQUIRE_FALSE(doc->modified());
        // 光标钳制到新内容字节长度 = 4×3 + 1 = 13
        REQUIRE(doc->cursorPosition() == 13);
    }
    SECTION("强制同编码重载：丢弃未保存修改（不置脏）") {
        doc->setContent("脏内容");
        const auto r = m.reloadWithEncoding(doc, TextEncoding::Gbk);
        REQUIRE(r.has_value());
        REQUIRE(doc->content() == "你\n好");
        REQUIRE_FALSE(doc->modified());
    }
    SECTION("未命名文档：报错且不动内容") {
        Document* untitled = m.newDocument();
        untitled->setContent("x");
        const auto r = m.reloadWithEncoding(untitled, TextEncoding::Utf8);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().kind == StorageErrorKind::InvalidUrl);
        REQUIRE(r.error().message.find(L"尚未保存") != std::wstring::npos);
        REQUIRE(untitled->content() == "x");
    }
    SECTION("读取失败：保留原文档内容") {
        Document* doc2 = *m.openPath(file);
        doc2->setContent("keep me");
        std::filesystem::remove(file);
        const auto r = m.reloadWithEncoding(doc2, TextEncoding::Gbk);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().kind == StorageErrorKind::NotFound);
        REQUIRE(doc2->content() == "keep me"); // 不产生半更新状态
        REQUIRE(doc2->modified());
    }
}

// ===========================================================================
// saveWithEncoding：一步转换、失败回滚、BOM 历史值（TC-U09 文档侧）
// ===========================================================================
TEST_CASE("DocumentManager saveWithEncoding（编码转换）", "[core][dm]") {
    testutil::TempDir dir(L"convert");
    DocumentManager m;

    SECTION("GBK → UTF-8 转换：磁盘变 UTF-8、无 BOM（历史值 false）") {
        const auto file = dir.file(L"gbk.txt");
        testutil::writeBytes(file, kGbkNiHao);
        Document* doc = *m.openPath(file);
        REQUIRE(doc->encoding() == TextEncoding::Gbk);
        const auto r = m.saveWithEncoding(doc, TextEncoding::Utf8);
        REQUIRE(r.has_value());
        REQUIRE(testutil::readBytes(file) == kUtf8NiHao); // 无 BOM
        REQUIRE(doc->encoding() == TextEncoding::Utf8);
        REQUIRE_FALSE(doc->modified());
    }
    SECTION("UTF-8 → GBK 转换：磁盘变 GBK；hasBom 历史保留，转回 UTF-8 时写回 BOM") {
        const auto file = dir.file(L"bom.txt");
        testutil::writeBytes(file, cat({kBom, kUtf8NiHao})); // UTF-8 带 BOM
        Document* doc = *m.openPath(file);
        REQUIRE(doc->hasBom());
        // 转 GBK：无 BOM 概念，hasBom 记录保留
        REQUIRE(m.saveWithEncoding(doc, TextEncoding::Gbk).has_value());
        REQUIRE(testutil::readBytes(file) == kGbkNiHao);
        REQUIRE(doc->hasBom()); // 历史值不清除
        // 再转回 UTF-8：BOM 按历史值写回（TC-U09）
        REQUIRE(m.saveWithEncoding(doc, TextEncoding::Utf8).has_value());
        REQUIRE(testutil::readBytes(file) == cat({kBom, kUtf8NiHao}));
    }
    SECTION("UTF-8 无 BOM 转 GBK 再转回：全程无 BOM（TC-U09 历史值 false）") {
        const auto file = dir.file(L"plain.txt");
        testutil::writeBytes(file, kUtf8NiHao);
        Document* doc = *m.openPath(file);
        REQUIRE_FALSE(doc->hasBom());
        REQUIRE(m.saveWithEncoding(doc, TextEncoding::Gbk).has_value());
        REQUIRE(m.saveWithEncoding(doc, TextEncoding::Utf8).has_value());
        REQUIRE(testutil::readBytes(file) == kUtf8NiHao); // 不新增 BOM
    }
    SECTION("GBK 不可编码：失败、编码回滚、磁盘未被触碰、脏保留") {
        const auto file = dir.file(L"fail.txt");
        testutil::writeBytes(file, kGbkNiHao);
        Document* doc = *m.openPath(file);
        doc->setContent(cat({"你\n好\n", kEmojiGrin, " 结尾"}));
        const auto r = m.saveWithEncoding(doc, TextEncoding::Gbk);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().kind == StorageErrorKind::EncodingFailed);
        REQUIRE(r.error().message.find(L"第 3 行") != std::wstring::npos);
        REQUIRE(r.error().message.find(L"😀") != std::wstring::npos);
        REQUIRE(doc->encoding() == TextEncoding::Gbk); // 回滚到原编码
        REQUIRE(testutil::readBytes(file) == kGbkNiHao); // 磁盘原样
        REQUIRE(doc->modified());                        // 脏保留
    }
    SECTION("未命名文档：返回 InvalidUrl，编码保留供后续另存为按该编码写出") {
        Document* doc = m.newDocument();
        doc->setContent(cat({kUtf8NiHao}));
        const auto r = m.saveWithEncoding(doc, TextEncoding::Gbk);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().kind == StorageErrorKind::InvalidUrl);
        REQUIRE(doc->encoding() == TextEncoding::Gbk); // 已设并保留
        // 另存为按已设编码写出（GBK 字节）
        const SaveAsResult sr = m.saveAs(doc, dir.file(L"later.txt"));
        REQUIRE(sr.written);
        REQUIRE(testutil::readBytes(dir.file(L"later.txt")) == kGbkNiHao);
    }
}

// ===========================================================================
// close：邻居激活、事件时序
// ===========================================================================
TEST_CASE("DocumentManager close 与激活邻居", "[core][dm]") {
    DocumentManager m;
    Document* d1 = m.newDocument();
    Document* d2 = m.newDocument();
    Document* d3 = m.newDocument();
    REQUIRE(m.activeDocument() == d3);

    SECTION("关闭活动文档：优先激活前一个") {
        m.close(2);
        REQUIRE(m.documentCount() == 2);
        REQUIRE(m.activeDocument() == d2);
    }
    SECTION("关闭首个（其时为活动）：无前一个则激活后一个") {
        m.setActiveDocument(d1); // 关闭的是活动文档才触发邻居激活
        m.close(0);
        REQUIRE(m.activeDocument() == d2); // 原 index 1 前移
        REQUIRE(m.documentCount() == 2);
    }
    SECTION("关闭非活动的首个文档：活动不变") {
        m.close(0); // 活动是 d3，不受影响
        REQUIRE(m.activeDocument() == d3);
        REQUIRE(m.documentCount() == 2);
    }
    SECTION("关闭非活动文档：活动不变") {
        m.setActiveDocument(d1);
        m.close(1);
        REQUIRE(m.activeDocument() == d1);
    }
    SECTION("关闭最后一个：活动置空（欢迎页状态）") {
        m.close(0);
        m.close(0);
        m.close(0);
        REQUIRE(m.documentCount() == 0);
        REQUIRE(m.activeDocument() == nullptr);
    }
    SECTION("documentClosed 事件在移除后、销毁前发出（可读 uid）") {
        std::string uidOfClosed;
        Document* closed = nullptr;
        const std::string expectedUid = d1->uid(); // 关闭前留档（关闭后指针悬垂不可读）
        const auto token = m.documentClosed.subscribe([&](Document* doc) {
            closed = doc;
            uidOfClosed = doc->uid(); // 订阅者仍可安全访问（析构尚未发生）
        });
        m.close(0);
        m.documentClosed.unsubscribe(token);
        REQUIRE(closed == d1);
        REQUIRE(uidOfClosed == expectedUid); // 析构前读取成功
        REQUIRE(uidOfClosed.size() == 36);
    }
    SECTION("closeDocument 按指针关闭；未知指针无操作") {
        m.closeDocument(d2); // 关闭非活动文档：活动不变
        REQUIRE(m.documentCount() == 2);
        REQUIRE(m.activeDocument() == d3);
        m.close(99); // 越界索引无操作
        m.closeDocument(nullptr); // 空指针无操作
        REQUIRE(m.documentCount() == 2);
    }
}

// ===========================================================================
// 回调：多订阅、退订、各事件触发点
// ===========================================================================
TEST_CASE("DocumentManager 回调事件", "[core][dm]") {
    testutil::TempDir dir(L"events");
    DocumentManager m;
    const auto file = dir.file(L"e.txt");
    testutil::writeBytes(file, "hello");

    int listChanged1 = 0, listChanged2 = 0, activeChanged = 0, contentChanged = 0,
        modifiedChanged = 0, metadataChanged = 0;
    const auto t1 = m.documentsChanged.subscribe([&] { ++listChanged1; });
    const auto t2 = m.documentsChanged.subscribe([&] { ++listChanged2; });
    const auto t3 = m.activeDocumentChanged.subscribe([&] { ++activeChanged; });
    const auto t4 = m.documentContentChanged.subscribe([&](Document*) { ++contentChanged; });
    const auto t5 = m.documentModifiedChanged.subscribe([&](Document*) { ++modifiedChanged; });
    const auto t6 = m.documentMetadataChanged.subscribe([&](Document*) { ++metadataChanged; });

    SECTION("新建：documentsChanged + activeDocumentChanged；两个订阅者都收到") {
        Document* doc = m.newDocument();
        REQUIRE(listChanged1 == 1);
        REQUIRE(listChanged2 == 1);
        REQUIRE(activeChanged == 1);
        // 编辑：content/modified/metadata 事件按文档转发
        doc->setContent("x");
        REQUIRE(contentChanged == 1);
        REQUIRE(modifiedChanged == 1); // 干净→脏翻转
        doc->setEncoding(TextEncoding::Gbk);
        REQUIRE(metadataChanged == 1);
    }
    SECTION("打开：documentsChanged + active；加载即干净（无 modified 翻转）") {
        auto opened = m.openPath(file);
        REQUIRE(opened.has_value());
        REQUIRE(listChanged1 == 1);
        REQUIRE(activeChanged == 1);
        // 初始加载的 applyLoadedState 发生在回调接线之前（与旧版一致：文档入列
        // 前加载，订阅者经 documentsChanged 补连后才收到后续编辑事件）
        REQUIRE(contentChanged == 0);
        REQUIRE(modifiedChanged == 0); // 加载后即干净
        // 接线后的编辑事件正常流转
        (*opened)->setContent("edit");
        REQUIRE(contentChanged == 1);
        REQUIRE(modifiedChanged == 1);
    }
    SECTION("退订后不再接收") {
        m.documentsChanged.unsubscribe(t2);
        m.newDocument();
        REQUIRE(listChanged1 == 1);
        REQUIRE(listChanged2 == 0);
    }
    SECTION("激活切换触发 activeDocumentChanged；同指针不触发") {
        Document* a = m.newDocument();
        Document* b = m.newDocument();
        REQUIRE(activeChanged == 2);
        m.setActiveDocument(a);
        REQUIRE(activeChanged == 3);
        m.setActiveDocument(a); // 同指针
        REQUIRE(activeChanged == 3);
        m.activate(1);
        REQUIRE(activeChanged == 4);
        REQUIRE(m.activeDocument() == b);
    }
    SECTION("保存清脏触发 modifiedChanged（脏→干净）") {
        Document* doc = m.newDocument();
        doc->setContent("s");
        REQUIRE(modifiedChanged == 1);
        doc->setUrl(dir.file(L"n.txt"));
        REQUIRE(m.save(doc).has_value());
        REQUIRE(modifiedChanged == 2);
        REQUIRE_FALSE(doc->modified());
    }
    // 清理订阅（token 有效性覆盖）
    m.documentsChanged.unsubscribe(t1);
    m.documentsChanged.unsubscribe(t2);
    m.activeDocumentChanged.unsubscribe(t3);
    m.documentContentChanged.unsubscribe(t4);
    m.documentModifiedChanged.unsubscribe(t5);
    m.documentMetadataChanged.unsubscribe(t6);
}

// ===========================================================================
// 格式注册表与兜底
// ===========================================================================
namespace {
class MarkdownFormat : public DocumentFormat {
public:
    std::string id() const override { return "markdown"; }
    std::wstring displayName() const override { return L"Markdown"; }
    std::vector<std::wstring> nameFilters() const override { return {L"Markdown 文件 (*.md)"}; }
};
} // namespace

TEST_CASE("DocumentManager 格式注册表", "[core][dm]") {
    DocumentManager m;

    SECTION("默认兜底：任何文件名 → plaintext") {
        REQUIRE(m.formatForPath(std::filesystem::path(L"D:/x/a.txt"))->id() == "plaintext");
        REQUIRE(m.formatForPath(std::filesystem::path(L"D:/x/noext"))->id() == "plaintext");
        REQUIRE(m.formatForPath(std::filesystem::path(L"D:/x/a.md"))->id() == "plaintext");
    }
    SECTION("注册具体格式后优先于兜底") {
        MarkdownFormat md;
        m.registerFormat(md);
        REQUIRE(m.formatForPath(std::filesystem::path(L"D:/x/readme.MD"))->id() == "markdown");
        REQUIRE(m.formatForPath(std::filesystem::path(L"D:/x/a.txt"))->id() == "plaintext");
        REQUIRE(m.formatForPath(std::filesystem::path(L"D:/x/noext"))->id() == "plaintext");
    }
    SECTION("名过滤器聚合去重") {
        MarkdownFormat md;
        m.registerFormat(md);
        const auto filters = m.nameFilters();
        REQUIRE(std::find(filters.begin(), filters.end(), L"文本文件 (*.txt)") != filters.end());
        REQUIRE(std::find(filters.begin(), filters.end(), L"Markdown 文件 (*.md)") != filters.end());
        REQUIRE(std::find(filters.begin(), filters.end(), L"所有文件 (*)") != filters.end());
        // 去重：数量 = 3（无重复项）
        std::vector<std::wstring> unique(filters);
        std::sort(unique.begin(), unique.end());
        REQUIRE(std::unique(unique.begin(), unique.end()) == unique.end());
    }
    SECTION("重复注册同一实例被忽略") {
        MarkdownFormat md;
        m.registerFormat(md);
        m.registerFormat(md);
        const auto filters = m.nameFilters();
        REQUIRE(std::count(filters.begin(), filters.end(), L"Markdown 文件 (*.md)") == 1);
    }
}

// ===========================================================================
// 补充：hasUnsavedChanges / documentForPath
// ===========================================================================
TEST_CASE("DocumentManager 聚合查询", "[core][dm]") {
    testutil::TempDir dir(L"query");
    DocumentManager m;
    REQUIRE_FALSE(m.hasUnsavedChanges());

    Document* d1 = m.newDocument();
    REQUIRE_FALSE(m.hasUnsavedChanges());
    d1->setContent("dirty");
    REQUIRE(m.hasUnsavedChanges());

    const auto file = dir.file(L"q.txt");
    testutil::writeBytes(file, "q");
    auto opened = m.openPath(file);
    REQUIRE(opened.has_value());
    REQUIRE(m.documentForPath(file) == *opened);
    REQUIRE(m.documentForPath({}) == nullptr);
    REQUIRE(m.documentForPath(dir.file(L"none.txt")) == nullptr);
}
