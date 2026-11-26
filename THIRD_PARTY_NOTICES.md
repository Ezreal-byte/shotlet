# 第三方依赖说明

Shotlet 使用以下第三方软件。发布包含这些组件的二进制文件时，应一并提供相应许可证文本。

| 组件 | 版本 | 许可证 | 许可证文本 |
|------|------|--------|------------|
| SDL | 3.4.18 | zlib License | [SDL LICENSE](https://github.com/libsdl-org/SDL/blob/release-3.4.18/LICENSE.txt) |
| Dear ImGui | `e05bd9deb9a73b67acb5c04b8a581e06a2d42657`（1.92.7 WIP） | MIT License | [ImGui LICENSE](https://github.com/ocornut/imgui/blob/e05bd9deb9a73b67acb5c04b8a581e06a2d42657/LICENSE.txt) |
| nlohmann/json | 3.11.3 | MIT License | [仓库中的许可证](src/third_party/nlohmann/LICENSE.MIT) |
| libc++、libc++abi、libunwind（可选 Linux 兼容构建） | Zig 0.15.2 随附的 LLVM 运行库 | Apache-2.0 WITH LLVM-exception | Linux 安装包 `share/licenses/shotlet/LLVM-*-LICENSE.txt` |
| Zig 编译器运行库（可选 Linux 兼容构建） | 0.15.2 | MIT License | Linux 安装包 `share/licenses/shotlet/Zig-LICENSE.txt` |

SDL 和 Dear ImGui 在本项目工作区外按 [README 构建说明](README.md#build) 获取；表中版本用于复现当前构建。nlohmann/json 的单头文件源码和许可证随仓库提供。

SDL 源码通过 `cmake/patches` 应用本地补丁，包括静态构建的动态 API 裁剪、macOS 透明窗口行为，以及 Linux 软件渲染的 UV 步长修正。后者使 SDL 3.4.18 在 ImGui 分别存放颜色与纹理坐标时正确绘制图片及中文字形。发布的 SDL 组件包含这些修改，不是未经修改的上游二进制。
