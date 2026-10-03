"""`bench-vid-maker` command line: bench-vid-maker <config.toml> [--save PATH | --no-save]"""
from __future__ import annotations

import argparse
import dataclasses
import json
import os
import pathlib
import sys
import time

from rich.console import Console

from .config import PROJECT_ROOT, load_config
from .recorder import VideoRecorder
from .ui import estimate_frame_height, run_with_ui


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="bench-vid-maker",
        description="Run a set of benchmark tasks defined in a TOML config, with a clean, "
        "accurately-timed live UI suited for screen recording.",
    )
    ap.add_argument("config", type=pathlib.Path, help="TOML file describing the tasks to compare")
    ap.add_argument(
        "--save", type=pathlib.Path, default=None, help="write results as JSON to this path"
    )
    ap.add_argument("--no-save", action="store_true", help="do not write a results JSON file")
    ap.add_argument(
        "--video", type=pathlib.Path, default=None,
        help="also render an mp4 of the run to this path (default: results/<config-stem>_<timestamp>.mp4 "
        "when --video is passed with no path)",
    )
    ap.add_argument("--video-only", action="store_true", help="render --video without opening a live terminal UI")
    ap.add_argument("--fps", type=int, default=20, help="video frame rate (default: 20)")
    ap.add_argument("--video-cols", type=int, default=150, help="video width in character cells")
    ap.add_argument(
        "--video-rows", type=int, default=None,
        help="video height in character cells (default: auto-fit to content)",
    )
    args = ap.parse_args(argv)

    try:
        cfg = load_config(args.config)
    except (FileNotFoundError, ValueError) as e:
        print(f"bench-vid-maker: {e}", file=sys.stderr)
        return 2

    recorder = None
    if args.video is not None or args.video_only:
        video_path = args.video
        if video_path is None:
            stamp = time.strftime("%Y%m%d-%H%M%S")
            video_path = PROJECT_ROOT / "results" / f"{cfg.path.stem}_{stamp}.mp4"
        video_rows = args.video_rows or estimate_frame_height(cfg, width_chars=args.video_cols)
        try:
            recorder = VideoRecorder(
                video_path, width_chars=args.video_cols, height_chars=video_rows, fps=args.fps
            )
        except FileNotFoundError as e:
            print(f"bench-vid-maker: {e}", file=sys.stderr)
            return 2
    else:
        video_rows = args.video_rows or 42

    console = (
        Console(file=open(os.devnull, "w"), width=args.video_cols, height=video_rows)
        if args.video_only
        else None
    )
    results = run_with_ui(cfg, console=console, recorder=recorder)

    if recorder is not None:
        recorder.close()
        print(f"wrote {recorder.out_path}")

    if not args.no_save:
        out = args.save
        if out is None:
            stamp = time.strftime("%Y%m%d-%H%M%S")
            out = PROJECT_ROOT / "results" / f"{cfg.path.stem}_{stamp}.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(
            json.dumps(
                {
                    "title": cfg.title,
                    "config": str(cfg.path),
                    "results": [dataclasses.asdict(r) for r in results],
                },
                indent=2,
            )
        )
        print(f"wrote {out}")

    return 0 if all(r.ok for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
