"""
Python bridge for Lithon.

    import lithon
    lithon.run("script.py")                    # native when provably safe, else interpreter
    lithon.run("script.py", mode="strict")     # native only; raises if it cannot be proven safe
    lithon.run_source("print(2 + 3)")          # run a source string
    ir = lithon.compile("script.py")           # just get the IR text

How it works
------------
Compile step (this process): the frontend turns the source into Lithon IR
using Python's own `ast` module. That is the ONLY place Python is involved.
Run step (separate process): build/tier_runner executes the IR natively (or
on the fallback interpreter). The native program never touches CPython: no
Python objects, no reference counting, no garbage collector.

The run step is a subprocess on purpose. Lithon's JIT is young, and a bug in
generated machine code should take down a child process, not your Python
session.
"""
from __future__ import annotations

import ast
import gc
import importlib.util
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

__all__ = ["run", "run_source", "compile", "compile_source", "RunResult",
           "LithonError", "MODES"]
__version__ = "0.1.0"

MODES = ("auto", "strict", "interp")

_ROOT = Path(os.environ.get("LITHON_HOME", Path(__file__).resolve().parent.parent))
_FRONTEND = _ROOT / "src" / "frontend" / "frontend.py"
_RUNNER = Path(os.environ.get("LITHON_RUNNER", _ROOT / "build" / "tier_runner"))

_frontend_module = None


class LithonError(RuntimeError):
    """Compile failure, type-check rejection, or strict-mode refusal."""

    def __init__(self, message: str, returncode: Optional[int] = None):
        super().__init__(message)
        self.returncode = returncode


@dataclass
class RunResult:
    returncode: int
    tier: str            # "native", "interpreter", or "unknown"
    stdout: str          # empty unless capture=True
    stderr: str


def _load_frontend():
    global _frontend_module
    if _frontend_module is None:
        if not _FRONTEND.exists():
            raise LithonError(f"frontend not found at {_FRONTEND}; set LITHON_HOME")
        spec = importlib.util.spec_from_file_location("_lithon_frontend", _FRONTEND)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _frontend_module = mod
    return _frontend_module


def compile_source(source: str, *, disable_gc: bool = True) -> str:
    """Python source -> Lithon IR text."""
    fe = _load_frontend()
    was_enabled = gc.isenabled()
    if disable_gc and was_enabled:
        gc.disable()   # compile step only; the native run never uses Python's GC anyway
    try:
        try:
            tree = ast.parse(source)
            return fe.build_program(tree)
        except SyntaxError as e:
            raise LithonError(f"syntax error: {e}") from e
        except NotImplementedError as e:
            raise LithonError(f"not supported by Lithon yet: {e}") from e
    finally:
        if disable_gc and was_enabled:
            gc.enable()


def compile(path) -> str:  # noqa: A001 (shadowing the builtin is intentional here)
    """Python file -> Lithon IR text."""
    return compile_source(Path(path).read_text())


def _tier_from(stderr: str) -> str:
    if "[tier1]" in stderr:
        return "native"
    if "[tier0]" in stderr:
        return "interpreter"
    return "unknown"


def run_ir(ir_text: str, mode: str = "auto", capture: bool = False) -> RunResult:
    if mode not in MODES:
        raise ValueError(f"mode must be one of {MODES}, got {mode!r}")
    if not _RUNNER.exists():
        raise LithonError(
            f"native runner not found at {_RUNNER}. Build it first (see "
            "src/jit/tier_runner.cpp) or set LITHON_RUNNER.")

    with tempfile.NamedTemporaryFile("w", suffix=".ir", delete=False) as f:
        f.write(ir_text)
        ir_path = f.name
    try:
        proc = subprocess.run(
            [str(_RUNNER), ir_path, f"--{mode}"],
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.PIPE,
            text=True,
        )
    finally:
        os.unlink(ir_path)

    result = RunResult(proc.returncode, _tier_from(proc.stderr),
                       proc.stdout if capture else "", proc.stderr)
    if proc.returncode == 3:
        raise LithonError(_explain(proc.stderr), 3)
    if proc.returncode != 0:
        raise LithonError(_explain(proc.stderr) or f"runner exited {proc.returncode}",
                          proc.returncode)
    return result


def _explain(stderr: str) -> str:
    keep = [ln for ln in stderr.splitlines()
            if ln.startswith(("[guard]", "[strict]", "RCR error", "error:"))]
    return "\n".join(keep) or stderr.strip()


def run(path, mode: str = "auto", capture: bool = False) -> RunResult:
    """Compile a Python file and execute it. See module docstring for modes."""
    return run_ir(compile(path), mode=mode, capture=capture)


def run_source(source: str, mode: str = "auto", capture: bool = False) -> RunResult:
    return run_ir(compile_source(source), mode=mode, capture=capture)
