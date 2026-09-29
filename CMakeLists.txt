cmake_minimum_required(VERSION 3.25)

set(_ytec_version_manifest_path "${CMAKE_CURRENT_SOURCE_DIR}/version.json")
set_property(
  DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_ytec_version_manifest_path}"
)
if(NOT EXISTS "${_ytec_version_manifest_path}")
  message(FATAL_ERROR "Required product version manifest is missing: version.json")
endif()
file(SIZE "${_ytec_version_manifest_path}" _ytec_version_manifest_size)
if(
  _ytec_version_manifest_size LESS 1
  OR _ytec_version_manifest_size GREATER 4096
)
  message(FATAL_ERROR "version.json size is outside the allowed 1..4096 byte range")
endif()
file(READ "${_ytec_version_manifest_path}" _ytec_version_manifest LIMIT 4097)

string(
  JSON _ytec_version_member_count
  ERROR_VARIABLE _ytec_version_error
  LENGTH "${_ytec_version_manifest}"
)
if(NOT _ytec_version_error STREQUAL "NOTFOUND")
  message(FATAL_ERROR "version.json must be a valid JSON object: ${_ytec_version_error}")
endif()
if(NOT _ytec_version_member_count EQUAL 4)
  message(FATAL_ERROR "version.json must contain exactly four members")
endif()

set(_ytec_expected_version_members channel display file numeric)
set(_ytec_seen_version_members)
math(EXPR _ytec_version_last_member "${_ytec_version_member_count} - 1")
foreach(_ytec_version_index RANGE 0 ${_ytec_version_last_member})
  string(
    JSON _ytec_version_member
    ERROR_VARIABLE _ytec_version_error
    MEMBER "${_ytec_version_manifest}" ${_ytec_version_index}
  )
  if(NOT _ytec_version_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "Cannot enumerate version.json: ${_ytec_version_error}")
  endif()
  if(NOT _ytec_version_member IN_LIST _ytec_expected_version_members)
    message(FATAL_ERROR "version.json contains an unknown member: ${_ytec_version_member}")
  endif()
  if(_ytec_version_member IN_LIST _ytec_seen_version_members)
    message(FATAL_ERROR "version.json contains a duplicate member: ${_ytec_version_member}")
  endif()
  list(APPEND _ytec_seen_version_members "${_ytec_version_member}")
endforeach()

foreach(_ytec_version_member IN LISTS _ytec_expected_version_members)
  if(NOT _ytec_version_member IN_LIST _ytec_seen_version_members)
    message(FATAL_ERROR "version.json is missing member: ${_ytec_version_member}")
  endif()
  string(
    JSON _ytec_version_type
    ERROR_VARIABLE _ytec_version_error
    TYPE "${_ytec_version_manifest}" "${_ytec_version_member}"
  )
  if(
    NOT _ytec_version_error STREQUAL "NOTFOUND"
    OR NOT _ytec_version_type STREQUAL "STRING"
  )
    message(FATAL_ERROR "version.json member ${_ytec_version_member} must be a string")
  endif()
  string(
    JSON _ytec_version_${_ytec_version_member}
    ERROR_VARIABLE _ytec_version_error
    GET "${_ytec_version_manifest}" "${_ytec_version_member}"
  )
  if(NOT _ytec_version_error STREQUAL "NOTFOUND")
    message(FATAL_ERROR "Cannot read version.json member ${_ytec_version_member}")
  endif()
endforeach()

if(
  NOT _ytec_version_numeric MATCHES
    "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"
)
  message(FATAL_ERROR "version.json numeric must be a three-part numeric version")
endif()
if(NOT _ytec_version_channel MATCHES "^[a-z0-9]+(-[a-z0-9]+)*$")
  message(FATAL_ERROR "version.json channel has an invalid format")
endif()
if(NOT _ytec_version_display STREQUAL "${_ytec_version_numeric}-${_ytec_version_channel}")
  message(FATAL_ERROR "version.json display must equal numeric-channel")
endif()
if(NOT _ytec_version_file STREQUAL "${_ytec_version_numeric}.0")
  message(FATAL_ERROR "version.json file must equal numeric.0")
endif()

string(REPLACE "." "," YTEC_RC_FILE_VERSION_COMMAS "${_ytec_version_file}")
set(YTEC_PRODUCT_FILE_VERSION "${_ytec_version_file}")
set(YTEC_PRODUCT_VERSION "${_ytec_version_display}")
set(YTEC_PRODUCT_CHANNEL "${_ytec_version_channel}")

project(
  YtecDiskClone
  VERSION "${_ytec_version_numeric}"
  DESCRIPTION "Y-TEC Windows disk clone and image backup tool"
  LANGUAGES C CXX
)

if(NOT WIN32)
  message(FATAL_ERROR "Y-TEC Tsumugi Drive supports Windows only.")
endif()

if(NOT MSVC)
  message(FATAL_ERROR "Y-TEC Tsumugi Drive requires the MSVC toolchain.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Y-TEC Tsumugi Drive must be configured for x64.")
endif()

# MSVC 19.50 can emit /showIncludes as UTF-8 even when CMake 3.29 decodes the
# Japanese prefix as CP932.  Ninja then records zero header dependencies and
# can link ABI-incompatible stale objects after a public header change.
# Correct only the known mojibake value; English and other valid prefixes are
# left untouched.
if(
  CMAKE_GENERATOR MATCHES "Ninja"
  AND CMAKE_CL_SHOWINCLUDES_PREFIX MATCHES "^繝"
)
  set(CMAKE_CL_SHOWINCLUDES_PREFIX "メモ: インクルード ファイル: ")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(YTEC_ENABLE_ASAN "Enable MSVC AddressSanitizer instrumentation" OFF)
option(YTEC_ENABLE_MSVC_ANALYZE "Enable MSVC native code analysis" OFF)
option(
  YTEC_BUILD_DESTRUCTIVE_VM_TESTS
  "Build VM-only destructive disk integration harnesses"
  OFF
)

include(CTest)
include(cmake/YtecWarnings.cmake)

set(ZSTD_BUILD_STATIC ON CACHE BOOL "Build bundled static Zstandard" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "Do not build a Zstandard DLL" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "Do not build the Zstandard CLI" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "Use project-owned Zstandard tests" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "Do not build Zstandard contrib" FORCE)
set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "No dictionaries in product image formats" FORCE)
set(ZSTD_BUILD_DEPRECATED OFF CACHE BOOL "No deprecated Zstandard API" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "Reject legacy Zstandard frames" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "Use bounded single-thread chunks" FORCE)
add_subdirectory(third_party/zstd/build/cmake EXCLUDE_FROM_ALL)
add_library(ytec::zstd ALIAS libzstd_static)
target_compile_options(libzstd_static PRIVATE /utf-8)
if(YTEC_ENABLE_ASAN)
  target_compile_options(libzstd_static PRIVATE /fsanitize=address)
endif()

add_subdirectory(third_party/argon2 EXCLUDE_FROM_ALL)

add_subdirectory(src)

function(ytec_add_product_version_resource target original_filename description)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "Cannot add VERSIONINFO to missing target: ${target}")
  endif()
  set(YTEC_RC_INTERNAL_NAME "${target}")
  set(YTEC_RC_ORIGINAL_FILENAME "${original_filename}")
  set(YTEC_RC_FILE_DESCRIPTION "${description}")
  set(_ytec_rc_output_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/version")
  file(MAKE_DIRECTORY "${_ytec_rc_output_dir}")
  set(_ytec_rc_output "${_ytec_rc_output_dir}/${target}-version.rc")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/YtecProductVersion.rc.in"
    "${_ytec_rc_output}"
    @ONLY
  )
  target_sources("${target}" PRIVATE "${_ytec_rc_output}")
endfunction()

ytec_add_product_version_resource(
  ytec-tsumugi-drive
  "ytec-tsumugi-drive.exe"
  "Y-TEC Tsumugi Drive"
)
ytec_add_product_version_resource(
  ytec-winpe-environment
  "ytec-winpe-environment.exe"
  "Y-TEC Tsumugi Drive WinPE Environment"
)
ytec_add_product_version_resource(
  ytec-winpe-app
  "ytec-winpe-app.exe"
  "Y-TEC Tsumugi Drive WinPE CLI"
)
ytec_add_product_version_resource(
  ytec-winpe-gui
  "ytec-winpe-gui.exe"
  "Y-TEC Tsumugi Drive WinPE GUI"
)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
