# QuestDB 45-Minute Soak Test Guide

This guide explains how to build the engine with QuestDB support, install and run QuestDB, and execute a realistic 45-minute soak test with periodic network blips. This is the recommended way to validate the full hardened QuestDB persistence stack (Phases 0-5) for serious long-running shadow or live campaigns.

## Prerequisites

- Ubuntu 22.04+ / Debian 12+ (or equivalent Linux)
- At least 8 GB RAM, 4 CPU cores recommended
- Docker (for easiest QuestDB setup)
- Build tools: `cmake`, `ninja-build` or `make`, `g++` (GCC 11+), `git`
- Python 3.10+

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git python3 python3-pip docker.io
sudo usermod -aG docker $USER   # log out and back in after this
pip3 install requests
```

## 1. Clone and Checkout the Branch

```bash
git clone <your-repo-url> truetest-core
cd truetest-core
git checkout monte-carlo          # (historical example from before merge; current mainline includes the work)
```

## 2. Build the Engine with QuestDB Support

```bash
# Configure
cmake -S . -B out/build/linux-default \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_QUESTDB=ON \
  -G Ninja

# Build (this will take a few minutes)
cmake --build out/build/linux-default -j $(nproc) --target engine_shadow
```

The resulting binary will be at:
`out/build/linux-default/engine_shadow`

You can also build `engine_live` if you want to test against real exchange data.

## 3. Install and Launch QuestDB

### Recommended: Docker (fastest & cleanest)

```bash
# Pull and run QuestDB (exposes HTTP on 9000, ILP on 9009)
docker run -d --rm \
  --name questdb \
  -p 9000:9000 \
  -p 9009:9009 \
  -v questdb-data:/var/lib/questdb \
  questdb/questdb:latest

# Wait ~10-15 seconds for it to become ready
sleep 15

# Verify it's up
curl -s http://localhost:9000/exec?query=SELECT%201 | head -c 200
```

You should see JSON output containing `"dataset"`.

### Alternative: Native install (advanced)

Follow the official QuestDB installation instructions for your OS and start it with:

```bash
questdb start
```

## 4. Prepare Your Run Tag and Environment

Choose a descriptive run tag:

```bash
RUN_TAG="soak_$(date +%Y%m%d_%H%M)"
echo "Using run tag: $RUN_TAG"
```

Make sure the helper scripts are executable:

```bash
chmod +x scripts/questdb_*.py
```

## 5. Start the 45-Minute Soak Test

Run the soak test with realistic blip injection (simulates network instability):

```bash
python3 scripts/questdb_soak_test.py \
  --run-tag "$RUN_TAG" \
  --duration-minutes 45 \
  --blip-every-minutes 5 \
  --blip-duration-seconds 45 \
  --rows-per-second 40 \
  --host 127.0.0.1 \
  --ilp-port 9009
```

**Recommended flags explanation**:
- `--duration-minutes 45` — the target length of the test
- `--blip-every-minutes 5` — inject a failure window every 5 minutes
- `--blip-duration-seconds 45` — each blip lasts 45 seconds (tests fallback + recovery)
- `--rows-per-second 40` — moderate load (adjust based on your hardware)

The script will periodically print progress and blip status.

## 6. Monitor During the Run (Optional but Recommended)

In another terminal you can watch the health surface live if you also run the engine with a TUI, or poll the health check script:

```bash
watch -n 30 'python3 scripts/questdb_health_check.py \
  --run-tag "$RUN_TAG" \
  --recent-minutes 2'
```

## 7. Post-Run Verification (Mandatory Operator Ritual)

Once the 45-minute run finishes, run the full verification suite:

```bash
echo "=== Post-run verification for $RUN_TAG ==="

python3 scripts/questdb_health_check.py \
  --run-tag "$RUN_TAG" \
  --recent-minutes 10 \
  --require-activity

python3 scripts/questdb_campaign_summary.py \
  --run-tag "$RUN_TAG"

python3 scripts/questdb_verify_reconciliation.py \
  --run-tag "$RUN_TAG"
```

**Manual steps you must still perform**:
1. Check whether a fallback file was created: `ls -l ${RUN_TAG}.questdb_fallback.ilp`
2. Compare order/event counts from the binary log (`.ttlog.zst`) against QuestDB.
3. Document any anomalies.
4. Sign the results in your evidence bundle / campaign report.

## 8. Cleanup

```bash
docker stop questdb || true
docker rm questdb || true
```

## Tips for a High-Quality 45-Minute Test

- Run on real hardware (not a heavily loaded VM).
- Use `--persist-strict` when launching the real engine (not just the Python simulator) for the most realistic test.
- Combine with `--record` so you have the binary log for reconciliation.
- Consider running with a real data feed or long replay file instead of the synthetic Python sender for even better fidelity.
- Take screenshots of the TUI health panel during and after blips.

## Troubleshooting

- **"Connection refused"**: QuestDB is not running or ports are not forwarded.
- **Script exits immediately**: Make sure you passed `--duration-minutes`.
- **High CPU during soak**: Lower `--rows-per-second`.
- **Want to test with the real engine**: Use the built `engine_shadow` binary with `--persist --persist-strict --run-tag $RUN_TAG ...`

---

This guide + the scripts in `scripts/questdb_*.py` give you a repeatable, auditable way to validate long-running QuestDB persistence.
