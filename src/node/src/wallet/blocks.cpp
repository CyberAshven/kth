// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kth/node/wallet/blocks.hpp>

#include <algorithm>

#include <kth/domain/chain/compact_filter.hpp>
#include <kth/domain/chain/input_prefix_filter.hpp>
#include <kth/infrastructure/error.hpp>

namespace kth::node::wallet {

std::error_code check_block(domain::chain::block const& blk, hash_digest const& expected) {
    if (blk.transactions().empty()) return error::empty_block;
    if (blk.hash() != expected) return error::bad_stream;
    if (blk.generate_merkle_root() != blk.header().merkle()) return error::merkle_mismatch;
    if ( ! blk.is_distinct_transaction_set()) return error::bad_stream;
    return {};
}

std::error_code check_regular_filter(domain::chain::block const& blk, byte_span filter) {
    auto const rebuilt = domain::chain::build_regular_filter(blk);
    if ( ! rebuilt || ! std::ranges::equal(rebuilt->to_data(), filter)) return error::bad_stream;
    return {};
}

std::error_code check_input_prefix_filter(domain::chain::block const& blk, byte_span filter) {
    auto const rebuilt = domain::chain::to_data(domain::chain::build_input_prefix_filter(blk));
    if ( ! std::ranges::equal(rebuilt, filter)) return error::bad_stream;
    return {};
}

std::vector<uint32_t> prefix_inputs(domain::chain::transaction const& tx, uint16_t prefix, uint8_t bits) {
    std::vector<uint32_t> out;
    if (tx.is_coinbase() || bits > 16) return out;
    auto const shift = 16u - bits;
    auto const low = uint32_t(prefix) << shift;
    auto const high = (uint32_t(prefix) + 1) << shift;
    auto const& inputs = tx.inputs();
    auto const counted = std::min(inputs.size(), domain::chain::input_prefix_inputs);
    for (size_t at = 0; at < counted; ++at) {
        auto const value = uint32_t(domain::chain::input_prefix(inputs[at]));
        if (value >= low && value < high) out.push_back(uint32_t(at));
    }
    return out;
}

} // namespace kth::node::wallet
