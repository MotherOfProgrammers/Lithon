"""Clean, live terminal UI for screen-recording a benchmark comparison.

Three acts, always in this order:

1. Typing -- every task's source types itself onto screen, side by side.
   Nothing is running yet; this is a pure intro, not timed.
2. Starting -- a brief "starting..." popover on every panel at once.
3. Running -- every task's process is launched at the exact same instant,
   under a big clock that starts at 00:00.00 right then. This is the only
   part that's actually timed; the typing intro never affects the numbers.

A final table + chart breaks down wall-clock vs. CPU user/system time per
task and is held on screen for a while.
"""
from __future__ import annotations

import pathlib
import threading
import time
from dataclasses import dataclass, field

from rich.align import Align
from rich.columns import Columns
from rich.console import Console, Group
from rich.live import Live
from rich.panel import Panel
from rich.syntax import Syntax
from rich.table import Table
from rich.text import Text

from .config import BenchConfig, Task
from .recorder import VideoRecorder
from .runner import TaskResult, Tick, run_task

PALETTE = ["cyan", "magenta", "green", "yellow", "blue", "bright_red"]
BAR_WIDTH = 40
TRACK_STYLE = "grey35"            # unfilled portion of a bar: a dim, neutral track, not a hatched smear
TYPE_CHARS_PER_SECOND = 70.0      # typewriter speed for the code intro
START_FLASH_SECONDS = 0.5         # how long the "starting..." popover stays up
SPINNER_FRAMES = "|/-\\"   # ASCII spinner -- guaranteed to exist in any monospace font
UI_HZ = 20
PANEL_PADDING = (1, 2)


def _fmt(seconds: float) -> str:
    if seconds < 60:
        return f"{seconds:6.3f}s"
    m, s = divmod(seconds, 60)
    return f"{int(m)}m {s:05.2f}s"


def _bar(fraction: float, color: str, width: int = BAR_WIDTH) -> Text:
    fraction = max(0.0, min(fraction, 1.0))
    filled = round(fraction * width)
    return Text.assemble((" " * filled, f"on {color}"), (" " * (width - filled), f"on {TRACK_STYLE}"))


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


# --------------------------------------------------------------------------
# Panel sizing: every panel (intro or running) uses the exact same fixed
# width/height, so nothing ever jumps or resizes between acts.
# --------------------------------------------------------------------------

def estimate_panel_size(cfg: BenchConfig, n_cols: int, total_width: int) -> tuple[int, int]:
    sources = [_task_source(t) for t in cfg.tasks]
    max_lines = max((len(s.splitlines()) for s in sources if s is not None), default=1)
    max_line_len = max((len(line) for s in sources if s for line in s.splitlines()), default=20)
    # code lines + 1 blank separator + 3 footer rows (the starting popover is
    # itself a 3-line bordered box) + 2 border rows + vertical padding
    height = max_lines + 1 + 3 + 2 + PANEL_PADDING[0] * 2
    inner_w = max(max_line_len + 2, max(len(t.name) for t in cfg.tasks) + 6, 28)
    gap = 2 * (n_cols - 1)
    width = min(inner_w + 2 + PANEL_PADDING[1] * 2, (total_width - gap) // n_cols)
    return width, height


def _code_block(source: str | None, command: list[str], revealed: int, cursor: bool) -> Syntax | Text:
    if source is None:
        return Text(" ".join(command), style="dim")
    text = source[:revealed]
    if cursor:
        text += "▌"
    # word_wrap off: panel height is sized from raw line counts, so wrapping
    # would silently grow content past the fixed panel height.
    return Syntax(text, "python", theme="monokai", word_wrap=False, background_color="default")


def _frame_shell(cfg: BenchConfig, top: Panel, panels: list[Panel]) -> Group:
    parts: list = [Text(""), Text(cfg.title, style="bold white", justify="center")]
    if cfg.subtitle:
        parts.append(Text(cfg.subtitle, style="dim", justify="center"))
    parts.append(Text(""))
    parts.append(Align.center(top))
    parts.append(Text(""))
    parts.append(Columns(panels, equal=True, expand=True, align="center"))
    return Group(*parts)


def _status_box(lines: list[Text], border: str) -> Panel:
    return Panel(Align.center(Group(*lines)), border_style=border, width=42)


# --------------------------------------------------------------------------
# Act 1 & 2: typing, then starting -- nothing is running yet.
# --------------------------------------------------------------------------

def _render_intro(
    cfg: BenchConfig, sources: list[str | None], panel_size: tuple[int, int],
    type_elapsed: float, typed_ats: list[float], starting: bool,
) -> Group:
    w, h = panel_size
    panels = []
    for i, task in enumerate(cfg.tasks):
        color = PALETTE[i % len(PALETTE)]
        source = sources[i]
        n = min(len(source), int(TYPE_CHARS_PER_SECOND * type_elapsed)) if source is not None else 0
        this_typed = type_elapsed < typed_ats[i]
        cursor = this_typed and int(type_elapsed * 2) % 2 == 0
        body = [_code_block(source, task.command, n, cursor), Text("")]
        if starting:
            popover = Panel(
                Text("\u25cf starting\u2026", style=f"bold {color}", justify="center"),
                border_style=color, padding=(0, 1), width=16,
            )
            body.append(Align.center(popover))
        else:
            body.append(Text("typing…" if this_typed else "", style="dim"))
        panels.append(Panel(
            Group(*body), title=f"[bold {color}]{task.name}[/]", border_style=color,
            padding=PANEL_PADDING, width=w, height=h,
        ))
    label = "starting…" if starting else "typing…"
    top = _status_box(
        [Text(label, style="bold white", justify="center"), Text("not timed", style="dim", justify="center")],
        border="bright_white",
    )
    return _frame_shell(cfg, top, panels)


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


def _clock_panel(elapsed: float, all_done: bool) -> Panel:
    m, s = divmod(max(elapsed, 0.0), 60)
    # Six decimal places (microsecond resolution): a precise, "racing timer"
    # look where every digit shown is a real digit of time.perf_counter().
    spaced = "  ".join(f"{int(m):02d}:{s:09.6f}")
    style = "bold green" if all_done else "bold white"
    label = "both finished" if all_done else "running together"
    return _status_box(
        [Text(spaced, style=style, justify="center"), Text(label, style="dim", justify="center")],
        border="green" if all_done else "bright_white",
    )


def _run_panel(state: _LiveState, color: str, width: int, height: int) -> Panel:
    full = state.source if state.source is not None else " ".join(state.task.command)
    body: list = [_code_block(state.source, state.task.command, len(full), False), Text("")]

    if state.result is None:
        frame = SPINNER_FRAMES[int(state.elapsed * 10) % len(SPINNER_FRAMES)]
        body.append(Group(
            Text(f"{frame} running   {_fmt(state.elapsed)}", style=f"bold {color}"),
            Text(f"  cpu {state.cpu_percent:5.1f}%   user {state.cpu_user:.2f}s   sys {state.cpu_system:.2f}s",
                 style="dim"),
        ))
        border = color
    elif state.result.ok:
        r = state.result
        pct = r.cpu_total / r.elapsed * 100 if r.elapsed else 0.0
        body.append(Group(
            Text(f"\u25cf done   {_fmt(r.elapsed)}", style="bold green"),
            Text(f"  cpu {pct:5.1f}%   user {r.cpu_user:.2f}s   sys {r.cpu_system:.2f}s", style="dim"),
        ))
        border = "green"
    else:
        r = state.result
        body.append(Text(f"\u25cf failed (exit {r.returncode})   {_fmt(r.elapsed)}", style="bold red"))
        border = "red"

    return Panel(
        Group(*body), title=f"[bold {color}]{state.task.name}[/]", border_style=border,
        padding=PANEL_PADDING, width=width, height=height,
    )


def _render_running(cfg: BenchConfig, states: list[_LiveState], elapsed: float, panel_size: tuple[int, int]) -> Group:
    snapshots = [s.snapshot() for s in states]
    all_done = all(s.done for s in snapshots)
    w, h = panel_size
    panels = [_run_panel(s, PALETTE[i % len(PALETTE)], w, h) for i, s in enumerate(snapshots)]
    return _frame_shell(cfg, _clock_panel(elapsed, all_done), panels)


# --------------------------------------------------------------------------
# Closing summary
# --------------------------------------------------------------------------

def _vertical_chart(results: list[TaskResult], height: int = 10) -> Table:
    """One column per task, bar height proportional to wall time, with a
    percentage-of-slowest label on top -- fills the space below the table
    with something readable at a glance, not just another number."""
    slowest = max((r.elapsed for r in results), default=0.0)
    grid = Table.grid(padding=(0, 3), expand=True)
    for _ in results:
        grid.add_column(justify="center")

    def bar_rows(r: TaskResult, color: str) -> Group:
        pct = (r.elapsed / slowest * 100) if slowest else 0.0
        filled = round((r.elapsed / slowest) * height) if slowest else 0
        rows = [
            Text("█" * 8 if row >= height - filled else " " * 8,
                 style=color if row >= height - filled else TRACK_STYLE, justify="center")
            for row in range(height)
        ]
        return Group(
            Text(f"{pct:.0f}%", style=f"bold {color}", justify="center"),
            *rows,
            Text("\u2500" * 8, style="dim", justify="center"),
            Text(r.name, style=f"bold {color}", justify="center"),
            Text(_fmt(r.elapsed).strip(), style=color, justify="center"),
        )

    grid.add_row(*[bar_rows(r, PALETTE[i % len(PALETTE)]) for i, r in enumerate(results)])
    return grid


def _render_summary(cfg: BenchConfig, results: list[TaskResult]) -> Group:
    total = sum(r.elapsed for r in results)
    slowest = max(r.elapsed for r in results)

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
        speedup = slowest / r.elapsed if r.elapsed > 0 else float("inf")
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
            f"{speedup:.2f}x" if r.elapsed < slowest else "—",
        )

    return Group(
        Text(""),
        Text(cfg.title, style="bold white", justify="center"),
        Text(""),
        table,
        Text(""),
        Text("Wall time, visually", style="bold white", justify="center"),
        Text(""),
        _vertical_chart(results),
    )


def estimate_frame_height(cfg: BenchConfig, width_chars: int = 150, pad: int = 2, lo: int = 16, hi: int = 60) -> int:
    """How many character rows the tallest frame (intro / running / summary)
    actually needs, so a recorded video isn't mostly black canvas."""
    probe = Console(width=width_chars, height=hi * 2)
    panel_size = estimate_panel_size(cfg, len(cfg.tasks), width_chars)
    sources = [_task_source(t) for t in cfg.tasks]
    typed_ats = [len(s) / TYPE_CHARS_PER_SECOND if s else 0.0 for s in sources]
    states = [_LiveState(task=t, source=sources[i]) for i, t in enumerate(cfg.tasks)]

    samples: list = [
        _render_intro(cfg, sources, panel_size, 0.0, typed_ats, starting=False),
        _render_intro(cfg, sources, panel_size, max(typed_ats) + 0.01, typed_ats, starting=True),
        _render_running(cfg, states, 999.0, panel_size),
    ]
    dummy_results = [
        TaskResult(name=t.name, command=t.command, returncode=0, elapsed=1.0,
                   cpu_user=0.5, cpu_system=0.1, stdout="", stderr="")
        for t in cfg.tasks
    ]
    samples.append(Panel(_render_summary(cfg, dummy_results), border_style="green"))

    deepest = 0
    for sample in samples:
        lines = probe.render_lines(sample, pad=False)
        used = max((i for i, line in enumerate(lines) if any(seg.text.strip() for seg in line)), default=0) + 1
        deepest = max(deepest, used)
    return max(lo, min(hi, deepest + pad))


def _capture(recorder: VideoRecorder | None, frame, last_capture: float) -> float:
    """Repeat `frame` enough times to cover however much real wall-clock
    actually elapsed since the previous capture, so video playback time
    always matches the real run (rasterizing/encoding speed never matters)."""
    if recorder is None:
        return last_capture
    now = time.perf_counter()
    hold = max(1, round((now - last_capture) * recorder.fps))
    recorder.capture(frame, hold_frames=hold)
    return time.perf_counter()


def run_with_ui(
    cfg: BenchConfig,
    console: Console | None = None,
    recorder: VideoRecorder | None = None,
) -> list[TaskResult]:
    console = console or Console()
    sources = [_task_source(t) for t in cfg.tasks]
    typed_ats = [len(s) / TYPE_CHARS_PER_SECOND if s else 0.0 for s in sources]
    typing_done_at = max(typed_ats)
    panel_size = estimate_panel_size(cfg, len(cfg.tasks), console.width)

    with Live(console=console, refresh_per_second=UI_HZ, transient=False) as live:
        # Act 1 + 2: type every task's source, then flash "starting" --
        # purely cosmetic, no process exists yet.
        intro_start = time.perf_counter()
        last_capture = time.perf_counter()
        intro_end = typing_done_at + START_FLASH_SECONDS
        while True:
            te = time.perf_counter() - intro_start
            starting = te >= typing_done_at
            frame = _render_intro(cfg, sources, panel_size, te, typed_ats, starting)
            live.update(frame)
            last_capture = _capture(recorder, frame, last_capture)
            if te >= intro_end:
                break
            time.sleep(1.0 / UI_HZ)

        # Act 3: launch every task at the same instant; the clock starts
        # clean at 00:00.00 right here.
        states = [_LiveState(task=t, source=sources[i]) for i, t in enumerate(cfg.tasks)]
        results: list[TaskResult | None] = [None] * len(states)

        def worker(i: int, state: _LiveState) -> None:
            result = run_task(state.task, on_tick=state.on_tick)
            with state.lock:
                state.elapsed = result.elapsed
                state.cpu_user = result.cpu_user
                state.cpu_system = result.cpu_system
                state.done = True
                state.result = result
            results[i] = result

        threads = [threading.Thread(target=worker, args=(i, s), daemon=True) for i, s in enumerate(states)]
        start = time.perf_counter()
        for th in threads:
            th.start()
        last_capture = time.perf_counter()
        while not all(s.snapshot().done for s in states):
            elapsed = time.perf_counter() - start
            frame = _render_running(cfg, states, elapsed, panel_size)
            live.update(frame)
            last_capture = _capture(recorder, frame, last_capture)
            time.sleep(1.0 / UI_HZ)
        for th in threads:
            th.join()

        frame = _render_running(cfg, states, time.perf_counter() - start, panel_size)
        live.update(frame)
        if recorder is not None:
            recorder.capture(frame, hold_frames=recorder.fps * 2)  # hold ~2s on the finished frame
        time.sleep(1.2)  # let the "both finished" frame sit for a beat on camera

        summary = Panel(_render_summary(cfg, results), border_style="green")
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
