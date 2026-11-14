#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stable_tag='^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'

main() {
  cd "$repo_root"
  parse_arguments "$@"
  check_worktree
  commit="$(git rev-parse HEAD)"
  remote_url="$(git remote get-url origin)"
  fetch_main
  latest="$(latest_version)"
  git merge-base --is-ancestor "$latest" "$commit"
  local previous_commit
  previous_commit="$(git rev-parse "$latest^{commit}")"
  [ "$previous_commit" != "$commit" ] || fail 'HEAD is already released.'
  tag="$(next_version)"
  check_tag
  check_ci
  ./scripts/lint.sh
  SL_VERSION="$tag" ./scripts/check.sh
  check_release
  confirm_release
}

parse_arguments() {
  bump="${1:-}"
  dry_run=''
  case "$bump" in
    patch | minor | major) ;;
    *) fail 'Usage: scripts/release.sh patch|minor|major [--dry-run]' ;;
  esac
  shift
  if [ "${1:-}" = --dry-run ]; then
    dry_run=--dry-run
    shift
  fi
  [ "$#" -eq 0 ] || fail 'Unexpected release arguments.'
}

next_version() {
  [[ "$latest" =~ $stable_tag ]] || fail 'Invalid stable release tag.'
  local major="${BASH_REMATCH[1]}" minor="${BASH_REMATCH[2]}" patch="${BASH_REMATCH[3]}"
  case "$bump" in
    patch) patch=$((patch + 1)) ;;
    minor)
      minor=$((minor + 1))
      patch=0
      ;;
    major)
      major=$((major + 1))
      minor=0
      patch=0
      ;;
  esac
  printf 'v%s.%s.%s\n' "$major" "$minor" "$patch"
}

confirm_release() {
  printf 'Release %s\nCommit: %s\nMessage: Release %s\n' "$tag" "$commit" "$tag"
  if [ -n "$dry_run" ]; then
    printf 'Dry run complete. No tag created or pushed.\n'
    return
  fi
  local answer
  printf 'Type %s to create and push the tag: ' "$tag"
  IFS= read -r answer || answer=''
  if [ "$answer" != "$tag" ]; then
    printf '\nRelease cancelled.\n'
    return
  fi
  publish_release
}

publish_release() {
  check_release
  git tag -a "$tag" -m "Release $tag" "$commit"
  if git -c push.followTags=false push origin "refs/tags/$tag:refs/tags/$tag"; then
    printf 'Pushed %s. The Release workflow will publish the archives and update Homebrew.\n' "$tag"
    return
  fi
  fail "Push failed; local tag $tag remains. Inspect origin before retrying its push."
}

check_release() {
  check_worktree
  local current_commit
  current_commit="$(git rev-parse HEAD)"
  [ "$commit" = "$current_commit" ] || fail 'HEAD changed during release.'
  fetch_main
  check_tag
  local current_latest
  current_latest="$(latest_version)"
  [ "$latest" = "$current_latest" ] || fail 'Release tags changed. Run the command again.'
  check_ci
}

latest_version() {
  local tags _object ref tag
  tags="$(git ls-remote --tags --refs --sort=-version:refname origin 'v[0-9]*')" ||
    fail 'Unable to read origin tags.'
  while read -r _object ref; do
    tag="${ref#refs/tags/}"
    if [[ "$tag" =~ $stable_tag ]]; then
      printf '%s\n' "$tag"
      return
    fi
  done <<<"$tags"
  fail 'No stable vMAJOR.MINOR.PATCH tag found on origin.'
}

check_worktree() {
  local branch dirty
  branch="$(git symbolic-ref --quiet --short HEAD)"
  [ "$branch" = main ] || fail 'Release requires the main branch.'
  dirty="$(git status --porcelain)"
  [ -z "$dirty" ] || fail 'Release requires a clean working tree.'
}

fetch_main() {
  git fetch --prune origin +refs/heads/main:refs/remotes/origin/main --tags
  local remote_commit
  remote_commit="$(git rev-parse refs/remotes/origin/main)"
  [ "$commit" = "$remote_commit" ] || fail 'HEAD must match origin/main. Update main first.'
}

check_tag() {
  local status
  if git show-ref --verify --quiet "refs/tags/$tag"; then
    fail "Tag $tag already exists."
  else
    status=$?
    [ "$status" -eq 1 ] || fail 'Unable to check release tag.'
  fi
}

check_ci() {
  local result
  result="$(gh run list --repo "$remote_url" --workflow ci.yml --branch main \
    --event push --commit "$commit" --limit 1 --json status,conclusion \
    --jq '.[0] | "\(.status):\(.conclusion)"')"
  [ "$result" = completed:success ] || fail "CI must pass for $commit (got: $result)."
}

fail() {
  printf '%s\n' "$*" >&2
  exit 1
}

main "$@"
