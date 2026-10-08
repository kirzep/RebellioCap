function(rebelliocap_copy_runtime_dependencies target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "Unknown target: ${target}")
  endif()

  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:${target}> $<TARGET_RUNTIME_DLLS:${target}>
    COMMAND_EXPAND_LISTS
    COMMENT "Copying runtime dependencies for ${target}")
endfunction()

# vcpkg's FindFFMPEG exports import-library paths, not imported DLL targets.
# Copy precisely the pinned dynamic runtime libraries for this package.
function(rebelliocap_copy_ffmpeg_runtime target)
  foreach(component IN ITEMS avformat-63 avcodec-63 avutil-61 swresample-7 swscale-10 avfilter-12)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<IF:$<CONFIG:Debug>,debug/bin,bin>/${component}.dll"
        "$<TARGET_FILE_DIR:${target}>/${component}.dll")
  endforeach()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<IF:$<CONFIG:Debug>,debug/bin/zd.dll,bin/z.dll>"
      "$<TARGET_FILE_DIR:${target}>/$<IF:$<CONFIG:Debug>,zd.dll,z.dll>")
endfunction()
