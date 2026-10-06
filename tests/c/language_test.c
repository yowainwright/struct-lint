#include "language.h"

#include <string.h>

static const struct {
  const char *path;
  const char *language;
} cases[] = {
    {"main.ts", "typescript"},
    {"main.mts", "typescript"},
    {"main.cts", "typescript"},
    {"main.d.ts", "typescript"},
    {"main.d.mts", "typescript"},
    {"main.d.cts", "typescript"},
    {"main.tsx", "tsx"},
    {"main.js", "javascript"},
    {"main.jsx", "javascript"},
    {"main.cjs", "javascript"},
    {"main.mjs", "javascript"},
    {"main.go", "go"},
    {"main.c", "c"},
    {"main.h", "c"},
    {"main.py", "python"},
    {"main.pyi", "python"},
    {"main.sh", "bash"},
    {"main.bash", "bash"},
    {"view.vue", "vue"},
    {"view.svelte", "svelte"},
    {"view.astro", "astro"},
    {"view.mdx", "mdx"},
};

static int check_extensions(void) {
  for (size_t index = 0; index < sizeof(cases) / sizeof(*cases); index++) {
    const SlLanguagePack *pack = sl_language_for_path(cases[index].path);
    if (pack == NULL || strcmp(pack->id, cases[index].language) != 0) return 0;
  }
  return sl_language_for_path("main.txt") == NULL;
}

static int invalid_resolver(const SlCallResolutionRequest *request, SlResolvedFunction *result) {
  (void)request;
  *result = (SlResolvedFunction){0};
  return 1;
}

int main(void) {
  if (!check_extensions()) return 1;
  const SlLanguagePack pack = {.resolve_call = invalid_resolver};
  const SlCallResolutionRequest request = {0};
  SlResolvedFunction result = {0};
  return sl_language_resolve_call(&pack, &request, &result) ? 1 : 0;
}
