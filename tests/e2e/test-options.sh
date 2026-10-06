#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary="$1"
fixture="$repo_root/tests/fixtures/typescript/section-order.ts"

expected_version() {
  if [ -n "${SL_VERSION:-}" ]; then
    printf '%s\n' "${SL_VERSION#v}"
    return
  fi
  local build_dir
  build_dir="$(dirname "$binary")"
  cat "$build_dir/struct-lint-version.txt"
}

assert_version() {
  local version
  version="$(expected_version)"
  test "$(STRUCT_LINT_PROFILE=missing "$binary" --version)" = "struct-lint $version"
}

assert_help() {
  local help
  help="$(STRUCT_LINT_PROFILE=missing "$binary" --help)"
  [[ "$help" == 'usage: struct-lint '* ]]
  for option in --profile --format --no-ignore --help --version; do
    [[ "$help" == *"$option"* ]]
  done
}

assert_section_order() {
  local status=0
  local output
  output="$(STRUCT_LINT_PROFILE=missing "$binary" --profile ci "$fixture" 2>&1)" || status=$?
  test "$status" -eq 1
  test "$output" = "$fixture:2:1: error[section-order] imports must appear before public types"
}

assert_missing_profile_default() {
  local status=0
  STRUCT_LINT_PROFILE=missing "$binary" "$fixture" >/dev/null 2>&1 || status=$?
  test "$status" -eq 2
}

assert_invalid_profile() {
  local status=0
  "$binary" --profile missing "$fixture" >/dev/null 2>&1 || status=$?
  test "$status" -eq 2
}

main() {
  assert_version
  assert_help
  assert_section_order
  assert_missing_profile_default
  assert_invalid_profile
}

main "$@"
