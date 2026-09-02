#!/usr/bin/env bash
set -euo pipefail

source_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
test_binary=${1:?expected absolute truetest_tests path}
test_root=$(mktemp -d "${TMPDIR:-/tmp}/truetest-observability-contract.XXXXXX")
trap 'rm -rf "$test_root"' EXIT

output="$test_root/evidence"
bash "$source_root/scripts/capture-observability-evidence.sh" \
    --output-dir "$output" \
    --test-binary "$test_binary" \
    --input "$source_root/tests/fixtures/observability_one_trade.csv" \
    > "$test_root/capture.log"

required_nonempty=(
    manifest.md effective-config.json input.sha256 binary.sha256 cmake-cache.sha256 signals.csv
    orders.csv risk-decisions.csv fills.csv trade-reconciliation.csv metrics.json
    semantic-result.csv test-results.txt bars.csv indicators.csv execution.csv positions.csv exits.csv
    completeness.csv artifacts.sha256 environment.txt toolchain.txt compiler.sha256 capture.argv.sh
)
for artifact in "${required_nonempty[@]}"; do
    test -s "$output/$artifact"
done
for artifact in git-status.txt working-tree.patch untracked.sha256; do
    test -f "$output/$artifact"
done

grep -q 'status: `EVIDENCE_CAPTURED_WITH_UNVERIFIED_LINKS`' "$output/manifest.md"
grep -q 'trace invariance: `PASS`' "$output/manifest.md"
grep -q 'scope: `TEST_COMPONENT_HARNESS`' "$output/manifest.md"
grep -q 'risk_per_rule_pass_sequence,UNVERIFIED' "$output/completeness.csv"
grep -q 'engine_orchestration,UNVERIFIED' "$output/completeness.csv"
grep -q 'production_strategy_predicate_emission,UNVERIFIED' "$output/completeness.csv"
grep -q 'trace_latency_allocation_effects,UNVERIFIED' "$output/completeness.csv"
grep -q 'risk_snapshot_inputs,VERIFIED_HARNESS_CONSTRUCTION' "$output/completeness.csv"
grep -q 'INDEPENDENT_ORACLE' "$output/signals.csv"
grep -q 'PRODUCTION_RETURN_VALUE' "$output/orders.csv"
grep -q 'HARNESS_ASSIGNED' "$output/orders.csv"
grep -q '^order_id,model,book_side,book_price,book_quantity,intended_price,reference_price,fill_price,slippage,' \
    "$output/execution.csv"
grep -q 'CONTROLLED_BOOK_PRE_SUBMIT_READBACK' "$output/execution.csv"
grep -q 'HARNESS_DERIVED' "$output/execution.csv"
grep -q 'VERIFIED_FILL_FIELDS_AND_INDEPENDENT_ACCOUNTING' \
    "$output/trade-reconciliation.csv"
grep -q 'report_final_equity_to_portfolio_cash,VERIFIED' "$output/completeness.csv"
grep -q 'protective_exit_precedence' "$output/signals.csv"
grep -q '"total_orders": 2' "$output/metrics.json"
grep -q '"total_fills": 2' "$output/metrics.json"
grep -q '"total_trades": 1' "$output/metrics.json"
grep -q '^trade,index,fill_id,order_id' "$output/semantic-result.csv"
grep -q '^summary,data_rows_rejected,' "$output/semantic-result.csv"
grep -q '^summary,exit_intents_armed,' "$output/semantic-result.csv"
grep -q '^equity_curve,' "$output/semantic-result.csv"
grep -Eq 'ALL[[:space:]]+1 TESTS PASSED' "$output/test-results.txt"
grep -q -- '--output-dir' "$output/capture.argv.sh"
grep -q -- '--test-binary' "$output/capture.argv.sh"
grep -q -- '--input' "$output/capture.argv.sh"

(cd "$output" && sha256sum -c artifacts.sha256)
cp "$output/manifest.md" "$test_root/manifest.md"
printf '\nTAMPERED\n' >> "$output/manifest.md"
if (cd "$output" && sha256sum -c artifacts.sha256 >/dev/null 2>&1); then
    echo "manifest tampering was not detected" >&2
    exit 1
fi
mv "$test_root/manifest.md" "$output/manifest.md"
(cd "$output" && sha256sum -c artifacts.sha256 >/dev/null)

if bash "$source_root/scripts/capture-observability-evidence.sh" \
    --output-dir "$output" --test-binary "$test_binary" \
    --input "$source_root/tests/fixtures/observability_one_trade.csv" \
    >/dev/null 2>&1; then
    echo "existing evidence output was overwritten" >&2
    exit 1
fi

forbidden_parent="$source_root/.observability-contract-$BASHPID"
forbidden_output="$forbidden_parent/nested/evidence"
if bash "$source_root/scripts/capture-observability-evidence.sh" \
    --output-dir "$forbidden_output" --test-binary "$test_binary" \
    --input "$source_root/tests/fixtures/observability_one_trade.csv" \
    >/dev/null 2>&1; then
    echo "source-tree evidence output was accepted" >&2
    exit 1
fi
test ! -e "$forbidden_parent"
