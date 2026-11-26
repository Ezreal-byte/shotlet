"""Reject the wrong CPU architecture or a glibc/libstdc++ dependency above the release baseline."""
import argparse
from pathlib import Path
import re
import subprocess


def version(value):
    return tuple(map(int, value.split(".")))


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("x86_64", "aarch64"), required=True)
    parser.add_argument("--max-glibc", default="2.28")
    parser.add_argument("--static-cxx", action="store_true")
    parser.add_argument("binaries", type=Path, nargs="+")
    args = parser.parse_args()
    for binary in args.binaries:
        with binary.open("rb") as stream:
            header = stream.read(20)
        require(len(header) == 20 and header[:6] == b"\x7fELF\x02\x01", f"{binary}: expected little-endian ELF64")
        machine = int.from_bytes(header[18:20], "little")
        require(machine == {"x86_64": 62, "aarch64": 183}[args.arch], f"{binary}: wrong ELF architecture {machine}")
        text = subprocess.check_output(["readelf", "--version-info", "--wide", str(binary)], text=True)
        require("GLIBC_ABI_" not in text and "GLIBC_PRIVATE" not in text, f"{binary}: unexpected glibc ABI extension")
        versions = set(re.findall(r"\bGLIBC_(\d+(?:\.\d+)+)\b", text))
        require(versions, f"{binary}: no glibc symbol requirements found")
        newest = max(versions, key=version)
        require(version(newest) <= version(args.max_glibc), f"{binary}: GLIBC_{newest} exceeds {args.max_glibc}")
        dynamic = subprocess.check_output(["readelf", "--dynamic", "--wide", str(binary)], text=True)
        if args.static_cxx:
            require(not re.search(r"\(NEEDED\).*\[lib(?:stdc\+\+|c\+\+|c\+\+abi)\.so", dynamic),
                    f"{binary}: dynamic C++ runtime")
            require("GLIBCXX_" not in text, f"{binary}: dynamic libstdc++ symbols")
        print(f"PASS {binary}: {args.arch}, glibc <= {newest}")


if __name__ == "__main__":
    main()
