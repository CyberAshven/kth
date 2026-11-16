// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kth/blockchain/spent_outputs.hpp>

#include <cstring>
#include <map>
#include <optional>
#include <utility>

#include <boost/unordered/unordered_flat_map.hpp>

#include <kth/blockchain/utxo_builder.hpp>

namespace kth::blockchain {

namespace {

utxoz::raw_outpoint key_of(domain::chain::output_point const& point) {
    utxoz::raw_outpoint key{};
    std::memcpy(key.data(), point.hash().data(), hash_size);
    auto const index = point.index();
    key[32] = uint8_t(index & 0xff);
    key[33] = uint8_t((index >> 8) & 0xff);
    key[34] = uint8_t((index >> 16) & 0xff);
    key[35] = uint8_t((index >> 24) & 0xff);
    return key;
}

} // namespace

std::expected<undo_output, database::result_code>
output_of_stored_entry(std::span<uint8_t const> value) {
    // utxo_entry's layout: the output, its height and median time past (4
    // bytes each, not needed here), then the coinbase flag. Read in place, so
    // the output is not copied out of a whole entry.
    byte_reader reader(value);
    auto output = domain::chain::output::from_data(reader, true);
    if ( ! output || ! reader.skip(2 * sizeof(uint32_t))) {
        return std::unexpected(database::result_code::other);
    }
    auto const coinbase = reader.read_byte();
    if ( ! coinbase) {
        return std::unexpected(database::result_code::other);
    }
    return undo_output{std::move(*output), *coinbase != 0};
}

std::expected<spent_output_list, database::result_code>
spent_outputs_with_heights(domain::chain::block const& blk, uint32_t height,
                           database::block_undo const& undo, undo_output_reader const& read) {
    auto const& txs = blk.transactions();
    size_t inputs = 0;
    for (size_t i = 1; i < txs.size(); ++i) {
        inputs += txs[i].inputs().size();
    }

    spent_output_list spent;
    spent.outputs.reserve(inputs);
    spent.heights.reserve(inputs);
    spent.coinbase.reserve(inputs);
    if (inputs == 0) {
        return spent;
    }

    boost::unordered_flat_map<utxoz::raw_outpoint, database::spent_output const*, outpoint_fast_hasher> recorded;
    recorded.reserve(undo.spent.size());
    for (auto const& entry : undo.spent) {
        recorded.emplace(entry.key, &entry);
    }

    // Filled only when an input names an output the record does not hold: most
    // blocks spend little of what they create, and this hashes every
    // transaction of the block.
    std::optional<std::map<hash_digest, size_t>> created;

    for (size_t i = 1; i < txs.size(); ++i) {
        for (auto const& input : txs[i].inputs()) {
            auto const& point = input.previous_output();
            if (auto const it = recorded.find(key_of(point)); it != recorded.end()) {
                auto stored = read(*it->second);
                if ( ! stored) {
                    return std::unexpected(stored.error());
                }
                spent.outputs.push_back(std::move(stored->output));
                spent.heights.push_back(it->second->height);
                spent.coinbase.push_back(stored->coinbase);
                continue;
            }
            if ( ! created) {
                created.emplace();
                for (size_t t = 0; t < txs.size(); ++t) {
                    created->emplace(txs[t].hash(), t);
                }
            }
            auto const it = created->find(point.hash());
            if (it == created->end() || point.index() >= txs[it->second].outputs().size()) {
                return std::unexpected(database::result_code::key_not_found);
            }
            spent.outputs.push_back(txs[it->second].outputs()[point.index()]);
            spent.heights.push_back(height);
            spent.coinbase.push_back(txs[it->second].is_coinbase());
        }
    }
    return spent;
}

} // namespace kth::blockchain
