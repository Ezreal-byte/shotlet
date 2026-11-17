#include "FontProvider.h"
#include "../utils/Logger.h"

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shotlet {

// 通过 CoreText 动态获取 PingFang SC 字体文件路径
static std::string FindPingFangFontPath() {
    NSDictionary* attrs = @{
        (NSString*)kCTFontFamilyNameAttribute: @"PingFang SC"
    };
    CTFontDescriptorRef descriptor =
        CTFontDescriptorCreateWithAttributes((CFDictionaryRef)attrs);

    CFArrayRef matched =
        CTFontDescriptorCreateMatchingFontDescriptors(descriptor, NULL);
    CFRelease(descriptor);

    if (!matched) return "";

    std::string result;
    CFIndex count = CFArrayGetCount(matched);
    for (CFIndex i = 0; i < count; i++) {
        CTFontDescriptorRef desc =
            (CTFontDescriptorRef)CFArrayGetValueAtIndex(matched, i);
        CFURLRef url =
            (CFURLRef)CTFontDescriptorCopyAttribute(desc, kCTFontURLAttribute);
        if (url) {
            NSString* path = [(__bridge NSURL*)url path];
            result = [path UTF8String];
            CFRelease(url);
            break;
        }
    }
    CFRelease(matched);
    return result;
}

// 缓存字体路径查找结果
struct FontPathCache {
    std::string path;
    bool found = false;
    bool searched = false;
};

static const FontPathCache& GetCachedFontPath() {
    static FontPathCache cached;
    if (!cached.searched) {
        cached.path = FindPingFangFontPath();
        cached.found = !cached.path.empty();
        cached.searched = true;
        if (!cached.found) {
            cached.path = "/System/Library/Fonts/STHeiti Medium.ttc";
            Logger::Warning("[FontProvider] PingFang SC not found via CoreText, falling back to STHeiti");
        }
    }
    return cached;
}

std::vector<std::string> GetChineseFontPaths() {
    const auto& cache = GetCachedFontPath();
    if (cache.found) {
        return { cache.path };
    }
    return { "/System/Library/Fonts/STHeiti Medium.ttc" };
}

int GetChineseFontNo() {
    const auto& cache = GetCachedFontPath();
    return cache.found ? 7 : 1;
}

// 字体文件数据缓存（预加载，避免每次 AddFontFromFileTTF 的磁盘 I/O）
// stb_truetype 对字体数据纯只读、按需访问（只解析用到的字形），
// 因此用 mmap 映射而非 fread 读入：74.6MB 的 PingFang.ttc 变成 file-backed
// clean pages——不计入物理 footprint，内存压力下系统可直接回收，
// page-in 也只加载实际触碰的页。映射有意保持到进程退出（缓存语义），
// file-backed 映射不占 dirty 内存，无需 munmap。
//
// SIGBUS 风险评估：mmap 的文件若被截断（映射区间缩小），后续访问越界页
// 会 SIGBUS。当前映射的文件全部位于 /System/Library/Fonts（PingFang.ttc
// 经 CoreText 解析、STHeiti 兜底），受 SIP 保护无法被就地截断；macOS
// 更新替换字体走新 inode + rename/unlink，旧 inode 的已映射页不受影响
// （映射绑定 inode 而非路径）。若将来映射非 SIP 保护路径（如用户字体），
// 需重新评估此假设或退回 fread 拷贝。
struct FontDataCache {
    std::vector<const unsigned char*> dataPtrs;
    std::vector<int> dataSizes;
    std::vector<std::vector<unsigned char>> ownedBuffers;  // 仅 mmap 失败时持有数据
    bool loaded = false;
};

// mmap 只读映射字体文件；失败返回 nullptr（调用方走 fread 兜底）
static const unsigned char* MapFontFileReadOnly(const std::string& path, size_t& outSize) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return nullptr;

    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return nullptr;
    }

    void* p = mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);  // 映射建立后 fd 即可关闭，不影响映射生命周期
    if (p == MAP_FAILED) return nullptr;

    outSize = static_cast<size_t>(st.st_size);
    return static_cast<const unsigned char*>(p);
}

// fread 兜底：mmap 不可用时退化为整文件读入（dirty 内存，但保证功能可用）
static bool LoadFontFileFallback(const std::string& path, std::vector<unsigned char>& out) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fileSize <= 0) { fclose(fp); return false; }
    out.resize(static_cast<size_t>(fileSize));
    size_t read = fread(out.data(), 1, out.size(), fp);
    fclose(fp);
    return read == out.size();
}

static FontDataCache& GetFontDataCache() {
    static FontDataCache cache;
    if (!cache.loaded) {
        auto paths = GetChineseFontPaths();
        for (const auto& path : paths) {
            size_t mappedSize = 0;
            if (const unsigned char* p = MapFontFileReadOnly(path, mappedSize)) {
                cache.dataPtrs.push_back(p);
                cache.dataSizes.push_back(static_cast<int>(mappedSize));
                Logger::Debug("[FontProvider] Mapped font file: {} ({} bytes)", path, mappedSize);
                continue;
            }
            Logger::Warning("[FontProvider] mmap failed, falling back to fread: {}", path);
            std::vector<unsigned char> buffer;
            if (!LoadFontFileFallback(path, buffer)) {
                Logger::Error("[FontProvider] Failed to load font file: {}", path);
                continue;
            }
            cache.dataSizes.push_back(static_cast<int>(buffer.size()));
            cache.ownedBuffers.push_back(std::move(buffer));
            cache.dataPtrs.push_back(cache.ownedBuffers.back().data());
        }
        cache.loaded = true;
    }
    return cache;
}

std::vector<const unsigned char*> GetCachedFontData() {
    return GetFontDataCache().dataPtrs;
}

std::vector<int> GetCachedFontDataSizes() {
    return GetFontDataCache().dataSizes;
}

// ImGui 字体 atlas：实际传入的是像素字号 px
// Retina 屏在默认缩放模式下，1 pt = 2 px
float GetTextLayerFontSize() {
    return 23.8f;
}

float GetTopTooltipFontSize() {
    return 21.0f;
}

float GetTooltipFontSize() {
    return 16.8f;
}

// 这里是给 NSView 原生控件使用，返回的单位是 pt
float GetOcrFontSize() {
    return 13.0f;
}

} // namespace shotlet
