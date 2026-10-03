# bench_vid_maker

A tiny, reusable uv project that turns a benchmark comparison (CPython vs
Lithon, or anything else) into an actual video file with a clean, honestly-
timed live UI.

Three acts, always in this order:

1. **Typing** -- every task's source types itself onto screen, side by
   side, at a fixed pace. Nothing is running yet; this is a pure intro and
   is never counted toward any time shown.
2. **Starting** -- a brief "starting..." popover flashes on every panel at
   once.
3. **Running** -- every task's process is launched at the *exact same
   instant*, under a big clock that starts clean at `00:00.00` right then.
   This is the only part that's actually timed. Each panel shows a live
   spinner, elapsed wall time, and CPU usage (instantaneous %, cumulative
   user/system seconds, sampled straight from the OS via `psutil`) until
   the process exits, then a done/failed badge.

A closing table + bar chart breaks down wall-clock vs. CPU user/system time
per task and is held on screen for a while.

Every number is real: `time.perf_counter()` around each subprocess (spawn
to exit) for wall time, `psutil` process accounting for CPU time, six
decimal places shown throughout so nothing is rounded away. The typing
intro never affects the clock -- it starts at zero exactly when the
processes launch.

## Run the included comparison

```
cd bench_vid_maker
uv run bench-vid-maker configs/fsum_1b.toml                      # live in the terminal
uv run bench-vid-maker configs/fsum_1b.toml --video out.mp4      # live AND recorded
uv run bench-vid-maker configs/fsum_1b.toml --video-only --video out.mp4   # recorded, no terminal UI
```

This types in `tasks/python_sum.py` (plain CPython) and
`test_codes_lithon/test_1b_fsum.py` (through the repo's built `lithon`
executable), then launches both **together**, on the same
1,000,000,000-iteration sum. There's also `configs/fsum_1k.toml`, a
1,000-iteration version that finishes in well under a second -- useful for
quickly checking the UI/video itself rather than waiting out a real
benchmark. Results are also saved as JSON under `results/` (use `--no-save`
to skip that).

First run: `uv sync` to create the project's own `.venv` (deps: `rich`,
`pillow`, `psutil`). Requires a `cmake --build build` in the repo root
first so `lithon` exists, and `ffmpeg` on `PATH` for `--video`.

### How the video is made

Each frame (the same `rich` renderable shown live in the terminal) is
rasterized to an RGB image with Pillow -- honoring every color, bold, and
panel border rich draws -- and streamed straight into `ffmpeg`'s stdin as
raw video; nothing but the final `.mp4` touches disk. A frame is held
(repeated) for exactly as many ticks as real wall-clock time actually
passed since the previous frame, so video playback time always matches the
real run, regardless of how long rasterizing/encoding itself took.

Text is rasterized with Windows' Consolas (falls back to JetBrains Mono,
then DejaVu Sans Mono on Linux). Only glyphs confirmed present in that font
are used anywhere in the UI (plain ASCII spinner `|/-\`, filled dot `●` for
status, block/line-drawing characters for bars) -- `rich` itself will
happily use glyphs the font doesn't have, which silently render as tofu
boxes in the video, so this was checked with `fontTools` against the
font's actual cmap rather than assumed.

Useful flags:

| flag | default | meaning |
|---|---|---|
| `--video PATH` | — | also/only render an mp4 |
| `--video-only` | off | skip the live terminal UI, just record |
| `--fps N` | 20 | video frame rate |
| `--video-cols N` | 150 | video width, in character cells |
| `--video-rows N` | auto | video height, in character cells (auto-fits the tallest frame so you don't get a mostly-black canvas) |

## Add a new comparison

1. Drop the program(s) you want to compare anywhere in the repo (or under
   `bench_vid_maker/tasks/`).
2. Write a config, e.g. `configs/my_comparison.toml`:

   ```toml
   title = "My comparison"
   subtitle = "optional subtitle"

   [[tasks]]
   name = "CPython"
   command = ["{python}", "bench_vid_maker/tasks/my_task.py"]

   [[tasks]]
   name = "Lithon"
   command = ["{lithon}", "test_codes_lithon/my_task.py"]
   # cwd = "."   # optional, relative to the repo root
   ```

   You aren't limited to two tasks -- add as many `[[tasks]]` entries as
   you like; they all type in together and then launch together, panels
   laid out side by side.

3. `uv run bench-vid-maker configs/my_comparison.toml --video out.mp4`

All command/`cwd` paths are resolved relative to the repo root (found by
walking up to the nearest `.git`), so configs stay short regardless of
where you invoke the tool from. `{python}` expands to the interpreter
running bench-vid-maker; `{lithon}` expands to the repo's built
`.venv/Scripts/lithon.exe` / `.venv/bin/lithon` (falling back to `PATH`).
Add more placeholders in `src/bench_vid_maker/config.py::_expand` as you
compare more engines.

The typewriter animation reads whichever file is the task's last command
argument; if that isn't a real file (e.g. `python -c "..."`), the panel
just shows the raw command line instead and skips straight to the
running/elapsed indicator.

For statistically rigorous (multi-run, median/min/max) numbers rather than
a single on-camera run, use `tools/bench.py` in the repo root instead.
