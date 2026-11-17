// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kth/node/wallet/service.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <fstream>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <stop_token>
#include <thread>
#include <tuple>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/thread_pool.hpp>
#include <asio/use_awaitable.hpp>

#include <spdlog/spdlog.h>

#include <kth/blockchain/pools/header_organizer.hpp>
#include <kth/blockchain/settings.hpp>
#include <kth/domain/chain/script_filter_builder.hpp>
#include <kth/infrastructure/formats/base_16.hpp>
#include <kth/infrastructure/utility/async_channel.hpp>
#include <kth/infrastructure/utility/task_group.hpp>
#include <kth/network/peer_session.hpp>
#include <kth/network/protocols_coro.hpp>
#include <kth/node/user_agent.hpp>
#include <kth/node/wallet/blocks.hpp>
#include <kth/node/wallet/detail/service_impl.hpp>
#include <kth/node/wallet/filter_store.hpp>
#include <kth/node/wallet/scan.hpp>

namespace kth::node::wallet {

namespace {

using namespace std::chrono_literals;
namespace message = domain::message;
using domain::chain::point;
using block_ptr = std::shared_ptr<domain::chain::block const>;
using network::raw_message;
using type_id = message::inventory_vector::type_id;

constexpr uint8_t script_type = message::script_compact_filter;
constexpr uint8_t prefix_type = message::input_prefix_compact_filter;
constexpr uint32_t checkpoint_interval = uint32_t(message::compact_filter_checkpoint_interval);
constexpr uint32_t headers_per_request = uint32_t(message::max_compact_filter_headers);
constexpr uint32_t filters_per_request = uint32_t(message::max_compact_filters_per_request);
constexpr size_t headers_per_answer = 2000;
constexpr size_t header_size = 80;
// Blocks at the tip downloaded one by one while following it; further
// behind, the mempool copy starts over.
constexpr uint32_t live_window = 12;
constexpr size_t recent_blocks = 24;
constexpr size_t fee_samples = 2000;
constexpr uint64_t default_relay_fee = 1000;
constexpr auto broadcast_wait = 10s;
constexpr auto retry_pause = 5s;
constexpr auto follow_pause = 30s;
constexpr auto connect_timeout = std::chrono::seconds(10);

// What a waiting request does with a message: not its own, part of its
// answer with more to come, or the end of its answer.
enum class step { skip, more, done };
using matcher = std::function<step(raw_message const&)>;

struct waiter {
    explicit waiter(matcher match_message) : match(std::move(match_message)) {}
    matcher match;
    std::vector<raw_message> taken;
    kth::select_channel<int> signal{1};
};

// A connection to a peer. Its one reader hands each message to the request
// that matches it, or handles it as one nobody asked for.
struct peer_link {
    uint64_t id = 0;
    std::string endpoint;
    network::peer_session::ptr session;
    std::list<waiter*> waiters;
    uint64_t services = 0;
    bool isolated = false;
    bool closed = false;
    // The filter types it did not answer for.
    std::set<uint8_t> no_filters;
};

domain::chain::block genesis_block(domain::config::network network) {
    switch (network) {
        case domain::config::network::testnet: return domain::chain::block::genesis_testnet();
        case domain::config::network::regtest: return domain::chain::block::genesis_regtest();
#if defined(KTH_CURRENCY_BCH)
        case domain::config::network::testnet4: return domain::chain::block::genesis_testnet4();
        case domain::config::network::scalenet: return domain::chain::block::genesis_scalenet();
        case domain::config::network::tempnet:
        case domain::config::network::chipnet: return domain::chain::block::genesis_chipnet();
#endif
        default:
        case domain::config::network::mainnet: return domain::chain::block::genesis_mainnet();
    }
}

blockchain::settings validation_settings(domain::config::network network) {
    blockchain::settings result(network);
    result.checkpoints.clear();
    result.checkpoints_sorted.clear();
    result.max_checkpoint_height = 0;
    return result;
}

size_t default_capacity(domain::config::network network) {
    return network == domain::config::network::regtest ? 200'000 : 2'000'000;
}

hash_list locator(blockchain::header_index const& index) {
    hash_list result;
    auto height = index.active_tip_height();
    int32_t step = 1;
    for (;;) {
        result.push_back(index.get_hash(index.active_at(height)));
        if (height == 0) return result;
        if (result.size() > 10) step *= 2;
        height = std::max<int32_t>(0, height - step);
    }
}

// host:port, [v6]:port or host alone.
std::pair<std::string, uint16_t> split_endpoint(std::string const& endpoint, uint16_t fallback) {
    auto host = endpoint;
    uint16_t port = fallback;
    auto const parse_port = [&](std::string const& text) {
        if (text.empty() || ! std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; }) || text.size() > 5) return;
        auto const value = std::stoul(text);
        if (value > 0 && value <= 65535) port = uint16_t(value);
    };
    if ( ! host.empty() && host.front() == '[') {
        auto const close = host.find(']');
        if (close != std::string::npos) {
            if (close + 1 < host.size() && host[close + 1] == ':') parse_port(host.substr(close + 2));
            host = host.substr(1, close - 1);
        }
        return {host, port};
    }
    auto const colon = host.rfind(':');
    if (colon != std::string::npos && host.find(':') == colon) {
        parse_port(host.substr(colon + 1));
        host = host.substr(0, colon);
    }
    return {host, port};
}

::asio::awaitable<void> pause(std::chrono::steady_clock::duration duration) {
    ::asio::steady_timer timer(co_await ::asio::this_coro::executor, duration);
    co_await timer.async_wait(::asio::as_tuple(::asio::use_awaitable));
}

// Waits for the one token a lock channel holds. False once it is closed.
::asio::awaitable<bool> acquire(kth::select_channel<int>& lock) {
    auto executor = co_await ::asio::this_coro::executor;
    auto completion = co_await ::asio::experimental::make_select(::asio::experimental::receive(lock))
        .async_wait(executor, {}, std::stop_token{}, ::asio::as_tuple(::asio::deferred));
    auto& [ec, selected] = completion;
    (void)selected;
    co_return ! ec;
}

struct lock_release {
    kth::select_channel<int>& lock;
    ~lock_release() { (void)lock.try_send(0); }
};

std::optional<domain::chain::transaction> transaction_in(domain::chain::block const& blk, hash_digest const& txid, uint32_t& position) {
    auto const& txs = blk.transactions();
    for (size_t at = 0; at < txs.size(); ++at) {
        if (txs[at].hash() == txid) {
            position = uint32_t(at);
            return txs[at];
        }
    }
    return std::nullopt;
}

} // namespace


namespace detail {

namespace {

unconfirmed_tx new_entry(mempool_tx const& entry, mempool_copy const& pool) {
    return unconfirmed_tx{entry.txid, pool.has_unconfirmed_parent(entry.tx), entry.fee, {}, {}};
}

} // namespace

std::vector<unconfirmed_tx> unconfirmed_of(target_state const& state, mempool_copy const& pool) {
    std::map<point, std::pair<uint64_t, domain::chain::token_data_opt>> owned;
    std::map<hash_digest, unconfirmed_tx> entries;
    for (auto const& [txid, entry] : pool.transactions()) {
        auto const& outputs = entry.tx.outputs();
        for (uint32_t vout = 0; vout < outputs.size(); ++vout) {
            if ( ! state.watched().owns(outputs[vout])) continue;
            owned.insert_or_assign(point(txid, vout), std::pair{outputs[vout].value(), outputs[vout].token_data()});
            auto [found, inserted] = entries.try_emplace(txid, new_entry(entry, pool));
            (void)inserted;
            found->second.outputs.push_back(received{vout, outputs[vout].value(), outputs[vout].token_data(), outputs[vout].script().bytes()});
        }
    }
    for (auto const& [txid, entry] : pool.transactions()) {
        auto const& inputs = entry.tx.inputs();
        for (uint32_t vin = 0; vin < inputs.size(); ++vin) {
            point const outpoint(inputs[vin].previous_output().hash(), inputs[vin].previous_output().index());
            std::optional<std::pair<uint64_t, domain::chain::token_data_opt>> coin_of;
            if (auto const confirmed = state.coins().find(outpoint); confirmed != state.coins().end()) {
                coin_of = std::pair{confirmed->second.value, confirmed->second.token};
            } else if (auto const pending = owned.find(outpoint); pending != owned.end()) {
                coin_of = pending->second;
            }
            if ( ! coin_of) continue;
            auto [found, inserted] = entries.try_emplace(txid, new_entry(entry, pool));
            (void)inserted;
            found->second.inputs.push_back(spent{vin, outpoint, coin_of->first, coin_of->second});
        }
    }
    std::vector<unconfirmed_tx> out;
    for (auto& [txid, entry] : entries) out.push_back(std::move(entry));
    std::ranges::sort(out, [](unconfirmed_tx const& a, unconfirmed_tx const& b) {
        if (a.unconfirmed_parent != b.unconfirmed_parent) return ! a.unconfirmed_parent;
        return encode_hash(a.txid) < encode_hash(b.txid);
    });
    return out;
}

balance balance_of(target_state const& state, mempool_copy const& pool, token_filter tokens) {
    balance result{0, 0};
    for (auto const& owned : state.unspent()) {
        if (keeps(tokens, owned.token)) result.confirmed += owned.value;
    }
    for (auto const& entry : unconfirmed_of(state, pool)) {
        for (auto const& out : entry.outputs) {
            if (keeps(tokens, out.token)) result.unconfirmed += int64_t(out.value);
        }
        for (auto const& in : entry.inputs) {
            if (keeps(tokens, in.token)) result.unconfirmed -= int64_t(in.value);
        }
    }
    return result;
}

std::vector<utxo> unspent_of(target_state const& state, mempool_copy const& pool, token_filter tokens) {
    std::vector<utxo> out;
    for (auto const& owned : state.unspent()) {
        if (keeps(tokens, owned.token) && ! pool.spender(owned.outpoint)) {
            out.push_back(utxo{owned.outpoint, owned.value, owned.token, owned.script, owned.height});
        }
    }
    for (auto const& entry : unconfirmed_of(state, pool)) {
        for (auto const& received_output : entry.outputs) {
            point const outpoint(entry.txid, received_output.vout);
            if (keeps(tokens, received_output.token) && ! pool.spender(outpoint)) {
                out.push_back(utxo{outpoint, received_output.value, received_output.token, received_output.script, std::nullopt});
            }
        }
    }
    return out;
}

domain::chain::block genesis_of(domain::config::network network) {
    return genesis_block(network);
}

std::pair<hash_list, hash_digest> merkle_branch_and_root(hash_list leaves, size_t index) {
    hash_list branch;
    while (leaves.size() > 1) {
        if (leaves.size() % 2 != 0) leaves.push_back(leaves.back());
        branch.push_back(leaves[index ^ 1]);
        hash_list next;
        next.reserve(leaves.size() / 2);
        for (size_t at = 0; at < leaves.size(); at += 2) {
            data_chunk pair(leaves[at].begin(), leaves[at].end());
            pair.insert(pair.end(), leaves[at + 1].begin(), leaves[at + 1].end());
            next.push_back(bitcoin_hash(pair));
        }
        leaves = std::move(next);
        index /= 2;
    }
    return {std::move(branch), leaves.empty() ? null_hash : leaves.front()};
}

} // namespace detail

namespace {

// ============================================================================
// The peer path: headers and filters from peers, blocks where they match.
// ============================================================================

struct peer_impl final : service::impl {
    peer_settings settings;
    network::settings transport;
    std::filesystem::path folder;
    size_t threads;

    // Headers, validated as a kth node validates them.
    blockchain::header_index index;
    blockchain::settings consensus;
    blockchain::header_organizer organizer;
    std::filesystem::path header_file;
    int32_t written_height = -1;
    hash_digest written_tip{};

    std::optional<filter_store> scripts;
    std::optional<filter_store> prefixes;

    // Peers.
    std::vector<std::string> endpoints;
    size_t next_endpoint = 0;
    std::vector<std::shared_ptr<peer_link>> links;
    uint64_t next_link_id = 1;
    size_t next_link_at = 0;
    std::mt19937_64 random{std::random_device{}()};

    // Watched targets and their histories.
    std::map<hash_digest, target_state> targets;
    uint64_t generation = 0;

    // The mempool copy, from relay.
    mempool_copy pool;
    std::set<hash_digest> requested;
    std::map<uint64_t, uint64_t> fee_filters;
    std::deque<uint64_t> fee_rates;
    std::map<point, uint64_t> recent_outputs;
    std::deque<std::vector<point>> recent_created;
    std::optional<uint32_t> live_tip;
    std::set<hash_digest> pending_broadcasts;
    std::set<hash_digest> relayed;
    std::map<hash_digest, std::string> rejections;

    // Downloaded blocks.
    std::map<hash_digest, block_ptr> cache;
    std::deque<std::pair<hash_digest, size_t>> cache_order;
    size_t cache_bytes = 0;
    std::set<hash_digest> inflight;

    std::optional<bool> transaction_keys;
    bool synced = false;
    detail::change_signal changes;
    std::optional<uint32_t> announced_tip;
    // Double-spend proofs from relay, by their hash.
    std::map<hash_digest, dsproof_info> dsproofs;
    std::set<hash_digest> requested_dsproofs;
    // Which peer served the filters of each appended batch: a filter that
    // does not fit its block is traced to the peer that served it.
    std::vector<std::tuple<uint8_t, uint32_t, uint32_t, uint64_t>> served_by;
    bool disputed = false;
    std::string last_error;
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    uint64_t blocks_downloaded = 0;
    std::ofstream sent_log;

    mutable std::mutex status_mutex;
    status snapshot;
    std::atomic<bool> stopping{false};

    // Last: destroyed first, so no operation outlives what it uses.
    std::unique_ptr<::asio::io_context> io;
    std::optional<::asio::executor_work_guard<::asio::io_context::executor_type>> work;
    kth::select_channel<int> wake{1};
    kth::select_channel<int> history_lock{1};
    std::unique_ptr<kth::task_group> tasks;
    std::unique_ptr<::asio::thread_pool> scan_pool;
    std::thread thread;

    impl(peer_settings given, size_t capacity)
        : settings(std::move(given))
        , transport(settings.network)
        , threads(settings.scan_threads != 0 ? settings.scan_threads : std::max<size_t>(1, std::thread::hardware_concurrency()))
        , index(capacity)
        , consensus(validation_settings(settings.network))
        , organizer(index, consensus, settings.network)
        , pool(settings.mempool_bytes)
        , io(std::make_unique<::asio::io_context>(1)) {
        // It keeps headers and filters: it serves no blocks. It asks peers
        // to announce transactions, for the mempool copy.
        transport.services = 0;
        transport.relay_transactions = true;
        transport.validate_checksum = true;
        transport.user_agent = settings.user_agent.empty() ? get_user_agent({"wallet"}) : settings.user_agent;
        transport.channel_handshake_seconds = 10;
        transport.channel_expiration_minutes = 365 * 24 * 60;
        endpoints = settings.peers;
        auto const genesis = genesis_block(settings.network);
        [[maybe_unused]] auto const added = index.add(genesis.hash(), genesis.header());
        organizer.sync_tip();
        organizer.start();
        (void)history_lock.try_send(0);
        work.emplace(io->get_executor());
        tasks = std::make_unique<kth::task_group>("wallet peers", io->get_executor());
        scan_pool = std::make_unique<::asio::thread_pool>(1);
    }

    ~impl() {
        stopping = true;
        ::asio::post(*io, [this] { shutdown(); });
        work.reset();
        auto const deadline = std::chrono::steady_clock::now() + 10s;
        while (thread.joinable() && ! io->stopped() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        io->stop();
        if (thread.joinable()) thread.join();
        scan_pool->join();
        links.clear();
        io.reset();
        organizer.stop();
        persist();
    }

    void start_thread() {
        thread = std::thread([this] { io->run(); });
    }

    ::asio::any_io_executor executor() const override { return io->get_executor(); }

    void ring() { (void)wake.try_send(0); }

    void shutdown() {
        wake.close();
        history_lock.close();
        changes.close();
        for (auto const& link : links) {
            if (link->session) link->session->stop();
        }
    }

    // ------------------------------------------------------------------------
    // Headers file: the active chain's 80-byte headers, replayed through the
    // organizer on open, so a damaged or foreign file is refused.

    void restore() {
        std::ifstream input(header_file, std::ios::binary | std::ios::ate);
        if ( ! input) return;
        auto const length = uint64_t(std::streamoff(input.tellg()));
        if (length < header_size || length % header_size != 0 || length / header_size > index.capacity()) return;
        auto const count = length / header_size;
        input.seekg(0);
        std::array<uint8_t, header_size> bytes{};
        auto read_header = [&]() -> std::optional<message::header> {
            if ( ! input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return std::nullopt;
            byte_reader reader(bytes);
            auto header = domain::chain::header::from_data(reader);
            if ( ! header) return std::nullopt;
            return message::header(*header);
        };
        auto const first = read_header();
        if ( ! first || domain::chain::hash(*first) != index.get_hash(index.active_at(0))) return;
        for (uint64_t height = 1; height < count;) {
            message::header::list batch;
            auto const end = std::min<uint64_t>(count, height + headers_per_answer);
            for (; height < end; ++height) {
                auto header = read_header();
                if ( ! header) return;
                batch.push_back(std::move(*header));
            }
            auto const result = organizer.add_headers(batch);
            if (result.reorg_candidate) organizer.adopt_tip(result.reorg_branch_head);
            if (result.error && result.error != error::stale_chain) return;
        }
        written_height = organizer.header_height();
        written_tip = index.get_hash(index.active_at(written_height));
    }

    void persist() {
        if (header_file.empty()) return;
        auto const height = organizer.header_height();
        auto const append = written_height >= 0 && written_height <= height &&
            index.get_hash(index.active_at(written_height)) == written_tip;
        if (append && written_height == height) return;
        std::ofstream output(header_file, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
        if ( ! output) return;
        for (int32_t at = append ? written_height + 1 : 0; at <= height; ++at) {
            std::array<uint8_t, header_size> bytes{};
            byte_writer writer(bytes);
            if ( ! index.get_header(index.active_at(at)).to_data(writer, true)) return;
            output.write(reinterpret_cast<char const*>(bytes.data()), bytes.size());
        }
        output.flush();
        if ( ! output) return;
        written_height = height;
        written_tip = index.get_hash(index.active_at(height));
    }

    uint32_t header_tip() const {
        return uint32_t(std::max(0, organizer.header_height()));
    }

    std::optional<hash_digest> hash_at(uint32_t height) const {
        if (height > header_tip()) return std::nullopt;
        auto const at = index.active_at(int32_t(height));
        if (at == blockchain::header_index::null_index) return std::nullopt;
        return index.get_hash(at);
    }

    // ------------------------------------------------------------------------
    // Status

    void update_status() {
        status next;
        next.source = "peers";
        next.header_height = header_tip();
        next.tip = scripts ? scripts->tip() : std::nullopt;
        next.input_prefix_tip = prefixes ? prefixes->tip() : std::nullopt;
        next.peers = size_t(std::ranges::count_if(links, [](auto const& link) { return ! link->closed && ! link->isolated; }));
        next.synced = synced;
        next.transaction_keys = transaction_keys;
        next.bytes_received = bytes_received;
        next.bytes_sent = bytes_sent;
        next.blocks_downloaded = blocks_downloaded;
        next.filter_bytes = (scripts ? scripts->data_bytes() : 0) + (prefixes ? prefixes->data_bytes() : 0);
        next.error = last_error;
        if (next.tip != announced_tip) {
            announced_tip = next.tip;
            changes.bump();
        }
        std::lock_guard<std::mutex> guard(status_mutex);
        snapshot = std::move(next);
    }

    status current() const override {
        std::lock_guard<std::mutex> guard(status_mutex);
        return snapshot;
    }

    // ------------------------------------------------------------------------
    // Peers

    void log_sent(std::string_view command, data_chunk const& packet) {
        bytes_sent += packet.size();
        if ( ! sent_log.is_open()) return;
        auto const payload = packet.size() > message::heading::satoshi_fixed_size()
            ? byte_span(packet.data() + message::heading::satoshi_fixed_size(), packet.size() - message::heading::satoshi_fixed_size())
            : byte_span{};
        sent_log << command << ' ' << encode_base16(payload) << '\n';
        sent_log.flush();
    }

    template <typename Message>
    data_chunk packet_of(peer_link const& link, Message const& item) const {
        return message::serialize(link.session->negotiated_version(), item, transport.identifier);
    }

    template <typename Message>
    ::asio::awaitable<std::error_code> send(peer_link& link, Message const& item) {
        if (link.closed) co_return error::channel_stopped;
        auto packet = packet_of(link, item);
        log_sent(Message::command, packet);
        co_return co_await link.session->send_raw(std::move(packet));
    }

    // Sends a request and collects the messages its matcher takes, until it
    // says done, the peer closes, or the request times out.
    template <typename Message>
    ::asio::awaitable<std::expected<std::vector<raw_message>, std::error_code>>
    request(std::shared_ptr<peer_link> link, Message const& item, matcher match) {
        if ( ! link || link->closed) co_return std::unexpected(error::channel_stopped);
        waiter wait(std::move(match));
        link->waiters.push_back(&wait);
        struct leave {
            peer_link& link;
            waiter* entry;
            ~leave() { link.waiters.remove(entry); }
        } guard{*link, &wait};
        auto packet = packet_of(*link, item);
        log_sent(Message::command, packet);
        auto const ec = co_await link->session->send_raw(std::move(packet));
        if (ec) co_return std::unexpected(ec);
        auto const deadline = std::chrono::steady_clock::now() + settings.request_timeout;
        auto executor = co_await ::asio::this_coro::executor;
        auto completion = co_await ::asio::experimental::make_select(
                ::asio::experimental::receive(wait.signal), ::asio::experimental::after(deadline))
            .async_wait(executor, {}, std::stop_token{}, ::asio::as_tuple(::asio::deferred));
        auto& [failed, selected] = completion;
        if (failed) co_return std::unexpected(error::channel_stopped);
        if (selected.index == 1) co_return std::unexpected(error::channel_timeout);
        co_return std::move(wait.taken);
    }

    ::asio::awaitable<void> pump(std::shared_ptr<peer_link> link) {
        while ( ! stopping) {
            auto received = co_await link->session->receive();
            if ( ! received) break;
            auto raw = std::move(*received);
            bytes_received += message::heading::satoshi_fixed_size() + raw.payload.size();
            bool taken = false;
            for (auto it = link->waiters.begin(); it != link->waiters.end(); ++it) {
                auto* entry = *it;
                auto const next = entry->match(raw);
                if (next == step::skip) continue;
                entry->taken.push_back(std::move(raw));
                if (next == step::done) {
                    link->waiters.erase(it);
                    (void)entry->signal.try_send(0);
                }
                taken = true;
                break;
            }
            if ( ! taken) co_await unsolicited(link, raw);
        }
        link->closed = true;
        for (auto* entry : link->waiters) entry->signal.close();
        link->waiters.clear();
        update_status();
    }

    ::asio::awaitable<void> unsolicited(std::shared_ptr<peer_link> link, raw_message const& raw) {
        auto const command = raw.heading.command();
        byte_reader reader(raw.payload);
        auto const version = link->session->negotiated_version();
        if (command == message::ping::command) {
            auto const ping = message::ping::from_data(reader, version);
            if (ping) (void)co_await send(*link, message::pong(ping->nonce()));
        } else if (command == message::inventory::command) {
            auto const inventory = message::inventory::from_data(reader, version);
            if ( ! inventory) co_return;
            hash_list wanted;
            hash_list proofs;
            for (auto const& item : inventory->inventories()) {
                if (item.type() == type_id::block) {
                    ring();
                } else if (item.type() == type_id::transaction) {
                    if (pending_broadcasts.contains(item.hash())) relayed.insert(item.hash());
                    // Every announced transaction is fetched: what is asked for says nothing.
                    if ( ! pool.contains(item.hash()) && requested.insert(item.hash()).second) wanted.push_back(item.hash());
                } else if (item.type() == type_id::double_spend_proof) {
                    if ( ! dsproofs.contains(item.hash()) && requested_dsproofs.insert(item.hash()).second) {
                        proofs.push_back(item.hash());
                    }
                }
            }
            if (requested_dsproofs.size() > 10'000) requested_dsproofs.clear();
            if ( ! proofs.empty()) {
                auto const fetch = message::get_data::create(proofs, type_id::double_spend_proof);
                if (fetch) (void)co_await send(*link, *fetch);
            }
            if (requested.size() > 100'000) requested.clear();
            if ( ! wanted.empty()) {
                auto const fetch = message::get_data::create(wanted, type_id::transaction);
                if (fetch) (void)co_await send(*link, *fetch);
            }
        } else if (command == message::headers::command) {
            ring();
        } else if (command == message::transaction::command) {
            auto tx = message::transaction::from_data(reader, version);
            if (tx) {
                requested.erase(tx->hash());
                on_mempool_tx(domain::chain::transaction(std::move(*tx)));
            }
        } else if (command == message::double_spend_proof::command) {
            auto const proof = message::double_spend_proof::from_data(reader, version);
            if (proof) on_dsproof(*proof, raw.payload);
        } else if (command == message::fee_filter::command) {
            auto const fee = message::fee_filter::from_data(reader, version);
            if (fee) fee_filters[link->id] = fee->minimum_fee();
        } else if (command == message::reject::command) {
            auto const rejected = message::reject::from_data(reader, version);
            if (rejected && pending_broadcasts.contains(rejected->data())) {
                rejections[rejected->data()] = rejected->reason();
            }
        }
    }

    uint64_t next_nonce() {
        uint64_t nonce = 0;
        while (nonce == 0) nonce = random();
        return nonce;
    }

    ::asio::awaitable<std::shared_ptr<peer_link>> connect(std::string endpoint, bool isolated) {
        auto executor = co_await ::asio::this_coro::executor;
        auto const [host, port] = split_endpoint(endpoint, transport.inbound_port);
        auto connected = co_await network::async_connect(executor, host, port, transport, connect_timeout);
        if ( ! connected) co_return nullptr;
        auto session = *connected;
        auto config = network::make_handshake_config(transport, header_tip(), next_nonce());
        config.ws_discovery = false;
        auto handshake = co_await network::perform_handshake_direct(*session, config);
        if ( ! handshake) {
            session->stop();
            co_return nullptr;
        }
        auto link = std::make_shared<peer_link>();
        link->id = next_link_id++;
        link->endpoint = std::move(endpoint);
        link->session = session;
        link->isolated = isolated;
        if (handshake->peer_version) link->services = handshake->peer_version->services();
        tasks->spawn("wallet peer I/O", [session]() -> ::asio::awaitable<void> { (void)co_await session->run(); });
        tasks->spawn("wallet peer reader", pump(link));
        co_return link;
    }

    ::asio::awaitable<void> keep_peers() {
        std::erase_if(links, [](auto const& link) { return link->closed; });
        auto const wanted = std::min(settings.service_peers, endpoints.size());
        size_t attempts = 0;
        while (links.size() < wanted && attempts < endpoints.size() && ! stopping) {
            auto const endpoint = endpoints[next_endpoint++ % endpoints.size()];
            ++attempts;
            if (std::ranges::any_of(links, [&](auto const& link) { return link->endpoint == endpoint; })) continue;
            auto link = co_await connect(endpoint, false);
            if (link) links.push_back(std::move(link));
        }
        update_status();
    }

    std::shared_ptr<peer_link> next_link() {
        std::vector<std::shared_ptr<peer_link>> open;
        for (auto const& link : links) {
            if ( ! link->closed) open.push_back(link);
        }
        if (open.empty()) return nullptr;
        return open[next_link_at++ % open.size()];
    }

    std::vector<std::shared_ptr<peer_link>> filter_servers(uint8_t type) const {
#if defined(KTH_CURRENCY_BCH)
        auto const filters = uint64_t(message::version::service::node_compact_filters);
#else
        auto const filters = uint64_t{0};
#endif
        std::vector<std::shared_ptr<peer_link>> out;
        for (auto const& link : links) {
            if ( ! link->closed && (link->services & filters) != 0 && ! link->no_filters.contains(type)) out.push_back(link);
        }
        return out;
    }

    // ------------------------------------------------------------------------
    // Headers

    ::asio::awaitable<std::error_code> sync_headers(std::shared_ptr<peer_link> link) {
        while ( ! stopping) {
            auto const tip_before = index.get_hash(index.active_at(organizer.header_height()));
            message::get_headers ask(locator(index), null_hash);
            auto answer = co_await request(link, ask, [](raw_message const& m) {
                return m.heading.command() == message::headers::command ? step::done : step::skip;
            });
            if ( ! answer) co_return answer.error();
            byte_reader reader(answer->back().payload);
            auto const headers = message::headers::from_data(reader, link->session->negotiated_version());
            if ( ! headers || ! headers->is_sequential()) co_return error::bad_stream;
            auto const& elements = headers->elements();
            if (elements.empty()) co_return error::success;
            if (index.size() + elements.size() > index.capacity()) co_return error::operation_failed;
            auto const result = organizer.add_headers(elements);
            if (result.reorg_candidate) {
                organizer.adopt_tip(result.reorg_branch_head);
                on_reorg(uint32_t(std::max(0, result.reorg_fork_height)));
            } else if (result.error && result.error != error::stale_chain) {
                co_return error::bad_stream;
            }
            auto const tip_after = index.get_hash(index.active_at(organizer.header_height()));
            update_status();
            if (elements.size() < headers_per_answer || tip_after == tip_before) co_return error::success;
        }
        co_return error::service_stopped;
    }

    void on_reorg(uint32_t fork) {
        spdlog::info("[wallet] Chain reorganization above height {}", fork);
        ++generation;
        if (scripts) (void)scripts->truncate(fork + 1);
        if (prefixes) (void)prefixes->truncate(fork + 1);
        for (auto& [key, state] : targets) (void)state.rollback(fork);
        if (live_tip) live_tip = std::min(*live_tip, fork);
        pool.clear();
        requested.clear();
        dsproofs.clear();
        synced = false;
        changes.bump();
    }

    // ------------------------------------------------------------------------
    // Filters

    using header_answer = std::expected<message::compact_filter_headers, std::error_code>;

    ::asio::awaitable<header_answer> filter_hashes(std::shared_ptr<peer_link> link, uint8_t type,
            uint32_t start, hash_digest const& stop) {
        message::get_compact_filter_headers ask{type, start, stop};
        auto const version = link->session->negotiated_version();
        auto answer = co_await request(link, ask, [type, stop, version](raw_message const& m) {
            if (m.heading.command() != message::compact_filter_headers::command) return step::skip;
            byte_reader reader(m.payload);
            auto const parsed = message::compact_filter_headers::from_data(reader, version);
            return parsed && parsed->filter_type == type && parsed->stop_hash == stop ? step::done : step::skip;
        });
        if ( ! answer) co_return std::unexpected(answer.error());
        byte_reader reader(answer->back().payload);
        auto parsed = message::compact_filter_headers::from_data(reader, version);
        if ( ! parsed) co_return std::unexpected(error::bad_stream);
        co_return std::move(*parsed);
    }

    // The filter header at `before` (the block before a birthday), chained
    // from the agreed checkpoint at or below it.
    ::asio::awaitable<std::expected<hash_digest, std::error_code>> header_before(std::shared_ptr<peer_link> link,
            uint8_t type, uint32_t before, std::vector<hash_digest> const& checkpoints) {
        auto const base = (before / checkpoint_interval) * checkpoint_interval;
        hash_digest previous = null_hash;
        uint32_t start = 0;
        if (base > 0) {
            auto const at = base / checkpoint_interval - 1;
            if (at >= checkpoints.size()) co_return std::unexpected(error::not_found);
            previous = checkpoints[at];
            if (before == base) co_return previous;
            start = base + 1;
        }
        auto const stop = hash_at(before);
        if ( ! stop) co_return std::unexpected(error::not_found);
        auto answer = co_await filter_hashes(link, type, start, *stop);
        if ( ! answer) co_return std::unexpected(answer.error());
        if (answer->previous_filter_header != previous || answer->filter_hashes.size() != size_t(before - start + 1)) {
            co_return std::unexpected(error::bad_stream);
        }
        for (auto const& hash : answer->filter_hashes) previous = next_filter_header_from_hash(hash, previous);
        co_return previous;
    }

    ::asio::awaitable<std::error_code> sync_filters(uint8_t type) {
        auto& store = type == script_type ? *scripts : *prefixes;
        auto const tip = header_tip();
        auto const tip_hash = hash_at(tip);
        if ( ! tip_hash) co_return error::success;
        auto const birthday = std::min(settings.birthday, tip);
        auto const generation_before = generation;
        if (store.next() > tip && store.first() <= birthday) co_return error::success;

        // Checkpoints from every peer that serves the type: they must agree.
        std::optional<std::vector<hash_digest>> agreed;
        for (auto const& link : filter_servers(type)) {
            message::get_compact_filter_checkpoint ask{type, *tip_hash};
            auto const version = link->session->negotiated_version();
            auto const stop = *tip_hash;
            auto answer = co_await request(link, ask, [type, stop, version](raw_message const& m) {
                if (m.heading.command() != message::compact_filter_checkpoint::command) return step::skip;
                byte_reader reader(m.payload);
                auto const parsed = message::compact_filter_checkpoint::from_data(reader, version);
                return parsed && parsed->filter_type == type && parsed->stop_hash == stop ? step::done : step::skip;
            });
            if ( ! answer) {
                if (answer.error() == error::channel_timeout) link->no_filters.insert(type);
                continue;
            }
            byte_reader reader(answer->back().payload);
            auto const parsed = message::compact_filter_checkpoint::from_data(reader, version);
            if ( ! parsed) continue;
            if ( ! agreed) {
                agreed = parsed->filter_headers;
                continue;
            }
            auto const common = std::min(agreed->size(), parsed->filter_headers.size());
            if ( ! std::equal(agreed->begin(), agreed->begin() + common, parsed->filter_headers.begin())) {
                last_error = "peers disagree on the filter header checkpoints of filter type " + std::to_string(type);
                co_return error::bad_stream;
            }
        }
        if ( ! agreed) co_return error::success;
        auto const& checkpoints = *agreed;
        auto const servers = filter_servers(type);
        if (servers.empty()) co_return error::success;

        // A store that starts after the birthday, or holds nothing yet at a
        // birthday past genesis, starts again at the birthday.
        if (store.first() > birthday || (store.empty() && store.first() != birthday)) {
            hash_digest previous = null_hash;
            if (birthday > 0) {
                auto const before = co_await header_before(servers.front(), type, birthday - 1, checkpoints);
                if ( ! before) co_return before.error();
                previous = *before;
            }
            if (auto const ec = store.restart(birthday, previous)) co_return ec;
        }

        size_t turn = 0;
        while (store.next() <= tip && ! stopping) {
            if (generation != generation_before) co_return error::success;
            auto const start = store.next();
            auto const end = std::min(start + headers_per_request - 1, tip);
            auto const link = servers[turn++ % servers.size()];
            if (link->closed) co_return error::channel_stopped;
            auto const stop = hash_at(end);
            if ( ! stop) co_return error::success;
            auto hashes = co_await filter_hashes(link, type, start, *stop);
            if ( ! hashes) co_return hashes.error();
            if (hashes->previous_filter_header != store.tip_header()) {
                last_error = "filter headers do not chain to the stored ones at height " + std::to_string(start);
                co_return error::bad_stream;
            }
            if (hashes->filter_hashes.size() != size_t(end - start + 1)) co_return error::bad_stream;

            std::vector<filter_entry> entries(end - start + 1);
            std::map<hash_digest, size_t> position;
            auto header = store.tip_header();
            for (uint32_t height = start; height <= end; ++height) {
                auto const at = size_t(height - start);
                auto const block = hash_at(height);
                if ( ! block) co_return error::success;
                header = next_filter_header_from_hash(hashes->filter_hashes[at], header);
                if (height > 0 && height % checkpoint_interval == 0) {
                    auto const cp = height / checkpoint_interval - 1;
                    if (cp < checkpoints.size() && checkpoints[cp] != header) {
                        last_error = "filter header at " + std::to_string(height) + " differs from the checkpoint";
                        co_return error::bad_stream;
                    }
                }
                entries[at].block = *block;
                entries[at].header = header;
                position[*block] = at;
            }

            for (auto chunk = start; chunk <= end; chunk += filters_per_request) {
                auto const chunk_end = std::min(chunk + filters_per_request - 1, end);
                auto const chunk_stop = *hash_at(chunk_end);
                message::get_compact_filters ask{type, chunk, chunk_stop};
                auto const version = link->session->negotiated_version();
                auto remaining = size_t(chunk_end - chunk + 1);
                std::set<hash_digest> wanted;
                for (auto height = chunk; height <= chunk_end; ++height) wanted.insert(entries[height - start].block);
                auto answer = co_await request(link, ask, [type, version, remaining, wanted](raw_message const& m) mutable {
                    if (m.heading.command() != message::compact_filter::command) return step::skip;
                    byte_reader reader(m.payload);
                    auto const parsed = message::compact_filter::from_data(reader, version);
                    if ( ! parsed || parsed->filter_type != type || ! wanted.contains(parsed->block_hash)) return step::skip;
                    wanted.erase(parsed->block_hash);
                    return --remaining == 0 ? step::done : step::more;
                });
                if ( ! answer) co_return answer.error();
                for (auto const& raw : *answer) {
                    byte_reader reader(raw.payload);
                    auto parsed = message::compact_filter::from_data(reader, version);
                    if ( ! parsed) co_return error::bad_stream;
                    auto const found = position.find(parsed->block_hash);
                    if (found == position.end()) co_return error::bad_stream;
                    if (bitcoin_hash(parsed->filter) != hashes->filter_hashes[found->second]) {
                        last_error = "a filter does not match its filter hash";
                        co_return error::bad_stream;
                    }
                    entries[found->second].filter = std::move(parsed->filter);
                }
            }
            if (generation != generation_before || store.next() != start) co_return error::success;
            if (auto const ec = store.append(entries)) co_return ec;
            served_by.emplace_back(type, start, end, link->id);
            update_status();
        }
        co_return error::success;
    }

    // ------------------------------------------------------------------------
    // Blocks

    void cache_insert(hash_digest const& hash, block_ptr const& blk, size_t bytes) {
        if (cache.contains(hash)) return;
        cache.emplace(hash, blk);
        cache_order.emplace_back(hash, bytes);
        cache_bytes += bytes;
        while (cache_bytes > settings.block_cache_bytes && cache_order.size() > 1) {
            auto const [old, size] = cache_order.front();
            cache_order.pop_front();
            cache.erase(old);
            cache_bytes -= size;
        }
    }

    // A filter that does not fit the block it was served for: the block is
    // proven by its header, so the filter is the one that lies. Filters from
    // that height on are dropped and synced again.
    void filter_dispute(uint32_t height, uint8_t type) {
        spdlog::warn("[wallet] The filter of type {} at height {} does not fit its block", type, height);
        last_error = "a peer served a filter that does not fit block " + std::to_string(height);
        disputed = true;
        // The peer that served it serves no more filters to this client.
        for (auto const& [kind, first, last, id] : served_by) {
            if (kind != type || height < first || height > last) continue;
            for (auto const& link : links) {
                if (link->id != id) continue;
                link->no_filters.insert(script_type);
                link->no_filters.insert(prefix_type);
            }
        }
        std::erase_if(served_by, [height](auto const& range) { return std::get<2>(range) >= height; });
        ++generation;
        if (scripts) (void)scripts->truncate(height);
        if (prefixes) (void)prefixes->truncate(height);
        synced = false;
        ring();
    }

    void check_filters(uint32_t height, domain::chain::block const& blk, hash_digest const& hash) {
        if (scripts && scripts->block(height) == hash) {
            auto const filter = scripts->read(height);
            if (filter) {
                auto const checked = check_script_filter(blk, *filter);
                if ( ! checked) {
                    filter_dispute(height, script_type);
                    return;
                }
                if (transaction_keys != checked->transaction_keys) {
                    transaction_keys = checked->transaction_keys;
                    spdlog::info("[wallet] Peers' script filters {} transaction keys", *transaction_keys ? "hold" : "do not hold");
                }
            }
        }
        if (prefixes && prefixes->block(height) == hash) {
            auto const filter = prefixes->read(height);
            if (filter && check_input_prefix_filter(blk, *filter)) filter_dispute(height, prefix_type);
        }
    }

    ::asio::awaitable<std::expected<block_ptr, std::error_code>> fetch_block(uint32_t height) {
        // Peers serve no body for the genesis block; it is known.
        if (height == 0) co_return std::make_shared<domain::chain::block const>(genesis_block(settings.network));
        auto const hash = hash_at(height);
        if ( ! hash) co_return std::unexpected(error::not_found);
        if (auto const cached = cache.find(*hash); cached != cache.end()) co_return cached->second;
        while (inflight.contains(*hash) && ! stopping) {
            co_await pause(20ms);
            if (auto const cached = cache.find(*hash); cached != cache.end()) co_return cached->second;
        }
        if (stopping) co_return std::unexpected(error::service_stopped);
        inflight.insert(*hash);
        struct done {
            std::set<hash_digest>& inflight;
            hash_digest hash;
            ~done() { inflight.erase(hash); }
        } guard{inflight, *hash};

        std::error_code last = error::not_found;
        for (size_t attempt = 0; attempt < 4 && ! stopping; ++attempt) {
            std::shared_ptr<peer_link> link;
            if (settings.isolate_downloads && ! endpoints.empty()) {
                link = co_await connect(endpoints[next_endpoint++ % endpoints.size()], true);
            } else {
                link = next_link();
            }
            if ( ! link) {
                last = error::channel_stopped;
                co_await pause(200ms);
                continue;
            }
            auto const version = link->session->negotiated_version();
            auto const ask = message::get_data::create(hash_list{*hash}, type_id::block);
            if ( ! ask) co_return std::unexpected(error::operation_failed);
            auto const wanted = *hash;
            auto answer = co_await request(link, *ask, [wanted, version](raw_message const& m) {
                auto const command = m.heading.command();
                if (command == message::block::command && m.payload.size() >= header_size &&
                    bitcoin_hash(byte_span(m.payload.data(), header_size)) == wanted) return step::done;
                if (command == message::not_found::command) {
                    byte_reader reader(m.payload);
                    auto const missing = message::not_found::from_data(reader, version);
                    if (missing && std::ranges::any_of(missing->inventories(), [&](auto const& item) {
                            return item.type() == type_id::block && item.hash() == wanted; })) return step::done;
                }
                return step::skip;
            });
            if (link->isolated) link->session->stop();
            if ( ! answer) {
                last = answer.error();
                continue;
            }
            auto const& raw = answer->back();
            if (raw.heading.command() != message::block::command) {
                last = error::not_found;
                continue;
            }
            byte_reader reader(raw.payload);
            auto body = message::block::from_data(reader, version);
            if ( ! body || reader.remaining_size() != 0) {
                last = error::bad_stream;
                continue;
            }
            auto blk = std::make_shared<domain::chain::block const>(domain::chain::block(std::move(*body)));
            if (auto const ec = check_block(*blk, *hash)) {
                last = ec;
                continue;
            }
            check_filters(height, *blk, *hash);
            cache_insert(*hash, blk, raw.payload.size());
            ++blocks_downloaded;
            co_return blk;
        }
        co_return std::unexpected(last);
    }

    // Downloads the blocks at `heights`, a few at a time, and hands each to
    // `use` in height order.
    template <typename Use>
    ::asio::awaitable<std::error_code> each_block(std::vector<uint32_t> const& heights, Use use) {
        auto const width = std::max<size_t>(1, settings.parallel_downloads);
        auto executor = co_await ::asio::this_coro::executor;
        for (size_t at = 0; at < heights.size(); at += width) {
            auto const count = std::min(width, heights.size() - at);
            std::vector<std::expected<block_ptr, std::error_code>> fetched(count, std::unexpected(error::not_found));
            {
                kth::task_group downloads("wallet block downloads", executor);
                for (size_t i = 0; i < count; ++i) {
                    downloads.spawn("wallet block download", [this, &fetched, i, height = heights[at + i]]() -> ::asio::awaitable<void> {
                        fetched[i] = co_await fetch_block(height);
                    });
                }
                co_await downloads.join();
            }
            for (size_t i = 0; i < count; ++i) {
                if ( ! fetched[i]) co_return fetched[i].error();
                if (auto const ec = use(heights[at + i], *fetched[i])) co_return ec;
            }
        }
        co_return error::success;
    }

    // ------------------------------------------------------------------------
    // Following the tip: every new block is downloaded (all of them, so
    // peers learn nothing from which), and the mempool copy follows it.

    ::asio::awaitable<std::error_code> follow_tip() {
        if ( ! scripts || ! scripts->tip()) co_return error::success;
        auto const tip = *scripts->tip();
        uint32_t start = tip;
        if (live_tip && *live_tip >= tip) co_return error::success;
        if (live_tip && tip - *live_tip <= live_window) {
            start = *live_tip + 1;
        } else if (live_tip) {
            // Too many blocks went by: the copy starts over from relay.
            pool.clear();
        }
        std::vector<uint32_t> heights;
        for (auto height = start; height <= tip; ++height) heights.push_back(height);
        co_return co_await each_block(heights, [this](uint32_t height, block_ptr const& blk) -> std::error_code {
            if ( ! pool.remove_for_block(blk->transactions()).empty()) {
                resolve_dsproofs();
                changes.bump();
            }
            std::vector<point> created;
            for (auto const& tx : blk->transactions()) {
                auto const txid = tx.hash();
                for (uint32_t vout = 0; vout < tx.outputs().size(); ++vout) {
                    recent_outputs.insert_or_assign(point(txid, vout), tx.outputs()[vout].value());
                    created.emplace_back(txid, vout);
                }
            }
            for (size_t at = 1; at < blk->transactions().size(); ++at) {
                auto const& tx = blk->transactions()[at];
                uint64_t in = 0;
                bool known = true;
                for (auto const& input : tx.inputs()) {
                    auto const found = recent_outputs.find(point(input.previous_output().hash(), input.previous_output().index()));
                    if (found == recent_outputs.end()) {
                        known = false;
                        break;
                    }
                    in += found->second;
                }
                uint64_t out = 0;
                for (auto const& output : tx.outputs()) out += output.value();
                if (known && in >= out) fee_rates.push_back((in - out) * 1000 / std::max<size_t>(1, tx.serialized_size(true)));
            }
            while (fee_rates.size() > fee_samples) fee_rates.pop_front();
            recent_created.push_back(std::move(created));
            while (recent_created.size() > recent_blocks) {
                for (auto const& old : recent_created.front()) recent_outputs.erase(old);
                recent_created.pop_front();
            }
            live_tip = height;
            return {};
        });
    }

    // ------------------------------------------------------------------------
    // Mempool copy

    std::optional<uint64_t> known_value(point const& outpoint) const {
        if (auto const found = recent_outputs.find(outpoint); found != recent_outputs.end()) return found->second;
        for (auto const& [key, state] : targets) {
            if (auto const owned = state.coins().find(outpoint); owned != state.coins().end()) return owned->second.value;
        }
        return std::nullopt;
    }

    void on_mempool_tx(domain::chain::transaction tx) {
        if (pool.insert(std::move(tx), [this](point const& outpoint) { return known_value(outpoint); })) {
            resolve_dsproofs();
            changes.bump();
        }
    }

    // The mempool transaction a proof is about, and its descendants.
    void describe(dsproof_info& info) const {
        info.descendants.clear();
        auto const spender = pool.spender(info.outpoint);
        info.txid = spender.value_or(null_hash);
        if ( ! spender) return;
        std::vector<hash_digest> pending{*spender};
        std::set<hash_digest> seen;
        while ( ! pending.empty()) {
            auto const txid = pending.back();
            pending.pop_back();
            if ( ! seen.insert(txid).second) continue;
            info.descendants.push_back(txid);
            auto const* entry = pool.find(txid);
            if (entry == nullptr) continue;
            for (uint32_t vout = 0; vout < entry->tx.outputs().size(); ++vout) {
                if (auto const child = pool.spender(point(txid, vout))) pending.push_back(*child);
            }
        }
    }

    void on_dsproof(message::double_spend_proof const& proof, data_chunk const& raw) {
        auto const dspid = proof.hash();
        requested_dsproofs.erase(dspid);
        dsproof_info info{dspid, null_hash, raw, point(proof.out_point().hash(), proof.out_point().index()), {}};
        describe(info);
        dsproofs.insert_or_assign(dspid, std::move(info));
        changes.bump();
    }

    // Proofs follow the mempool: descendants that arrive later join, proofs
    // whose transaction left it go.
    void resolve_dsproofs() {
        for (auto it = dsproofs.begin(); it != dsproofs.end();) {
            describe(it->second);
            if (it->second.txid == null_hash) {
                it = dsproofs.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::vector<unconfirmed_tx> unconfirmed_of(target_state const& state) const {
        return detail::unconfirmed_of(state, pool);
    }

    // ------------------------------------------------------------------------
    // Histories: one scan for every watched target, then the blocks the
    // filters point to, in height order.

    ::asio::awaitable<std::expected<scan_hits, std::error_code>> scan_keys(std::vector<scan_key> keys, uint32_t from, uint32_t to) {
        if ( ! scripts) co_return scan_hits{};
        auto locations = scripts->locations(from, to + 1);
        auto path = scripts->data_path();
        auto const workers = threads;
        co_return co_await ::asio::co_spawn(scan_pool->get_executor(),
            [path, locations = std::move(locations), keys = std::move(keys), workers]()
                -> ::asio::awaitable<std::expected<scan_hits, std::error_code>> {
                co_return scan_script_filters(path, locations, keys, workers);
            }, ::asio::use_awaitable);
    }

    ::asio::awaitable<std::error_code> catch_up() {
        if ( ! co_await acquire(history_lock)) co_return error::service_stopped;
        lock_release release{history_lock};
        for (size_t rounds = 0; rounds < 16 && ! stopping; ++rounds) {
            auto const generation_before = generation;
            if ( ! scripts || ! scripts->tip()) co_return error::success;
            auto const tip = *scripts->tip();
            std::vector<scan_key> keys;
            std::vector<hash_digest> owners;
            uint32_t from = tip + 1;
            for (auto const& [key, state] : targets) {
                auto const next = std::max(state.next_height(), scripts->first());
                if (next > tip) continue;
                keys.push_back(scan_key{key, next});
                owners.push_back(key);
                from = std::min(from, next);
            }
            if (keys.empty()) co_return error::success;
            auto const hits = co_await scan_keys(keys, from, tip);
            if ( ! hits) co_return hits.error();
            if (generation != generation_before) continue;

            std::map<uint32_t, std::vector<hash_digest>> due;
            if (settings.download_all_blocks) {
                for (auto height = from; height <= tip; ++height) {
                    for (size_t k = 0; k < keys.size(); ++k) {
                        if (keys[k].from <= height) due[height].push_back(owners[k]);
                    }
                }
            } else {
                for (auto const& [height, matched] : *hits) {
                    for (auto const k : matched) due[height].push_back(owners[k]);
                }
            }
            std::set<uint32_t> fetch;
            for (auto const& [height, owned] : due) fetch.insert(height);
            if (settings.decoy_blocks > 0 && ! settings.download_all_blocks) {
                std::uniform_int_distribution<uint32_t> any(scripts->first(), tip);
                for (size_t i = 0; i < due.size() * settings.decoy_blocks; ++i) fetch.insert(any(random));
            }
            std::vector<uint32_t> heights(fetch.begin(), fetch.end());
            auto const applied = co_await each_block(heights, [&](uint32_t height, block_ptr const& blk) -> std::error_code {
                if (generation != generation_before) return error::service_stopped;
                auto const found = due.find(height);
                if (found == due.end()) return {};
                for (auto const& key : found->second) {
                    auto const state = targets.find(key);
                    if (state != targets.end() && state->second.next_height() <= height) {
                        (void)state->second.apply_block(height, blk->hash(), blk->transactions());
                    }
                }
                return {};
            });
            if (generation != generation_before) continue;
            if (applied) co_return applied;
            for (auto const& key : owners) {
                auto const state = targets.find(key);
                if (state != targets.end()) state->second.set_synced(tip);
            }
            co_return error::success;
        }
        co_return error::success;
    }

    // ------------------------------------------------------------------------
    // The loop that keeps everything current.

    ::asio::awaitable<std::error_code> run() override {
        std::error_code result;
        while ( ! stopping) {
            co_await keep_peers();
            std::error_code ec;
            auto const peer = next_link();
            if ( ! peer) {
                ec = error::channel_stopped;
                last_error = "no peer connected";
            } else {
                ec = co_await sync_headers(peer);
                if ( ! ec && scripts) ec = co_await sync_filters(script_type);
                if ( ! ec && prefixes) ec = co_await sync_filters(prefix_type);
                if ( ! ec) {
                    synced = scripts && scripts->tip() == header_tip() &&
                        ( ! prefixes || prefixes->tip() == header_tip() || filter_servers(prefix_type).empty());
                    if (synced) ec = co_await follow_tip();
                }
                if ( ! ec && ! targets.empty()) ec = co_await catch_up();
                persist();
            }
            if (disputed) {
                disputed = false;
                if ( ! ec) ec = error::bad_stream;
            } else if ( ! ec && ! synced && filter_servers(script_type).empty()) {
                last_error = "no connected peer serves script filters";
            } else if ( ! ec) {
                last_error.clear();
            }
            if (ec && ! stopping) {
                synced = false;
                if (last_error.empty()) last_error = ec.message();
                spdlog::debug("[wallet] Sync round: {}", ec.message());
            }
            result = ec;
            update_status();
            co_await rest(ec ? retry_pause : follow_pause);
        }
        shutdown();
        co_await tasks->join();
        update_status();
        co_return result;
    }

    ::asio::awaitable<void> rest(std::chrono::steady_clock::duration duration) {
        auto executor = co_await ::asio::this_coro::executor;
        auto completion = co_await ::asio::experimental::make_select(
                ::asio::experimental::receive(wake), ::asio::experimental::after(std::chrono::steady_clock::now() + duration))
            .async_wait(executor, {}, std::stop_token{}, ::asio::as_tuple(::asio::deferred));
        (void)completion;
    }

    // ------------------------------------------------------------------------
    // Answers

    target_state& watch_one(target const& watched, uint32_t birthday) {
        auto const key = watched.key();
        auto found = targets.find(key);
        if (found == targets.end()) {
            found = targets.emplace(key, target_state(watched, std::max(birthday, settings.birthday))).first;
        }
        return found->second;
    }

    ::asio::awaitable<std::error_code> watch(std::vector<target> watched, uint32_t birthday) override {
        for (auto const& one : watched) (void)watch_one(one, birthday);
        co_return co_await catch_up();
    }

    ::asio::awaitable<std::expected<target_state const*, std::error_code>> ready(target const& watched) {
        (void)watch_one(watched, settings.birthday);
        auto const ec = co_await catch_up();
        if (ec) co_return std::unexpected(ec);
        auto const found = targets.find(watched.key());
        if (found == targets.end()) co_return std::unexpected(error::not_found);
        co_return &found->second;
    }

    ::asio::awaitable<std::expected<target_history, std::error_code>> history(target watched) override {
        auto const state = co_await ready(watched);
        if ( ! state) co_return std::unexpected(state.error());
        co_return target_history{(*state)->synced(), (*state)->history(), unconfirmed_of(**state)};
    }

    ::asio::awaitable<std::expected<balance, std::error_code>> get_balance(target watched, token_filter tokens) override {
        auto const state = co_await ready(watched);
        if ( ! state) co_return std::unexpected(state.error());
        co_return detail::balance_of(**state, pool, tokens);
    }

    ::asio::awaitable<std::expected<std::vector<utxo>, std::error_code>> unspent(target watched, token_filter tokens) override {
        auto const state = co_await ready(watched);
        if ( ! state) co_return std::unexpected(state.error());
        co_return detail::unspent_of(**state, pool, tokens);
    }

    // The block that holds `txid`, and its height and position.
    struct located {
        block_ptr blk;
        uint32_t height;
        uint32_t position;
        domain::chain::transaction tx;
    };

    ::asio::awaitable<std::expected<located, std::error_code>> locate(hash_digest const& txid, std::optional<uint32_t> hint) {
        std::vector<uint32_t> candidates;
        if (hint) candidates.push_back(*hint);
        for (auto const& [key, state] : targets) {
            for (auto const& entry : state.history()) {
                if (entry.txid == txid) candidates.push_back(entry.height);
            }
        }
        for (auto const height : candidates) {
            if (height > header_tip()) continue;
            auto const blk = co_await fetch_block(height);
            if ( ! blk) continue;
            uint32_t position = 0;
            if (auto tx = transaction_in(**blk, txid, position)) co_return located{*blk, height, position, std::move(*tx)};
        }
        if (transaction_keys && ! *transaction_keys) co_return std::unexpected(error::not_implemented);
        if ( ! scripts || ! scripts->tip()) co_return std::unexpected(error::not_found);
        auto const hits = co_await scan_keys({scan_key{domain::chain::transaction_key(txid), scripts->first()}},
            scripts->first(), *scripts->tip());
        if ( ! hits) co_return std::unexpected(hits.error());
        // Newest first: a lookup is mostly of a recent transaction.
        for (auto it = hits->rbegin(); it != hits->rend(); ++it) {
            auto const blk = co_await fetch_block(it->first);
            if ( ! blk) co_return std::unexpected(blk.error());
            uint32_t position = 0;
            if (auto tx = transaction_in(**blk, txid, position)) co_return located{*blk, it->first, position, std::move(*tx)};
        }
        co_return std::unexpected(error::not_found);
    }

    ::asio::awaitable<std::expected<transaction_info, std::error_code>> transaction(hash_digest txid, std::optional<uint32_t> hint) override {
        if (auto const pending = pool.find(txid)) co_return transaction_info{pending->tx, std::nullopt};
        auto found = co_await locate(txid, hint);
        if ( ! found) co_return std::unexpected(found.error());
        co_return transaction_info{std::move(found->tx), confirmation{found->height, found->blk->hash(), found->position}};
    }

    ::asio::awaitable<std::expected<merkle_proof, std::error_code>> merkle(hash_digest txid, std::optional<uint32_t> hint) override {
        auto found = co_await locate(txid, hint);
        if ( ! found) co_return std::unexpected(found.error());
        co_return merkle_proof{txid, confirmation{found->height, found->blk->hash(), found->position},
            found->blk->transaction_merkle_branch(found->position)};
    }

    ::asio::awaitable<std::expected<std::pair<domain::chain::transaction, merkle_proof>, std::error_code>>
    transaction_at(uint32_t height, uint32_t position) override {
        auto const blk = co_await fetch_block(height);
        if ( ! blk) co_return std::unexpected(blk.error());
        auto const& txs = (*blk)->transactions();
        if (position >= txs.size()) co_return std::unexpected(error::not_found);
        merkle_proof proof{txs[position].hash(), confirmation{height, (*blk)->hash(), position},
            (*blk)->transaction_merkle_branch(position)};
        co_return std::pair{txs[position], std::move(proof)};
    }

    ::asio::awaitable<std::expected<std::vector<rpa_match>, std::error_code>>
    rpa_history(uint16_t prefix, uint8_t bits, uint32_t from, std::optional<uint32_t> to) override {
        if (bits > 16 || (bits < 16 && (uint32_t(prefix) >> bits) != 0)) co_return std::unexpected(error::operation_failed);
        if ( ! prefixes || ! prefixes->tip()) co_return std::unexpected(error::not_implemented);
        auto const last = std::min(to.value_or(*prefixes->tip()), *prefixes->tip());
        auto const first = std::max(from, prefixes->first());
        std::vector<rpa_match> out;
        if (first > last) co_return out;
        auto locations = prefixes->locations(first, last + 1);
        auto path = prefixes->data_path();
        auto const workers = threads;
        std::vector<scan_prefix> asked{scan_prefix{prefix, bits, first}};
        auto const hits = co_await ::asio::co_spawn(scan_pool->get_executor(),
            [path, locations = std::move(locations), asked, workers]() -> ::asio::awaitable<std::expected<scan_hits, std::error_code>> {
                co_return scan_input_prefix_filters(path, locations, asked, workers);
            }, ::asio::use_awaitable);
        if ( ! hits) co_return std::unexpected(hits.error());
        std::vector<uint32_t> heights;
        for (auto const& [height, matched] : *hits) heights.push_back(height);
        auto const ec = co_await each_block(heights, [&](uint32_t height, block_ptr const& blk) -> std::error_code {
            auto const& txs = blk->transactions();
            for (size_t at = 1; at < txs.size(); ++at) {
                auto inputs = prefix_inputs(txs[at], prefix, bits);
                if ( ! inputs.empty()) out.push_back(rpa_match{txs[at].hash(), height, std::move(inputs)});
            }
            return {};
        });
        if (ec) co_return std::unexpected(ec);
        co_return out;
    }

    std::vector<rpa_match> rpa_mempool(uint16_t prefix, uint8_t bits) const override {
        std::vector<rpa_match> out;
        for (auto const& [txid, entry] : pool.transactions()) {
            auto inputs = prefix_inputs(entry.tx, prefix, bits);
            if ( ! inputs.empty()) out.push_back(rpa_match{txid, std::nullopt, std::move(inputs)});
        }
        return out;
    }

    // Sends the transaction to one peer (over a connection of its own when
    // downloads are isolated) and waits: a reject fails it, another peer
    // announcing it proves it was accepted and relayed.
    ::asio::awaitable<std::expected<hash_digest, std::error_code>> broadcast(domain::chain::transaction tx) override {
        auto const txid = tx.hash();
        std::shared_ptr<peer_link> link;
        if (settings.isolate_downloads && ! endpoints.empty()) {
            link = co_await connect(endpoints[next_endpoint++ % endpoints.size()], true);
        } else {
            link = next_link();
        }
        if ( ! link) co_return std::unexpected(error::channel_stopped);
        pending_broadcasts.insert(txid);
        struct forget {
            impl& self;
            hash_digest txid;
            ~forget() {
                self.pending_broadcasts.erase(txid);
                self.relayed.erase(txid);
                self.rejections.erase(txid);
            }
        } guard{*this, txid};
        auto const ec = co_await send(*link, message::transaction(tx));
        if (ec) co_return std::unexpected(ec);
        // A pong after the transaction: the peer has read it.
        auto const nonce = next_nonce();
        auto const pong = co_await request(link, message::ping(nonce), [nonce, version = link->session->negotiated_version()](raw_message const& m) {
            if (m.heading.command() != message::pong::command) return step::skip;
            byte_reader reader(m.payload);
            auto const parsed = message::pong::from_data(reader, version);
            return parsed && parsed->nonce() == nonce ? step::done : step::skip;
        });
        auto const others = std::ranges::count_if(links, [&](auto const& other) { return other != link && ! other->closed; });
        auto const deadline = std::chrono::steady_clock::now() + broadcast_wait;
        while ( ! stopping) {
            if (auto const reason = rejections.find(txid); reason != rejections.end()) {
                last_error = "transaction rejected: " + reason->second;
                if (link->isolated) link->session->stop();
                co_return std::unexpected(error::operation_failed);
            }
            if (relayed.contains(txid)) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                if (others > 0 || ! pong) {
                    if (link->isolated) link->session->stop();
                    co_return std::unexpected(error::channel_timeout);
                }
                break;
            }
            co_await pause(50ms);
        }
        if (link->isolated) link->session->stop();
        on_mempool_tx(std::move(tx));
        co_return txid;
    }

    fee_estimate fees() const override {
        uint64_t relay = default_relay_fee;
        if ( ! fee_filters.empty()) {
            relay = 0;
            for (auto const& [id, rate] : fee_filters) relay = std::max(relay, rate);
            relay = std::max<uint64_t>(relay, 1);
        }
        std::vector<uint64_t> rates(fee_rates.begin(), fee_rates.end());
        for (auto const& [txid, entry] : pool.transactions()) {
            if (entry.fee) rates.push_back(*entry.fee * 1000 / std::max<size_t>(1, entry.size));
        }
        std::ranges::sort(rates);
        auto const median = rates.empty() ? relay : rates[rates.size() / 2];
        return fee_estimate{relay, std::max(median, relay)};
    }

    mempool_summary mempool() const override {
        mempool_summary out{pool.size(), pool.bytes(), {}};
        for (auto const& [txid, entry] : pool.transactions()) {
            if (entry.fee) out.fee_rates.emplace_back(double(*entry.fee) / double(std::max<size_t>(1, entry.size)), entry.size);
        }
        return out;
    }

    ::asio::awaitable<std::expected<std::pair<hash_list, hash_digest>, std::error_code>>
    header_branch(uint32_t height, uint32_t cp_height) override {
        if (height > cp_height || cp_height > header_tip()) co_return std::unexpected(error::operation_failed);
        hash_list leaves;
        leaves.reserve(size_t(cp_height) + 1);
        for (uint32_t at = 0; at <= cp_height; ++at) leaves.push_back(*hash_at(at));
        co_return detail::merkle_branch_and_root(std::move(leaves), height);
    }

    // Spending an output puts its script's key (or its category's) in the
    // spending block's script filter: only the blocks those keys match are read.
    ::asio::awaitable<std::expected<std::optional<utxo>, std::error_code>> output(point outpoint) override {
        if (pool.spender(outpoint)) co_return std::optional<utxo>{};
        auto const found = co_await transaction(outpoint.hash(), std::nullopt);
        if ( ! found) {
            if (found.error() == error::not_found) co_return std::optional<utxo>{};
            co_return std::unexpected(found.error());
        }
        auto const& outputs = found->tx.outputs();
        if (outpoint.index() >= outputs.size()) co_return std::optional<utxo>{};
        auto const& out = outputs[outpoint.index()];
        auto const& script = out.script().bytes();
        // OP_RETURN outputs are not spendable outputs.
        if ( ! script.empty() && script.front() == 0x6a) co_return std::optional<utxo>{};
        std::optional<uint32_t> height;
        if (found->confirmed) height = found->confirmed->height;
        utxo result{outpoint, out.value(), out.token_data(), script, height};
        if ( ! height || ! scripts || ! scripts->tip() || *scripts->tip() <= *height) co_return std::optional<utxo>{result};
        std::vector<scan_key> keys;
        if ( ! script.empty()) keys.push_back(scan_key{domain::chain::script_key(script), *height + 1});
        if (out.token_data()) keys.push_back(scan_key{domain::chain::category_key(out.token_data()->id), *height + 1});
        if (keys.empty()) co_return std::optional<utxo>{result};
        auto const hits = co_await scan_keys(keys, *height + 1, *scripts->tip());
        if ( ! hits) co_return std::unexpected(hits.error());
        for (auto const& [at, matched] : *hits) {
            auto const blk = co_await fetch_block(at);
            if ( ! blk) co_return std::unexpected(blk.error());
            for (auto const& tx : (*blk)->transactions()) {
                for (auto const& in : tx.inputs()) {
                    if (in.previous_output().hash() == outpoint.hash() && in.previous_output().index() == outpoint.index()) {
                        co_return std::optional<utxo>{};
                    }
                }
            }
        }
        co_return std::optional<utxo>{result};
    }

    ::asio::awaitable<std::optional<dsproof_info>> dsproof(hash_digest id) override {
        if (auto const found = dsproofs.find(id); found != dsproofs.end()) co_return found->second;
        for (auto const& [dspid, info] : dsproofs) {
            if (std::ranges::find(info.descendants, id) != info.descendants.end()) co_return info;
        }
        co_return std::nullopt;
    }

    ::asio::awaitable<std::vector<hash_digest>> dsproof_transactions() override {
        std::set<hash_digest> all;
        for (auto const& [dspid, info] : dsproofs) all.insert(info.descendants.begin(), info.descendants.end());
        co_return std::vector<hash_digest>(all.begin(), all.end());
    }

    ::asio::awaitable<uint64_t> changed(uint64_t seen, std::chrono::steady_clock::time_point deadline) override {
        co_return co_await changes.wait(seen, deadline);
    }

    void request_stop() override {
        stopping = true;
        ::asio::post(*io, [this] { shutdown(); });
    }

    bool stopped() const override {
        return stopping;
    }

    bool unwatch(target const& watched) override {
        return targets.erase(watched.key()) != 0;
    }

    ::asio::awaitable<std::optional<domain::chain::header>> header_at(uint32_t height) override {
        co_return header(height);
    }

    std::optional<domain::chain::header> header(uint32_t height) const {
        if (height > header_tip()) return std::nullopt;
        return index.get_header(index.active_at(int32_t(height)));
    }
};

} // namespace

// ============================================================================
// service
// ============================================================================

std::expected<std::unique_ptr<service>, std::error_code> service::from_peers(peer_settings settings) {
    if (settings.peers.empty()) return std::unexpected(error::operation_failed);
    auto const capacity = settings.header_capacity != 0 ? settings.header_capacity : default_capacity(settings.network);
    auto const folder = settings.directory / std::string(domain::config::chain_name(settings.network,
        domain::config::chain_name_format::display));
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) return std::unexpected(error::file_system);

    auto state = std::make_unique<peer_impl>(std::move(settings), capacity);
    state->folder = folder;
    state->header_file = folder / "headers.dat";
    state->restore();
    auto const block_at = [&state](uint32_t height) { return state->hash_at(height); };
    auto scripts = filter_store::open(folder, script_type, block_at);
    if ( ! scripts) return std::unexpected(scripts.error());
    state->scripts.emplace(std::move(*scripts));
    if (state->settings.input_prefix_filters) {
        auto prefixes = filter_store::open(folder, prefix_type, block_at);
        if ( ! prefixes) return std::unexpected(prefixes.error());
        state->prefixes.emplace(std::move(*prefixes));
    }
    if ( ! state->settings.sent_log.empty()) {
        state->sent_log.open(state->settings.sent_log, std::ios::app);
        if ( ! state->sent_log) return std::unexpected(error::file_system);
    }
    state->update_status();
    state->start_thread();
    return std::unique_ptr<service>(new service(std::unique_ptr<impl>(std::move(state))));
}

std::unique_ptr<service> service::from_chain(blockchain::block_chain& chain, domain::config::network network,
        chain_settings settings) {
    return std::unique_ptr<service>(new service(detail::make_chain_impl(chain, network, settings)));
}

service::service(std::unique_ptr<impl> state) : impl_(std::move(state)) {}

service::~service() = default;

::asio::any_io_executor service::executor() const {
    return impl_->executor();
}

::asio::awaitable<std::error_code> service::run() {
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->run(), ::asio::use_awaitable);
}

void service::stop() {
    impl_->request_stop();
}

status service::current() const {
    return impl_->current();
}

::asio::awaitable<std::expected<status, std::error_code>> service::wait_synced(std::chrono::seconds timeout) {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        auto const now = impl_->current();
        if (now.synced) co_return now;
        if (impl_->stopped()) co_return std::unexpected(error::service_stopped);
        if (std::chrono::steady_clock::now() >= deadline) co_return std::unexpected(error::channel_timeout);
        co_await pause(100ms);
    }
}

::asio::awaitable<std::error_code> service::watch(std::vector<target> targets, uint32_t birthday) {
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->watch(std::move(targets), birthday), ::asio::use_awaitable);
}

::asio::awaitable<bool> service::unwatch(target watched) {
    auto* state = impl_.get();
    co_return co_await ::asio::co_spawn(impl_->executor(), [state, watched]() -> ::asio::awaitable<bool> {
        co_return state->unwatch(watched);
    }, ::asio::use_awaitable);
}

::asio::awaitable<std::expected<target_history, std::error_code>> service::history(target watched) {
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->history(std::move(watched)), ::asio::use_awaitable);
}

::asio::awaitable<std::expected<balance, std::error_code>> service::get_balance(target watched, token_filter tokens) {
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->get_balance(std::move(watched), tokens), ::asio::use_awaitable);
}

::asio::awaitable<std::expected<std::vector<utxo>, std::error_code>> service::unspent(target watched, token_filter tokens) {
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->unspent(std::move(watched), tokens), ::asio::use_awaitable);
}

::asio::awaitable<std::expected<transaction_info, std::error_code>> service::transaction(hash_digest txid, std::optional<uint32_t> height_hint) {
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->transaction(txid, height_hint), ::asio::use_awaitable);
}

::asio::awaitable<std::expected<merkle_proof, std::error_code>> service::merkle(hash_digest txid, std::optional<uint32_t> height_hint) {
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->merkle(txid, height_hint), ::asio::use_awaitable);
}

::asio::awaitable<std::expected<std::pair<domain::chain::transaction, merkle_proof>, std::error_code>>
service::transaction_at(uint32_t height, uint32_t position) {
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->transaction_at(height, position), ::asio::use_awaitable);
}

::asio::awaitable<std::expected<std::vector<rpa_match>, std::error_code>>
service::rpa_history(uint16_t prefix, uint8_t bits, uint32_t from, std::optional<uint32_t> to) {
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->rpa_history(prefix, bits, from, to), ::asio::use_awaitable);
}

::asio::awaitable<std::vector<rpa_match>> service::rpa_mempool(uint16_t prefix, uint8_t bits) {
    auto* state = impl_.get();
    co_return co_await ::asio::co_spawn(impl_->executor(), [state, prefix, bits]() -> ::asio::awaitable<std::vector<rpa_match>> {
        co_return state->rpa_mempool(prefix, bits);
    }, ::asio::use_awaitable);
}

::asio::awaitable<std::expected<hash_digest, std::error_code>> service::broadcast(domain::chain::transaction tx) {
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->broadcast(std::move(tx)), ::asio::use_awaitable);
}

::asio::awaitable<fee_estimate> service::fees() {
    auto* state = impl_.get();
    co_return co_await ::asio::co_spawn(impl_->executor(), [state]() -> ::asio::awaitable<fee_estimate> {
        co_return state->fees();
    }, ::asio::use_awaitable);
}

::asio::awaitable<mempool_summary> service::mempool() {
    auto* state = impl_.get();
    co_return co_await ::asio::co_spawn(impl_->executor(), [state]() -> ::asio::awaitable<mempool_summary> {
        co_return state->mempool();
    }, ::asio::use_awaitable);
}

::asio::awaitable<std::expected<std::pair<hash_list, hash_digest>, std::error_code>>
service::header_branch(uint32_t height, uint32_t cp_height) {
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->header_branch(height, cp_height), ::asio::use_awaitable);
}

::asio::awaitable<std::expected<std::optional<utxo>, std::error_code>> service::output(domain::chain::point outpoint) {
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->output(outpoint), ::asio::use_awaitable);
}

::asio::awaitable<std::optional<dsproof_info>> service::dsproof(hash_digest dspid_or_txid) {
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->dsproof(dspid_or_txid), ::asio::use_awaitable);
}

::asio::awaitable<std::vector<hash_digest>> service::dsproof_transactions() {
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->dsproof_transactions(), ::asio::use_awaitable);
}

::asio::awaitable<uint64_t> service::changed(uint64_t seen, std::chrono::seconds timeout) {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    co_return co_await ::asio::co_spawn(impl_->executor(), impl_->changed(seen, deadline), ::asio::use_awaitable);
}

::asio::awaitable<std::optional<domain::chain::header>> service::header(uint32_t height) {
    auto* state = impl_.get();
    co_return co_await ::asio::co_spawn(impl_->executor(), [state, height]() -> ::asio::awaitable<std::optional<domain::chain::header>> {
        co_return co_await state->header_at(height);
    }, ::asio::use_awaitable);
}

} // namespace kth::node::wallet
