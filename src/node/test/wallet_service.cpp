// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Wallet support answered from peers, against peers in this process: each
// listens on a port the OS assigns, serves a regtest chain with its script
// and input prefix filters, and answers what a Knuth node answers.

#include "wallet_peer_harness.hpp"

#include "../../blockchain/test/reorg_chain_fixture.hpp"

TEST_CASE("wallet from peers: histories, balances, a transaction by hash alone, RPA, and nothing watched sent", "[wallet][wallet_peers]") {
    answers_chain made;
    auto& served = made.served;
    auto const& category = made.category;
    auto const& nft = made.nft;
    auto const& paying = made.paying;
    auto const& spend = made.spend;
    auto const& again = made.again;
    auto const& unrelated = made.unrelated;
    auto const& ground = made.ground;

    test_peer peer(served);
    temporary_directory dir("answers");
    auto settings = settings_for(dir.path, {peer.endpoint()});
    settings.sent_log = dir.path / "sent.log";
    running_service running(settings);
    auto& service = *running.service;

    auto const synced = wait_for(service, service.wait_synced(30s));
    REQUIRE(synced);
    REQUIRE(synced->header_height == 30u);
    REQUIRE(synced->tip == 30u);
    REQUIRE(synced->input_prefix_tip == 30u);
    REQUIRE(synced->transaction_keys == true);

    auto const target = wallet::target::of_script(watched_script);
    auto const history = wait_for(service, service.history(target));
    REQUIRE(history);
    REQUIRE(history->synced == 30u);
    REQUIRE(history->confirmed.size() == 3u);
    REQUIRE(history->confirmed[0].txid == paying.hash());
    REQUIRE(history->confirmed[0].height == 5u);
    REQUIRE(history->confirmed[1].txid == spend.hash());
    REQUIRE(history->confirmed[1].inputs.at(0).value == 1000u);
    REQUIRE(history->confirmed[2].txid == again.hash());
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

    // A token category, followed to the script it was sent to.
    auto const tokens = wait_for(service, service.unspent(wallet::target::of_category(category), wallet::token_filter::only));
    REQUIRE(tokens);
    REQUIRE(tokens->size() == 1u);
    REQUIRE(tokens->at(0).token == chain::token_data_opt{nft});
    REQUIRE(tokens->at(0).script == other_script);

    // A transaction no watched target touches, by its hash alone.
    auto const found = wait_for(service, service.transaction(unrelated.hash(), std::nullopt));
    REQUIRE(found);
    REQUIRE(found->tx.hash() == unrelated.hash());
    REQUIRE(found->confirmed);
    REQUIRE(found->confirmed->height == 15u);
    auto const proof = wait_for(service, service.merkle(unrelated.hash(), std::nullopt));
    REQUIRE(proof);
    REQUIRE(chain::block::merkle_branch_root(unrelated.hash(), proof->confirmed.position, proof->branch) ==
        served.blocks[15].header().merkle());
    auto const missing = wait_for(service, service.transaction(digest_of(999), std::nullopt));
    REQUIRE( ! missing);
    REQUIRE(missing.error() == error::not_found);

    // RPA: every transaction with a counted input in the prefix.
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
    REQUIRE( ! wait_for(service, service.rpa_history(0x1ff, 8, 0, std::nullopt)));

    // An output by its outpoint: unspent, and spent by a later block.
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

    // Nothing sent names what was watched or looked up.
    std::ifstream log(settings.sent_log);
    std::string line;
    std::vector<std::string> secrets{
        encode_base16(watched_script),
        encode_base16(chain::script_key(watched_script)),
        encode_hash(chain::script_key(watched_script)),
        encode_base16(chain::category_key(category)),
        encode_base16(chain::transaction_key(unrelated.hash())),
        encode_base16(unrelated.hash()),
        encode_hash(unrelated.hash()),
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
    }
    REQUIRE(lines > 0u);

    // A broadcast to the only peer: no reject after its pong.
    auto const fresh = spending({chain::point(again.hash(), 0)}, {{400, other_script}}, 6);
    auto const sent = wait_for(service, service.broadcast(fresh));
    REQUIRE(sent);
    REQUIRE(*sent == fresh.hash());
    auto const pending = wait_for(service, service.history(target));
    REQUIRE(pending);
    REQUIRE(pending->unconfirmed.size() == 1u);
    REQUIRE(pending->unconfirmed[0].inputs.at(0).value == 500u);
    auto const after = wait_for(service, service.get_balance(target, wallet::token_filter::include));
    REQUIRE(after);
    REQUIRE(after->unconfirmed == -500);
}

TEST_CASE("wallet from peers: a filter that does not fit its block is found and dropped", "[wallet][wallet_peers]") {
    served_chain served;
    for (uint32_t height = 1; height <= 10; ++height) {
        served.mine({spending({chain::point(digest_of(200 + height), 0)}, {{10, other_script}}, height)});
    }
    // The tip's filter leaves out the block's outputs, which the client
    // finds when it downloads the tip.
    auto const tip = served.blocks.back().hash();
    std::vector<data_chunk> wrong{to_chunk(digest_of(5))};
    auto const set = chain::golomb_set::build(chain::compact_filter_key(tip), wrong);
    REQUIRE(set);
    data_chunk served_bytes(hash_size, 0);
    auto const bytes = set->to_data();
    served_bytes.insert(served_bytes.end(), bytes.begin(), bytes.end());
    served.replace_script_filter(10, served_bytes);

    test_peer peer(served);
    temporary_directory dir("dispute");
    running_service running(settings_for(dir.path, {peer.endpoint()}));
    auto& service = *running.service;
    // The peer that served it is refused, which stays in the status.
    REQUIRE(status_reaches(service, [](wallet::status const& now) {
        return std::ranges::any_of(now.refused, [](std::string const& why) {
            return why.find("does not fit block 10") != std::string::npos;
        });
    }));
    REQUIRE(service.current().tip.value_or(0) < 10u);
}

TEST_CASE("wallet from peers: a birthday syncs only the filters after it, and peers that disagree are refused", "[wallet][wallet_peers]") {
    served_chain served;
    auto const early = spending({chain::point(digest_of(300), 0)}, {{100, watched_script}}, 1);
    auto const late = spending({chain::point(digest_of(301), 0)}, {{200, watched_script}}, 2);
    for (uint32_t height = 1; height <= 1010; ++height) {
        std::vector<chain::transaction> txs;
        if (height == 3) txs.push_back(early);
        if (height == 1004) txs.push_back(late);
        served.mine(std::move(txs));
    }

    SECTION("birthday") {
        test_peer peer(served);
        temporary_directory dir("birthday");
        auto settings = settings_for(dir.path, {peer.endpoint()});
        settings.birthday = 1000;
        running_service running(settings);
        auto& service = *running.service;
        auto const synced = wait_for(service, service.wait_synced(60s));
        REQUIRE(synced);
        REQUIRE(synced->tip == 1010u);
        // Eleven filters of each type, not a thousand and eleven.
        REQUIRE(synced->filter_bytes < 11u * 2u * 200u);
        auto const history = wait_for(service, service.history(wallet::target::of_script(watched_script)));
        REQUIRE(history);
        REQUIRE(history->confirmed.size() == 1u);
        REQUIRE(history->confirmed[0].txid == late.hash());
    }

    SECTION("disagreeing checkpoints") {
        auto other = served;
        other.replace_script_filter(500, data_chunk(40, 0x33));
        test_peer first(served);
        test_peer second(other);
        temporary_directory dir("disagree");
        running_service running(settings_for(dir.path, {first.endpoint(), second.endpoint()}));
        auto& service = *running.service;
        REQUIRE(status_reaches(service, [](wallet::status const& now) {
            return now.error.find("disagree") != std::string::npos;
        }));
        REQUIRE( ! service.current().synced);
    }
}

TEST_CASE("wallet from the node's own chain answers as from peers", "[wallet][wallet_chain]") {
    // Blocks 1 to 110 pay the miner's script; block 101 spends block 1's
    // coinbase back to it, so that coinbase is fully spent.
    auto const flags = blockchain::settings(domain::config::network::regtest).enabled_flags();
    auto const base_time = uint32_t(zulu_time()) - 140 * test::block_spacing;
    std::vector<chain::block> blocks;
    auto previous = chain::block::genesis_regtest().hash();
    std::optional<chain::transaction> spend;
    for (uint32_t height = 1; height <= 110; ++height) {
        std::vector<chain::transaction> txs;
        uint64_t fees = 0;
        if (height == 101) {
            auto const& coinbase = blocks.front().transactions().front();
            spend = test::spend_p2pkh(coinbase, 0, coinbase.outputs()[0].value() - 1000, flags);
            txs.push_back(*spend);
            fees = 1000;
        }
        blocks.push_back(test::mine_block(previous, height, base_time + height * test::block_spacing, 3, std::move(txs), fees));
        previous = blocks.back().hash();
    }
    test::chain_fixture fixture("wallet_chain_source");
    REQUIRE(fixture.created());
    REQUIRE(fixture.start());
    REQUIRE(fixture.organizer().add_headers(test::headers_of(blocks)).headers_added == blocks.size());
    test::persist_headers(fixture, blocks, 1);
    test::connect_bodies(fixture, blocks, 1);
    REQUIRE(fixture.chain().chain_view()->connected_tip_height == 110u);

    auto own = wallet::service::from_chain(fixture.chain(), domain::config::network::regtest);
    REQUIRE(own->current().source == "chain");

    // Found by its hash alone, before anything is watched: a coinbase whose
    // only output is spent.
    auto const coinbase_txid = blocks.front().transactions().front().hash();
    auto const found = wait_for(*own, own->transaction(coinbase_txid, std::nullopt));
    REQUIRE(found);
    REQUIRE(found->confirmed);
    REQUIRE(found->confirmed->height == 1u);
    REQUIRE( ! wait_for(*own, own->transaction(digest_of(4242), std::nullopt)));

    auto const miner = wallet::target::of_script(test::miner_script().bytes());
    auto const own_history = wait_for(*own, own->history(miner));
    REQUIRE(own_history);
    REQUIRE(own_history->synced == 110u);
    REQUIRE(own_history->confirmed.size() == 111u);
    auto const own_balance = wait_for(*own, own->get_balance(miner, wallet::token_filter::include));
    REQUIRE(own_balance);
    auto const own_coins = wait_for(*own, own->unspent(miner, wallet::token_filter::include));
    REQUIRE(own_coins);
    REQUIRE(own_coins->size() == 110u);
    auto const value = chain::input_prefix(spend->inputs()[0]);
    auto const own_rpa = wait_for(*own, own->rpa_history(value, 16, 0, std::nullopt));
    REQUIRE(own_rpa);
    REQUIRE(std::ranges::any_of(*own_rpa, [&](auto const& match) { return match.txid == spend->hash() && match.height == 101u; }));

    // The same blocks, served by a peer: the answers are the same.
    served_chain served;
    for (auto const& blk : blocks) served.append(blk);
    test_peer peer(served);
    temporary_directory dir("chain_equality");
    running_service running(settings_for(dir.path, {peer.endpoint()}));
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
    auto const peer_found = wait_for(from_peers, from_peers.transaction(coinbase_txid, std::nullopt));
    REQUIRE(peer_found);
    REQUIRE(peer_found->confirmed->height == 1u);
}

TEST_CASE("wallet double-spend proofs: the full node lists its own as the peer path lists those relayed", "[wallet][wallet_chain]") {
    auto const flags = blockchain::settings(domain::config::network::regtest).enabled_flags();
    auto const base_time = uint32_t(zulu_time()) - 140 * test::block_spacing;
    std::vector<chain::block> blocks;
    auto previous = chain::block::genesis_regtest().hash();
    for (uint32_t height = 1; height <= 110; ++height) {
        blocks.push_back(test::mine_block(previous, height, base_time + height * test::block_spacing, 9, {}, 0));
        previous = blocks.back().hash();
    }
    test::chain_fixture fixture("wallet_dsproofs");
    REQUIRE(fixture.created());
    REQUIRE(fixture.start());
    REQUIRE(fixture.organizer().add_headers(test::headers_of(blocks)).headers_added == blocks.size());
    test::persist_headers(fixture, blocks, 1);
    test::connect_bodies(fixture, blocks, 1);
    auto& node_chain = fixture.chain();

    // A mempool transaction spending block 1's coinbase, and a proof that the
    // same output was spent twice.
    auto const& coinbase = blocks.front().transactions().front();
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
    auto const by_hash = wait_for(*own, own->dsproof(proof->hash()));
    REQUIRE(by_hash);

    // The same chain, mempool transaction and proof, relayed by a peer.
    served_chain served;
    for (auto const& blk : blocks) served.append(blk);
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
    REQUIRE(peer_list == own_list);
    auto const peer_proof = wait_for(from_peers, from_peers.dsproof(pending.hash()));
    REQUIRE(peer_proof);
    REQUIRE(peer_proof->dspid == own_proof->dspid);
    REQUIRE(peer_proof->raw == own_proof->raw);
    REQUIRE(peer_proof->outpoint == own_proof->outpoint);
}
