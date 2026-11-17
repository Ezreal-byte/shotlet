#include "history/HistoryStore.h"
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    try {
        shotlet::HistoryStore store(argv[1]);
        const int start = std::stoi(argv[2]), count = std::stoi(argv[3]);
        for (int i = 0; i < count; ++i) {
            shotlet::CaptureDocument document;
            document.width = 8; document.height = 6; document.logicalSize = {8, 6};
            document.createdAtUnixMs = start + i;
            document.originalRgba.assign(8 * 6 * 4, 255);
            auto pixels = std::shared_ptr<const unsigned char>(document.originalRgba.data(), [](const unsigned char*) {});
            std::string error;
            const auto generation = store.RegisterCandidate(error);
            if (!generation || !store.Write(*generation, document, {pixels, 8, 6}, "copy", error)) {
                std::cerr << error << '\n'; return 1;
            }
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
