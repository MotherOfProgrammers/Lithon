"""`bench-vid-maker-all`: build a video (+ results JSON) for every config
under configs/, one at a time.

Sequential on purpose -- these are CPU-bound timing comparisons, so running
two at once would make every number (wall time, CPU%) meaningless.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

from .config import PROJECT_ROOT


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="bench-vid-maker-all",
        description="Run `bench-vid-maker --video-only` against every *.toml config in a directory, sequentially.",
    )
    ap.add_argument(
        "configs_dir", type=pathlib.Path, nargs="?", default=PROJECT_ROOT / "configs",
        help="directory of *.toml configs to build (default: configs/)",
    )
    ap.add_argument(
        "--out-dir", type=pathlib.Path, default=PROJECT_ROOT / "results",
        help="where each config's <stem>.mp4 / <stem>.json is written (default: results/)",
    )
    ap.add_argument(
        "extra", nargs=argparse.REMAINDER,
        help="extra args forwarded to every bench-vid-maker invocation, e.g. -- --no-music --fps 30",
    )
    args = ap.parse_args(argv)

    configs = sorted(args.configs_dir.glob("*.toml"))
    if not configs:
        print(f"bench-vid-maker-all: no *.toml configs found under {args.configs_dir}", file=sys.stderr)
        return 2

    args.out_dir.mkdir(parents=True, exist_ok=True)
    failures: list[str] = []
    for i, cfg_path in enumerate(configs, 1):
        stem = cfg_path.stem
        video = args.out_dir / f"{stem}.mp4"
        save = args.out_dir / f"{stem}.json"
        print(f"=== [{i}/{len(configs)}] building {stem} ===", flush=True)
        cmd = [
            sys.executable, "-m", "bench_vid_maker.cli", str(cfg_path),
            "--video-only", "--video", str(video), "--save", str(save),
            *args.extra,
        ]
        result = subprocess.run(cmd)
        if result.returncode != 0:
            failures.append(stem)
            print(f"bench-vid-maker-all: {stem} failed (exit {result.returncode})", file=sys.stderr)

    if failures:
        print(f"bench-vid-maker-all: {len(failures)}/{len(configs)} failed: {', '.join(failures)}", file=sys.stderr)
        return 1
    print(f"bench-vid-maker-all: built {len(configs)} video(s) in {args.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
