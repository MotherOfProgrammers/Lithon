# Contributing to Lithon(Boa)

Thank you for your interest in building the zero-dependency Python AOT compiler!

## How to Get Started

1. **Pick an Issue:** Browse open GitHub issues, specifically those tagged with `good first issue` or `compiler-backend`.
2. **Read the Architecture Specs:** Review the design contracts in `docs/WHITEPAPER.md` and active RFCs in `docs/rfcs/`.
3. **Run the Test Suite:** Ensure all 23/23 tests pass locally before starting work:
   ```bash
   python3 tools/run_regression.py
   python3 tools/run_typed_regression.py
