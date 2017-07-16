set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_unittest(CPPFormatTests
  CleanupTest.cpp
  FormatTest.cpp
  FormatTestComments.cpp
  FormatTestSelective.cpp
  NamespaceEndCommentsFixerTest.cpp
  SortIncludesTest.cpp
  UsingDeclarationsSorterTest.cpp
  )

target_link_libraries(CPPFormatTests
  clangBasic
  cppFormat
  clangFrontend
  clangRewrite
  clangToolingCore
  )
