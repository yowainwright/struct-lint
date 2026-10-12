#include "struct_lint.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  const char *source;
  const char *rule;
  size_t calls;
} SourceCase;

static const char *const extensions[] = {"ts", "tsx", "mts", "cts", "js", "jsx", "mjs", "cjs"};

static const SourceCase cases[] = {
    {"export function api() { return 1; }\nfunction helper() { return api(); }\n", NULL, 1},
    {"function helper() { return api(); }\nexport function api() { return 1; }\n", "function-order",
     1},
    {"export function api() { return shared(); }\nexport function shared() { return 1; }\n", NULL,
     1},
    {"export function shared() { return 1; }\nexport function api() { return shared(); }\n",
     "function-order", 1},
    {"function caller() { return helper(); }\nfunction helper() { return 1; }\n", NULL, 1},
    {"function helper() { return 1; }\nfunction caller() { return helper(); }\n", "function-order",
     1},
    {"export const api = () => 1;\nconst helper = () => api();\n", NULL, 1},
    {"function api() { return 1; }\nfunction helper() { return api(); }\nexport { api };\n", NULL,
     1},
    {"export function api() { return helper(); }\nexport function shared() { return 1; }\n"
     "function helper() { return shared(); }\n",
     NULL, 2},
    {"function main() { return helper(); }\nfunction helper() { return 1; }\n", NULL, 1},
    {"function helper() { return 1; }\nfunction main() { return helper(); }\n", "function-order",
     1},
    {"function main() {}\nconst value = 1;\n", "section-order", 0},
    {"// struct-lint-disable-next function-order -- test\nfunction helper() { return 1; }\n"
     "function main() { return helper(); }\n",
     NULL, 1},
    {"function main(helper) { return helper(); }\nfunction helper() { return 1; }\n", NULL, 0},
};

static int write_source(const char *path, const char *source) {
  FILE *file = fopen(path, "wb");
  if (file == NULL) return 0;
  const int written = fputs(source, file) >= 0;
  return fclose(file) == 0 && written;
}

static int report_matches(const SlReport *report, const SourceCase *test, const char *path) {
  if (report->call_count != test->calls) return 0;
  if (test->rule == NULL) return report->count == 0 && report->file_count == 1;
  if (report->count != 1) return 0;
  const SlDiagnostic *diagnostic = &report->diagnostics[0];
  const int parse_error = strcmp(test->rule, "parse-error") == 0;
  return strcmp(diagnostic->rule_id, test->rule) == 0 && strcmp(diagnostic->path, path) == 0 &&
         diagnostic->fixed_error == parse_error && diagnostic->line > 0 && diagnostic->column > 0;
}

static int check_source(const char *path, const SourceCase *test) {
  if (!write_source(path, test->source)) return 0;
  const SlRequest request = {&path, 1, 0, 1};
  SlReport report = {0};
  const SlStatus status = sl_analyze(&request, &report);
  const int passed = status == SL_OK && report_matches(&report, test, path);
  if (!passed)
    fprintf(stderr, "%s: status %d, diagnostics %zu, calls %zu\n", path, status, report.count,
            report.call_count);
  sl_report_free(&report);
  return passed;
}

static int check_variants(void) {
  for (size_t extension = 0; extension < sizeof(extensions) / sizeof(*extensions); extension++) {
    char path[32];
    snprintf(path, sizeof(path), "main.%s", extensions[extension]);
    for (size_t index = 0; index < sizeof(cases) / sizeof(*cases); index++) {
      if (!check_source(path, &cases[index])) return 0;
    }
  }
  return 1;
}

static int check_dialects(void) {
  const SourceCase jsx = {"export function main() { return <div>{helper()}</div>; }\n"
                          "function helper() { return 1; }\n",
                          NULL, 1};
  const SourceCase typed = {"export function main(): number { return <number>1; }\n", NULL, 0};
  const SourceCase invalid_js = {typed.source, "parse-error", 0};
  const SourceCase invalid_tsx = {typed.source, "parse-error", 0};
  const SourceCase tsx = {
      "export function main(): unknown { return <div>{helper<number>(1)}</div>; }\n"
      "function helper<T>(value: T): T { return value; }\n",
      NULL, 1};
  if (!check_source("component.js", &jsx) || !check_source("component.jsx", &jsx)) return 0;
  if (!check_source("component.tsx", &tsx) || !check_source("assertion.ts", &typed)) return 0;
  return check_source("invalid.js", &invalid_js) && check_source("invalid.tsx", &invalid_tsx);
}

static int check_public_calls(void) {
  const SourceCase c = {"int api(void) { return 1; }\n"
                        "static int helper(void) { return api(); }\n",
                        NULL, 1};
  const SourceCase go = {"package example\nfunc Api() int { return 1 }\n"
                         "func helper() int { return Api() }\n",
                         NULL, 1};
  const SourceCase python = {"def api():\n    return 1\n"
                             "def _helper():\n    return api()\n",
                             NULL, 1};
  const SourceCase bash = {"main() { :; }\nhelper() { main; }\n", NULL, 1};
  return check_source("public-call.c", &c) && check_source("public-call.go", &go) &&
         check_source("public-call.py", &python) && check_source("public-call.sh", &bash);
}

static int check_commonjs(void) {
  const SourceCase commonjs = {"#!/usr/bin/env node\nconst fs = require('fs');\n"
                               "function main() { return helper(); }\n"
                               "function helper() { return fs; }\nmodule.exports = main;\n",
                               NULL, 1};
  return check_source("module.cjs", &commonjs);
}

static int check_declarations(void) {
  const SourceCase declaration = {"export interface Value { value: number; }\n"
                                  "export declare function main(): Value;\n",
                                  NULL, 0};
  const char *const paths[] = {"main.d.ts", "main.d.mts", "main.d.cts"};
  for (size_t index = 0; index < sizeof(paths) / sizeof(*paths); index++) {
    if (!check_source(paths[index], &declaration)) return 0;
  }
  return 1;
}

static int write_import(const char *specifier) {
  char source[256];
  snprintf(source, sizeof(source),
           "import { helper as local } from './%s';\n"
           "export function main() { return <div>{local()}</div>; }\n",
           specifier);
  return write_source("graph.tsx", source);
}

static int check_graph(const char *helper, const char *specifier) {
  if (!write_import(specifier)) return 0;
  const char *const paths[] = {"graph.tsx", helper};
  const SlRequest request = {paths, 2, 0, 1};
  SlReport report = {0};
  const SlStatus status = sl_analyze(&request, &report);
  const int passed = status == SL_OK && report.count == 0 && report.call_count == 1 &&
                     strcmp(report.calls[0].callee_path, helper) == 0 &&
                     strcmp(report.calls[0].callee_name, "helper") == 0;
  if (!passed) fprintf(stderr, "unresolved import: %s\n", specifier);
  sl_report_free(&report);
  return passed;
}

static int check_imports(void) {
  const char *const source = "export function helper() { return 1; }\n";
  for (size_t index = 0; index < sizeof(extensions) / sizeof(*extensions); index++) {
    if (strcmp(extensions[index], "cjs") == 0) continue;
    char helper[64], specifier[64];
    snprintf(helper, sizeof(helper), "helper-%s.%s", extensions[index], extensions[index]);
    snprintf(specifier, sizeof(specifier), "helper-%s", extensions[index]);
    if (!write_source(helper, source)) return 0;
    if (!check_graph(helper, helper) || !check_graph(helper, specifier)) return 0;
  }
  if (!write_source("directory/index.jsx", source)) return 0;
  return check_graph("directory/index.jsx", "directory");
}

int main(void) {
  const int passed = check_variants() && check_dialects() && check_commonjs() &&
                     check_declarations() && check_imports() && check_public_calls();
  return passed ? 0 : 1;
}
