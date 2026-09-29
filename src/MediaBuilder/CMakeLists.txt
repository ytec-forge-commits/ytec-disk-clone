add_library(
  MediaBuilder
  STATIC
    src/adk_detection.cpp
    src/driver_selection.cpp
    src/windows_adk_environment.cpp
    src/windows_driver_inventory.cpp
)

target_include_directories(
  MediaBuilder
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  MediaBuilder
  PUBLIC
    ytec::clone_core
  PRIVATE
    ytec::boot_repair
    Advapi32
    bcrypt
    Msi
    setupapi
    Version
)

ytec_configure_target(MediaBuilder)
add_library(ytec::media_builder ALIAS MediaBuilder)

add_executable(
  ytec-winpe-environment
  src/adk_diagnostic_main.cpp
)

target_link_libraries(
  ytec-winpe-environment
  PRIVATE
    ytec::media_builder
)

ytec_configure_target(ytec-winpe-environment)
