#include <variant>
#include <eosio/asset.hpp>
#include <eosio/eosio.hpp>

using namespace eosio;

namespace gasfunds {
    /**
     * @brief GasFund Contract
     *
     * This contract is responsible for calculating and distributing rewards on the blockchain,
     * including regular block rewards and EVM gas fee rewards
     */
    class contract_actions {
      public:
      /**
       * @brief Evm claim action
       *
       * @param caller - the account that calls the method
       * @param proxy - proxy address
       * @param sender - sender address
       * @param receiver - receiver address
       * @param receiver_type - receiver type
       */
        [[eosio::action]]
        void evmclaim(const name& caller, const checksum160& proxy, const checksum160& sender, uint64_t receiver, const uint8_t receiver_type);

        /**
         * @brief Evm enf claim action
         *
         * @param caller - the account that calls the method
         * @param proxy - proxy address
         * @param sender - sender address
         */
        [[eosio::action]]
        void evmenfclaim(const name& caller, const checksum160& proxy, const checksum160& sender);

        /**
         * @brief Evm rams claim action
         *
         * @param caller - the account that calls the method
         * @param proxy - proxy address
         * @param sender - sender address
         */
        [[eosio::action]]
        void evmramsclaim(const name& caller, const checksum160& proxy, const checksum160& sender);
    };

    using evmclaim_action = action_wrapper<"evmclaim"_n, &contract_actions::evmclaim>;
    using evmenfclaim_action = action_wrapper<"evmenfclaim"_n, &contract_actions::evmenfclaim>;
    using evmramsclaim_action = action_wrapper<"evmramsclaim"_n, &contract_actions::evmramsclaim>;
}
