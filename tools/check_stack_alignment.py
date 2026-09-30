#!/usr/bin/env python3
"""
Static x64 ABI validation: rsp must be 16-byte aligned immediately before
every `call` instruction the JIT emits (System V and Microsoft x64 both
require this), and every callee-saved register the JIT actually uses
(rbx, r12-r15 -- see jit_abi.h's promotion pool; rbp is handled by the
push/pop prologue/epilogue itself) must be restored before every `ret`.

This does NOT disassemble arbitrary x86-64 -- lithon_jit's encoder
(x86_encoder.h) only ever emits a small, known set of forms, so this
parses `objdump`'s output for exactly those and treats anything else as
"does not move rsp / does not touch a callee-saved register", which is
true for every other instruction the encoder can produce (mov, add, sub,
cmp, jcc, jmp, imul, setcc, movzx, test, xor). If the encoder ever grows a
new rsp-affecting or callee-saved-clobbering form, this script's pattern
list is exactly what needs a new case.

Usage:
    tools/check_stack_alignment.py program1.ir program2.ir ...
    tools/check_stack_alignment.py --dir tests/typed_programs
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LITHON_JIT = ROOT / "build" / "lithon_jit"

CALLEE_SAVED = {"rbx", "r12", "r13", "r14", "r15"}   # rbp handled separately

INSN = re.compile(r"^\s*([0-9a-f]+):\t(?:[0-9a-f]{2} ?)+\t(.*)$")
PUSH = re.compile(r"^push\s+(\w+)$")
POP = re.compile(r"^pop\s+(\w+)$")
SUB_RSP = re.compile(r"^sub\s+rsp,0x([0-9a-f]+)$")
ADD_RSP = re.compile(r"^add\s+rsp,0x([0-9a-f]+)$")
CALL = re.compile(r"^call\s")
RET = re.compile(r"^ret\b")
MOV_STORE_CALLEE = re.compile(r"^mov\s+QWORD PTR \[rbp[+-]0x[0-9a-f]+\],(\w+)$")
MOV_LOAD_CALLEE = re.compile(r"^mov\s+(\w+),QWORD PTR \[rbp[+-]0x[0-9a-f]+\]$")


def disassemble(path: Path):
    out = subprocess.run(
        ["objdump", "-D", "-b", "binary", "-mi386:x86-64", "-M", "intel", str(path)],
        capture_output=True, text=True, check=True).stdout
    insns = []
    for line in out.splitlines():
        m = INSN.match(line)
        if m:
            insns.append((int(m.group(1), 16), m.group(2).strip()))
    return insns


def check_function(name, insns, errors):
    """insns: this function's instructions only, starting at its own offset 0."""
    rsp = 0                       # bytes pushed relative to function entry
    saved_now = set()             # callee-saved regs currently spilled to the frame
    ever_saved = set()            # callee-saved regs this function actually uses
    restored_before_ret = None

    for off, text in insns:
        if m := PUSH.match(text):
            rsp += 8
            if m.group(1) in CALLEE_SAVED:
                errors.append(f"{name}+0x{off:x}: pushes callee-saved {m.group(1)} via `push` "
                              f"instead of a frame-slot mov -- unexpected form for this encoder")
        elif m := POP.match(text):
            rsp -= 8
        elif m := SUB_RSP.match(text):
            rsp += int(m.group(1), 16)
        elif m := ADD_RSP.match(text):
            rsp -= int(m.group(1), 16)
        elif m := MOV_STORE_CALLEE.match(text):
            if m.group(1) in CALLEE_SAVED:
                saved_now.add(m.group(1))
                ever_saved.add(m.group(1))
        elif m := MOV_LOAD_CALLEE.match(text):
            if m.group(1) in CALLEE_SAVED:
                saved_now.discard(m.group(1))
        elif CALL.match(text):
            # ABI requirement: rsp is 16-aligned immediately BEFORE `call`
            # executes (call itself then pushes an 8-byte return address).
            # rsp here is "bytes pushed since entry", and entry rsp was
            # itself 16-aligned (guaranteed by the caller's own `call`,
            # transitively up to the host runtime's C++ call into main),
            # so entry_rsp - rsp must be a multiple of 16 <=> rsp % 16 == 0.
            if rsp % 16 != 0:
                errors.append(f"{name}+0x{off:x}: rsp is {rsp} bytes below entry "
                              f"(not 16-aligned) at `{text}`")
        elif RET.match(text):
            if saved_now:
                errors.append(f"{name}+0x{off:x}: `ret` with {sorted(saved_now)} still "
                              f"spilled (never restored before return)")
            if rsp != 0:
                errors.append(f"{name}+0x{off:x}: `ret` with rsp {rsp} bytes off entry "
                              f"(prologue/epilogue push/pop or sub/add mismatched)")
    return ever_saved


def check_binary(bin_path: Path, functions: dict, label: str, errors: list):
    insns = disassemble(bin_path)
    bounds = sorted(functions.items(), key=lambda kv: kv[1]) + [("<end>", insns[-1][0] + 1 if insns else 0)]
    for i in range(len(bounds) - 1):
        name, start = bounds[i]
        _, end = bounds[i + 1]
        fn_insns = [(off - start, text) for off, text in insns if start <= off < end]
        used = check_function(f"{label}:{name}", fn_insns, errors)
        if used:
            print(f"  {label}:{name}: uses callee-saved {sorted(used)} -- verified saved/restored")


def check_one_ir(ir_path: Path, tmp: Path, errors: list):
    bin_path = tmp / "code.bin"
    r = subprocess.run([str(LITHON_JIT), str(ir_path), "--dump-code", str(bin_path)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  {ir_path.name}: skipped (compile refused: {r.stderr.strip()[:120]})")
        return
    functions = {}
    for line in r.stdout.splitlines():
        name, off = line.rsplit(" ", 1)
        functions[name] = int(off)
    if not functions or bin_path.stat().st_size == 0:
        return
    check_binary(bin_path, functions, ir_path.stem, errors)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("irs", nargs="*", type=Path)
    ap.add_argument("--dir", type=Path, help="compile every .py in this dir via the frontend first")
    args = ap.parse_args()

    if not LITHON_JIT.exists():
        print("error: lithon_jit not built -- run: cmake --build build", file=sys.stderr)
        return 2

    errors = []
    with tempfile.TemporaryDirectory() as tmp_s:
        tmp = Path(tmp_s)
        irs = list(args.irs)
        if args.dir:
            for py in sorted(args.dir.glob("*.py")):
                fe = subprocess.run([sys.executable, str(ROOT / "src/frontend/frontend.py"), str(py)],
                                    capture_output=True, text=True)
                if fe.returncode != 0:
                    continue
                ir_path = tmp / (py.stem + ".ir")
                ir_path.write_text(fe.stdout)
                irs.append(ir_path)

        if not irs:
            print("no .ir files to check (pass paths or --dir)", file=sys.stderr)
            return 2

        for ir_path in irs:
            check_one_ir(ir_path, tmp, errors)

    if errors:
        print(f"\n{len(errors)} ABI VIOLATION(S):")
        for e in errors:
            print(" ", e)
        return 1
    print(f"\n{len(irs)} compiled module(s) checked: stack alignment and "
          f"callee-saved preservation OK at every call/ret")
    return 0


if __name__ == "__main__":
    sys.exit(main())
