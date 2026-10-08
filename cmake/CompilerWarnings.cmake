function(rebelliocap_enable_warnings target)
  if(NOT MSVC)
    message(FATAL_ERROR "RebellioCap warning policy requires MSVC.")
  endif()

  target_compile_options(${target} INTERFACE
    /W4
    /WX
    /permissive-
    /Zc:__cplusplus
    /utf-8
    /sdl)
  target_link_options(${target} INTERFACE
    /DYNAMICBASE
    /NXCOMPAT
    /guard:cf)
endfunction()
