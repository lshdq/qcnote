// M2-A U1 单测：存储层（TC-U06 存储侧/TC-U08/TC-U09 + 原子写与错误路径）。
// 文件写入测试专用临时目录（QCNOTE_TEST_TMP / %TEMP%\qcnote-test-core），用例自清理。
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "core/localstorage.h"
#include "core/storage.h"
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

static LocalStorage& storage() {
    static LocalStorage s;
    return s;
}

// ===========================================================================
// TC-U06（存储侧）：同一文件按强制编码的两种解释
// ===========================================================================
TEST_CASE("TC-U06 强制编码读取（LocalStorage::read forced）", "[core][storage]") {
    testutil::TempDir dir(L"u06");
    const auto file = dir.file(L"g.txt");
    testutil::writeBytes(file, kGbkNiHao); // 磁盘是 GBK 字节

    SECTION("自动识别 → GBK 正确解码") {
        const auto r = storage().read(file, std::nullopt);
        REQUIRE(r.has_value());
        REQUIRE(r->encoding == TextEncoding::Gbk);
        REQUIRE(r->content == kUtf8NiHao);
        REQUIRE_FALSE(r->hasBom);
    }
    SECTION("强制 GBK → 同自动识别") {
        const auto r = storage().read(file, TextEncoding::Gbk);
        REQUIRE(r.has_value());
        REQUIRE(r->encoding == TextEncoding::Gbk);
        REQUIRE(r->content == kUtf8NiHao);
    }
    SECTION("强制 UTF-8 → U+FFFD 呈现（同一字节流两种解释）") {
        const auto r = storage().read(file, TextEncoding::Utf8);
        REQUIRE(r.has_value());
        REQUIRE(r->encoding == TextEncoding::Utf8);
        // C4 E3 BA C3 按最大无效子序列：C4 → 1 个 U+FFFD；E3 BA（C3 非
        // continuation 中断）→ 1 个；C3（尾部截断）→ 1 个，共 3 个
        REQUIRE(r->content == cat({kReplacement, kReplacement, kReplacement}));
    }
    SECTION("UTF-8 文件强制 GBK：可打开但乱码，不失败") {
        const auto f2 = dir.file(L"u.txt");
        testutil::writeBytes(f2, kUtf8NiHao);
        const auto r = storage().read(f2, TextEncoding::Gbk);
        REQUIRE(r.has_value());
        REQUIRE(r->encoding == TextEncoding::Gbk);
        REQUIRE(r->content != kUtf8NiHao);
        REQUIRE(r->content == *decodeGbk(kUtf8NiHao));
    }
}

// ===========================================================================
// TC-U08：换行归一化（探测/写回往返/空文件/单行）
// ===========================================================================
TEST_CASE("TC-U08 换行归一化与风格写回往返", "[core][storage]") {
    testutil::TempDir dir(L"u08");
    LocalStorage& s = storage();

    SECTION("混合换行 → 内容归一 LF，任一 CRLF 即 Crlf") {
        const auto file = dir.file(L"mixed.txt");
        testutil::writeBytes(file, "a\r\nb\rc\nd\r\n");
        const auto r = s.read(file, std::nullopt);
        REQUIRE(r.has_value());
        REQUIRE(r->content == "a\nb\nc\nd\n");
        REQUIRE(r->lineEnding == LineEnding::Crlf);
    }
    SECTION("仅孤立 CR → 归一 LF、按 Lf 记录") {
        const auto file = dir.file(L"cr.txt");
        testutil::writeBytes(file, "a\rb\r");
        const auto r = s.read(file, std::nullopt);
        REQUIRE(r.has_value());
        REQUIRE(r->content == "a\nb\n");
        REQUIRE(r->lineEnding == LineEnding::Lf);
    }
    SECTION("空文件 → 空内容 + Lf") {
        const auto file = dir.file(L"empty.txt");
        testutil::writeBytes(file, "");
        const auto r = s.read(file, std::nullopt);
        REQUIRE(r.has_value());
        REQUIRE(r->content.empty());
        REQUIRE(r->lineEnding == LineEnding::Lf);
        REQUIRE(r->encoding == TextEncoding::Utf8); // 空文件是合法 UTF-8
    }
    SECTION("单行无换行") {
        const auto file = dir.file(L"single.txt");
        testutil::writeBytes(file, "one line");
        const auto r = s.read(file, std::nullopt);
        REQUIRE(r.has_value());
        REQUIRE(r->content == "one line");
        REQUIRE(r->lineEnding == LineEnding::Lf);
    }
    SECTION("CRLF 写回：磁盘为 \r\n，读回往返一致") {
        const auto file = dir.file(L"crlf.txt");
        REQUIRE(s.write(file, "a\nb", TextEncoding::Utf8, false, LineEnding::Crlf).has_value());
        REQUIRE(testutil::readBytes(file) == "a\r\nb");
        const auto back = s.read(file, std::nullopt);
        REQUIRE(back.has_value());
        REQUIRE(back->content == "a\nb");
        REQUIRE(back->lineEnding == LineEnding::Crlf);
    }
    SECTION("LF 写回：磁盘保持 \n") {
        const auto file = dir.file(L"lf.txt");
        REQUIRE(s.write(file, "a\nb", TextEncoding::Utf8, false, LineEnding::Lf).has_value());
        REQUIRE(testutil::readBytes(file) == "a\nb");
    }
    SECTION("GBK + CRLF 组合往返") {
        const auto file = dir.file(L"gbkcrlf.txt");
        REQUIRE(s.write(file, "你\n好", TextEncoding::Gbk, false, LineEnding::Crlf).has_value());
        REQUIRE(testutil::readBytes(file) == cat({kGbkNiHao.substr(0, 2), "\r\n",
                                                  kGbkNiHao.substr(2)}));
        const auto back = s.read(file, std::nullopt);
        REQUIRE(back.has_value());
        REQUIRE(back->content == "你\n好");
        REQUIRE(back->encoding == TextEncoding::Gbk);
        REQUIRE(back->lineEnding == LineEnding::Crlf);
    }
}

// ===========================================================================
// TC-U09：BOM 写回（存储侧）
// ===========================================================================
TEST_CASE("TC-U09 BOM 写回", "[core][storage]") {
    testutil::TempDir dir(L"u09");
    LocalStorage& s = storage();

    SECTION("hasBom=true 保存写回 BOM") {
        const auto file = dir.file(L"bom.txt");
        REQUIRE(s.write(file, cat({kUtf8NiHao}), TextEncoding::Utf8, true, LineEnding::Lf)
                    .has_value());
        REQUIRE(testutil::readBytes(file) == cat({kBom, kUtf8NiHao}));
        // 读回：剥离 BOM、记录 hasBom
        const auto back = s.read(file, std::nullopt);
        REQUIRE(back.has_value());
        REQUIRE(back->hasBom);
        REQUIRE(back->content == kUtf8NiHao);
    }
    SECTION("hasBom=false 不新增 BOM") {
        const auto file = dir.file(L"nobom.txt");
        REQUIRE(
            s.write(file, cat({kUtf8NiHao}), TextEncoding::Utf8, false, LineEnding::Lf).has_value());
        REQUIRE(testutil::readBytes(file) == kUtf8NiHao);
    }
    SECTION("GBK 无 BOM 概念（hasBom 参数被忽略）") {
        const auto file = dir.file(L"gbk.txt");
        REQUIRE(
            s.write(file, cat({kUtf8NiHao}), TextEncoding::Gbk, true, LineEnding::Lf).has_value());
        REQUIRE(testutil::readBytes(file) == kGbkNiHao); // 无 BOM 前缀
    }
    SECTION("GBK 转 UTF-8 保存时 BOM 取传入值（文档历史值由 DocumentManager 提供）") {
        const auto file = dir.file(L"conv.txt");
        testutil::writeBytes(file, kGbkNiHao);
        auto loaded = s.read(file, std::nullopt); // GBK 读取
        REQUIRE(loaded.has_value());
        // 文档历史 hasBom=false → 转 UTF-8 写出无 BOM
        REQUIRE(s.write(file, loaded->content, TextEncoding::Utf8, loaded->hasBom,
                        loaded->lineEnding)
                    .has_value());
        REQUIRE(testutil::readBytes(file) == kUtf8NiHao);
    }
}

// ===========================================================================
// 补充：原子写——失败不破坏原文件、临时文件清理
// ===========================================================================
TEST_CASE("补充：原子写失败不破坏原文件且清理临时文件", "[core][storage]") {
    testutil::TempDir dir(L"atomic");
    LocalStorage& s = storage();

    SECTION("GBK 不可编码：写盘前失败，原文件与目录不受影响") {
        const auto file = dir.file(L"keep.txt");
        testutil::writeBytes(file, kUtf8NiHao);
        const auto r = s.write(file, cat({kUtf8NiHao, kEmojiGrin}), TextEncoding::Gbk, false,
                               LineEnding::Lf);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().kind == StorageErrorKind::EncodingFailed);
        REQUIRE(r.error().message.find(L"第 1 行") != std::wstring::npos); // 含行号定位
        REQUIRE(testutil::readBytes(file) == kUtf8NiHao);                 // 原文件原样
        REQUIRE(testutil::entryCount(dir.path) == 1);                     // 无临时文件残留
    }

    SECTION("目标被独占打开（无 FILE_SHARE_DELETE）：替换失败，原文件完整，临时清理") {
        const auto file = dir.file(L"locked.txt");
        testutil::writeBytes(file, "original");
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0 /* 不共享删除 */, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(handle != INVALID_HANDLE_VALUE);
        const auto r = s.write(file, "new content", TextEncoding::Utf8, false, LineEnding::Lf);
        CloseHandle(handle);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().kind == StorageErrorKind::WriteFailed);
        REQUIRE(testutil::readBytes(file) == "original");     // 原文件未被破坏
        REQUIRE(testutil::entryCount(dir.path) == 1);         // 临时文件已清理
    }

    SECTION("目录不存在：临时文件创建失败报错") {
        const auto file = dir.path / L"no-such-dir" / L"x.txt";
        const auto r = s.write(file, "x", TextEncoding::Utf8, false, LineEnding::Lf);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().kind == StorageErrorKind::WriteFailed);
    }

    SECTION("目标路径是目录：替换失败（目录不可被文件替换）") {
        const auto sub = dir.path / L"subdir";
        std::filesystem::create_directories(sub);
        const auto r = s.write(sub, "x", TextEncoding::Utf8, false, LineEnding::Lf);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().kind == StorageErrorKind::WriteFailed);
        REQUIRE(std::filesystem::is_directory(sub)); // 目录原样
        REQUIRE(testutil::entryCount(dir.path) == 1); // 临时清理（子目录本身计 1 项）
    }

    SECTION("成功写：无临时文件残留") {
        const auto file = dir.file(L"ok.txt");
        REQUIRE(s.write(file, "hello", TextEncoding::Utf8, false, LineEnding::Lf).has_value());
        REQUIRE(testutil::entryCount(dir.path) == 1);
    }
}

// ===========================================================================
// 补充：读取错误路径（不存在/目录/空路径）
// ===========================================================================
TEST_CASE("补充：读取错误类别与消息", "[core][storage]") {
    testutil::TempDir dir(L"errs");
    LocalStorage& s = storage();

    SECTION("文件不存在 → NotFound + 中文消息") {
        const auto r = s.read(dir.file(L"missing.txt"), std::nullopt);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().kind == StorageErrorKind::NotFound);
        REQUIRE(r.error().message.find(L"文件不存在") != std::wstring::npos);
    }
    SECTION("目标是目录 → IsDirectory（isFile 预检，DEC-015 对齐）") {
        const auto sub = dir.path / L"adir";
        std::filesystem::create_directories(sub);
        const auto r = s.read(sub, std::nullopt);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().kind == StorageErrorKind::IsDirectory);
        REQUIRE(r.error().message.find(L"目录") != std::wstring::npos);
    }
    SECTION("空路径 → InvalidUrl") {
        const auto r = s.read({}, std::nullopt);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().kind == StorageErrorKind::InvalidUrl);
    }
    SECTION("写空路径 → InvalidUrl") {
        const auto r = s.write({}, "x", TextEncoding::Utf8, false, LineEnding::Lf);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().kind == StorageErrorKind::InvalidUrl);
    }
}

// ===========================================================================
// 补充：storageForUrl scheme 分发（未注册报「不支持的存储类型」）
// ===========================================================================
TEST_CASE("补充：scheme 分发与不支持的存储类型", "[core][storage]") {
    testutil::TempDir dir(L"scheme");

    SECTION("LocalStorage 声明 file scheme 且支持本地 path") {
        LocalStorage s;
        REQUIRE(s.scheme() == L"file");
        REQUIRE(s.supports(dir.path / L"x.txt"));
    }
    SECTION("未注册任何存储：storageForUrl 为空，openPath 报不支持的存储类型") {
        DocumentManager manager(false); // 不注册内置存储
        REQUIRE(manager.storageForUrl(dir.file(L"x.txt")) == nullptr);
        const auto r = manager.openPath(dir.file(L"x.txt"));
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().message.find(L"不支持的存储类型") != std::wstring::npos);
    }
    SECTION("parseFileUrl：file:/// 剥前缀，其他 scheme 空") {
        REQUIRE(parseFileUrl(L"file:///C:/dir/a.txt") == std::filesystem::path(L"C:/dir/a.txt"));
        REQUIRE(parseFileUrl(L"C:\\dir\\a.txt") == std::filesystem::path(L"C:\\dir\\a.txt"));
        REQUIRE_FALSE(parseFileUrl(L"http://example.com/a.txt").has_value());
        REQUIRE_FALSE(parseFileUrl(L"file://host/share/a.txt").has_value());
    }
    SECTION("LocalStorage::supports 仅认本地 path 形态（异 scheme URL 不吞）") {
        LocalStorage s;
        REQUIRE(s.supports(dir.file(L"x.txt")));
        REQUIRE(s.supports(std::filesystem::path(L"C:\\temp\\a.txt")));
        REQUIRE(s.supports(std::filesystem::path(L"\\\\server\\share\\a.txt")));  // UNC 合法本地形态
        REQUIRE_FALSE(s.supports(std::filesystem::path(L"http://example.com/a.txt")));
        REQUIRE_FALSE(s.supports(std::filesystem::path(L"file://host/share/a.txt")));
    }
    SECTION("后注册的异 scheme 存储不被先注册的 LocalStorage 吞掉") {
        // 最小桩：只参与分发（read/write 不被本测试调用）
        class NetStorageStub : public DocumentStorage {
        public:
            std::wstring scheme() const override { return L"http"; }
            bool supports(const std::filesystem::path& path) const override {
                return path.wstring().starts_with(L"http://");
            }
            std::expected<LoadedData, StorageError>
            read(const std::filesystem::path&, std::optional<TextEncoding>) const override {
                return std::unexpected(
                    StorageError{StorageErrorKind::UnsupportedScheme, L"stub 不可读"});
            }
            std::expected<void, StorageError>
            write(const std::filesystem::path&, const std::string&, TextEncoding, bool,
                  LineEnding) const override {
                return std::unexpected(
                    StorageError{StorageErrorKind::UnsupportedScheme, L"stub 不可写"});
            }
        };
        DocumentManager manager;  // 默认注册 LocalStorage（先注册）
        NetStorageStub net;       // 后注册的网络 scheme 存储
        manager.registerStorage(net);
        REQUIRE(manager.storageForUrl(std::filesystem::path(L"http://example.com/a.txt")) ==
                &net);
        REQUIRE(manager.storageForUrl(dir.file(L"x.txt")) != &net);  // 本地 path 不归它
        REQUIRE(manager.storageForUrl(dir.file(L"x.txt")) != nullptr);
    }
}
