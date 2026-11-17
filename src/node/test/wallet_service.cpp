// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Wallet support answered from peers, against peers in this process: each
// listens on a port the OS assigns, serves a regtest chain with its script
// and input prefix filters, and answers what a Knuth node answers.

#include <test_helpers.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#include <kth/domain/chain/compact_filter.hpp>
#include <kth/domain/chain/golomb_set.hpp>
#include <kth/domain/chain/input_prefix_filter.hpp>
#include <kth/domain/chain/script_filter_builder.hpp>
#include <kth/network/peer_session.hpp>
#include <kth/network/protocols_coro.hpp>
#include <kth/node/wallet/electrum.hpp>
#include <kth/node/wallet/service.hpp>

#include "sync_harness.hpp"
#include "../../blockchain/test/reorg_chain_fixture.hpp"
#include "../../blockchain/test/regtest_miner.hpp"

using namespace kth;
using namespace std::chrono_literals;
namespace chain = kth::domain::chain;
namespace message = kth::domain::message;
namespace wallet = kth::node::wallet;
using tcp = ::asio::ip::tcp;

namespace {

struct temporary_directory {
    std::filesystem::path path;
    explicit temporary_directory(std::string_view tag)
        : path(std::filesystem::temp_directory_path() / (std::string("kth_wallet_service_") + std::string(tag))) {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
        std::filesystem::create_directories(path);
    }
    ~temporary_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

chain::script script_of(data_chunk bytes) {
    return chain::script(bytes, false);
}

hash_digest digest_of(uint32_t seed) {
    uint8_t const bytes[] = {uint8_t(seed), uint8_t(seed >> 8), uint8_t(seed >> 16), uint8_t(seed >> 24), 0xa5};
    return sha256_hash(bytes);
}

// Static data: decoded without assertions, which belong to test cases.
data_chunk decoded(std::string_view hex) {
    auto bytes = decode_base16(hex);
    return bytes ? std::move(*bytes) : data_chunk{};
}

data_chunk const watched_script = decoded("76a914111111111111111111111111111111111111111188ac");
data_chunk const other_script = decoded("76a914222222222222222222222222222222222222222288ac");

struct paid {
    uint64_t value;
    data_chunk script;
    chain::token_data_opt token{};
};

chain::transaction spending(std::vector<chain::point> const& previous, std::vector<paid> const& outputs, uint32_t salt) {
    chain::input::list inputs;
    for (auto const& point : previous) {
        inputs.emplace_back(chain::output_point{point.hash(), point.index()}, script_of({0x01, uint8_t(salt), uint8_t(salt >> 8)}), 0xfffffffe);
    }
    chain::output::list made;
    for (auto const& out : outputs) made.emplace_back(out.value, script_of(out.script), out.token);
    return chain::transaction(2, 0, std::move(inputs), std::move(made));
}

// A regtest chain with the filters a node serves for it.
struct served_chain {
    std::vector<chain::block> blocks;
    std::map<hash_digest, uint32_t> heights;
    std::map<chain::point, chain::output> created;
    std::vector<std::vector<chain::output>> spent;
    std::array<std::vector<data_chunk>, 3> filters;
    std::array<std::vector<hash_digest>, 3> hashes;
    std::array<std::vector<hash_digest>, 3> headers;
    // Announced to every client after the handshake, and served on request.
    std::vector<chain::transaction> pooled;
    std::vector<message::double_spend_proof> proofs;
    uint32_t base_time = uint32_t(std::time(nullptr)) - 4000u * 600u;

    served_chain() {
        add(chain::block::genesis_regtest(), {});
    }

    void add(chain::block blk, std::vector<chain::output> spends) {
        auto const height = uint32_t(blocks.size());
        heights[blk.hash()] = height;
        for (auto const& tx : blk.transactions()) {
            for (uint32_t vout = 0; vout < tx.outputs().size(); ++vout) {
                created.insert_or_assign(chain::point(tx.hash(), vout), tx.outputs()[vout]);
            }
        }
        auto const script = chain::build_script_filter(blk, spends);
        REQUIRE(script);
        put(1, chain::to_data(*script));
        put(2, chain::to_data(chain::build_input_prefix_filter(blk)));
        spent.push_back(std::move(spends));
        blocks.push_back(std::move(blk));
    }

    void put(uint8_t type, data_chunk filter) {
        auto const previous = headers[type].empty() ? null_hash : headers[type].back();
        auto const hash = bitcoin_hash(filter);
        data_chunk pair(hash.begin(), hash.end());
        pair.insert(pair.end(), previous.begin(), previous.end());
        headers[type].push_back(bitcoin_hash(pair));
        hashes[type].push_back(hash);
        filters[type].push_back(std::move(filter));
    }

    // Mines the next block with `txs` after its coinbase.
    void mine(std::vector<chain::transaction> txs) {
        auto const height = uint32_t(blocks.size());
        append(test::mine_block(blocks.back().hash(), height, base_time + height * 600, height, std::move(txs), 0));
    }

    // A block made elsewhere; an output it spends that this chain did not
    // make is taken for one of another script.
    void append(chain::block blk) {
        std::vector<chain::output> spends;
        auto const& txs = blk.transactions();
        for (size_t at = 1; at < txs.size(); ++at) {
            for (auto const& in : txs[at].inputs()) {
                auto const found = created.find(chain::point(in.previous_output().hash(), in.previous_output().index()));
                spends.push_back(found != created.end() ? found->second
                    : chain::output(1, script_of(other_script), chain::token_data_opt{}));
            }
        }
        add(std::move(blk), std::move(spends));
    }

    // Replaces a block's served script filter: the filter header chain
    // follows the new bytes, so only the block itself can show it is wrong.
    void replace_script_filter(uint32_t height, data_chunk filter) {
        auto saved = filters[1];
        saved[height] = std::move(filter);
        filters[1].clear();
        hashes[1].clear();
        headers[1].clear();
        for (auto& bytes : saved) put(1, std::move(bytes));
    }

    uint32_t tip() const { return uint32_t(blocks.size() - 1); }
};

network::settings server_settings() {
    network::settings settings(domain::config::network::regtest);
    settings.user_agent = "/WalletTestPeer:1/";
    settings.relay_transactions = false;
    settings.services = uint64_t(message::version::service::node_network) |
        uint64_t(message::version::service::node_compact_filters);
    settings.channel_handshake_seconds = 5;
    return settings;
}

// One in-process peer: listens on a port the OS assigns and answers headers,
// filters and blocks from its chain.
struct test_peer {
    served_chain const& chain;
    ::asio::io_context io;
    tcp::acceptor acceptor{io, tcp::endpoint(::asio::ip::make_address("127.0.0.1"), 0)};
    std::thread thread;
    std::atomic<size_t> sessions{0};

    explicit test_peer(served_chain const& served) : chain(served) {
        ::asio::co_spawn(io, accept(), ::asio::detached);
        thread = std::thread([this] { io.run(); });
    }

    ~test_peer() {
        ::asio::post(io, [this] {
            std::error_code ignored;
            acceptor.close(ignored);
        });
        io.stop();
        thread.join();
    }

    // Bound before the client is made: the port is ready when this returns.
    std::string endpoint() const {
        return "127.0.0.1:" + std::to_string(acceptor.local_endpoint().port());
    }

    ::asio::awaitable<void> accept() {
        for (;;) {
            auto [ec, socket] = co_await acceptor.async_accept(::asio::as_tuple(::asio::use_awaitable));
            if (ec) co_return;
            ++sessions;
            ::asio::co_spawn(io, serve(std::move(socket)), ::asio::detached);
        }
    }

    std::optional<uint32_t> height_of(hash_digest const& hash) const {
        auto const found = chain.heights.find(hash);
        if (found == chain.heights.end()) return std::nullopt;
        return found->second;
    }

    ::asio::awaitable<void> serve(tcp::socket socket) {
        auto const settings = server_settings();
        auto peer = std::make_shared<network::peer_session>(std::move(socket), settings, true);
        auto config = network::make_handshake_config(settings, chain.tip(), 0x5eed0000 + sessions.load());
        config.ws_discovery = false;
        auto const handshake = co_await network::perform_handshake_direct(*peer, config);
        if ( ! handshake) {
            peer->stop();
            co_return;
        }
        ::asio::co_spawn(io, [peer]() -> ::asio::awaitable<void> { (void)co_await peer->run(); }, ::asio::detached);
        auto const version = peer->negotiated_version();
        if ( ! chain.pooled.empty() || ! chain.proofs.empty()) {
            message::inventory_vector::list items;
            for (auto const& tx : chain.pooled) items.emplace_back(message::inventory_vector::type_id::transaction, tx.hash());
            for (auto const& proof : chain.proofs) items.emplace_back(message::inventory_vector::type_id::double_spend_proof, proof.hash());
            auto const announced = message::inventory::create(std::move(items));
            if (announced) (void)co_await peer->send(*announced);
        }
        for (;;) {
            auto received = co_await peer->receive(std::chrono::steady_clock::now() + 120s);
            if ( ! received) break;
            auto const& raw = *received;
            auto const command = raw.heading.command();
            byte_reader reader(raw.payload);
            if (command == message::ping::command) {
                auto const ping = message::ping::from_data(reader, version);
                if (ping) (void)co_await peer->send(message::pong(ping->nonce()));
            } else if (command == message::get_headers::command) {
                auto const ask = message::get_headers::from_data(reader, version);
                if ( ! ask) continue;
                uint32_t from = 0;
                for (auto const& hash : ask->start_hashes()) {
                    if (auto const at = height_of(hash)) {
                        from = *at;
                        break;
                    }
                }
                message::header::list list;
                for (auto height = from + 1; height <= chain.tip() && list.size() < 2000; ++height) {
                    list.emplace_back(chain.blocks[height].header());
                }
                auto const answer = message::headers::create(std::move(list));
                if (answer) (void)co_await peer->send(*answer);
            } else if (command == message::get_compact_filter_checkpoint::command) {
                auto const ask = message::get_compact_filter_checkpoint::from_data(reader, version);
                if ( ! ask || ask->filter_type == 0 || ask->filter_type > 2) continue;
                auto const stop = height_of(ask->stop_hash);
                if ( ! stop) continue;
                message::compact_filter_checkpoint answer{ask->filter_type, ask->stop_hash, {}};
                for (uint32_t height = 1000; height <= *stop; height += 1000) {
                    answer.filter_headers.push_back(chain.headers[ask->filter_type][height]);
                }
                (void)co_await peer->send(answer);
            } else if (command == message::get_compact_filter_headers::command) {
                auto const ask = message::get_compact_filter_headers::from_data(reader, version);
                if ( ! ask || ask->filter_type == 0 || ask->filter_type > 2) continue;
                auto const stop = height_of(ask->stop_hash);
                if ( ! stop || ask->start_height > *stop || *stop - ask->start_height >= 2000) continue;
                auto const type = ask->filter_type;
                message::compact_filter_headers answer{type, ask->stop_hash,
                    ask->start_height == 0 ? null_hash : chain.headers[type][ask->start_height - 1], {}};
                for (auto height = ask->start_height; height <= *stop; ++height) {
                    answer.filter_hashes.push_back(chain.hashes[type][height]);
                }
                (void)co_await peer->send(answer);
            } else if (command == message::get_compact_filters::command) {
                auto const ask = message::get_compact_filters::from_data(reader, version);
                if ( ! ask || ask->filter_type == 0 || ask->filter_type > 2) continue;
                auto const stop = height_of(ask->stop_hash);
                if ( ! stop || ask->start_height > *stop) continue;
                for (auto height = ask->start_height; height <= *stop; ++height) {
                    message::compact_filter answer{ask->filter_type, chain.blocks[height].hash(), chain.filters[ask->filter_type][height]};
                    (void)co_await peer->send(answer);
                }
            } else if (command == message::get_data::command) {
                auto const ask = message::get_data::from_data(reader, version);
                if ( ! ask) continue;
                for (auto const& item : ask->inventories()) {
                    auto const at = height_of(item.hash());
                    if (item.type() == message::inventory_vector::type_id::block && at) {
                        (void)co_await peer->send(message::block(chain.blocks[*at]));
                    }
                    if (item.type() == message::inventory_vector::type_id::transaction) {
                        for (auto const& tx : chain.pooled) {
                            if (tx.hash() == item.hash()) (void)co_await peer->send(message::transaction(tx));
                        }
                    }
                    if (item.type() == message::inventory_vector::type_id::double_spend_proof) {
                        for (auto const& proof : chain.proofs) {
                            if (proof.hash() == item.hash()) (void)co_await peer->send(proof);
                        }
                    }
                }
            }
        }
        peer->stop();
    }
};

template <typename Awaitable>
auto wait_for(wallet::service& service, Awaitable&& operation) {
    auto future = ::asio::co_spawn(service.executor(), std::forward<Awaitable>(operation), ::asio::use_future);
    REQUIRE(future.wait_for(60s) == std::future_status::ready);
    return future.get();
}

// Whether the service's status comes to satisfy `reached` within 30 s, woken
// by its changes (a moved status is one), not by sleeping between looks.
template <typename Predicate>
bool status_reaches(wallet::service& service, Predicate reached) {
    auto const deadline = std::chrono::steady_clock::now() + 30s;
    auto seen = wait_for(service, service.changed(0, 0s));
    while ( ! reached(service.current())) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        seen = wait_for(service, service.changed(seen, 5s));
    }
    return true;
}

struct running_service {
    std::unique_ptr<wallet::service> service;
    std::future<std::error_code> running;

    explicit running_service(wallet::peer_settings settings) {
        auto made = wallet::service::from_peers(std::move(settings));
        REQUIRE(made);
        service = std::move(*made);
        running = ::asio::co_spawn(service->executor(), service->run(), ::asio::use_future);
    }

    ~running_service() {
        service->stop();
        (void)running.wait_for(30s);
    }
};

wallet::peer_settings settings_for(std::filesystem::path const& directory, std::vector<std::string> peers) {
    wallet::peer_settings settings;
    settings.network = domain::config::network::regtest;
    settings.directory = directory;
    settings.peers = std::move(peers);
    settings.scan_threads = 2;
    settings.request_timeout = 10s;
    settings.header_capacity = 5000;
    return settings;
}

} // namespace

TEST_CASE("wallet from peers: histories, balances, a transaction by hash alone, RPA, and nothing watched sent", "[wallet][wallet_peers]") {
    served_chain served;
    auto const category = digest_of(7);
    auto const nft = chain::make_non_fungible(category, chain::capability_t::minting, data_chunk{0x0a});
    // Height 5 pays the watched script and mints a token to another script;
    // 9 spends the watched coin; 12 pays it again; 15 has a transaction no
    // target touches; 20 one whose first input a sender ground for RPA.
    auto const paying = spending({chain::point(digest_of(100), 0)}, {{1000, watched_script}, {2000, other_script}, {3000, other_script, nft}}, 1);
    auto const spend = spending({chain::point(paying.hash(), 0)}, {{900, other_script}}, 2);
    auto const again = spending({chain::point(digest_of(101), 1)}, {{500, watched_script}}, 3);
    auto const unrelated = spending({chain::point(digest_of(102), 0)}, {{77, other_script}}, 4);
    auto const ground = spending({chain::point(digest_of(103), 0), chain::point(digest_of(104), 0)}, {{66, other_script}}, 5);
    for (uint32_t height = 1; height <= 30; ++height) {
        std::vector<chain::transaction> txs;
        if (height == 5) txs.push_back(paying);
        if (height == 9) txs.push_back(spend);
        if (height == 12) txs.push_back(again);
        if (height == 15) txs.push_back(unrelated);
        if (height == 20) txs.push_back(ground);
        served.mine(std::move(txs));
    }

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

    // The local Electrum listener answers the same, in Fulcrum's shapes.
    wallet::electrum_server electrum(service, domain::config::network::regtest);
    auto const ask = [&](std::string const& method, std::string const& params) {
        return wait_for(service, electrum.answer(R"({"jsonrpc":"2.0","id":7,"method":")" + method + R"(","params":)" + params + "}"));
    };
    auto const hash_text = "\"" + encode_hash(sha256_hash(watched_script)) + "\"";
    REQUIRE(ask("blockchain.scripthash.get_balance", "[" + hash_text + "]") ==
        R"({"jsonrpc":"2.0","result":{"confirmed":500,"unconfirmed":0},"id":7})");
    auto const listed = ask("blockchain.scripthash.get_history", "[" + hash_text + "]");
    REQUIRE(listed.find(encode_hash(paying.hash()) + R"(","height":5})") != std::string::npos);
    REQUIRE(listed.find(encode_hash(again.hash()) + R"(","height":12})") != std::string::npos);
    auto const ranged = ask("blockchain.scripthash.get_history", "[" + hash_text + ",6,12]");
    REQUIRE(ranged.find(encode_hash(spend.hash())) != std::string::npos);
    REQUIRE(ranged.find(encode_hash(again.hash())) == std::string::npos);
    REQUIRE(ask("blockchain.scripthash.listunspent", "[" + hash_text + "]") ==
        R"({"jsonrpc":"2.0","result":[{"tx_hash":")" + encode_hash(again.hash()) + R"(","tx_pos":0,"height":12,"value":500}],"id":7})");
    REQUIRE(ask("blockchain.scripthash.get_balance", "[" + hash_text + ",\"bad\"]").find(R"("code":-32602)") != std::string::npos);
    auto const raw = ask("blockchain.transaction.get", "[\"" + encode_hash(unrelated.hash()) + "\"]");
    data_chunk unrelated_bytes(unrelated.serialized_size(true));
    byte_writer unrelated_writer(unrelated_bytes);
    REQUIRE(unrelated.to_data(unrelated_writer, true));
    REQUIRE(raw.find("\"" + encode_base16(unrelated_bytes) + "\"") != std::string::npos);
    REQUIRE(ask("blockchain.headers.get_tip", "[]").find(R"("height":30)") != std::string::npos);
    REQUIRE(ask("blockchain.transaction.get_height", "[\"" + encode_hash(unrelated.hash()) + "\"]") ==
        R"({"jsonrpc":"2.0","result":15,"id":7})");
    char prefix_text[8];
    std::snprintf(prefix_text, sizeof(prefix_text), "%04x", unsigned(value));
    REQUIRE(ask("blockchain.rpa.get_history", std::string("[\"") + prefix_text + "\",0]").find(encode_hash(ground.hash())) != std::string::npos);
    // to_height is exclusive, and a prefix shorter than 8 bits is refused, as Fulcrum refuses it.
    REQUIRE(ask("blockchain.rpa.get_history", std::string("[\"") + prefix_text + "\",0,20]").find(encode_hash(ground.hash())) == std::string::npos);
    REQUIRE(ask("blockchain.rpa.get_history", std::string("[\"") + prefix_text + "\",20,21]").find(encode_hash(ground.hash())) != std::string::npos);
    REQUIRE(ask("blockchain.rpa.get_history", std::string("[\"") + prefix_text[0] + "\",0]").find(R"("code":-32602)") != std::string::npos);
    REQUIRE(ask("no.such.method", "[]").find(R"("code":-32601)") != std::string::npos);
    REQUIRE(wait_for(service, electrum.answer("not json")).find(R"("code":-32700)") != std::string::npos);
    auto const batch = wait_for(service, electrum.answer(R"([{"id":1,"method":"server.ping","params":[]},{"id":2,"method":"server.version","params":[]}])"));
    REQUIRE(batch.front() == '[');
    REQUIRE(batch.find(R"("result":null,"id":1)") != std::string::npos);

    // An output by its outpoint: unspent, and spent by a later block.
    REQUIRE(ask("blockchain.utxo.get_info", "[\"" + encode_hash(again.hash()) + "\",0]") ==
        R"({"jsonrpc":"2.0","result":{"confirmed_height":12,"scripthash":)" + hash_text + R"(,"value":500},"id":7})");
    REQUIRE(ask("blockchain.utxo.get_info", "[\"" + encode_hash(paying.hash()) + "\",0]") ==
        R"({"jsonrpc":"2.0","result":null,"id":7})");
    REQUIRE(ask("blockchain.utxo.get_info", "[\"" + encode_hash(paying.hash()) + "\",9]") ==
        R"({"jsonrpc":"2.0","result":null,"id":7})");

    // Header proofs: each branch folds its block hash to the same root.
    auto const branch5 = wait_for(service, service.header_branch(5, 10));
    auto const branch9 = wait_for(service, service.header_branch(9, 10));
    REQUIRE(branch5);
    REQUIRE(branch9);
    REQUIRE(branch5->second == branch9->second);
    REQUIRE(chain::block::merkle_branch_root(served.blocks[5].hash(), 5, branch5->first) == branch5->second);
    REQUIRE(chain::block::merkle_branch_root(served.blocks[9].hash(), 9, branch9->first) == branch9->second);
    REQUIRE( ! wait_for(service, service.header_branch(11, 10)));
    auto const proven = ask("blockchain.block.header", "[5,10]");
    REQUIRE(proven.find(R"("root":")" + encode_hash(branch5->second) + "\"") != std::string::npos);
    REQUIRE(ask("blockchain.block.headers", "[3,4,10]").find(R"("root":")" + encode_hash(branch5->second) + "\"") != std::string::npos);
    REQUIRE(ask("blockchain.block.header", "[5,40]").find(R"("code":-32602)") != std::string::npos);
    REQUIRE(ask("blockchain.transaction.dsproof.list", "[]") == R"({"jsonrpc":"2.0","result":[],"id":7})");
    REQUIRE(ask("blockchain.transaction.dsproof.get", "[\"" + encode_hash(again.hash()) + "\"]") == R"({"jsonrpc":"2.0","result":null,"id":7})");
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
    REQUIRE(status_reaches(service, [](wallet::status const& now) {
        return now.error.find("does not fit block 10") != std::string::npos;
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

TEST_CASE("wallet from the node's own chain: direct parse and filters built on the fly agree, timed", "[wallet][wallet_chain]") {
    // 400 blocks; from 101 on, each spends the coinbase of the block 100 below.
    constexpr uint32_t length = 400;
    auto const flags = blockchain::settings(domain::config::network::regtest).enabled_flags();
    auto const base_time = uint32_t(zulu_time()) - (length + 30) * test::block_spacing;
    std::vector<chain::block> blocks;
    auto previous = chain::block::genesis_regtest().hash();
    for (uint32_t height = 1; height <= length; ++height) {
        std::vector<chain::transaction> txs;
        uint64_t fees = 0;
        if (height > 100) {
            auto const& coinbase = blocks[height - 101].transactions().front();
            txs.push_back(test::spend_p2pkh(coinbase, 0, coinbase.outputs()[0].value() - 1000, flags));
            fees = 1000;
        }
        blocks.push_back(test::mine_block(previous, height, base_time + height * test::block_spacing, 5, std::move(txs), fees));
        previous = blocks.back().hash();
    }
    test::chain_fixture fixture("wallet_chain_timing");
    REQUIRE(fixture.created());
    REQUIRE(fixture.start());
    REQUIRE(fixture.organizer().add_headers(test::headers_of(blocks)).headers_added == blocks.size());
    test::persist_headers(fixture, blocks, 1);
    test::connect_bodies(fixture, blocks, 1);
    REQUIRE(fixture.chain().chain_view()->connected_tip_height == length);

    auto const dense = wallet::target::of_script(test::miner_script().bytes());
    auto const sparse = wallet::target::of_script(other_script);
    auto const timed = [&](wallet::chain_scan scan, wallet::target const& target) {
        wallet::chain_settings settings;
        settings.scan = scan;
        auto own = wallet::service::from_chain(fixture.chain(), domain::config::network::regtest, settings);
        auto const started = std::chrono::steady_clock::now();
        auto history = wait_for(*own, own->history(target));
        auto const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        REQUIRE(history);
        return std::pair{std::move(*history), seconds};
    };
    auto const [direct_dense, direct_dense_seconds] = timed(wallet::chain_scan::direct, dense);
    auto const [filters_dense, filters_dense_seconds] = timed(wallet::chain_scan::filters, dense);
    auto const [direct_sparse, direct_sparse_seconds] = timed(wallet::chain_scan::direct, sparse);
    auto const [filters_sparse, filters_sparse_seconds] = timed(wallet::chain_scan::filters, sparse);
    REQUIRE(direct_dense.confirmed.size() == length + (length - 100));
    REQUIRE(filters_dense.confirmed.size() == direct_dense.confirmed.size());
    for (size_t at = 0; at < direct_dense.confirmed.size(); ++at) {
        REQUIRE(filters_dense.confirmed[at].txid == direct_dense.confirmed[at].txid);
        REQUIRE(filters_dense.confirmed[at].height == direct_dense.confirmed[at].height);
    }
    REQUIRE(direct_sparse.confirmed.empty());
    REQUIRE(filters_sparse.confirmed.empty());
    std::printf("wallet birthday scan of %u blocks: a script in every block: direct %.3f s, filters %.3f s; "
        "a script in none: direct %.3f s, filters %.3f s\n", length, direct_dense_seconds, filters_dense_seconds,
        direct_sparse_seconds, filters_sparse_seconds);
    std::fflush(stdout);
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
