// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test_helpers.hpp>

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <kth/blockchain/script_filter_source.hpp>
#include <kth/database/block_undo.hpp>
#include <kth/database/databases/utxo_entry.hpp>
#include <kth/domain/chain/script_filter_builder.hpp>

using namespace kth;
namespace chain = kth::domain::chain;
using blockchain::output_of_stored_entry;
using blockchain::script_filter_cache;
using blockchain::script_filter_const_ptr;
using blockchain::spent_outputs;
using database::block_undo;
using database::result_code;
using database::spent_output;

namespace {

hash_digest leading_byte(uint8_t first) {
    hash_digest hash{};
    hash.front() = first;
    return hash;
}

// A pay-to-public-key-hash script whose hash is `fill` twenty times.
chain::script script_of(uint8_t fill) {
    data_chunk bytes{0x76, 0xa9, 0x14};
    bytes.insert(bytes.end(), 20, fill);
    bytes.push_back(0x88);
    bytes.push_back(0xac);
    return chain::script{std::move(bytes), false};
}

chain::input spend(hash_digest const& hash, uint32_t index) {
    return chain::input{chain::output_point{hash, index}, chain::script{}, max_input_sequence};
}

chain::output pay(uint64_t value, uint8_t fill, chain::token_data_opt token = {}) {
    return chain::output{value, script_of(fill), std::move(token)};
}

chain::transaction coinbase_tx() {
    data_chunk one{0x51};
    return chain::transaction{1, 0,
        chain::input::list{chain::input{chain::output_point{null_hash, chain::point::null_index},
            chain::script{std::move(one), false}, max_input_sequence}},
        chain::output::list{pay(5000, 0xc0)}};
}

chain::block block_of(chain::transaction::list txs) {
    chain::header const header{4, null_hash, null_hash, 1293623863, 0, 0};
    return chain::block{header, std::move(txs)};
}

utxoz::raw_outpoint key_of(hash_digest const& hash, uint32_t index) {
    utxoz::raw_outpoint key{};
    std::memcpy(key.data(), hash.data(), hash.size());
    key[32] = uint8_t(index & 0xff);
    key[33] = uint8_t((index >> 8) & 0xff);
    key[34] = uint8_t((index >> 16) & 0xff);
    key[35] = uint8_t((index >> 24) & 0xff);
    return key;
}

// The entry a full-mode undo record keeps for an output: the stored UTXO entry.
spent_output recorded(hash_digest const& hash, uint32_t index, chain::output const& output, uint32_t height) {
    auto const data = database::utxo_entry{output, height, 1293620000, false}.to_data();
    return spent_output{key_of(hash, index), std::vector<uint8_t>(data.begin(), data.end()), height};
}

std::expected<chain::output, result_code> read_full(spent_output const& entry) {
    return output_of_stored_entry(entry.value);
}

chain::token_data_opt fungible(uint64_t amount) {
    hash_digest category{};
    category.fill(0x22);
    return chain::token_data_opt{chain::make_fungible(category, amount)};
}

// A filter whose block pays to `count` scripts starting at `first`.
script_filter_const_ptr filter_of(uint8_t first, size_t count) {
    chain::output::list outputs;
    for (size_t i = 0; i < count; ++i) {
        outputs.push_back(pay(1000 + i, uint8_t(first + i)));
    }
    data_chunk one{0x51};
    chain::transaction const tx{1, 0,
        chain::input::list{chain::input{chain::output_point{null_hash, chain::point::null_index},
            chain::script{std::move(one), false}, max_input_sequence}},
        std::move(outputs)};
    auto filter = chain::build_script_filter(block_of({tx}), {});
    REQUIRE(filter);
    return std::make_shared<chain::script_filter const>(std::move(*filter));
}

} // namespace

// =============================================================================
// The outputs a block spends, from the block and its undo record
// =============================================================================

TEST_CASE("the output of a full-mode undo entry is read from the stored UTXO entry", "[script_filter_source]") {
    auto const plain = pay(1234, 0x11);
    auto const tokens = pay(1000, 0x12, fungible(40));

    auto const plain_entry = recorded(leading_byte(1), 0, plain, 700000);
    auto const token_entry = recorded(leading_byte(1), 1, tokens, 700001);

    auto const read_plain = output_of_stored_entry(plain_entry.value);
    REQUIRE(read_plain);
    CHECK(*read_plain == plain);

    auto const read_tokens = output_of_stored_entry(token_entry.value);
    REQUIRE(read_tokens);
    CHECK(*read_tokens == tokens);

    // Cut inside the script: not an output.
    auto const cut = std::span<uint8_t const>(plain_entry.value).first(12);
    auto const read_cut = output_of_stored_entry(cut);
    REQUIRE( ! read_cut);
    CHECK(read_cut.error() == result_code::other);
}

TEST_CASE("the spent outputs follow the inputs of the block, whatever the order of the record",
          "[script_filter_source]") {
    auto const funding = leading_byte(0x99);
    auto const first = pay(100, 0x11, fungible(100));
    auto const second = pay(200, 0x12);
    auto const third = pay(300, 0x13);
    auto const unrelated = pay(400, 0x14);

    chain::transaction const two_inputs{2, 0,
        chain::input::list{spend(funding, 0), spend(funding, 1)},
        chain::output::list{pay(250, 0x21)}};
    chain::transaction const one_input{2, 0,
        chain::input::list{spend(funding, 2)},
        chain::output::list{pay(290, 0x22)}};
    auto const block = block_of({coinbase_tx(), two_inputs, one_input});

    // Newest input first, and one entry no input names: what a block that
    // re-creates an earlier coinbase keeps for the entry it displaced.
    block_undo undo;
    undo.spent.push_back(recorded(funding, 2, third, 30));
    undo.spent.push_back(recorded(leading_byte(0x77), 0, unrelated, 5));
    undo.spent.push_back(recorded(funding, 1, second, 20));
    undo.spent.push_back(recorded(funding, 0, first, 10));

    auto const spent = spent_outputs(block, undo, read_full);
    REQUIRE(spent);
    REQUIRE(spent->size() == 3);
    CHECK((*spent)[0] == first);
    CHECK((*spent)[1] == second);
    CHECK((*spent)[2] == third);

    // And the filter built from them is the one built from the outputs in
    // input order.
    auto const from_record = chain::build_script_filter(block, *spent);
    auto const expected = chain::build_script_filter(block, chain::output::list{first, second, third});
    REQUIRE(from_record);
    REQUIRE(expected);
    CHECK(*from_record == *expected);

    // In another order the filter is the same set of keys under another root,
    // so the order is what the check above is about.
    auto const swapped = chain::build_script_filter(block, chain::output::list{second, first, third});
    REQUIRE(swapped);
    CHECK(swapped->root != expected->root);
}

TEST_CASE("an output created and spent in the same block is taken from the block", "[script_filter_source]") {
    auto const funding = leading_byte(0x99);
    auto const old = pay(900, 0x11);

    chain::transaction const creator{2, 0,
        chain::input::list{spend(funding, 0)},
        chain::output::list{pay(500, 0x31), pay(390, 0x32)}};
    chain::transaction const spender{2, 0,
        chain::input::list{spend(creator.hash(), 1)},
        chain::output::list{pay(380, 0x33)}};

    // Only the output that existed before the block is in its record.
    block_undo undo;
    undo.spent.push_back(recorded(funding, 0, old, 10));

    SECTION("the creating transaction stands first") {
        auto const block = block_of({coinbase_tx(), creator, spender});
        auto const spent = spent_outputs(block, undo, read_full);
        REQUIRE(spent);
        REQUIRE(spent->size() == 2);
        CHECK((*spent)[0] == old);
        CHECK((*spent)[1] == creator.outputs()[1]);
    }

    SECTION("the creating transaction stands after the one that spends its output") {
        auto const block = block_of({coinbase_tx(), spender, creator});
        auto const spent = spent_outputs(block, undo, read_full);
        REQUIRE(spent);
        REQUIRE(spent->size() == 2);
        CHECK((*spent)[0] == creator.outputs()[1]);
        CHECK((*spent)[1] == old);
    }
}

TEST_CASE("an input whose output is in neither the record nor the block is reported", "[script_filter_source]") {
    auto const funding = leading_byte(0x99);
    chain::transaction const creator{2, 0,
        chain::input::list{spend(funding, 0)},
        chain::output::list{pay(500, 0x31)}};

    block_undo undo;
    undo.spent.push_back(recorded(funding, 0, pay(900, 0x11), 10));

    SECTION("an output of no known transaction") {
        chain::transaction const stray{2, 0,
            chain::input::list{spend(leading_byte(0x55), 0)},
            chain::output::list{pay(1, 0x41)}};
        auto const spent = spent_outputs(block_of({coinbase_tx(), creator, stray}), undo, read_full);
        REQUIRE( ! spent);
        CHECK(spent.error() == result_code::key_not_found);
    }

    SECTION("an output index the transaction of the block does not have") {
        chain::transaction const past_the_end{2, 0,
            chain::input::list{spend(creator.hash(), 1)},
            chain::output::list{pay(1, 0x41)}};
        auto const spent = spent_outputs(block_of({coinbase_tx(), creator, past_the_end}), undo, read_full);
        REQUIRE( ! spent);
        CHECK(spent.error() == result_code::key_not_found);
    }

    SECTION("an empty record") {
        auto const spent = spent_outputs(block_of({coinbase_tx(), creator}), block_undo{}, read_full);
        REQUIRE( ! spent);
        CHECK(spent.error() == result_code::key_not_found);
    }
}

TEST_CASE("an entry that cannot be read fails the whole block", "[script_filter_source]") {
    auto const funding = leading_byte(0x99);
    chain::transaction const tx{2, 0,
        chain::input::list{spend(funding, 0), spend(funding, 1)},
        chain::output::list{pay(500, 0x31)}};

    block_undo undo;
    undo.spent.push_back(recorded(funding, 0, pay(900, 0x11), 10));
    undo.spent.push_back(recorded(funding, 1, pay(800, 0x12), 11));

    size_t reads = 0;
    auto const spent = spent_outputs(block_of({coinbase_tx(), tx}), undo,
        [&](spent_output const& entry) -> std::expected<chain::output, result_code> {
            ++reads;
            if (entry.height == 11) {
                return std::unexpected(result_code::db_corrupt);
            }
            return output_of_stored_entry(entry.value);
        });
    REQUIRE( ! spent);
    CHECK(spent.error() == result_code::db_corrupt);
    CHECK(reads == 2);
}

TEST_CASE("a block of one transaction spends nothing and reads no entry", "[script_filter_source]") {
    // The coinbase input names no output, and a record is not looked at.
    block_undo undo;
    undo.spent.push_back(recorded(null_hash, chain::point::null_index, pay(1, 0x11), 1));

    size_t reads = 0;
    auto const spent = spent_outputs(block_of({coinbase_tx()}), undo,
        [&](spent_output const&) -> std::expected<chain::output, result_code> {
            ++reads;
            return std::unexpected(result_code::other);
        });
    REQUIRE(spent);
    CHECK(spent->empty());
    CHECK(reads == 0);
}

// =============================================================================
// The filters built most recently
// =============================================================================

TEST_CASE("the filter cache gives back what it was given, by block hash", "[script_filter_source]") {
    auto const a = filter_of(0x10, 3);
    auto const b = filter_of(0x40, 5);
    script_filter_cache cache(script_filter_cache::cost(*a) + script_filter_cache::cost(*b));

    CHECK(cache.find(leading_byte(1)) == nullptr);
    CHECK(cache.size() == 0);
    CHECK(cache.bytes() == 0);

    cache.insert(leading_byte(1), a);
    cache.insert(leading_byte(2), b);
    CHECK(cache.find(leading_byte(1)) == a);
    CHECK(cache.find(leading_byte(2)) == b);
    CHECK(cache.find(leading_byte(3)) == nullptr);
    CHECK(cache.size() == 2);
    CHECK(cache.bytes() == script_filter_cache::cost(*a) + script_filter_cache::cost(*b));

    // The cost of an entry is the filter as it is served.
    CHECK(script_filter_cache::cost(*a) == chain::to_data(*a).size());
}

TEST_CASE("the filter cache drops the entry used longest ago when the budget is passed",
          "[script_filter_source]") {
    auto const a = filter_of(0x10, 4);
    auto const b = filter_of(0x40, 4);
    auto const c = filter_of(0x70, 4);
    auto const all = script_filter_cache::cost(*a) + script_filter_cache::cost(*b) + script_filter_cache::cost(*c);

    SECTION("in the order they were given") {
        script_filter_cache cache(all - 1);
        cache.insert(leading_byte(1), a);
        cache.insert(leading_byte(2), b);
        cache.insert(leading_byte(3), c);
        CHECK(cache.find(leading_byte(1)) == nullptr);
        CHECK(cache.find(leading_byte(2)) == b);
        CHECK(cache.find(leading_byte(3)) == c);
        CHECK(cache.size() == 2);
        CHECK(cache.bytes() == script_filter_cache::cost(*b) + script_filter_cache::cost(*c));
    }

    SECTION("a filter that was asked for again stays") {
        script_filter_cache cache(all - 1);
        cache.insert(leading_byte(1), a);
        cache.insert(leading_byte(2), b);
        CHECK(cache.find(leading_byte(1)) == a);
        cache.insert(leading_byte(3), c);
        CHECK(cache.find(leading_byte(1)) == a);
        CHECK(cache.find(leading_byte(2)) == nullptr);
        CHECK(cache.find(leading_byte(3)) == c);
    }

    SECTION("everything fits a budget of exactly their size") {
        script_filter_cache cache(all);
        cache.insert(leading_byte(1), a);
        cache.insert(leading_byte(2), b);
        cache.insert(leading_byte(3), c);
        CHECK(cache.size() == 3);
        CHECK(cache.bytes() == all);
    }
}

TEST_CASE("the filter cache keeps no filter larger than its budget, and one entry for a block",
          "[script_filter_source]") {
    auto const a = filter_of(0x10, 4);
    auto const again = filter_of(0x10, 4);

    script_filter_cache small(script_filter_cache::cost(*a) - 1);
    small.insert(leading_byte(1), a);
    CHECK(small.find(leading_byte(1)) == nullptr);
    CHECK(small.size() == 0);
    CHECK(small.bytes() == 0);

    // Two requests can build the filter of one block at the same time. The
    // first one given stays, and it is counted once.
    script_filter_cache cache(4 * script_filter_cache::cost(*a));
    cache.insert(leading_byte(1), a);
    cache.insert(leading_byte(1), again);
    CHECK(cache.find(leading_byte(1)) == a);
    CHECK(cache.size() == 1);
    CHECK(cache.bytes() == script_filter_cache::cost(*a));

    cache.insert(leading_byte(2), nullptr);
    CHECK(cache.size() == 1);
}
