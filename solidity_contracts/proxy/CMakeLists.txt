generate_solidity_bytecode_target(
   CONTRACT_NAME Erc20Proxy
   CONTRACT_SOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/proxy.sol"
   CONTRACT_NAMESPACE "proxy"
   BYTECODE_HEADER_OUTPUT_PATH "${SOLIDITY_BYTECODES_DIR}/proxy/proxy_bytecode.hpp"
)

add_custom_target(GenerateProxyBytecode ALL
   DEPENDS Erc20Proxy
)
