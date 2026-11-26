#include "ImageOutput_linux.h"
#include <SDL3/SDL.h>
#include <cstring>
#include <fstream>
#include "core/CaptureDocument.h"
#include "utils/AtomicFile.h"

namespace shotlet {
    bool SaveLinuxPng(const unsigned char *rgba, int width, int height, const std::filesystem::path &path,
                      std::string &error) {
        error.clear();
        if (!rgba || !DocumentPixelBytes(width, height) || width > 65535 || height > 65535) {
            error = "Invalid PNG pixels or dimensions";
            return false;
        }
        AtomicFile output;
        if (!output.Prepare(path, error))
            return false;
        auto *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, const_cast<unsigned char *>(rgba),
                                              width * 4);
        if (!surface) {
            error = SDL_GetError();
            return false;
        }
        const bool encoded = SDL_SavePNG(surface, output.Path().c_str());
        SDL_DestroySurface(surface);
        if (!encoded) {
            error = SDL_GetError();
            return false;
        }
        // Also reject a short write: preserve the destination on disk-full failures.
        std::ifstream input(output.Path(), std::ios::binary);
        unsigned char tail[12]{};
        if (input) {
            input.seekg(-12, std::ios::end);
            input.read(reinterpret_cast<char *>(tail), sizeof(tail));
        }
        constexpr unsigned char end[] = {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xae, 0x42, 0x60, 0x82};
        if (!input || std::memcmp(tail, end, sizeof(end))) {
            error = "Incomplete PNG output";
            return false;
        }
        input.close();
        return output.Commit(error);
    }
} // namespace shotlet
