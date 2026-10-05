#!/usr/bin/env python3
"""Writes benchmark results into the tests they came from.

Each _test.cc with benchmarks has a "Benchmarks" banner. This puts, at the
end of the comment under that banner, the output of the last run of that
file's benchmarks, with the date and the machine, replacing whatever results
were there before. Run it after perf/benchmarks.sh, with the same label:

    perf/benchmarks.sh recorded
    perf/record_results.py recorded
"""
import datetime
import glob
import os
import platform
import re
import subprocess
import sys

BEGIN = "// Results. Written by perf/record_results.py; do not edit by hand."
END = "// End of results."


def first_line(command):
    try:
        return subprocess.run(command, shell=True, capture_output=True,
                              text=True).stdout.strip().split("\n")[0]
    except OSError:
        return ""


def machine():
    """What the numbers were measured on, as comment lines."""
    cpu = first_line("lscpu | sed -n 's/^Model name: *//p'")
    cores = first_line("nproc")
    cache = first_line("lscpu | sed -n 's/^L3 cache: *//p'")
    memory_kb = int(first_line("sed -n 's/^MemTotal: *\\([0-9]*\\).*/\\1/p' /proc/meminfo") or 0)
    disk = first_line("lsblk -d -n -o MODEL $(findmnt -n -o SOURCE / | sed 's/p[0-9]*$//')")
    filesystem = first_line("findmnt -n -o FSTYPE /")
    governor = first_line("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
    compiler = first_line("clang --version")
    return [
        f"//   Date      {datetime.date.today().isoformat()}",
        f"//   CPU       {cpu}, {cores} cores, L3 {cache}",
        f"//   Memory    {round(memory_kb / 1024 / 1024)} GB",
        f"//   Disk      {disk} ({filesystem})",
        f"//   System    {platform.system()} {platform.release()}, CPU governor: {governor}",
        f"//   Compiler  {compiler}",
        "//   Build     bazel -c opt (-O2), C++20, no exceptions",
        "//   I/O       io_uring, except where a benchmark's name says epoll",
    ]


def record(test_file, results, about):
    lines = open(test_file).read().split("\n")

    # Take out the results of an earlier run, and the blank comment line
    # that led up to them.
    if BEGIN in lines:
        begin, end = lines.index(BEGIN), lines.index(END)
        while lines[begin - 1] in ("//", "// clang-format off"):
            begin -= 1
        while lines[end + 1] == "// clang-format on":
            end += 1
        del lines[begin:end + 1]

    # The banner's comment runs from the banner to the first line that is
    # not a comment.
    banner = next(i for i, line in enumerate(lines) if line == "// Benchmarks")
    after = next(i for i in range(banner, len(lines))
                 if not lines[i].startswith("//"))

    # Formatting is off for the table, whose lines are wider than the rest
    # of the file and would be wrapped.
    block = (["//", "// clang-format off", BEGIN, "//"] + about + ["//"] +
             ["//   " + line.rstrip() for line in results] +
             [END, "// clang-format on"])
    lines[after:after] = block
    open(test_file, "w").write("\n".join(lines))


def main():
    label = sys.argv[1]
    about = machine()
    for path in sorted(glob.glob(f"perf-reports/raw/{label}/*_test.txt")):
        results = [line for line in open(path).read().split("\n") if line]
        if not any(line.startswith("BM_") for line in results):
            continue

        # "gm_ledger_test.txt" holds the results of gm/ledger_test.cc.
        package, _, name = os.path.basename(path)[:-len(".txt")].partition("_")
        record(f"{package}/{name}.cc", results, about)
        print(f"{package}/{name}.cc")


main()
