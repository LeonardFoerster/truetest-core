# Launcher scripts

Only two thin convenience wrappers are supported. They do not load `.env`, infer live mode, or select a live binary.

## Desk

```bash
./launch-desk.sh [engine_shadow arguments]
```

The script configures/builds the `linux-dev` preset and executes `engine_shadow --mode shadow --desk`. It rejects all `--mode` overrides, `--live`, credential flags, and `TT_MODE=live`. Use `--print-command` to inspect without executing and `--skip-build` to reuse the configured tree.

## Tests

```bash
./launch-test.sh
./launch-test.sh --filter 'DataBridge*:*LiveSafetySession*'
```

The script configures/builds the `linux-tests` preset serially and executes the `truetest_tests` GoogleTest binary. `--print-command` prints the command; `--skip-build` reuses the tree. Use the canonical serial `ctest --preset linux-tests` command when the complete CTest matrix is required.

## Removed wrappers

`launch-default.sh`, `launch-bench.sh`, `launch-live.sh`, and `launch-helpers.sh` were removed because they selected inconsistent presets/targets and could make an inert helper look like an approved live ritual. For benchmarks use the `linux-benchmarks` commands in `AGENTS.md`. For live operation use the explicit, attended procedure in [`../governance/01-prod.md`](../governance/01-prod.md); there is deliberately no live launcher.
