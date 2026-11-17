#pragma once
#include <filesystem>
#include <string>

namespace shotlet {
// 临时文件与目标位于同一文件系统；提交失败或取消时保留原文件。
class AtomicFile {
public:
    ~AtomicFile();
    AtomicFile() = default;
    AtomicFile(const AtomicFile&) = delete;
    AtomicFile& operator=(const AtomicFile&) = delete;
    bool Prepare(const std::filesystem::path& target, std::string& error);
    bool Commit(std::string& error);
    void Discard();
    const std::filesystem::path& Path() const { return m_temporary; }
private:
    std::filesystem::path m_target, m_directory, m_temporary;
};
}
