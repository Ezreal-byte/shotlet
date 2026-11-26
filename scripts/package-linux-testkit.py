"""Bundle one or both Linux TGZ packages with the Python-free desktop test script."""
import argparse
import hashlib
import io
from pathlib import Path
import re
import tarfile


README = """Shotlet Linux 基础截图测试包

在新的独立目录解压，进入 shotlet-linux-testkit 后执行：
  bash test-shotlet.sh
脚本自动选取本机 x86_64 或 ARM64 安装包；本包只有一个架构时须在对应机器运行。
没有可用 Vulkan 设备时自动回退 CPU，也可执行：
  bash test-shotlet.sh --software

请拉选区、添加矩形／箭头和中文，然后点击完成、保存或 Ctrl+S。
按终端提示保存 Screenshot.png，再打开图片检查底图、标注和中文。
脚本检查 PNG 结构；安装 pngcheck 时额外检查 CRC 和图像数据。
日志、设置和截图保留在 /tmp/shotlet-test.*，请在重启前复制需要的结果。

只预检：bash test-shotlet.sh --check-only
仅校验、解压和打印程序路径：bash test-shotlet.sh --prepare-only
程序放在脚本旁的固定目录，重跑会校验并复用原文件。
若 KYSEC 拒绝执行，请按单位策略由管理员仅授权打印的程序文件后重跑。
脚本不会关闭或修改安全策略。

需要 X11、X11／XRandR／Xinerama／Fontconfig 库、中文字体及 Zenity。
兼容构建以 glibc 2.28 为基线；普通原生编译产物以 ldd 报告的实际依赖为准。
测试脚本只需要 Bash 和常见系统命令，不调用 Python，不修改 HOME，不上传内容。
软件回退覆盖基础截图、标注和 PNG 保存，工具栏图标使用采样位图缓存。
OCR、贴图、Wayland、录屏、长截图和全局热键未在本阶段开放；其他附加窗口仍依赖 GPU。
退出码：0=准备／预检／保存检查通过，1=失败，2=取消，3=执行被系统拒绝。
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packages", type=Path, nargs="+", required=True,
                        help="One TGZ per architecture, produced by CPack")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    versions, architectures = set(), set()
    contents = {"test-shotlet.sh": Path(__file__).with_name("test-linux-tarball.sh").read_bytes(),
                "README.txt": README.encode("utf-8")}
    for package in args.packages:
        match = re.fullmatch(r"shotlet-([0-9]+(?:\.[0-9]+)+)-linux-(amd64|arm64)\.tar\.gz", package.name)
        if not match or not package.is_file():
            parser.error(f"Not a Shotlet Linux TGZ: {package}")
        version, architecture = match.groups()
        if architecture in architectures:
            parser.error(f"Multiple packages for {architecture}")
        if args.output.resolve() == package.resolve():
            parser.error("Output must differ from input packages")
        versions.add(version)
        architectures.add(architecture)
        contents[package.name] = package.read_bytes()
    if len(versions) != 1:
        parser.error("All packages must have the same version")
    contents["SHA256SUMS"] = "".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in contents.items()
    ).encode("utf-8")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Only the explicit files above are included; no checkout, settings or test logs.
    with tarfile.open(args.output, "w:gz") as archive:
        directory = tarfile.TarInfo("shotlet-linux-testkit")
        directory.type = tarfile.DIRTYPE
        directory.mode = 0o755
        archive.addfile(directory)
        for name, data in contents.items():
            info = tarfile.TarInfo(f"shotlet-linux-testkit/{name}")
            info.size = len(data)
            info.mode = 0o755 if name.endswith(".sh") else 0o644
            archive.addfile(info, io.BytesIO(data))
    print(f"{hashlib.sha256(args.output.read_bytes()).hexdigest()}  {args.output}")


if __name__ == "__main__":
    main()
