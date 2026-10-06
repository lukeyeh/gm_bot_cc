#!/usr/bin/env bash
# Runs every benchmark in the repository, optimised, and saves the output of
# each test binary under perf-reports/raw/<label>/. Run inside `nix develop`.
#
#   perf/benchmarks.sh baseline
#   perf/benchmarks.sh after "//gm:ledger_test //gm:phrase_test"
set -euo pipefail
label=${1:?usage: run_benchmarks.sh <label> [targets]}
targets=${2:-$(bazel query 'kind(cc_test, //...) except attr(name, "_fuzz_test$", //...)' 2>/dev/null)}
out=perf-reports/raw/$label
mkdir -p "$out"
bazel build -c opt $targets >/dev/null 2>&1
for target in $targets; do
  binary=bazel-bin/${target#//}
  binary=${binary/://}
  name=${target#//}
  name=${name//[:\/]/_}
  # Tests that keep files use TEST_TMPDIR; give them a memory-backed one, as
  # well as a second run on real disk where it matters (see the report).
  TEST_TMPDIR=$(mktemp -d) "$binary" --benchmark_filter=all \
    --benchmark_min_time=0.3s 2>/dev/null | grep -E "^BM_|^-----|^Benchmark" > "$out/$name.txt" || true
  echo "== $target"; grep -E "^BM_" "$out/$name.txt" || true
done
