"""macOS daemon 内存采样：记录 RSS 趋势、日志阶段和 vmmap 分类，不操作截图界面。"""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import time


def run(*command):
    return subprocess.run(command, capture_output=True, text=True, check=True, timeout=30).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--log-file", type=Path)
    parser.add_argument("--duration", type=float, default=0, help="观察秒数，0 表示单次采样")
    parser.add_argument("--interval", type=float, default=1)
    args = parser.parse_args()
    if sys.platform != "darwin" or args.pid <= 0 or args.duration < 0 or args.interval <= 0:
        parser.error("需要 macOS、有效 PID 和非负时长；采样间隔必须大于零")
    # 每次运行使用新目录，防止不同进程的基准结果混在一起。
    args.output.mkdir(parents=True, exist_ok=False)
    metadata = {
        "pid": args.pid,
        "macos": run("sw_vers", "-productVersion").strip(),
        "hardware": run("sysctl", "-n", "hw.model", "machdep.cpu.brand_string", "hw.memsize").splitlines(),
        "process": run("ps", "-p", str(args.pid), "-o", "pid=,ppid=,command=").strip(),
        "units": "RSS 使用 KiB；vmmap 保留工具的原始单位。peak_rss_kib 为本次观察的采样峰值。",
    }
    (args.output / "metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2))
    sequence = 0
    start = time.monotonic()
    peak_rss = 0
    pending_snapshots = []
    log = args.log_file.open(encoding="utf-8") if args.log_file else None
    if log:
        log.seek(0, 2)

    def snapshot(phase):
        nonlocal sequence
        raw = run("vmmap", "-summary", str(args.pid))
        sequence += 1
        filename = f"{sequence:03d}-{phase}.vmmap.txt"
        (args.output / filename).write_text(raw)
        footprint = re.search(r"^Physical footprint:\s+(\S+)", raw, re.MULTILINE)
        peak = re.search(r"^Physical footprint \(peak\):\s+(\S+)", raw, re.MULTILINE)
        return {"phase": phase, "vmmap": filename,
                "footprint": footprint.group(1) if footprint else None,
                "process_peak_footprint": peak.group(1) if peak else None}

    try:
        with (args.output / "samples.jsonl").open("w", encoding="utf-8") as samples:
            initial = snapshot("baseline")
            while True:
                now = time.monotonic()
                rss = int(run("ps", "-p", str(args.pid), "-o", "rss=").strip())
                peak_rss = max(peak_rss, rss)
                sample = {"elapsed_seconds": round(now - start, 3), "rss_kib": rss,
                          "peak_rss_kib": peak_rss, "events": []}
                if initial:
                    sample["snapshots"] = [initial]
                    initial = None
                if log:
                    for line in log.readlines():
                        if not re.search(rf"\bpid={args.pid}\b", line):
                            continue
                        if "event=" in line:
                            sample["events"].append(line.strip())
                        if "event=capture.selection_finished" in line:
                            pending_snapshots.append((now, "capture-finished"))
                        elif "event=daemon.overlays_reclaimed" in line:
                            # 等当前主循环的 pool 排空，再观察窗口回收后的稳定值。
                            pending_snapshots.append((now + 5, "reclaimed-plus-5s"))
                ready = [item for item in pending_snapshots if item[0] <= now]
                pending_snapshots[:] = [item for item in pending_snapshots if item[0] > now]
                for _, phase in ready:
                    sample.setdefault("snapshots", []).append(snapshot(phase))
                samples.write(json.dumps(sample, ensure_ascii=False) + "\n")
                samples.flush()
                if now - start >= args.duration:
                    break
                time.sleep(min(args.interval, max(0, args.duration - (time.monotonic() - start))))
    except (subprocess.SubprocessError, ValueError) as error:
        raise SystemExit(f"采样停止：{error}；已采集数据保留在 {args.output}") from error
    finally:
        if log:
            log.close()
    print(f"采样完成：{args.output}；观察期间 RSS 峰值 {peak_rss / 1024:.1f} MiB")


if __name__ == "__main__":
    main()
