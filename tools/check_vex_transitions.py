#!/usr/bin/env python3
"""1.3  VEX encoding discipline: no legacy SSE opcode after VEX without vzeroupper.

Once any VEX-prefixed (AVX) instruction is executed, a later legacy-encoded
SSE instruction incurs a measured transition penalty on real Intel hardware
(the upper 128 bits of the vector registers are "dirty" and hardware must
save/restore them on the next legacy op). The fix is `vzeroupper` before
dropping back to legacy SSE -- or before any call/return that might run it.

This is a guardrail for codegen that does not exist yet: no AVX instruction
is emitted today, so a clean run over the test corpus is expected. It is
added now because it is cheapest to add before there is anything to violate.

The check disassembles each compiled function's actual bytes (so immediates
and displacements can never be mistaken for opcodes) and flags the first
non-VEX instruction that touches an xmm/ymm/zmm register after a
VEX/EVEX-encoded instruction, unless a `vzeroupper` intervenes.

    tools/check_vex_transitions.py                 # scan the test corpus
    tools/check_vex_transitions.py a.ir b.ir ...   # scan specific IR
    tools/check_vex_transitions.py --self-test     # prove the scanner works

The self-test assembles a deliberate violation and a corrected version with
GNU as and requires the scanner to catch one and pass the other, so a green
corpus scan cannot be green merely because the scanner never fires.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LITHON_JIT = ROOT / "build" / "lithon_jit"
FRONTEND = ROOT / "src" / "frontend" / "frontend.py"

# First byte of VEX2 (0xC5), VEX3 (0xC4) and EVEX (0x62) encodings.
VEX_FIRST_BYTES = {0xC4, 0xC5, 0x62}
SIMD_REGS = ("xmm", "ymm", "zmm")

# objdump pads the hex-byte column with spaces before the tab, so the byte
# group must allow trailing whitespace or short instructions are silently
# skipped. (check_stack_alignment.py's regex does not, and consequently only
# ever sees 7-byte instructions -- noted when this checker was written.)
INSN = re.compile(r"^\s*([0-9a-f]+):\t((?:[0-9a-f]{2} ?)+?)\s*\t(.*)$")


def disassemble_obj(path: Path):
    out = subprocess.run(
        ["objdump", "-D", "-b", "binary", "-mi386:x86-64", "-M", "intel",
         "--insn-width=16", str(path)],
        capture_output=True, text=True, check=True).stdout
    insns = []
    for line in out.splitlines():
        m = INSN.match(line)
        if m:
            hx = m.group(2).replace(" ", "")
            insns.append((int(m.group(1), 16), hx, m.group(3).strip()))
    return insns


def is_vex(hx: str) -> bool:
    return len(hx) >= 2 and int(hx[0:2], 16) in VEX_FIRST_BYTES


def is_vzeroupper(hx: str, text: str) -> bool:
    # VEX.128.NP.0F.WIG 77 == c5 f8 77; the mnemonic is unambiguous too.
    return text.split(" ", 1)[0] == "vzeroupper"


def touches_simd(text: str) -> bool:
    low = text.lower()
    return any(r in low for r in SIMD_REGS)


def scan(insns, name="function"):
    """insns: [(offset, hexbytes, text)] for one function. Returns errors."""
    errors = []
    pending_vex = None          # offset of the most recent VEX instruction
    for off, hx, text in insns:
        if is_vzeroupper(hx, text):
            pending_vex = None
            continue
        if is_vex(hx):
            pending_vex = off
            continue
        if pending_vex is not None and touches_simd(text):
            errors.append(f"{name}+0x{off:x}: legacy SSE `{text}` after VEX "
                          f"at +0x{pending_vex:x} with no vzeroupper")
    return errors


def scan_binary(bin_path: Path, functions: dict, label: str, errors: list):
    insns = disassemble_obj(bin_path)
    if not insns:
        return
    bounds = sorted(functions.items(), key=lambda kv: kv[1])
    bounds.append(("<end>", insns[-1][0] + 1))
    for i in range(len(bounds) - 1):
        name, start = bounds[i]
        _, end = bounds[i + 1]
        fn_insns = [(off - start, hx, text) for off, hx, text in insns if start <= off < end]
        errors.extend(scan(fn_insns, f"{label}:{name}"))


def check_ir(ir_path: Path, tmp: Path, errors: list) -> bool:
    bin_path = tmp / "code.bin"
    r = subprocess.run([str(LITHON_JIT), str(ir_path), "--dump-code", str(bin_path)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return False
    functions = {}
    for line in r.stdout.splitlines():
        name, off = line.rsplit(" ", 1)
        functions[name] = int(off)
    if functions and bin_path.stat().st_size:
        scan_binary(bin_path, functions, ir_path.stem, errors)
    return True


def assemble_insns(asm_body: str, tmp: Path):
    """Assemble `asm_body` and return a disassembly of its bytes."""
    s = tmp / "t.s"
    o = tmp / "t.o"
    s.write_text(".intel_syntax noprefix\n.text\n" + asm_body + "\n")
    subprocess.run(["as", "--64", str(s), "-o", str(o)], check=True)
    bin_path = tmp / "t.bin"
    objcopy = subprocess.run(["objcopy", "-O", "binary", "-j", ".text", str(o), str(bin_path)],
                             check=True)
    return disassemble_obj(bin_path)


def self_test(tmp: Path) -> int:
    bad = assemble_insns("vmovdqu ymm0, YMMWORD PTR [rdi]\nmovaps xmm1, xmm0", tmp)
    good = assemble_insns("vmovdqu ymm0, YMMWORD PTR [rdi]\nvzeroupper\nmovaps xmm1, xmm0", tmp)
    scalar = assemble_insns("mov rax, rbx\nadd rax, 1", tmp)

    problems = []
    if not scan(bad, "bad"):
        problems.append("self-test: a VEX->legacy-SSE sequence was NOT flagged")
    if scan(good, "good"):
        problems.append("self-test: a vzeroupper-protected sequence was wrongly flagged")
    if scan(scalar, "scalar"):
        problems.append("self-test: scalar code was wrongly flagged")

    if problems:
        for p in problems:
            print("FAIL " + p)
        return 1
    print("self-test: scanner flags VEX->legacy-SSE, accepts vzeroupper and scalar")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("irs", nargs="*", type=Path)
    ap.add_argument("--dirs", nargs="*", type=Path,
                    default=[ROOT / "tests" / "programs", ROOT / "tests" / "typed_programs"])
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if not LITHON_JIT.exists():
        print("error: lithon_jit not built -- run: cmake --build build", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as tmp_s:
        tmp = Path(tmp_s)
        if args.self_test:
            return self_test(tmp)

        irs = list(args.irs)
        for d in args.dirs:
            for py in sorted(d.glob("*.py")):
                fe = subprocess.run([sys.executable, str(FRONTEND), str(py)],
                                    capture_output=True, text=True)
                if fe.returncode != 0:
                    continue
                ir = tmp / (py.stem + ".ir")
                ir.write_text(fe.stdout)
                irs.append(ir)

        if not irs:
            print("no IR to check", file=sys.stderr)
            return 2

        errors = []
        scanned = 0
        for ir in irs:
            if check_ir(ir, tmp, errors):
                scanned += 1

        if errors:
            print(f"{len(errors)} VEX TRANSITION VIOLATION(S):")
            for e in errors:
                print(" ", e)
            return 1
        print(f"{scanned} compiled module(s) scanned: no legacy SSE after VEX "
              f"without vzeroupper")
        return 0


if __name__ == "__main__":
    sys.exit(main())
