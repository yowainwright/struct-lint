#include "struct_lint.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  const char *path;
  const char *rule;
  size_t calls;
} Fixture;

static const Fixture fixtures[] = {
    {"c/clean.c", NULL, 1},
    {"c/function-order.c", "function-order", 1},
    {"c/export-order.c", "function-order", 0},
    {"c/section-order.c", "section-order", 0},
    {"c/parse-error.c", "parse-error", 0},
    {"c/conditional-branches.c", "function-order", 2},
    {"c/conditional-call-facts.c", NULL, 2},
    {"c/conditional-shadow.c", "function-order", 1},
    {"c/declarations.h", NULL, 0},
    {"go/clean.go", NULL, 1},
    {"go/function-order.go", "function-order", 1},
    {"go/section-order.go", "section-order", 0},
    {"go/parse-error.go", "parse-error", 0},
    {"go/suppressed-function.go", NULL, 1},
    {"go/suppressed-section.go", NULL, 0},
    {"go/shadow.go", NULL, 0},
    {"go/nested.go", NULL, 0},
    {"go/assignment.go", NULL, 0},
    {"go/export-order.go", "function-order", 0},
    {"go/unicode-export.go", "function-order", 0},
    {"go/recursive.go", NULL, 2},
    {"go/grouped.go", NULL, 0},
    {"go/methods.go", NULL, 1},
    {"python/clean.py", NULL, 1},
    {"python/function-order.py", "function-order", 1},
    {"python/section-order.py", "section-order", 0},
    {"python/parse-error.py", "parse-error", 0},
    {"python/suppressed-function.py", NULL, 1},
    {"python/suppressed-section.py", NULL, 0},
    {"python/shadow.py", NULL, 0},
    {"python/nested.py", NULL, 0},
    {"python/assignment.py", NULL, 0},
    {"python/export-order.py", "function-order", 0},
    {"python/recursive.py", NULL, 2},
    {"python/decorated.py", NULL, 1},
    {"python/comments.py", NULL, 0},
    {"python/signatures.pyi", NULL, 0},
    {"python/import-shadow.py", NULL, 0},
    {"python/walrus.py", NULL, 0},
    {"python/with-binding.py", NULL, 0},
    {"bash/clean.sh", NULL, 1},
    {"bash/function-order.sh", "function-order", 1},
    {"bash/section-order.sh", "section-order", 0},
    {"bash/parse-error.sh", "parse-error", 0},
    {"bash/suppressed-function.sh", NULL, 1},
    {"bash/suppressed-section.sh", NULL, 0},
    {"bash/shadow.sh", NULL, 0},
    {"bash/nested.sh", NULL, 0},
    {"bash/assignment.sh", NULL, 1},
    {"bash/recursive.sh", NULL, 2},
    {"bash/export.sh", NULL, 1},
    {"bash/comments.sh", NULL, 0},
    {"bash/alternate.bash", NULL, 1},
    {"bash/source-order.sh", "section-order", 0},
};

static int report_matches(const SlReport *report, const Fixture *fixture) {
  const size_t diagnostics = fixture->rule == NULL ? 0 : 1;
  if (report->count != diagnostics || report->call_count != fixture->calls) return 0;
  if (fixture->rule == NULL) return report->file_count == 1;
  const int parse_error = strcmp(fixture->rule, "parse-error") == 0;
  const SlDiagnostic *diagnostic = &report->diagnostics[0];
  return strcmp(diagnostic->rule_id, fixture->rule) == 0 &&
         strcmp(diagnostic->path, fixture->path) == 0 && diagnostic->fixed_error == parse_error &&
         diagnostic->line > 0 && diagnostic->column > 0;
}

static int check_fixture(const Fixture *fixture) {
  const SlRequest request = {&fixture->path, 1, 1, 1};
  SlReport report = {0};
  const SlStatus status = sl_analyze(&request, &report);
  const int passed = status == SL_OK && report_matches(&report, fixture);
  if (!passed) {
    fprintf(stderr, "%s: status %d, diagnostics %zu, calls %zu\n", fixture->path, status,
            report.count, report.call_count);
    for (size_t index = 0; index < report.count; index++)
      fprintf(stderr, "  %s\n", report.diagnostics[index].message);
  }
  sl_report_free(&report);
  return passed;
}

static int check_conditional_call_fact(void) {
  const char *const path = "c/conditional-call-facts.c";
  const SlRequest request = {&path, 1, 1, 1};
  SlReport report = {0};
  const SlStatus status = sl_analyze(&request, &report);
  int first_target = 0;
  int second_target = 0;
  for (size_t index = 0; index < report.call_count; index++) {
    first_target |= report.calls[index].callee_line == 3;
    second_target |= report.calls[index].callee_line == 5;
  }
  const int passed = status == SL_OK && report.count == 0 && report.call_count == 2 &&
                     first_target && second_target;
  if (!passed) fprintf(stderr, "incorrect conditional call target: %s\n", path);
  sl_report_free(&report);
  return passed;
}

static int check_mixed_scan(size_t diagnostics, size_t calls) {
  const char *const paths[] = {"c", "go", "python", "bash", "go/clean.go", "python"};
  const SlRequest request = {paths, sizeof(paths) / sizeof(*paths), 1, 1};
  SlReport report = {0};
  const SlStatus status = sl_analyze(&request, &report);
  const int passed = status == SL_OK && report.count == diagnostics && report.call_count == calls;
  if (!passed)
    fprintf(stderr, "mixed scan: status %d, diagnostics %zu, calls %zu\n", status, report.count,
            report.call_count);
  sl_report_free(&report);
  return passed;
}

int main(void) {
  size_t diagnostics = 0;
  size_t calls = 0;
  for (size_t index = 0; index < sizeof(fixtures) / sizeof(*fixtures); index++) {
    if (!check_fixture(&fixtures[index])) return 1;
    diagnostics += fixtures[index].rule != NULL;
    calls += fixtures[index].calls;
  }
  if (!check_conditional_call_fact()) return 1;
  return check_mixed_scan(diagnostics, calls) ? 0 : 1;
}
