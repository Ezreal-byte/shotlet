#pragma once
#include "i18n/Localization.h"
#include <string>
#include <filesystem>

namespace shotlet {

// 激活应用前台状态，确保系统保存弹窗能获取键盘焦点
// macOS: 临时切换到 Regular 模式激活 NSApp
void ActivateAppForSaveDialog();
std::filesystem::path DefaultRecordingDirectory();
// 自动创建默认目录并生成唯一文件名；启动录制时调用，不显示保存对话框。
std::filesystem::path CreateRecordingSavePath(const std::filesystem::path& directory, const char* extension, std::string& error);
std::filesystem::path ChooseExportDirectory(Language language, TextId title = TextId::HistoryExport);
bool RevealInFileManager(const std::filesystem::path& file);
// 交由系统默认应用打开文件，路径通过原生 API 传递，避免命令行转义。
bool OpenFileWithDefaultApplication(const std::filesystem::path& file);
struct CaptureSourceInfo { std::string application, window; };
CaptureSourceInfo GetForegroundCaptureSource();

// 使用系统文件保存对话框保存 PNG 图片
// 返回保存的文件路径，失败返回空字符串
std::string SaveFileWithDialog(const unsigned char* rgbaData, int width, int height,
                               Language language = Language::English);

} // namespace shotlet
