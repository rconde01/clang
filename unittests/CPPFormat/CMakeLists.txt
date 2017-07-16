set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_unittest(FormatTests
  CleanupTest.cpp
  FormatTest.cpp
  FormatTestComments.cpp
  FormatTestSelective.cpp
  NamespaceEndCommentsFixerTest.cpp
  SortIncludesTest.cpp
  UsingDeclarationsSorterTest.cpp
  )

target_link_libraries(FormatTests
  clangBasic
  cppFormat
  clangFrontend
  clangRewrite
  clangToolingCore
  )
