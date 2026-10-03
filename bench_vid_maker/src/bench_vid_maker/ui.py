"""Clean, live terminal UI for screen-recording a benchmark comparison.

Three acts, always in this order:

1. Typing -- every task's source types itself onto screen, side by side.
   Nothing is running yet; this is a pure intro, not timed.
2. Starting -- one large modal, centered over the (now fully typed) panels,
   flashes briefly and then disappears the instant the processes launch.
3. Running -- every task's process is launched at the exact same instant,
   under a big clock that starts at 00:00.000000 right then. This is the
   only part that's actually timed; the typing intro never affects the
   numbers. A finished panel's background tints to show it's done.

If the running phase takes longer than RUNNING_VIDEO_TARGET_SECONDS of real
time, the *recorded video* of that phase (never the live terminal, and
never the numbers shown) is time-lapsed down to roughly that length, and
the applied speed-up is reported on screen.

A final table + a Chrome-DevTools-style resource waterfall break down
wall-clock vs. CPU user/system time per task and are held on screen for a
while.
"""
from __future__ import annotations

import pathlib
import platform
import sys
import threading
import time
from dataclasses import dataclass, field

import psutil
from rich.align import Align
from rich.columns import Columns
from rich.console import Console, Group, RenderableType
from rich.live import Live
from rich.measure import Measurement
from rich.panel import Panel
from rich.segment import Segment
from rich.style import Style
from rich.syntax import Syntax
from rich.table import Table
from rich.text import Text

from .config import BenchConfig, Task
from .recorder import VideoRecorder
from .runner import TaskResult, Tick, run_task

PALETTE = ["cyan", "magenta", "green", "yellow", "blue", "bright_red"]
BAR_WIDTH = 40
WATERFALL_WIDTH = 56
TRACK_STYLE = "grey35"            # unfilled portion of a bar: a dim, neutral track, not a hatched smear
TYPE_CHARS_PER_SECOND = 40.0      # typewriter speed for the code intro -- slow enough to actually read
TYPING_DONE_PAUSE_SECONDS = 1.0   # beat on the fully-typed, finished code before the starting modal appears
START_FLASH_SECONDS = 1.1         # how long the centered "starting" modal stays up
SPINNER_FRAMES = "|/-\\"   # ASCII spinner -- guaranteed to exist in any monospace font
UI_HZ = 20
PANEL_PADDING = (1, 2)
TOP_PADDING = (1, 3)
TOP_LINES = 3              # every top status box always has exactly 3 lines, so it never resizes
RUNNING_VIDEO_TARGET_SECONDS = 10.0  # recorded running segment is time-lapsed to roughly this long

# Nerd Font icon glyphs (JetBrainsMono Nerd Font Mono is the primary UI font,
# see recorder.py) -- each confirmed present in that font's cmap.
ICON_TERMINAL = "\uf120"
ICON_CPU = "\uf2db"
ICON_BOLT = "\uf0e7"
ICON_CHECK = "\uf00c"
ICON_CROSS = "\uf00d"
ICON_TROPHY = "\uf091"
ICON_ROCKET = "\uf135"
ICON_DESKTOP = "\uf108"
ICON_HDD = "\uf0a0"
ICON_TAG = "\uf02b"
ICON_PYTHON = "\ue73c"
ICON_MUSIC = "\uf001"


def _force_utf8_stdio() -> None:
    """Windows consoles (and anything else) default stdout to a legacy code
    page like cp1252, which can't encode the Nerd Font icons above -- Rich
    would blow up mid-render with a UnicodeEncodeError. Reconfigure both
    streams to UTF-8 first; errors='replace' so a glyph-less code page
    degrades to '?' instead of crashing the run."""
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is None:
            continue
        try:
            reconfigure(encoding="utf-8", errors="replace")
        except (ValueError, OSError):
            pass


def _fmt(seconds: float) -> str:
    if seconds < 60:
        return f"{seconds:6.3f}s"
    m, s = divmod(seconds, 60)
    return f"{int(m)}m {s:05.2f}s"


def _bar(fraction: float, color: str, width: int = BAR_WIDTH) -> Text:
    fraction = max(0.0, min(fraction, 1.0))
    filled = round(fraction * width)
    return Text.assemble((" " * filled, f"on {color}"), (" " * (width - filled), f"on {TRACK_STYLE}"))


def _round_speedup(x: float) -> float:
    """Round a video time-lapse multiplier to a clean, human-picked-looking
    step: nearest 0.5x below 10x, nearest 5x at or above it -- so the number
    shown on screen reads like "~175x", not an arbitrary timing artifact."""
    if x < 10:
        return round(x * 2) / 2
    return round(x / 5) * 5


def _task_source(task: Task) -> str | None:
    """Best-effort: the task's own script, for the typewriter animation.
    Falls back to the raw command line if we can't find a readable file
    (e.g. `python -c "..."`)."""
    if not task.command:
        return None
    candidate = pathlib.Path(task.command[-1])
    if not candidate.is_absolute():
        candidate = task.cwd / candidate
    if candidate.is_file():
        try:
            return candidate.read_text(encoding="utf-8")
        except OSError:
            pass
    return None


def _source_line_count(source: str | None) -> int:
    return source.count("\n") + 1 if source is not None else 1


# --------------------------------------------------------------------------
# Compositing: paints one already-rendered renderable (a modal) centered on
# top of another (the running frame), with the background dimmed -- a real
# single-instance "modal" look, not a per-panel popover. Rich has no z-order
# of its own, so this flattens both renderables to raw styled character
# cells first and splices the modal's cells over the background's.
# --------------------------------------------------------------------------

class _Composed:
    """A renderable made of pre-resolved rows of Segments."""

    def __init__(self, lines: list[list[Segment]]):
        self._lines = lines

    def __rich_console__(self, console, options):
        for line in self._lines:
            yield from line
            yield Segment.line()

    def __rich_measure__(self, console, options) -> Measurement:
        width = max((sum(seg.cell_length for seg in line) for line in self._lines), default=0)
        return Measurement(width, width)


def _row_chars(line: list[Segment]) -> list[tuple[str, Style | None]]:
    chars: list[tuple[str, Style | None]] = []
    for seg in line:
        chars.extend((ch, seg.style) for ch in seg.text)
    return chars


def _chars_to_segments(chars: list[tuple[str, Style | None]]) -> list[Segment]:
    segments: list[Segment] = []
    run: list[str] = []
    run_style: Style | None = None
    started = False
    for ch, style in chars:
        if not started or style != run_style:
            if run:
                segments.append(Segment("".join(run), run_style))
            run, run_style, started = [ch], style, True
        else:
            run.append(ch)
    if run:
        segments.append(Segment("".join(run), run_style))
    return segments


def _overlay_center(
    base: RenderableType, overlay: RenderableType, console: Console, overlay_width: int, dim_base: bool = True
) -> _Composed:
    width, height = console.width, console.height
    base_lines = console.render_lines(base, pad=True)
    overlay_lines = console.render_lines(overlay, options=console.options.update(width=overlay_width), pad=True)
    top = max(0, (height - len(overlay_lines)) // 2)
    left = max(0, (width - overlay_width) // 2)

    rows = [_row_chars(line) for line in base_lines]
    if dim_base:
        rows = [[(ch, (style or Style()) + Style(dim=True)) for ch, style in row] for row in rows]
    for i, oline in enumerate(overlay_lines):
        r = top + i
        if not (0 <= r < len(rows)):
            continue
        row = rows[r]
        for j, pair in enumerate(_row_chars(oline)):
            idx = left + j
            if 0 <= idx < len(row):
                row[idx] = pair
    return _Composed([_chars_to_segments(row) for row in rows])


# --------------------------------------------------------------------------
# Panel sizing: every panel (intro or running) uses the exact same fixed
# width/height -- the full width/height available in the frame, so nothing
# ever jumps, resizes, or leaves the terminal panels looking cramped.
# --------------------------------------------------------------------------

def _top_box_height() -> int:
    return TOP_LINES + 2 * TOP_PADDING[0] + 2


def _header_rows(cfg: BenchConfig) -> int:
    # blank + title + [subtitle] + blank + top box + blank, exactly what
    # _frame_shell below emits before the panel row.
    return 1 + 1 + (1 if cfg.subtitle else 0) + 1 + _top_box_height() + 1


def _top_width(total_width: int) -> int:
    return max(40, min(total_width - 4, 96))


def estimate_panel_size(cfg: BenchConfig, n_cols: int, total_width: int, total_height: int) -> tuple[int, int]:
    gap = 2 * (n_cols - 1)
    width = max(28, (total_width - gap) // n_cols)
    height = max(12, total_height - _header_rows(cfg))
    return width, height


def _code_block(source: str | None, command: list[str], revealed: int, cursor: bool) -> Syntax | Text:
    if source is None:
        return Text(" ".join(command), style="dim")
    text = source[:revealed]
    if cursor:
        text += "▌"
    # word_wrap off: panel width/height are fixed independent of content, so
    # wrapping would silently grow content past the fixed panel height.
    return Syntax(text, "python", theme="monokai", word_wrap=False, background_color="default")


def _fill_panel_body(code: RenderableType, code_lines: int, footer: list[Text], panel_height: int) -> Group:
    """Code at the top (as typed), footer status pinned to the bottom -- the
    gap between them simply grows with the panel instead of leaving the
    footer floating in the middle of a tall, mostly-empty box."""
    content_height = panel_height - 2 - 2 * PANEL_PADDING[0]
    filler = max(1, content_height - code_lines - len(footer))
    return Group(code, *([Text("")] * filler), *footer)


def _frame_shell(cfg: BenchConfig, top: Panel, panels: list[Panel]) -> Group:
    parts: list = [Text(""), Text(cfg.title, style="bold white", justify="center")]
    if cfg.subtitle:
        parts.append(Text(cfg.subtitle, style="dim", justify="center"))
    parts.append(Text(""))
    parts.append(Align.center(top))
    parts.append(Text(""))
    parts.append(Columns(panels, equal=True, expand=True, align="center"))
    return Group(*parts)


def _status_box(lines: list[Text], border: str, width: int) -> Panel:
    height = len(lines) + 2 * TOP_PADDING[0] + 2
    return Panel(Align.center(Group(*lines)), border_style=border, padding=TOP_PADDING, width=width, height=height)


# --------------------------------------------------------------------------
# Act 1: typing -- nothing is running yet.
# --------------------------------------------------------------------------

def _render_intro(
    cfg: BenchConfig, sources: list[str | None], panel_size: tuple[int, int],
    type_elapsed: float, typed_ats: list[float], total_width: int,
) -> Group:
    w, h = panel_size
    panels = []
    for i, task in enumerate(cfg.tasks):
        color = PALETTE[i % len(PALETTE)]
        source = sources[i]
        n = min(len(source), int(TYPE_CHARS_PER_SECOND * type_elapsed)) if source is not None else 0
        this_typed = type_elapsed < typed_ats[i]
        cursor = this_typed and int(type_elapsed * 2) % 2 == 0
        code = _code_block(source, task.command, n, cursor)
        footer = [Text("typing…" if this_typed else "ready", style="dim")]
        body = _fill_panel_body(code, _source_line_count(source), footer, h)
        panels.append(Panel(
            body, title=f"[bold {color}]{ICON_TERMINAL} {task.name}[/]", border_style=color,
            padding=PANEL_PADDING, width=w, height=h,
        ))
    engines = ", ".join(t.name for t in cfg.tasks)
    lines = [
        Text("typing…", style="bold white", justify="center"),
        Text("not timed", style="dim", justify="center"),
        Text(f"{len(cfg.tasks)} engines: {engines}", style="dim", justify="center"),
    ]
    top = _status_box(lines, border="bright_white", width=_top_width(total_width))
    return _frame_shell(cfg, top, panels)


def _starting_modal(cfg: BenchConfig) -> Panel:
    names = ", ".join(t.name for t in cfg.tasks)
    body = Group(
        Text(ICON_ROCKET, style="bold bright_white", justify="center"),
        Text(""),
        Text("STARTING BENCHMARK", style="bold bright_white", justify="center"),
        Text(""),
        Text(f"launching {names} at the same instant", style="white", justify="center"),
        Text("the clock starts the moment every process spawns", style="dim italic", justify="center"),
    )
    return Panel(Align.center(body), border_style="bright_white", padding=(2, 4), width=64)


# --------------------------------------------------------------------------
# Act 3: running -- every task launched at the same instant, real clock.
# --------------------------------------------------------------------------

@dataclass
class _LiveState:
    task: Task
    source: str | None
    elapsed: float = 0.0
    cpu_percent: float = 0.0
    cpu_user: float = 0.0
    cpu_system: float = 0.0
    done: bool = False
    result: TaskResult | None = None
    lock: threading.Lock = field(default_factory=threading.Lock)

    def on_tick(self, tick: Tick) -> None:
        with self.lock:
            self.elapsed = tick.elapsed
            self.cpu_percent = tick.cpu_percent
            self.cpu_user = tick.cpu_user
            self.cpu_system = tick.cpu_system

    def snapshot(self) -> "_LiveState":
        with self.lock:
            return _LiveState(
                task=self.task, source=self.source, elapsed=self.elapsed,
                cpu_percent=self.cpu_percent, cpu_user=self.cpu_user, cpu_system=self.cpu_system,
                done=self.done, result=self.result,
            )


def _progress_row(states: list[_LiveState]) -> Text:
    row = Text(justify="center")
    for i, s in enumerate(states):
        if i:
            row.append("    ")
        color = PALETTE[i % len(PALETTE)]
        mark = ICON_CHECK if s.done else SPINNER_FRAMES[int(s.elapsed * 10) % len(SPINNER_FRAMES)]
        row.append(f"{mark} {s.task.name}", style=f"bold {color}" if s.done else f"dim {color}")
    return row


def _clock_panel(
    states: list[_LiveState], elapsed: float, all_done: bool, speedup: float | None, width: int
) -> Panel:
    m, s = divmod(max(elapsed, 0.0), 60)
    # Six decimal places (microsecond resolution): a precise, "racing timer"
    # look where every digit shown is a real digit of time.perf_counter().
    spaced = "  ".join(f"{int(m):02d}:{s:09.6f}")
    style = "bold green" if all_done else "bold white"
    if all_done:
        label = f"both finished -- {ICON_BOLT} video time-lapsed {speedup:.0f}x" if speedup else "both finished"
    else:
        label = "running together"
    lines = [
        Text(spaced, style=style, justify="center"),
        Text(label, style="dim", justify="center"),
        _progress_row(states),
    ]
    return _status_box(lines, border="green" if all_done else "bright_white", width=width)


def _run_panel(state: _LiveState, color: str, width: int, height: int) -> Panel:
    full = state.source if state.source is not None else " ".join(state.task.command)
    code = _code_block(state.source, state.task.command, len(full), False)
    code_lines = _source_line_count(state.source)

    bg = "none"
    if state.result is None:
        frame = SPINNER_FRAMES[int(state.elapsed * 10) % len(SPINNER_FRAMES)]
        footer = [
            Text(f"{frame} running   {_fmt(state.elapsed)}", style=f"bold {color}"),
            Text(f"  {ICON_CPU} {state.cpu_percent:5.1f}%   user {state.cpu_user:.2f}s   sys {state.cpu_system:.2f}s",
                 style="dim"),
        ]
        border = color
    elif state.result.ok:
        r = state.result
        pct = r.cpu_total / r.elapsed * 100 if r.elapsed else 0.0
        footer = [
            Text(f"{ICON_CHECK} done   {_fmt(r.elapsed)}", style="bold green"),
            Text(f"  {ICON_CPU} {pct:5.1f}%   user {r.cpu_user:.2f}s   sys {r.cpu_system:.2f}s", style="dim"),
        ]
        border, bg = "green", "on #06230f"
    else:
        r = state.result
        footer = [Text(f"{ICON_CROSS} failed (exit {r.returncode})   {_fmt(r.elapsed)}", style="bold red")]
        border, bg = "red", "on #2a0a0a"

    body = _fill_panel_body(code, code_lines, footer, height)
    return Panel(
        body, title=f"[bold {color}]{ICON_TERMINAL} {state.task.name}[/]", border_style=border, style=bg,
        padding=PANEL_PADDING, width=width, height=height,
    )


def _render_running(
    cfg: BenchConfig, states: list[_LiveState], elapsed: float, panel_size: tuple[int, int],
    total_width: int, speedup: float | None = None,
) -> Group:
    snapshots = [s.snapshot() for s in states]
    all_done = all(s.done for s in snapshots)
    w, h = panel_size
    panels = [_run_panel(s, PALETTE[i % len(PALETTE)], w, h) for i, s in enumerate(snapshots)]
    top = _clock_panel(snapshots, elapsed, all_done, speedup, _top_width(total_width))
    return _frame_shell(cfg, top, panels)


# --------------------------------------------------------------------------
# Closing summary
# --------------------------------------------------------------------------

def _waterfall_legend() -> Text:
    legend = Text(justify="center")
    legend.append("\u25a0 ", style="bold white")
    legend.append("CPU user    ", style="dim")
    legend.append("\u25a0 ", style="bold grey58")
    legend.append("CPU sys    ", style="dim")
    legend.append("\u25a0 ", style=f"bold {TRACK_STYLE}")
    legend.append("idle / wait (I/O, scheduling)", style="dim")
    return legend


def _waterfall_chart(results: list[TaskResult], width: int = WATERFALL_WIDTH) -> Table:
    """A Chrome-DevTools-Network-panel-style waterfall: every task's bar sits
    on the same shared time axis (0..slowest), stacked into CPU-user /
    CPU-sys / idle segments, so you can see at a glance both how long each
    task took *and* how that time was actually spent."""
    slowest = max((r.elapsed for r in results), default=0.0) or 1.0

    grid = Table.grid(padding=(0, 1), expand=True)
    grid.add_column(ratio=0)
    grid.add_column(ratio=1)
    grid.add_column(ratio=0, justify="right")

    half = f"{slowest / 2:.2f}s"
    end = f"{slowest:.2f}s"
    ruler = Text("0s", style="dim")
    pad_mid = max(1, width // 2 - len(ruler.plain) - len(half) // 2)
    ruler.append(" " * pad_mid + half, style="dim")
    pad_end = max(1, width - len(ruler.plain) - len(end))
    ruler.append(" " * pad_end + end, style="dim")
    grid.add_row(Text(""), ruler, Text(""))

    for i, r in enumerate(results):
        color = PALETTE[i % len(PALETTE)]
        total_w = max(1, round((r.elapsed / slowest) * width))
        user_w = min(total_w, round((r.cpu_user / r.elapsed) * total_w) if r.elapsed else 0)
        sys_w = min(total_w - user_w, round((r.cpu_system / r.elapsed) * total_w) if r.elapsed else 0)
        idle_w = total_w - user_w - sys_w
        idle_s = max(0.0, r.elapsed - r.cpu_total)
        cpu_pct = r.cpu_total / r.elapsed * 100 if r.elapsed else 0.0
        bar = Text.assemble(
            (" " * user_w, f"on {color}"),
            (" " * sys_w, "on grey58"),
            (" " * idle_w, f"on {TRACK_STYLE}"),
            (" " * (width - total_w), ""),
        )
        grid.add_row(Text(r.name, style=f"bold {color}"), bar, Text(_fmt(r.elapsed).strip(), style=color))
        grid.add_row(
            Text(""),
            Text(
                f"user {r.cpu_user:.3f}s   sys {r.cpu_system:.3f}s   idle/wait {idle_s:.3f}s"
                f"   ({cpu_pct:.0f}% of a core)",
                style="dim",
            ),
            Text(""),
        )
    return grid


def _environment_block(cfg: BenchConfig, recorder: VideoRecorder | None) -> Group:
    """What this run actually used: hardware, OS, interpreter, every
    resolved command -- so the numbers above are reproducible/auditable,
    not just a bare table of times."""
    cores_logical = psutil.cpu_count(logical=True) or 1
    cores_physical = psutil.cpu_count(logical=False) or cores_logical
    ram_gb = psutil.virtual_memory().total / (1024 ** 3)
    os_line = f"{platform.system()} {platform.release()} ({platform.machine()})"
    cpu_line = f"{platform.processor() or platform.machine()}  ·  {cores_physical} physical / {cores_logical} logical cores"
    if cfg.cores:
        cpu_line += f"  ·  pinned to {cfg.cores} for this run"

    grid = Table.grid(padding=(0, 2))
    grid.add_column(justify="right")
    grid.add_column()

    def row(icon: str, label: str, value: str) -> None:
        grid.add_row(Text(f"{icon} {label}", style="dim"), Text(value, style="bold white"))

    row(ICON_DESKTOP, "OS", os_line)
    row(ICON_CPU, "CPU", cpu_line)
    row(ICON_HDD, "RAM", f"{ram_gb:.1f} GB")
    row(ICON_PYTHON, "Python", f"{platform.python_version()} ({platform.python_implementation()})")
    for t in cfg.tasks:
        grid.add_row(Text(f"{ICON_TAG} {t.name}", style="dim"), Text(" ".join(t.command), style="white"))
    if recorder is not None:
        note = f"{recorder.img_w}x{recorder.img_h} @ {recorder.fps}fps"
        if recorder.music:
            note += f"   {ICON_MUSIC} + procedurally synthesized background music (non-copyright)"
        row(ICON_BOLT, "video", note)

    return Group(
        Text(f"{ICON_DESKTOP} Environment", style="bold white", justify="center"),
        Text(""),
        Align.center(grid),
    )


def _render_summary(
    cfg: BenchConfig, results: list[TaskResult], speedup: float | None = None, recorder: VideoRecorder | None = None,
) -> Group:
    total = sum(r.elapsed for r in results)
    slowest_r = max(results, key=lambda r: r.elapsed)
    fastest_r = min(results, key=lambda r: r.elapsed)
    slowest = slowest_r.elapsed

    table = Table(title="Time allocation", show_lines=False)
    table.add_column("Task")
    table.add_column("Wall (s)", justify="right")
    table.add_column("CPU user (s)", justify="right")
    table.add_column("CPU sys (s)", justify="right")
    table.add_column("CPU total", justify="right")
    table.add_column("Share of total", justify="right")
    table.add_column(f"Allocation (of {_fmt(slowest)})")
    table.add_column("vs. slowest", justify="right")

    for i, r in enumerate(results):
        color = PALETTE[i % len(PALETTE)]
        share = r.elapsed / total if total else 0.0
        speedup_r = slowest / r.elapsed if r.elapsed > 0 else float("inf")
        cpu_pct = r.cpu_total / r.elapsed * 100 if r.elapsed else 0.0
        # Six decimal places throughout, matching the clock -- every digit is
        # a real digit from time.perf_counter()/psutil, never fabricated.
        table.add_row(
            Text(r.name, style=f"bold {color}"),
            f"{r.elapsed:.6f}",
            f"{r.cpu_user:.6f}",
            f"{r.cpu_system:.6f}",
            f"{r.cpu_total:.6f}s ({cpu_pct:.0f}%)",
            f"{share * 100:5.1f}%",
            _bar(r.elapsed / slowest if slowest else 0.0, color),
            f"{speedup_r:.2f}x" if r.elapsed < slowest else "—",
        )

    headline = Text(justify="center")
    if fastest_r is not slowest_r and fastest_r.elapsed:
        headline.append(f"{ICON_TROPHY} {fastest_r.name} ", style="bold green")
        headline.append(f"finished {slowest / fastest_r.elapsed:.2f}x faster ", style="bold green")
        headline.append(f"than {slowest_r.name}", style="bold green")
    else:
        headline.append("all tasks finished", style="bold green")
    if cfg.cores:
        headline.append(f"   ·   pinned to {cfg.cores} core{'s' if cfg.cores != 1 else ''}", style="dim")

    parts: list = [Text(""), Text(cfg.title, style="bold white", justify="center"), Text(""), headline]
    if speedup:
        parts.append(Text(
            f"({ICON_BOLT} video time-lapsed {speedup:.0f}x so the running segment fits in "
            f"~{RUNNING_VIDEO_TARGET_SECONDS:.0f}s on screen -- every number above is the real measurement)",
            style="dim italic", justify="center",
        ))
    parts += [
        Text(""),
        _environment_block(cfg, recorder),
        Text(""),
        table,
        Text(""),
        Text("Resource allocation timeline", style="bold white", justify="center"),
        _waterfall_legend(),
        Text(""),
        _waterfall_chart(results),
    ]
    return Group(*parts)


def _capture(recorder: VideoRecorder | None, frame, last_capture: float) -> float:
    """Repeat `frame` enough times to cover however much real wall-clock
    actually elapsed since the previous capture, so video playback time
    always matches the real run (rasterizing/encoding speed never matters).
    Used for the intro/starting/summary acts, which are never time-lapsed."""
    if recorder is None:
        return last_capture
    now = time.perf_counter()
    hold = max(1, round((now - last_capture) * recorder.fps))
    recorder.capture(frame, hold_frames=hold)
    return time.perf_counter()


def _flush_timelapse(recorder: VideoRecorder, buffered: list[tuple[float, RenderableType]], speedup: float) -> None:
    """Writes the buffered running-phase frames to the recorder compressed
    in time by `speedup`, so the recorded running segment plays back faster
    than it happened live -- a time-lapse. Every number any frame shows
    remains the real, accurate measurement; only *playback speed* changes."""
    carry = 0.0
    prev_ts = 0.0
    for ts, frame in buffered:
        carry += ((ts - prev_ts) / speedup) * recorder.fps
        prev_ts = ts
        hold = int(carry)
        carry -= hold
        if hold > 0:
            recorder.capture(frame, hold_frames=hold)


def run_with_ui(
    cfg: BenchConfig,
    console: Console | None = None,
    recorder: VideoRecorder | None = None,
) -> list[TaskResult]:
    _force_utf8_stdio()
    console = console or Console()
    sources = [_task_source(t) for t in cfg.tasks]
    typed_ats = [len(s) / TYPE_CHARS_PER_SECOND if s else 0.0 for s in sources]
    typing_done_at = max(typed_ats)
    panel_size = estimate_panel_size(cfg, len(cfg.tasks), console.width, console.height)

    with Live(console=console, refresh_per_second=UI_HZ, transient=False) as live:
        # Act 1: type every task's source -- purely cosmetic, no process
        # exists yet, never affects the clock.
        intro_start = time.perf_counter()
        last_capture = time.perf_counter()
        while True:
            te = time.perf_counter() - intro_start
            frame = _render_intro(cfg, sources, panel_size, te, typed_ats, console.width)
            live.update(frame)
            last_capture = _capture(recorder, frame, last_capture)
            if te >= typing_done_at:
                break
            time.sleep(1.0 / UI_HZ)

        base_frame = _render_intro(cfg, sources, panel_size, typing_done_at, typed_ats, console.width)

        # A short beat on the finished, fully-typed code before anything else
        # happens -- gives the viewer a moment to actually read it.
        pause_begin = time.perf_counter()
        while time.perf_counter() - pause_begin < TYPING_DONE_PAUSE_SECONDS:
            live.update(base_frame)
            last_capture = _capture(recorder, base_frame, last_capture)
            time.sleep(1.0 / UI_HZ)

        # Act 2: one single, large modal centered over the fully-typed
        # panels -- it closes (disappears) the instant Act 3 launches.
        modal = _starting_modal(cfg)
        starting_begin = time.perf_counter()
        while time.perf_counter() - starting_begin < START_FLASH_SECONDS:
            frame = _overlay_center(base_frame, modal, console, overlay_width=64)
            live.update(frame)
            last_capture = _capture(recorder, frame, last_capture)
            time.sleep(1.0 / UI_HZ)

        # Act 3: launch every task at the same instant; the clock starts
        # clean at 00:00.000000 right here. Frames are buffered rather than
        # streamed live to the recorder so the segment can be time-lapsed
        # afterwards into a short, watchable clip no matter how long the
        # slowest task actually takes in real life.
        states = [_LiveState(task=t, source=sources[i]) for i, t in enumerate(cfg.tasks)]
        results: list[TaskResult | None] = [None] * len(states)

        def worker(i: int, state: _LiveState) -> None:
            result = run_task(state.task, on_tick=state.on_tick, cores=cfg.cores)
            with state.lock:
                state.elapsed = result.elapsed
                state.cpu_user = result.cpu_user
                state.cpu_system = result.cpu_system
                state.done = True
                state.result = result
            results[i] = result

        threads = [threading.Thread(target=worker, args=(i, s), daemon=True) for i, s in enumerate(states)]
        run_start = time.perf_counter()
        for th in threads:
            th.start()

        buffered: list[tuple[float, RenderableType]] = []
        while not all(s.snapshot().done for s in states):
            elapsed = time.perf_counter() - run_start
            frame = _render_running(cfg, states, elapsed, panel_size, console.width)
            live.update(frame)
            if recorder is not None:
                buffered.append((elapsed, frame))
            time.sleep(1.0 / UI_HZ)
        for th in threads:
            th.join()

        real_duration = time.perf_counter() - run_start
        speedup = real_duration / RUNNING_VIDEO_TARGET_SECONDS if real_duration > RUNNING_VIDEO_TARGET_SECONDS else 1.0
        speedup_display = _round_speedup(speedup) if speedup > 1.0 else None

        final_frame = _render_running(cfg, states, real_duration, panel_size, console.width, speedup=speedup_display)
        live.update(final_frame)
        if recorder is not None:
            buffered.append((real_duration, final_frame))
            _flush_timelapse(recorder, buffered, speedup)
            recorder.capture(final_frame, hold_frames=recorder.fps * 2)  # hold ~2s on the finished frame
        time.sleep(1.2)  # let the "both finished" frame sit for a beat on camera

        summary = Panel(
            Align(_render_summary(cfg, results, speedup_display, recorder), align="center", vertical="middle"),
            border_style="green", width=console.width, height=console.height,
        )
        live.update(summary)
        if recorder is not None:
            recorder.capture(summary, hold_frames=recorder.fps * 8)  # hold ~8s on the summary
        time.sleep(0.2)

    final = [r for r in results if r is not None]
    for r in final:
        if not r.ok:
            console.print(f"[bold red]{r.name} exited {r.returncode}[/]")
            if r.stderr.strip():
                console.print(r.stderr.strip())

    return final
