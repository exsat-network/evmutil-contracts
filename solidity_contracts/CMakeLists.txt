add_subdirectory(compile_tools)

add_subdirectory(proxy)
add_subdirectory(evmutil)
add_subdirectory(utils)

add_custom_target(GenerateAllBytecode ALL
   DEPENDS GenerateProxyBytecode
   DEPENDS GenerateUtilsBytecode
   DEPENDS GenerateEvmUtilBytecode
)
