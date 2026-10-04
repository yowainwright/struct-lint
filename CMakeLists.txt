cmake_minimum_required(VERSION 3.24)

project(struct_lint VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
set(AMALGAMATED ON CACHE BOOL "Build Tree-sitter as one C translation unit" FORCE)

include(FetchContent)
include(CTest)

FetchContent_Declare(
  tree_sitter
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
  GIT_TAG 64402de2857cc197ecc4ca3bc144ea91fda7e72e
)
FetchContent_MakeAvailable(tree_sitter)

find_package(Threads REQUIRED)

function(add_language_grammar language repository revision source_dir)
  set(dependency "tree_sitter_${language}_source")
  FetchContent_Declare(${dependency}
    GIT_REPOSITORY "https://github.com/${repository}.git"
    GIT_TAG "${revision}"
    SOURCE_SUBDIR source-only
  )
  FetchContent_MakeAvailable(${dependency})
  set(grammar_dir "${${dependency}_SOURCE_DIR}/${source_dir}")
  add_library(tree_sitter_${language} STATIC "${grammar_dir}/parser.c")
  set(scanner "${grammar_dir}/scanner.c")
  if(language STREQUAL "mdx")
    include(cmake/mdx-scanner.cmake)
  endif()
  if(EXISTS "${scanner}")
    target_sources(tree_sitter_${language} PRIVATE "${scanner}")
  endif()
  target_include_directories(tree_sitter_${language} PRIVATE "${grammar_dir}")
  target_link_libraries(tree_sitter_${language} PUBLIC tree-sitter)
endfunction()

add_language_grammar(typescript tree-sitter/tree-sitter-typescript f975a621f4e7f532fe322e13c4f79495e0a7b2e7 typescript/src)
FetchContent_GetProperties(tree_sitter_typescript_source)
target_sources(tree_sitter_typescript PRIVATE
  "${tree_sitter_typescript_source_SOURCE_DIR}/tsx/src/parser.c"
  "${tree_sitter_typescript_source_SOURCE_DIR}/tsx/src/scanner.c"
)
add_language_grammar(javascript tree-sitter/tree-sitter-javascript 44c892e0be055ac465d5eeddae6d3e194424e7de src)
add_language_grammar(c tree-sitter/tree-sitter-c b780e47fc780ddc8da13afa35a3f4ed5c157823d src)
add_language_grammar(go tree-sitter/tree-sitter-go 1547678a9da59885853f5f5cc8a99cc203fa2e2c src)
add_language_grammar(python tree-sitter/tree-sitter-python 293fdc02038ee2bf0e2e206711b69c90ac0d413f src)
add_language_grammar(bash tree-sitter/tree-sitter-bash a06c2e4415e9bc0346c6b86d401879ffb44058f7 src)
add_language_grammar(vue tree-sitter-grammars/tree-sitter-vue ce8011a414fdf8091f4e4071752efc376f4afb08 src)
add_language_grammar(svelte tree-sitter-grammars/tree-sitter-svelte ae5199db47757f785e43a14b332118a5474de1a2 src)
add_language_grammar(astro virchau13/tree-sitter-astro 213f6e6973d9b456c6e50e86f19f66877e7ef0ee src)
add_language_grammar(mdx srazzak/tree-sitter-mdx 3aa29e8de1bf0213948a04fe953039b6ab73777b src)

add_library(struct_lint STATIC
  src/analyzer.c
  src/files.c
  src/language.c
  src/languages/typescript.c
  src/languages/c.c
  src/languages/embedded.c
  src/languages/go.c
  src/languages/python.c
  src/languages/bash.c
)
target_include_directories(struct_lint PUBLIC include PRIVATE src)
target_link_libraries(struct_lint PRIVATE
  tree-sitter tree_sitter_typescript tree_sitter_javascript
  tree_sitter_c tree_sitter_go tree_sitter_python tree_sitter_bash Threads::Threads
  tree_sitter_vue tree_sitter_svelte tree_sitter_astro tree_sitter_mdx
)
target_compile_definitions(struct_lint PRIVATE _POSIX_C_SOURCE=200809L)
if(UNIX AND NOT APPLE)
  target_compile_definitions(struct_lint PRIVATE _XOPEN_SOURCE=700)
endif()
if(APPLE)
  target_compile_definitions(struct_lint PRIVATE _DARWIN_C_SOURCE)
endif()

add_executable(struct-lint src/cli.c)
target_link_libraries(struct-lint PRIVATE struct_lint)

target_compile_definitions(struct-lint PRIVATE SL_VERSION="${PROJECT_VERSION}")

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(struct_lint PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(struct-lint PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_TESTING)
  foreach(test api facts language allocation multilang script commonjs embedded)
    add_executable(struct-lint-${test}-test tests/c/${test}_test.c)
    target_link_libraries(struct-lint-${test}-test PRIVATE struct_lint)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(struct-lint-${test}-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
  endforeach()
  target_include_directories(struct-lint-language-test PRIVATE
    src
    ${tree_sitter_SOURCE_DIR}/lib/include
  )
  target_include_directories(struct-lint-allocation-test PRIVATE
    ${tree_sitter_SOURCE_DIR}/lib/include
  )
  foreach(test api facts language multilang script commonjs embedded)
    add_test(NAME struct-lint-${test} COMMAND struct-lint-${test}-test)
  endforeach()
  set_tests_properties(struct-lint-facts PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/typescript"
  )
  set_tests_properties(struct-lint-multilang PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
  )
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/script-fixtures/directory")
  set_tests_properties(struct-lint-script PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/script-fixtures"
  )
  foreach(test commonjs embedded)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${test}-fixtures")
    set_tests_properties(struct-lint-${test} PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${test}-fixtures"
    )
  endforeach()
  add_test(NAME struct-lint-allocation COMMAND struct-lint-allocation-test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/typescript/function-order.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/typescript/section-order.ts
  )
  foreach(test options discovery readme cli script-cli embedded-cli)
    add_test(NAME struct-lint-${test}
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/test-${test}.sh
              $<TARGET_FILE:struct-lint>
    )
  endforeach()
  foreach(test setup bootstrap)
    add_test(NAME struct-lint-${test} COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/test-${test}.sh)
  endforeach()
endif()
