add_library(
  DirectShrinkTarget
  STATIC
    src/target_contract.cpp
    src/windows_target_platform.cpp
)

add_library(ytec::direct_shrink_target ALIAS DirectShrinkTarget)

target_include_directories(
  DirectShrinkTarget
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  DirectShrinkTarget
  PUBLIC
    ytec::clone_core
    ytec::image_format
    ytec::migration_core
    ytec::operation_core
  PRIVATE
    Advapi32
    Ole32
    Uuid
    ytec::boot_repair
    ytec::disk_model
    ytec::vss_requester
    ytec::windows_dism
    ytec::windows_shrink_analysis
    ytec::windows_shrink_restore
    wuguid
)

ytec_configure_target(DirectShrinkTarget)
