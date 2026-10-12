#!/usr/bin/env python3
"""Summarise the once-a-second performance lines of an xe.log.

Reads the lines written with log_gpu_frame_time_breakdown = true
("GpuFrame: ...") and log_shared_memory_uploads = true ("SharedMemUploads:
..."), optionally only between two log-relative second marks, and prints one
summary per run. Each GpuFrame line covers about one second.

    python3 tools/perf_log_summary.py xe.log
    python3 tools/perf_log_summary.py xe.log --from 60 --to 240 --csv out.csv
"""

import argparse
import csv
import re
import statistics
import sys

GPU_FRAME = re.compile(
    r"GpuFrame: (?P<frames>\d+) frames, interval avg=(?P<avg>[\d.]+)ms "
    r"max=(?P<max>[\d.]+)ms \| per frame: exec=(?P<exec>[\d.]+)ms "
    r"draws=(?P<draws>\d+) draw=(?P<draw>[\d.]+)ms swap=(?P<swap>[\d.]+)ms "
    r"stall=(?P<stall>[\d.]+)ms")
SHMEM = re.compile(
    r"SharedMemUploads: (?P<frames>\d+) frames in (?P<ms>\d+) ms, per frame: "
    r"(?P<up>[\d.]+) KB uploaded \((?P<refresh>[\d.]+) KB of it refresh-only\), "
    r"(?P<skipped>[\d.]+) KB refresh uploads skipped")


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return float("nan")
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log")
    parser.add_argument("--from", dest="start", type=int, default=0,
                        help="skip the first N GpuFrame samples (seconds)")
    parser.add_argument("--to", dest="end", type=int, default=None,
                        help="stop after GpuFrame sample N (seconds)")
    parser.add_argument("--target-fps", type=float, default=30.0)
    parser.add_argument("--csv", help="also write the per-second samples here")
    args = parser.parse_args()

    samples, shmem = [], []
    with open(args.log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = GPU_FRAME.search(line)
            if m:
                samples.append({k: float(v) for k, v in m.groupdict().items()})
                continue
            m = SHMEM.search(line)
            if m:
                shmem.append({k: float(v) for k, v in m.groupdict().items()})

    samples = samples[args.start:args.end]
    if not samples:
        sys.exit("no GpuFrame lines - was log_gpu_frame_time_breakdown = true?")

    fps = [s["frames"] for s in samples]
    worst_ms = [s["max"] for s in samples]
    target_ms = 1000.0 / args.target_fps
    print(f"seconds sampled          : {len(samples)}")
    print(f"fps avg / median / min   : {statistics.mean(fps):.1f} / "
          f"{statistics.median(fps):.1f} / {min(fps):.0f}")
    print(f"fps 5th percentile       : {percentile(fps, 0.05):.0f}")
    print(f"seconds below {args.target_fps:.0f} fps     : "
          f"{sum(1 for v in fps if v < args.target_fps - 0.5)} "
          f"({100.0 * sum(1 for v in fps if v < args.target_fps - 0.5) / len(fps):.0f}%)")
    print(f"frame interval avg       : {statistics.mean(s['avg'] for s in samples):.1f} ms")
    print(f"worst frame per second   : median {statistics.median(worst_ms):.1f} ms, "
          f"95th pct {percentile(worst_ms, 0.95):.1f} ms, max {max(worst_ms):.1f} ms")
    print(f"frames > 1.5x target     : seconds with a hitch "
          f"{sum(1 for v in worst_ms if v > 1.5 * target_ms)}")
    for key, label in (("exec", "PM4 exec"), ("draw", "draw processing"),
                       ("swap", "swap"), ("stall", "idle (waiting for guest)")):
        print(f"GPU thread {label:<26}: {statistics.mean(s[key] for s in samples):.2f} ms/frame")
    print(f"draws per frame          : {statistics.mean(s['draws'] for s in samples):.0f}")
    if shmem:
        print(f"shared memory uploads    : {statistics.mean(s['up'] for s in shmem):.0f} KB/frame, "
              f"refresh-only {statistics.mean(s['refresh'] for s in shmem):.0f} KB/frame, "
              f"skipped {statistics.mean(s['skipped'] for s in shmem):.0f} KB/frame")

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(samples[0].keys()))
            writer.writeheader()
            writer.writerows(samples)


if __name__ == "__main__":
    main()
