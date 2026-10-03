# bench_vid_maker

A tiny, reusable uv project that turns a benchmark comparison (CPython vs
Lithon, or anything else) into an actual video file with a clean, honestly-
timed live UI.

Three acts, always in this order:

1. **Typing** -- every task's source types itself onto screen, side by
   side, at a fixed pace. Nothing is running yet; this is a pure intro and
   is never counted toward any time shown.
2. **Starting** -- one single, large modal flashes centered over the
   (now fully typed) panels, and closes the instant the processes launch.
3. **Running** -- every task's process is launched at the *exact same
   instant*, under a big clock that starts clean at `00:00.00` right then.
   This is the only part that's actually timed. Each panel shows a live
   spinner, elapsed wall time, and CPU usage (instantaneous %, cumulative
   user/system seconds, sampled straight from the OS via `psutil`) until
   the process exits, then a done/failed badge and a tinted background.

Every panel is sized to fill the whole frame -- width and height -- so
nothing looks cramped in a sea of black. If the running phase takes longer
than ~10s of real time, the *recorded video* of that phase (never the live
terminal, never the numbers shown) is time-lapsed down to roughly that
length, and the applied speed-up (e.g. "173x") is reported on screen.

A closing, full-height summary breaks down wall-clock vs. CPU user/system
time per task (table), how that time was actually spent (a Chrome-
DevTools-Network-panel-style resource waterfall), and the environment the
run actually used (OS, CPU, cores, RAM, Python version, every resolved
command) -- and is held on screen for a while.

Every number is real: `time.perf_counter()` around each subprocess (spawn
to exit) for wall time, `psutil` process accounting for CPU time, six
decimal places shown throughout so nothing is rounded away. The typing
intro never affects the clock -- it starts at zero exactly when the
processes launch. CPU% is computed as cumulative CPU time over elapsed
wall time (not psutil's own interval `cpu_percent()`, which is noisy/
quantized on Windows over short polling windows).

## Run the included comparison

```
cd bench_vid_maker
uv run bench-vid-maker configs/fsum_1b.toml                      # live in the terminal
uv run bench-vid-maker configs/fsum_1b.toml --video out.mp4      # live AND recorded
uv run bench-vid-maker configs/fsum_1b.toml --video-only --video out.mp4   # recorded, no terminal UI
uv run bench-vid-maker configs/fsum_1b_4core.toml --video out.mp4         # same, pinned to 4 cores
```

This types in `tasks/python_sum.py` (plain CPython) and
`test_codes_lithon/test_1b_fsum.py` (through the repo's built `lithon`
executable), then launches both **together**, on the same
1,000,000,000-iteration sum. There's also `configs/fsum_1k.toml`, a
1,000-iteration version that finishes in well under a second -- useful for
quickly checking the UI/video itself rather than waiting out a real
benchmark, plus `configs/fsum_1b_{1,4,6,8}core.toml`, the same comparison
with every task's CPU affinity pinned to 1/4/6/8 cores (`cores = N` in the
config, or `--cores N` on the command line to override it for any config).
`configs/euler_lim.toml` compares a heavier, nested-loop workload instead
(approximating `e` via its limit definition out to i=1,000).
Results are also saved as JSON under `results/` (use `--no-save` to skip
that).

First run: `uv sync` to create the project's own `.venv` (deps: `rich`,
`pillow`, `psutil`). Requires a `cmake --build build` in the repo root
first so `lithon` exists, and `ffmpeg` on `PATH` for `--video`.

### How the video is made

Each frame (the same `rich` renderable shown live in the terminal) is
rasterized to an RGB image with Pillow -- honoring every color, bold, and
panel border rich draws -- and streamed straight into `ffmpeg`'s stdin as
raw video; nothing but the final `.mp4` touches disk. During the typing/
starting/summary acts, a frame is held (repeated) for exactly as many
ticks as real wall-clock time actually passed since the previous frame, so
video playback time matches the real run there regardless of how long
rasterizing/encoding itself took.

The *running* act is different: its frames are buffered instead of
streamed live, so that if the slowest task takes a long time in real life
(minutes), the recorded clip of that segment can be **time-lapsed** down
to roughly `RUNNING_VIDEO_TARGET_SECONDS` (10s by default, see `ui.py`)
once both tasks are done and the real duration is known. Every number any
frame shows is still the real measurement -- only *playback speed*
changes -- and the applied multiplier (rounded to a clean step, e.g.
"~175x") is reported on the finished frame and in the summary.

Text is rasterized with the **JetBrains Mono Nerd Font** (falls back to
Consolas, then DejaVu Sans Mono on Linux) so the UI can use small icon
glyphs (a terminal glyph on each panel, a microchip for CPU stats, a
trophy for the winner, etc.) alongside plain ASCII/box-drawing characters.
Only glyphs confirmed present in that font's cmap are used anywhere in the
UI -- `rich` itself will happily use glyphs the font doesn't have, which
silently render as tofu boxes in the video, so this was checked with
`fontTools` against the font's actual cmap rather than assumed.

A short, procedurally-generated background track (a pulsing bass note
under a fast four-note arpeggio, synthesized on the fly by `ffmpeg`'s own
`lavfi` sources -- not a downloaded/licensed file, so there's no copyright
to clear) is muxed into the video by default; pass `--no-music` to skip
it.

Useful flags:

| flag | default | meaning |
|---|---|---|
| `--video PATH` | — | also/only render an mp4 |
| `--video-only` | off | skip the live terminal UI, just record |
| `--fps N` | 20 | video frame rate |
| `--video-cols N` | 234 | video width, in character cells (~4K-class at the default font size) |
| `--video-rows N` | 56 | video height, in character cells |
| `--cores N` | — | pin every task's process to N CPU cores (overrides the config's `cores`) |
| `--no-music` | off | don't mux in the procedurally-generated background music |

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
