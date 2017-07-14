option(BUILD_CPP_FORMAT_VS_PLUGIN "Build cpp-format VS plugin" OFF)
if (BUILD_CPP_FORMAT_VS_PLUGIN)
  add_custom_target(cpp_format_exe_for_vsix
      ${CMAKE_COMMAND} -E copy_if_different
      "${LLVM_TOOLS_BINARY_DIR}/cpp-format.exe"
      "${CMAKE_CURRENT_SOURCE_DIR}/ClangFormat/clang-format.exe"
      DEPENDS clang-format)

  # Build number added to Clang version to ensure that new VSIX can be upgraded
  string(TIMESTAMP CPP_FORMAT_VSIX_BUILD %y%m%d%H%M UTC)

  if (NOT CPP_FORMAT_VS_VERSION)
    set(CPP_FORMAT_VS_VERSION "${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}.${LLVM_VERSION_PATCH}.${CLANG_CPP_VSIX_BUILD}")
  endif()

  configure_file("source.extension.vsixmanifest.in"
      "${CMAKE_CURRENT_SOURCE_DIR}/ClangFormat/source.extension.vsixmanifest")

  find_program(NUGET_EXE nuget PATHS ${NUGET_EXE_DIR})
  if (NOT NUGET_EXE)
    message(FATAL_ERROR "Could not find nuget.exe. Download from https://www.nuget.org/nuget.exe"
                        " and add parent directory to PATH or pass it via NUGET_EXE_DIR var.")
  endif()

  add_custom_target(cpp_format_vsix ALL
      COMMAND ${NUGET_EXE} restore "${CMAKE_CURRENT_SOURCE_DIR}/ClangFormat.sln"
      COMMAND devenv "${CMAKE_CURRENT_SOURCE_DIR}/ClangFormat.sln" /Build Release
      DEPENDS cpp_format_exe_for_vsix "${CMAKE_CURRENT_SOURCE_DIR}/ClangFormat/source.extension.vsixmanifest"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/ClangFormat/bin/Release/ClangFormat.vsix"
      "${LLVM_TOOLS_BINARY_DIR}/ClangFormat.vsix"
      DEPENDS cpp_format_exe_for_vsix)
endif()
