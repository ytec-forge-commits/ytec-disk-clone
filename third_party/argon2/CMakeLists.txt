add_library(
  ytec_argon2
  STATIC
    src/argon2.c
    src/core.c
    src/encoding.c
    src/ref.c
    src/blake2/blake2b.c
)

add_library(ytec::argon2 ALIAS ytec_argon2)

target_include_directories(
  ytec_argon2
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/blake2
)

target_compile_definitions(ytec_argon2 PRIVATE ARGON2_NO_THREADS)
target_compile_options(ytec_argon2 PRIVATE /W4 /WX /wd4996 /utf-8)

if(YTEC_ENABLE_ASAN)
  target_compile_options(ytec_argon2 PRIVATE /fsanitize=address)
endif()
