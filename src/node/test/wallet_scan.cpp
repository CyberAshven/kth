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

#include <kth/domain/chain/compact_filter.hpp>
#include <kth/domain/chain/golomb_set.hpp>
#include <kth/domain/chain/input_prefix_filter.hpp>
#include <kth/node/wallet/filter_files.hpp>
#include <kth/node/wallet/scan.hpp>

using namespace kth;
namespace chain = kth::domain::chain;
namespace wallet = kth::node::wallet;

namespace {

struct temporary_directory {
    std::filesystem::path path;
    explicit temporary_directory(std::string_view tag)
        : path(std::filesystem::temp_directory_path() / (std::string("kth_wallet_scan_") + std::string(tag))) {
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

// A served script filter (root, then the set) holding `keys`, for `block`.
data_chunk script_filter_of(hash_digest const& block, std::vector<hash_digest> const& keys) {
    std::vector<data_chunk> items;
    for (auto const& key : keys) items.push_back(to_chunk(key));
    auto const set = chain::golomb_set::build(chain::compact_filter_key(block), items);
    REQUIRE(set);
    data_chunk served(hash_size, 0x42);
    auto const bytes = set->to_data();
    served.insert(served.end(), bytes.begin(), bytes.end());
    return served;
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

TEST_CASE("wallet scan finds each key in its blocks from its own height, on any number of threads", "[wallet]") {
    temporary_directory dir("scan_keys");
    std::vector<hash_digest> watched;
    for (uint32_t i = 0; i < 4; ++i) watched.push_back(digest_of(90000 + i));
    std::vector<hash_digest> blocks;
    std::vector<data_chunk> filters;
    for (uint32_t height = 0; height < 200; ++height) {
        auto const block = digest_of(height * 7 + 3);
        std::vector<hash_digest> keys;
        for (uint32_t j = 0; j < 50; ++j) keys.push_back(digest_of(height * 1000 + j + 100000));
        // Key i is in the blocks whose height is a multiple of i + 2.
        for (uint32_t i = 0; i < watched.size(); ++i) {
            if (height % (i + 2) == 0) keys.push_back(watched[i]);
        }
        blocks.push_back(block);
        filters.push_back(script_filter_of(block, keys));
    }
    // A filter too short to hold a root matches every key.
    filters[150] = data_chunk(10, 0);
    auto const lookup = [&](uint32_t height) -> std::optional<hash_digest> {
        if (height < blocks.size()) return blocks[height];
        return std::nullopt;
    };
    auto store = wallet::filter_files::open(dir.path, 1, lookup);
    REQUIRE(store);
    REQUIRE( ! store->append(chain_of_filters(blocks, filters)));
    std::vector<wallet::scan_key> keys;
    for (uint32_t i = 0; i < watched.size(); ++i) keys.push_back({watched[i], i * 10});
    auto const where = store->locations(0, 200);
    for (size_t threads : {1, 3, 8}) {
        auto const hits = wallet::scan_script_filters(store->data_path(), where, keys, threads);
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
            // A false positive is possible, at 1 in 784931 per block and key.
            for (auto const height : expected) {
                REQUIRE(std::ranges::find(found, height) != found.end());
            }
            REQUIRE(found.size() <= expected.size() + 1);
        }
    }
    // Nothing to scan.
    REQUIRE(wallet::scan_script_filters(store->data_path(), {}, keys, 2)->empty());
    REQUIRE(wallet::scan_script_filters(store->data_path(), where, {}, 2)->empty());
    // A file that is gone.
    auto const missing = wallet::scan_script_filters(dir.path / "missing.dat", where, keys, 1);
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

TEST_CASE("wallet script filter match returns every key when the filter does not parse", "[wallet]") {
    auto const block = digest_of(4);
    std::vector<hash_digest> keys{digest_of(1), digest_of(2), digest_of(3)};
    auto const filter = script_filter_of(block, {digest_of(2), digest_of(9)});
    REQUIRE(wallet::match_script_filter(block, filter, keys) == std::vector<size_t>{1});
    REQUIRE(wallet::match_script_filter(block, data_chunk(32, 0), keys) == std::vector<size_t>{0, 1, 2});
    auto truncated = filter;
    truncated.resize(hash_size + 2);
    REQUIRE(wallet::match_script_filter(block, truncated, keys).size() == 3u);
}
