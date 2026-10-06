#include "embedded.h"
#include "files.h"
#include "language.h"
#include "nodes.h"
#include "struct_lint.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tree_sitter/api.h>
#include <unistd.h>

typedef struct {
  char *bytes;
  size_t length;
  const TSRange *ranges;
  uint32_t range_count;
} Source;

typedef struct {
  char *name;
  TSNode node;
  char **calls;
  TSNode *call_nodes;
  size_t call_count;
  char **bindings;
  TSNode *binding_nodes;
  size_t binding_count;
  int exported;
  int entrypoint;
  int suppress_function_order;
} FunctionFact;

typedef struct {
  FunctionFact *items;
  size_t count;
} FunctionList;

typedef struct {
  const char *name;
  FunctionFact *function;
} NameSlot;

typedef struct {
  NameSlot *slots;
  size_t capacity;
} NameIndex;

typedef struct {
  const char *name;
  const SlFileFact *file;
  const SlDeclarationFact *declaration;
  int ambiguous;
} ProjectNameSlot;

typedef struct {
  ProjectNameSlot *slots;
  size_t capacity;
  int scoped;
} ProjectNameIndex;

typedef struct {
  ProjectNameIndex locals;
  ProjectNameIndex exports;
} ProjectIndex;

typedef struct {
  const ProjectIndex *index;
  const SlFileFact *file;
  size_t target_line;
} ProjectLookup;

typedef struct {
  char *local_name;
  char *export_name;
} ExportAlias;

typedef struct {
  ExportAlias *items;
  size_t count;
  size_t capacity;
} ExportTable;

typedef struct {
  size_t start;
  size_t end;
} TextRange;

typedef SlStatus (*TopLevelVisitor)(TSNode node, void *context);

static int nodes_coexist(const SlLanguagePack *pack, TSNode left, TSNode right);

typedef struct {
  size_t function;
  size_t next_call;
  size_t next_target;
} ComponentFrame;

typedef struct {
  FunctionList *functions;
  const SlLanguagePack *pack;
  const NameIndex *names;
  size_t *indices;
  size_t *lowlinks;
  size_t *components;
  size_t *stack;
  ComponentFrame *frames;
  unsigned char *on_stack;
  size_t stack_count;
  size_t frame_count;
  size_t next_index;
  size_t next_component;
} ComponentSearch;

typedef struct {
  const SlRequest *request;
  const SlFileList *files;
  SlReport *reports;
  size_t index;
  size_t count;
  SlStatus status;
} AnalysisWorker;

typedef struct {
  AnalysisWorker *workers;
  pthread_t *threads;
  SlReport *reports;
  size_t worker_count;
  size_t file_count;
} AnalysisPool;

static SlStatus read_source(const char *path, Source *source) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) return SL_IO_ERROR;
  if (fseek(file, 0, SEEK_END) != 0) return fclose(file), SL_IO_ERROR;
  const long size = ftell(file);
  if (size < 0 || fseek(file, 0, SEEK_SET) != 0) return fclose(file), SL_IO_ERROR;
  source->bytes = malloc((size_t)size + 1);
  if (source->bytes == NULL) return fclose(file), SL_OUT_OF_MEMORY;
  source->length = fread(source->bytes, 1, (size_t)size, file);
  const int read_failed = ferror(file);
  fclose(file);
  if (read_failed) return free(source->bytes), SL_IO_ERROR;
  source->bytes[source->length] = '\0';
  return SL_OK;
}

static int node_is(TSNode node, const char *type) { return strcmp(ts_node_type(node), type) == 0; }

static const char *section_name(SlDeclarationKind section) {
  if (section == SL_DECLARATION_IMPORT) return "imports";
  if (section == SL_DECLARATION_TYPE) return "public types";
  if (section == SL_DECLARATION_CONSTANT) return "constants";
  if (section == SL_DECLARATION_FUNCTION) return "functions";
  return "declarations";
}

static SlStatus visit_top_level(TSNode parent, const SlLanguagePack *pack, TopLevelVisitor visitor,
                                void *context) {
  TSTreeCursor cursor = ts_tree_cursor_new(parent);
  int started = 0;
  TSNode node;
  SlStatus status = SL_OK;
  while (status == SL_OK && !ts_node_is_null(node = sl_next_named_child(&cursor, &started))) {
    const int container = pack->top_level_container != NULL && pack->top_level_container(node);
    status = container ? visit_top_level(node, pack, visitor, context) : visitor(node, context);
  }
  ts_tree_cursor_delete(&cursor);
  return status;
}

static SlStatus append_diagnostic(SlReport *report, SlDiagnostic diagnostic) {
  const size_t size = (report->count + 1) * sizeof(*report->diagnostics);
  SlDiagnostic *items = realloc(report->diagnostics, size);
  if (items == NULL) {
    free(diagnostic.path);
    free(diagnostic.message);
    return SL_OUT_OF_MEMORY;
  }
  report->diagnostics = items;
  report->diagnostics[report->count] = diagnostic;
  report->count++;
  return SL_OK;
}

static char *node_text(const Source *source, TSNode node) {
  const uint32_t start = ts_node_start_byte(node);
  const uint32_t end = ts_node_end_byte(node);
  const size_t length = end - start;
  char *text = malloc(length + 1);
  if (text == NULL) return NULL;
  memcpy(text, source->bytes + start, length);
  text[length] = '\0';
  return text;
}

static int node_text_equals(const Source *source, TSNode node, const char *value) {
  const uint32_t start = ts_node_start_byte(node);
  const size_t length = ts_node_end_byte(node) - start;
  return strlen(value) == length && memcmp(source->bytes + start, value, length) == 0;
}

static int has_export_name(const SlDeclarationFact *fact, const char *name) {
  for (size_t index = 0; index < fact->export_name_count; index++) {
    if (strcmp(fact->export_names[index], name) == 0) return 1;
  }
  return 0;
}

static SlStatus append_export_name(SlDeclarationFact *fact, char *name) {
  if (name == NULL) return SL_OUT_OF_MEMORY;
  if (has_export_name(fact, name)) return free(name), SL_OK;
  const size_t count = fact->export_name_count + 1;
  char **names = realloc(fact->export_names, count * sizeof(*names));
  if (names == NULL) return free(name), SL_OUT_OF_MEMORY;
  fact->export_names = names;
  fact->export_names[fact->export_name_count++] = name;
  return SL_OK;
}

static void export_table_free(ExportTable *table) {
  for (size_t index = 0; index < table->count; index++) {
    free(table->items[index].local_name);
    free(table->items[index].export_name);
  }
  free(table->items);
  *table = (ExportTable){0};
}

static SlStatus reserve_export_alias(ExportTable *table) {
  if (table->count < table->capacity) return SL_OK;
  const size_t capacity = table->capacity == 0 ? 8 : table->capacity * 2;
  ExportAlias *items = realloc(table->items, capacity * sizeof(*items));
  if (items == NULL) return SL_OUT_OF_MEMORY;
  table->items = items;
  table->capacity = capacity;
  return SL_OK;
}

static SlStatus append_export_alias(ExportTable *table, char *local_name, char *export_name) {
  const int names_allocated = local_name != NULL && export_name != NULL;
  const SlStatus status = names_allocated ? reserve_export_alias(table) : SL_OUT_OF_MEMORY;
  if (status != SL_OK) {
    free(local_name);
    free(export_name);
    return status;
  }
  table->items[table->count++] =
      (ExportAlias){.local_name = local_name, .export_name = export_name};
  return SL_OK;
}

static SlStatus collect_export_alias(TSNode node, const Source *source, const SlLanguagePack *pack,
                                     ExportTable *table) {
  const TSNode local = pack->exported_reference_name_node(node, source->bytes);
  if (ts_node_is_null(local)) return SL_OK;
  const TSNode exported = pack->exported_name_node(node, source->bytes);
  const char *implicit = pack->implicit_export_name(node, source->bytes);
  if (ts_node_is_null(exported) && implicit == NULL) return SL_OK;
  char *name = ts_node_is_null(exported) ? strdup(implicit) : node_text(source, exported);
  return append_export_alias(table, node_text(source, local), name);
}

static SlStatus collect_export_aliases(TSNode node, const Source *source,
                                       const SlLanguagePack *pack, ExportTable *table) {
  if (pack->is_function_node(node)) return SL_OK;
  const SlStatus alias_status = collect_export_alias(node, source, pack, table);
  if (alias_status != SL_OK) return alias_status;
  TSTreeCursor cursor = ts_tree_cursor_new(node);
  int started = 0;
  TSNode child;
  while (!ts_node_is_null(child = sl_next_named_child(&cursor, &started))) {
    const SlStatus status = collect_export_aliases(child, source, pack, table);
    if (status != SL_OK) return ts_tree_cursor_delete(&cursor), status;
  }
  ts_tree_cursor_delete(&cursor);
  return SL_OK;
}

static SlStatus collect_direct_export(TSNode input, const Source *source,
                                      const SlLanguagePack *pack, ExportTable *table) {
  if (!pack->is_exported(input, source->bytes)) return SL_OK;
  const TSNode declaration = pack->declaration_node(input);
  const TSNode local = pack->name_node(declaration);
  const char *implicit = pack->implicit_export_name(input, source->bytes);
  const TSNode exported = pack->exported_name_node(input, source->bytes);
  const int anonymous = ts_node_is_null(local) || ts_node_is_missing(local);
  if (anonymous && implicit == NULL) return SL_OK;
  char *local_name = anonymous ? strdup(implicit) : node_text(source, local);
  const TSNode export_node = ts_node_is_null(exported) ? local : exported;
  char *export_name = implicit == NULL ? node_text(source, export_node) : strdup(implicit);
  return append_export_alias(table, local_name, export_name);
}

typedef struct {
  const Source *source;
  const SlLanguagePack *pack;
  ExportTable *table;
} ExportCollection;

static SlStatus collect_direct_export_node(TSNode node, void *context) {
  ExportCollection *collection = context;
  if (!collection->pack->is_exported(node, collection->source->bytes)) return SL_OK;
  SlStatus status =
      collect_export_aliases(node, collection->source, collection->pack, collection->table);
  if (status == SL_OK)
    status = collect_direct_export(node, collection->source, collection->pack, collection->table);
  return status;
}

static SlStatus collect_direct_exports(TSNode root, const Source *source,
                                       const SlLanguagePack *pack, ExportTable *table) {
  ExportCollection collection = {source, pack, table};
  return visit_top_level(root, pack, collect_direct_export_node, &collection);
}

static int compare_export_aliases(const void *left_value, const void *right_value) {
  const ExportAlias *left = left_value;
  const ExportAlias *right = right_value;
  const int local_order = strcmp(left->local_name, right->local_name);
  if (local_order != 0) return local_order;
  return strcmp(left->export_name, right->export_name);
}

static SlStatus export_table_build(TSNode root, const Source *source, const SlLanguagePack *pack,
                                   ExportTable *table) {
  const SlStatus status = collect_direct_exports(root, source, pack, table);
  if (status != SL_OK) return export_table_free(table), status;
  if (table->count > 1)
    qsort(table->items, table->count, sizeof(*table->items), compare_export_aliases);
  return SL_OK;
}

static size_t export_table_find(const ExportTable *table, const char *local_name) {
  size_t left = 0;
  size_t right = table->count;
  while (left < right) {
    const size_t middle = left + (right - left) / 2;
    const int order = strcmp(table->items[middle].local_name, local_name);
    if (order < 0) left = middle + 1;
    if (order >= 0) right = middle;
  }
  return left;
}

static int export_table_contains(const ExportTable *table, const char *local_name) {
  const size_t index = export_table_find(table, local_name);
  if (index == table->count) return 0;
  return strcmp(table->items[index].local_name, local_name) == 0;
}

static size_t line_start(const Source *source, size_t offset) {
  while (offset > 0 && source->bytes[offset - 1] != '\n')
    offset--;
  return offset;
}

static TextRange previous_line(const Source *source, size_t before) {
  size_t end = before;
  if (end > 0 && source->bytes[end - 1] == '\n') end--;
  if (end > 0 && source->bytes[end - 1] == '\r') end--;
  const size_t start = line_start(source, end);
  return (TextRange){start, end};
}

static TextRange trim_range(const Source *source, TextRange range) {
  while (range.start < range.end &&
         (source->bytes[range.start] == ' ' || source->bytes[range.start] == '\t')) {
    range.start++;
  }
  while (range.end > range.start &&
         (source->bytes[range.end - 1] == ' ' || source->bytes[range.end - 1] == '\t')) {
    range.end--;
  }
  return range;
}

static int previous_content_line(const Source *source, size_t declaration_start,
                                 TextRange *content) {
  size_t cursor = line_start(source, declaration_start);
  while (cursor > 0) {
    const TextRange line = previous_line(source, cursor);
    const TextRange trimmed = trim_range(source, line);
    if (trimmed.start < trimmed.end) return *content = trimmed, 1;
    cursor = line.start;
  }
  return 0;
}

static int suppression_prefix_matches(const Source *source, TextRange line,
                                      const SlLanguagePack *pack, size_t *rule_start) {
  const char *suffix = " struct-lint-disable-next ";
  const size_t comment_length = strlen(pack->line_comment_prefix);
  const size_t suffix_length = strlen(suffix);
  const size_t prefix_length = comment_length + suffix_length;
  if (line.end - line.start <= prefix_length + 4) return 0;
  const char *text = source->bytes + line.start;
  if (memcmp(text, pack->line_comment_prefix, comment_length) != 0) return 0;
  if (memcmp(text + comment_length, suffix, suffix_length) != 0) return 0;
  *rule_start = line.start + prefix_length;
  return 1;
}

static int suppression_rule_range(const Source *source, TextRange line, const SlLanguagePack *pack,
                                  TextRange *rule) {
  size_t start = 0;
  if (!suppression_prefix_matches(source, line, pack, &start)) return 0;
  for (size_t cursor = start; cursor + 4 < line.end; cursor++) {
    if (memcmp(source->bytes + cursor, " -- ", 4) != 0) continue;
    if (cursor == start) return 0;
    *rule = (TextRange){start, cursor};
    return 1;
  }
  return 0;
}

static int source_includes(const Source *source, TextRange line) {
  if (source->range_count == 0) return 1;
  for (uint32_t index = 0; index < source->range_count; index++) {
    const TSRange *range = &source->ranges[index];
    if (range->start_byte <= line.start && range->end_byte >= line.end) return 1;
  }
  return 0;
}

static int declaration_suppression(const Source *source, TSNode node, const SlLanguagePack *pack,
                                   TextRange *rule) {
  TextRange line = {0};
  if (!previous_content_line(source, ts_node_start_byte(node), &line)) return 0;
  if (!source_includes(source, line)) return 0;
  return suppression_rule_range(source, line, pack, rule);
}

static int declaration_suppresses(const Source *source, TSNode node, const SlLanguagePack *pack,
                                  const char *rule_id) {
  TextRange rule = {0};
  if (!declaration_suppression(source, node, pack, &rule)) return 0;
  const size_t length = rule.end - rule.start;
  return strlen(rule_id) == length && memcmp(source->bytes + rule.start, rule_id, length) == 0;
}

static char *range_text(const Source *source, TextRange range) {
  const size_t length = range.end - range.start;
  char *text = malloc(length + 1);
  if (text == NULL) return NULL;
  memcpy(text, source->bytes + range.start, length);
  text[length] = '\0';
  return text;
}

static SlStatus collect_fact_suppression(TSNode input, const Source *source,
                                         const SlLanguagePack *pack, SlDeclarationFact *fact) {
  TextRange rule = {0};
  if (!declaration_suppression(source, input, pack, &rule)) return SL_OK;
  fact->suppressions = calloc(1, sizeof(*fact->suppressions));
  if (fact->suppressions == NULL) return SL_OUT_OF_MEMORY;
  fact->suppressions[0] = range_text(source, rule);
  if (fact->suppressions[0] == NULL) return SL_OUT_OF_MEMORY;
  fact->suppression_count = 1;
  return SL_OK;
}

static SlStatus append_call(FunctionFact *function, char *name, TSNode node) {
  if (name == NULL) return SL_OUT_OF_MEMORY;
  const size_t size = (function->call_count + 1) * sizeof(*function->calls);
  char **calls = realloc(function->calls, size);
  if (calls == NULL) return free(name), SL_OUT_OF_MEMORY;
  function->calls = calls;
  if (ts_node_is_null(node)) {
    function->calls[function->call_count++] = name;
    return SL_OK;
  }
  TSNode *nodes = realloc(function->call_nodes, (function->call_count + 1) * sizeof(*nodes));
  if (nodes == NULL) return free(name), SL_OUT_OF_MEMORY;
  function->call_nodes = nodes;
  function->calls[function->call_count] = name;
  function->call_nodes[function->call_count++] = node;
  return SL_OK;
}

static SlStatus append_binding(FunctionFact *function, char *name, TSNode node) {
  if (name == NULL) return SL_OUT_OF_MEMORY;
  const size_t size = (function->binding_count + 1) * sizeof(*function->bindings);
  char **bindings = realloc(function->bindings, size);
  if (bindings == NULL) return free(name), SL_OUT_OF_MEMORY;
  function->bindings = bindings;
  function->bindings[function->binding_count] = name;
  if (ts_node_is_null(node)) {
    function->binding_count++;
    return SL_OK;
  }
  TSNode *nodes = realloc(function->binding_nodes, (function->binding_count + 1) * sizeof(*nodes));
  if (nodes == NULL) return free(name), SL_OUT_OF_MEMORY;
  function->binding_nodes = nodes;
  function->binding_nodes[function->binding_count++] = node;
  return SL_OK;
}

static TSNode lexical_scope(TSNode node) {
  for (TSNode parent = ts_node_parent(node); !ts_node_is_null(parent);
       parent = ts_node_parent(parent))
    if (node_is(parent, "compound_statement") || node_is(parent, "for_statement") ||
        node_is(parent, "function_definition"))
      return parent;
  return (TSNode){0};
}

static int scope_contains(TSNode scope, TSNode node) {
  for (TSNode parent = node; !ts_node_is_null(parent); parent = ts_node_parent(parent))
    if (ts_node_eq(parent, scope)) return 1;
  return 0;
}

static int binding_visible_at_call(const SlLanguagePack *pack, TSNode binding, TSNode call) {
  if (!nodes_coexist(pack, binding, call)) return 0;
  if (ts_node_start_byte(binding) >= ts_node_start_byte(call)) return 0;
  const TSNode binding_scope = lexical_scope(binding);
  const TSNode call_scope = lexical_scope(call);
  return !ts_node_is_null(binding_scope) && !ts_node_is_null(call_scope) &&
         scope_contains(binding_scope, call_scope);
}

static int function_has_binding(const FunctionFact *function, const Source *source,
                                const SlLanguagePack *pack, TSNode name, TSNode call) {
  for (size_t index = 0; index < function->binding_count; index++) {
    if (!node_text_equals(source, name, function->bindings[index])) continue;
    const TSNode binding =
        function->binding_nodes == NULL ? (TSNode){0} : function->binding_nodes[index];
    if (ts_node_is_null(binding) || binding_visible_at_call(pack, binding, call)) return 1;
  }
  return 0;
}

static SlStatus collect_bindings(TSNode node, const Source *source, const SlLanguagePack *pack,
                                 FunctionFact *function, int is_root) {
  const TSNode binding = is_root ? (TSNode){0} : pack->binding_name_node(node);
  if (!ts_node_is_null(binding)) {
    const TSNode context = pack->nodes_coexist == NULL ? (TSNode){0} : binding;
    const SlStatus status = append_binding(function, node_text(source, binding), context);
    if (status != SL_OK) return status;
  }
  if (!is_root && pack->is_function_node(node)) return SL_OK;
  const uint32_t count = ts_node_named_child_count(node);
  for (uint32_t index = 0; index < count; index++) {
    const SlStatus status =
        collect_bindings(ts_node_named_child(node, index), source, pack, function, 0);
    if (status != SL_OK) return status;
  }
  return SL_OK;
}

static SlStatus collect_calls(TSNode node, const Source *source, const SlLanguagePack *pack,
                              FunctionFact *function, int is_root) {
  if (!is_root && pack->is_function_node(node)) return SL_OK;
  const TSNode called_name = pack->called_name_node(node);
  const int is_shadowed = !ts_node_is_null(called_name) &&
                          function_has_binding(function, source, pack, called_name, node);
  if (!ts_node_is_null(called_name) && !is_shadowed) {
    char *name = pack->call_name == NULL ? node_text(source, called_name)
                                         : pack->call_name(node, source->bytes);
    const TSNode context = pack->nodes_coexist == NULL ? (TSNode){0} : node;
    const SlStatus status = append_call(function, name, context);
    if (status != SL_OK) return status;
  }
  const uint32_t count = ts_node_named_child_count(node);
  for (uint32_t index = 0; index < count; index++) {
    const TSNode child = ts_node_named_child(node, index);
    const SlStatus status = collect_calls(child, source, pack, function, 0);
    if (status != SL_OK) return status;
  }
  return SL_OK;
}

static SlStatus collect_function_body(TSNode node, const Source *source, const SlLanguagePack *pack,
                                      FunctionFact *function) {
  const TSNode root = pack->function_node(node);
  const SlStatus binding_status = collect_bindings(root, source, pack, function, 1);
  if (binding_status != SL_OK) return binding_status;
  return collect_calls(root, source, pack, function, 1);
}

static const char *fact_kind(SlDeclarationKind kind) {
  if (kind == SL_DECLARATION_IMPORT) return "import";
  if (kind == SL_DECLARATION_TYPE) return "type";
  if (kind == SL_DECLARATION_CONSTANT) return "constant";
  if (kind == SL_DECLARATION_FUNCTION) return "function";
  return NULL;
}

static void export_names_free(SlDeclarationFact *fact) {
  for (size_t index = 0; index < fact->export_name_count; index++)
    free(fact->export_names[index]);
  free(fact->export_names);
  fact->export_names = NULL;
  fact->export_name_count = 0;
}

static void declaration_fact_free(SlDeclarationFact *fact) {
  free(fact->name);
  export_names_free(fact);
  for (size_t index = 0; index < fact->call_count; index++)
    free(fact->calls[index]);
  free(fact->calls);
  free(fact->call_target_lines);
  for (size_t index = 0; index < fact->suppression_count; index++)
    free(fact->suppressions[index]);
  free(fact->suppressions);
  *fact = (SlDeclarationFact){0};
}

static void import_fact_free(SlImportFact *fact) {
  free(fact->local_name);
  free(fact->imported_name);
  free(fact->source);
  free(fact->target_path);
  *fact = (SlImportFact){0};
}

static SlStatus append_import(SlFileFact *file, SlImportFact fact) {
  const size_t count = file->import_count + 1;
  SlImportFact *imports = realloc(file->imports, count * sizeof(*imports));
  if (imports == NULL) return import_fact_free(&fact), SL_OUT_OF_MEMORY;
  file->imports = imports;
  file->imports[file->import_count++] = fact;
  return SL_OK;
}

static SlStatus imported_binding_name(TSNode node, const Source *source, const SlLanguagePack *pack,
                                      char **name) {
  *name = NULL;
  const TSNode imported = pack->imported_name_node(node);
  const char *implicit = pack->implicit_imported_name(node);
  const int named = !ts_node_is_null(imported);
  if (!named && implicit == NULL) return SL_OK;
  *name = named ? node_text(source, imported) : strdup(implicit);
  return *name == NULL ? SL_OUT_OF_MEMORY : SL_OK;
}

static SlStatus add_import_binding(TSNode node, const Source *source, const SlLanguagePack *pack,
                                   const char *import_source, const char *target_path,
                                   SlFileFact *file) {
  const TSNode local = pack->import_local_name_node(node);
  if (ts_node_is_null(local)) return SL_OK;
  char *imported_name = NULL;
  const SlStatus name_status = imported_binding_name(node, source, pack, &imported_name);
  if (name_status != SL_OK) return name_status;
  if (imported_name == NULL) return SL_OK;
  SlImportFact fact = {.local_name = node_text(source, local),
                       .imported_name = imported_name,
                       .source = strdup(import_source),
                       .target_path = target_path == NULL ? NULL : strdup(target_path)};
  const int allocated = fact.local_name && fact.imported_name && fact.source;
  const int target_allocated = target_path == NULL || fact.target_path != NULL;
  if (!allocated || !target_allocated) return import_fact_free(&fact), SL_OUT_OF_MEMORY;
  return append_import(file, fact);
}

static SlStatus collect_import_bindings(TSNode node, const Source *source,
                                        const SlLanguagePack *pack, const char *import_source,
                                        const char *target_path, SlFileFact *file) {
  const TSNode local = pack->import_local_name_node(node);
  if (!ts_node_is_null(local))
    return add_import_binding(node, source, pack, import_source, target_path, file);
  const uint32_t count = ts_node_named_child_count(node);
  for (uint32_t index = 0; index < count; index++) {
    const TSNode child = ts_node_named_child(node, index);
    const SlStatus status =
        collect_import_bindings(child, source, pack, import_source, target_path, file);
    if (status != SL_OK) return status;
  }
  return SL_OK;
}

static SlStatus collect_declaration_imports(const char *path, TSNode declaration,
                                            const Source *source, const SlLanguagePack *pack,
                                            SlFileFact *file) {
  const TSNode source_node = pack->import_source_node(declaration, source->bytes);
  if (ts_node_is_null(source_node)) return SL_OK;
  char *import_source = node_text(source, source_node);
  if (import_source == NULL) return SL_OUT_OF_MEMORY;
  pack->normalize_import_source(import_source);
  char *target_path = pack->resolve_import(path, import_source, declaration);
  const SlStatus status =
      collect_import_bindings(declaration, source, pack, import_source, target_path, file);
  free(target_path);
  free(import_source);
  return status;
}

static SlStatus collect_imports(const char *path, TSNode declaration, const Source *source,
                                const SlLanguagePack *pack, SlFileFact *file) {
  SlStatus status = collect_declaration_imports(path, declaration, source, pack, file);
  if (status != SL_OK) return status;
  const SlDeclarationKind kind = pack->declaration_kind(declaration, source->bytes);
  if (kind != SL_DECLARATION_IMPORT && kind != SL_DECLARATION_CONSTANT) return SL_OK;
  const uint32_t count = ts_node_named_child_count(declaration);
  for (uint32_t index = 0; index < count; index++) {
    const TSNode child = ts_node_named_child(declaration, index);
    status = collect_declaration_imports(path, child, source, pack, file);
    if (status != SL_OK) return status;
  }
  return SL_OK;
}

typedef struct {
  const Source *source;
  const SlLanguagePack *pack;
  const FunctionFact *caller;
  size_t *target_lines;
} CallTargetCollection;

static SlStatus collect_call_target(TSNode node, void *context) {
  CallTargetCollection *collection = context;
  if (!collection->pack->is_function_node(node)) return SL_OK;
  const TSNode name = collection->pack->name_node(node);
  for (size_t index = 0; index < collection->caller->call_count; index++) {
    const int same_name =
        node_text_equals(collection->source, name, collection->caller->calls[index]);
    if (collection->target_lines[index] != 0 || !same_name) continue;
    if (!nodes_coexist(collection->pack, collection->caller->call_nodes[index], node)) continue;
    collection->target_lines[index] = ts_node_start_point(node).row + 1;
  }
  return SL_OK;
}

static SlStatus collect_call_target_lines(TSNode root, const Source *source,
                                          const SlLanguagePack *pack, const FunctionFact *caller,
                                          size_t *target_lines) {
  CallTargetCollection collection = {source, pack, caller, target_lines};
  return visit_top_level(root, pack, collect_call_target, &collection);
}

static SlStatus collect_fact_calls(TSNode node, const Source *source, const SlLanguagePack *pack,
                                   TSNode root, SlDeclarationFact *fact) {
  if (strcmp(fact->kind, "function") != 0) return SL_OK;
  FunctionFact function = {0};
  SlStatus status = collect_function_body(node, source, pack, &function);
  fact->calls = function.calls;
  fact->call_count = function.call_count;
  if (status == SL_OK && pack->nodes_coexist != NULL && function.call_count > 0) {
    fact->call_target_lines = calloc(function.call_count, sizeof(*fact->call_target_lines));
    if (fact->call_target_lines == NULL) status = SL_OUT_OF_MEMORY;
    else status = collect_call_target_lines(root, source, pack, &function, fact->call_target_lines);
  }
  free(function.call_nodes);
  for (size_t index = 0; index < function.binding_count; index++)
    free(function.bindings[index]);
  free(function.bindings);
  free(function.binding_nodes);
  return status;
}

static SlStatus append_declaration(SlFileFact *file, SlDeclarationFact fact) {
  const size_t size = (file->declaration_count + 1) * sizeof(*file->declarations);
  SlDeclarationFact *declarations = realloc(file->declarations, size);
  if (declarations == NULL) return SL_OUT_OF_MEMORY;
  file->declarations = declarations;
  file->declarations[file->declaration_count++] = fact;
  return SL_OK;
}

static char *declaration_name(TSNode input, TSNode node, const Source *source,
                              const SlLanguagePack *pack) {
  const TSNode name_node = pack->name_node(node);
  const int anonymous = ts_node_is_null(name_node) || ts_node_is_missing(name_node);
  if (!anonymous) return node_text(source, name_node);
  const char *implicit = pack->implicit_export_name(input, source->bytes);
  if (implicit != NULL) return strdup(implicit);
  return strdup("");
}

static SlDeclarationFact create_declaration_fact(SlDeclarationKind kind, char *name, int entrypoint,
                                                 TSPoint point) {
  return (SlDeclarationFact){.kind = fact_kind(kind),
                             .name = name,
                             .entrypoint = entrypoint,
                             .line = point.row + 1,
                             .column = point.column + 1};
}

static SlStatus collect_declaration_exports(TSNode input, const ExportTable *exports,
                                            const Source *source, const SlLanguagePack *pack,
                                            SlDeclarationFact *fact) {
  if (pack->nodes_coexist != NULL) {
    if (!pack->is_exported(input, source->bytes)) return SL_OK;
    const SlStatus status = append_export_name(fact, strdup(fact->name));
    if (status == SL_OK) fact->exported = 1;
    return status;
  }
  size_t index = export_table_find(exports, fact->name);
  while (index < exports->count && strcmp(exports->items[index].local_name, fact->name) == 0) {
    const SlStatus status = append_export_name(fact, strdup(exports->items[index].export_name));
    if (status != SL_OK) return status;
    index++;
  }
  fact->exported = fact->export_name_count > 0;
  return SL_OK;
}

static SlStatus initialize_declaration_fact(TSNode input, TSNode root, const ExportTable *exports,
                                            const Source *source, const SlLanguagePack *pack,
                                            SlDeclarationKind declaration_kind,
                                            SlDeclarationFact *fact) {
  const TSNode node = pack->declaration_node(input);
  char *name = declaration_name(input, node, source, pack);
  if (name == NULL) return SL_OUT_OF_MEMORY;
  const TSPoint point = ts_node_start_point(input);
  const int entrypoint =
      declaration_kind == SL_DECLARATION_FUNCTION && pack->is_entrypoint_name(name);
  *fact = create_declaration_fact(declaration_kind, name, entrypoint, point);
  const SlStatus export_status = collect_declaration_exports(input, exports, source, pack, fact);
  if (export_status != SL_OK) return export_status;
  const SlStatus calls_status = collect_fact_calls(node, source, pack, root, fact);
  if (calls_status != SL_OK) return calls_status;
  return collect_fact_suppression(input, source, pack, fact);
}

static SlStatus collect_declaration_fact(TSNode input, TSNode root, const ExportTable *exports,
                                         const Source *source, const SlLanguagePack *pack,
                                         SlFileFact *file) {
  const SlDeclarationKind declaration_kind = pack->declaration_kind(input, source->bytes);
  if (fact_kind(declaration_kind) == NULL) return SL_OK;
  SlDeclarationFact fact = {0};
  const SlStatus fact_status =
      initialize_declaration_fact(input, root, exports, source, pack, declaration_kind, &fact);
  if (fact_status != SL_OK) return declaration_fact_free(&fact), fact_status;
  const SlStatus status = append_declaration(file, fact);
  if (status != SL_OK) declaration_fact_free(&fact);
  return status;
}

static void file_fact_free(SlFileFact *file) {
  free(file->path);
  free(file->resolved_path);
  for (size_t index = 0; index < file->declaration_count; index++) {
    declaration_fact_free(&file->declarations[index]);
  }
  for (size_t index = 0; index < file->import_count; index++)
    import_fact_free(&file->imports[index]);
  free(file->declarations);
  free(file->imports);
  *file = (SlFileFact){0};
}

static SlStatus append_file_fact(SlReport *report, SlFileFact file) {
  const size_t size = (report->file_count + 1) * sizeof(*report->files);
  SlFileFact *files = realloc(report->files, size);
  if (files == NULL) return SL_OUT_OF_MEMORY;
  report->files = files;
  report->files[report->file_count++] = file;
  return SL_OK;
}

typedef struct {
  const char *path;
  TSNode root;
  const ExportTable *exports;
  const Source *source;
  const SlLanguagePack *pack;
  SlFileFact *file;
} FileFactCollection;

static SlStatus collect_file_declaration(TSNode declaration, void *context) {
  FileFactCollection *collection = context;
  SlStatus status = collect_imports(collection->path, declaration, collection->source,
                                    collection->pack, collection->file);
  if (status == SL_OK)
    status = collect_declaration_fact(declaration, collection->root, collection->exports,
                                      collection->source, collection->pack, collection->file);
  return status;
}

static SlStatus collect_file_fact(const char *path, TSNode root, const ExportTable *exports,
                                  const Source *source, const SlLanguagePack *pack,
                                  SlReport *report) {
  SlFileFact file = {.path = strdup(path), .resolved_path = realpath(path, NULL)};
  if (file.path == NULL || file.resolved_path == NULL) return file_fact_free(&file), SL_IO_ERROR;
  FileFactCollection collection = {path, root, exports, source, pack, &file};
  const SlStatus collect_status =
      visit_top_level(root, pack, collect_file_declaration, &collection);
  if (collect_status != SL_OK) return file_fact_free(&file), collect_status;
  const SlStatus status = append_file_fact(report, file);
  if (status != SL_OK) file_fact_free(&file);
  return status;
}

static SlStatus append_function(FunctionList *functions, FunctionFact function) {
  const size_t size = (functions->count + 1) * sizeof(*functions->items);
  FunctionFact *items = realloc(functions->items, size);
  if (items == NULL) return SL_OUT_OF_MEMORY;
  functions->items = items;
  functions->items[functions->count++] = function;
  return SL_OK;
}

static void function_free(FunctionFact *function) {
  free(function->name);
  for (size_t index = 0; index < function->call_count; index++)
    free(function->calls[index]);
  free(function->calls);
  free(function->call_nodes);
  for (size_t index = 0; index < function->binding_count; index++)
    free(function->bindings[index]);
  free(function->bindings);
  free(function->binding_nodes);
  *function = (FunctionFact){0};
}

static FunctionFact create_function_fact(char *name, TSNode node, int exported, int entrypoint,
                                         int suppressed) {
  return (FunctionFact){.name = name,
                        .node = node,
                        .exported = exported,
                        .entrypoint = entrypoint,
                        .suppress_function_order = suppressed};
}

static SlStatus collect_function(TSNode input, const ExportTable *exports, const Source *source,
                                 const SlLanguagePack *pack, FunctionList *functions) {
  if (pack->declaration_kind(input, source->bytes) != SL_DECLARATION_FUNCTION) return SL_OK;
  const TSNode node = pack->declaration_node(input);
  char *name = declaration_name(input, node, source, pack);
  if (name == NULL) return SL_OUT_OF_MEMORY;
  const int exported = pack->nodes_coexist == NULL ? export_table_contains(exports, name)
                                                   : pack->is_exported(input, source->bytes);
  const int entrypoint = pack->is_entrypoint_name(name);
  const int suppressed = declaration_suppresses(source, input, pack, "function-order");
  FunctionFact function = create_function_fact(name, node, exported, entrypoint, suppressed);
  const SlStatus call_status = collect_function_body(node, source, pack, &function);
  if (call_status != SL_OK) return function_free(&function), call_status;
  const SlStatus status = append_function(functions, function);
  if (status != SL_OK) function_free(&function);
  return status;
}

typedef struct {
  const ExportTable *exports;
  const Source *source;
  const SlLanguagePack *pack;
  FunctionList *functions;
} FunctionCollection;

static SlStatus collect_function_node(TSNode node, void *context) {
  FunctionCollection *collection = context;
  return collect_function(node, collection->exports, collection->source, collection->pack,
                          collection->functions);
}

static SlStatus collect_functions(TSNode root, const ExportTable *exports, const Source *source,
                                  const SlLanguagePack *pack, FunctionList *functions) {
  FunctionCollection collection = {exports, source, pack, functions};
  return visit_top_level(root, pack, collect_function_node, &collection);
}

static void function_list_free(FunctionList *functions) {
  for (size_t index = 0; index < functions->count; index++)
    function_free(&functions->items[index]);
  free(functions->items);
  *functions = (FunctionList){0};
}

static size_t hash_name(const char *name) {
  size_t hash = 1469598103934665603ULL;
  for (const unsigned char *byte = (const unsigned char *)name; *byte != '\0'; byte++) {
    hash = (hash ^ *byte) * 1099511628211ULL;
  }
  return hash;
}

static size_t hash_path_name(const char *path, const char *name) {
  size_t hash = hash_name(path);
  for (const unsigned char *byte = (const unsigned char *)name; *byte != '\0'; byte++)
    hash = (hash ^ *byte) * 1099511628211ULL;
  return hash;
}

static size_t index_capacity(size_t count) {
  size_t capacity = 8;
  while (capacity < count * 2)
    capacity *= 2;
  return capacity;
}

static void name_index_insert(NameIndex *index, FunctionFact *function) {
  size_t slot = hash_name(function->name) & (index->capacity - 1);
  while (index->slots[slot].name != NULL)
    slot = (slot + 1) & (index->capacity - 1);
  index->slots[slot] = (NameSlot){function->name, function};
}

static SlStatus name_index_build(FunctionList *functions, NameIndex *index) {
  index->capacity = index_capacity(functions->count);
  index->slots = calloc(index->capacity, sizeof(*index->slots));
  if (index->slots == NULL) return SL_OUT_OF_MEMORY;
  for (size_t item = 0; item < functions->count; item++)
    name_index_insert(index, &functions->items[item]);
  return SL_OK;
}

static FunctionFact *name_index_find(const NameIndex *index, const char *name) {
  size_t slot = hash_name(name) & (index->capacity - 1);
  while (index->slots[slot].name != NULL) {
    if (strcmp(index->slots[slot].name, name) == 0) return index->slots[slot].function;
    slot = (slot + 1) & (index->capacity - 1);
  }
  return NULL;
}

static int name_index_next(const NameIndex *index, const char *name, size_t *slot,
                           FunctionFact **function) {
  if (*slot == SIZE_MAX) *slot = hash_name(name) & (index->capacity - 1);
  while (index->slots[*slot].name != NULL) {
    const size_t current = *slot;
    *slot = (current + 1) & (index->capacity - 1);
    if (strcmp(index->slots[current].name, name) != 0) continue;
    *function = index->slots[current].function;
    return 1;
  }
  *function = NULL;
  return 0;
}

static void component_search_free(ComponentSearch *search) {
  free(search->indices);
  free(search->lowlinks);
  free(search->components);
  free(search->stack);
  free(search->frames);
  free(search->on_stack);
  *search = (ComponentSearch){0};
}

static int component_search_alloc(ComponentSearch *search, size_t count) {
  search->indices = malloc(count * sizeof(*search->indices));
  search->lowlinks = malloc(count * sizeof(*search->lowlinks));
  search->components = malloc(count * sizeof(*search->components));
  search->stack = malloc(count * sizeof(*search->stack));
  search->frames = malloc(count * sizeof(*search->frames));
  search->on_stack = calloc(count, sizeof(*search->on_stack));
  return search->indices != NULL && search->lowlinks != NULL && search->components != NULL &&
         search->stack != NULL && search->frames != NULL && search->on_stack != NULL;
}

static SlStatus component_search_init(ComponentSearch *search, FunctionList *functions,
                                      const SlLanguagePack *pack, const NameIndex *names) {
  *search = (ComponentSearch){.functions = functions, .pack = pack, .names = names};
  if (functions->count == 0) return SL_OK;
  if (!component_search_alloc(search, functions->count)) {
    return component_search_free(search), SL_OUT_OF_MEMORY;
  }
  for (size_t index = 0; index < functions->count; index++) {
    search->indices[index] = SIZE_MAX;
    search->components[index] = SIZE_MAX;
  }
  return SL_OK;
}

static size_t function_index(const ComponentSearch *search, const FunctionFact *function) {
  return (size_t)(function - search->functions->items);
}

static int call_target_matches(const ComponentSearch *search, const FunctionFact *caller,
                               size_t call, const FunctionFact *target) {
  const int same_name = strcmp(caller->calls[call], target->name) == 0;
  if (!same_name || search->pack->nodes_coexist == NULL) return same_name;
  return search->pack->nodes_coexist(caller->call_nodes[call], target->node);
}

static void lower_lowlink(ComponentSearch *search, size_t current, size_t candidate) {
  if (candidate < search->lowlinks[current]) search->lowlinks[current] = candidate;
}

static void component_pop(ComponentSearch *search, size_t root) {
  size_t member = SIZE_MAX;
  while (member != root) {
    member = search->stack[--search->stack_count];
    search->on_stack[member] = 0;
    search->components[member] = search->next_component;
  }
  search->next_component++;
}

static void component_enter(ComponentSearch *search, size_t current) {
  search->indices[current] = search->next_index;
  search->lowlinks[current] = search->next_index++;
  search->stack[search->stack_count++] = current;
  search->on_stack[current] = 1;
  search->frames[search->frame_count++] =
      (ComponentFrame){.function = current, .next_target = SIZE_MAX};
}

static int component_visit_target(ComponentSearch *search, ComponentFrame *frame,
                                  FunctionFact *target) {
  const size_t next = function_index(search, target);
  if (search->indices[next] == SIZE_MAX) return component_enter(search, next), 1;
  if (search->on_stack[next]) lower_lowlink(search, frame->function, search->indices[next]);
  return 1;
}

static int component_advance_linear(ComponentSearch *search, ComponentFrame *frame,
                                    FunctionFact *function) {
  if (frame->next_call == function->call_count) return 0;
  FunctionFact *target = name_index_find(search->names, function->calls[frame->next_call++]);
  return target == NULL || component_visit_target(search, frame, target);
}

static int component_advance_conditional(ComponentSearch *search, ComponentFrame *frame,
                                         FunctionFact *function) {
  while (frame->next_call < function->call_count) {
    FunctionFact *target = NULL;
    const char *const call = function->calls[frame->next_call];
    if (!name_index_next(search->names, call, &frame->next_target, &target)) {
      frame->next_target = SIZE_MAX;
      frame->next_call++;
      continue;
    }
    if (!call_target_matches(search, function, frame->next_call, target)) continue;
    return component_visit_target(search, frame, target);
  }
  return 0;
}

static int component_advance(ComponentSearch *search, ComponentFrame *frame) {
  FunctionFact *function = &search->functions->items[frame->function];
  if (search->pack->nodes_coexist == NULL) return component_advance_linear(search, frame, function);
  return component_advance_conditional(search, frame, function);
}

static void component_finish_frame(ComponentSearch *search) {
  const size_t current = search->frames[--search->frame_count].function;
  if (search->frame_count > 0) {
    const size_t parent = search->frames[search->frame_count - 1].function;
    lower_lowlink(search, parent, search->lowlinks[current]);
  }
  if (search->lowlinks[current] == search->indices[current]) component_pop(search, current);
}

static void component_visit(ComponentSearch *search, size_t current) {
  component_enter(search, current);
  while (search->frame_count > 0) {
    ComponentFrame *frame = &search->frames[search->frame_count - 1];
    if (component_advance(search, frame)) continue;
    component_finish_frame(search);
  }
}

static SlStatus assign_components(FunctionList *functions, const SlLanguagePack *pack,
                                  const NameIndex *names, ComponentSearch *search) {
  const SlStatus status = component_search_init(search, functions, pack, names);
  if (status != SL_OK) return status;
  for (size_t index = 0; index < functions->count; index++) {
    if (search->indices[index] == SIZE_MAX) component_visit(search, index);
  }
  return SL_OK;
}

static SlStatus add_function_diagnostic(const char *path, const FunctionFact *target,
                                        const FunctionFact *caller, SlReport *report) {
  char message[192];
  snprintf(message, sizeof(message), "%s must appear below caller %s", target->name, caller->name);
  const TSPoint point = ts_node_start_point(target->node);
  SlDiagnostic diagnostic = {strdup(path),     point.row + 1,   point.column + 1,
                             "function-order", strdup(message), 0};
  if (diagnostic.path != NULL && diagnostic.message != NULL) {
    return append_diagnostic(report, diagnostic);
  }
  free(diagnostic.path);
  free(diagnostic.message);
  return SL_OUT_OF_MEMORY;
}

static SlStatus analyze_call_target(const char *path, FunctionFact *caller, size_t call,
                                    FunctionFact *target, const ComponentSearch *search,
                                    SlReport *report, int *reported) {
  const int earlier = ts_node_start_byte(target->node) < ts_node_start_byte(caller->node);
  if (!earlier || !call_target_matches(search, caller, call, target)) return SL_OK;
  if (target->suppress_function_order) return SL_OK;
  const size_t current = function_index(search, caller);
  const size_t target_index = function_index(search, target);
  if (search->components[current] == search->components[target_index]) return SL_OK;
  const SlStatus status = add_function_diagnostic(path, target, caller, report);
  if (status == SL_OK) *reported = 1;
  return status;
}

static SlStatus analyze_linear_function_calls(const char *path, FunctionFact *caller,
                                              const ComponentSearch *search, SlReport *report) {
  for (size_t call = 0; call < caller->call_count; call++) {
    FunctionFact *target = name_index_find(search->names, caller->calls[call]);
    if (target == NULL) continue;
    int reported = 0;
    const SlStatus status =
        analyze_call_target(path, caller, call, target, search, report, &reported);
    if (status != SL_OK || reported) return status;
  }
  return SL_OK;
}

static SlStatus analyze_conditional_function_calls(const char *path, FunctionFact *caller,
                                                   const ComponentSearch *search,
                                                   SlReport *report) {
  for (size_t call = 0; call < caller->call_count; call++) {
    size_t target_slot = SIZE_MAX;
    FunctionFact *target = NULL;
    while (name_index_next(search->names, caller->calls[call], &target_slot, &target)) {
      int reported = 0;
      const SlStatus status =
          analyze_call_target(path, caller, call, target, search, report, &reported);
      if (status != SL_OK || reported) return status;
    }
  }
  return SL_OK;
}

static SlStatus analyze_function_calls(const char *path, FunctionFact *caller,
                                       const ComponentSearch *search, SlReport *report) {
  if (search->pack->nodes_coexist == NULL)
    return analyze_linear_function_calls(path, caller, search, report);
  return analyze_conditional_function_calls(path, caller, search, report);
}

static SlStatus add_export_diagnostic(const char *path, const FunctionFact *internal,
                                      const FunctionFact *exported, SlReport *report) {
  char message[192];
  const char *role = exported->exported ? "exported" : "entry";
  snprintf(message, sizeof(message), "%s must appear below %s function %s", internal->name, role,
           exported->name);
  const TSPoint point = ts_node_start_point(internal->node);
  SlDiagnostic diagnostic = {strdup(path),     point.row + 1,   point.column + 1,
                             "function-order", strdup(message), 0};
  if (diagnostic.path != NULL && diagnostic.message != NULL) {
    return append_diagnostic(report, diagnostic);
  }
  free(diagnostic.path);
  free(diagnostic.message);
  return SL_OUT_OF_MEMORY;
}

static const FunctionFact *next_compatible_export(const FunctionList *functions,
                                                  const SlLanguagePack *pack, size_t index) {
  for (size_t next = index + 1; next < functions->count; next++) {
    const FunctionFact *candidate = &functions->items[next];
    const int exported = candidate->exported || candidate->entrypoint;
    if (exported && !candidate->suppress_function_order &&
        nodes_coexist(pack, functions->items[index].node, candidate->node))
      return candidate;
  }
  return NULL;
}

static SlStatus analyze_export_order_conditional(const char *path, const FunctionList *functions,
                                                 const ComponentSearch *search, SlReport *report) {
  for (size_t index = 0; index < functions->count; index++) {
    const FunctionFact *function = &functions->items[index];
    if (function->suppress_function_order) continue;
    if (function->exported || function->entrypoint) continue;
    const FunctionFact *next_export = next_compatible_export(functions, search->pack, index);
    if (next_export == NULL) continue;
    const size_t current = search->components[function_index(search, function)];
    const size_t exported = search->components[function_index(search, next_export)];
    if (current == exported) continue;
    return add_export_diagnostic(path, function, next_export, report);
  }
  return SL_OK;
}

static SlStatus analyze_export_order_linear(const char *path, const FunctionList *functions,
                                            const ComponentSearch *search, SlReport *report) {
  const FunctionFact *next_export = NULL;
  for (size_t index = functions->count; index-- > 0;) {
    const FunctionFact *function = &functions->items[index];
    if (function->suppress_function_order) continue;
    if (function->exported || function->entrypoint) {
      next_export = function;
      continue;
    }
    if (next_export == NULL) continue;
    const size_t current = search->components[function_index(search, function)];
    const size_t exported = search->components[function_index(search, next_export)];
    if (current == exported) continue;
    return add_export_diagnostic(path, function, next_export, report);
  }
  return SL_OK;
}

static SlStatus analyze_export_order(const char *path, const FunctionList *functions,
                                     const ComponentSearch *search, SlReport *report) {
  if (search->pack->nodes_coexist == NULL)
    return analyze_export_order_linear(path, functions, search, report);
  return analyze_export_order_conditional(path, functions, search, report);
}

static SlStatus diagnose_function_order(const char *path, FunctionList *functions,
                                        const ComponentSearch *search, SlReport *report) {
  const size_t initial_diagnostics = report->count;
  for (size_t item = 0; item < functions->count; item++) {
    const SlStatus status = analyze_function_calls(path, &functions->items[item], search, report);
    if (status != SL_OK) return status;
  }
  if (report->count != initial_diagnostics) return SL_OK;
  return analyze_export_order(path, functions, search, report);
}

static SlStatus analyze_function_order(const char *path, TSNode root, const ExportTable *exports,
                                       const Source *source, const SlLanguagePack *pack,
                                       SlReport *report) {
  FunctionList functions = {0};
  const SlStatus collect_status = collect_functions(root, exports, source, pack, &functions);
  if (collect_status != SL_OK) return function_list_free(&functions), collect_status;
  if (functions.count < 2) return function_list_free(&functions), SL_OK;
  NameIndex names = {0};
  SlStatus status = name_index_build(&functions, &names);
  ComponentSearch search = {0};
  if (status == SL_OK) status = assign_components(&functions, pack, &names, &search);
  if (status == SL_OK) status = diagnose_function_order(path, &functions, &search, report);
  component_search_free(&search);
  free(names.slots);
  function_list_free(&functions);
  return status;
}

static SlStatus add_order_diagnostic(const char *path, TSNode node, SlDeclarationKind current,
                                     SlDeclarationKind previous, SlReport *report) {
  char message[128];
  snprintf(message, sizeof(message), "%s must appear before %s", section_name(current),
           section_name(previous));
  const TSPoint point = ts_node_start_point(node);
  SlDiagnostic diagnostic = {strdup(path),    point.row + 1,   point.column + 1,
                             "section-order", strdup(message), 0};
  if (diagnostic.path != NULL && diagnostic.message != NULL) {
    return append_diagnostic(report, diagnostic);
  }
  free(diagnostic.path);
  free(diagnostic.message);
  return SL_OUT_OF_MEMORY;
}

typedef struct {
  TSNode node;
  SlDeclarationKind section;
} SectionFact;

typedef struct {
  const char *path;
  const Source *source;
  const SlLanguagePack *pack;
  SlReport *report;
  SectionFact *sections;
  size_t section_count;
  size_t function_count;
  SlDeclarationKind highest;
} RootAnalysis;

static int nodes_coexist(const SlLanguagePack *pack, TSNode left, TSNode right) {
  return pack->nodes_coexist == NULL || pack->nodes_coexist(left, right);
}

static SlDeclarationKind highest_compatible_section(const RootAnalysis *analysis, TSNode node) {
  SlDeclarationKind highest = SL_DECLARATION_NONE;
  for (size_t index = 0; index < analysis->section_count; index++) {
    const SectionFact *previous = &analysis->sections[index];
    if (previous->section > highest && nodes_coexist(analysis->pack, node, previous->node))
      highest = previous->section;
  }
  return highest;
}

static SlStatus append_section(RootAnalysis *analysis, TSNode node, SlDeclarationKind section) {
  const size_t count = analysis->section_count + 1;
  SectionFact *items = realloc(analysis->sections, count * sizeof(*items));
  if (items == NULL) return SL_OUT_OF_MEMORY;
  analysis->sections = items;
  analysis->sections[analysis->section_count++] = (SectionFact){node, section};
  return SL_OK;
}

static SlStatus analyze_root_declaration(TSNode node, void *context) {
  RootAnalysis *analysis = context;
  const SlDeclarationKind section = analysis->pack->declaration_kind(node, analysis->source->bytes);
  if (section == SL_DECLARATION_FUNCTION) analysis->function_count++;
  if (section == SL_DECLARATION_NONE) return SL_OK;
  const SlDeclarationKind highest = analysis->pack->nodes_coexist == NULL
                                        ? analysis->highest
                                        : highest_compatible_section(analysis, node);
  const int out_of_order = section < highest;
  const int suppressed = out_of_order && declaration_suppresses(analysis->source, node,
                                                                analysis->pack, "section-order");
  if (out_of_order && !suppressed) {
    const SlStatus status =
        add_order_diagnostic(analysis->path, node, section, highest, analysis->report);
    if (status != SL_OK) return status;
  }
  const SlStatus status = append_section(analysis, node, section);
  if (section > analysis->highest) analysis->highest = section;
  return status;
}

static SlStatus analyze_root(const char *path, TSNode root, const ExportTable *exports,
                             const Source *source, const SlLanguagePack *pack, SlReport *report) {
  RootAnalysis analysis = {path, source, pack, report, NULL, 0, 0, SL_DECLARATION_NONE};
  const SlStatus status = visit_top_level(root, pack, analyze_root_declaration, &analysis);
  free(analysis.sections);
  if (status != SL_OK) return status;
  if (analysis.function_count < 2) return SL_OK;
  return analyze_function_order(path, root, exports, source, pack, report);
}

static int compare_diagnostics(const void *left_value, const void *right_value) {
  const SlDiagnostic *left = left_value;
  const SlDiagnostic *right = right_value;
  const int path_order = strcmp(left->path, right->path);
  if (path_order != 0) return path_order;
  if (left->line != right->line) return left->line < right->line ? -1 : 1;
  if (left->column != right->column) return left->column < right->column ? -1 : 1;
  return strcmp(left->rule_id, right->rule_id);
}

static void sort_diagnostics(SlReport *report) {
  if (report->count < 2) return;
  qsort(report->diagnostics, report->count, sizeof(*report->diagnostics), compare_diagnostics);
}

static int find_error_node(TSNode node, TSNode *error) {
  if (node_is(node, "ERROR") || ts_node_is_missing(node)) return *error = node, 1;
  const uint32_t count = ts_node_child_count(node);
  for (uint32_t index = 0; index < count; index++) {
    if (find_error_node(ts_node_child(node, index), error)) return 1;
  }
  return 0;
}

static SlStatus add_parse_diagnostic(const char *path, TSNode root, const SlLanguagePack *pack,
                                     SlReport *report) {
  TSNode error = root;
  find_error_node(root, &error);
  const TSPoint point = ts_node_start_point(error);
  SlDiagnostic diagnostic = {strdup(path),
                             point.row + 1,
                             point.column + 1,
                             "parse-error",
                             strdup(pack->parse_error_message),
                             1};
  if (diagnostic.path != NULL && diagnostic.message != NULL) {
    return append_diagnostic(report, diagnostic);
  }
  free(diagnostic.path);
  free(diagnostic.message);
  return SL_OUT_OF_MEMORY;
}

static SlStatus collect_requested_facts(const char *path, TSNode root, const ExportTable *exports,
                                        const Source *source, const SlLanguagePack *pack,
                                        int collect_facts, SlReport *report) {
  if (!collect_facts) return SL_OK;
  return collect_file_fact(path, root, exports, source, pack, report);
}

static SlStatus analyze_parsed_file(const char *path, TSNode root, const Source *source,
                                    const SlLanguagePack *pack, int collect_facts,
                                    SlReport *report) {
  if (ts_node_has_error(root)) return add_parse_diagnostic(path, root, pack, report);
  ExportTable exports = {0};
  SlStatus status = export_table_build(root, source, pack, &exports);
  if (status == SL_OK)
    status = collect_requested_facts(path, root, &exports, source, pack, collect_facts, report);
  if (status == SL_OK) status = analyze_root(path, root, &exports, source, pack, report);
  export_table_free(&exports);
  return status;
}

static SlStatus analyze_script(const char *path, const Source *source, TSParser *parser,
                               const SlLanguagePack *pack, const TSRange *ranges, uint32_t count,
                               int collect_facts, SlReport *report) {
  const TSLanguage *language = pack->tree_sitter_language();
  if (ts_parser_language(parser) != language && !ts_parser_set_language(parser, language))
    return SL_INVALID_ARGUMENT;
  if (!ts_parser_set_included_ranges(parser, ranges, count)) return SL_INVALID_ARGUMENT;
  TSTree *tree = ts_parser_parse_string(parser, NULL, source->bytes, (uint32_t)source->length);
  if (tree == NULL) return SL_OUT_OF_MEMORY;
  Source script = *source;
  script.ranges = ranges;
  script.range_count = count;
  const SlStatus status =
      analyze_parsed_file(path, ts_tree_root_node(tree), &script, pack, collect_facts, report);
  ts_tree_delete(tree);
  return status;
}

static SlStatus analyze_mdx(const char *path, const Source *source, TSParser *parser,
                            const SlScriptRegions *regions, int collect_facts, SlReport *report) {
  TSRange *ranges = malloc(regions->count * sizeof(*ranges));
  if (ranges == NULL) return SL_OUT_OF_MEMORY;
  for (uint32_t index = 0; index < regions->count; index++)
    ranges[index] = regions->items[index].range;
  const SlStatus status = analyze_script(path, source, parser, &sl_javascript_pack, ranges,
                                         regions->count, collect_facts, report);
  return free(ranges), status;
}

static SlStatus analyze_regions(const char *path, const Source *source, TSParser *parser,
                                const SlLanguagePack *host, const SlScriptRegions *regions,
                                int collect_facts, SlReport *report) {
  if (regions->count == 0) return SL_OK;
  if (host == &sl_mdx_pack)
    return analyze_mdx(path, source, parser, regions, collect_facts, report);
  for (uint32_t index = 0; index < regions->count; index++) {
    const SlScriptRegion *region = &regions->items[index];
    const SlStatus status = analyze_script(path, source, parser, region->pack, &region->range, 1,
                                           collect_facts, report);
    if (status != SL_OK) return status;
  }
  return SL_OK;
}

static SlStatus analyze_embedded(const char *path, TSNode root, const Source *source,
                                 TSParser *parser, const SlLanguagePack *pack, int collect_facts,
                                 SlReport *report) {
  if (ts_node_has_error(root)) return add_parse_diagnostic(path, root, pack, report);
  SlScriptRegions regions = {0};
  SlStatus status = sl_script_regions(root, source->bytes, pack, &regions);
  if (status == SL_OK)
    status = analyze_regions(path, source, parser, pack, &regions, collect_facts, report);
  free(regions.items);
  return status;
}

static SlStatus analyze_source(const char *path, TSParser *parser, const Source *source,
                               const SlLanguagePack *pack, int collect_facts, SlReport *report) {
  if (!pack->embedded)
    return analyze_script(path, source, parser, pack, NULL, 0, collect_facts, report);
  if (!ts_parser_set_language(parser, pack->tree_sitter_language())) return SL_INVALID_ARGUMENT;
  ts_parser_set_included_ranges(parser, NULL, 0);
  TSTree *tree = ts_parser_parse_string(parser, NULL, source->bytes, (uint32_t)source->length);
  if (tree == NULL) return SL_OUT_OF_MEMORY;
  const TSNode root = ts_tree_root_node(tree);
  const SlStatus status = analyze_embedded(path, root, source, parser, pack, collect_facts, report);
  ts_tree_delete(tree);
  return status;
}

static SlStatus analyze_file(const char *path, TSParser *parser, int collect_facts,
                             SlReport *report) {
  const SlLanguagePack *pack = sl_language_for_path(path);
  if (pack == NULL) return SL_INVALID_ARGUMENT;
  Source source = {0};
  const SlStatus read_status = read_source(path, &source);
  if (read_status != SL_OK) return read_status;
  const SlStatus status = analyze_source(path, parser, &source, pack, collect_facts, report);
  free(source.bytes);
  return status;
}

static int request_paths_are_valid(const SlRequest *request) {
  if (request->paths == NULL || request->path_count == 0) return 0;
  for (size_t index = 0; index < request->path_count; index++) {
    const char *path = request->paths[index];
    if (path == NULL || path[0] == '\0') return 0;
  }
  return 1;
}

static int request_is_valid(const SlRequest *request, const SlReport *report) {
  if (request == NULL || report == NULL) return 0;
  const int gitignore_is_boolean = request->use_gitignore == 0 || request->use_gitignore == 1;
  const int facts_is_boolean = request->collect_facts == 0 || request->collect_facts == 1;
  if (!gitignore_is_boolean || !facts_is_boolean) return 0;
  return request_paths_are_valid(request);
}

static SlStatus create_parser(TSParser **parser) {
  *parser = ts_parser_new();
  if (*parser == NULL) return SL_OUT_OF_MEMORY;
  return SL_OK;
}

static void *run_analysis_worker(void *value) {
  AnalysisWorker *worker = value;
  TSParser *parser = NULL;
  worker->status = create_parser(&parser);
  for (size_t index = worker->index; index < worker->files->count && worker->status == SL_OK;
       index += worker->count) {
    worker->status = analyze_file(worker->files->items[index].path, parser,
                                  worker->request->collect_facts, &worker->reports[index]);
  }
  ts_parser_delete(parser);
  return NULL;
}

static size_t analysis_worker_count(size_t file_count) {
  const long processor_count = sysconf(_SC_NPROCESSORS_ONLN);
  size_t count = processor_count > 0 ? (size_t)processor_count : 1;
  if (count > 8) count = 8;
  if (count > file_count) count = file_count;
  return count;
}

static void analysis_pool_free(AnalysisPool *pool) {
  for (size_t index = 0; index < pool->file_count; index++)
    sl_report_free(&pool->reports[index]);
  free(pool->reports);
  free(pool->threads);
  free(pool->workers);
  *pool = (AnalysisPool){0};
}

static SlStatus analysis_pool_init(const SlRequest *request, const SlFileList *files,
                                   AnalysisPool *pool) {
  pool->worker_count = analysis_worker_count(files->count);
  pool->reports = calloc(files->count, sizeof(*pool->reports));
  pool->workers = calloc(pool->worker_count, sizeof(*pool->workers));
  pool->threads = calloc(pool->worker_count, sizeof(*pool->threads));
  if (pool->reports == NULL || pool->workers == NULL || pool->threads == NULL)
    return analysis_pool_free(pool), SL_OUT_OF_MEMORY;
  pool->file_count = files->count;
  for (size_t index = 0; index < pool->worker_count; index++) {
    pool->workers[index] =
        (AnalysisWorker){request, files, pool->reports, index, pool->worker_count, SL_OK};
  }
  return SL_OK;
}

static SlStatus spawn_analysis_workers(AnalysisPool *pool, size_t *created) {
  *created = 0;
  for (size_t index = 1; index < pool->worker_count; index++) {
    const int result =
        pthread_create(&pool->threads[*created], NULL, run_analysis_worker, &pool->workers[index]);
    if (result != 0) return SL_OUT_OF_MEMORY;
    (*created)++;
  }
  return SL_OK;
}

static SlStatus join_analysis_workers(AnalysisPool *pool, size_t created) {
  SlStatus status = SL_OK;
  for (size_t index = 0; index < created; index++) {
    if (pthread_join(pool->threads[index], NULL) != 0) status = SL_IO_ERROR;
  }
  return status;
}

static SlStatus analysis_worker_status(const AnalysisPool *pool) {
  for (size_t index = 0; index < pool->worker_count; index++) {
    if (pool->workers[index].status != SL_OK) return pool->workers[index].status;
  }
  return SL_OK;
}

static SlStatus run_analysis_workers(AnalysisPool *pool) {
  size_t created = 0;
  const SlStatus spawn_status = spawn_analysis_workers(pool, &created);
  run_analysis_worker(&pool->workers[0]);
  const SlStatus join_status = join_analysis_workers(pool, created);
  if (spawn_status != SL_OK) return spawn_status;
  if (join_status != SL_OK) return join_status;
  return analysis_worker_status(pool);
}

static SlStatus move_diagnostics(SlReport *destination, SlReport *source) {
  if (source->count == 0) return SL_OK;
  const size_t count = destination->count + source->count;
  SlDiagnostic *items = realloc(destination->diagnostics, count * sizeof(*items));
  if (items == NULL) return SL_OUT_OF_MEMORY;
  memcpy(items + destination->count, source->diagnostics,
         source->count * sizeof(*source->diagnostics));
  free(source->diagnostics);
  destination->diagnostics = items;
  destination->count = count;
  source->diagnostics = NULL;
  source->count = 0;
  return SL_OK;
}

static SlStatus move_file_facts(SlReport *destination, SlReport *source) {
  if (source->file_count == 0) return SL_OK;
  const size_t count = destination->file_count + source->file_count;
  SlFileFact *items = realloc(destination->files, count * sizeof(*items));
  if (items == NULL) return SL_OUT_OF_MEMORY;
  memcpy(items + destination->file_count, source->files,
         source->file_count * sizeof(*source->files));
  free(source->files);
  destination->files = items;
  destination->file_count = count;
  source->files = NULL;
  source->file_count = 0;
  return SL_OK;
}

static SlStatus merge_analysis_reports(AnalysisPool *pool, SlReport *report) {
  for (size_t index = 0; index < pool->file_count; index++) {
    SlStatus status = move_diagnostics(report, &pool->reports[index]);
    if (status == SL_OK) status = move_file_facts(report, &pool->reports[index]);
    if (status != SL_OK) return status;
  }
  return SL_OK;
}

static void call_fact_free(SlCallFact *call) {
  free(call->caller_path);
  free(call->caller_name);
  free(call->callee_path);
  free(call->callee_name);
  *call = (SlCallFact){0};
}

static void project_name_index_insert(ProjectNameIndex *index, const SlFileFact *file,
                                      const SlDeclarationFact *declaration, const char *name) {
  size_t slot = hash_path_name(file->resolved_path, name) & (index->capacity - 1);
  while (index->slots[slot].name != NULL &&
         (strcmp(index->slots[slot].name, name) != 0 ||
          (index->scoped && index->slots[slot].file != file) ||
          strcmp(index->slots[slot].file->resolved_path, file->resolved_path) != 0))
    slot = (slot + 1) & (index->capacity - 1);
  if (index->slots[slot].name != NULL) {
    index->slots[slot].ambiguous = 1;
    return;
  }
  index->slots[slot] = (ProjectNameSlot){name, file, declaration, 0};
}

static void project_index_local_file(ProjectNameIndex *index, const SlFileFact *file) {
  for (size_t item = 0; item < file->declaration_count; item++) {
    const SlDeclarationFact *declaration = &file->declarations[item];
    if (strcmp(declaration->kind, "function") == 0)
      project_name_index_insert(index, file, declaration, declaration->name);
  }
}

static void project_index_export_declaration(ProjectNameIndex *index, const SlFileFact *file,
                                             const SlDeclarationFact *declaration) {
  for (size_t name = 0; name < declaration->export_name_count; name++) {
    project_name_index_insert(index, file, declaration, declaration->export_names[name]);
  }
}

static void project_index_export_file(ProjectNameIndex *index, const SlFileFact *file) {
  for (size_t item = 0; item < file->declaration_count; item++) {
    const SlDeclarationFact *declaration = &file->declarations[item];
    if (strcmp(declaration->kind, "function") == 0)
      project_index_export_declaration(index, file, declaration);
  }
}

static size_t project_declaration_count(const SlReport *report) {
  size_t count = 0;
  for (size_t file = 0; file < report->file_count; file++)
    count += report->files[file].declaration_count;
  return count;
}

static size_t file_export_count(const SlFileFact *file) {
  size_t count = 0;
  for (size_t item = 0; item < file->declaration_count; item++)
    count += file->declarations[item].export_name_count;
  return count;
}

static size_t project_export_count(const SlReport *report) {
  size_t count = 0;
  for (size_t file = 0; file < report->file_count; file++)
    count += file_export_count(&report->files[file]);
  return count;
}

static SlStatus project_name_index_allocate(ProjectNameIndex *index, size_t count) {
  index->capacity = index_capacity(count);
  index->slots = calloc(index->capacity, sizeof(*index->slots));
  return index->slots == NULL ? SL_OUT_OF_MEMORY : SL_OK;
}

static void project_index_free(ProjectIndex *index) {
  free(index->locals.slots);
  free(index->exports.slots);
  *index = (ProjectIndex){0};
}

static SlStatus project_index_build(const SlReport *report, ProjectIndex *index) {
  index->locals.scoped = 1;
  SlStatus status = project_name_index_allocate(&index->locals, project_declaration_count(report));
  if (status == SL_OK)
    status = project_name_index_allocate(&index->exports, project_export_count(report));
  if (status != SL_OK) return project_index_free(index), status;
  for (size_t file = 0; file < report->file_count; file++) {
    project_index_local_file(&index->locals, &report->files[file]);
    project_index_export_file(&index->exports, &report->files[file]);
  }
  return SL_OK;
}

static const ProjectNameSlot *project_index_find(const ProjectNameIndex *index, const char *path,
                                                 const char *name, const SlFileFact *file) {
  size_t slot = hash_path_name(path, name) & (index->capacity - 1);
  while (index->slots[slot].name != NULL) {
    const int name_matches = strcmp(index->slots[slot].name, name) == 0;
    const int path_matches = strcmp(index->slots[slot].file->resolved_path, path) == 0;
    const int scope_matches = !index->scoped || index->slots[slot].file == file;
    if (!name_matches || !path_matches || !scope_matches) {
      slot = (slot + 1) & (index->capacity - 1);
      continue;
    }
    if (index->slots[slot].ambiguous) return NULL;
    return &index->slots[slot];
  }
  return NULL;
}

static int project_local_line_lookup(const ProjectLookup *lookup, const char *name,
                                     SlResolvedFunction *result) {
  for (size_t index = 0; index < lookup->file->declaration_count; index++) {
    const SlDeclarationFact *declaration = &lookup->file->declarations[index];
    const int matches = strcmp(declaration->kind, "function") == 0 &&
                        declaration->line == lookup->target_line &&
                        strcmp(declaration->name, name) == 0;
    if (!matches) continue;
    *result = (SlResolvedFunction){.file = lookup->file, .declaration = declaration};
    return 1;
  }
  return 0;
}

static int project_symbol_lookup(const void *context, const char *path, const char *name,
                                 int exported, SlResolvedFunction *result) {
  const ProjectLookup *lookup = context;
  if (!exported && lookup->target_line != 0) return project_local_line_lookup(lookup, name, result);
  const ProjectIndex *index = lookup->index;
  const ProjectNameIndex *names = exported ? &index->exports : &index->locals;
  const ProjectNameSlot *slot = project_index_find(names, path, name, lookup->file);
  if (slot == NULL) return 0;
  *result = (SlResolvedFunction){.file = slot->file, .declaration = slot->declaration};
  return 1;
}

static int resolve_called_function(const SlReport *report, const ProjectIndex *index,
                                   const SlFileFact *file, const char *name, size_t target_line,
                                   SlResolvedFunction *result) {
  const SlLanguagePack *pack = sl_language_for_path(file->path);
  if (pack == NULL) return 0;
  const ProjectLookup lookup = {index, file, target_line};
  const SlCallResolutionRequest request = {.project = report,
                                           .caller_file = file,
                                           .called_name = name,
                                           .lookup_context = &lookup,
                                           .lookup = project_symbol_lookup};
  return sl_language_resolve_call(pack, &request, result);
}

static size_t file_call_capacity(const SlFileFact *file) {
  size_t count = 0;
  for (size_t item = 0; item < file->declaration_count; item++)
    count += file->declarations[item].call_count;
  return count;
}

static size_t project_call_capacity(const SlReport *report) {
  size_t count = 0;
  for (size_t file = 0; file < report->file_count; file++)
    count += file_call_capacity(&report->files[file]);
  return count;
}

static SlStatus allocate_project_calls(SlReport *report) {
  const size_t capacity = project_call_capacity(report);
  if (capacity == 0) return SL_OK;
  report->calls = calloc(capacity, sizeof(*report->calls));
  return report->calls == NULL ? SL_OUT_OF_MEMORY : SL_OK;
}

static SlStatus add_resolved_call(SlReport *report, const ProjectIndex *index,
                                  const SlFileFact *caller_file, const SlDeclarationFact *caller,
                                  const char *name, size_t target_line) {
  SlResolvedFunction callee = {0};
  if (!resolve_called_function(report, index, caller_file, name, target_line, &callee))
    return SL_OK;
  SlCallFact call = {.caller_path = strdup(caller_file->path),
                     .caller_name = strdup(caller->name),
                     .callee_path = strdup(callee.file->path),
                     .callee_name = strdup(callee.declaration->name),
                     .caller_line = caller->line,
                     .caller_column = caller->column,
                     .callee_line = callee.declaration->line,
                     .callee_column = callee.declaration->column};
  const int allocated =
      call.caller_path && call.caller_name && call.callee_path && call.callee_name;
  if (!allocated) return call_fact_free(&call), SL_OUT_OF_MEMORY;
  report->calls[report->call_count++] = call;
  return SL_OK;
}

static SlStatus resolve_declaration_calls(SlReport *report, const ProjectIndex *index,
                                          const SlFileFact *file,
                                          const SlDeclarationFact *declaration) {
  if (strcmp(declaration->kind, "function") != 0) return SL_OK;
  for (size_t call = 0; call < declaration->call_count; call++) {
    const size_t target_line =
        declaration->call_target_lines == NULL ? 0 : declaration->call_target_lines[call];
    const SlStatus status =
        add_resolved_call(report, index, file, declaration, declaration->calls[call], target_line);
    if (status != SL_OK) return status;
  }
  return SL_OK;
}

static SlStatus resolve_file_calls(SlReport *report, const ProjectIndex *index,
                                   const SlFileFact *file) {
  for (size_t item = 0; item < file->declaration_count; item++) {
    const SlStatus status =
        resolve_declaration_calls(report, index, file, &file->declarations[item]);
    if (status != SL_OK) return status;
  }
  return SL_OK;
}

static SlStatus resolve_project_calls(SlReport *report, const ProjectIndex *index) {
  for (size_t file = 0; file < report->file_count; file++) {
    const SlStatus status = resolve_file_calls(report, index, &report->files[file]);
    if (status != SL_OK) return status;
  }
  return SL_OK;
}

static SlStatus build_project_calls(SlReport *report) {
  ProjectIndex index = {0};
  SlStatus status = project_index_build(report, &index);
  if (status != SL_OK) return status;
  status = allocate_project_calls(report);
  if (status == SL_OK) status = resolve_project_calls(report, &index);
  project_index_free(&index);
  return status;
}

static SlStatus analyze_files(const SlRequest *request, const SlFileList *files, SlReport *report) {
  AnalysisPool pool = {0};
  SlStatus status = analysis_pool_init(request, files, &pool);
  if (status == SL_OK) status = run_analysis_workers(&pool);
  if (status == SL_OK) status = merge_analysis_reports(&pool, report);
  analysis_pool_free(&pool);
  return status;
}

SlStatus sl_analyze(const SlRequest *request, SlReport *report) {
  if (!request_is_valid(request, report)) return SL_INVALID_ARGUMENT;
  *report = (SlReport){0};
  SlFileList files = {0};
  const SlStatus discovery_status =
      sl_discover_files(request->paths, request->path_count, request->use_gitignore, &files);
  if (discovery_status != SL_OK) return discovery_status;
  SlStatus status = analyze_files(request, &files, report);
  if (status == SL_OK && request->collect_facts) status = build_project_calls(report);
  if (status == SL_OK) sort_diagnostics(report);
  sl_file_list_free(&files);
  if (status != SL_OK) sl_report_free(report);
  return status;
}

void sl_report_free(SlReport *report) {
  if (report == NULL) return;
  for (size_t index = 0; index < report->count; index++) {
    free(report->diagnostics[index].path);
    free(report->diagnostics[index].message);
  }
  for (size_t index = 0; index < report->file_count; index++) {
    file_fact_free(&report->files[index]);
  }
  for (size_t index = 0; index < report->call_count; index++) {
    call_fact_free(&report->calls[index]);
  }
  free(report->diagnostics);
  free(report->files);
  free(report->calls);
  *report = (SlReport){0};
}
