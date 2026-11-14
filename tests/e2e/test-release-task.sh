#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
main() {
  setup_fixtures
  test_bumps
  test_remote_version
  test_rejections
  test_confirmation
  test_publish_failures
  test_arguments
  printf 'Release task checks passed.\n'
}

setup_fixtures() {
  mkdir -p "$repo_root/.build"
  temporary_dir="$(mktemp -d "$repo_root/.build/release-task.XXXXXX")"
  trap 'rm -rf "$temporary_dir"' EXIT
  export SL_TEST_ROOT="$temporary_dir/project with spaces"
  export SL_TEST_LOG="$temporary_dir/commands"
  mkdir -p "$SL_TEST_ROOT/scripts" "$temporary_dir/bin"
  cp "$repo_root/scripts/release.sh" "$SL_TEST_ROOT/scripts/"
  cp "$repo_root/tests/fixtures/release-task/release-task.sh" "$temporary_dir/bin/git"
  setup_steps
  chmod +x "$temporary_dir/bin/"* "$SL_TEST_ROOT/scripts/"*.sh
  export PATH="$temporary_dir/bin:$PATH"
}

setup_steps() {
  cat >"$temporary_dir/bin/gh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
printf 'gh %s\n' "$*" >>"$SL_TEST_LOG"
test "${SL_TEST_FAIL-}" != gh
printf '%s\n' "${SL_TEST_CI-completed:success}"
SH
  cat >"$temporary_dir/step" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
step="${0##*/}"
printf '%s %s\n' "$step" "${SL_VERSION-}" >>"$SL_TEST_LOG"
test "${SL_TEST_FAIL-}" != "$step"
SH
  cp "$temporary_dir/step" "$SL_TEST_ROOT/scripts/lint.sh"
  cp "$temporary_dir/step" "$SL_TEST_ROOT/scripts/check.sh"
}

test_bumps() {
  for bump in patch minor major; do
    case "$bump" in
      patch) version=v1.2.4 ;;
      minor) version=v1.3.0 ;;
      major) version=v2.0.0 ;;
    esac
    run_release "$bump" --dry-run
    grep -Fq "Release $version" "$temporary_dir/output"
    grep -Fq "check.sh $version" "$SL_TEST_LOG"
    assert_no_publish
  done
}

test_remote_version() {
  SL_TEST_LOCAL_TAG=v9.0.0 run_release patch --dry-run
  grep -Fq 'Release v1.2.4' "$temporary_dir/output"
  assert_no_publish
}

test_rejections() {
  SL_TEST_BRANCH=feature reject_release
  SL_TEST_BRANCH='' reject_release
  SL_TEST_DIRTY='?? file' reject_release
  SL_TEST_REMOTE=newer reject_release
  SL_TEST_TAGS='' reject_release
  SL_TEST_PREVIOUS=commit reject_release
  SL_TEST_CHANGE_AT=2 reject_release
  for failure in fetch remote-tags ancestor duplicate gh lint.sh check.sh; do
    SL_TEST_FAIL="$failure" reject_release
  done
  for ci in '' in_progress:success completed:failure completed:cancelled; do
    SL_TEST_CI="$ci" reject_release
  done
}

test_confirmation() {
  run_release patch <<<'no'
  assert_no_publish
  run_release patch </dev/null
  assert_no_publish
  run_release patch <<<'v1.2.4'
  grep -Fxq 'git tag -a v1.2.4 -m Release v1.2.4 commit' "$SL_TEST_LOG"
  grep -Fxq 'git -c push.followTags=false push origin refs/tags/v1.2.4:refs/tags/v1.2.4' "$SL_TEST_LOG"
}

test_publish_failures() {
  if SL_TEST_CHANGE_AT=3 run_release patch <<<'v1.2.4'; then exit 1; fi
  assert_no_publish
  if SL_TEST_FAIL=tag run_release patch <<<'v1.2.4'; then exit 1; fi
  if grep -q '^git -c push.followTags=false push' "$SL_TEST_LOG"; then exit 1; fi
  if SL_TEST_FAIL=push run_release patch <<<'v1.2.4'; then exit 1; fi
  grep -Fq 'local tag v1.2.4 remains' "$temporary_dir/output"
}

test_arguments() {
  if run_release patch --unknown; then exit 1; fi
  assert_no_publish
  if run_release invalid; then exit 1; fi
  assert_no_publish
}

reject_release() {
  if run_release patch --dry-run; then
    echo 'Release accepted an invalid state' >&2
    exit 1
  fi
  assert_no_publish
}

run_release() {
  : >"$SL_TEST_LOG"
  bash "$SL_TEST_ROOT/scripts/release.sh" "$@" >"$temporary_dir/output" 2>&1
}

assert_no_publish() {
  if grep -Eq '^git (tag -a|-c push.followTags=false push)' "$SL_TEST_LOG"; then
    echo 'Release published unexpectedly' >&2
    exit 1
  fi
}

main "$@"
