#include "language.h"

#include <string.h>

static int path_has_extension(const char *path, const char *extension) {
  const size_t path_length = strlen(path);
  const size_t extension_length = strlen(extension);
  if (path_length < extension_length) return 0;
  return strcmp(path + path_length - extension_length, extension) == 0;
}

static int pack_accepts_path(const SlLanguagePack *pack, const char *path) {
  for (size_t index = 0; index < pack->extension_count; index++) {
    if (path_has_extension(path, pack->extensions[index])) return 1;
  }
  return 0;
}

const SlLanguagePack *sl_language_for_path(const char *path) {
  const SlLanguagePack *const packs[] = {&sl_typescript_pack, &sl_tsx_pack, &sl_javascript_pack,
                                         &sl_c_pack,          &sl_go_pack,  &sl_python_pack,
                                         &sl_bash_pack,       &sl_vue_pack, &sl_svelte_pack,
                                         &sl_astro_pack,      &sl_mdx_pack};
  const size_t count = sizeof(packs) / sizeof(*packs);
  for (size_t index = 0; index < count; index++) {
    if (pack_accepts_path(packs[index], path)) return packs[index];
  }
  return NULL;
}

int sl_language_resolve_call(const SlLanguagePack *pack, const SlCallResolutionRequest *request,
                             SlResolvedFunction *result) {
  if (pack == NULL || request == NULL || result == NULL) return 0;
  *result = (SlResolvedFunction){0};
  if (pack->resolve_call == NULL) return 0;
  if (!pack->resolve_call(request, result)) return 0;
  return result->file != NULL && result->declaration != NULL;
}
