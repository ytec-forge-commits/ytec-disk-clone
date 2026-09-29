add_library(
  WindowsDism
  STATIC
    src/dism.cpp
)

add_library(ytec::windows_dism ALIAS WindowsDism)

target_include_directories(
  WindowsDism
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  WindowsDism
  PUBLIC
    ytec::boot_repair
    ytec::clone_core
)

ytec_configure_target(WindowsDism)
