// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Wallet support on a full node: answered from the node's own chain. A
// target is read from its birthday on by parsing the stored blocks, many at
// once, and applying them in height order, as Satoshi's wallet rescanned:
// outputs to a watched script or category, inputs that spend the target's
// own outpoints. Each block that connects later is read the same way. No
// request leaves the node and nothing is stored: the histories are what the
// wallet owns, kept in memory and rebuilt from the chain.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <stop_token>
#include <thread>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <kth/blockchain/interface/block_chain.hpp>
#include <kth/infrastructure/utility/async_channel.hpp>
#include <kth/infrastructure/utility/task_group.hpp>
#include <kth/node/wallet/blocks.hpp>
#include <kth/node/wallet/detail/service_impl.hpp>

namespace kth::node::wallet {

namespace {

using namespace std::chrono_literals;
using domain::chain::point;
using block_ptr = std::shared_ptr<domain::chain::block const>;

constexpr uint64_t default_relay_fee = 1000;
constexpr auto follow_interval = 1s;

struct chain_impl final : service::impl {
    blockchain::block_chain& chain;
    domain::config::network network;
    chain_settings settings;

    std::map<hash_digest, target_state> targets;
    mempool_copy pool;
    std::set<hash_digest> pooled;
    std::optional<uint32_t> followed;
    std::string last_error;
    uint64_t blocks_read = 0;

    mutable std::mutex status_mutex;
    status snapshot;
    std::atomic<bool> stopping{false};

    std::unique_ptr<::asio::io_context> io;
    std::optional<::asio::executor_work_guard<::asio::io_context::executor_type>> work;
    kth::select_channel<int> wake{1};
    kth::select_channel<int> history_lock{1};
    std::thread thread;

    chain_impl(blockchain::block_chain& owner, domain::config::network net, chain_settings given)
        : chain(owner), network(net), settings(given), pool(given.mempool_bytes),
          io(std::make_unique<::asio::io_context>(1)) {
        (void)history_lock.try_send(0);
        work.emplace(io->get_executor());
        update_status();
        thread = std::thread([this] { io->run(); });
    }

    ~chain_impl() override {
        stopping = true;
        ::asio::post(*io, [this] { shutdown(); });
        work.reset();
        auto const deadline = std::chrono::steady_clock::now() + 10s;
        while ( ! io->stopped() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
        io->stop();
        if (thread.joinable()) thread.join();
        io.reset();
    }

    void shutdown() {
        wake.close();
        history_lock.close();
    }

    uint32_t tip() const {
        auto const view = chain.chain_view();
        return view ? uint32_t(view->connected_tip_height) : 0;
    }

    ::asio::any_io_executor executor() const override { return io->get_executor(); }

    void request_stop() override {
        stopping = true;
        ::asio::post(*io, [this] { shutdown(); });
    }

    bool stopped() const override { return stopping; }

    void update_status() {
        status next;
        next.source = "chain";
        next.header_height = tip();
        next.tip = next.header_height;
        next.synced = true;
        next.transaction_keys = true;
        next.blocks_downloaded = blocks_read;
        next.error = last_error;
        std::lock_guard<std::mutex> guard(status_mutex);
        snapshot = std::move(next);
    }

    status current() const override {
        std::lock_guard<std::mutex> guard(status_mutex);
        return snapshot;
    }

    ::asio::awaitable<std::expected<block_ptr, std::error_code>> fetch_block(uint32_t height) {
        // The node stores no body for the genesis block; it is known.
        if (height == 0) co_return std::make_shared<domain::chain::block const>(detail::genesis_of(network));
        auto fetched = co_await chain.fetch_block(size_t(height));
        if ( ! fetched) co_return std::unexpected(fetched.error());
        ++blocks_read;
        co_return block_ptr(fetched->first);
    }

    // Reads the blocks of [from, to] a batch at a time, the batch's reads
    // running at once, and hands each to `use` in height order (or, when
    // `newest_first`, from `to` down). `use` returns true to stop.
    template <typename Use>
    ::asio::awaitable<std::error_code> each_block(uint32_t from, uint32_t to, bool newest_first, Use use) {
        if (from > to) co_return error::success;
        auto const width = std::max<size_t>(1, settings.parallel_reads);
        auto executor = co_await ::asio::this_coro::executor;
        uint64_t const count = uint64_t(to) - from + 1;
        for (uint64_t done = 0; done < count && ! stopping; done += width) {
            auto const batch = size_t(std::min<uint64_t>(width, count - done));
            std::vector<uint32_t> heights;
            for (size_t i = 0; i < batch; ++i) {
                heights.push_back(newest_first ? uint32_t(to - done - i) : uint32_t(from + done + i));
            }
            std::vector<std::expected<block_ptr, std::error_code>> fetched(batch, std::unexpected(error::not_found));
            {
                kth::task_group reads("wallet block reads", executor);
                for (size_t i = 0; i < batch; ++i) {
                    reads.spawn("wallet block read", [this, &fetched, &heights, i]() -> ::asio::awaitable<void> {
                        fetched[i] = co_await fetch_block(heights[i]);
                    });
                }
                co_await reads.join();
            }
            for (size_t i = 0; i < batch; ++i) {
                if ( ! fetched[i]) co_return fetched[i].error();
                if (use(heights[i], *fetched[i])) co_return error::success;
            }
        }
        co_return stopping ? std::error_code(error::service_stopped) : std::error_code{};
    }

    // Every watched target up to the connected tip: the blocks from the
    // lowest next height on, each applied to the targets that have not read it.
    ::asio::awaitable<std::error_code> catch_up() {
        auto executor = co_await ::asio::this_coro::executor;
        auto completion = co_await ::asio::experimental::make_select(::asio::experimental::receive(history_lock))
            .async_wait(executor, {}, std::stop_token{}, ::asio::as_tuple(::asio::deferred));
        auto& [locked, selected] = completion;
        (void)selected;
        if (locked) co_return error::service_stopped;
        struct release {
            kth::select_channel<int>& lock;
            ~release() { (void)lock.try_send(0); }
        } guard{history_lock};

        auto const last = tip();
        uint32_t from = last + 1;
        for (auto const& [key, state] : targets) from = std::min(from, state.next_height());
        if (from > last) co_return error::success;
        auto const ec = co_await each_block(from, last, false, [&](uint32_t height, block_ptr const& blk) {
            for (auto& [key, state] : targets) {
                if (state.next_height() <= height) (void)state.apply_block(height, blk->hash(), blk->transactions());
            }
            return false;
        });
        if (ec) co_return ec;
        for (auto& [key, state] : targets) state.set_synced(last);
        co_return error::success;
    }

    std::optional<uint64_t> known_value(point const& outpoint) const {
        for (auto const& [key, state] : targets) {
            if (auto const owned = state.coins().find(outpoint); owned != state.coins().end()) return owned->second.value;
        }
        return std::nullopt;
    }

    // The mempool copy follows the node's own mempool.
    ::asio::awaitable<void> refresh_pool() {
        auto const txids = chain.get_mempool_txids();
        std::set<hash_digest> now(txids.begin(), txids.end());
        if (now == pooled) co_return;
        pool.clear();
        for (auto const& txid : txids) {
            auto const tx = co_await chain.fetch_unconfirmed_transaction(txid);
            if ( ! tx || ! *tx) continue;
            (void)pool.insert(domain::chain::transaction(**tx), [this](point const& outpoint) { return known_value(outpoint); });
        }
        pooled = std::move(now);
    }

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

    bool unwatch(target const& watched) override {
        return targets.erase(watched.key()) != 0;
    }

    ::asio::awaitable<std::expected<target_state const*, std::error_code>> ready(target const& watched) {
        (void)watch_one(watched, settings.birthday);
        auto const ec = co_await catch_up();
        if (ec) co_return std::unexpected(ec);
        co_await refresh_pool();
        auto const found = targets.find(watched.key());
        if (found == targets.end()) co_return std::unexpected(error::not_found);
        co_return &found->second;
    }

    ::asio::awaitable<std::expected<target_history, std::error_code>> history(target watched) override {
        auto const state = co_await ready(watched);
        if ( ! state) co_return std::unexpected(state.error());
        co_return target_history{(*state)->synced(), (*state)->history(), detail::unconfirmed_of(**state, pool)};
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

    struct located {
        block_ptr blk;
        uint32_t height;
        uint32_t position;
    };

    // The block that holds `txid`: the height hint, a watched history, then
    // the stored blocks from the tip down. No index: the node reads its own
    // blocks, as a wallet reads its filters.
    ::asio::awaitable<std::expected<located, std::error_code>> locate(hash_digest const& txid, std::optional<uint32_t> hint) {
        auto const find_in = [&](block_ptr const& blk) -> std::optional<uint32_t> {
            auto const& txs = blk->transactions();
            for (size_t at = 0; at < txs.size(); ++at) {
                if (txs[at].hash() == txid) return uint32_t(at);
            }
            return std::nullopt;
        };
        std::vector<uint32_t> candidates;
        if (hint) candidates.push_back(*hint);
        for (auto const& [key, state] : targets) {
            for (auto const& entry : state.history()) {
                if (entry.txid == txid) candidates.push_back(entry.height);
            }
        }
        for (auto const height : candidates) {
            if (height > tip()) continue;
            auto const blk = co_await fetch_block(height);
            if ( ! blk) continue;
            if (auto const at = find_in(*blk)) co_return located{*blk, height, *at};
        }
        std::optional<located> found;
        auto const ec = co_await each_block(0, tip(), true, [&](uint32_t height, block_ptr const& blk) {
            if (auto const at = find_in(blk)) {
                found = located{blk, height, *at};
                return true;
            }
            return false;
        });
        if (found) co_return *found;
        if (ec) co_return std::unexpected(ec);
        co_return std::unexpected(error::not_found);
    }

    ::asio::awaitable<std::expected<transaction_info, std::error_code>> transaction(hash_digest txid, std::optional<uint32_t> hint) override {
        co_await refresh_pool();
        if (auto const pending = pool.find(txid)) co_return transaction_info{pending->tx, std::nullopt};
        auto const found = co_await locate(txid, hint);
        if ( ! found) co_return std::unexpected(found.error());
        co_return transaction_info{found->blk->transactions()[found->position],
            confirmation{found->height, found->blk->hash(), found->position}};
    }

    ::asio::awaitable<std::expected<merkle_proof, std::error_code>> merkle(hash_digest txid, std::optional<uint32_t> hint) override {
        auto const found = co_await locate(txid, hint);
        if ( ! found) co_return std::unexpected(found.error());
        co_return merkle_proof{txid, confirmation{found->height, found->blk->hash(), found->position},
            found->blk->transaction_merkle_branch(found->position)};
    }

    ::asio::awaitable<std::expected<std::pair<domain::chain::transaction, merkle_proof>, std::error_code>>
    transaction_at(uint32_t height, uint32_t position) override {
        if (height > tip()) co_return std::unexpected(error::not_found);
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
        auto const last = std::min(to.value_or(tip()), tip());
        std::vector<rpa_match> out;
        auto const ec = co_await each_block(from, last, false, [&](uint32_t height, block_ptr const& blk) {
            auto const& txs = blk->transactions();
            for (size_t at = 1; at < txs.size(); ++at) {
                auto inputs = prefix_inputs(txs[at], prefix, bits);
                if ( ! inputs.empty()) out.push_back(rpa_match{txs[at].hash(), height, std::move(inputs)});
            }
            return false;
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

    // Into the node's own mempool, which relays it.
    ::asio::awaitable<std::expected<hash_digest, std::error_code>> broadcast(domain::chain::transaction tx) override {
        auto const txid = tx.hash();
        auto const pointer = std::make_shared<domain::message::transaction const>(std::move(tx));
        auto const ec = co_await chain.submit_transaction(pointer);
        if (ec) {
            last_error = "transaction rejected: " + ec.message();
            co_return std::unexpected(ec);
        }
        co_await refresh_pool();
        co_return txid;
    }

    fee_estimate fees() const override {
        std::vector<uint64_t> rates;
        for (auto const& [txid, entry] : pool.transactions()) {
            if (entry.fee) rates.push_back(*entry.fee * 1000 / std::max<size_t>(1, entry.size));
        }
        std::ranges::sort(rates);
        auto const median = rates.empty() ? default_relay_fee : rates[rates.size() / 2];
        return fee_estimate{default_relay_fee, std::max(median, default_relay_fee)};
    }

    mempool_summary mempool() const override {
        mempool_summary out{pool.size(), pool.bytes(), {}};
        for (auto const& [txid, entry] : pool.transactions()) {
            if (entry.fee) out.fee_rates.emplace_back(double(*entry.fee) / double(std::max<size_t>(1, entry.size)), entry.size);
        }
        return out;
    }

    ::asio::awaitable<std::optional<domain::chain::header>> header_at(uint32_t height) override {
        if (height > tip()) co_return std::nullopt;
        auto const fetched = co_await chain.fetch_block_header(size_t(height));
        if ( ! fetched || ! fetched->first) co_return std::nullopt;
        co_return domain::chain::header(*fetched->first);
    }

    // Follows the connected tip: each new block is read for every target.
    ::asio::awaitable<std::error_code> run() override {
        auto executor = co_await ::asio::this_coro::executor;
        while ( ! stopping) {
            auto const now = tip();
            if ( ! followed || *followed != now) {
                if ( ! targets.empty()) {
                    auto const ec = co_await catch_up();
                    if (ec) last_error = ec.message();
                }
                co_await refresh_pool();
                followed = now;
            }
            update_status();
            auto completion = co_await ::asio::experimental::make_select(
                    ::asio::experimental::receive(wake), ::asio::experimental::after(std::chrono::steady_clock::now() + follow_interval))
                .async_wait(executor, {}, std::stop_token{}, ::asio::as_tuple(::asio::deferred));
            (void)completion;
        }
        update_status();
        co_return error::success;
    }
};

} // namespace

std::unique_ptr<service::impl> detail::make_chain_impl(blockchain::block_chain& chain, domain::config::network network,
        chain_settings settings) {
    return std::make_unique<chain_impl>(chain, network, settings);
}

} // namespace kth::node::wallet
