#ifndef STRUCT_LINT_H
#define STRUCT_LINT_H

#include <stddef.h>

typedef struct {
  char *path;
  size_t line;
  size_t column;
  const char *rule_id;
  char *message;
  int fixed_error;
} SlDiagnostic;

typedef struct {
  const char *kind;
  char *name;
  int exported;
  int entrypoint;
  char **export_names;
  size_t export_name_count;
  size_t line;
  size_t column;
  char **calls;
  size_t call_count;
  /* Optional per-call 1-based local target lines for branch-aware resolution. */
  size_t *call_target_lines;
  char **suppressions;
  size_t suppression_count;
} SlDeclarationFact;

typedef struct {
  char *local_name;
  /* Export name, "*" for an ES namespace, or "module.exports" for require(). */
  char *imported_name;
  char *source;
  char *target_path;
} SlImportFact;

typedef struct {
  char *path;
  char *resolved_path;
  SlDeclarationFact *declarations;
  size_t declaration_count;
  SlImportFact *imports;
  size_t import_count;
} SlFileFact;

typedef struct {
  char *caller_path;
  char *caller_name;
  char *callee_path;
  char *callee_name;
  size_t caller_line;
  size_t caller_column;
  size_t callee_line;
  size_t callee_column;
} SlCallFact;

typedef struct {
  SlDiagnostic *diagnostics;
  size_t count;
  SlFileFact *files;
  size_t file_count;
  SlCallFact *calls;
  size_t call_count;
} SlReport;

typedef struct {
  const char *const *paths;
  size_t path_count;
  int use_gitignore;
  int collect_facts;
} SlRequest;

typedef enum { SL_OK = 0, SL_INVALID_ARGUMENT = 1, SL_IO_ERROR = 2, SL_OUT_OF_MEMORY = 3 } SlStatus;

SlStatus sl_analyze(const SlRequest *request, SlReport *report);
void sl_report_free(SlReport *report);

#endif
