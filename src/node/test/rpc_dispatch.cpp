// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test_helpers.hpp>

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <string>
#include <catch2/catch_approx.hpp>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <kth/node/mining/job_store.hpp>
#include <kth/node/rpc/dispatch.hpp>
#include <kth/node/rpc/query.hpp>
#include "../../blockchain/test/reorg_chain_fixture.hpp"
#include "sync_harness.hpp"

using namespace kth;

TEST_CASE("getblockheader dispatch honors verbose and rejects invalid types", "[rpc dispatch]") {
    test::chain_fixture fixture("rpc_header_verbose");
    REQUIRE(fixture.created());
    REQUIRE(fixture.start());
    node::mining::job_store jobs(1, 60);
    node::rpc::method_context context{fixture.chain(), jobs};
    node::rpc::dispatcher dispatcher;
    node::rpc::register_builtin_methods(dispatcher);

    auto const genesis = domain::chain::block::genesis_regtest();
    auto const hash = encode_hash(genesis.hash());
    auto request = [&](std::string const& suffix) {
        auto const body = std::string(R"({"method":"getblockheader","params":[")")
            + hash + '"' + suffix + R"(],"id":7})";
        ::asio::io_context io;
        auto result = ::asio::co_spawn(io, dispatcher.handle(context, body), ::asio::use_future);
        io.run();
        return result.get();
    };

    auto const raw = request(",false");
    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(raw);
    simdjson::ondemand::document document;
    REQUIRE(parser.iterate(padded).get(document) == simdjson::SUCCESS);
    std::string_view hex;
    REQUIRE(document["result"].get_string().get(hex) == simdjson::SUCCESS);
    REQUIRE(hex.size() == 160);
    auto const bytes = decode_base16(hex);
    REQUIRE(bytes);
    byte_reader reader(*bytes);
    auto const decoded = domain::chain::header::from_data(reader);
    REQUIRE(decoded);
    REQUIRE(*decoded == genesis.header());
    REQUIRE(raw == node::rpc::build_success("7", '"' + std::string(hex) + '"'));
    auto const verbose = request(",true");
    REQUIRE(verbose.find(R"("result":{)") != std::string::npos);
    REQUIRE(verbose.find(R"("hash":")" + hash + '"') != std::string::npos);
    REQUIRE(request("") == verbose);
    REQUIRE(request(",null") == verbose);
    for (auto const* invalid : {",0", ",1", ",\"false\"", ",{}", ",[]"}) {
        CAPTURE(invalid);
        REQUIRE(request(invalid) == node::rpc::build_error("7", -32602, "verbose must be a boolean"));
    }
}

TEST_CASE("validateaddress dispatch checks addresses against the node's network", "[rpc dispatch]") {
    test::chain_fixture fixture("rpc_validate_address");
TEST_CASE("help dispatch lists the registered methods", "[rpc dispatch]") {
    test::chain_fixture fixture("rpc_help");
    REQUIRE(fixture.created());
    REQUIRE(fixture.start());
TEST_CASE("getblockheader dispatch counts confirmations from the connected tip", "[rpc dispatch]") {
    test::chain_fixture fixture("rpc_header_confirmations");
TEST_CASE("getchaintips dispatch reports the connected tip and the branches beyond it", "[rpc dispatch]") {
    test::chain_fixture fixture("rpc_chain_tips");
    REQUIRE(fixture.created());
    REQUIRE(fixture.start());

    // Five blocks with their headers stored, three of them connected.
    auto const base_time = uint32_t(zulu_time()) - 40u * test::block_spacing;
    std::vector<domain::chain::block> trunk;
    auto prev = domain::chain::block::genesis_regtest().hash();
TEST_CASE("getnetworkhashps dispatch computes the rate over the connected chain", "[rpc dispatch]") {
    test::chain_fixture fixture("rpc_network_hash_ps");
    REQUIRE(fixture.created());
    REQUIRE(fixture.start());

    // Five connected blocks, two minutes apart.
    auto const base_time = uint32_t(zulu_time()) - 40u * test::block_spacing;
    auto const genesis = domain::chain::block::genesis_regtest();
    std::vector<domain::chain::block> trunk;
    auto prev = genesis.hash();
TEST_CASE("decoderawtransaction dispatch decodes for the node's network", "[rpc dispatch]") {
    test::chain_fixture fixture("rpc_decode_raw_transaction");
    REQUIRE(fixture.created());
    REQUIRE(fixture.start());
    node::mining::job_store jobs(1, 60);
TEST_CASE("getnetworkinfo dispatch needs the P2P node", "[rpc dispatch]") {
    test::chain_fixture fixture("rpc_network_info");
    REQUIRE(fixture.created());
    REQUIRE(fixture.start());
    node::mining::job_store jobs(1, 60);
    // No P2P node in this context, as when the server runs without one.
    node::rpc::method_context context{fixture.chain(), jobs};
    node::rpc::dispatcher dispatcher;
    node::rpc::register_builtin_methods(dispatcher);

    auto request = [&](std::string const& params) {
        auto const body = R"({"method":"decoderawtransaction","params":)" + params + R"(,"id":6})";
        ::asio::io_context io;
        auto result = ::asio::co_spawn(io, dispatcher.handle(context, body), ::asio::use_future);
        io.run();
        return result.get();
    };

    // The regtest genesis coinbase, as EloPool decodes its own coinbase at startup.
    auto const coinbase = domain::chain::block::genesis_regtest().transactions().front();
    auto const hex = node::rpc::transaction_to_hex(coinbase);
    auto const reply = request(R"([")" + hex + R"("])");
    REQUIRE(reply.find(R"("txid":")" + encode_hash(coinbase.hash()) + '"') != std::string::npos);
    REQUIRE(reply.find(R"("vin":[{"coinbase":")") != std::string::npos);
    REQUIRE(reply.find(R"("error":null)") != std::string::npos);

    REQUIRE(request(R"([")" + hex + R"(00"])") ==
        node::rpc::build_error("6", -32602, "transaction decode failed"));
    REQUIRE(request(R"(["zz"])") == node::rpc::build_error("6", -32602, "hexstring is not valid hex"));
    REQUIRE(request("[]") == node::rpc::build_error("6", -32602, "decoderawtransaction requires [hexstring]"));
}

TEST_CASE("gettxout dispatch reads the UTXO set and the mempool", "[rpc dispatch]") {
    test::chain_fixture fixture("rpc_get_tx_out");
    REQUIRE(fixture.created());
    REQUIRE(fixture.start());

    // Five connected blocks.
    auto const base_time = uint32_t(zulu_time()) - 40u * test::block_spacing;
    std::vector<domain::chain::block> trunk;
    auto prev = domain::chain::block::genesis_regtest().hash();
    for (uint32_t h = 1; h <= 5; ++h) {
        trunk.push_back(test::mine_block(prev, h, base_time + h * test::block_spacing, 0, {}, 0));
        prev = trunk.back().hash();
    }
    auto const added = fixture.organizer().add_headers(test::headers_of(trunk));
    REQUIRE(added.headers_added == trunk.size());
    REQUIRE(fixture.organizer().add_headers(test::headers_of(trunk)).headers_added == trunk.size());
    test::persist_headers(fixture, trunk, 1);
    test::connect_bodies(fixture, {trunk.begin(), trunk.begin() + 3}, 1);
    REQUIRE(fixture.chain().chain_view()->connected_tip_height == 3u);

TEST_CASE("estimatefee dispatch answers with or without a target", "[rpc dispatch]") {
    test::chain_fixture fixture("rpc_estimate_fee");
    REQUIRE(fixture.created());
    REQUIRE(fixture.start());
    test::connect_bodies(fixture, trunk, 1);
    REQUIRE(fixture.chain().chain_view()->connected_tip_height == 5u);
    // A two-block side branch off block 2, headers only.
    std::vector<domain::chain::block> side;
    prev = trunk[1].hash();
    for (uint32_t h = 3; h <= 4; ++h) {
        side.push_back(test::mine_block(prev, h, base_time + h * test::block_spacing + 60, /*salt*/ 1, {}, 0));
        prev = side.back().hash();
    }
    REQUIRE(fixture.organizer().add_headers(test::headers_of(side)).headers_added == side.size());

    node::mining::job_store jobs(1, 60);
    node::rpc::method_context context{fixture.chain(), jobs};
    node::rpc::dispatcher dispatcher;
    node::rpc::register_builtin_methods(dispatcher);

    auto request = [&](std::string const& params) {
        auto const body = R"({"method":"validateaddress","params":)" + params + R"(,"id":3})";
        auto const body = R"({"method":"help","params":)" + params + R"(,"id":5})";
        auto const body = R"({"method":"estimatefee","params":)" + params + R"(,"id":2})";
    auto request = [&](std::string const& params) {
        auto const body = R"({"method":"getnetworkhashps","params":)" + params + R"(,"id":4})";
    auto request = [&](std::string const& params) {
        auto const body = R"({"method":"gettxout","params":)" + params + R"(,"id":7})";
        ::asio::io_context io;
        auto result = ::asio::co_spawn(io, dispatcher.handle(context, body), ::asio::use_future);
        io.run();
        return result.get();
    };

    // The fixture chain is regtest, so bchreg addresses are this network's.
    REQUIRE(request(R"(["bchreg:qpzz8n7jp6847yyx8t33matrgcsdx6c0cv99t2zdvc"])") == node::rpc::build_success("3",
        R"({"isvalid":true,"address":"bchreg:qpzz8n7jp6847yyx8t33matrgcsdx6c0cv99t2zdvc",)"
        R"("scriptPubKey":"76a9144423cfd20e8f5f10863ae31df5634620d36b0fc388ac",)"
        R"("isscript":false,"istokenaware":false})"));
    REQUIRE(request(R"(["bitcoincash:qpzz8n7jp6847yyx8t33matrgcsdx6c0cvmtevrfgz"])") ==
        node::rpc::build_success("3", R"({"isvalid":false})"));
    REQUIRE(request("[]") == node::rpc::build_error("3", -32602, "validateaddress requires [address]"));
namespace {

// The getblockchaininfo response from `chain`, through the dispatcher.
std::string blockchain_info(blockchain::block_chain& chain) {
    node::mining::job_store jobs(1, 60);
    node::rpc::method_context context{chain, jobs};
    node::rpc::dispatcher dispatcher;
    node::rpc::register_builtin_methods(dispatcher);
    ::asio::io_context io;
    auto result = ::asio::co_spawn(io,
        dispatcher.handle(context, R"({"method":"getblockchaininfo","params":[],"id":1})"),
        ::asio::use_future);
    io.run();
    return result.get();
}

} // namespace

TEST_CASE("getblockchaininfo dispatch reports the connected tip from one view", "[rpc dispatch]") {
    test::chain_fixture fixture("rpc_blockchain_info");
    REQUIRE(fixture.created());
    REQUIRE(fixture.start());

    auto const raw = blockchain_info(fixture.chain());
    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(raw);
    simdjson::ondemand::document document;
    REQUIRE(parser.iterate(padded).get(document) == simdjson::SUCCESS);
    simdjson::ondemand::object result;
    REQUIRE(document["result"].get_object().get(result) == simdjson::SUCCESS);

    auto const genesis = domain::chain::block::genesis_regtest();
    std::string_view text;
    std::uint64_t number = 0;
    bool flag = false;
    double progress = 0;

    REQUIRE(result["chain"].get_string().get(text) == simdjson::SUCCESS);
    REQUIRE(text == "regtest");
    REQUIRE(result["blocks"].get_uint64().get(number) == simdjson::SUCCESS);
    REQUIRE(number == 0u);
    REQUIRE(result["headers"].get_uint64().get(number) == simdjson::SUCCESS);
    REQUIRE(number == 0u);
    REQUIRE(result["bestblockhash"].get_string().get(text) == simdjson::SUCCESS);
    REQUIRE(text == encode_hash(genesis.hash()));
    REQUIRE(result["mediantime"].get_uint64().get(number) == simdjson::SUCCESS);
    REQUIRE(number == genesis.header().timestamp());
    REQUIRE(result["verificationprogress"].get_double().get(progress) == simdjson::SUCCESS);
    REQUIRE(progress == 1.0);
    // The regtest genesis is from 2011, far past the default 24-hour limit.
    REQUIRE(result["initialblockdownload"].get_bool().get(flag) == simdjson::SUCCESS);
    REQUIRE(flag);
    REQUIRE(result["chainwork"].get_string().get(text) == simdjson::SUCCESS);
    std::ostringstream work;
    work << std::hex << std::setfill('0') << std::setw(64)
         << domain::chain::header::proof(genesis.header().bits());
    REQUIRE(text == work.str());
    REQUIRE(result["size_on_disk"].get_uint64().get(number) == simdjson::SUCCESS);
    REQUIRE(result["warnings"].get_string().get(text) == simdjson::SUCCESS);
    REQUIRE(text.empty());
}

TEST_CASE("getblockchaininfo dispatch leaves initial download once synchronized", "[rpc dispatch]") {
    // A limit of zero turns the clock half off; the genesis-only chain has
    // connected everything it has headers for, so the node is synchronized.
    test::chain_fixture fixture("rpc_blockchain_info_synced", /*notify_limit_hours*/ 0);
    REQUIRE(fixture.created());
    REQUIRE(fixture.start());
    REQUIRE(fixture.chain().synchronization().synchronized());

    auto const raw = blockchain_info(fixture.chain());
    REQUIRE(raw.find(R"("initialblockdownload":false)") != std::string::npos);
    // help itself is registered last and is in its own list.
    auto const methods = dispatcher.methods();
    REQUIRE(std::ranges::is_sorted(methods));
    REQUIRE(std::ranges::binary_search(methods, std::string("help")));
    REQUIRE(std::ranges::binary_search(methods, std::string("getblockcount")));

    // One method per line, newlines escaped as the JSON string carries them.
    std::string listing;
    for (auto const& method : methods) {
        listing += (listing.empty() ? "" : "\\n") + method;
    }
    REQUIRE(request("[]") == node::rpc::build_success("5", '"' + listing + '"'));
    REQUIRE(request(R"([""])") == request("[]"));

    // Fulcrum sends this on every connection and needs a successful reply.
    REQUIRE(request(R"(["help"])") == node::rpc::build_success("5", R"("help")"));
    REQUIRE(request(R"(["getblockhash"])") == node::rpc::build_success("5", R"("getblockhash")"));
    REQUIRE(request(R"(["nosuchmethod"])") ==
        node::rpc::build_success("5", R"("help: unknown command: nosuchmethod")"));
    auto confirmations = [&](hash_digest const& hash) {
        auto const body = std::string(R"({"method":"getblockheader","params":[")")
            + encode_hash(hash) + R"(",true],"id":9})";
        ::asio::io_context io;
        auto result = ::asio::co_spawn(io, dispatcher.handle(context, body), ::asio::use_future);
        io.run();
        auto const raw = result.get();
    auto rate = [&](std::string const& params) {
        auto const raw = request(params);
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(raw);
        simdjson::ondemand::document document;
        REQUIRE(parser.iterate(padded).get(document) == simdjson::SUCCESS);
        std::int64_t value = 0;
        REQUIRE(document["result"]["confirmations"].get_int64().get(value) == simdjson::SUCCESS);
        return value;
    };

    REQUIRE(confirmations(domain::chain::block::genesis_regtest().hash()) == 4);
    REQUIRE(confirmations(trunk[1].hash()) == 2);
    REQUIRE(confirmations(trunk[2].hash()) == 1);
    // Stored, but its block is not connected yet.
    REQUIRE(confirmations(trunk[4].hash()) == -1);
    auto const expected = node::rpc::build_success("2",
        node::rpc::render_estimate_fee(fixture.chain().chain_settings().byte_fee_satoshis));
    // Fulcrum sends one argument to nodes it does not know to take none.
    REQUIRE(request("[6]") == expected);
    REQUIRE(request("[]") == expected);
        double value = -1;
        REQUIRE(document["result"].get_double().get(value) == simdjson::SUCCESS);
        return value;
    };

    // Every regtest block adds the same work. The window of n blocks ending at
    // height h spans the times of blocks h - n through h.
    auto const work = domain::chain::header::proof(genesis.header().bits()).convert_to<double>();
    auto const span = [&](uint32_t first, uint32_t last) {
        auto const time = [&](uint32_t h) {
            return h == 0 ? genesis.header().timestamp() : trunk[h - 1].header().timestamp();
        };
        return static_cast<double>(time(last) - time(first));
    };

    REQUIRE(rate("[2,5]") == Catch::Approx(2 * work / span(3, 5)));
    REQUIRE(rate("[1,3]") == Catch::Approx(work / span(2, 3)));
    // The tip, by default and by -1; a window longer than the chain stops at genesis.
    REQUIRE(rate("[2]") == rate("[2,5]"));
    REQUIRE(rate("[2,-1]") == rate("[2,5]"));
    REQUIRE(rate("[2,99]") == rate("[2,5]"));
    REQUIRE(rate("[]") == Catch::Approx(5 * work / span(0, 5)));
    REQUIRE(rate("[0]") == rate("[]"));
    REQUIRE(request(R"(["x"])") == node::rpc::build_error("4", -32602,
        "getnetworkhashps takes [nblocks, height] as integers"));
    auto const quoted = [](hash_digest const& hash) { return '"' + encode_hash(hash) + '"'; };

    // The coinbase of block 2, four blocks deep.
    auto const coinbase = trunk[1].transactions().front().hash();
    auto const reply = request("[" + quoted(coinbase) + ",0]");
    CHECK(reply.find(R"("bestblock":)" + quoted(trunk[4].hash())) != std::string::npos);
    CHECK(reply.find(R"("confirmations":4,"value":50.00000000,)") != std::string::npos);
    CHECK(reply.find(R"("coinbase":true})") != std::string::npos);

    // No such output, and no such transaction.
    auto const none = node::rpc::build_success("7", "null");
    CHECK(request("[" + quoted(coinbase) + ",1]") == none);
    CHECK(request("[" + quoted(null_hash) + ",0]") == none);

    // A mempool transaction that spends it: spent with include_mempool, still
    // unspent in the confirmed set without; the pool transaction's own output
    // the other way around.
    domain::chain::input::list ins;
    ins.emplace_back(domain::chain::output_point{coinbase, 0}, domain::chain::script{}, 0xffffffffu);
    domain::chain::output::list outs;
    outs.emplace_back(4999990000u, test::miner_script(), domain::chain::token_data_opt{});
    auto const spend = std::make_shared<domain::message::transaction>(
        domain::chain::transaction{2u, 0u, std::move(ins), std::move(outs)});
    REQUIRE(fixture.chain().mempool_ref().add(blockchain::mempool_entry{
        spend, 10000u, uint32_t(spend->serialized_size(true)), 1u, 0u}));

    CHECK(request("[" + quoted(coinbase) + ",0]") == none);
    CHECK(request("[" + quoted(coinbase) + ",0,true]") == none);
    CHECK(request("[" + quoted(coinbase) + ",0,false]") == reply);
    auto const pooled = request("[" + quoted(spend->hash()) + ",0]");
    CHECK(pooled.find(R"("confirmations":0,"value":49.99990000,)") != std::string::npos);
    CHECK(pooled.find(R"("coinbase":false})") != std::string::npos);
    CHECK(request("[" + quoted(spend->hash()) + ",0,false]") == none);

    CHECK(request("[" + quoted(coinbase) + "]") ==
        node::rpc::build_error("7", -32602, "gettxout requires [txid, n]"));
    CHECK(request("[" + quoted(coinbase) + R"(,0,"yes"])") ==
        node::rpc::build_error("7", -32602, "include_mempool must be a boolean"));
    ::asio::io_context io;
    auto result = ::asio::co_spawn(io,
        dispatcher.handle(context, R"({"method":"getnetworkinfo","params":[],"id":8})"),
        ::asio::use_future);
    io.run();
    REQUIRE(result.get() == node::rpc::build_error("8", -32603, "network unavailable"));
    ::asio::io_context io;
    auto result = ::asio::co_spawn(io,
        dispatcher.handle(context, R"({"method":"getchaintips","params":[],"id":3})"),
        ::asio::use_future);
    io.run();

    REQUIRE(result.get() == node::rpc::build_success("3", node::rpc::render_chain_tips({
        {5u, trunk[4].hash(), 2u, "headers-only"},
        {4u, side[1].hash(), 2u, "headers-only"},
        {3u, trunk[2].hash(), 0u, "active"}})));
}
