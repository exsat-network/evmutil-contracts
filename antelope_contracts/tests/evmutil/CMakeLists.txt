set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/contracts.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/contracts.hpp)

include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}
    ${EXTERNAL_DIR}/silkworm/third_party/intx/include
    ${EXTERNAL_DIR}/silkworm
    ${EXTERNAL_DIR}/silkworm/third_party/ethash/include
    ${EXTERNAL_DIR}/silkworm/third_party/evmone/include
    ${EXTERNAL_DIR}/silkworm/third_party/evmone/lib
    ${EXTERNAL_DIR}/silkworm/third_party/evmone/evmc/include
    ${EXTERNAL_DIR}/silkworm/third_party/secp256k1/include
    ${EXTERNAL_DIR}/expected/include
    ${SOLIDITY_BYTECODES_DIR}/
)

add_eosio_test_executable( evmutil_test
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/evmutil_tester.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/integrated_tests.cpp
    ${EXTERNAL_DIR}/silkworm/silkworm/core/rlp/encode.cpp
    ${EXTERNAL_DIR}/silkworm/silkworm/core/rlp/decode.cpp
    ${EXTERNAL_DIR}/silkworm/silkworm/core/types/transaction.cpp
    ${EXTERNAL_DIR}/silkworm/silkworm/core/types/y_parity_and_chain_id.cpp
    ${EXTERNAL_DIR}/silkworm/silkworm/core/types/account.cpp
    ${EXTERNAL_DIR}/silkworm/silkworm/core/common/util.cpp
    ${EXTERNAL_DIR}/silkworm/silkworm/core/common/endian.cpp
    ${EXTERNAL_DIR}/silkworm/silkworm/core/common/assert.cpp
    ${EXTERNAL_DIR}/silkworm/silkworm/core/execution/address.cpp
    ${EXTERNAL_DIR}/silkworm/silkworm/core/crypto/ecdsa.c
    ${EXTERNAL_DIR}/silkworm/third_party/ethash/lib/keccak/keccak.c
    ${EXTERNAL_DIR}/silkworm/third_party/ethash/lib/ethash/ethash.cpp
    ${EXTERNAL_DIR}/silkworm/third_party/ethash/lib/ethash/primes.c
)
add_dependencies(evmutil_test contracts)
# TODO: add back eos-vm-oc once change to disable EOS VM OC subjective limits during unit test are added
add_test(NAME evmutil_tests COMMAND evmutil_test --report_level=detailed --color_output )
