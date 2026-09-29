# Contributing to Lithon

Thank you for contributing to Project-Lithon!

## Development Workflow

1. **Create a Feature Branch:** Always branch off `master` (`git checkout -b feat/your-feature`).
2. **Commit Guidelines:** Use clear commit messages (`feat: ...`, `fix: ...`, `perf: ...`).
3. **Run Benchmarks:** Ensure `./benchmark_fib` passes locally before submitting a PR.
4. **Code Owners:** PRs modifying `/src/tier1/` or infrastructure require review from `@MotherOfProgrammers`.

## Coding Standards

- C++20 for runtime components.
- Strict zero-allocation policies inside hot execution loops.
