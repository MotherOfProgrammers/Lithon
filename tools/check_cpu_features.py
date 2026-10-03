#!/usr/bin/env python3
"""1.2  Verify runtime CPUID detection against the kernel's own view.

`cpu_features_test --dump` prints the flags Lithon decoded from CPUID/XGETBV;
this parses /proc/cpuinfo (and lscpu, when present) and requires the two to
agree bit for bit. A mismatch means either the CPUID bit mapping in
src/jit/cpu_features.h is wrong or the OS-enablement gate is.

The synthetic-generation test in cpu_features_test covers CPUs this host is
not (Nehalem / Ivy Bridge / Haswell / Skylake-X); this script is the
"does the running host actually report what the kernel says" half. Between
the two, the mapping is checked against more than one CPU generation even
when only one physical host is available.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DUMP = ROOT / "build" / "cpu_features_test"

# Lithon dump key -> the names the kernel may use in /proc/cpuinfo or lscpu.
FLAG_ALIASES = {
    "sse2":     ["sse2"],
    "sse3":     ["pni", "sse3"],
    "ssse3":    ["ssse3"],
    "sse41":    ["sse4_1"],
    "sse42":    ["sse4_2"],
    "avx":      ["avx"],
    "fma":      ["fma"],
    "avx2":     ["avx2"],
    "avx512f":  ["avx512f"],
    "avx512dq": ["avx512dq"],
    "avx512cd": ["avx512cd"],
    "avx512bw": ["avx512bw"],
    "avx512vl": ["avx512vl"],
}


def kernel_flags():
    text = pathlib.Path("/proc/cpuinfo").read_text()
    flags = set()
    vendor = ""
    for line in text.splitlines():
        if line.startswith("vendor_id") and not vendor:
            vendor = line.split(":", 1)[1].strip()
        if line.startswith("flags"):
            flags.update(line.split(":", 1)[1].split())
    return vendor, flags


def lscpu_flags():
    try:
        out = subprocess.run(["lscpu"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    for line in out.splitlines():
        if line.startswith("Flags:"):
            return set(line.split(":", 1)[1].split())
    return None


def main():
    if not DUMP.exists():
        print(f"error: {DUMP} not built -- run: cmake --build build", file=sys.stderr)
        return 2

    dump = {}
    for line in subprocess.run([str(DUMP), "--dump"], capture_output=True,
                               text=True, check=True).stdout.splitlines():
        k, _, v = line.partition("=")
        dump[k] = v

    vendor, flags = kernel_flags()
    if dump.get("vendor") != vendor:
        print(f"MISMATCH vendor: lithon={dump.get('vendor')!r} cpuinfo={vendor!r}")
        return 1

    checked = 0
    problems = []
    for key, names in FLAG_ALIASES.items():
        if key not in dump:
            problems.append(f"{key}: missing from --dump output")
            continue
        mine = dump[key] == "1"
        # If the kernel knows the flag at all, use it; if the kernel exposes
        # none of the aliases (older kernel, masked feature), treat as absent.
        theirs = any(n in flags for n in names)
        checked += 1
        if mine != theirs:
            problems.append(f"{key}: lithon={int(mine)} kernel={int(theirs)} "
                            f"(names checked: {names})")

    # lscpu is the second independent kernel view, when the tool exists.
    lflags = lscpu_flags()
    if lflags is not None:
        for key, names in FLAG_ALIASES.items():
            if key not in dump:
                continue
            mine = dump[key] == "1"
            theirs = any(n in lflags for n in names)
            if mine != theirs:
                problems.append(f"{key}: lithon={int(mine)} lscpu={int(theirs)}")

    if problems:
        print(f"{len(problems)} FEATURE MISMATCH(ES):")
        for p in problems:
            print(" ", p)
        return 1
    print(f"{checked} features match /proc/cpuinfo"
          + (" and lscpu" if lflags is not None else "")
          + f" on {vendor} (raw CPUID bits)")
    print(f"  usable_avx={dump.get('usable_avx')} usable_avx512={dump.get('usable_avx512')} "
          f"simd_level={dump.get('simd_level')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
