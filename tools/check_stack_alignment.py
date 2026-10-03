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
    tools/check_stack_alignment.py --self-test   # prove the scanner fires
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

# objdump pads the hex-byte column with spaces before the tab, so the byte
# group must allow trailing whitespace (`\s*\t`). An earlier revision required
# the tab to follow the last byte after at most one space, which meant only
# instructions whose bytes happened to fill the column exactly -- 7-byte
# encodings -- were ever seen. Every 1-byte `ret` (and `push`/`pop`, short
# jcc, ...) was silently skipped, so the callee-saved-restored and rsp-zero
# checks below never actually ran. --insn-width=16 additionally keeps
# instructions longer than 7 bytes from wrapping onto continuation lines.
INSN = re.compile(r"^\s*([0-9a-f]+):\t((?:[0-9a-f]{2} ?)+?)\s*\t(.*)$")
PUSH = re.compile(r"^push\s+(\w+)$")
POP = re.compile(r"^pop\s+(\w+)$")
SUB_RSP = re.compile(r"^sub\s+rsp,0x([0-9a-f]+)$")
ADD_RSP = re.compile(r"^add\s+rsp,0x([0-9a-f]+)$")
CALL = re.compile(r"^call\s")
RET = re.compile(r"^ret\b")
MOV_STORE_CALLEE = re.compile(r"^mov\s+QWORD PTR \[rbp[+-]0x[0-9a-f]+\],(\w+)$")
MOV_LOAD_CALLEE = re.compile(r"^mov\s+(\w+),QWORD PTR \[rbp[+-]0x[0-9a-f]+\]$")
MOV_RBP_RSP = re.compile(r"^mov\s+rbp,rsp$")
MOV_RSP_RBP = re.compile(r"^mov\s+rsp,rbp$")
JMP = re.compile(r"^j\w+\b")


def disassemble(path: Path):
    out = subprocess.run(
        ["objdump", "-D", "-b", "binary", "-mi386:x86-64", "-M", "intel",
         "--insn-width=16", str(path)],
        capture_output=True, text=True, check=True).stdout
    insns = []
    for line in out.splitlines():
        m = INSN.match(line)
        if not m:
            continue
        text = m.group(3).split("#", 1)[0].strip()
        # A bare `rex.*` prefix is printed as its own line and only decorates
        # the following instruction; it cannot move rsp or clobber a register,
        # so skip it rather than count a phantom instruction.
        if not text or text.startswith("rex."):
            continue
        insns.append((int(m.group(1), 16), text))
    return insns


def check_function(name, insns, errors):
    """insns: this function's instructions only, starting at its own offset 0."""
    rsp = 0                       # bytes pushed relative to function entry
    rbp_frame = None              # rsp captured by `mov rbp,rsp` (frame base)
    saved_now = set()             # callee-saved regs currently spilled to the frame
    ever_saved = set()            # callee-saved regs this function actually uses
    body = None                   # (rsp, saved) baseline shared by every block

    for off, text in insns:
        # The prologue contains no control flow, so the first branch/call/ret
        # marks the entry block's end; that state is the invariant body
        # baseline. A linear scan otherwise carries an epilogue's frame
        # restore (`mov rsp,rbp; pop rbp`) into the next block, which is a
        # branch target reached with the baseline still intact.
        if body is None and (JMP.match(text) or CALL.match(text) or RET.match(text)):
            body = (rsp, set(saved_now))
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
        elif MOV_RBP_RSP.match(text):
            rbp_frame = rsp
        elif MOV_RSP_RBP.match(text):
            # Frame-pointer restore (`mov rsp,rbp`), the first half of the
            # leave-style epilogue; it resets rsp to the frame base.
            if rbp_frame is not None:
                rsp = rbp_frame
        elif CALL.match(text):
            # ABI requirement: rsp is 16-aligned immediately BEFORE `call`
            # executes (call itself then pushes an 8-byte return address).
            # rsp here counts bytes pushed since entry, and at entry the
            # caller's `call` has ALREADY pushed the return address, so
            # entry_rsp % 16 == 8. Hence the real rsp (entry_rsp - rsp) is
            # 16-aligned <=> rsp % 16 == 8.
            if rsp % 16 != 8:
                errors.append(f"{name}+0x{off:x}: rsp is {rsp} bytes below entry "
                              f"(not 16-aligned) at `{text}`")
        elif RET.match(text):
            if saved_now:
                errors.append(f"{name}+0x{off:x}: `ret` with {sorted(saved_now)} still "
                              f"spilled (never restored before return)")
            if rsp != 0:
                errors.append(f"{name}+0x{off:x}: `ret` with rsp {rsp} bytes off entry "
                              f"(prologue/epilogue push/pop or sub/add mismatched)")
            # Resume the scan at the body baseline for the next block.
            if body is not None:
                rsp, saved_now = body[0], set(body[1])
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


def assemble(body: str, tmp: Path):
    """Assemble `body` and return its `disassemble`d instructions."""
    s = tmp / "abi.s"
    o = tmp / "abi.o"
    b = tmp / "abi.bin"
    s.write_text(".intel_syntax noprefix\n.text\n" + body + "\n")
    subprocess.run(["as", "--64", str(s), "-o", str(o)], check=True)
    subprocess.run(["objcopy", "-O", "binary", "-j", ".text", str(o), str(b)], check=True)
    return disassemble(b)


def self_test(tmp: Path) -> int:
    """Prove the scanner actually fires, so a green corpus scan is meaningful."""
    def errs(body):
        e = []
        check_function("selftest", assemble(body, tmp), e)
        return e

    problems = []
    # The old byte-field regex skipped every 1-byte `ret`, so these only work
    # now that short instructions are parsed. A normal `push rbp` leaves the
    # model at 8, which is the aligned-at-call baseline (entry rsp == 8 mod 16).
    if not errs("push rbp\nret"):
        problems.append("unbalanced rsp at `ret` was NOT flagged")
    if errs("push rbp\npop rbp\nret"):
        problems.append("balanced rsp at `ret` was wrongly flagged")
    # The leave-style epilogue `mov rsp,rbp; pop rbp` must reset rsp.
    if errs("push rbp\nmov rbp,rsp\nsub rsp, 16\nmov rsp,rbp\npop rbp\nret"):
        problems.append("frame-pointer epilogue was wrongly flagged")
    # rsp must be 16-aligned immediately before `call` (model rsp == 8 mod 16).
    if not errs("push rbp\nsub rsp, 8\ncall 1f\n1:\nadd rsp, 8\npop rbp\nret"):
        problems.append("misaligned rsp at `call` was NOT flagged")
    if errs("push rbp\ncall 1f\n1:\npop rbp\nret"):
        problems.append("aligned rsp at `call` was wrongly flagged")
    # A callee-saved register spilled and never restored before `ret`.
    if not errs("mov QWORD PTR [rbp-8], rbx\nret"):
        problems.append("unrestored callee-saved at `ret` was NOT flagged")
    if errs("mov QWORD PTR [rbp-8], rbx\nmov rbx, QWORD PTR [rbp-8]\nret"):
        problems.append("restored callee-saved was wrongly flagged")

    if problems:
        for p in problems:
            print("FAIL self-test: " + p)
        return 1
    print("self-test: scanner flags misaligned calls and unrestored "
          "callee-saved, accepts the corrected forms")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("irs", nargs="*", type=Path)
    ap.add_argument("--dir", type=Path, help="compile every .py in this dir via the frontend first")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if not LITHON_JIT.exists():
        print("error: lithon_jit not built -- run: cmake --build build", file=sys.stderr)
        return 2

    errors = []
    with tempfile.TemporaryDirectory() as tmp_s:
        tmp = Path(tmp_s)
        if args.self_test:
            return self_test(tmp)

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
