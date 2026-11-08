// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef KTH_NETWORK_TX_INVENTORY_HPP
#define KTH_NETWORK_TX_INVENTORY_HPP

#include <chrono>
#include <cstdint>
#include <vector>

#include <kth/infrastructure/math/hash.hpp>
#include <kth/network/define.hpp>

namespace kth::network {

/// BCHN CRollingBloomFilter: a filter of roughly the last `elements` hashes
/// inserted, with false positives at `fp_rate`. It keeps three generations of
/// elements / 2 entries and forgets the oldest as a new one starts. Keyed by a
/// random SipHash key, drawn again on reset(). Allocated on the first insert.
class KN_API rolling_bloom_filter {
public:
    rolling_bloom_filter(uint32_t elements, double fp_rate);

    void insert(hash_digest const& hash);

    [[nodiscard]]
    bool contains(hash_digest const& hash) const;

    void reset();

private:
    [[nodiscard]]
    uint64_t hash(uint32_t n, hash_digest const& value) const;

    uint32_t hash_functions_;
    uint32_t entries_per_generation_;
    uint32_t entries_this_generation_{0};
    uint32_t generation_{1};
    size_t words_;
    uint64_t k0_{0};
    uint64_t k1_{0};
    std::vector<uint64_t> data_;
};

/// What a peer told the node about the transactions it wants announced: the
/// ones it knows already (BCHN filterInventoryKnown) and its fee filter
/// (BIP133, BCHN minFeeFilter).
///
/// Owned by the peer's session and touched only on the session's strand: the
/// read loop records what the peer announces, sends and asks to filter, and
/// the peer's service records what it announces and reads all of it. Both run
/// on that strand (p2p_node::run_peer_protocols); nothing else touches it.
class KN_API tx_inventory {
public:
    /// BCHN sizes filterInventoryKnown for 50,000 entries at one in a million.
    static constexpr uint32_t known_capacity = 50'000;

    tx_inventory();

    void add_known(hash_digest const& txid) { known_.insert(txid); }

    [[nodiscard]]
    bool known(hash_digest const& txid) const { return known_.contains(txid); }

    /// The peer's minimum fee per 1000 bytes; zero filters nothing.
    [[nodiscard]]
    uint64_t fee_filter() const { return fee_filter_; }

    void set_fee_filter(uint64_t fee_per_kb) { fee_filter_ = fee_per_kb; }

private:
    rolling_bloom_filter known_;
    uint64_t fee_filter_{0};
};

/// What a peer's service hears when a transaction is admitted: the last
/// admission's id, and when the node's inbound peers are next due an
/// announcement (BCHN gives them one schedule, so they all hear of a
/// transaction at once).
struct tx_admissions {
    uint64_t last{0};
    std::chrono::steady_clock::time_point inbound_due{};
};

} // namespace kth::network

#endif // KTH_NETWORK_TX_INVENTORY_HPP
