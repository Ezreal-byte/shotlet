#!/usr/bin/env python3
"""由 Shotlet 构建并导出 C SDK 与独立截图运行时；宿主只消费预编译产物。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
FORMAT_VERSION = 1
PROTOCOL_VERSION = 2


def architecture():
    value = platform.machine().lower()
    return {"aarch64": "arm64", "x86_64": "amd64", "x64": "amd64"}.get(value, value)


def export_package(build, destination, config):
    metadata = json.loads((build / f"capture-sdk-{config}.json").read_text(encoding="utf-8"))
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="capture-sdk-", dir=destination.parent) as temporary:
        package = Path(temporary) / "package"
        (package / "lib").mkdir(parents=True)
        (package / "include" / "shotlet").mkdir(parents=True)
        shutil.copy2(ROOT / "include/shotlet/capture.h", package / "include/shotlet/capture.h")
        libraries = {}
        for name in ("sdk", "core", "sdl"):
            source = Path(metadata[name])
            shutil.copy2(source, package / "lib" / source.name)
            libraries[name] = source.name
        runtime = package / "runtime"
        runtime.mkdir()
        binary = Path(metadata["helper"])
        if sys.platform == "darwin":
            bundle = binary.parents[2]
            shutil.copytree(bundle, runtime / bundle.name)
            helper = f"runtime/{bundle.name}"
            executable = f"{helper}/Contents/MacOS/{binary.name}"
        else:
            shutil.copy2(binary, runtime / binary.name)
            resources = binary.parent / "resources"
            if resources.is_dir():
                shutil.copytree(resources, runtime / "resources")
            helper = executable = f"runtime/{binary.name}"
        cmake = package / "lib/cmake/ShotletCapture"
        cmake.mkdir(parents=True)
        contents = (ROOT / "cmake/sdk/ShotletCaptureConfig.cmake.in").read_text(encoding="utf-8")
        for name, filename in libraries.items():
            contents = contents.replace(f"@{name.upper()}_LIBRARY@", filename)
        (cmake / "ShotletCaptureConfig.cmake").write_text(contents, encoding="utf-8")
        shutil.copy2(ROOT / "docs/capture-sdk.md", package / "README.md")
        licenses = package / "licenses"
        licenses.mkdir()
        for source, name in ((ROOT / "sdl3/LICENSE.txt", "SDL.txt"),
                             (ROOT / "imgui/LICENSE.txt", "ImGui.txt"),
                             (ROOT / "src/third_party/nlohmann/LICENSE.MIT", "nlohmann.txt")):
            shutil.copy2(source, licenses / name)
        files = {path.relative_to(package).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in sorted(package.rglob("*")) if path.is_file()}
        manifest = {"format_version": FORMAT_VERSION, "protocol_version": PROTOCOL_VERSION,
                    "abi_version": 2, "capabilities": ["capture", "permissions-v1", "host-permissions-v1", "actions-v1"], "version": metadata["version"], "platform": sys.platform,
                    "architecture": architecture(), "minimum_macos": metadata["minimum_macos"],
                    "helper": helper, "executable": executable, "files": files}
        (package / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        # 先完整生成产物，再替换旧包；失败时保留原有可用 SDK。
        backup = Path(temporary) / "previous"
        if destination.exists():
            if not (destination / "manifest.json").is_file():
                raise RuntimeError("目标目录不是已有截图 SDK，拒绝覆盖")
            destination.rename(backup)
        try:
            package.rename(destination)
        except BaseException:
            if backup.exists():
                backup.rename(destination)
            raise
        if backup.exists():
            shutil.rmtree(backup)
    print(f"截图 SDK 已导出：{destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/capture-sdk")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--config", default="Release", choices=("Release", "MinSizeRel", "Debug", "RelWithDebInfo"))
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    args = parser.parse_args()
    if sys.platform not in ("darwin", "win32"):
        raise RuntimeError("截图 SDK 导出目前支持 macOS 与 Windows")
    build = args.build_dir.resolve()
    output = args.output or ROOT / "dist/capture-sdk" / f"{sys.platform}-{architecture()}"
    subprocess.run(["cmake", "-S", str(ROOT), "-B", str(build), f"-DCMAKE_BUILD_TYPE={args.config}",
                    "-DSHOTLET_BUILD_CAPTURE_SDK=ON", "-DSHOTLET_BUILD_NODE_ADDON=OFF",
                    "-DSHOTLET_BUILD_TESTS=OFF"], check=True)
    subprocess.run(["cmake", "--build", str(build), "--config", args.config, "--target",
                    "shotlet_capture", "capture_sdk_check", "--parallel", str(args.jobs)], check=True)
    export_package(build, output.resolve(), args.config)


if __name__ == "__main__":
    main()
