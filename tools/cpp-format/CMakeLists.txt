set(LLVM_LINK_COMPONENTS support)

add_clang_tool(cpp-format
  ClangFormat.cpp
  )

set(CLANG_FORMAT_LIB_DEPS
  clangBasic
  cppFormat
  clangRewrite
  clangToolingCore
  )

target_link_libraries(cpp-format
  ${CLANG_FORMAT_LIB_DEPS}
  )

if( LLVM_USE_SANITIZE_COVERAGE )
  add_subdirectory(fuzzer)
endif()

install(PROGRAMS cpp-format-bbedit.applescript
  DESTINATION share/clang
  COMPONENT cpp-format)
install(PROGRAMS cpp-format-diff.py
  DESTINATION share/clang
  COMPONENT cpp-format)
install(PROGRAMS cpp-format-sublime.py
  DESTINATION share/clang
  COMPONENT cpp-format)
install(PROGRAMS cpp-format.el
  DESTINATION share/clang
  COMPONENT cpp-format)
install(PROGRAMS cpp-format.py
  DESTINATION share/clang
  COMPONENT cpp-format)
install(PROGRAMS git-clang-format
  DESTINATION bin
  COMPONENT cpp-format)
