#!/usr/bin/env python3
"""Cross-check the JIT's x86 encoder against GNU as.

Every instruction form x every register combination is assembled by `as`; the
encoder's bytes are compared with as's. Where the bytes differ we do NOT just
wave it through: the encoder's bytes are re-assembled as raw `.byte` data,
disassembled by objdump, and the two disassemblies must be identical. That
accepts legitimate encoding choices (e.g. as prefers imm8 forms and omits a
REX prefix the encoder always emits) but rejects any real difference."""
import pathlib, re, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
INSN = re.compile(r"\s*[0-9a-f]+:\t((?:[0-9a-f]{2} )+)\s*(?:\t(.*))?$")


def disassemble(tmp, name, asm_text):
    s, o = tmp / f"{name}.s", tmp / f"{name}.o"
    s.write_text(asm_text)
    r = subprocess.run(["as", "--64", str(s), "-o", str(o)], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:2000]); sys.exit(2)
    dis = subprocess.run(["objdump", "-d", "-M", "intel", "--insn-width=16", str(o)],
                         capture_output=True, text=True, check=True).stdout
    out = []
    for l in dis.splitlines():
        m = INSN.match(l)
        if m:
            out.append((m.group(1).replace(" ", ""), (m.group(2) or "").strip()))
    return out


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        exe = tmp / "dump"
        subprocess.run(["g++", "-std=c++20", f"-I{ROOT/'src'/'jit'}", "-o", str(exe),
                        str(ROOT/"src"/"jit"/"encoder_asm_dump.cpp")], check=True)
        lines = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout.splitlines()
        cases = [l.rsplit("|", 1) for l in lines]
        head = ".intel_syntax noprefix\n.text\n"

        theirs = disassemble(tmp, "as", head + "\n".join(t for t, _ in cases) + "\n")
        # one instruction per case, so each case's raw bytes decode independently
        mine_asm = head + "\n".join(".byte " + ",".join("0x" + b[i:i+2] for i in range(0, len(b), 2))
                                     for _, b in cases) + "\n"
        mine = disassemble(tmp, "enc", mine_asm)
        if len(theirs) != len(cases) or len(mine) != len(cases):
            print(f"instruction count mismatch: cases={len(cases)} as={len(theirs)} encoder={len(mine)}")
            return 2

        exact = equivalent = bad = 0
        for (text, enc_hex), (as_hex, as_txt), (_, enc_txt) in zip(cases, theirs, mine):
            # A bare 0x40 REX (no W/R/X/B bits) is a no-op for al/cl/dl/bl. The encoder
            # always emits it for setcc because it is REQUIRED for spl/bpl/sil/dil, where
            # as emits it too and the bytes match exactly.
            if enc_hex.startswith("40") and enc_txt.startswith("rex "):
                enc_txt = enc_txt[4:]
            enc_txt, as_txt = " ".join(enc_txt.split()), " ".join(as_txt.split())
            if enc_hex == as_hex:
                exact += 1
            elif enc_txt == as_txt:
                equivalent += 1
            else:
                bad += 1
                if bad <= 20:
                    print(f"MISMATCH  {text:<36} encoder={enc_hex} ({enc_txt})  as={as_hex} ({as_txt})")
        n = len(cases)
        print(f"{n} cases: {exact} byte-identical to GNU as, {equivalent} different encoding / same instruction, {bad} WRONG")
        return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
