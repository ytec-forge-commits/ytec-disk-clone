add_library(
  CliTools
  STATIC
    src/cli_runner.cpp
)

add_library(ytec::cli_tools ALIAS CliTools)

target_include_directories(
  CliTools
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  CliTools
  PUBLIC
    ytec::clone_core
    ytec::disk_model
)

ytec_configure_target(CliTools)

add_executable(
  ytec-disk-inventory
  src/main.cpp
)

target_link_libraries(
  ytec-disk-inventory
  PRIVATE
    ytec::cli_tools
)

ytec_configure_target(ytec-disk-inventory)

