#include "AtomicFile.h"
#include <atomic>
#include <chrono>
#if defined(SHOTLET_PLATFORM_WINDOWS)
#include "platform/PlatformWin32.h"
#endif

namespace shotlet {
AtomicFile::~AtomicFile() { Discard(); }

bool AtomicFile::Prepare(const std::filesystem::path& target, std::string& error) {
    Discard();
    error.clear();
    if (target.empty()) { error = "未指定保存路径"; return false; }
    static std::atomic_uint64_t sequence{0};
    constexpr int MaximumAttempts = 16;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto parent = target.has_parent_path() ? target.parent_path() : std::filesystem::path(".");
    for (int attempt = 0; attempt < MaximumAttempts; ++attempt) {
        const auto directory = parent / (".shotlet-output-" + std::to_string(nonce) + "-" + std::to_string(sequence++));
        std::error_code ec;
        if (std::filesystem::create_directory(directory, ec)) {
            m_target = target; m_directory = directory;
            m_temporary = directory / target.filename();
            return true;
        }
        if (ec && ec != std::errc::file_exists) { error = ec.message(); return false; }
    }
    error = "无法创建临时输出目录";
    return false;
}

bool AtomicFile::Commit(std::string& error) {
    error.clear();
    if (m_temporary.empty()) { error = "没有可保存的文件"; return false; }
#if defined(SHOTLET_PLATFORM_WINDOWS)
    if (!MoveFileExW(m_temporary.c_str(), m_target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = std::system_category().message(GetLastError()); return false;
    }
#else
    std::error_code ec;
    std::filesystem::rename(m_temporary, m_target, ec);
    if (ec) { error = ec.message(); return false; }
#endif
    Discard();
    return true;
}

void AtomicFile::Discard() {
    std::error_code ignored;
    if (!m_directory.empty()) std::filesystem::remove_all(m_directory, ignored);
    m_target.clear(); m_directory.clear(); m_temporary.clear();
}
}
