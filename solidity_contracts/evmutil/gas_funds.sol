// SPDX-License-Identifier: MIT

pragma solidity ^0.8.18;

// Gas Funds
contract GasFunds  {

    string  public linkedEOSAccountName;
    address public linkedEOSAddress;
    address public evmAddress;

    constructor() {
        linkedEOSAccountName = "evmutil.xsat";
        linkedEOSAddress = 0xbbBbbbBbbBBbBBbBBBbbbBbB5530eA015740a800;
        evmAddress = 0xBBbBbbbbbBbbbbBBBBbbbBbb56e40ee0D9000000;
    }

    function _isReservedAddress(address addr) internal pure returns (bool) {
        return ((uint160(addr) & uint160(0xFffFfFffffFfFFffffFFFffF0000000000000000)) == uint160(0xBBbbBbBbbBbbBbbbBbbbBBbb0000000000000000));
    }

    function claim(address _target,uint8 _receiver_type) external {
        // The action is aynchronously viewed from EVM and looks UNSAFE.
        // BUT in fact the call will be executed as inline action.
        // If the cross chain call fail, the whole tx including the EVM action will be rejected.
        bytes memory receiver_msg = abi.encodeWithSignature("claim(address,address,uint8)", _target, msg.sender, _receiver_type);
        (bool success, ) = evmAddress.call(abi.encodeWithSignature("bridgeMsgV0(string,bool,bytes)", linkedEOSAccountName, true, receiver_msg ));
        if(!success) { revert(); }
    }

    function enfClaim() external {
        // The action is aynchronously viewed from EVM and looks UNSAFE.
        // BUT in fact the call will be executed as inline action.
        // If the cross chain call fail, the whole tx including the EVM action will be rejected.
        bytes memory receiver_msg = abi.encodeWithSignature("enfClaim(address)", msg.sender);
        (bool success, ) = evmAddress.call(abi.encodeWithSignature("bridgeMsgV0(string,bool,bytes)", linkedEOSAccountName, true, receiver_msg ));
        if(!success) { revert(); }
    }

    function ramsClaim() external {
        // The action is aynchronously viewed from EVM and looks UNSAFE.
        // BUT in fact the call will be executed as inline action.
        // If the cross chain call fail, the whole tx including the EVM action will be rejected.
        bytes memory receiver_msg = abi.encodeWithSignature("ramsClaim(address)", msg.sender);
        (bool success, ) = evmAddress.call(abi.encodeWithSignature("bridgeMsgV0(string,bool,bytes)", linkedEOSAccountName, true, receiver_msg ));
        if(!success) { revert(); }
    }
}
