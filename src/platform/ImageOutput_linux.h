#pragma once
#include <filesystem>
#include <string>

namespace shotlet {
    // Write in the destination filesystem and replace only after a complete PNG exists.
    bool SaveLinuxPng(const unsigned char *rgba, int width, int height, const std::filesystem::path &path,
                      std::string &error);
} // namespace shotlet
