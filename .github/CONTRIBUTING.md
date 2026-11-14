# Contributing

Requires a C11 compiler, CMake 3.24 or newer, Git, clang-format, and ShellCheck.
`mise install` can provision CMake and ShellCheck using `mise.toml`.

```sh
./scripts/bootstrap.sh
```

Bootstrap checks the installed tools, runs lint and build/tests, and installs
Git hooks. CMake fetches pinned parser dependencies on the first build.
Bootstrap can be rerun; CI skips hook installation.

Use `./scripts/format.sh` to format C sources and `./scripts/lint.sh` to
check C formatting and shell scripts. The formatter reads
`scripts/.clang-format` explicitly. Editors also need this configuration path.

The check script builds into `.build`. Set `SL_BUILD_DIR` to use another
directory. `./scripts/build.sh` builds just the CLI; `./scripts/benchmark.sh`
measures the resulting executable.

## Changes

Keep changes focused and C functions small. Add C or end-to-end coverage for
behavior changes. Describe the problem, resulting behavior, and validation in
the pull request.

Use `./scripts/setup.sh` to install hooks without building. It preserves
unmanaged hooks and symlinks, and stops if `core.hooksPath` is configured.

- `pre-commit` checks staged whitespace, C formatting, and shell scripts.
- `commit-msg` checks Conventional Commit messages.
- `post-merge` refreshes managed hooks and rebuilds when `CMakeLists.txt` or
  `cmake/` changes.

Full builds and tests run through `./scripts/check.sh`, bootstrap, and CI.
They do not run during commits.

## Releases

```sh
mise run release:dry-run
mise run release:patch
mise run release:minor
mise run release:major
```

Tagged releases take their version from the `vX.Y.Z` tag. The release
workflow passes it to `scripts/check.sh` as `SL_VERSION`; local builds use
CMake's `git describe` version.

Tagged releases publish `struct-lint-{darwin,linux}-{arm64,amd64}.tar.gz`
archives and `SHA256SUMS`. Each archive includes the executable, `LICENSE`,
and `LICENSES/`.

Homebrew registration lives in `yowainwright/homebrew-tap`, under
`brews/struct-lint.json`. The release workflow checks tap access before building.
After publishing all four archives, it calls the tap's reusable publication
workflow with the release tag to generate a formula update PR, wait for required
checks, and verify the merged formula URLs match the inventory version.

Configure the `HOMEBREW_TAP_TOKEN` Actions secret with Contents and Pull requests
write access to `yowainwright/homebrew-tap`. Missing tap access fails the preflight.

## Dependency updates

[Codependence](https://github.com/yowainwright/codependence) updates GitHub Actions
weekly or through a manual run of the `Update GitHub Actions` workflow. Its policy
lives in [.codependencerc](../.codependencerc); CMake dependency pins remain manual.
The tap's reusable publication workflow stays pinned because its repository
does not publish stable action tags. Other GitHub Actions remain eligible for updates.

The workflow installs the CLI with `brew install yowainwright/tap/codependence`,
validates changed workflows with actionlint, and opens one update PR. It uses the
`CODEPENDENCE_TOKEN` repository secret, which needs Contents, Pull requests, and
Workflows write access. Dependabot version-update configuration has been removed.

## Adding a language

Language packs belong in `src/languages/` and implement `SlLanguagePack` from
`src/language.h`. Each pack identifies declarations, imports, exports, calls,
and entry points using a Tree-sitter grammar.

Register the pack in `src/language.c`, link its grammar in `CMakeLists.txt`,
and add C and CLI fixtures. The analyzer and CLI are shared across languages.
