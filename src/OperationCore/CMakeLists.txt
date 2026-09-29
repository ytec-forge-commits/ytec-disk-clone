add_library(
  OperationCore
  STATIC
    src/checkpoint.cpp
    src/operation.cpp
    src/resume_slot.cpp
    src/sha256.cpp
    src/windows_resume_slot_platform.cpp
)

add_library(ytec::operation_core ALIAS OperationCore)

target_include_directories(
  OperationCore
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
  OperationCore
  PUBLIC
    ytec::clone_core
  PRIVATE
    bcrypt
)

ytec_configure_target(OperationCore)
