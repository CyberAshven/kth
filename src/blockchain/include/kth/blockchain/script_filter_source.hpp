// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef KTH_BLOCKCHAIN_SCRIPT_FILTER_SOURCE_HPP
#define KTH_BLOCKCHAIN_SCRIPT_FILTER_SOURCE_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include <kth/blockchain/define.hpp>
#include <kth/database/block_undo.hpp>
#include <kth/database/databases/result_code.hpp>
#include <kth/domain.hpp>
#include <kth/domain/chain/script_filter_builder.hpp>

namespace kth::blockchain {

// The script filter of a block is built from what the node already keeps: the
// block in the block files, and the undo record written when the block was
// connected, which holds every output the block spent that existed before it.
// Nothing is stored for the filter itself.

using script_filter_const_ptr = std::shared_ptr<domain::chain::script_filter const>;

// The memory the chain gives the filters built most recently. The filter of a
// 32 MB block is about 0.4 MB.
constexpr size_t script_filter_cache_bytes = 32 * 1024 * 1024;

// Reads the output an undo entry stands for. In full mode the entry holds the
// output; in reference mode it points at the transaction in the block files.
using undo_output_reader = std::function<
    std::expected<domain::chain::output, database::result_code>(database::spent_output const&)>;

// The output held by a full-mode undo entry: the stored UTXO entry opens with
// the output in wire form.
[[nodiscard]]
KB_API
std::expected<domain::chain::output, database::result_code>
output_of_stored_entry(std::span<uint8_t const> value);

// The outputs a block spends, one for each input of each transaction after the
// coinbase, in the order of the inputs. That is the order the filter builder
// takes them in.
//
// An undo record is not in that order and does not cover every input: its
// entries are found by outpoint, and an output created and spent in the same
// block has no entry, so it is taken from the block. The transaction that
// creates it can stand after the one that spends it, as the transactions of a
// block are ordered by their hashes.
//
// key_not_found when an input spends an output that is in neither. An entry no
// input names is left alone: a block that re-creates an earlier coinbase keeps
// the entry it displaced in its record.
[[nodiscard]]
KB_API
std::expected<std::vector<domain::chain::output>, database::result_code>
spent_outputs(domain::chain::block const& blk, database::block_undo const& undo,
              undo_output_reader const& read);

// The script filters built most recently, by block hash, within a budget of
// bytes. A wallet that follows the tip asks for the filters of the last blocks,
// and so does every other wallet, so these are built once.
//
// The filter of a block depends on the block and the outputs it spends, and
// both are fixed by the block hash. So an entry never goes stale, and a
// reorganization needs no invalidation.
class KB_API script_filter_cache {
public:
    explicit script_filter_cache(size_t max_bytes);

    // The filter of the block, or null. A hit makes the entry the newest.
    [[nodiscard]]
    script_filter_const_ptr find(hash_digest const& hash);

    // Keeps the filter as the newest entry and drops the oldest ones until the
    // budget holds. A filter larger than the whole budget is not kept.
    void insert(hash_digest const& hash, script_filter_const_ptr filter);

    [[nodiscard]]
    size_t bytes() const;

    [[nodiscard]]
    size_t size() const;

    // The bytes an entry counts for: the serialized filter.
    [[nodiscard]]
    static size_t cost(domain::chain::script_filter const& filter);

private:
    struct entry {
        hash_digest hash;
        script_filter_const_ptr filter;
        size_t bytes;
    };

    size_t const max_bytes_;
    mutable std::mutex mutex_;
    std::list<entry> entries_;   // newest first
    std::map<hash_digest, std::list<entry>::iterator> by_hash_;
    size_t bytes_ = 0;
};

} // namespace kth::blockchain

#endif // KTH_BLOCKCHAIN_SCRIPT_FILTER_SOURCE_HPP
