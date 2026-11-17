# 第三方依赖说明

Shotlet 使用以下第三方软件。发布包含这些组件的二进制文件时，应一并提供相应许可证文本。

| 组件 | 版本 | 许可证 | 许可证文本 |
|------|------|--------|------------|
| SDL | 3.4.18 | zlib License | [SDL LICENSE](https://github.com/libsdl-org/SDL/blob/release-3.4.18/LICENSE.txt) |
| Dear ImGui | `e05bd9deb9a73b67acb5c04b8a581e06a2d42657`（1.92.7 WIP） | MIT License | [ImGui LICENSE](https://github.com/ocornut/imgui/blob/e05bd9deb9a73b67acb5c04b8a581e06a2d42657/LICENSE.txt) |
| nlohmann/json | 3.11.3 | MIT License | [仓库中的许可证](src/third_party/nlohmann/LICENSE.MIT) |

SDL 和 Dear ImGui 在本项目工作区外按 [README 构建说明](README.md#build) 获取；表中版本用于复现当前构建。nlohmann/json 的单头文件源码和许可证随仓库提供。
