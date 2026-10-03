"""Runs one Task as a subprocess, timing it with time.perf_counter() and
calling back at a fixed cadence so a live UI can show an accurate, smoothly
updating timer -- plus live CPU usage -- while the process is in flight."""
from __future__ import annotations

import dataclasses
import subprocess
import time
from collections.abc import Callable

import psutil

from .config import Task

TICK_SECONDS = 0.05  # UI refresh cadence while a task is running


@dataclasses.dataclass
class Tick:
    elapsed: float       # wall-clock seconds since process spawn
    cpu_percent: float   # instantaneous CPU utilization since the previous tick
    cpu_user: float      # cumulative user CPU seconds consumed so far
    cpu_system: float    # cumulative system CPU seconds consumed so far


@dataclasses.dataclass
class TaskResult:
    name: str
    command: list[str]
    returncode: int
    elapsed: float       # wall-clock seconds, process start to exit
    cpu_user: float       # cumulative user CPU seconds (last sample before exit)
    cpu_system: float     # cumulative system CPU seconds (last sample before exit)
    stdout: str
    stderr: str

    @property
    def ok(self) -> bool:
        return self.returncode == 0

    @property
    def cpu_total(self) -> float:
        return self.cpu_user + self.cpu_system


def run_task(task: Task, on_tick: Callable[[Tick], None] | None = None) -> TaskResult:
    """Spawn `task`, polling at TICK_SECONDS so `on_tick(Tick)` can drive a
    live display. `elapsed` is wall-clock from process spawn to process exit
    (what a viewer watching a timer on screen would see); CPU figures come
    from the OS's own process accounting (psutil), sampled at the same
    cadence -- polling only affects UI smoothness, never the recorded
    values."""
    start = time.perf_counter()
    proc = subprocess.Popen(
        task.command, cwd=task.cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
    )
    try:
        ps_proc: psutil.Process | None = psutil.Process(proc.pid)
        ps_proc.cpu_percent(interval=None)  # prime the internal sampler
    except psutil.Error:
        ps_proc = None

    cpu_user = cpu_system = cpu_percent = 0.0

    def sample() -> None:
        nonlocal cpu_user, cpu_system, cpu_percent
        if ps_proc is None:
            return
        try:
            times = ps_proc.cpu_times()
            cpu_user, cpu_system = times.user, times.system
            cpu_percent = ps_proc.cpu_percent(interval=None)
        except psutil.Error:
            pass

    stdout = stderr = ""
    try:
        while True:
            try:
                stdout, stderr = proc.communicate(timeout=TICK_SECONDS)
                break
            except subprocess.TimeoutExpired:
                sample()
                if on_tick is not None:
                    on_tick(Tick(time.perf_counter() - start, cpu_percent, cpu_user, cpu_system))
    except BaseException:
        proc.kill()
        proc.wait()
        raise
    sample()  # one last, freshest-possible reading before the process is gone
    elapsed = time.perf_counter() - start
    if on_tick is not None:
        on_tick(Tick(elapsed, cpu_percent, cpu_user, cpu_system))
    return TaskResult(
        name=task.name,
        command=task.command,
        returncode=proc.returncode,
        elapsed=elapsed,
        cpu_user=cpu_user,
        cpu_system=cpu_system,
        stdout=stdout,
        stderr=stderr,
    )
