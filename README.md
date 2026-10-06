# struct-lint

struct-lint checks the order of code in source files: imports, types, and
constants first, then functions above the helpers they call.

Written in C, using Tree-sitter to parse source files.

| Language | Files |
| --- | --- |
| JavaScript | `.js`, `.jsx`, `.mjs`, `.cjs` |
| TypeScript | `.ts`, `.tsx`, `.mts`, `.cts` |
| C | `.c`, `.h` |
| Vue, Svelte, Astro | `.vue`, `.svelte`, `.astro` scripts |
| MDX | `.mdx` imports and exports |
| Go | `.go` |
| Python | `.py`, `.pyi` |
| Bash | `.sh`, `.bash` |

TypeScript declarations (`.d.ts`, `.d.mts`, `.d.cts`) and names such as
`main.test.js` or `main.spec.tsx` are included. JSX syntax works in JavaScript
files and `.tsx`; `.ts` uses the separate TypeScript grammar.

## Example

In this file, the helper appears before its caller:

```ts
function helper(): number {
  return 42;
}

export function main(): number {
  return helper();
}
```

Running `struct-lint --profile ci main.ts` reports:

```text
main.ts:1:1: error[function-order] helper must appear below caller main
```

Move the helper below `main` to fix it:

```ts
export function main(): number {
  return helper();
}

function helper(): number {
  return 42;
}
```

## Build

Requires a C11 compiler, CMake 3.24 or newer, and Git. From this repository:

```sh
cmake -S . -B .build -DCMAKE_BUILD_TYPE=Release
cmake --build .build --target struct-lint --parallel
.build/struct-lint --version
```

CMake downloads the pinned Tree-sitter parser and language grammars on the
first build. The executable is `.build/struct-lint`.

## Usage

```sh
.build/struct-lint src
.build/struct-lint --profile ci src
.build/struct-lint --profile ci --format json src test.ts
```

With no paths, struct-lint scans the current directory. Directory scans honor
nested `.gitignore` files. Explicit files are always checked; overlapping paths
are checked once.

| Option | Behavior |
| --- | --- |
| `--profile local` | Report warnings. This is the default. |
| `--profile ci` | Report errors and fail when findings exist. |
| `--format human\|json` | Choose output format; defaults to `human`. |
| `--no-ignore` | Include files excluded by `.gitignore`. |
| `--help` | Show usage. |
| `--version` | Show the version. |

`STRUCT_LINT_PROFILE` sets the default profile; `--profile` takes precedence.
Human diagnostics go to stderr. JSON goes to stdout.

Exit codes: `0` for no errors, `1` for lint errors, and `2` for invalid usage or
analysis failures. Parse errors fail under either profile.

## Rules

| Rule | Checks |
| --- | --- |
| `section-order` | Imports, types, constants, then functions. |
| `function-order` | Entry points and public functions precede private helpers; callers precede callees. Recursive cycles stay together. |
| `parse-error` | Source can be parsed using its language's grammar. |

The analyzer checks top-level declarations and direct calls between named
functions. It does not check Go methods, class internals, C++ files, or dynamic
calls. Entry points are `main`, plus `init` in Go. Go functions with uppercase
names and Python functions without a leading underscore count as public. In C,
functions with external linkage count as public. Bash `source` and `.` commands
count as imports.

To suppress an ordering rule, put
`// struct-lint-disable-next <rule-id> -- <reason>` immediately above the
top-level declaration.
Use `#` instead of `//` in Python and Bash.

## Development

```sh
./scripts/bootstrap.sh
./scripts/lint.sh
./scripts/check.sh
./scripts/benchmark.sh
```

Bootstrap checks development tools, builds/tests the project, and installs
Git hooks. See [Contributing](.github/CONTRIBUTING.md) for prerequisites and hooks.

The check script builds the C code and runs API, language, CLI, and README
tests. The benchmark checks runtime, peak memory, and executable size; CI
enforces limits of 2 seconds, 64 MiB of memory, and a 7 MiB executable on Linux x64.
The size allowance includes the embedded-language grammars.

The [C API](include/struct_lint.h) exposes `sl_analyze` and `sl_report_free`.
Set `SlRequest.collect_facts` to collect declarations, imports, exports, and
resolved calls alongside diagnostics.
JavaScript and TypeScript resolve relative ES imports and static CommonJS
`require` bindings, including aliases, destructuring, and namespace calls.
CommonJS exports can reference named functions or assign functions directly to
`module.exports` or `exports.name`. Explicit paths and directory index files
work; package manifests, dynamic paths, and reassigned exports are not resolved.
CommonJS extension lookup follows Node's `.js`, `.json`, `.node` order; use an
explicit extension for `.cjs` and TypeScript files. C, Go, Python, and Bash
resolve calls within each file. C preprocessor branches (`#if`, `#ifdef`, and
related directives) are checked independently. struct-lint does not evaluate
macros or preprocess C files, so it may report findings in branches excluded
from the current build. Syntax produced by macro expansion is not analyzed.

Vue and Svelte check inline JavaScript and TypeScript scripts. Astro checks
frontmatter and inline scripts. Each script block has its own ordering checks
and file-facts record, with positions in the original file. MDX imports and
exports share one module scope; prose and fenced examples are excluded.
Templates, styles, external scripts, and unsupported script languages are not
analyzed for declaration order.
Call facts include declaration positions to distinguish names repeated across
script blocks.

See [Contributing](.github/CONTRIBUTING.md) for development tools and adding a
language.

## License

MIT. Third-party notices are in `LICENSES/`.
Keep `LICENSE` and `LICENSES/` with redistributed binaries.
