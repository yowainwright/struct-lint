#include "common.h"

const TSLanguage *tree_sitter_c(void);

static TSNode declarator_name(TSNode node) {
  if (ts_node_is_null(node)) return (TSNode){0};
  if (sl_node_is(node, "identifier") || sl_node_is(node, "type_identifier")) return node;
  return declarator_name(sl_field(node, "declarator"));
}

static int top_level_container(TSNode node) {
  return sl_node_is(node, "preproc_if") || sl_node_is(node, "preproc_ifdef") ||
         sl_node_is(node, "preproc_elif") || sl_node_is(node, "preproc_elifdef") ||
         sl_node_is(node, "preproc_else");
}

static int conditional_group(TSNode node) {
  return sl_node_is(node, "preproc_if") || sl_node_is(node, "preproc_ifdef") ||
         sl_node_is(node, "preproc_elif") || sl_node_is(node, "preproc_elifdef");
}

static TSNode child_under(TSNode node, TSNode ancestor) {
  TSNode parent = ts_node_parent(node);
  while (!ts_node_is_null(parent) && !ts_node_eq(parent, ancestor)) {
    node = parent;
    parent = ts_node_parent(parent);
  }
  return ts_node_eq(parent, ancestor) ? node : (TSNode){0};
}

static TSNode branch_arm(TSNode node, TSNode group) {
  const TSNode child = child_under(node, group);
  const int alternative = sl_node_is(child, "preproc_elif") ||
                          sl_node_is(child, "preproc_elifdef") || sl_node_is(child, "preproc_else");
  return alternative ? child : group;
}

static int nodes_coexist(TSNode left, TSNode right) {
  for (TSNode parent = ts_node_parent(left); !ts_node_is_null(parent);
       parent = ts_node_parent(parent)) {
    if (!conditional_group(parent)) continue;
    const TSNode right_arm = child_under(right, parent);
    if (ts_node_is_null(right_arm)) continue;
    if (!ts_node_eq(branch_arm(left, parent), branch_arm(right, parent))) return 0;
  }
  return 1;
}

static int is_type_specifier(TSNode node) {
  return sl_node_is(node, "struct_specifier") || sl_node_is(node, "union_specifier") ||
         sl_node_is(node, "enum_specifier");
}

static SlDeclarationKind declaration_kind(TSNode node, const char *source) {
  (void)source;
  if (sl_node_is(node, "preproc_include")) return SL_DECLARATION_IMPORT;
  if (sl_node_is(node, "function_definition")) return SL_DECLARATION_FUNCTION;
  if (sl_node_is(node, "type_definition")) return SL_DECLARATION_TYPE;
  if (sl_node_is(node, "preproc_def") || sl_node_is(node, "preproc_function_def"))
    return SL_DECLARATION_CONSTANT;
  if (!sl_node_is(node, "declaration")) return SL_DECLARATION_NONE;
  const TSNode type = sl_field(node, "type");
  const TSNode declarator = sl_field(node, "declarator");
  if (ts_node_is_null(declarator) && is_type_specifier(type)) return SL_DECLARATION_TYPE;
  if (sl_node_is(declarator, "function_declarator")) return SL_DECLARATION_NONE;
  return SL_DECLARATION_CONSTANT;
}

static TSNode name_node(TSNode node) {
  if (sl_node_is(node, "preproc_include")) return sl_field(node, "path");
  if (sl_node_is(node, "preproc_def") || sl_node_is(node, "preproc_function_def"))
    return sl_field(node, "name");
  if (sl_node_is(node, "declaration") && ts_node_is_null(sl_field(node, "declarator")))
    return sl_field(sl_field(node, "type"), "name");
  if (sl_node_is(node, "function_definition") || sl_node_is(node, "type_definition") ||
      sl_node_is(node, "declaration"))
    return declarator_name(sl_field(node, "declarator"));
  return (TSNode){0};
}

static int is_function_node(TSNode node) { return sl_node_is(node, "function_definition"); }

static int declarator_wrapper(TSNode node) {
  return sl_node_is(node, "declarator") || sl_node_is(node, "pointer_declarator") ||
         sl_node_is(node, "array_declarator") || sl_node_is(node, "function_declarator") ||
         sl_node_is(node, "parenthesized_declarator") || sl_node_is(node, "init_declarator");
}

static TSNode binding_name_node(TSNode node) {
  if (!sl_node_is(node, "identifier")) return (TSNode){0};
  TSNode child = node;
  for (TSNode parent = ts_node_parent(child); !ts_node_is_null(parent);
       child = parent, parent = ts_node_parent(parent)) {
    const int owner = sl_node_is(parent, "declaration") ||
                      sl_node_is(parent, "parameter_declaration") ||
                      sl_node_is(parent, "type_definition");
    if (owner) return sl_field_is(parent, "declarator", child) ? node : (TSNode){0};
    if (!declarator_wrapper(parent)) return (TSNode){0};
  }
  return (TSNode){0};
}

static TSNode called_name_node(TSNode node) {
  if (!sl_node_is(node, "call_expression")) return (TSNode){0};
  const TSNode function = sl_field(node, "function");
  return sl_node_is(function, "identifier") ? function : (TSNode){0};
}

static TSNode import_source_node(TSNode node, const char *source) {
  (void)source;
  return sl_node_is(node, "preproc_include") ? sl_field(node, "path") : (TSNode){0};
}

static int is_exported(TSNode node, const char *source) {
  if (!is_function_node(node)) return 0;
  const uint32_t count = ts_node_named_child_count(node);
  for (uint32_t index = 0; index < count; index++) {
    const TSNode child = ts_node_named_child(node, index);
    if (sl_node_is(child, "storage_class_specifier") && sl_text_is(child, source, "static"))
      return 0;
  }
  return 1;
}

static const char *const extensions[] = {".c", ".h"};

const SlLanguagePack sl_c_pack = {
    .id = "c",
    .parse_error_message = "could not parse C source",
    .line_comment_prefix = "//",
    .extensions = extensions,
    .extension_count = sizeof(extensions) / sizeof(*extensions),
    .tree_sitter_language = tree_sitter_c,
    .top_level_container = top_level_container,
    .nodes_coexist = nodes_coexist,
    .declaration_node = sl_identity_node,
    .declaration_kind = declaration_kind,
    .name_node = name_node,
    .function_node = sl_identity_node,
    .is_function_node = is_function_node,
    .binding_name_node = binding_name_node,
    .called_name_node = called_name_node,
    .import_source_node = import_source_node,
    .normalize_import_source = sl_keep_source,
    .resolve_import = sl_no_import,
    .import_local_name_node = sl_no_node,
    .imported_name_node = sl_no_node,
    .implicit_imported_name = sl_no_text,
    .exported_reference_name_node = sl_no_source_node,
    .exported_name_node = sl_no_source_node,
    .implicit_export_name = sl_no_source_text,
    .is_exported = is_exported,
    .is_entrypoint_name = sl_is_main,
    .resolve_call = sl_resolve_local_call,
};
