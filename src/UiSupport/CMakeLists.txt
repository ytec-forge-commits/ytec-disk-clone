add_library(
  UiSupport
  STATIC
    src/error_presentation.cpp
    src/private_fonts.cpp
)

add_library(ytec::ui_support ALIAS UiSupport)

target_include_directories(
  UiSupport
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  UiSupport
  PUBLIC
    ytec::clone_core
  PRIVATE
    gdi32
)

ytec_configure_target(UiSupport)

set(
  YTEC_LINE_SEED_FONT_RESOURCE
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/line_seed_fonts.rc"
  PARENT_SCOPE
)
