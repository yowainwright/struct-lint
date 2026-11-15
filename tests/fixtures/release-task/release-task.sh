#!/usr/bin/env bash
set -euo pipefail

main() {
  printf 'git %s\n' "$*" >>"$SL_TEST_LOG"
  case "$*" in
    'symbolic-ref --quiet --short HEAD') echo "${SL_TEST_BRANCH-main}" ;;
    'status --porcelain') printf '%s' "${SL_TEST_DIRTY-}" ;;
    'rev-parse '*) revision "$@" ;;
    'remote get-url origin') echo https://github.com/example/project.git ;;
    'fetch --prune origin +refs/heads/main:refs/remotes/origin/main --tags') test "${SL_TEST_FAIL-}" != fetch ;;
    'tag --list v[0-9]* --sort=-version:refname') printf '%s\n' "${SL_TEST_LOCAL_TAG-${SL_TEST_TAGS-v1.2.3}}" ;;
    'ls-remote --tags --refs --sort=-version:refname origin v[0-9]*') remote_tags ;;
    'merge-base --is-ancestor '*) test "${SL_TEST_FAIL-}" != ancestor ;;
    'show-ref --verify --quiet refs/tags/'*) test "${SL_TEST_FAIL-}" = duplicate ;;
    'tag -a '*) test "${SL_TEST_FAIL-}" != tag ;;
    '-c push.followTags=false push origin '*) test "${SL_TEST_FAIL-}" != push ;;
    *)
      printf 'Unexpected git command: %s\n' "$*" >&2
      exit 99
      ;;
  esac
}

revision() {
  case "$*" in
    'rev-parse HEAD') echo commit ;;
    'rev-parse refs/remotes/origin/main') remote_commit ;;
    'rev-parse v'*'^{commit}') echo "${SL_TEST_PREVIOUS-previous}" ;;
    *) exit 99 ;;
  esac
}

remote_tags() {
  local tag
  while IFS= read -r tag; do
    printf 'object\trefs/tags/%s\n' "$tag"
  done <<<"${SL_TEST_TAGS-v1.2.3}"
  test "${SL_TEST_FAIL-}" != remote-tags
}

remote_commit() {
  local fetches
  fetches="$(grep -c '^git fetch ' "$SL_TEST_LOG")"
  if [ "$fetches" -ge "${SL_TEST_CHANGE_AT-99}" ]; then
    echo changed
    return
  fi
  echo "${SL_TEST_REMOTE-commit}"
}

main "$@"
