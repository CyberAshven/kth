// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Wallet support answered from the node's own chain, against the blockchain
// test fixture: a regtest chain the node stores. Nothing is asked of any peer.

#include <test_helpers.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>

#include <kth/domain/chain/input_prefix_filter.hpp>
#include <kth/node/wallet/service.hpp>

#include "sync_harness.hpp"
#include "../../blockchain/test/reorg_chain_fixture.hpp"
#include "../../blockchain/test/regtest_miner.hpp"

using namespace kth;
using namespace std::chrono_literals;
namespace chain = kth::domain::chain;
namespace message = kth::domain::message;
namespace wallet = kth::node::wallet;

namespace {

template <typename Awaitable>
auto wait_for(wallet::service& service, Awaitable&& operation) {
    auto future = ::asio::co_spawn(service.executor(), std::forward<Awaitable>(operation), ::asio::use_future);
    REQUIRE(future.wait_for(60s) == std::future_status::ready);
    return future.get();
}

hash_digest digest_of(uint32_t seed) {
    uint8_t const bytes[] = {uint8_t(seed), uint8_t(seed >> 8), uint8_t(seed >> 16), uint8_t(seed >> 24), 0xc3};
    return sha256_hash(bytes);
}

// Blocks 1 to `length` paying the miner's script; when `spend_at` is set,
// that block spends block 1's coinbase back to the miner, so that coinbase is
// fully spent. The blocks are stored and connected by the fixture.
struct stored_chain {
    std::vector<chain::block> blocks;
    std::optional<chain::transaction> spend;
    test::chain_fixture fixture;

    stored_chain(char const* name, uint32_t length, std::optional<uint32_t> spend_at) : fixture(name) {
        auto const flags = blockchain::settings(domain::config::network::regtest).enabled_flags();
        auto const base_time = uint32_t(zulu_time()) - (length + 30) * test::block_spacing;
        auto previous = chain::block::genesis_regtest().hash();
        for (uint32_t height = 1; height <= length; ++height) {
            std::vector<chain::transaction> txs;
            uint64_t fees = 0;
            if (spend_at && height == *spend_at) {
                auto const& coinbase = blocks.front().transactions().front();
                spend = test::spend_p2pkh(coinbase, 0, coinbase.outputs()[0].value() - 1000, flags);
                txs.push_back(*spend);
                fees = 1000;
            }
            blocks.push_back(test::mine_block(previous, height, base_time + height * test::block_spacing, 3, std::move(txs), fees));
            previous = blocks.back().hash();
        }
        REQUIRE(fixture.created());
        REQUIRE(fixture.start());
        REQUIRE(fixture.organizer().add_headers(test::headers_of(blocks)).headers_added == blocks.size());
        test::persist_headers(fixture, blocks, 1);
        test::connect_bodies(fixture, blocks, 1);
        REQUIRE(fixture.chain().chain_view()->connected_tip_height == length);
    }
};

} // namespace

TEST_CASE("wallet from the node's own chain: histories, balances, unspent outputs, RPA and a spent transaction by hash alone", "[wallet][wallet_chain]") {
    stored_chain stored("wallet_chain_answers", 110, 101);
    auto own = wallet::service::from_chain(stored.fixture.chain(), domain::config::network::regtest);
    REQUIRE(own->current().source == "chain");
    REQUIRE(own->current().synced);
    REQUIRE(own->current().tip == 110u);

    // Found by its hash alone, before anything is watched: a coinbase whose
    // only output is spent.
    auto const coinbase_txid = stored.blocks.front().transactions().front().hash();
    auto const found = wait_for(*own, own->transaction(coinbase_txid, std::nullopt));
    REQUIRE(found);
    REQUIRE(found->confirmed);
    REQUIRE(found->confirmed->height == 1u);
    auto const proof = wait_for(*own, own->merkle(coinbase_txid, std::nullopt));
    REQUIRE(proof);
    REQUIRE(chain::block::merkle_branch_root(coinbase_txid, proof->confirmed.position, proof->branch) ==
        stored.blocks.front().header().merkle());
    auto const missing = wait_for(*own, own->transaction(digest_of(4242), std::nullopt));
    REQUIRE( ! missing);
    REQUIRE(missing.error() == error::not_found);

    // Every coinbase and the spend; every coinbase but the first unspent.
    auto const miner = wallet::target::of_script(test::miner_script().bytes());
    auto const history = wait_for(*own, own->history(miner));
    REQUIRE(history);
    REQUIRE(history->synced == 110u);
    REQUIRE(history->confirmed.size() == 111u);
    REQUIRE(history->confirmed[101].txid == stored.spend->hash());
    REQUIRE(history->confirmed[101].height == 101u);
    REQUIRE(history->confirmed[101].inputs.size() == 1u);
    auto const coins = wait_for(*own, own->unspent(miner, wallet::token_filter::include));
    REQUIRE(coins);
    REQUIRE(coins->size() == 110u);
    REQUIRE(std::ranges::none_of(*coins, [&](auto const& coin) { return coin.outpoint == chain::point(coinbase_txid, 0); }));
    auto const balance = wait_for(*own, own->get_balance(miner, wallet::token_filter::include));
    REQUIRE(balance);
    uint64_t total = 0;
    for (auto const& coin : *coins) total += coin.value;
    REQUIRE(balance->confirmed == total);
    REQUIRE(balance->unconfirmed == 0);
    // Tokens only: the miner holds none.
    auto const tokens = wait_for(*own, own->unspent(miner, wallet::token_filter::only));
    REQUIRE(tokens);
    REQUIRE(tokens->empty());

    // RPA: the spend's first input is in its own prefix; a prefix wider than
    // 16 bits is refused.
    auto const value = chain::input_prefix(stored.spend->inputs()[0]);
    auto const rpa = wait_for(*own, own->rpa_history(value, 16, 0, std::nullopt));
    REQUIRE(rpa);
    REQUIRE(std::ranges::any_of(*rpa, [&](auto const& match) { return match.txid == stored.spend->hash() && match.height == 101u; }));
    REQUIRE( ! wait_for(*own, own->rpa_history(0x1ff, 8, 0, std::nullopt)));

    // An output by its outpoint: spent, then unspent.
    auto const spent = wait_for(*own, own->output(chain::point(coinbase_txid, 0)));
    REQUIRE(spent);
    REQUIRE( ! *spent);
    auto const later = stored.blocks[50].transactions().front().hash();
    auto const unspent = wait_for(*own, own->output(chain::point(later, 0)));
    REQUIRE(unspent);
    REQUIRE(*unspent);
    REQUIRE((*unspent)->height == 51u);

    // Header proofs: each branch folds its block hash to the same root.
    auto const branch5 = wait_for(*own, own->header_branch(5, 10));
    auto const branch9 = wait_for(*own, own->header_branch(9, 10));
    REQUIRE(branch5);
    REQUIRE(branch9);
    REQUIRE(branch5->second == branch9->second);
    REQUIRE(chain::block::merkle_branch_root(stored.blocks[4].hash(), 5, branch5->first) == branch5->second);
    REQUIRE( ! wait_for(*own, own->header_branch(11, 10)));
}

TEST_CASE("wallet double-spend proofs: the full node lists its own", "[wallet][wallet_chain]") {
    stored_chain stored("wallet_chain_dsproofs", 110, std::nullopt);
    auto& node_chain = stored.fixture.chain();
    auto const flags = blockchain::settings(domain::config::network::regtest).enabled_flags();

    // A mempool transaction spending block 1's coinbase, and a proof that the
    // same output was spent twice.
    auto const& coinbase = stored.blocks.front().transactions().front();
    auto const pending = test::spend_p2pkh(coinbase, 0, coinbase.outputs()[0].value() - 1000, flags);
    auto const pooled = std::make_shared<message::transaction const>(pending);
    auto submitted = ::asio::co_spawn(node_chain.executor(), [&node_chain, pooled]() -> ::asio::awaitable<code> {
        co_return co_await node_chain.submit_transaction(pooled);
    }, ::asio::use_future);
    REQUIRE(submitted.get() == error::success);

    auto own = wallet::service::from_chain(node_chain, domain::config::network::regtest);
    // No proof yet: nothing listed.
    REQUIRE(wait_for(*own, own->dsproof_transactions()).empty());
    REQUIRE( ! wait_for(*own, own->dsproof(pending.hash())));

    message::double_spend_proof::spender first{};
    first.out_sequence = 1;
    first.push_data = data_chunk{0x01, 0x02};
    message::double_spend_proof::spender second{};
    second.out_sequence = 2;
    second.push_data = data_chunk{0x03, 0x04};
    auto const proof = std::make_shared<message::double_spend_proof const>(chain::output_point{coinbase.hash(), 0}, first, second);
    auto organized = ::asio::co_spawn(node_chain.executor(), [&node_chain, proof]() -> ::asio::awaitable<code> {
        co_return co_await node_chain.organize(proof);
    }, ::asio::use_future);
    REQUIRE(organized.get() == error::success);

    REQUIRE(wait_for(*own, own->dsproof_transactions()) == std::vector<hash_digest>{pending.hash()});
    auto const by_txid = wait_for(*own, own->dsproof(pending.hash()));
    REQUIRE(by_txid);
    REQUIRE(by_txid->dspid == proof->hash());
    REQUIRE(by_txid->txid == pending.hash());
    REQUIRE(by_txid->outpoint == chain::point(coinbase.hash(), 0));
    auto const by_hash = wait_for(*own, own->dsproof(proof->hash()));
    REQUIRE(by_hash);
    REQUIRE(by_hash->txid == pending.hash());
    // The mempool transaction itself is in the mempool copy.
    auto const pending_found = wait_for(*own, own->transaction(pending.hash(), std::nullopt));
    REQUIRE(pending_found);
    REQUIRE( ! pending_found->confirmed);
}
