// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test_helpers.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/use_future.hpp>

#include <boost/unordered/unordered_flat_set.hpp>

#include <kth/domain/message/get_data.hpp>
#include <kth/domain/message/inventory.hpp>
#include <kth/domain/message/not_found.hpp>
#include <kth/domain/message/ping.hpp>
#include <kth/domain/message/transaction.hpp>
#include <kth/domain/message/version.hpp>
#include <kth/node/detail/block_announcements.hpp>
#include <kth/node/detail/tx_relay.hpp>
#include <kth/node/p2p_node.hpp>
#include <kth/network/settings.hpp>

#include "header_peer_harness.hpp"
#include "tx_relay_harness.hpp"

using namespace kth;
using namespace kth::node;
using namespace kth::node::detail;
using namespace kth::test;
using namespace std::chrono_literals;

namespace {

using tracker = tx_request_tracker;

hash_digest txid_of(uint32_t tag) {
    hash_digest out = null_hash;
    std::memcpy(out.data(), &tag, sizeof(tag));
    out[31] = 0x7e;
    return out;
}

tracker::time_point const t0 = tracker::time_point{} + 1h;

// Whom each request goes to, as {peer, txids}, sorted by peer.
std::vector<tracker::request> due(tracker& requests, tracker::time_point now) {
    auto out = requests.take_requests(now);
    std::sort(out.begin(), out.end(), [](auto const& a, auto const& b) { return a.peer < b.peer; });
    return out;
}

bool asked(std::vector<tracker::request> const& requests, uint64_t peer, hash_digest const& txid) {
    for (auto const& request : requests) {
        if (request.peer == peer
            && std::find(request.txids.begin(), request.txids.end(), txid) != request.txids.end()) {
            return true;
        }
    }
    return false;
}

} // namespace

// -----------------------------------------------------------------------------
// The tracker, as state
// -----------------------------------------------------------------------------

TEST_CASE("tx request tracker: one announcer at a time is asked", "[tx_relay][tracker]") {
    tracker requests;
    auto const tx = txid_of(1);
    REQUIRE(requests.announced(1, true, tx, t0));
    REQUIRE(requests.announced(2, true, tx, t0));

    auto const first = due(requests, t0);
    REQUIRE(first.size() == 1u);
    CHECK(asked(first, 1, tx));
    CHECK(requests.in_flight(1) == 1u);

    // While that request is out, nobody else is asked.
    CHECK(due(requests, t0 + 59s).empty());
    CHECK(requests.in_flight(2) == 0u);
}

TEST_CASE("tx request tracker: an unanswered request passes to the next announcer at its deadline",
          "[tx_relay][tracker]") {
    tracker requests;
    auto const tx = txid_of(2);
    REQUIRE(requests.announced(1, true, tx, t0));
    REQUIRE(requests.announced(2, true, tx, t0));
    REQUIRE(asked(due(requests, t0), 1, tx));

    REQUIRE(requests.next_wakeup() == t0 + tracker::request_interval);
    CHECK(due(requests, t0 + tracker::request_interval - 1ms).empty());
    auto const next = due(requests, t0 + tracker::request_interval);
    CHECK(asked(next, 2, tx));
    CHECK(requests.in_flight(1) == 0u);
    CHECK(requests.in_flight(2) == 1u);
}

TEST_CASE("tx request tracker: a notfound passes it on at once", "[tx_relay][tracker]") {
    tracker requests;
    auto const tx = txid_of(3);
    REQUIRE(requests.announced(1, true, tx, t0));
    REQUIRE(requests.announced(2, true, tx, t0));
    REQUIRE(asked(due(requests, t0), 1, tx));

    requests.not_found(1, tx);
    CHECK(requests.in_flight(1) == 0u);
    CHECK(asked(due(requests, t0 + 1ms), 2, tx));
}

TEST_CASE("tx request tracker: a peer that leaves passes on what it was asked",
          "[tx_relay][tracker]") {
    tracker requests;
    auto const tx = txid_of(4);
    REQUIRE(requests.announced(1, true, tx, t0));
    REQUIRE(requests.announced(2, true, tx, t0));
    REQUIRE(asked(due(requests, t0), 1, tx));

    requests.disconnected(1);
    CHECK(requests.announcements(1) == 0u);
    CHECK(asked(due(requests, t0 + 1ms), 2, tx));
}

TEST_CASE("tx request tracker: when every announcer failed, the transaction is forgotten",
          "[tx_relay][tracker]") {
    tracker requests;
    auto const tx = txid_of(5);
    REQUIRE(requests.announced(1, true, tx, t0));
    REQUIRE(asked(due(requests, t0), 1, tx));
    requests.not_found(1, tx);

    CHECK_FALSE(requests.tracking(tx));
    CHECK(requests.announcements(1) == 0u);
    CHECK_FALSE(requests.next_wakeup().has_value());

    // A later announcement starts over.
    REQUIRE(requests.announced(1, true, tx, t0 + 1s));
    CHECK(asked(due(requests, t0 + 1s), 1, tx));
}

TEST_CASE("tx request tracker: outbound peers are asked first, the others after a delay",
          "[tx_relay][tracker]") {
    tracker requests;
    auto const tx = txid_of(6);
    REQUIRE(requests.announced(1, false, tx, t0));
    CHECK(due(requests, t0).empty());
    REQUIRE(requests.next_wakeup() == t0 + tracker::nonpreferred_delay);

    // An outbound announcement made later is still due first.
    REQUIRE(requests.announced(2, true, tx, t0 + 1s));
    CHECK(asked(due(requests, t0 + 1s), 2, tx));

    // And on its own, an inbound announcer is asked once its delay passed.
    auto const other = txid_of(7);
    REQUIRE(requests.announced(3, false, other, t0));
    CHECK_FALSE(asked(due(requests, t0 + 1s), 3, other));
    CHECK(asked(due(requests, t0 + tracker::nonpreferred_delay), 3, other));
}

TEST_CASE("tx request tracker: an overloaded peer's announcements wait longer",
          "[tx_relay][tracker]") {
    tracker requests;
    for (uint32_t i = 0; i < tracker::max_peer_in_flight; ++i) {
        REQUIRE(requests.announced(1, true, txid_of(100 + i), t0));
    }
    REQUIRE(due(requests, t0).front().txids.size() == tracker::max_peer_in_flight);
    REQUIRE(requests.in_flight(1) == tracker::max_peer_in_flight);

    auto const tx = txid_of(99);
    REQUIRE(requests.announced(1, true, tx, t0));
    CHECK_FALSE(asked(due(requests, t0 + 1s), 1, tx));
    CHECK(asked(due(requests, t0 + tracker::overloaded_delay), 1, tx));
}

TEST_CASE("tx request tracker: a peer's announcements are bounded, and a repeat is not one more",
          "[tx_relay][tracker]") {
    tracker requests;
    auto const tx = txid_of(8);
    REQUIRE(requests.announced(1, true, tx, t0));
    CHECK_FALSE(requests.announced(1, true, tx, t0));
    CHECK(requests.announcements(1) == 1u);

    for (uint32_t i = 1; i < tracker::max_peer_announcements; ++i) {
        REQUIRE(requests.announced(1, true, txid_of(1'000'000 + i), t0));
    }
    CHECK(requests.announcements(1) == tracker::max_peer_announcements);
    CHECK_FALSE(requests.announced(1, true, txid_of(9), t0));
    // Another peer is not affected.
    CHECK(requests.announced(2, true, txid_of(9), t0));
}

TEST_CASE("tx request tracker: a transaction being validated is asked of nobody until forgotten",
          "[tx_relay][tracker]") {
    tracker requests;
    auto const tx = txid_of(10);
    REQUIRE(requests.announced(1, true, tx, t0));
    REQUIRE(requests.announced(2, true, tx, t0));
    REQUIRE(asked(due(requests, t0), 1, tx));

    requests.received(1, tx);
    requests.hold(tx);
    CHECK(requests.held(tx));
    CHECK(requests.in_flight(1) == 0u);
    CHECK(due(requests, t0 + 10min).empty());

    requests.forget(tx);
    CHECK_FALSE(requests.tracking(tx));
    CHECK(requests.announcements(2) == 0u);
    CHECK(due(requests, t0 + 20min).empty());
}

TEST_CASE("recent rejects: kept until the tip moves, and bounded", "[tx_relay][rejects]") {
    recent_rejects rejects;
    rejects.reset_for(txid_of(0xaa));
    rejects.insert(txid_of(1));
    CHECK(rejects.contains(txid_of(1)));

    rejects.reset_for(txid_of(0xaa));
    CHECK(rejects.contains(txid_of(1)));

    rejects.reset_for(txid_of(0xbb));
    CHECK_FALSE(rejects.contains(txid_of(1)));

    for (uint32_t i = 0; i <= recent_rejects::capacity; ++i) {
        rejects.insert(txid_of(10 + i));
    }
    CHECK(rejects.size() == recent_rejects::capacity);
    CHECK_FALSE(rejects.contains(txid_of(10)));
    CHECK(rejects.contains(txid_of(11)));
}

// -----------------------------------------------------------------------------
// The owner, running
// -----------------------------------------------------------------------------

namespace {

domain::message::transaction::const_ptr make_tx(uint32_t locktime) {
    return std::make_shared<domain::message::transaction const>(
        domain::message::transaction(1, locktime, domain::chain::input::list{}, domain::chain::output::list{}));
}

// The owner and the admission task over a chain that is only what the relay
// reads of it.
struct relay_rig {
    ::asio::io_context ioc;
    network::settings const config = relay_settings();
    tx_relay_channels channels;
    tx_admission_channel admission{tx_admission_capacity};

    bool synchronized = true;
    boost::unordered_flat_set<hash_digest> mempool;
    hash_digest tip = txid_of(0xaa);
    code verdict = error::success;
    std::vector<domain::message::transaction::const_ptr> organized;

    std::future<void> owner;
    std::future<void> admitter;

    tx_relay_view view() {
        return {
            .synchronized = [this] { return synchronized; },
            .in_mempool = [this](hash_digest const& txid) { return mempool.contains(txid); },
            .tip = [this] { return tip; },
        };
    }

    void start(bool admitting = true) {
        owner = ::asio::co_spawn(ioc, tx_relay_task(channels, view(), admission,
            [this](::asio::awaitable<void> task) {
                ::asio::co_spawn(ioc, std::move(task), ::asio::detached);
            }), ::asio::use_future);
        if (admitting) {
            admitter = ::asio::co_spawn(ioc, tx_admission_task(admission,
                [this](domain::message::transaction::const_ptr tx) -> ::asio::awaitable<code> {
                    organized.push_back(tx);
                    if ( ! verdict) {
                        mempool.insert(tx->hash());
                    }
                    co_return verdict;
                }, channels), ::asio::use_future);
        }
        drain(ioc);
    }

    void submit(tx_relay_command command) {
        REQUIRE(channels.submit(std::move(command)));
        drain(ioc);
    }

    bool ended(std::future<void> const& task) const {
        return task.wait_for(0s) == std::future_status::ready;
    }

    ~relay_rig() {
        channels.close();
        drain(ioc);
    }
};

} // namespace

TEST_CASE("tx relay: a peer that never delivers does not keep another from being asked",
          "[tx_relay][owner]") {
    // The first announcer is gone before its getdata leaves. The transaction is
    // not left marked as asked for: a live peer's announcement asks it.
    relay_rig rig;
    auto gone = make_peer(rig.ioc, rig.config, 1);
    make_preferred(gone);
    gone->stop();
    auto live = connect(rig.ioc, rig.config, 2);
    rig.start();

    auto const tx = make_tx(1);
    rig.submit(tx_announced{gone, {tx->hash()}});
    rig.submit(tx_announced{live.session, {tx->hash()}});
    CHECK(contains(read_requests(rig.ioc, live, 2s), tx->hash()));
}

TEST_CASE("tx relay: a getdata that cannot be sent passes the transaction on at once",
          "[tx_relay][owner]") {
    // Both peers are known to the owner already, so no newcomer prompts a look
    // at which sessions stopped: only the failed send says so.
    relay_rig rig;
    auto first = connect(rig.ioc, rig.config, 1);
    auto second = connect(rig.ioc, rig.config, 2);
    rig.start();
    rig.submit(tx_announced{first.session, {make_tx(20)->hash()}});
    rig.submit(tx_announced{second.session, {make_tx(21)->hash()}});

    first.session->stop();
    auto const tx = make_tx(22);
    rig.submit(tx_announced{first.session, {tx->hash()}});
    rig.submit(tx_announced{second.session, {tx->hash()}});
    CHECK(contains(read_requests(rig.ioc, second, 2s, 2), tx->hash()));
}

TEST_CASE("tx relay: what a disconnected peer was asked passes to the next announcer",
          "[tx_relay][owner]") {
    relay_rig rig;
    auto first = connect(rig.ioc, rig.config, 1);
    auto second = connect(rig.ioc, rig.config, 2);
    rig.start();

    auto const tx = make_tx(2);
    rig.submit(tx_announced{first.session, {tx->hash()}});
    rig.submit(tx_announced{second.session, {tx->hash()}});
    REQUIRE(contains(read_requests(rig.ioc, first, 2s), tx->hash()));
    CHECK(read_requests(rig.ioc, second, 100ms).empty());

    rig.submit(tx_peer_gone{1});
    CHECK(contains(read_requests(rig.ioc, second, 2s), tx->hash()));
}

TEST_CASE("tx relay: a notfound passes the transaction to the next announcer",
          "[tx_relay][owner]") {
    relay_rig rig;
    auto first = connect(rig.ioc, rig.config, 1);
    auto second = connect(rig.ioc, rig.config, 2);
    rig.start();

    auto const tx = make_tx(3);
    rig.submit(tx_announced{first.session, {tx->hash()}});
    rig.submit(tx_announced{second.session, {tx->hash()}});
    REQUIRE(contains(read_requests(rig.ioc, first, 2s), tx->hash()));

    rig.submit(tx_not_found{1, {tx->hash()}});
    CHECK(contains(read_requests(rig.ioc, second, 2s), tx->hash()));
}

TEST_CASE("tx relay: a peer whose queue is full does not hold up the others",
          "[tx_relay][owner]") {
    // The owner never waits on a peer: the getdata for the blocked one waits in
    // a task of its own, and the other is asked meanwhile.
    relay_rig rig;
    auto blocked = connect(rig.ioc, rig.config, 1);
    auto other = connect(rig.ioc, rig.config, 2);
    auto filled = ::asio::co_spawn(rig.ioc, [session = blocked.session]() -> ::asio::awaitable<void> {
        for (size_t i = 0; i < network::peer_session::outbound_capacity; ++i) {
            REQUIRE(co_await session->send(domain::message::ping{uint64_t(i)}) == error::success);
        }
    }, ::asio::use_future);
    drain(rig.ioc);
    REQUIRE(filled.wait_for(0s) == std::future_status::ready);
    rig.start();

    auto const stuck = make_tx(4);
    auto const flowing = make_tx(5);
    rig.submit(tx_announced{blocked.session, {stuck->hash()}});
    rig.submit(tx_announced{other.session, {flowing->hash()}});
    CHECK(contains(read_requests(rig.ioc, other, 2s), flowing->hash()));
}

TEST_CASE("tx relay: a delivered transaction is validated once and reaches the pool",
          "[tx_relay][owner]") {
    relay_rig rig;
    auto peer = connect(rig.ioc, rig.config, 1);
    auto copy = connect(rig.ioc, rig.config, 2);
    rig.start();

    auto const tx = make_tx(6);
    rig.submit(tx_announced{peer.session, {tx->hash()}});
    REQUIRE(contains(read_requests(rig.ioc, peer, 2s), tx->hash()));
    rig.submit(tx_received{1, tx});
    rig.submit(tx_received{2, tx});             // a second copy, unasked
    REQUIRE(rig.organized.size() == 1u);
    CHECK(rig.mempool.contains(tx->hash()));

    // In the pool now: announcing it asks for nothing.
    rig.submit(tx_announced{copy.session, {tx->hash()}});
    CHECK(read_requests(rig.ioc, copy, 200ms).empty());
}

TEST_CASE("tx relay: a rejected transaction is not asked for again until the tip moves",
          "[tx_relay][owner]") {
    relay_rig rig;
    rig.verdict = error::insufficient_fee;
    auto peer = connect(rig.ioc, rig.config, 1);
    rig.start();

    auto const tx = make_tx(7);
    rig.submit(tx_received{1, tx});
    REQUIRE(rig.organized.size() == 1u);

    rig.submit(tx_announced{peer.session, {tx->hash()}});
    CHECK(read_requests(rig.ioc, peer, 200ms).empty());

    rig.tip = txid_of(0xbb);
    rig.submit(tx_announced{peer.session, {tx->hash()}});
    CHECK(contains(read_requests(rig.ioc, peer, 2s), tx->hash()));
}

TEST_CASE("tx relay: a transaction missing its inputs is not remembered as rejected",
          "[tx_relay][owner]") {
    relay_rig rig;
    rig.verdict = error::missing_previous_output;
    auto peer = connect(rig.ioc, rig.config, 1);
    rig.start();

    auto const tx = make_tx(8);
    rig.submit(tx_received{1, tx});
    REQUIRE(rig.organized.size() == 1u);

    // Its parent may arrive next: asked for again when announced.
    rig.submit(tx_announced{peer.session, {tx->hash()}});
    CHECK(contains(read_requests(rig.ioc, peer, 2s), tx->hash()));
}

TEST_CASE("tx relay: until the node is synchronized, nothing is asked for or validated",
          "[tx_relay][owner]") {
    relay_rig rig;
    rig.synchronized = false;
    auto peer = connect(rig.ioc, rig.config, 1);
    rig.start();

    auto const tx = make_tx(9);
    rig.submit(tx_announced{peer.session, {tx->hash()}});
    rig.submit(tx_received{1, tx});
    CHECK(read_requests(rig.ioc, peer, 200ms).empty());
    CHECK(rig.organized.empty());
}

TEST_CASE("tx relay: past what validation holds, a transaction is not taken and is asked again",
          "[tx_relay][owner]") {
    // No admission task: the channel fills and stays full.
    relay_rig rig;
    auto peer = connect(rig.ioc, rig.config, 1);
    auto other = connect(rig.ioc, rig.config, 2);
    rig.start(false);

    for (uint32_t i = 0; i < tx_admission_capacity; ++i) {
        rig.submit(tx_received{1, make_tx(100 + i)});
    }
    auto const extra = make_tx(99);
    rig.submit(tx_received{1, extra});

    // Not held, so its announcement is asked for.
    rig.submit(tx_announced{other.session, {extra->hash()}});
    CHECK(contains(read_requests(rig.ioc, other, 2s), extra->hash()));
    // What was taken is held: announcing it asks for nothing.
    rig.submit(tx_announced{peer.session, {make_tx(100)->hash()}});
    CHECK(read_requests(rig.ioc, peer, 200ms).empty());
}

TEST_CASE("tx relay: closing the channel ends both tasks, also before they ever ran",
          "[tx_relay][owner][shutdown]") {
    SECTION("while running") {
        relay_rig rig;
        rig.start();
        REQUIRE_FALSE(rig.ended(rig.owner));
        REQUIRE_FALSE(rig.ended(rig.admitter));
        rig.channels.close();
        drain(rig.ioc);
        CHECK(rig.ended(rig.owner));
        CHECK(rig.ended(rig.admitter));
    }
    SECTION("before they start") {
        relay_rig rig;
        rig.channels.close();
        rig.start();
        CHECK(rig.ended(rig.owner));
        CHECK(rig.ended(rig.admitter));
    }
}

// -----------------------------------------------------------------------------
// The read loop's side
// -----------------------------------------------------------------------------

namespace {

network::raw_message wire(std::string const& command, data_chunk payload) {
    network::raw_message raw;
    raw.heading = domain::message::heading(0xdab5bffa, command, uint32_t(payload.size()), 0);
    raw.payload = std::move(payload);
    return raw;
}

// Serialized at the version the read loop parses it with.
template <typename Message>
data_chunk payload_of(Message const& message, network::peer_session::ptr const& peer) {
    auto const version = peer->negotiated_version();
    data_chunk out(message.serialized_size(version));
    byte_writer writer(out);
    REQUIRE(message.to_data(writer, version).has_value());
    return out;
}

std::vector<tx_relay_command> submitted(p2p_node& node) {
    std::vector<tx_relay_command> out;
    std::error_code ec;
    while (auto command = node.tx_relay().commands.try_receive(ec)) {
        out.push_back(std::move(*command));
    }
    return out;
}

} // namespace

TEST_CASE("relay routing: the transaction entries of an inv go to the relay with its blocks to theirs",
          "[tx_relay][routing]") {
    using domain::message::inventory_vector;
    ::asio::io_context ioc;
    auto const settings = relay_settings();
    p2p_node node(settings);
    auto peer = make_peer(ioc, settings, 9501);

    auto const block = txid_of(0xb1);
    auto const tx = txid_of(0x71);
    auto inv = domain::message::inventory::create(inventory_vector::list{
        {inventory_vector::type_id::block, block},
        {inventory_vector::type_id::transaction, tx}});
    REQUIRE(inv.has_value());
    CHECK_FALSE(node.observe_block_message(peer, wire(domain::message::inventory::command, payload_of(*inv, peer))));

    auto const got = submitted(node);
    REQUIRE(got.size() == 1u);
    auto const* announced = std::get_if<tx_announced>(&got[0]);
    REQUIRE(announced != nullptr);
    CHECK(announced->peer == peer);
    CHECK(announced->txids == hash_list{tx});
    CHECK(node.take_announced_blocks().hashes == hash_list{block});
}

TEST_CASE("relay routing: a tx is consumed and handed over, a notfound handed over and carried on",
          "[tx_relay][routing]") {
    ::asio::io_context ioc;
    auto const settings = relay_settings();
    p2p_node node(settings);
    auto peer = make_peer(ioc, settings, 9502);

    auto const tx = make_tx(11);
    CHECK(node.observe_transaction_message(peer, wire(domain::message::transaction::command, payload_of(*tx, peer))));
    auto missing = domain::message::not_found::create(hash_list{txid_of(0x72)},
        domain::message::inventory_vector::type_id::transaction);
    REQUIRE(missing.has_value());
    CHECK_FALSE(node.observe_transaction_message(peer, wire(domain::message::not_found::command, payload_of(*missing, peer))));
    // Neither is anything else's business.
    CHECK_FALSE(node.observe_transaction_message(peer, wire(domain::message::ping::command, data_chunk(8, 0))));

    auto const got = submitted(node);
    REQUIRE(got.size() == 2u);
    auto const* received = std::get_if<tx_received>(&got[0]);
    REQUIRE(received != nullptr);
    CHECK(received->peer == 9502u);
    CHECK(received->tx->hash() == tx->hash());
    auto const* not_found = std::get_if<tx_not_found>(&got[1]);
    REQUIRE(not_found != nullptr);
    CHECK(not_found->txids == hash_list{txid_of(0x72)});
}

TEST_CASE("relay routing: a node that does not relay hands nothing over",
          "[tx_relay][routing]") {
    using domain::message::inventory_vector;
    ::asio::io_context ioc;
    auto settings = relay_settings();
    settings.relay_transactions = false;
    p2p_node node(settings);
    auto peer = make_peer(ioc, settings, 9503);

    auto inv = domain::message::inventory::create(inventory_vector::list{
        {inventory_vector::type_id::transaction, txid_of(0x73)}});
    REQUIRE(inv.has_value());
    CHECK_FALSE(node.observe_block_message(peer, wire(domain::message::inventory::command, payload_of(*inv, peer))));
    CHECK(node.observe_transaction_message(peer, wire(domain::message::transaction::command, payload_of(*make_tx(12), peer))));
    CHECK(submitted(node).empty());
}

TEST_CASE("relay routing: a relay that cannot take more refuses at once",
          "[tx_relay][routing][saturation]") {
    // Nothing drains the channel here. The read loop must still never wait:
    // one past capacity is refused on the spot, and the message is consumed.
    ::asio::io_context ioc;
    auto const settings = relay_settings();
    p2p_node node(settings);
    auto peer = make_peer(ioc, settings, 9504);

    auto const capacity = tx_relay_channels::command_capacity;
    for (size_t i = 0; i < capacity; ++i) {
        REQUIRE(node.observe_transaction_message(peer,
            wire(domain::message::transaction::command, payload_of(*make_tx(1000 + uint32_t(i)), peer))));
    }
    CHECK(node.observe_transaction_message(peer,
        wire(domain::message::transaction::command, payload_of(*make_tx(5000), peer))));
    CHECK(submitted(node).size() == capacity);
}
