// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef KTH_NODE_WALLET_DETAIL_SERVICE_IMPL_HPP
#define KTH_NODE_WALLET_DETAIL_SERVICE_IMPL_HPP

#include <chrono>
#include <memory>
#include <stop_token>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/deferred.hpp>
#include <asio/this_coro.hpp>

#include <kth/infrastructure/utility/async_channel.hpp>

#include <kth/domain/chain/block.hpp>
#include <kth/node/wallet/history.hpp>
#include <kth/node/wallet/service.hpp>

namespace kth::node::wallet {

// What a source behind wallet::service answers: the node's own chain, or
// peers. Every operation runs on the source's executor.
struct service::impl {
    virtual ~impl() = default;

    virtual ::asio::any_io_executor executor() const = 0;
    virtual ::asio::awaitable<std::error_code> run() = 0;
    // Thread safe.
    virtual void request_stop() = 0;
    virtual bool stopped() const = 0;
    virtual status current() const = 0;

    virtual ::asio::awaitable<std::error_code> watch(std::vector<target> watched, uint32_t birthday) = 0;
    virtual bool unwatch(target const& watched) = 0;
    virtual ::asio::awaitable<std::expected<target_history, std::error_code>> history(target watched) = 0;
    virtual ::asio::awaitable<std::expected<balance, std::error_code>> get_balance(target watched, token_filter tokens) = 0;
    virtual ::asio::awaitable<std::expected<std::vector<utxo>, std::error_code>> unspent(target watched, token_filter tokens) = 0;
    virtual ::asio::awaitable<std::expected<transaction_info, std::error_code>> transaction(hash_digest txid, std::optional<uint32_t> hint) = 0;
    virtual ::asio::awaitable<std::expected<merkle_proof, std::error_code>> merkle(hash_digest txid, std::optional<uint32_t> hint) = 0;
    virtual ::asio::awaitable<std::expected<std::pair<domain::chain::transaction, merkle_proof>, std::error_code>>
        transaction_at(uint32_t height, uint32_t position) = 0;
    virtual ::asio::awaitable<std::expected<std::vector<rpa_match>, std::error_code>>
        rpa_history(uint16_t prefix, uint8_t bits, uint32_t from, std::optional<uint32_t> to) = 0;
    virtual std::vector<rpa_match> rpa_mempool(uint16_t prefix, uint8_t bits) const = 0;
    virtual ::asio::awaitable<std::expected<hash_digest, std::error_code>> broadcast(domain::chain::transaction tx) = 0;
    virtual fee_estimate fees() const = 0;
    virtual mempool_summary mempool() const = 0;
    virtual ::asio::awaitable<std::optional<domain::chain::header>> header_at(uint32_t height) = 0;
    virtual ::asio::awaitable<std::expected<std::pair<hash_list, hash_digest>, std::error_code>>
        header_branch(uint32_t height, uint32_t cp_height) = 0;
    virtual ::asio::awaitable<std::expected<std::optional<utxo>, std::error_code>> output(domain::chain::point outpoint) = 0;
    virtual ::asio::awaitable<std::optional<dsproof_info>> dsproof(hash_digest id) = 0;
    virtual ::asio::awaitable<std::vector<hash_digest>> dsproof_transactions() = 0;
    virtual ::asio::awaitable<uint64_t> changed(uint64_t seen, std::chrono::steady_clock::time_point deadline) = 0;
};

namespace detail {

// A count of changes, and the operations waiting for it to move. One owner:
// the source's executor.
class change_signal {
public:
    [[nodiscard]] uint64_t count() const { return count_; }

    void bump() {
        ++count_;
        for (auto const& waiting : waiters_) (void)waiting->try_send(0);
        waiters_.clear();
    }

    void close() {
        for (auto const& waiting : waiters_) waiting->close();
        waiters_.clear();
        closed_ = true;
    }

    ::asio::awaitable<uint64_t> wait(uint64_t seen, std::chrono::steady_clock::time_point deadline) {
        if (count_ > seen || closed_) co_return count_;
        auto waiting = std::make_shared<kth::select_channel<int>>(1);
        waiters_.push_back(waiting);
        auto executor = co_await ::asio::this_coro::executor;
        auto completion = co_await ::asio::experimental::make_select(
                ::asio::experimental::receive(*waiting), ::asio::experimental::after(deadline))
            .async_wait(executor, {}, std::stop_token{}, ::asio::as_tuple(::asio::deferred));
        (void)completion;
        std::erase(waiters_, waiting);
        co_return count_;
    }

private:
    uint64_t count_ = 0;
    bool closed_ = false;
    std::vector<std::shared_ptr<kth::select_channel<int>>> waiters_;
};

// The Merkle branch of `leaves[index]` and the root, pairing as transaction
// trees do (a last node without a partner is paired with itself).
std::pair<hash_list, hash_digest> merkle_branch_and_root(hash_list leaves, size_t index);

// Answers both sources give the same way, from a target's confirmed state
// and the mempool transactions the source knows.
std::vector<unconfirmed_tx> unconfirmed_of(target_state const& state, mempool_copy const& pool);
balance balance_of(target_state const& state, mempool_copy const& pool, token_filter tokens);
std::vector<utxo> unspent_of(target_state const& state, mempool_copy const& pool, token_filter tokens);

// The genesis block of a network. A node stores no body for it, and its
// coinbase pays nothing anyone can spend.
domain::chain::block genesis_of(domain::config::network network);

// The full-node source.
std::unique_ptr<service::impl> make_chain_impl(blockchain::block_chain& chain, domain::config::network network,
    chain_settings settings);

} // namespace detail
} // namespace kth::node::wallet

#endif // KTH_NODE_WALLET_DETAIL_SERVICE_IMPL_HPP
