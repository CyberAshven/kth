// Before/after probe for k-nuth/kth#772. TEST ONLY, never proposed for
// merging: copied into two scratch trees, the parent of #830 (cc8860ed) and
// master after #830, so the same deadline race runs against both. It is a
// reduced form of the stress case in #815's message_waits.cpp, written
// against the API both trees share: wait_for_any_message(peer, seconds) and
// peer.messages(). The only per-tree difference is how a test sends into and
// drains the inbound channel (concurrent_channel before #830, select_channel
// after it).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <expected>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <test_helpers.hpp>

#include <asio/co_spawn.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/thread_pool.hpp>

#include <kth/network/peer_session.hpp>
#include <kth/network/protocols_coro.hpp>

using namespace kth;
using namespace kth::network;
using namespace std::chrono_literals;

namespace {

using clock_type = std::chrono::steady_clock;
using message_result = std::expected<raw_message, code>;
using inbound = std::remove_reference_t<decltype(std::declval<peer_session&>().messages())>;
constexpr bool fixed_tree = std::is_same_v<inbound, kth::select_channel<raw_message>>;

raw_message make_ping(uint64_t nonce) {
    raw_message result;
    result.heading.set_command(domain::message::ping::command);
    result.payload = kth::to_data_chunk(domain::message::ping{nonce}, 70015);
    return result;
}

template <typename Channel>
bool send_one(Channel& channel, raw_message message) {
    if constexpr (fixed_tree) {
        return channel.try_send(std::move(message));
    } else {
        return channel.try_send(std::error_code{}, std::move(message));
    }
}

template <typename Channel>
size_t drain_matching(Channel& channel, data_chunk const& expected) {
    size_t found = 0;
    if constexpr (fixed_tree) {
        std::error_code ec;
        while (auto message = channel.try_receive(ec)) {
            found += message->payload == expected ? 1 : 0;
        }
    } else {
        while (channel.try_receive([&](std::error_code ec, raw_message message) {
            if ( ! ec && message.payload == expected) {
                ++found;
            }
        })) {
        }
    }
    return found;
}

// Arms the send for `offset` around the wait's own deadline (now + 1 s, taken
// inside wait_for_any_message a few microseconds later on this same thread),
// then waits. Both timers are on the unstranded pool.
::asio::awaitable<message_result> wait_with_send_near_deadline(peer_session::ptr peer,
    std::chrono::microseconds offset, uint64_t nonce, std::shared_ptr<std::promise<bool>> sent)
{
    auto executor = co_await ::asio::this_coro::executor;
    auto timer = std::make_shared<::asio::steady_timer>(executor, clock_type::now() + 1s + offset);
    timer->async_wait([peer, timer, sent, nonce](std::error_code ec) {
        sent->set_value( ! ec && send_one(peer->messages(), make_ping(nonce)));
    });
    co_return co_await wait_for_any_message(*peer, 1s);
}

} // namespace

TEST_CASE("issue 772 probe: a message at the deadline is never lost on eight threads", "[stress772]") {
    std::printf("stress772: tree=%s\n", fixed_tree ? "fix" : "master");
    ::asio::thread_pool pool{8};
    network::settings settings;
    settings.protocol_maximum = 70015;
    std::vector<peer_session::ptr> peers;

    size_t lost = 0;
    size_t duplicated = 0;
    size_t consumed = 0;
    size_t timed_out = 0;
    size_t total = 0;

    constexpr size_t batches = 32;
    constexpr size_t width = 64;
    for (size_t batch = 0; batch < batches; ++batch) {
        struct pending {
            peer_session::ptr peer;
            std::future<message_result> received;
            std::future<bool> sent;
            uint64_t nonce;
        };
        std::vector<pending> waits;
        waits.reserve(width);

        for (size_t i = 0; i < width; ++i) {
            auto peer = std::make_shared<peer_session>(::asio::ip::tcp::socket(pool.get_executor()), settings);
            peers.push_back(peer);

            auto const nonce = uint64_t(batch * width + i + 1);
            // From 60 us before the deadline to 97 us after it, 2.5 us apart.
            auto const offset = std::chrono::microseconds(int(i) * 5 / 2 - 60);
            auto sent = std::make_shared<std::promise<bool>>();
            auto sent_future = sent->get_future();
            auto received = std::make_shared<std::promise<message_result>>();
            auto received_future = received->get_future();
            ::asio::co_spawn(pool, wait_with_send_near_deadline(peer, offset, nonce, sent),
                [received](std::exception_ptr exception, message_result value) {
                    if (exception) {
                        received->set_exception(exception);
                    } else {
                        received->set_value(std::move(value));
                    }
                });
            waits.push_back({std::move(peer), std::move(received_future), std::move(sent_future), nonce});
        }

        auto const bound = clock_type::now() + 10s;
        for (auto& wait : waits) {
            REQUIRE(wait.sent.wait_until(bound) == std::future_status::ready);
            REQUIRE(wait.sent.get());
            REQUIRE(wait.received.wait_until(bound) == std::future_status::ready);
            auto result = wait.received.get();
            auto const expected = make_ping(wait.nonce).payload;
            size_t accounted = 0;
            if (result) {
                REQUIRE(result->payload == expected);
                ++accounted;
                ++consumed;
            } else {
                REQUIRE(result.error() == error::channel_timeout);
                ++timed_out;
            }
            accounted += drain_matching(wait.peer->messages(), expected);
            ++total;
            lost += accounted == 0 ? 1 : 0;
            duplicated += accounted > 1 ? 1 : 0;
        }
    }

    std::printf("stress772: waits=%zu consumed=%zu timed_out=%zu lost=%zu duplicated=%zu\n",
        total, consumed, timed_out, lost, duplicated);
    for (auto const& peer : peers) {
        peer->stop();
    }
    pool.join();
    CHECK(duplicated == 0);
    REQUIRE(lost == 0);
}
