// SPDX-License-Identifier: MIT

pragma solidity ^0.8.18;

// Gas Funds
contract GasFunds {
    string public linkedEOSAccountName;
    address public evmAddress;

    constructor() {
        linkedEOSAccountName = "evmutil.xsat";
        evmAddress = 0xBBbBbbbbbBbbbbBBBBbbbBbb56e40ee0D9000000; //evm.xsat
    }

    function _bridgeCall(bytes memory receiver_msg) private {
        // 注释保留在辅助函数中
        // The action is aynchronously viewed from EVM and looks UNSAFE.
        // BUT in fact the call will be executed as inline action.
        // If the cross chain call fail, the whole tx including the EVM action will be rejected.
        (bool success, ) = evmAddress.call(
            abi.encodeWithSignature(
                "bridgeMsgV0(string,bool,bytes)",
                linkedEOSAccountName,
                true,
                receiver_msg
            )
        );
        if (!success) {
            revert();
        }
    }

    function claim(address _target, uint8 _receiver_type) external {
        bytes memory receiver_msg = abi.encodeWithSignature(
            "claim(address,address,uint8)",
            _target,
            msg.sender,
            _receiver_type
        );
        _bridgeCall(receiver_msg);
    }

    function enfClaim() external {
        bytes memory receiver_msg = abi.encodeWithSignature(
            "enfClaim(address)",
            msg.sender
        );
        _bridgeCall(receiver_msg);
    }

    function ramsClaim() external {
        bytes memory receiver_msg = abi.encodeWithSignature(
            "ramsClaim(address)",
            msg.sender
        );
        _bridgeCall(receiver_msg);
    }
}
