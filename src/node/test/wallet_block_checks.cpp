// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// What a whole block downloaded from a peer is checked against before a
// wallet's history reads it.

#include <test_helpers.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include <kth/domain/chain/compact_filter.hpp>
#include <kth/domain/chain/golomb_set.hpp>
#include <kth/domain/chain/input_prefix_filter.hpp>
#include <kth/infrastructure/utility/byte_writer.hpp>
#include <kth/node/wallet/blocks.hpp>

using namespace kth;
namespace chain = kth::domain::chain;
namespace wallet = kth::node::wallet;

namespace {

data_chunk chunk_of(std::string_view hex) {
    auto decoded = decode_base16(hex);
    REQUIRE(decoded);
    return std::move(*decoded);
}

hash_digest digest_of(uint32_t seed) {
    uint8_t const bytes[] = {uint8_t(seed), uint8_t(seed >> 8), uint8_t(seed >> 16), uint8_t(seed >> 24), 0x5a};
    return sha256_hash(bytes);
}

chain::block block_of_hex(std::string_view hex) {
    auto const raw = chunk_of(hex);
    byte_reader reader(raw);
    auto block = chain::block::from_data(reader);
    REQUIRE(block);
    return std::move(*block);
}

chain::script script_of(data_chunk bytes) {
    return chain::script(bytes, false);
}

chain::transaction coinbase_paying(data_chunk const& script, uint64_t value, uint32_t salt) {
    chain::input::list inputs;
    inputs.emplace_back(chain::output_point{null_hash, chain::point::null_index},
        script_of({0x01, uint8_t(salt), uint8_t(salt >> 8)}), 0xffffffff);
    chain::output::list outputs;
    outputs.emplace_back(value, script_of(script), chain::token_data_opt{});
    return chain::transaction(1, 0, std::move(inputs), std::move(outputs));
}

struct paid {
    uint64_t value;
    data_chunk script;
    chain::token_data_opt token{};
};

chain::transaction spending(std::vector<chain::point> const& previous, std::vector<paid> const& outputs, uint32_t salt) {
    chain::input::list inputs;
    for (auto const& point : previous) {
        inputs.emplace_back(chain::output_point{point.hash(), point.index()}, script_of({0x01, uint8_t(salt)}), 0xffffffff);
    }
    chain::output::list made;
    for (auto const& out : outputs) {
        made.emplace_back(out.value, script_of(out.script), out.token);
    }
    return chain::transaction(2, 0, std::move(inputs), std::move(made));
}

chain::block block_with(hash_digest const& previous, chain::transaction::list txs) {
    chain::block candidate(chain::header{0x20000000, previous, null_hash, 1700000000, 0x207fffff, 0}, txs);
    auto const root = candidate.generate_merkle_root();
    return chain::block(chain::header{0x20000000, previous, root, 1700000000, 0x207fffff, 0}, std::move(txs));
}

// Static data: decoded without assertions, which belong to test cases.
data_chunk decoded(std::string_view hex) {
    auto bytes = decode_base16(hex);
    return bytes ? std::move(*bytes) : data_chunk{};
}

data_chunk const p2pkh_a = decoded("76a914000102030405060708090a0b0c0d0e0f1011121388ac");
data_chunk const p2pkh_b = decoded("76a914aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa88ac");

} // namespace

// Block checks
// ----------------------------------------------------------------------------

TEST_CASE("wallet block check needs the requested hash, the Merkle root and distinct transactions", "[wallet]") {
    auto const coinbase = coinbase_paying(p2pkh_a, 50, 1);
    auto const spend = spending({chain::point(digest_of(1), 0)}, {{10, p2pkh_b}}, 2);
    auto const good = block_with(digest_of(0), {coinbase, spend});
    REQUIRE( ! wallet::check_block(good, good.hash()));
    REQUIRE(wallet::check_block(good, digest_of(5)) == error::bad_stream);

    // Transactions that are not the ones the header commits to.
    chain::block swapped(good.header(), chain::transaction::list{coinbase, spending({chain::point(digest_of(1), 0)}, {{11, p2pkh_b}}, 2)});
    REQUIRE(wallet::check_block(swapped, swapped.hash()) == error::merkle_mismatch);

    // The duplicate-transaction mutation: the same root with the last one repeated.
    auto const third = spending({chain::point(digest_of(7), 0)}, {{12, p2pkh_b}}, 3);
    auto const odd = block_with(digest_of(0), {coinbase, spend, third});
    chain::block mutated(odd.header(), chain::transaction::list{coinbase, spend, third, third});
    REQUIRE(mutated.generate_merkle_root() == odd.header().merkle());
    REQUIRE(wallet::check_block(mutated, mutated.hash()) == error::bad_stream);

    chain::block empty(good.header(), chain::transaction::list{});
    REQUIRE(wallet::check_block(empty, empty.hash()) == error::empty_block);
}

TEST_CASE("wallet block check against its regular filter, rebuilt from the block", "[wallet]") {
    // bchd simnet block 127 and the regular filter bchd serves for it (the
    // vectors of test/chain/compact_filter.cpp of the domain).
    auto const block = block_of_hex("01000030faec54d16b2a5bdbc3371f5e1593a8166e95a55d0df4f8ee33b6d8281bee6d79816c95ada87beb3bac7be56331697aa815d04915e94c1d6570812f083e9669f791b0f66affff7f20000000000301000000010000000000000000000000000000000000000000000000000000000000000000ffffffff12017f086f88e3cf77f9ac8d062f626368642fffffffff01c621062a010000001976a914b2275cc60091cdde77bddb8c132f60494cb2ea6b88ac00000000010000000178618fd7cc15e6088f2b77cc5ac25dd92ad86619bbe91d693427a9cd965e8708000000006441dfea2c0f1c08e147bdf45182ca31e99745704f5caa7f348d3b9e55ff01a594141bf235e38ad141d4389033312c576917f0652c754bb452127a069a3dfab511d9412102be31cab636041636a1846f4358a241f608ca9d9f29177585280590f9d97e033fffffffff0300a3e1110000000017a91469d7ef8f42a25e8791bb37d5fb48456f102a3cb9870000000000000000176a157363726970742066696c74657220766563746f72734a462418010000001976a91438b726cdbeb1d7f311544519c3412a4ad67c26fb88ac0000000002000000013a90cdbcad008793ceca6cc7d7dd73c22e1d85dd0858760234cae2c1c79fdf2100000000020152ffffffff02f07be111000000001976a914040404040404040404040404040404040404040488ac0000000000000000176a157363726970742066696c74657220766563746f727300000000");
    REQUIRE(encode_hash(block.hash()) == "0f5f466912edf56885b527866f8a3f12d8d620edef5ff3ebdf43ec00ab556868");
    REQUIRE( ! wallet::check_block(block, block.hash()));
    auto const served = chunk_of("07415a006e6bee1f3520c934743abce01b7f8480");
    REQUIRE( ! wallet::check_regular_filter(block, served));

    // A filter that leaves out one item of the block, keeps the same count
    // with other bytes, holds one item more, or is another block's: each is
    // refused.
    std::vector<data_chunk> items;
    auto const& txs = block.transactions();
    auto const add_scripts = [&items](chain::transaction const& tx) {
        for (auto const& out : tx.outputs()) {
            if ( ! out.script().bytes().empty()) items.push_back(out.script().bytes());
        }
    };
    add_scripts(txs[0]);
    for (size_t at = 1; at < txs.size(); ++at) {
        for (auto const& in : txs[at].inputs()) {
            data_chunk item(in.previous_output().serialized_size(true));
            byte_writer writer(item);
            REQUIRE(in.previous_output().to_data(writer, true));
            items.push_back(std::move(item));
        }
        add_scripts(txs[at]);
    }
    auto const key = chain::compact_filter_key(block.hash());
    auto const rebuilt = chain::golomb_set::build(key, items);
    REQUIRE(rebuilt);
    REQUIRE(rebuilt->to_data() == served);
    auto missing = items;
    missing.erase(missing.begin() + 1);
    auto const without = chain::golomb_set::build(key, missing);
    REQUIRE(without);
    REQUIRE(wallet::check_regular_filter(block, without->to_data()) == error::bad_stream);
    auto extra = items;
    extra.push_back(to_chunk(digest_of(1)));
    auto const with = chain::golomb_set::build(key, extra);
    REQUIRE(with);
    REQUIRE(wallet::check_regular_filter(block, with->to_data()) == error::bad_stream);
    auto flipped = served;
    flipped.back() ^= 0x01;
    REQUIRE(wallet::check_regular_filter(block, flipped) == error::bad_stream);
    auto const foreign = chain::golomb_set::build(chain::compact_filter_key(digest_of(2)), items);
    REQUIRE(foreign);
    REQUIRE(wallet::check_regular_filter(block, foreign->to_data()) == error::bad_stream);
    REQUIRE(wallet::check_regular_filter(block, data_chunk{}) == error::bad_stream);
    auto longer = served;
    longer.push_back(0x00);
    REQUIRE(wallet::check_regular_filter(block, longer) == error::bad_stream);
}

TEST_CASE("wallet block check against its input prefix filter, and the inputs of a prefix", "[wallet]") {
    // Block 2 of rpa-type2-vectors.md: one transaction after the coinbase,
    // one input, value 9408.
    auto const block = block_of_hex("00000020679d3aae7bad61757b5e1e3e225764d3a90a72d6f383e104953f82f36c7d5235382edb97970c0e4c882a3736d227780325ae84c0fdf065b1402c61a949e6ced658f35365ffff7f20000000000201000000010000000000000000000000000000000000000000000000000000000000000000ffffffff06010103727061ffffffff0100f2052a0100000001510000000002000000011111111111111111111111111111111111111111111111111111111111111111000000000151ffffffff01e803000000000000015100000000");
    REQUIRE( ! wallet::check_input_prefix_filter(block, chunk_of("010f8a0400")));
    REQUIRE(wallet::check_input_prefix_filter(block, chunk_of("00")) == error::bad_stream);
    REQUIRE(wallet::check_input_prefix_filter(block, chunk_of("010f8a0401")) == error::bad_stream);

    auto const& tx = block.transactions()[1];
    REQUIRE(wallet::prefix_inputs(tx, 0x94, 8) == std::vector<uint32_t>{0});
    REQUIRE(wallet::prefix_inputs(tx, 0x9408, 16) == std::vector<uint32_t>{0});
    REQUIRE(wallet::prefix_inputs(tx, 0x9, 4) == std::vector<uint32_t>{0});
    REQUIRE(wallet::prefix_inputs(tx, 0x0, 0) == std::vector<uint32_t>{0});
    REQUIRE(wallet::prefix_inputs(tx, 0x95, 8).empty());
    REQUIRE(wallet::prefix_inputs(tx, 0x9409, 16).empty());
    REQUIRE(wallet::prefix_inputs(tx, 0x94, 17).empty());
    REQUIRE(wallet::prefix_inputs(block.transactions()[0], 0x0, 0).empty());

    // Only the first 30 inputs count.
    std::vector<chain::point> previous;
    for (uint32_t i = 0; i < 31; ++i) previous.emplace_back(digest_of(400 + i), i);
    auto const wide = spending(previous, {{1, p2pkh_a}}, 7);
    auto const last = chain::input_prefix(wide.inputs()[30]);
    auto const matched = wallet::prefix_inputs(wide, last, 16);
    REQUIRE(std::ranges::find(matched, 30u) == matched.end());
    REQUIRE(wallet::prefix_inputs(wide, 0, 0).size() == 30u);
}
