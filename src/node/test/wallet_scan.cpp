// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test_helpers.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include <kth/domain/chain/compact_filter.hpp>
#include <kth/domain/chain/golomb_set.hpp>
#include <kth/domain/chain/input_prefix_filter.hpp>
#include <kth/node/wallet/filter_files.hpp>
#include <kth/node/wallet/scan.hpp>

using namespace kth;
namespace chain = kth::domain::chain;
namespace wallet = kth::node::wallet;

namespace {

int process_id() {
#if defined(_WIN32)
    return ::_getpid();
#else
    return ::getpid();
#endif
}

// A folder of its own for each test and process.
struct temporary_directory {
    std::filesystem::path path;
    explicit temporary_directory(std::string_view tag)
        : path(std::filesystem::temp_directory_path()
            / (std::string("kth_wallet_scan_") + std::string(tag) + "_" + std::to_string(process_id()))) {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
        std::filesystem::create_directories(path);
    }
    ~temporary_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

hash_digest digest_of(uint32_t seed) {
    uint8_t const bytes[] = {uint8_t(seed), uint8_t(seed >> 8), uint8_t(seed >> 16), uint8_t(seed >> 24), 0x5a};
    return sha256_hash(bytes);
}

// A served regular filter holding `items`, for `block`.
data_chunk regular_filter_of(hash_digest const& block, std::vector<data_chunk> const& items) {
    auto const set = chain::golomb_set::build(chain::compact_filter_key(block), items);
    REQUIRE(set);
    return set->to_data();
}

// A pay-to-public-key-hash script with a hash made from the seed.
data_chunk script_item(uint32_t seed) {
    auto const digest = digest_of(seed);
    data_chunk script{0x76, 0xa9, 0x14};
    script.insert(script.end(), digest.begin(), digest.begin() + 20);
    script.push_back(0x88);
    script.push_back(0xac);
    return script;
}

chain::block block_of_hex(std::string_view hex) {
    auto const raw = decode_base16(hex);
    REQUIRE(raw);
    byte_reader reader(*raw);
    auto block = chain::block::from_data(reader);
    REQUIRE(block);
    return std::move(*block);
}

std::vector<wallet::filter_entry> chain_of_filters(std::vector<hash_digest> const& blocks,
        std::vector<data_chunk> const& filters, hash_digest previous = null_hash) {
    std::vector<wallet::filter_entry> entries;
    for (size_t i = 0; i < blocks.size(); ++i) {
        previous = wallet::next_filter_header(filters[i], previous);
        entries.push_back(wallet::filter_entry{blocks[i], previous, filters[i]});
    }
    return entries;
}

} // namespace

TEST_CASE("wallet scan finds each item in its blocks from its own height, on any number of threads", "[wallet]") {
    temporary_directory dir("scan_items");
    // Two scripts and two outpoints.
    std::vector<data_chunk> watched{script_item(90000), script_item(90001),
        wallet::outpoint_item(chain::point(digest_of(90002), 0)), wallet::outpoint_item(chain::point(digest_of(90002), 1))};
    std::vector<hash_digest> blocks;
    std::vector<data_chunk> filters;
    for (uint32_t height = 0; height < 200; ++height) {
        auto const block = digest_of(height * 7 + 3);
        std::vector<data_chunk> items;
        for (uint32_t j = 0; j < 25; ++j) {
            items.push_back(script_item(height * 1000 + j + 100000));
            items.push_back(wallet::outpoint_item(chain::point(digest_of(height * 1000 + j + 500000), j)));
        }
        // Item i is in the blocks whose height is a multiple of i + 2.
        for (uint32_t i = 0; i < watched.size(); ++i) {
            if (height % (i + 2) == 0) items.push_back(watched[i]);
        }
        blocks.push_back(block);
        filters.push_back(regular_filter_of(block, items));
    }
    // A filter whose count promises values it does not hold matches every item.
    filters[150] = data_chunk{0x05, 0x00};
    auto const lookup = [&](uint32_t height) -> std::optional<hash_digest> {
        if (height < blocks.size()) return blocks[height];
        return std::nullopt;
    };
    auto store = wallet::filter_files::open(dir.path, chain::regular_filter_type, lookup);
    REQUIRE(store);
    REQUIRE( ! store->append(chain_of_filters(blocks, filters)));
    std::vector<wallet::scan_item> items;
    for (uint32_t i = 0; i < watched.size(); ++i) items.push_back({watched[i], i * 10});
    auto const where = store->locations(0, 200);
    for (size_t threads : {1, 3, 8}) {
        auto const hits = wallet::scan_regular_filters(store->data_path(), where, items, threads);
        REQUIRE(hits);
        REQUIRE(std::ranges::is_sorted(*hits, {}, [](auto const& hit) { return hit.first; }));
        for (uint32_t i = 0; i < watched.size(); ++i) {
            std::vector<uint32_t> found;
            for (auto const& [height, matched] : *hits) {
                if (std::ranges::find(matched, size_t(i)) != matched.end()) found.push_back(height);
            }
            std::vector<uint32_t> expected;
            for (uint32_t height = i * 10; height < 200; ++height) {
                if (height % (i + 2) == 0 || height == 150) expected.push_back(height);
            }
            // A false positive is possible, at 1 in 784931 per block and item.
            for (auto const height : expected) {
                REQUIRE(std::ranges::find(found, height) != found.end());
            }
            REQUIRE(found.size() <= expected.size() + 1);
        }
    }
    // Nothing to scan.
    REQUIRE(wallet::scan_regular_filters(store->data_path(), {}, items, 2)->empty());
    REQUIRE(wallet::scan_regular_filters(store->data_path(), where, {}, 2)->empty());
    // A file that is gone.
    auto const missing = wallet::scan_regular_filters(dir.path / "missing.dat", where, items, 1);
    REQUIRE( ! missing);
    REQUIRE(missing.error() == error::file_system);
}

TEST_CASE("wallet scan matches input prefixes against input prefix filters", "[wallet]") {
    temporary_directory dir("scan_prefixes");
    std::vector<hash_digest> blocks;
    std::vector<data_chunk> filters;
    for (uint32_t height = 0; height < 40; ++height) {
        std::vector<uint16_t> values{uint16_t(height * 3), 0xa5a5};
        if (height % 4 == 0) values.push_back(0x1234);
        std::ranges::sort(values);
        values.erase(std::unique(values.begin(), values.end()), values.end());
        blocks.push_back(digest_of(700 + height));
        filters.push_back(chain::to_data(chain::input_prefix_filter{values}));
    }
    filters[33] = data_chunk{0x02, 0x00};
    auto const lookup = [&](uint32_t height) -> std::optional<hash_digest> {
        if (height < blocks.size()) return blocks[height];
        return std::nullopt;
    };
    auto store = wallet::filter_files::open(dir.path, 2, lookup);
    REQUIRE(store);
    REQUIRE( ! store->append(chain_of_filters(blocks, filters)));
    std::vector<wallet::scan_prefix> prefixes{{0x12, 8, 0}, {0x1234, 16, 10}, {0xa, 4, 39}, {0x7777, 16, 0}};
    auto const hits = wallet::scan_input_prefix_filters(store->data_path(), store->locations(0, 40), prefixes, 2);
    REQUIRE(hits);
    auto const heights_of = [&](size_t k) {
        std::vector<uint32_t> out;
        for (auto const& [height, matched] : *hits) {
            if (std::ranges::find(matched, k) != matched.end()) out.push_back(height);
        }
        return out;
    };
    std::vector<uint32_t> byte_prefix;
    std::vector<uint32_t> whole;
    for (uint32_t height = 0; height < 40; ++height) {
        if (height % 4 == 0 || height == 33 || (height * 3) >> 8 == 0x12) byte_prefix.push_back(height);
        if (height >= 10 && (height % 4 == 0 || height == 33)) whole.push_back(height);
    }
    REQUIRE(heights_of(0) == byte_prefix);
    REQUIRE(heights_of(1) == whole);
    REQUIRE(heights_of(2) == std::vector<uint32_t>{39});
    REQUIRE(heights_of(3) == std::vector<uint32_t>{33});
}

TEST_CASE("wallet regular filter match finds the scripts and outpoints of a block, and every item when the filter does not parse", "[wallet]") {
    // bchd simnet block 127 and the regular filter bchd serves for it (the
    // vectors of test/chain/compact_filter.cpp of the domain).
    auto const block = block_of_hex("01000030faec54d16b2a5bdbc3371f5e1593a8166e95a55d0df4f8ee33b6d8281bee6d79816c95ada87beb3bac7be56331697aa815d04915e94c1d6570812f083e9669f791b0f66affff7f20000000000301000000010000000000000000000000000000000000000000000000000000000000000000ffffffff12017f086f88e3cf77f9ac8d062f626368642fffffffff01c621062a010000001976a914b2275cc60091cdde77bddb8c132f60494cb2ea6b88ac00000000010000000178618fd7cc15e6088f2b77cc5ac25dd92ad86619bbe91d693427a9cd965e8708000000006441dfea2c0f1c08e147bdf45182ca31e99745704f5caa7f348d3b9e55ff01a594141bf235e38ad141d4389033312c576917f0652c754bb452127a069a3dfab511d9412102be31cab636041636a1846f4358a241f608ca9d9f29177585280590f9d97e033fffffffff0300a3e1110000000017a91469d7ef8f42a25e8791bb37d5fb48456f102a3cb9870000000000000000176a157363726970742066696c74657220766563746f72734a462418010000001976a91438b726cdbeb1d7f311544519c3412a4ad67c26fb88ac0000000002000000013a90cdbcad008793ceca6cc7d7dd73c22e1d85dd0858760234cae2c1c79fdf2100000000020152ffffffff02f07be111000000001976a914040404040404040404040404040404040404040488ac0000000000000000176a157363726970742066696c74657220766563746f727300000000");
    auto const served = decode_base16("07415a006e6bee1f3520c934743abce01b7f8480");
    REQUIRE(served);
    auto const& txs = block.transactions();
    auto const paid = txs[1].outputs()[0].script().bytes();
    auto const spent = wallet::outpoint_item(txs[2].inputs()[0].previous_output());
    auto const coinbase_input = wallet::outpoint_item(txs[0].inputs()[0].previous_output());
    auto const foreign = script_item(4);
    std::vector<byte_span> items{byte_span(foreign), byte_span(paid), byte_span(spent), byte_span(coinbase_input)};
    // The coinbase input names no output, so it is not in the filter.
    REQUIRE(wallet::match_regular_filter(block.hash(), *served, items) == std::vector<size_t>{1, 2});

    auto const other = digest_of(4);
    auto const filter = regular_filter_of(other, {script_item(2), script_item(9)});
    auto const two = script_item(2);
    auto const one = script_item(1);
    std::vector<byte_span> asked{byte_span(one), byte_span(two)};
    REQUIRE(wallet::match_regular_filter(other, filter, asked) == std::vector<size_t>{1});
    REQUIRE(wallet::match_regular_filter(other, data_chunk{0x05}, asked) == std::vector<size_t>{0, 1});
    REQUIRE(wallet::match_regular_filter(other, data_chunk{}, asked).size() == 2u);
    auto truncated = filter;
    truncated.resize(2);
    REQUIRE(wallet::match_regular_filter(other, truncated, asked).size() == 2u);
}
