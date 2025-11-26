set(LLVM_LINK_COMPONENTS
  Core
  IRReader
  Support
  Analysis
  Passes
  SupportLSP
  TransformUtils
  AsmParser
  )

add_llvm_unittest(LspTests
    OptRunner.cpp
  )

target_compile_features(LspTests PRIVATE cxx_std_20)
target_link_libraries(LspTests PRIVATE LLVMTestingSupport)
