#pragma once
#include "i18n/Localization.h"

#include "PlatformMainContext.h"
#include <string_view>

namespace shotlet {

// 独立入口专用：后台模式持有单实例锁，重新启动脱离终端的子进程。
// --foreground 保留前台运行，但仍限制后台实例；普通模式和 Node addon 不受影响。
// instanceName 用于隔离进程测试，生产入口使用默认名称。
int RunWithDaemonProcess(PlatformEntryPoint entryPoint, int argc, char** argv,
                         std::string_view instanceName = "shotlet",
                         Language language = Language::English);

} // namespace shotlet
