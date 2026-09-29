function(ytec_configure_target target_name)
  target_compile_definitions(
    ${target_name}
    PRIVATE
      UNICODE
      _UNICODE
      WIN32_LEAN_AND_MEAN
      NOMINMAX
  )

  target_compile_options(
    ${target_name}
    PRIVATE
      /W4
      /WX
      /permissive-
      /Zc:__cplusplus
      /utf-8
      /sdl
      /guard:cf
  )

  target_link_options(
    ${target_name}
    PRIVATE
      /DYNAMICBASE
      /NXCOMPAT
      /guard:cf
  )

  if(YTEC_ENABLE_ASAN)
    target_compile_options(${target_name} PRIVATE /fsanitize=address)
    target_link_options(${target_name} PRIVATE /fsanitize=address)
  endif()

  if(YTEC_ENABLE_MSVC_ANALYZE)
    target_compile_options(${target_name} PRIVATE /analyze /analyze:external-)
  endif()
endfunction()

