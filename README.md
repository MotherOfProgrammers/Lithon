<div align="center">
<h1 align="center">Lithon</h1>
    <img src="assets/boa.png" alt="BOA" width="400" />

### Give Python wings.

![Status](https://img.shields.io/badge/status-early%20design-orange)
![Language](https://img.shields.io/badge/syntax-Python--like-blue)
![Typing](https://img.shields.io/badge/typing-mandatory%20static-green)
![Target](https://img.shields.io/badge/performance-C%2FRust--class-red)
![License](https://img.shields.io/badge/license-TBD-lightgrey)

</div>

---

## 🚀 What is Lithon?

Lithon is a **Python-flavored language built for speed from the
ground up.**

It looks like Python. It reads like Python. But under the hood, it
makes a different promise: **every value has a known, fixed type,
decided before the program ever runs** — no guessing, no dynamic
dispatch, no boxed objects hiding behind every number and string.

That one trade — explicit types instead of Python's "figure it out
at runtime" flexibility — is what lets Lithon compile straight down
to native machine code that runs in the same performance class as
C, C++, and Rust.

Python gave the world a language that's a joy to write.
Lithon is what happens when that same syntax is asked to run like
it was written in C. 🔥

---

## 💡 Why Lithon?

Python is loved for being readable, expressive, and fast to write.
It is not loved for being fast to *run*. Every number is a heap
object. Every `+` is a runtime decision. Every function call pays a
tax just for being dynamic.

A lot of tools try to fix this by bolting speed onto Python after
the fact — JIT warmup tricks, C extensions, "just rewrite the hot
part in Rust." Lithon asks a more direct question:

> **What if the language itself never allowed the slow path to
> exist in the first place?**

No inference guessing your intent. No silent conversions hiding
bugs. No interpreter overhead on the critical path. If Lithon can't
*prove* something is safe and fast, it won't compile — instead of
quietly running slow, or worse, running wrong.

---

## 🆚 How is this different?

| | Python | Cython / mypyc | Lithon |
|---|---|---|---|
| Syntax | Python | Python + annotations | Python-like |
| Typing | Dynamic | Optional | **Mandatory** |
| Speed on typed code | 🐢 | 🚗 | 🚀 |
| Silent type coercion | ✅ allowed | ⚠️ partial | ❌ never |
| Runtime type guessing | ✅ | ⚠️ partial | ❌ never |
| Compiles to | Bytecode | C, then native | Native machine code |
| Philosophy | "It'll figure itself out" | "Speed up what you annotate" | "Prove it, or it doesn't run" |

Lithon isn't trying to run your existing `.py` files unmodified.
It's a stricter, sharper language that happens to speak Python's
dialect — built for the code you'd *want* to be fast, not the code
you happen to already have. ✍️➡️⚙️

---

## 🎯 Use cases

Lithon is aimed at the code that Python traditionally hands off to
something else the moment performance matters:

- 🔢 **Numeric-heavy loops** — simulations, signal processing, tight
  arithmetic that Python usually farms out to NumPy or C extensions
- 🎮 **Performance-sensitive tooling** — game logic, real-time data
  processing, anything where interpreter overhead is the bottleneck
- 📊 **Data pipelines with predictable shapes** — fixed schemas,
  known types, no need for Python's full dynamic flexibility
- 🧪 **Learning how compilers actually work** — a small, honest,
  from-scratch language for exploring static typing and native
  codegen without wading into a massive existing compiler codebase
- 🏗️ **Anywhere "Python, but it has to be fast and correct" matters
  more than "Python, but it has to run every existing library"**

---

## 🧭 Where this is headed

Lithon trades Python's dynamic freedom for compile-time certainty —
on purpose. The bet is that a language willing to say **"no, that's
not safe, fix it"** at compile time can hand back speed that a fully
dynamic language structurally can't reach, without asking the
programmer to leave Python's syntax behind.

Still early. Still opinionated. Still built to prove that giving
Python real wings means being honest about what has to change to
get there. 🪽
