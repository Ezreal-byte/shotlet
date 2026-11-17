#include "HotkeyService.h"
#include "utils/Logger.h"

namespace shotlet {

bool HotkeyService::Register(int modifiers, int key, std::string& conflictMsg, Language language, Action) {
    Logger::Warning("[HotkeyService] Global hotkey not implemented for this platform");
    conflictMsg = Tr(language, TextId::HotkeyUnsupported);
    return false;
}

void HotkeyService::Unregister(Action) {
    Logger::Warning("[HotkeyService] Unregister: no hotkey registered on this platform");
}

bool HotkeyService::Poll(Action) {
    return false;
}

void HotkeyService::SetSuspended(bool) {}

} // namespace shotlet
