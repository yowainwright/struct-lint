#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary="$1"
temporary_dir="$(mktemp -d "$repo_root/.build/script-cli.XXXXXX")"
trap 'rm -rf "$temporary_dir"' EXIT

for extension in js jsx mjs cjs ts tsx mts cts; do
  printf '%s\n' 'function helper() { return 1; }' \
    'function main() { return helper(); }' >"$temporary_dir/main.$extension"
done
for extension in d.ts d.mts d.cts; do
  printf '%s\n' 'export declare function main(): void;' >"$temporary_dir/main.$extension"
done
for extension in js jsx tsx; do
  printf '%s\n' 'export function main() { return <div />; }' >"$temporary_dir/view.$extension"
  PATH=/nonexistent "$binary" --profile ci "$temporary_dir/view.$extension"
done

status=0
output="$("$binary" --profile ci --no-ignore "$temporary_dir" "$temporary_dir/main.js" 2>&1)" || status=$?
test "$status" -eq 1
test "$(wc -l <<<"$output" | tr -d ' ')" -eq 8
for extension in js jsx mjs cjs ts tsx mts cts; do
  expected="$temporary_dir/main.$extension:1:1: error[function-order] helper must appear below caller main"
  [[ "$output" == *"$expected"* ]]
done

printf '%s\n' 'function main( {' >"$temporary_dir/invalid.js"
status=0
output="$("$binary" --profile local "$temporary_dir/invalid.js" 2>&1)" || status=$?
test "$status" -eq 1
[[ "$output" == *'error[parse-error] could not parse JavaScript source' ]]

for extension in js jsx mjs cjs ts tsx mts cts; do
  printf '%s\n' 'export function api() { return 1; }' \
    'function helper() { return api(); }' >"$temporary_dir/public-call.$extension"
  "$binary" --profile ci "$temporary_dir/public-call.$extension"
done
