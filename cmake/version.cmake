if(DEFINED ENV{SL_VERSION} AND NOT "$ENV{SL_VERSION}" STREQUAL "")
  set(STRUCT_LINT_VERSION "$ENV{SL_VERSION}")
  set(version_component "(0|[1-9][0-9]*)")
  if(NOT STRUCT_LINT_VERSION MATCHES "^v${version_component}\\.${version_component}\\.${version_component}$")
    message(FATAL_ERROR "SL_VERSION must be a stable vMAJOR.MINOR.PATCH tag")
  endif()
else()
  execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE STRUCT_LINT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE STRUCT_LINT_GIT_STATUS
  )
  if(NOT STRUCT_LINT_GIT_STATUS EQUAL 0)
    message(FATAL_ERROR "Unable to derive struct-lint version from Git")
  endif()
endif()

string(REGEX REPLACE "^v" "" STRUCT_LINT_VERSION "${STRUCT_LINT_VERSION}")
