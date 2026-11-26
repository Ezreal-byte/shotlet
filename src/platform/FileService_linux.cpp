#include <SDL3/SDL.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include "FileService.h"
#include "ImageOutput_linux.h"
#include "utils/Logger.h"

namespace shotlet {
    namespace {
        struct DialogResult {
            std::mutex mutex;
            std::condition_variable changed;
            bool finished = false;
            std::string path, error;
        };
        void SDLCALL DialogCompleted(void *userdata, const char *const *files, int) {
            auto &result = *static_cast<DialogResult *>(userdata);
            std::lock_guard lock(result.mutex);
            if (!files) {
                result.error = SDL_GetError();
                if (result.error.empty())
                    result.error = "The native file chooser returned an error";
            } else if (*files)
                result.path = *files;
            result.finished = true;
            result.changed.notify_one();
        }
        std::string ChoosePath(SDL_FileDialogType type, Language language, TextId title) {
            DialogResult result;
            const SDL_DialogFileFilter filters[] = {{Tr(language, TextId::PngType), "png"}};
            const auto props = SDL_CreateProperties();
            if (!props) {
                Logger::Error("File dialog properties: {}", SDL_GetError());
                return {};
            }
            SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, Tr(language, title));
            // Zenity 4 file choosers reject custom button labels. Native labels follow
            // the desktop locale; title and file filters still use Shotlet's language.
            if (type == SDL_FILEDIALOG_SAVEFILE) {
                SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, "Screenshot.png");
                SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER,
                                       const_cast<SDL_DialogFileFilter *>(filters));
                SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 1);
            }
            SDL_ShowFileDialogWithProperties(type, DialogCompleted, &result, props);
            SDL_DestroyProperties(props);
            for (;;) {
                // Portal replies need SDL's D-Bus event processing on the main thread.
                SDL_PumpEvents();
                std::unique_lock lock(result.mutex);
                if (result.changed.wait_for(lock, std::chrono::milliseconds(10), [&] { return result.finished; }))
                    break;
            }
            if (!result.error.empty()) {
                Logger::Error("Linux file dialog failed: {}", result.error);
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, Tr(language, title), result.error.c_str(), nullptr);
                throw std::runtime_error(result.error);
            }
            return result.path;
        }
    } // namespace

    void ActivateAppForSaveDialog() {}
    std::filesystem::path ChooseExportDirectory(Language language, TextId title) {
        return ChoosePath(SDL_FILEDIALOG_OPENFOLDER, language, title);
    }
    bool OpenFileWithDefaultApplication(const std::filesystem::path &file) {
        if (file.empty())
            return false;
        std::error_code error;
        const auto path = std::filesystem::absolute(file, error);
        if (error)
            return false;
        // OpenURL handles launching the desktop opener; never interpolate shell commands.
        std::string url = "file://";
        constexpr char hex[] = "0123456789ABCDEF";
        for (unsigned char c: path.string()) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '-' ||
                c == '_' || c == '.' || c == '~')
                url += char(c);
            else {
                url += '%';
                url += hex[c >> 4];
                url += hex[c & 15];
            }
        }
        return SDL_OpenURL(url.c_str());
    }
    bool RevealInFileManager(const std::filesystem::path &file) {
        return !file.empty() && OpenFileWithDefaultApplication(file.parent_path().empty() ? "." : file.parent_path());
    }
    CaptureSourceInfo GetForegroundCaptureSource() { return {}; }

    std::string SaveFileWithDialog(const unsigned char *rgba, int width, int height, Language language) {
        if (!rgba || width <= 0 || height <= 0) {
            Logger::Error("Invalid screenshot pixels");
            return {};
        }
        auto path = ChoosePath(SDL_FILEDIALOG_SAVEFILE, language, TextId::SaveTitle);
        if (path.empty())
            return {};
        // Keep the exact selected path so the chooser's overwrite confirmation remains valid.
        std::string error;
        if (SaveLinuxPng(rgba, width, height, path, error))
            return path;
        Logger::Error("Failed to save screenshot: {}", error);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, Tr(language, TextId::SaveTitle), error.c_str(), nullptr);
        throw std::runtime_error(error);
    }
} // namespace shotlet
