// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Wallet support answered from peers, against peers in this process: each
// listens on a port the OS assigns, serves a regtest chain with its regular
// filters and input prefix filters, and answers what a Knuth node answers.

#include "wallet_peer_harness.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>

#include <kth/infrastructure/formats/base_16.hpp>
#include <kth/node/wallet/blocks.hpp>

namespace {

size_t getdata_lines(std::filesystem::path const& log_path) {
    std::ifstream log(log_path);
    std::string line;
    size_t count = 0;
    while (std::getline(log, line)) count += line.starts_with("getdata ") ? 1 : 0;
    return count;
}

std::vector<hash_digest> confirmed_txids(wallet::target_history const& history) {
    std::vector<hash_digest> out;
    for (auto const& entry : history.confirmed) out.push_back(entry.txid);
    return out;
}

bool refused_with(wallet::status const& now, std::string const& endpoint, std::string const& why) {
    return std::ranges::any_of(now.refused, [&](std::string const& entry) {
        return entry.starts_with(endpoint) && entry.find(why) != std::string::npos;
    });
}

} // namespace

TEST_CASE("wallet from peers: a payment and the spend its outpoint shows, RPA from input prefix filters, and nothing watched sent", "[wallet][wallet_peers]") {
    answers_chain made;
    auto& served = made.served;
    auto const& paying = made.paying;
    auto const& spend = made.spend;
    auto const& again = made.again;
    auto const& unrelated = made.unrelated;
    auto const& ground = made.ground;

    test_peer peer(served);
    temporary_directory dir("answers");
    auto settings = settings_for(dir.path, {peer.endpoint()});
    settings.sent_log = dir.path / "sent.log";
    settings.input_prefix_filters = true;
    running_service running(settings);
    auto& service = *running.service;

    auto const synced = wait_for(service, service.wait_synced(30s));
    REQUIRE(synced);
    REQUIRE(synced->header_height == 30u);
    REQUIRE(synced->tip == 30u);
    REQUIRE(synced->input_prefix_tip == 30u);
    REQUIRE( ! synced->transaction_keys);

    // The payment at 5 by its script, the spend at 9 by the outpoint it
    // spends (it pays another script), the payment at 12.
    auto const target = wallet::target::of_script(watched_script);
    auto const history = wait_for(service, service.history(target));
    REQUIRE(history);
    REQUIRE(history->synced == 30u);
    REQUIRE(confirmed_txids(*history) == std::vector<hash_digest>{paying.hash(), spend.hash(), again.hash()});
    REQUIRE(history->confirmed[0].height == 5u);
    REQUIRE(history->confirmed[1].height == 9u);
    REQUIRE(history->confirmed[1].inputs.at(0).value == 1000u);
    REQUIRE(history->confirmed[1].outputs.empty());
    REQUIRE(history->unconfirmed.empty());

    auto const balance = wait_for(service, service.get_balance(target, wallet::token_filter::include));
    REQUIRE(balance);
    REQUIRE(balance->confirmed == 500u);
    REQUIRE(balance->unconfirmed == 0);
    auto const coins = wait_for(service, service.unspent(target, wallet::token_filter::include));
    REQUIRE(coins);
    REQUIRE(coins->size() == 1u);
    REQUIRE(coins->at(0).outpoint == chain::point(again.hash(), 0));
    REQUIRE(coins->at(0).height == 12u);

    // The regular filter holds scripts as they are: a script known by its
    // hash alone and a token category wait for script keys.
    auto const by_hash = wait_for(service, service.history(wallet::target::of_script_hash(sha256_hash(watched_script))));
    REQUIRE( ! by_hash);
    REQUIRE(by_hash.error() == error::not_implemented);
    auto const tokens = wait_for(service, service.unspent(wallet::target::of_category(made.category), wallet::token_filter::only));
    REQUIRE( ! tokens);
    REQUIRE(tokens.error() == error::not_implemented);
    REQUIRE(wait_for(service, service.watch({target, wallet::target::of_category(made.category)}, 0)) == error::not_implemented);

    // A transaction no watched target touches: not by its hash alone, which
    // the regular filter does not hold, but with its height.
    auto const alone = wait_for(service, service.transaction(unrelated.hash(), std::nullopt));
    REQUIRE( ! alone);
    REQUIRE(alone.error() == error::not_implemented);
    auto const found = wait_for(service, service.transaction(unrelated.hash(), 15u));
    REQUIRE(found);
    REQUIRE(found->tx.hash() == unrelated.hash());
    REQUIRE(found->confirmed);
    REQUIRE(found->confirmed->height == 15u);
    auto const proof = wait_for(service, service.merkle(unrelated.hash(), std::nullopt));
    REQUIRE(proof);
    REQUIRE(chain::block::merkle_branch_root(unrelated.hash(), proof->confirmed.position, proof->branch) ==
        served.blocks[15].header().merkle());
    auto const missing = wait_for(service, service.transaction(digest_of(999), 15u));
    REQUIRE( ! missing);
    REQUIRE(missing.error() == error::not_found);
    // A transaction of a watched history needs no height.
    auto const own = wait_for(service, service.transaction(spend.hash(), std::nullopt));
    REQUIRE(own);
    REQUIRE(own->confirmed->height == 9u);

    // RPA: the block the input prefix filters point to holds the transaction
    // with a counted input in the prefix.
    auto const value = chain::input_prefix(ground.inputs()[0]);
    auto const matches = wait_for(service, service.rpa_history(value, 16, 0, std::nullopt));
    REQUIRE(matches);
    auto const ground_match = std::ranges::find_if(*matches, [&](auto const& match) { return match.txid == ground.hash(); });
    REQUIRE(ground_match != matches->end());
    REQUIRE(ground_match->height == 20u);
    REQUIRE(std::ranges::find(ground_match->inputs, 0u) != ground_match->inputs.end());
    auto const byte_prefix = wait_for(service, service.rpa_history(uint16_t(value >> 8), 8, 0, std::nullopt));
    REQUIRE(byte_prefix);
    REQUIRE(std::ranges::any_of(*byte_prefix, [&](auto const& match) { return match.txid == ground.hash(); }));
    auto const before = wait_for(service, service.rpa_history(value, 16, 0, 19));
    REQUIRE(before);
    REQUIRE(std::ranges::none_of(*before, [&](auto const& match) { return match.txid == ground.hash(); }));
    REQUIRE( ! wait_for(service, service.rpa_history(0x1ff, 8, 0, std::nullopt)));

    // An output by its outpoint: owned and unspent, owned and spent, past
    // the outputs of its transaction; one of a transaction found with its
    // height, spent at 17, and one of a watched transaction no block spends.
    auto const kept = wait_for(service, service.output(chain::point(again.hash(), 0)));
    REQUIRE(kept);
    REQUIRE(*kept);
    REQUIRE((*kept)->height == 12u);
    REQUIRE((*kept)->value == 500u);
    auto const gone = wait_for(service, service.output(chain::point(paying.hash(), 0)));
    REQUIRE(gone);
    REQUIRE( ! *gone);
    auto const none = wait_for(service, service.output(chain::point(paying.hash(), 9)));
    REQUIRE(none);
    REQUIRE( ! *none);
    auto const foreign_spent = wait_for(service, service.output(chain::point(unrelated.hash(), 0)));
    REQUIRE(foreign_spent);
    REQUIRE( ! *foreign_spent);
    auto const foreign_kept = wait_for(service, service.output(chain::point(paying.hash(), 1)));
    REQUIRE(foreign_kept);
    REQUIRE(*foreign_kept);
    REQUIRE((*foreign_kept)->value == 2000u);
    REQUIRE((*foreign_kept)->height == 5u);

    // Header proofs: each branch folds its block hash to the same root.
    auto const branch5 = wait_for(service, service.header_branch(5, 10));
    auto const branch9 = wait_for(service, service.header_branch(9, 10));
    REQUIRE(branch5);
    REQUIRE(branch9);
    REQUIRE(branch5->second == branch9->second);
    REQUIRE(chain::block::merkle_branch_root(served.blocks[5].hash(), 5, branch5->first) == branch5->second);
    REQUIRE(chain::block::merkle_branch_root(served.blocks[9].hash(), 9, branch9->first) == branch9->second);
    REQUIRE( ! wait_for(service, service.header_branch(11, 10)));
    REQUIRE(wait_for(service, service.dsproof_transactions()).empty());
    REQUIRE( ! wait_for(service, service.dsproof(again.hash())));
    REQUIRE(wait_for(service, service.changed(0, 0s)) > 0u);

    // Nothing sent names what was watched or looked up: no script, no
    // outpoint, no transaction hash, no prefix.
    std::ifstream log(settings.sent_log);
    std::string line;
    std::vector<std::string> secrets{
        encode_base16(watched_script),
        encode_base16(wallet::outpoint_item(chain::point(paying.hash(), 0))),
        encode_base16(wallet::outpoint_item(chain::point(unrelated.hash(), 0))),
        encode_base16(unrelated.hash()),
        encode_hash(unrelated.hash()),
        encode_base16(spend.hash()),
        encode_hash(spend.hash()),
    };
    size_t lines = 0;
    while (std::getline(log, line)) {
        ++lines;
        auto const command = line.substr(0, line.find(' '));
        REQUIRE(command != "getcfrange");
        REQUIRE(command != "getcfproof");
        REQUIRE(command != "filterload");
        REQUIRE(command != "filteradd");
        for (auto const& secret : secrets) REQUIRE(line.find(secret) == std::string::npos);
        // Filters of the regular and input prefix types only.
        if (command == "getcfilters" || command == "getcfheaders" || command == "getcfcheckpt") {
            REQUIRE((line.substr(command.size() + 1, 2) == "00" || line.substr(command.size() + 1, 2) == "02"));
        }
    }
    REQUIRE(lines > 0u);

    // A broadcast, over a connection of its own: no reject after its pong.
    auto const fresh = spending({chain::point(again.hash(), 0)}, {{400, other_script}}, 6);
    auto const sessions = peer.sessions.load();
    auto const sent = wait_for(service, service.broadcast(fresh));
    REQUIRE(sent);
    REQUIRE(*sent == fresh.hash());
    REQUIRE(peer.sessions.load() == sessions + 1);
    auto const pending = wait_for(service, service.history(target));
    REQUIRE(pending);
    REQUIRE(pending->unconfirmed.size() == 1u);
    REQUIRE(pending->unconfirmed[0].inputs.at(0).value == 500u);
    auto const after = wait_for(service, service.get_balance(target, wallet::token_filter::include));
    REQUIRE(after);
    REQUIRE(after->unconfirmed == -500);
}

TEST_CASE("wallet from peers: a spend in the same block, the same scan window, and a later one", "[wallet][wallet_peers]") {
    // A first window of 2,016 filters, and the start of a second.
    served_chain served;
    auto const first = spending({chain::point(digest_of(360), 0)}, {{100, watched_script}}, 1);
    auto const first_spent = spending({chain::point(first.hash(), 0)}, {{90, other_script}}, 2);
    auto const second = spending({chain::point(digest_of(361), 0)}, {{200, watched_script}, {5, other_script}}, 3);
    auto const second_spent = spending({chain::point(second.hash(), 0)}, {{190, other_script}}, 4);
    auto const third = spending({chain::point(digest_of(362), 0)}, {{300, watched_script}}, 5);
    auto const third_spent = spending({chain::point(third.hash(), 0)}, {{290, other_script}}, 6);
    auto const last = spending({chain::point(digest_of(363), 0)}, {{400, watched_script}}, 7);
    for (uint32_t height = 1; height <= 2100; ++height) {
        std::vector<chain::transaction> txs;
        if (height == 10) txs.push_back(first);
        if (height == 50) txs.push_back(first_spent);
        if (height == 100) txs.push_back(second);
        if (height == 2090) txs.push_back(second_spent);
        if (height == 2095) {
            txs.push_back(third);
            txs.push_back(third_spent);
        }
        if (height == 2099) txs.push_back(last);
        served.mine(std::move(txs));
    }
    test_peer peer(served);
    temporary_directory dir("windows");
    running_service running(settings_for(dir.path, {peer.endpoint()}));
    auto& service = *running.service;
    auto const synced = wait_for(service, service.wait_synced(60s));
    REQUIRE(synced);
    REQUIRE(synced->tip == 2100u);
    auto const target = wallet::target::of_script(watched_script);
    auto const history = wait_for(service, service.history(target));
    REQUIRE(history);
    REQUIRE(confirmed_txids(*history) == std::vector<hash_digest>{first.hash(), first_spent.hash(), second.hash(),
        second_spent.hash(), third.hash(), third_spent.hash(), last.hash()});
    auto const coins = wait_for(service, service.unspent(target, wallet::token_filter::include));
    REQUIRE(coins);
    REQUIRE(coins->size() == 1u);
    REQUIRE(coins->at(0).outpoint == chain::point(last.hash(), 0));
    auto const balance = wait_for(service, service.get_balance(target, wallet::token_filter::include));
    REQUIRE(balance);
    REQUIRE(balance->confirmed == 400u);
}

TEST_CASE("wallet from peers: a filter that does not fit its block is found and dropped", "[wallet][wallet_peers]") {
    served_chain served;
    for (uint32_t height = 1; height <= 10; ++height) {
        served.mine({spending({chain::point(digest_of(200 + height), 0)}, {{10, other_script}}, height)});
    }

    SECTION("a regular filter") {
        // The tip's filter leaves out the block's outputs, which the client
        // finds when it downloads the tip.
        served.replace_filter(regular_type, 10, regular_filter_without(served.blocks[10], other_script));
        test_peer peer(served);
        temporary_directory dir("dispute");
        running_service running(settings_for(dir.path, {peer.endpoint()}));
        auto& service = *running.service;
        // The peer that served it is refused, which stays in the status.
        REQUIRE(status_reaches(service, [](wallet::status const& now) {
            return std::ranges::any_of(now.refused, [](std::string const& why) {
                return why.find("a filter that does not fit block 10") != std::string::npos;
            });
        }));
        REQUIRE(service.current().tip.value_or(0) < 10u);
    }

    SECTION("an input prefix filter") {
        // The tip's input prefix filter leaves out the value of its input.
        served.replace_filter(prefix_type, 10, chain::to_data(chain::input_prefix_filter{}));
        test_peer peer(served);
        temporary_directory dir("dispute_prefix");
        auto settings = settings_for(dir.path, {peer.endpoint()});
        settings.input_prefix_filters = true;
        running_service running(settings);
        auto& service = *running.service;
        REQUIRE(status_reaches(service, [](wallet::status const& now) {
            return std::ranges::any_of(now.refused, [](std::string const& why) {
                return why.find("an input prefix filter that does not fit block 10") != std::string::npos;
            });
        }));
        REQUIRE(service.current().input_prefix_tip.value_or(0) < 10u);
    }
}

TEST_CASE("wallet from peers: a birthday syncs only the filters after it", "[wallet][wallet_peers]") {
    served_chain served;
    auto const early = spending({chain::point(digest_of(300), 0)}, {{100, watched_script}}, 1);
    auto const late = spending({chain::point(digest_of(301), 0)}, {{200, watched_script}}, 2);
    for (uint32_t height = 1; height <= 1010; ++height) {
        std::vector<chain::transaction> txs;
        if (height == 3) txs.push_back(early);
        if (height == 1004) txs.push_back(late);
        served.mine(std::move(txs));
    }
    test_peer peer(served);
    temporary_directory dir("birthday");
    auto settings = settings_for(dir.path, {peer.endpoint()});
    settings.birthday = 1000;
    running_service running(settings);
    auto& service = *running.service;
    auto const synced = wait_for(service, service.wait_synced(60s));
    REQUIRE(synced);
    REQUIRE(synced->tip == 1010u);
    // Eleven filters, not a thousand and eleven.
    REQUIRE(synced->filter_bytes < 11u * 200u);
    auto const history = wait_for(service, service.history(wallet::target::of_script(watched_script)));
    REQUIRE(history);
    REQUIRE(confirmed_txids(*history) == std::vector<hash_digest>{late.hash()});
}

TEST_CASE("wallet from the node's own chain answers as from peers", "[wallet][wallet_peers][wallet_chain]") {
    // Blocks 1 to 110 pay the miner's script; block 101 spends block 1's
    // coinbase back to it, so that coinbase is fully spent.
    stored_chain stored("wallet_peers_equality", 110, 101);
    auto own = wallet::service::from_chain(stored.fixture.chain(), domain::config::network::regtest);
    REQUIRE(own->current().source == "chain");
    auto const miner = wallet::target::of_script(test::miner_script().bytes());
    auto const own_history = wait_for(*own, own->history(miner));
    REQUIRE(own_history);
    REQUIRE(own_history->confirmed.size() == 111u);
    auto const own_balance = wait_for(*own, own->get_balance(miner, wallet::token_filter::include));
    REQUIRE(own_balance);
    auto const own_coins = wait_for(*own, own->unspent(miner, wallet::token_filter::include));
    REQUIRE(own_coins);
    REQUIRE(own_coins->size() == 110u);
    auto const value = chain::input_prefix(stored.spend->inputs()[0]);
    auto const own_rpa = wait_for(*own, own->rpa_history(value, 16, 0, std::nullopt));
    REQUIRE(own_rpa);
    REQUIRE(std::ranges::any_of(*own_rpa, [&](auto const& match) { return match.txid == stored.spend->hash() && match.height == 101u; }));
    auto const coinbase_txid = stored.blocks.front().transactions().front().hash();

    // The same blocks, served by a peer with their filters: the answers are
    // the same, the spend at 101 found by the outpoint it spends.
    served_chain served;
    for (auto const& blk : stored.blocks) served.append(blk);
    test_peer peer(served);
    temporary_directory dir("chain_equality");
    auto settings = settings_for(dir.path, {peer.endpoint()});
    settings.input_prefix_filters = true;
    running_service running(settings);
    auto& from_peers = *running.service;
    REQUIRE(wait_for(from_peers, from_peers.wait_synced(30s)));
    auto const peer_history = wait_for(from_peers, from_peers.history(miner));
    REQUIRE(peer_history);
    auto const entries = [](wallet::target_history const& history) {
        std::vector<std::pair<hash_digest, uint32_t>> out;
        for (auto const& tx : history.confirmed) out.emplace_back(tx.txid, tx.height);
        return out;
    };
    REQUIRE(entries(*peer_history) == entries(*own_history));
    auto const peer_balance = wait_for(from_peers, from_peers.get_balance(miner, wallet::token_filter::include));
    REQUIRE(peer_balance);
    REQUIRE(peer_balance->confirmed == own_balance->confirmed);
    auto const peer_coins = wait_for(from_peers, from_peers.unspent(miner, wallet::token_filter::include));
    REQUIRE(peer_coins);
    auto const outpoints = [](std::vector<wallet::utxo> const& coins) {
        std::vector<chain::point> out;
        for (auto const& coin : coins) out.push_back(coin.outpoint);
        std::ranges::sort(out);
        return out;
    };
    REQUIRE(outpoints(*peer_coins) == outpoints(*own_coins));
    auto const peer_rpa = wait_for(from_peers, from_peers.rpa_history(value, 16, 0, std::nullopt));
    REQUIRE(peer_rpa);
    auto const matches = [](std::vector<wallet::rpa_match> const& listed) {
        std::vector<std::pair<hash_digest, uint32_t>> out;
        for (auto const& match : listed) out.emplace_back(match.txid, match.height.value_or(0));
        std::ranges::sort(out);
        return out;
    };
    REQUIRE(matches(*peer_rpa) == matches(*own_rpa));
    // The spent coinbase: in the history, so found by its hash alone.
    auto const peer_found = wait_for(from_peers, from_peers.transaction(coinbase_txid, std::nullopt));
    REQUIRE(peer_found);
    REQUIRE(peer_found->confirmed->height == 1u);
}

// Expected to fail until the dsproof message parses: its spender reads its
// push data with read_remaining_bytes(), so the first spender takes the
// second's bytes and no relayed proof parses (k-nuth/kth#589, #587; the
// specification's push data is a counted list of pushes). The test peer
// serves the proof and the client receives it; it then fails to parse.
TEST_CASE("wallet double-spend proofs: the full node lists its own as the peer path lists those relayed", "[wallet][wallet_peers][wallet_chain][!shouldfail]") {
    stored_chain stored("wallet_dsproofs", 110, std::nullopt);
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

    auto own = wallet::service::from_chain(node_chain, domain::config::network::regtest);
    auto const own_list = wait_for(*own, own->dsproof_transactions());
    REQUIRE(own_list == std::vector<hash_digest>{pending.hash()});
    auto const own_proof = wait_for(*own, own->dsproof(pending.hash()));
    REQUIRE(own_proof);
    REQUIRE(own_proof->dspid == proof->hash());
    REQUIRE(own_proof->txid == pending.hash());

    // The same chain, mempool transaction and proof, relayed by a peer.
    served_chain served;
    for (auto const& blk : stored.blocks) served.append(blk);
    served.pooled.push_back(pending);
    served.proofs.push_back(*proof);
    test_peer peer(served);
    temporary_directory dir("dsproofs");
    running_service running(settings_for(dir.path, {peer.endpoint()}));
    auto& from_peers = *running.service;
    REQUIRE(wait_for(from_peers, from_peers.wait_synced(30s)));
    // A relayed proof is a change: woken by it, not by sleeping between looks.
    auto seen = wait_for(from_peers, from_peers.changed(0, 0s));
    auto peer_list = wait_for(from_peers, from_peers.dsproof_transactions());
    auto const deadline = std::chrono::steady_clock::now() + 30s;
    while (peer_list.empty() && std::chrono::steady_clock::now() < deadline) {
        seen = wait_for(from_peers, from_peers.changed(seen, 5s));
        peer_list = wait_for(from_peers, from_peers.dsproof_transactions());
    }
    auto const copied = wait_for(from_peers, from_peers.mempool());
    auto const proof_known = wait_for(from_peers, from_peers.dsproof(proof->hash()));
    INFO("sessions " << peer.sessions.load() << ", announcements " << peer.announcements.load()
        << ", transactions served " << peer.served_transactions.load() << ", proofs served " << peer.served_proofs.load()
        << ", mempool copy " << copied.count << ", proof known " << bool(proof_known));
    REQUIRE(peer_list == own_list);
    auto const peer_proof = wait_for(from_peers, from_peers.dsproof(pending.hash()));
    REQUIRE(peer_proof);
    REQUIRE(peer_proof->dspid == own_proof->dspid);
    REQUIRE(peer_proof->raw == own_proof->raw);
    REQUIRE(peer_proof->outpoint == own_proof->outpoint);
}

TEST_CASE("wallet from peers: peers that misstate filter checkpoints or headers are refused and the others serve", "[wallet][wallet_peers]") {
    served_chain served;
    auto const early = spending({chain::point(digest_of(310), 0)}, {{100, watched_script}}, 1);
    auto const late = spending({chain::point(digest_of(311), 0)}, {{200, watched_script}}, 2);
    for (uint32_t height = 1; height <= 1010; ++height) {
        std::vector<chain::transaction> txs;
        if (height == 3) txs.push_back(early);
        if (height == 1004) txs.push_back(late);
        served.mine(std::move(txs));
    }
    auto const settled = [&](wallet::service& service) {
        auto const synced = wait_for(service, service.wait_synced(60s));
        REQUIRE(synced);
        REQUIRE(synced->tip == 1010u);
        auto const history = wait_for(service, service.history(wallet::target::of_script(watched_script)));
        REQUIRE(history);
        REQUIRE(confirmed_txids(*history) == std::vector<hash_digest>{early.hash(), late.hash()});
    };

    SECTION("a checkpoint list one short") {
        auto other = served;
        other.short_checkpoints = true;
        test_peer liar(other);
        test_peer honest(served);
        temporary_directory dir("short_checkpoints");
        running_service running(settings_for(dir.path, {liar.endpoint(), honest.endpoint()}));
        auto& service = *running.service;
        REQUIRE(status_reaches(service, [&](wallet::status const& now) {
            return refused_with(now, liar.endpoint(), "filter checkpoints");
        }));
        settled(service);
    }

    SECTION("checkpoints that differ, settled by the block where the filter headers part") {
        auto other = served;
        other.replace_filter(regular_type, 500, data_chunk(40, 0x33));
        test_peer liar(other);
        test_peer honest(served);
        temporary_directory dir("disagree");
        running_service running(settings_for(dir.path, {liar.endpoint(), honest.endpoint()}));
        auto& service = *running.service;
        REQUIRE(status_reaches(service, [&](wallet::status const& now) {
            return refused_with(now, liar.endpoint(), "does not fit block 500");
        }));
        settled(service);
        REQUIRE( ! refused_with(service.current(), honest.endpoint(), ""));
    }
}

TEST_CASE("wallet from peers: a filter server a block behind the headers is asked again once it catches up", "[wallet][wallet_peers]") {
    // The filters' peer stays silent for a tip it does not have (BIP157):
    // that is no sign it serves no filters.
    served_chain behind;
    for (uint32_t height = 1; height <= 20; ++height) behind.mine({});
    auto ahead = behind;
    ahead.mine({});
    test_peer filters(behind);
    test_peer headers(ahead, false);
    temporary_directory dir("lagging_filters");
    running_service running(settings_for(dir.path, {filters.endpoint(), headers.endpoint()}));
    auto& service = *running.service;
    REQUIRE(status_reaches(service, [](wallet::status const& now) { return now.header_height == 21u; }));
    // Asked for block 21's checkpoints, it stays silent; the request times
    // out, and the round ends without them.
    REQUIRE(filters.silent.get_future().wait_for(30s) == std::future_status::ready);
    filters.switch_to(ahead);
    REQUIRE(status_reaches(service, [](wallet::status const& now) { return now.synced && now.tip == 21u; }));
}

TEST_CASE("wallet from peers: a filter above the last checkpoint is compared with another peer's and settled by its block", "[wallet][wallet_peers]") {
    served_chain served;
    auto const paid = spending({chain::point(digest_of(320), 0)}, {{700, watched_script}}, 1);
    for (uint32_t height = 1; height <= 30; ++height) {
        std::vector<chain::transaction> txs;
        if (height == 25) txs.push_back(paid);
        served.mine(std::move(txs));
    }
    // The liar's regular filter of block 25 leaves the payment out: a wallet
    // that took it would never see it.
    auto other = served;
    other.replace_filter(regular_type, 25, regular_filter_without(served.blocks[25], watched_script));

    // The liar is asked first.
    test_peer liar(other);
    test_peer honest(served);
    temporary_directory dir("cross_check");
    running_service running(settings_for(dir.path, {liar.endpoint(), honest.endpoint()}));
    auto& service = *running.service;
    REQUIRE(status_reaches(service, [&](wallet::status const& now) {
        return refused_with(now, liar.endpoint(), "does not fit block 25");
    }));
    REQUIRE(status_reaches(service, [](wallet::status const& now) { return now.synced && now.tip == 30u; }));
    auto const history = wait_for(service, service.history(wallet::target::of_script(watched_script)));
    REQUIRE(history);
    REQUIRE(confirmed_txids(*history) == std::vector<hash_digest>{paid.hash()});
}

TEST_CASE("wallet from peers: a reorganization rolls the histories back, below the birthday too", "[wallet][wallet_peers]") {
    served_chain first;
    auto const orphaned = spending({chain::point(digest_of(330), 0)}, {{800, watched_script}}, 1);
    auto const kept = spending({chain::point(digest_of(332), 0)}, {{600, watched_script}}, 3);
    // The coin kept below the fork is spent on the first branch only.
    auto const orphaned_spend = spending({chain::point(kept.hash(), 0)}, {{590, other_script}}, 4);
    for (uint32_t height = 1; height <= 40; ++height) {
        std::vector<chain::transaction> txs;
        if (height == 20) txs.push_back(kept);
        if (height == 35) txs.push_back(orphaned);
        if (height == 36) txs.push_back(orphaned_spend);
        first.mine(std::move(txs));
    }
    // A heavier branch from `fork`, paying the watched script at 38 instead.
    auto const branch = [&](uint32_t fork) {
        served_chain second;
        second.base_time = first.base_time;
        for (uint32_t height = 1; height <= fork; ++height) second.append(first.blocks[height]);
        return second;
    };
    auto const replacing = spending({chain::point(digest_of(331), 0)}, {{900, watched_script}}, 2);
    auto const target = wallet::target::of_script(watched_script);

    SECTION("above the birthday") {
        auto second = branch(30);
        for (uint32_t height = 31; height <= 45; ++height) {
            std::vector<chain::transaction> txs;
            if (height == 38) txs.push_back(replacing);
            second.mine(std::move(txs), 1000 + height);
        }
        test_peer peer(first);
        temporary_directory dir("peer_reorg");
        running_service running(settings_for(dir.path, {peer.endpoint()}));
        auto& service = *running.service;
        REQUIRE(wait_for(service, service.wait_synced(30s)));
        auto const before = wait_for(service, service.history(target));
        REQUIRE(before);
        REQUIRE(confirmed_txids(*before) == std::vector<hash_digest>{kept.hash(), orphaned.hash(), orphaned_spend.hash()});
        auto const before_coins = wait_for(service, service.unspent(target, wallet::token_filter::include));
        REQUIRE(before_coins);
        REQUIRE(before_coins->size() == 1u);
        peer.switch_to(second);
        REQUIRE(status_reaches(service, [](wallet::status const& now) { return now.synced && now.tip == 45u; }));
        // The coin of 20 is unspent again, the payment of 35 gone, 38 read.
        auto const after = wait_for(service, service.history(target));
        REQUIRE(after);
        REQUIRE(confirmed_txids(*after) == std::vector<hash_digest>{kept.hash(), replacing.hash()});
        auto const after_coins = wait_for(service, service.unspent(target, wallet::token_filter::include));
        REQUIRE(after_coins);
        REQUIRE(after_coins->size() == 2u);
        auto const balance = wait_for(service, service.get_balance(target, wallet::token_filter::include));
        REQUIRE(balance);
        REQUIRE(balance->confirmed == 1500u);
        REQUIRE( ! wait_for(service, service.transaction(orphaned.hash(), 35u)));
    }

    SECTION("below the birthday") {
        // The store starts at 32; the fork at 25 empties it.
        auto second = branch(25);
        for (uint32_t height = 26; height <= 45; ++height) {
            std::vector<chain::transaction> txs;
            if (height == 38) txs.push_back(replacing);
            second.mine(std::move(txs), 2000 + height);
        }
        test_peer peer(first);
        temporary_directory dir("peer_reorg_birthday");
        auto settings = settings_for(dir.path, {peer.endpoint()});
        settings.birthday = 32;
        running_service running(settings);
        auto& service = *running.service;
        REQUIRE(wait_for(service, service.wait_synced(30s)));
        peer.switch_to(second);
        REQUIRE(status_reaches(service, [](wallet::status const& now) { return now.synced && now.tip == 45u; }));
        auto const after = wait_for(service, service.history(target));
        REQUIRE(after);
        REQUIRE(confirmed_txids(*after) == std::vector<hash_digest>{replacing.hash()});
    }
}

TEST_CASE("wallet from peers: a transaction a peer pushes without being asked is not taken", "[wallet][wallet_peers]") {
    served_chain served;
    for (uint32_t height = 1; height <= 10; ++height) served.mine({});
    auto const announced = spending({chain::point(digest_of(340), 0)}, {{50, watched_script}}, 1);
    auto const pushed = spending({chain::point(digest_of(341), 0)}, {{5000, watched_script}}, 2);
    served.pooled.push_back(announced);
    served.pushed.push_back(pushed);
    test_peer peer(served);
    temporary_directory dir("pushed");
    running_service running(settings_for(dir.path, {peer.endpoint()}));
    auto& service = *running.service;
    REQUIRE(wait_for(service, service.wait_synced(30s)));
    auto const target = wallet::target::of_script(watched_script);
    // The pushed transaction came first; once the announced one is taken,
    // the pushed one was seen and dropped.
    auto seen = wait_for(service, service.changed(0, 0s));
    auto const deadline = std::chrono::steady_clock::now() + 30s;
    std::optional<wallet::target_history> history;
    while (std::chrono::steady_clock::now() < deadline) {
        auto const now = wait_for(service, service.history(target));
        REQUIRE(now);
        history = *now;
        if ( ! history->unconfirmed.empty()) break;
        seen = wait_for(service, service.changed(seen, 5s));
    }
    REQUIRE(history);
    REQUIRE(history->unconfirmed.size() == 1u);
    REQUIRE(history->unconfirmed[0].txid == announced.hash());
    REQUIRE( ! wait_for(service, service.transaction(pushed.hash(), std::nullopt)));
}

TEST_CASE("wallet from peers: which blocks are downloaded names nothing watched, and what that costs", "[wallet][wallet_peers]") {
    // Two whole buckets of 144 blocks and part of a third.
    served_chain served;
    auto const first_paid = spending({chain::point(digest_of(350), 0)}, {{100, watched_script}}, 1);
    auto const second_paid = spending({chain::point(digest_of(351), 0)}, {{200, watched_script}}, 2);
    auto const ground = spending({chain::point(digest_of(352), 0), chain::point(digest_of(353), 0)}, {{66, other_script}}, 3);
    for (uint32_t height = 1; height <= 300; ++height) {
        std::vector<chain::transaction> txs;
        if (height == 50) txs.push_back(first_paid);
        if (height == 200) txs.push_back(second_paid);
        if (height == 100) txs.push_back(ground);
        if (height % 7 == 0) txs.push_back(spending({chain::point(digest_of(1000 + height), 0)}, {{1, other_script}}, height));
        served.mine(std::move(txs));
    }
    test_peer peer(served);
    temporary_directory dir("privacy");
    auto const log_path = dir.path / "sent.log";
    auto settings = settings_for(dir.path, {peer.endpoint()});
    settings.sent_log = log_path;
    auto const target = wallet::target::of_script(watched_script);
    auto const bucket = [](uint32_t height) { return height / 144; };
    auto const prefix = chain::input_prefix(ground.inputs()[0]);

    std::set<uint32_t> first_session;
    uint64_t covered_bytes = 0;
    uint64_t sessions_before = 0;
    uint64_t sessions_after = 0;
    {
        running_service running(settings);
        auto& service = *running.service;
        REQUIRE(wait_for(service, service.wait_synced(30s)));
        auto const asked_before = blocks_asked(log_path, served);
        auto const received_before = service.current().bytes_received;
        sessions_before = peer.sessions.load();
        auto const history = wait_for(service, service.history(target));
        REQUIRE(history);
        REQUIRE(confirmed_txids(*history) == std::vector<hash_digest>{first_paid.hash(), second_paid.hash()});
        covered_bytes = service.current().bytes_received - received_before;
        sessions_after = peer.sessions.load();
        auto const asked = blocks_asked(log_path, served);
        for (auto const height : asked) {
            if ( ! asked_before.contains(height)) first_session.insert(height);
        }
        // Each needed block came with three others of its bucket.
        for (auto const real : {50u, 200u}) {
            REQUIRE(first_session.contains(real));
            auto const others = std::ranges::count_if(first_session, [&](uint32_t height) {
                return height != real && bucket(height) == bucket(real);
            });
            REQUIRE(others >= 3);
        }
        // Each over a connection of its own.
        REQUIRE(sessions_after - sessions_before >= first_session.size());

        // Asking for a transaction again downloads nothing.
        REQUIRE(wait_for(service, service.transaction(first_paid.hash(), std::nullopt)));
        auto const lines = getdata_lines(log_path);
        REQUIRE(wait_for(service, service.transaction(first_paid.hash(), std::nullopt)));
        REQUIRE(wait_for(service, service.merkle(first_paid.hash(), std::nullopt)));
        REQUIRE(getdata_lines(log_path) == lines);

        // RPA without input prefix filters: every block of the range, so
        // that none of the 65,536 prefixes is told apart by what was
        // downloaded.
        auto const asked_rpa_before = blocks_asked(log_path, served);
        auto const matches = wait_for(service, service.rpa_history(prefix, 16, 90, 110));
        REQUIRE(matches);
        REQUIRE(std::ranges::any_of(*matches, [&](auto const& match) { return match.txid == ground.hash(); }));
        auto const asked_rpa = blocks_asked(log_path, served);
        std::set<uint32_t> downloaded;
        for (auto height = 90u; height <= 110u; ++height) {
            if (asked_rpa.contains(height) || asked_rpa_before.contains(height)) downloaded.insert(height);
        }
        REQUIRE(downloaded.size() == 21u);

        // The same blocks without cover, for the cost.
        temporary_directory bare_dir("privacy_bare");
        auto bare = settings_for(bare_dir.path, {peer.endpoint()});
        bare.isolate_downloads = false;
        bare.decoy_blocks = 0;
        running_service bare_running(bare);
        auto& bare_service = *bare_running.service;
        REQUIRE(wait_for(bare_service, bare_service.wait_synced(30s)));
        auto const bare_before = bare_service.current().bytes_received;
        REQUIRE(wait_for(bare_service, bare_service.history(target)));
        auto const bare_bytes = bare_service.current().bytes_received - bare_before;
        std::printf("wallet privacy cost: a history of 2 blocks in 300 took %llu bytes with decoys and isolation, "
            "%llu bytes without\n", (unsigned long long)covered_bytes, (unsigned long long)bare_bytes);
        std::fflush(stdout);
    }

    // RPA with input prefix filters: the block they point to, with three
    // others of its bucket, not the whole range.
    {
        temporary_directory rpa_dir("privacy_rpa");
        auto const rpa_log = rpa_dir.path / "sent.log";
        auto with_filters = settings_for(rpa_dir.path, {peer.endpoint()});
        with_filters.sent_log = rpa_log;
        with_filters.input_prefix_filters = true;
        running_service running(with_filters);
        auto& service = *running.service;
        REQUIRE(wait_for(service, service.wait_synced(30s)));
        auto const asked_before = blocks_asked(rpa_log, served);
        auto const matches = wait_for(service, service.rpa_history(prefix, 16, 90, 110));
        REQUIRE(matches);
        REQUIRE(std::ranges::any_of(*matches, [&](auto const& match) { return match.txid == ground.hash() && match.height == 100u; }));
        std::set<uint32_t> asked;
        for (auto const height : blocks_asked(rpa_log, served)) {
            if ( ! asked_before.contains(height)) asked.insert(height);
        }
        REQUIRE(asked.contains(100u));
        auto const others = std::ranges::count_if(asked, [&](uint32_t height) {
            return height != 100u && bucket(height) == bucket(100u);
        });
        REQUIRE(others >= 3);
        std::set<uint32_t> in_range;
        for (auto const height : asked) {
            if (height >= 90u && height <= 110u) in_range.insert(height);
        }
        REQUIRE(in_range.size() < 21u);
    }

    // After a restart, the same blocks bring the same decoys.
    std::filesystem::remove(log_path);
    {
        running_service running(settings);
        auto& service = *running.service;
        REQUIRE(wait_for(service, service.wait_synced(30s)));
        auto const asked_before = blocks_asked(log_path, served);
        REQUIRE(wait_for(service, service.history(target)));
        std::set<uint32_t> second_session;
        for (auto const height : blocks_asked(log_path, served)) {
            if ( ! asked_before.contains(height)) second_session.insert(height);
        }
        auto const in_buckets = [&](std::set<uint32_t> const& heights) {
            std::set<uint32_t> out;
            for (auto const height : heights) {
                if (bucket(height) == bucket(50) || bucket(height) == bucket(200)) out.insert(height);
            }
            return out;
        };
        REQUIRE(in_buckets(second_session) == in_buckets(first_session));
    }
}
