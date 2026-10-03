"""Loads a comparison config (TOML) into a list of runnable Tasks.

Config shape::

    title = "1,000,000,000-iteration sum: CPython vs Lithon"
    subtitle = "optional, shown under the title"            # optional

    [[tasks]]
    name = "CPython"
    command = ["{python}", "bench_vid_maker/tasks/python_sum.py"]

    [[tasks]]
    name = "Lithon"
    command = ["{lithon}", "test_codes_lithon/test_1b_fsum.py"]
    cwd = "."                                                 # optional, relative to repo root

All relative paths (command args and ``cwd``) are resolved against the repo
root, so configs stay short and portable regardless of where you invoke
``bench-vid-maker`` from. ``{python}`` and ``{lithon}`` are expanded to the
interpreters/executables described below; add new placeholders in
``_placeholders`` as new engines get compared.
"""
from __future__ import annotations

import dataclasses
import pathlib
import shutil
import sys
import tomllib


def find_repo_root(start: pathlib.Path) -> pathlib.Path:
    """Walk upward from `start` looking for a `.git` directory; fall back to
    the bench_vid_maker project's own parent so the tool still works if it is
    ever copied outside the Lithon repo."""
    for candidate in (start, *start.parents):
        if (candidate / ".git").exists():
            return candidate
    return start.parent


PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[2]  # .../bench_vid_maker
REPO_ROOT = find_repo_root(PROJECT_ROOT)


def _find_lithon() -> str:
    for rel in (".venv/Scripts/lithon.exe", ".venv/bin/lithon"):
        p = REPO_ROOT / rel
        if p.exists():
            return str(p)
    found = shutil.which("lithon")
    if found:
        return found
    raise FileNotFoundError(
        "could not find a built `lithon` executable (looked in "
        f"{REPO_ROOT / '.venv/Scripts'} and {REPO_ROOT / '.venv/bin'}, and on PATH). "
        "Build/install Lithon first."
    )


@dataclasses.dataclass(frozen=True)
class Task:
    name: str
    command: list[str]
    cwd: pathlib.Path


@dataclasses.dataclass(frozen=True)
class BenchConfig:
    title: str
    subtitle: str | None
    tasks: list[Task]
    path: pathlib.Path


def _expand(token: str) -> str:
    if token == "{python}":
        return sys.executable
    if token == "{lithon}":
        return _find_lithon()
    return token


def load_config(path: pathlib.Path) -> BenchConfig:
    path = pathlib.Path(path).resolve()
    data = tomllib.loads(path.read_text(encoding="utf-8"))

    raw_tasks = data.get("tasks")
    if not raw_tasks:
        raise ValueError(f"{path}: no [[tasks]] entries")

    tasks: list[Task] = []
    for i, t in enumerate(raw_tasks):
        if "name" not in t or "command" not in t:
            raise ValueError(f"{path}: tasks[{i}] is missing 'name' or 'command'")
        command = [_expand(tok) for tok in t["command"]]
        cwd = REPO_ROOT / t.get("cwd", ".")
        tasks.append(Task(name=t["name"], command=command, cwd=cwd.resolve()))

    return BenchConfig(
        title=data.get("title", path.stem),
        subtitle=data.get("subtitle"),
        tasks=tasks,
        path=path,
    )
