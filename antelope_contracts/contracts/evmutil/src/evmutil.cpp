#include <evmutil/eosio.token.hpp>
#include <evmutil/evmutil.hpp>
#include <evmutil/hex.hpp>
#include <evmutil/evm_runtime.hpp>
#include <evmutil/endrmng.hpp>
#include <evmutil/gasfunds.hpp>
#include <evmutil/poolreg.hpp>
#include <evmutil/types.hpp>

#include <evmutil/reward_helper_bytecode.hpp>
#include <evmutil/stake_helper_bytecode.hpp>
#include <evmutil/gas_funds_bytecode.hpp>
#include <proxy/proxy_bytecode.hpp>

#include <silkworm/core/execution/address.hpp>
#include <silkworm/core/common/util.hpp>

namespace eosio {
   namespace internal_use_do_not_use {
      extern "C" {
         __attribute__((eosio_wasm_import))
         uint32_t get_code_hash(uint64_t account, uint32_t struct_version, char* data, uint32_t size);
      }
   }
} // namespace eosio


// Local helpers
namespace {

void readUint256(const evmutil::bytes &data, size_t offset, intx::uint256 &output) {
    auto read_uint256 = [&](const auto &data, size_t offset) -> intx::uint256 {
            uint8_t buffer[32]={};
            check(data.size() >= offset + 32, "not enough data in bridge_message_v0");
            memcpy(buffer, (void *)&(data[offset]), 32);
            return intx::be::load<intx::uint256>(buffer);
    };
    output = read_uint256(data, offset);
}

void readEvmAddress(const evmutil::bytes &data, size_t offset, evmc::address &output) {
    intx::uint256 value;
    readUint256(data, offset, value);
    check(value <= 0xffffFFFFffffFFFFffffFFFFffffFFFFffffFFFF_u256, "invalid evm address");

    memcpy(output.bytes, (void *)&(data[offset+ 32 - evmutil::kAddressLength]), evmutil::kAddressLength);
}

void readExSatAccount(const evmutil::bytes &data, size_t offset, uint64_t &output) {
    evmc::address dest_addr;
    readEvmAddress(data, offset, dest_addr);

    std::optional<uint64_t> dest_acc = silkworm::extract_reserved_address(dest_addr);
    check(!!dest_acc, "destination address in bridge_message_v0 must be reserved address");
    output = *dest_acc;
}

void readTokenAmount(const evmutil::bytes &data, size_t offset, uint64_t &output, uint64_t delta_precision) {
    intx::uint256 value;
    readUint256(data, offset, value);

    intx::uint256 mult = intx::exp(10_u256, intx::uint256(delta_precision));
    check(value % mult == 0_u256, "bridge amount can not have dust");
    value /= mult;

    output = (uint64_t)value;
    check(intx::uint256(output) == value && output < (1ull<<62)-1, "bridge amount value overflow");
    check(output > 0, "bridge amount must be positive");
}

checksum256 get_code_hash(name account) {
    char buff[64];

    eosio::check(internal_use_do_not_use::get_code_hash(account.value, 0, buff, sizeof(buff)) <= sizeof(buff), "get_code_hash() too big");
    using start_of_code_hash_return = std::tuple<unsigned_int, uint64_t, checksum256>;
    const auto& [v, s, code_hash] = unpack<start_of_code_hash_return>(buff, sizeof(buff));

    return code_hash;
}

inline uint128_t token_symbol_key(eosio::name token_contract, eosio::symbol_code symbol_code) {
    uint128_t v = token_contract.value;
    v <<= 64;
    v |= symbol_code.raw();
    return v;
}

template <size_t Size>
void initialize_data(evmutil::bytes& output, const unsigned char (&arr)[Size]) {
    static_assert(Size > 128); // ensure bytecode is compiled
    output.resize(Size);
    std::memcpy(output.data(), arr, Size);
}

} // namespace

namespace evmutil {

// Public Helpers

config_t evmutil::get_config() const {
    config_singleton_t config(get_self(), get_self().value);
    eosio::check(config.exists(), "evmutil config not exist");
    return config.get();
}

intx::uint256 evmutil::get_minimum_natively_representable(const config_t& config) const {
    return intx::exp(10_u256, intx::uint256(evm_precision - config.evm_gas_token_symbol.precision()));
}

void evmutil::set_config(const config_t &v) {
    config_singleton_t config(get_self(), get_self().value);
    config.set(v, get_self());
}

helpers_t evmutil::get_helpers() const {
    helpers_singleton_t helpers(get_self(), get_self().value);
    eosio::check(helpers.exists(), "evmutil config not exist");
    return helpers.get();
}

void evmutil::set_helpers(const helpers_t &v) {
    helpers_singleton_t helpers(get_self(), get_self().value);
    helpers.set(v, get_self());
}

// lookup nonce from the multi_index table of evm contract and assert
uint64_t evmutil::get_next_nonce() {

    config_t config = get_config();

    evm_runtime::next_nonce_table table(config.evm_account, config.evm_account.value);
    auto itr = table.find(receiver_account().value);
    uint64_t next_nonce = (itr == table.end() ? 0 : itr->next_nonce);

    evm_runtime::assertnonce_action act(config.evm_account, std::vector<eosio::permission_level>{});
    act.send(receiver_account(), next_nonce);
    return next_nonce;
}

// Actions

void evmutil::dpystakeimpl() {
    require_auth(get_self());

    bytes call_data;

    auto reserved_addr = silkworm::make_reserved_address(receiver_account().value);
    initialize_data(call_data, solidity::stakehelper::bytecode);

    bytes to = {};
    bytes value_zero;
    value_zero.resize(32, 0);

    uint64_t next_nonce = get_next_nonce();

    // required account opened in evm_runtime
    config_t config = get_config();
    evm_runtime::call_action call_act(config.evm_account, {{receiver_account(), "active"_n}});
    call_act.send(receiver_account(), to, value_zero, call_data, config.evm_init_gaslimit);

    evmc::address impl_addr = silkworm::create_address(reserved_addr, next_nonce);

    impl_contract_table_t contract_table(_self, _self.value);
    contract_table.emplace(_self, [&](auto &v) {
        v.id = contract_table.available_primary_key();
        v.address.resize(kAddressLength);
        memcpy(&(v.address[0]), impl_addr.bytes, kAddressLength);
    });

}

void evmutil::dpyrwdhelper() {
    require_auth(get_self());


    bytes call_data;

    auto reserved_addr = silkworm::make_reserved_address(receiver_account().value);
    initialize_data(call_data, solidity::rewardhelper::bytecode);

    bytes to = {};
    bytes value_zero;
    value_zero.resize(32, 0);

    uint64_t next_nonce = get_next_nonce();

    // required account opened in evm_runtime
    config_t config = get_config();
    evm_runtime::call_action call_act(config.evm_account, {{receiver_account(), "active"_n}});
    call_act.send(receiver_account(), to, value_zero, call_data, config.evm_init_gaslimit);

    evmc::address impl_addr = silkworm::create_address(reserved_addr, next_nonce);

    helpers_t helpers = get_helpers();
    helpers.reward_helper_address.resize(kAddressLength);
    memcpy(&(helpers.reward_helper_address[0]), impl_addr.bytes, kAddressLength);
    set_helpers(helpers);
}

void evmutil::setrwdhelper(std::string impl_address) {
    require_auth(get_self());
    auto address_bytes = from_hex(impl_address);
    eosio::check(!!address_bytes, "implementation address must be valid 0x EVM address");
    eosio::check(address_bytes->size() == kAddressLength, "invalid length of implementation address");

    helpers_t helpers = get_helpers();

    helpers.reward_helper_address.resize(kAddressLength);
    memcpy(&(helpers.reward_helper_address[0]), address_bytes->data(), kAddressLength);
    set_helpers(helpers);
}

void evmutil::setstakeimpl(std::string impl_address) {
    require_auth(get_self());
    auto address_bytes = from_hex(impl_address);
    eosio::check(!!address_bytes, "implementation address must be valid 0x EVM address");
    eosio::check(address_bytes->size() == kAddressLength, "invalid length of implementation address");

    uint64_t id = 0;
    impl_contract_table_t contract_table(_self, _self.value);

    contract_table.emplace(_self, [&](auto &v) {
        v.id = contract_table.available_primary_key();
        v.address.resize(kAddressLength);
        memcpy(&(v.address[0]), address_bytes->data(), kAddressLength);
    });
}

void evmutil::dpyvlddepbtc(std::string token_address, const eosio::asset &dep_fee, uint8_t erc20_precision) {
    require_auth(get_self());

    helpers_t helpers = get_helpers();
    eosio::check(!helpers.btc_deposit_address || helpers.btc_deposit_address.value().empty(), "cannot deploy again");

    impl_contract_table_t contract_table(_self, _self.value);
    eosio::check(contract_table.begin() != contract_table.end(), "no implementaion contract available");
    auto contract_itr = contract_table.end();
    --contract_itr;

    auto token_address_bytes = from_hex(token_address);
    eosio::check(!!token_address_bytes, "token address must be valid 0x EVM address");
    eosio::check(token_address_bytes->size() == kAddressLength, "invalid length of token address");

    bytes proxy_contract_addr = deploy_stake_helper_proxy(*token_address_bytes, contract_itr->address, dep_fee, erc20_precision, false, true);


    helpers.btc_deposit_address = proxy_contract_addr;
    set_helpers(helpers);
}

void evmutil::dpyvlddepsat(std::string token_address, const eosio::asset &dep_fee, uint8_t erc20_precision) {
    require_auth(get_self());

    helpers_t helpers = get_helpers();

    eosio::check(!helpers.xsat_deposit_address || helpers.xsat_deposit_address.value().empty(), "cannot deploy again");

    impl_contract_table_t contract_table(_self, _self.value);
    eosio::check(contract_table.begin() != contract_table.end(), "no implementaion contract available");
    auto contract_itr = contract_table.end();
    --contract_itr;

    auto token_address_bytes = from_hex(token_address);
    eosio::check(!!token_address_bytes, "token address must be valid 0x EVM address");
    eosio::check(token_address_bytes->size() == kAddressLength, "invalid length of token address");

    bytes proxy_contract_addr = deploy_stake_helper_proxy(*token_address_bytes, contract_itr->address, dep_fee, erc20_precision, true, true);

    helpers.xsat_deposit_address = proxy_contract_addr;
    set_helpers(helpers);
}

bytes evmutil::deploy_stake_helper_proxy(const bytes& erc20_address_bytes, const bytes& impl_address_bytes, const eosio::asset& dep_fee, uint8_t erc20_precision, bool notBTC, bool isValidatorDeposits) {
    eosio::check(impl_address_bytes.size() == kAddressLength, "invalid length of implementation address");

    config_t config = get_config();

    // 2^(256-64) = 6.2e+57, so the precision diff is at most 57
    eosio::check(erc20_precision >= dep_fee.symbol.precision() &&
    erc20_precision <= dep_fee.symbol.precision() + 57, "evmutil precision out of range");

    eosio::check(dep_fee.symbol == config.evm_gas_token_symbol, "deposit_fee should have native token symbol");
    intx::uint256 dep_fee_evm = dep_fee.amount;
    dep_fee_evm *= get_minimum_natively_representable(config);

    auto reserved_addr = silkworm::make_reserved_address(receiver_account().value);
    auto evm_reserved_addr = silkworm::make_reserved_address(config.evm_account.value);

    bytes call_data;
    initialize_data(call_data, solidity::proxy::bytecode);

    // constructor(address evmutil_impl_contract, memory _data)
    call_data.insert(call_data.end(), 32 - kAddressLength, 0);  // padding for address
    call_data.insert(call_data.end(), impl_address_bytes.begin(), impl_address_bytes.end());

    bytes constructor_data;
    // initialize(address,address,address,uint256,bool,bool) => 1d1e7e92
    uint8_t func_[4] = {0x1d,0x1e,0x7e,0x92};
    constructor_data.insert(constructor_data.end(), func_, func_ + sizeof(func_));

    auto pack_uint256 = [&](bytes &ds, const intx::uint256 &val) {
        uint8_t val_[32] = {};
        intx::be::store(val_, val);
        ds.insert(ds.end(), val_, val_ + sizeof(val_));
    };
    auto pack_uint32 = [&](bytes &ds, uint32_t val) {
        uint8_t val_[32] = {};
        val_[28] = (uint8_t)(val >> 24);
        val_[29] = (uint8_t)(val >> 16);
        val_[30] = (uint8_t)(val >> 8);
        val_[31] = (uint8_t)val;
        ds.insert(ds.end(), val_, val_ + sizeof(val_));
    };
    auto pack_string = [&](bytes &ds, const auto &str) {
        pack_uint32(ds, (uint32_t)str.size());
        for (size_t i = 0; i < (str.size() + 31) / 32 * 32; i += 32) {
            uint8_t name_[32] = {};
            memcpy(name_, str.data() + i, i + 32 > str.size() ? str.size() - i : 32);
            ds.insert(ds.end(), name_, name_ + sizeof(name_));
        }
    };

    constructor_data.insert(constructor_data.end(), 32 - kAddressLength, 0);  // padding for address
    constructor_data.insert(constructor_data.end(), reserved_addr.bytes, reserved_addr.bytes + kAddressLength);
    constructor_data.insert(constructor_data.end(), 32 - kAddressLength, 0);  // padding for address
    constructor_data.insert(constructor_data.end(), evm_reserved_addr.bytes, evm_reserved_addr.bytes + kAddressLength);
    constructor_data.insert(constructor_data.end(), 32 - kAddressLength, 0);  // padding for address
    constructor_data.insert(constructor_data.end(), erc20_address_bytes.begin(), erc20_address_bytes.end());

    pack_uint256(constructor_data, dep_fee_evm);          // offset 32

    pack_uint256(constructor_data, notBTC?1:0);   // notBTC
    pack_uint256(constructor_data, isValidatorDeposits?1:0);   // isValidatorDeposits

    pack_uint32(call_data, 64);                  // offset 32
    pack_string(call_data, constructor_data);    // offset 64

    bytes to = {};
    bytes value_zero;
    value_zero.resize(32, 0);

    uint64_t next_nonce = get_next_nonce();

    // required account opened in evm_runtime
    evm_runtime::call_action call_act(config.evm_account, {{receiver_account(), "active"_n}});
    call_act.send(receiver_account(), to, value_zero, call_data, config.evm_init_gaslimit);

    evmc::address proxy_contract_addr = silkworm::create_address(reserved_addr, next_nonce);
    bytes result;
    result.resize(kAddressLength, 0);
    memcpy(&(result[0]), proxy_contract_addr.bytes, kAddressLength);
    return result;
}

void evmutil::regtokenwithcodebytes(const bytes& erc20_address_bytes, const bytes& impl_address_bytes, const eosio::asset& dep_fee, uint8_t erc20_precision) {
    require_auth(get_self());

    token_table_t token_table(_self, _self.value);
    auto index_symbol = token_table.get_index<"by.tokenaddr"_n>();
    check(index_symbol.find(make_key(erc20_address_bytes)) == index_symbol.end(), "token already registered");

    bytes proxy_contract_addr = deploy_stake_helper_proxy(erc20_address_bytes, impl_address_bytes, dep_fee, erc20_precision, false, false);

    token_table.emplace(_self, [&](auto &v) {
        v.id = token_table.available_primary_key();
        v.address.resize(kAddressLength, 0);
        memcpy(&(v.address[0]), proxy_contract_addr.data(), kAddressLength);
        v.erc20_precision = erc20_precision;
        v.token_address.resize(kAddressLength, 0);
        memcpy(&(v.token_address[0]), erc20_address_bytes.data(), kAddressLength);
    });
}

void evmutil::regwithcode(std::string token_address, std::string impl_address, const eosio::asset &dep_fee, uint8_t erc20_precision) {
    require_auth(get_self());
    auto address_bytes = from_hex(impl_address);
    eosio::check(!!address_bytes, "implementation address must be valid 0x EVM address");
    eosio::check(address_bytes->size() == kAddressLength, "invalid length of implementation address");

    auto token_address_bytes = from_hex(token_address);
    eosio::check(!!token_address_bytes, "token address must be valid 0x EVM address");
    eosio::check(token_address_bytes->size() == kAddressLength, "invalid length of token address");

    regtokenwithcodebytes(*token_address_bytes, *address_bytes, dep_fee, erc20_precision);
}

void evmutil::regtoken(std::string token_address, const eosio::asset &dep_fee, uint8_t erc20_precision) {
    require_auth(get_self());

    impl_contract_table_t contract_table(_self, _self.value);
    eosio::check(contract_table.begin() != contract_table.end(), "no implementaion contract available");
    auto contract_itr = contract_table.end();
    --contract_itr;

    auto token_address_bytes = from_hex(token_address);
    eosio::check(!!token_address_bytes, "token address must be valid 0x EVM address");
    eosio::check(token_address_bytes->size() == kAddressLength, "invalid length of token address");

    regtokenwithcodebytes(*token_address_bytes, contract_itr->address, dep_fee, erc20_precision);
}

void evmutil::unregtoken(std::string proxy_address) {
    require_auth(get_self());

    auto proxy_address_bytes = from_hex(proxy_address);
    eosio::check(!!proxy_address_bytes, "token address must be valid 0x EVM address");
    eosio::check(proxy_address_bytes->size() == kAddressLength, "invalid length of token address");

    token_table_t token_table(_self, _self.value);
    auto index_symbol = token_table.get_index<"by.tokenaddr"_n>();
    auto token_table_iter = index_symbol.find(make_key(*proxy_address_bytes));
    eosio::check(token_table_iter != index_symbol.end(), "token not registered");

    index_symbol.erase(token_table_iter);
}

void evmutil::handle_endorser_stakes(const bridge_message_v0 &msg, uint64_t delta_precision, bool is_deposit, bool is_xsat) {

    check(msg.data.size() >= 4, "not enough data in bridge_message_v0");
    config_t config = get_config();

    uint32_t app_type = 0;
    memcpy((void *)&app_type, (const void *)&(msg.data[0]), sizeof(app_type));
    // 0xdc4653f4 : f45346dc : deposit(address,uint256,address)
    // 0xec8d3269 : 69328dec : withdraw(address,uint256,address)
    // 0x42b3c021 : 21c0b342 : claim(address,address)
    // 0x2b7d501d : 1d507d2b : restake(address,address,address)
    // 0xac2fd4fc : fcd42fac : claim2(address,address,uint256)

    if (app_type == 0x42b3c021) /* claim(address,address) */{
        check(msg.data.size() >= 4 + 32 /*to*/ + 32 /*from*/,
            "not enough data in bridge_message_v0 of application type 0x653332e5");

        uint64_t dest_acc;
        readExSatAccount(msg.data, 4, dest_acc);

        evmc::address sender_addr;
        readEvmAddress(msg.data, 4 + 32, sender_addr);

        // Use same claim
        endrmng::evmclaim_action evmclaim_act(config.endrmng_account, {{receiver_account(), "active"_n}});
        evmclaim_act.send(get_self(), make_key160(msg.sender), make_key160(sender_addr.bytes, kAddressLength), dest_acc);
    } else if (app_type == 0xac2fd4fc) /* claim2(address,address,uint256) */{
        check(msg.data.size() >= 4 + 32 /*to*/ + 32 /*from*/ + 32 /*donate_rate*/,
            "not enough data in bridge_message_v0 of application type 0xac2fd4fc");

        uint64_t dest_acc;
        readExSatAccount(msg.data, 4, dest_acc);

        evmc::address sender_addr;
        readEvmAddress(msg.data, 4 + 32, sender_addr);

        intx::uint256 value;
        readUint256(msg.data, 4 + 32 + 32, value);

        check(value <= 10000, "donate rate must smaller than 10000");

        uint16_t donate_rate = (uint16_t)value;

        endrmng::evmclaim2_action evmclaim2_act(config.endrmng_account, {{receiver_account(), "active"_n}});
        evmclaim2_act.send(get_self(), make_key160(msg.sender), make_key160(sender_addr.bytes, kAddressLength), dest_acc, donate_rate);
    } else if (app_type == 0xdc4653f4) /* deposit(address,uint256,address) */{
        check(msg.data.size() >= 4 + 32 + 32 + 32,
            "not enough data in bridge_message_v0 of application type 0xdc4653f4");

        uint64_t dest_acc;
        readExSatAccount(msg.data, 4, dest_acc);

        uint64_t dest_amount = 0;
        readTokenAmount(msg.data, 4 + 32, dest_amount, delta_precision);

        evmc::address sender_addr;
        readEvmAddress(msg.data, 4 + 32 + 32, sender_addr);

        if (is_xsat) {
            endrmng::evmstakexsat_action evmstakexsat_act(config.endrmng_account, {{receiver_account(), "active"_n}});
            evmstakexsat_act.send(get_self(), make_key160(msg.sender),make_key160(sender_addr.bytes, kAddressLength), dest_acc, eosio::asset(dest_amount, default_xsat_token_symbol));
        }
        else {
            endrmng::evmstake_action evmstake_act(config.endrmng_account, {{receiver_account(), "active"_n}});
            evmstake_act.send(get_self(), make_key160(msg.sender),make_key160(sender_addr.bytes, kAddressLength), dest_acc, eosio::asset(dest_amount, config.evm_gas_token_symbol));
        }

    } else if (app_type == 0xec8d3269) /* withdraw(address,uint256,address) */ {
        check(msg.data.size() >= 4 + 32 + 32 + 32,
            "not enough data in bridge_message_v0 of application type 0xec8d3269");

        uint64_t dest_acc;
        readExSatAccount(msg.data, 4, dest_acc);

        uint64_t dest_amount = 0;
        readTokenAmount(msg.data, 4 + 32, dest_amount, delta_precision);

        evmc::address sender_addr;
        readEvmAddress(msg.data, 4 + 32 + 32, sender_addr);

        if (is_xsat) {
            endrmng::evmunstkxsat_action evmunstkxsat_act(config.endrmng_account, {{receiver_account(), "active"_n}});
            evmunstkxsat_act.send(get_self(), make_key160(msg.sender), make_key160(sender_addr.bytes, kAddressLength), dest_acc, eosio::asset(dest_amount, default_xsat_token_symbol));
        }
        else {
            endrmng::evmunstake_action evmunstake_act(config.endrmng_account, {{receiver_account(), "active"_n}});
            evmunstake_act.send(get_self(), make_key160(msg.sender), make_key160(sender_addr.bytes, kAddressLength), dest_acc, eosio::asset(dest_amount, config.evm_gas_token_symbol));
        }
    } else if (app_type == 0x2b7d501d) /* restake(address,address,uint256,address) */{
        check(msg.data.size() >= 4 + 32 + 32 + 32 + 32,
            "not enough data in bridge_message_v0 of application type 0x97fba943");

        uint64_t from_acc;
        readExSatAccount(msg.data, 4, from_acc);

        uint64_t to_acc;
        readExSatAccount(msg.data, 4 + 32, to_acc);

        uint64_t dest_amount = 0;
        readTokenAmount(msg.data, 4 + 32 + 32, dest_amount, delta_precision);

        evmc::address sender_addr;
        readEvmAddress(msg.data, 4 + 32 + 32 + 32, sender_addr);

        if (is_xsat || is_deposit ) {
            // There's no valid routine to reach here by current design.
            // Assert here for extra protection.
            eosio::check(false, "invalid operation");
        }
        else {
            endrmng::evmnewstake_action evmnewstake_act(config.endrmng_account, {{receiver_account(), "active"_n}});
            evmnewstake_act.send(get_self(), make_key160(msg.sender),make_key160(sender_addr.bytes, kAddressLength), from_acc, to_acc, eosio::asset(dest_amount, config.evm_gas_token_symbol));
        }
    } else {
        eosio::check(false, "unsupported bridge_message version");
    }
}

void evmutil::handle_utxo_access(const bridge_message_v0 &msg) {

}

void evmutil::handle_rewards(const bridge_message_v0 &msg) {
    config_t config = get_config();
    check(msg.data.size() >= 4, "not enough data in bridge_message_v0");

    uint32_t app_type = 0;
    memcpy((void *)&app_type, (const void *)&(msg.data[0]), sizeof(app_type));

    // 0x42b3c021 : 21c0b342 : claim(address,address)
    // 0xc16fb607 : 07b66fc1 : vdrclaim(address,address)
    // 0x3d7bb560 : 60b57b3d : creditclaim(address,address,address)
    if (app_type == 0x42b3c021) {
        check(msg.data.size() >= 4 + 32 /*to*/ + 32 /*from*/,
            "not enough data in bridge_message_v0 of application type 0x653332e5");

        uint64_t dest_acc;
        readExSatAccount(msg.data, 4, dest_acc);

        // Note that there's a second argument in the call for the sender address.
        // We currently do not use it. But we collect in the bridge call in case we want to add more sanity checks here.

        poolreg::claim_action claim_act(config.poolreg_account, {{receiver_account(), "active"_n}});
        // seems hit some bug/limitation in the template, need an explicit conversion here.
        claim_act.send(eosio::name(dest_acc));
    } else if (app_type == 0xc16fb607) {
        check(msg.data.size() >= 4 + 32 /*to*/ + 32 /*from*/,
            "not enough data in bridge_message_v0 of application type 0x653332e5");

        uint64_t dest_acc;
        readExSatAccount(msg.data, 4, dest_acc);

        // Note that there's a second argument in the call for the sender address.
        // We currently do not use it. But we collect in the bridge call in case we want to add more sanity checks here.

        endrmng::vdrclaim_action vdrclaim_act(config.endrmng_account, {{receiver_account(), "active"_n}});
        // seems hit some bug/limitation in the template, need an explicit conversion here.
        vdrclaim_act.send(eosio::name(dest_acc));


    } else if (app_type == 0x3d7bb560) /* creditclaim(address,address,address) */{
        check(msg.data.size() >= 4 + 32 /*to*/ + 32 /*proxy*/ + 32 /*from*/,
            "not enough data in bridge_message_v0 of application type 0x653332e5");

        uint64_t dest_acc;
        readExSatAccount(msg.data, 4, dest_acc);

        evmc::address proxy_addr;
        readEvmAddress(msg.data, 4 + 32, proxy_addr);

        evmc::address sender_addr;
        readEvmAddress(msg.data, 4 + 32 + 32, sender_addr);

        endrmng::evmclaim_action evmclaim_act(config.endrmng_account, {{receiver_account(), "active"_n}});
        evmclaim_act.send(get_self(), make_key160(proxy_addr.bytes, kAddressLength), make_key160(sender_addr.bytes, kAddressLength), dest_acc);
    }
    else {
        eosio::check(false, "unsupported bridge_message version");
    }
}

void evmutil::transfer(eosio::name from, eosio::name to, eosio::asset quantity,
                     std::string memo) {
    if (to != get_self() || from == get_self()) return;
    eosio::check(false, "this address should not accept tokens");
}

void evmutil::onbridgemsg(const bridge_message_t &message) {
    config_t config = get_config();

    check(get_sender() == config.evm_account, "invalid sender of onbridgemsg");

    const bridge_message_v0 &msg = std::get<bridge_message_v0>(message);
    check(msg.receiver == receiver_account(), "invalid message receiver");

    helpers_t helpers = get_helpers();

    if (helpers.reward_helper_address == msg.sender) {
        handle_rewards(msg);
    }
    else if (helpers.btc_deposit_address && helpers.btc_deposit_address.value() == msg.sender) {
        // Reuse old logic. We KNOW the target is XBTC and delta-precision is 10
        handle_endorser_stakes(msg, 10, true, false);
    }
    else if (helpers.xsat_deposit_address && helpers.xsat_deposit_address.value() == msg.sender) {
        // Reuse old logic. We KNOW the target is XSAT and delta-precision is 10
        handle_endorser_stakes(msg, 10, true, true);
    }
    else if (helpers.gas_funds_address && helpers.gas_funds_address.value() == msg.sender){
        handle_gasfunds(msg);
    }
    else {
        checksum256 addr_key = make_key(msg.sender);
        token_table_t token_table(_self, _self.value);
        auto index = token_table.get_index<"by.address"_n>();
        auto itr = index.find(addr_key);

        check(itr != index.end() && itr->address == msg.sender, "ERC-20 token not registerred");

        handle_endorser_stakes(msg, itr->erc20_precision - config.evm_gas_token_symbol.precision(), false, false);
    }
}

void evmutil::init(eosio::name evm_account, eosio::symbol gas_token_symbol, uint64_t gaslimit, uint64_t init_gaslimit) {
    require_auth(get_self());

    config_singleton_t config_table(get_self(), get_self().value);
    eosio::check(!config_table.exists(), "evmutil config already initialized");

    config_t config;
    token_table_t token_table(_self, _self.value);
    if (token_table.begin() != token_table.end()) {
        eosio::check(evm_account == default_evm_account && gas_token_symbol == default_native_token_symbol, "can only init with native EOS symbol");
    }
    config.evm_account = evm_account;
    config.evm_gas_token_symbol = gas_token_symbol;
    config.evm_gaslimit = gaslimit;
    config.evm_init_gaslimit = init_gaslimit;
    set_config(config);

    helpers_t helpers;
    set_helpers(helpers);
}

void evmutil::setgaslimit(std::optional<uint64_t> gaslimit, std::optional<uint64_t> init_gaslimit) {
    require_auth(get_self());

    config_t config = get_config();
    if (gaslimit.has_value()) config.evm_gaslimit = *gaslimit;
    if (init_gaslimit.has_value()) config.evm_init_gaslimit = *init_gaslimit;
    set_config(config);
}

void evmutil::setdepfee(std::string proxy_address, const eosio::asset &fee) {
    require_auth(get_self());

    config_t config = get_config();

    eosio::check(fee.symbol == config.evm_gas_token_symbol, "deposit_fee should have native token symbol");

    auto address_bytes = from_hex(proxy_address);
    eosio::check(!!address_bytes, "token address must be valid 0x EVM address");
    eosio::check(address_bytes->size() == kAddressLength, "invalid length of token address");

    checksum256 addr_key = make_key(*address_bytes);
    token_table_t token_table(_self, _self.value);
    auto index = token_table.get_index<"by.address"_n>();
    auto token_table_iter = index.find(addr_key);

    check(token_table_iter != index.end() && token_table_iter->address == address_bytes, "ERC-20 token not registerred");

    intx::uint256 fee_evm = fee.amount;
    fee_evm *= get_minimum_natively_representable(config);

    auto pack_uint256 = [&](bytes &ds, const intx::uint256 &val) {
        uint8_t val_[32] = {};
        intx::be::store(val_, val);
        ds.insert(ds.end(), val_, val_ + sizeof(val_));
    };

    bytes call_data;
    // sha(setFee(uint256)) == 0x69fe0e2d
    uint8_t func_[4] = {0x69,0xfe,0x0e,0x2d};
    call_data.insert(call_data.end(), func_, func_ + sizeof(func_));
    pack_uint256(call_data, fee_evm);

    bytes value_zero;
    value_zero.resize(32, 0);

    evm_runtime::call_action call_act(config.evm_account, {{receiver_account(), "active"_n}});
    call_act.send(receiver_account(), token_table_iter->address, value_zero, call_data, config.evm_gaslimit);
}

void evmutil::setlocktime(std::string proxy_address, uint64_t locktime) {
    require_auth(get_self());

    config_t config = get_config();


    auto address_bytes = from_hex(proxy_address);
    eosio::check(!!address_bytes, "token address must be valid 0x EVM address");
    eosio::check(address_bytes->size() == kAddressLength, "invalid length of token address");

    helpers_t helpers = get_helpers();

    if (!((helpers.btc_deposit_address && helpers.btc_deposit_address.value() == *address_bytes) ||
        (helpers.xsat_deposit_address && helpers.xsat_deposit_address.value() == *address_bytes))) {
        checksum256 addr_key = make_key(*address_bytes);
        token_table_t token_table(_self, _self.value);
        auto index = token_table.get_index<"by.address"_n>();
        auto token_table_iter = index.find(addr_key);

        check(token_table_iter != index.end() && token_table_iter->address == address_bytes, "ERC-20 token not registerred");
    }

    auto pack_uint256 = [&](bytes &ds, const intx::uint256 &val) {
        uint8_t val_[32] = {};
        intx::be::store(val_, val);
        ds.insert(ds.end(), val_, val_ + sizeof(val_));
    };

    bytes call_data;
    // sha(setLockTime(uint256)) == 0xae04d45d
    uint8_t func_[4] = {0xae,0x04,0xd4,0x5d};
    call_data.insert(call_data.end(), func_, func_ + sizeof(func_));
    pack_uint256(call_data, intx::uint256(locktime));

    bytes value_zero;
    value_zero.resize(32, 0);

    evm_runtime::call_action call_act(config.evm_account, {{receiver_account(), "active"_n}});
    call_act.send(receiver_account(), *address_bytes, value_zero, call_data, config.evm_gaslimit);
}

void evmutil::upstakeimpl(std::string proxy_address) {
    require_auth(get_self());

    config_t config = get_config();

    auto address_bytes = from_hex(proxy_address);
    eosio::check(!!address_bytes, "token address must be valid 0x EVM address");
    eosio::check(address_bytes->size() == kAddressLength, "invalid length of token address");

    helpers_t helpers = get_helpers();

    if (!((helpers.btc_deposit_address && helpers.btc_deposit_address.value() == *address_bytes) ||
        (helpers.xsat_deposit_address && helpers.xsat_deposit_address.value() == *address_bytes))) {
        checksum256 addr_key = make_key(*address_bytes);
        token_table_t token_table(_self, _self.value);
        auto index = token_table.get_index<"by.address"_n>();
        auto token_table_iter = index.find(addr_key);

        check(token_table_iter != index.end() && token_table_iter->address == address_bytes, "ERC-20 token not registerred");
    }

    impl_contract_table_t contract_table(_self, _self.value);
    eosio::check(contract_table.begin() != contract_table.end(), "no implementaion contract available");
    auto contract_itr = contract_table.end();
    --contract_itr;


    auto pack_uint32 = [&](bytes &ds, uint32_t val) {
        uint8_t val_[32] = {};
        val_[28] = (uint8_t)(val >> 24);
        val_[29] = (uint8_t)(val >> 16);
        val_[30] = (uint8_t)(val >> 8);
        val_[31] = (uint8_t)val;
        ds.insert(ds.end(), val_, val_ + sizeof(val_));
    };

    bytes call_data;
    // sha(upgradeToAndCall(address,bytes)) == 4f1ef286
    uint8_t func_[4] = {0x4f,0x1e,0xf2,0x86};
    call_data.insert(call_data.end(), func_, func_ + sizeof(func_));


    call_data.insert(call_data.end(), 32 - kAddressLength, 0);  // padding for address offset 0
    call_data.insert(call_data.end(), contract_itr->address.begin(), contract_itr->address.end());

    pack_uint32(call_data, 64);                      // offset 32
    pack_uint32(call_data, 0);                      // offset 64

    bytes value_zero;
    value_zero.resize(32, 0);

    evm_runtime::call_action call_act(config.evm_account, {{receiver_account(), "active"_n}});
    call_act.send(receiver_account(), *address_bytes, value_zero, call_data, config.evm_gaslimit);
}

void evmutil::dpygasfunds() {
    require_auth(get_self());
    config_t config = get_config();

    bytes call_data;

    auto reserved_addr = silkworm::make_reserved_address(receiver_account().value);


    initialize_data(call_data, solidity::gasfunds::bytecode);

    bytes to = {};
    bytes value_zero;
    value_zero.resize(32, 0);

    uint64_t next_nonce = get_next_nonce();

    // required account opened in evm_runtime
    evm_runtime::call_action call_act(config.evm_account, {{receiver_account(), "active"_n}});

    call_act.send(receiver_account(), to, value_zero, call_data, config.evm_init_gaslimit);

    evmc::address impl_addr = silkworm::create_address(reserved_addr, next_nonce);

    helpers_t helpers = get_helpers();

    bytes impl_addr_bytes;
    impl_addr_bytes.resize(kAddressLength, 0);
    memcpy(&(impl_addr_bytes[0]), impl_addr.bytes, kAddressLength);
    helpers.gas_funds_address = impl_addr_bytes;
    set_helpers(helpers);
}
void evmutil::initgasfund() {
    require_auth(get_self());

    config_t config = get_config();

    if (!config.gasfund_account.has_value()) {
        config.gasfund_account = default_gasfund_account;
    }
    set_config(config);
}

void evmutil::setgasfunds(std::string impl_address) {
    require_auth(get_self());
    auto address_bytes_opt = from_hex(impl_address);
    eosio::check(!!address_bytes_opt, "implementation address must be valid 0x EVM address");
    eosio::check(address_bytes_opt->size() == kAddressLength, "invalid length of implementation address");

    helpers_t helpers = get_helpers();
    const auto& address_bytes = address_bytes_opt.value();
    helpers.gas_funds_address = bytes(address_bytes.begin(), address_bytes.end());

    set_helpers(helpers);
}
void evmutil::handle_gasfunds(const bridge_message_v0 &msg) {
    config_t config = get_config();
    check(msg.data.size() >= 4, "not enough data in bridge_message_v0");

    uint32_t app_type = 0;
    memcpy((void *)&app_type, (const void *)&(msg.data[0]), sizeof(app_type));

    // 0x136f93b4 : 0xb4936f13 : claim(address,address,uint8)
    // 0x4380f533 : 33f58043 : enfClaim(address)
    // 0x031a7229 : 29721a03 : ramsClaim(address)
    if (app_type == 0x136f93b4) {
        check(msg.data.size() >= 4 + 32 /*to*/ + 32 /*from*/ ,
            "not enough data in bridge_message_v0 of application type 0x42b3c021");

        uint64_t dest_acc;
        readExSatAccount(msg.data, 4, dest_acc);

        evmc::address sender_addr;
        readEvmAddress(msg.data, 4 + 32, sender_addr);

        intx::uint256 receiver_type;
        readUint256(msg.data, 4 + 32 + 32, receiver_type);

        gasfunds::evmclaim_action evmclaim_act(config.gasfund_account.value(), {{receiver_account(), "active"_n}});
        evmclaim_act.send(get_self(), make_key160(msg.sender),make_key160(sender_addr.bytes, kAddressLength), dest_acc, receiver_type);
    } else if (app_type == 0x4380f533) /* enfClaim(address) */ {
        check(msg.data.size() >= 4 + 32 /*from*/,
            "not enough data in bridge_message_v0 of application type 0x33f58043");

        evmc::address dest_acc;
        readEvmAddress(msg.data, 4, dest_acc);

        // Note that there's a second argument in the call for the sender address.
        // We currently do not use it. But we collect in the bridge call in case we want to add more sanity checks here.

        gasfunds::evmenfclaim_action evmenfclaim_act(config.gasfund_account.value(), {{receiver_account(), "active"_n}});
        // seems hit some bug/limitation in the template, need an explicit conversion here.
        evmenfclaim_act.send(get_self(), make_key160(msg.sender), make_key160(dest_acc.bytes, kAddressLength));


    } else if (app_type == 0x031a7229) /* ramsClaim(address) */{
        check(msg.data.size() >= 4 + 32 /*from*/,
            "not enough data in bridge_message_v0 of application type 0x29721a03");

        evmc::address dest_acc;
        readEvmAddress(msg.data, 4, dest_acc);

        // Note that there's a second argument in the call for the sender address.
        // We currently do not use it. But we collect in the bridge call in case we want to add more sanity checks here.

        gasfunds::evmramsclaim_action evmramsclaim_act(config.gasfund_account.value(), {{receiver_account(), "active"_n}});
        // seems hit some bug/limitation in the template, need an explicit conversion here.
        evmramsclaim_act.send(get_self(), make_key160(msg.sender), make_key160(dest_acc.bytes, kAddressLength));
    }
    else {
        eosio::check(false, "unsupported bridge_message version");
    }
}


inline eosio::name evmutil::receiver_account()const {
    return get_self();
}

}  // namespace evmutil
