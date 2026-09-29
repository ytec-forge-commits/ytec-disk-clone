add_library(
  WindowsShrinkAnalysis
  STATIC
    src/source_analysis.cpp
)

add_library(ytec::windows_shrink_analysis ALIAS WindowsShrinkAnalysis)

target_include_directories(
  WindowsShrinkAnalysis
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  WindowsShrinkAnalysis
  PUBLIC
    ytec::boot_repair
    ytec::clone_core
    ytec::disk_model
    ytec::image_format
    ytec::migration_core
)

ytec_configure_target(WindowsShrinkAnalysis)
