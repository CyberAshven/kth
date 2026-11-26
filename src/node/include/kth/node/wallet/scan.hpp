// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef KTH_NODE_WALLET_SCAN_HPP
#define KTH_NODE_WALLET_SCAN_HPP

#if ! defined(KTH_WITH_WALLET)
#error "Wallet support is an optional feature: build with the Conan options neutrino=True and wallet=True."
#endif

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include <kth/domain/chain/point.hpp>
#include <kth/infrastructure/math/hash.hpp>
#include <kth/infrastructure/utility/data.hpp>
#include <kth/node/define.hpp>
#include <kth/node/wallet/filter_files.hpp>

namespace kth::node::wallet {

// Matching what a wallet watches against the stored filters, all of it in one
// pass over each filter, the heights split over threads. Nothing leaves the
// process: the matches only say which whole blocks to download.

/// An item of the regular filter (filter type 0) watched from a height on:
/// an output script as the output holds it, or an outpoint as the input that
/// spends it holds it (outpoint_item).
struct scan_item {
    data_chunk item;
    uint32_t from;
};

/// The first `bits` bits (0 to 16) of an input prefix, right-aligned in
/// `prefix`, watched from a height on.
struct scan_prefix {
    uint16_t prefix;
    uint8_t bits;
    uint32_t from;
};

/// The heights that match, in increasing order, each with the positions of
/// the items or prefixes it matches, in increasing order.
using scan_hits = std::vector<std::pair<uint32_t, std::vector<size_t>>>;

/// The item of an outpoint in the regular filter: the transaction hash, then
/// the index in little-endian.
[[nodiscard]]
KND_API
data_chunk outpoint_item(domain::chain::point const& outpoint);

/// Tests `items` against the regular filters (filter type 0) at `locations`
/// of `data`, each with the key of its block. A filter that does not parse
/// matches every item, so it can never hide a payment or a spend.
/// file_system when the file cannot be read.
[[nodiscard]]
KND_API
std::expected<scan_hits, std::error_code> scan_regular_filters(std::filesystem::path const& data,
    std::span<filter_location const> locations, std::span<scan_item const> items, size_t threads);

/// Tests `prefixes` against the input prefix filters (filter type 2) at
/// `locations` of `data`. A filter that does not parse matches every prefix.
[[nodiscard]]
KND_API
std::expected<scan_hits, std::error_code> scan_input_prefix_filters(std::filesystem::path const& data,
    std::span<filter_location const> locations, std::span<scan_prefix const> prefixes, size_t threads);

/// The items of `items` the regular filter of `block` holds, by position;
/// all of them when the filter does not parse.
[[nodiscard]]
KND_API
std::vector<size_t> match_regular_filter(hash_digest const& block, byte_span filter,
    std::span<byte_span const> items);

} // namespace kth::node::wallet

#endif // KTH_NODE_WALLET_SCAN_HPP
