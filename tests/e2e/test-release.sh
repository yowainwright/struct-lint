#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
archive="$1"
temporary_dir=""

cleanup() {
  [ -z "$temporary_dir" ] && return
  rm -rf "$temporary_dir"
}

verify_licenses() {
  cmp "$repo_root/LICENSE" "$temporary_dir/LICENSE"
  for notice in "$repo_root"/LICENSES/*; do
    cmp "$notice" "$temporary_dir/LICENSES/$(basename "$notice")"
  done
}

verify_fixtures() {
  local binary="${1:?binary path required}"
  for fixture in readme/main.ts go/clean.go python/clean.py bash/clean.sh; do
    PATH=/nonexistent "$binary" --profile ci "$repo_root/tests/fixtures/$fixture"
  done
}

verify_options() {
  local binary="${1:?binary path required}"
  local version
  if [ -n "${SL_VERSION:-}" ]; then
    version="${SL_VERSION#v}"
  else
    local binary_version
    binary_version="$("$binary" --version)"
    version="${binary_version#struct-lint }"
  fi
  SL_VERSION="$version" "$repo_root/tests/e2e/test-options.sh" "$binary"
}

verify_cli() {
  local binary="${1:?binary path required}"
  "$repo_root/tests/e2e/test-cli.sh" "$binary"
  "$repo_root/tests/e2e/test-discovery.sh" "$binary"
  "$repo_root/tests/e2e/test-script-cli.sh" "$binary"
  "$repo_root/tests/e2e/test-embedded-cli.sh" "$binary"
}

main() {
  temporary_dir="$(mktemp -d "$repo_root/.build/release.XXXXXX")"
  trap cleanup EXIT
  tar -xzf "$archive" -C "$temporary_dir"
  local binary="$temporary_dir/struct-lint"
  verify_licenses
  verify_fixtures "$binary"
  verify_options "$binary"
  verify_cli "$binary"
}

main "$@"
