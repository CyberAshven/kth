// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef KTH_NODE_WALLET_BLOCKS_HPP
#define KTH_NODE_WALLET_BLOCKS_HPP

#if ! defined(KTH_WITH_WALLET)
#error "Wallet support is an optional feature: build with the Conan options neutrino=True and wallet=True."
#endif

#include <cstdint>
#include <expected>
#include <system_error>
#include <vector>

#include <kth/domain/chain/block.hpp>
#include <kth/domain/chain/transaction.hpp>
#include <kth/infrastructure/math/hash.hpp>
#include <kth/infrastructure/utility/data.hpp>
#include <kth/node/define.hpp>

namespace kth::node::wallet {

// What a whole block downloaded from a peer is checked against before a
// wallet's history reads it, and what a wallet looks for in it.

/// The block hashes to `expected` and its transactions to the Merkle root of
/// its header, with no transaction repeated (the duplicate-transaction
/// mutation). bad_stream otherwise; empty_block when it has no transaction.
[[nodiscard]]
KND_API
std::error_code check_block(domain::chain::block const& blk, hash_digest const& expected);

/// The regular filter (filter type 0) rebuilt from the block must equal the
/// one served: it holds the output scripts and the outpoints the block spends,
/// all read from the block alone, so any difference proves the serving peer
/// sent a filter that was not built from this block. bad_stream otherwise.
[[nodiscard]]
KND_API
std::error_code check_regular_filter(domain::chain::block const& blk, byte_span filter);

/// The input prefix filter rebuilt from the block must equal the one served:
/// it is built from the block alone, so any difference proves the serving
/// peer lied. bad_stream otherwise.
[[nodiscard]]
KND_API
std::error_code check_input_prefix_filter(domain::chain::block const& blk, byte_span filter);

/// The positions of the inputs of `tx` a sender may grind for a reusable
/// payment address (the first 30, none of a coinbase) whose prefix starts
/// with the `bits` bits of `prefix`.
[[nodiscard]]
KND_API
std::vector<uint32_t> prefix_inputs(domain::chain::transaction const& tx, uint16_t prefix, uint8_t bits);

} // namespace kth::node::wallet

#endif // KTH_NODE_WALLET_BLOCKS_HPP
