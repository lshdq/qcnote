// M2-A 核心层单测公共辅助：临时目录与文件读写。
// 临时目录优先取 QCNOTE_TEST_TMP 环境变量（本机构建由 CMake test 属性指向
// D:\temp\aiRunner\opencode\...；他机/CI 回退 %TEMP%\qcnote-test-core）。
#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace testutil {

/// 测试根目录（存在性保证）
inline std::filesystem::path tempRoot() {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (const char* env = std::getenv("QCNOTE_TEST_TMP"); env && *env) {
        const fs::path p = fs::path(env);
        fs::create_directories(p, ec);
        return p;
    }
    const fs::path p = fs::temp_directory_path() / L"qcnote-test-core";
    fs::create_directories(p, ec);
    return p;
}

namespace detail {
inline std::wstring uniqueSuffix() {
    static std::atomic<unsigned> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_wstring(static_cast<unsigned long long>(now)) + L"-"
           + std::to_wstring(GetCurrentProcessId()) + L"-"
           + std::to_wstring(counter.fetch_add(1));
}
} // namespace detail

/// 每个测试用例独立的临时目录（RAII：析构时递归清理）
struct TempDir {
    std::filesystem::path path;

    explicit TempDir(const wchar_t* tag)
        : path(tempRoot() / (std::wstring(tag) + L"-" + detail::uniqueSuffix())) {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path file(const wchar_t* name) const { return path / name; }
};

/// 写原始字节到文件（二进制，无任何转换）
inline void writeBytes(const std::filesystem::path& p, std::string_view bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// 读全部原始字节
inline std::string readBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

/// 目录内条目数（临时文件清理断言用）
inline std::size_t entryCount(const std::filesystem::path& dir) {
    std::error_code ec;
    std::size_t n = 0;
    for (auto it = std::filesystem::directory_iterator(dir, ec);
         it != std::filesystem::directory_iterator(); ++it)
        ++n;
    return n;
}

} // namespace testutil
