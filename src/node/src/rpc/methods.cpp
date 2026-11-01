// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <array>
#include <expected>
#include <filesystem>
#include <limits>
#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/awaitable.hpp>

#include <kth/blockchain/interface/block_chain.hpp>
#include <kth/domain/config/network.hpp>
#include <kth/domain/version.hpp>
#include <kth/domain/message/transaction.hpp>
#include <kth/infrastructure/formats/base_16.hpp>
#include <kth/infrastructure/utility/byte_reader.hpp>
#include <kth/infrastructure/utility/data.hpp>

#include <kth/node/rpc/dispatch.hpp>
#include <kth/node/rpc/error.hpp>
#include <kth/node/mining/block_submit.hpp>
#include <kth/node/mining/job_store.hpp>
#include <kth/node/rpc/json.hpp>
#include <kth/node/rpc/mining.hpp>
#include <kth/node/rpc/query.hpp>
#include <kth/node/p2p_node.hpp>

namespace kth::node::rpc {

namespace {

// The bytes the regular files in `dir` take (getblockchaininfo's size_on_disk,
// over the block and undo files).
std::uint64_t files_size(std::filesystem::path const& dir) {
    std::uint64_t total = 0;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    for (std::filesystem::directory_iterator const end; ! ec && it != end; it.increment(ec)) {
        std::error_code entry_ec;
        if (it->is_regular_file(entry_ec)) {
            auto const size = it->file_size(entry_ec);
            if ( ! entry_ec) {
                total += size;
            }
        }
    }
    return total;
// How many blocks the connected chain has from the block at `height` with
// `hash` up to its tip, that block included; -1 when that block is not on the
// connected chain, including a header stored ahead of it.
std::int64_t confirmations(blockchain::block_chain const& chain,
                           std::size_t height, hash_digest const& hash) {
    auto const view = chain.chain_view();
    if ( ! view || height > view->connected_tip_height) {
        return -1;
    }
    auto const on_chain = height == view->connected_tip_height
        ? std::expected<hash_digest, database::result_code>{view->tip_hash}
        : chain.get_block_hash(height);
    if ( ! on_chain || *on_chain != hash) {
        return -1;
    }
    return static_cast<std::int64_t>(view->connected_tip_height - height + 1);
}

// getblockcount -> the height of the most-work fully-validated block.
// C-API counterpart: kth_chain_sync_last_height (see docs/json-rpc.md).
::asio::awaitable<std::expected<std::string, rpc_error>>
get_block_count(method_context& ctx, request const& /*req*/) {
    auto const heights = co_await ctx.chain.fetch_last_height();
    if ( ! heights) {
        co_return std::unexpected(from_code(heights.error()));
    }

    writer w;
    w.value(static_cast<std::uint64_t>(heights->block));
    co_return w.str();
}

// getblocktemplatelight -> the next-block template minus the transactions, which
// are cached under `job_id` for a later submitblocklight to reassemble.
// C-API counterpart: kth_chain_async_fetch_mining_template (see docs/json-rpc.md).
::asio::awaitable<std::expected<std::string, rpc_error>>
get_block_template_light(method_context& ctx, request const& /*req*/) {
    auto const tmpl = co_await ctx.chain.fetch_mining_template(
        blockchain::default_coinbase_reserve_size);
    if ( ! tmpl) {
        co_return std::unexpected(from_code(tmpl.error()));
    }

    auto const job_id = ctx.jobs.add(tmpl->selection.txs);
    co_return render_mining_template(*tmpl, job_id);
}

// getmininginfo -> height, difficulty, mempool size, and network.
// C-API counterpart: kth_chain_async_fetch_mining_info (see docs/json-rpc.md).
::asio::awaitable<std::expected<std::string, rpc_error>>
get_mining_info(method_context& ctx, request const& /*req*/) {
    auto const info = co_await ctx.chain.fetch_mining_info();
    if ( ! info) {
        co_return std::unexpected(from_code(info.error()));
    }
    co_return render_mining_info(*info);
}

// submitblocklight -> reconstruct the full block from the miner's solved header +
// coinbase and the cached job selection, then submit it to the chain. Returns
// (Bitcoin submitblock semantics) null on accept, or a reject-reason string.
// C-API counterpart: kth_chain_async_organize_block (see docs/json-rpc.md).
::asio::awaitable<std::expected<std::string, rpc_error>>
submit_block_light(method_context& ctx, request const& req) {
    auto const args = params_strings(req.params);
    if (args.size() < 2 || args[0].empty() || args[1].empty()) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "submitblocklight requires [hexdata, job_id]"));
    }

    auto const raw = decode_base16(args[0]);
    if ( ! raw) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "hexdata is not valid hex"));
    }

    // The submitted block carries the solved header and just the coinbase.
    byte_reader reader(*raw);
    auto header_and_coinbase = domain::message::block::from_data(reader, 0u);
    if ( ! header_and_coinbase || header_and_coinbase->transactions().empty()) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "hexdata is not a decodable block with a coinbase"));
    }

    auto const job = ctx.jobs.get(args[1]);
    if ( ! job) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "job_id not found or expired"));
    }

    // Reassemble (coinbase followed by the cached selection) and submit.
    auto const ec = co_await mining::submit_block_light(
        ctx.chain,
        header_and_coinbase->header(),
        header_and_coinbase->transactions().front(),
        *job);

    writer w;
    if (ec) {
        w.value(ec.message()); // BIP22-style reject reason
    } else {
        w.value_null();        // accepted
    }
    co_return w.str();
}

// ---- blockchain query methods --------------------------------------------

// getbestblockhash -> the tip block hash.
// C-API counterpart: kth_chain_sync_block_hash (see docs/json-rpc.md).
::asio::awaitable<std::expected<std::string, rpc_error>>
get_best_block_hash(method_context& ctx, request const& /*req*/) {
    auto const heights = ctx.chain.get_last_heights();
    if ( ! heights) {
        co_return std::unexpected(make_error(error_code::internal_error,
            "chain height unavailable"));
    }
    auto const hash = ctx.chain.get_block_hash(heights->block);
    if ( ! hash) {
        co_return std::unexpected(make_error(error_code::internal_error,
            "best block hash unavailable"));
    }
    writer w;
    w.value(encode_hash(*hash));
    co_return w.str();
}

// getblockhash <height> -> the block hash at that height.
// C-API counterpart: kth_chain_sync_block_hash.
::asio::awaitable<std::expected<std::string, rpc_error>>
get_block_hash(method_context& ctx, request const& req) {
    auto const height = params_uint(req.params, 0);
    if ( ! height) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "getblockhash requires [height] (a non-negative integer)"));
    }
    auto const hash = ctx.chain.get_block_hash(static_cast<std::size_t>(*height));
    if ( ! hash) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "block height out of range"));
    }
    writer w;
    w.value(encode_hash(*hash));
    co_return w.str();
}

// getdifficulty -> the difficulty of the next required work.
// C-API counterpart: kth_chain_sync_mining_info.
::asio::awaitable<std::expected<std::string, rpc_error>>
get_difficulty(method_context& ctx, request const& /*req*/) {
    auto const info = co_await ctx.chain.fetch_mining_info();
    if ( ! info) {
        co_return std::unexpected(from_code(info.error()));
    }
    writer w;
    w.value(info->difficulty);
    co_return w.str();
}

// getblockchaininfo -> chain, heights, best block hash, difficulty, median time
// past, sync state, chain work, and the size of the block and undo files.
// getnetworkhashps [nblocks=120] [height=-1] -> the hashes per second the
// network did over the `nblocks` blocks ending at `height`, on the connected
// chain: the work those blocks add, over the time their timestamps span (the
// block before them included, as the first time). A non-positive nblocks
// counts the default 120; a height of -1 or past the connected tip means the
// connected tip. 0 when the window spans no time.
// C-API counterpart: kth_chain_sync_block_header_by_height (each block's time
// and bits, from which the work follows).
::asio::awaitable<std::expected<std::string, rpc_error>>
get_network_hash_ps(method_context& ctx, request const& req) {
    constexpr std::int64_t default_blocks = 120;
    auto const nblocks = params_int(req.params, 0, default_blocks);
    auto const height = params_int(req.params, 1, -1);
    if ( ! nblocks || ! height) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "getnetworkhashps takes [nblocks, height] as integers"));
    }

    auto const view = ctx.chain.chain_view();
    if ( ! view) {
        co_return std::unexpected(make_error(error_code::internal_error,
            "chain state unavailable"));
    }
    auto const& index = ctx.chain.headers();
    auto const tip = index.find(view->tip_hash);
    if (tip == blockchain::header_index::null_index) {
        co_return std::unexpected(make_error(error_code::internal_error,
            "connected tip not in the header index"));
    }

    auto const tip_height = view->connected_tip_height;
    auto const end_height = *height < 0 || std::uint64_t(*height) > tip_height
        ? tip_height : std::size_t(*height);
    auto const window = std::min(
        std::size_t(*nblocks <= 0 ? default_blocks : *nblocks), end_height);

    auto const last = index.get_ancestor(tip, static_cast<std::int32_t>(end_height));
    auto const first = index.get_ancestor(last, static_cast<std::int32_t>(end_height - window));
    if (last == blockchain::header_index::null_index ||
        first == blockchain::header_index::null_index) {
        co_return std::unexpected(make_error(error_code::internal_error,
            "window not in the header index"));
    }

    // Block times need not increase, so take the extremes over the window.
    auto min_time = std::numeric_limits<std::uint32_t>::max();
    auto max_time = std::numeric_limits<std::uint32_t>::min();
    for (auto idx = last; idx != blockchain::header_index::null_index;
         idx = index.get_parent_index(idx)) {
        auto const time = index.get_timestamp(idx);
        min_time = std::min(min_time, time);
        max_time = std::max(max_time, time);
        if (idx == first) {
            break;
        }
    }

    co_return render_network_hash_ps(index.get_chain_work(first), index.get_chain_work(last),
        min_time, max_time);
}

// getblockchaininfo -> chain, height, headers, best block hash, difficulty.
// C-API counterpart: kth_chain_sync_mining_info (+ sync_block_hash).
::asio::awaitable<std::expected<std::string, rpc_error>>
get_blockchain_info(method_context& ctx, request const& /*req*/) {
    // The tip fields from one published view: a height and a hash read apart
    // can describe two different tips.
    auto const view = ctx.chain.chain_view();
    if ( ! view || ! view->state) {
        co_return std::unexpected(make_error(error_code::internal_error,
            "chain state unavailable"));
    }
    auto const heights = ctx.chain.get_last_heights();
    if ( ! heights) {
        co_return std::unexpected(make_error(error_code::internal_error,
            "chain height unavailable"));
    }

    // The header index counts work from the genesis block's child on; the
    // chain's work includes the genesis block's own.
    std::optional<uint256_t> chain_work;
    auto const& index = ctx.chain.headers();
    if (auto const idx = index.find(view->tip_hash); idx != blockchain::header_index::null_index) {
        auto const genesis = index.get_ancestor(idx, 0);
        if (genesis != blockchain::header_index::null_index) {
            chain_work = index.get_chain_work(idx) +
                domain::chain::header::proof(index.get_bits(genesis));
        }
    }

    co_return render_blockchain_info({
        .chain = chain_name(view->state->network()),
        .blocks = view->connected_tip_height,
        .headers = heights->header,
        .best_block_hash = view->tip_hash,
        .difficulty = blockchain::difficulty_from_bits(view->state->work_required()),
        .median_time = view->state->median_time_past(),
        .chain_work = chain_work,
        .size_on_disk = files_size(ctx.chain.data_dir() / "blocks"),
        .initial_block_download = ! ctx.chain.synchronization().synchronized()});
}

// getchaintips -> every chain tip the header index knows, highest first: the
// connected tip as "active", and each other header without children with its
// branch length from the connected chain, as "valid-headers" when the node
// holds every block of the branch and "headers-only" otherwise.
// C-API counterpart: none yet; the C-API does not expose the header index.
::asio::awaitable<std::expected<std::string, rpc_error>>
get_chain_tips(method_context& ctx, request const& /*req*/) {
    using blockchain::header_index;
    auto const view = ctx.chain.chain_view();
    if ( ! view) {
        co_return std::unexpected(make_error(error_code::internal_error,
            "chain state unavailable"));
    }
    auto const& index = ctx.chain.headers();
    auto const active = index.find(view->tip_hash);
    if (active == header_index::null_index) {
        co_return std::unexpected(make_error(error_code::internal_error,
            "connected tip not in the header index"));
    }

    // A tip is an entry no other entry names as its parent. Entries are only
    // appended, so reading the count once gives a consistent prefix.
    auto const count = index.size();
    std::vector<bool> has_child(count, false);
    for (std::size_t i = 0; i < count; ++i) {
        auto const parent = index.get_parent_index(header_index::index_t(i));
        if (parent != header_index::null_index && parent < count) {
            has_child[parent] = true;
        }
    }

    auto const connected_height = std::int32_t(view->connected_tip_height);
    std::vector<chain_tip> tips;
    for (std::size_t i = 0; i < count; ++i) {
        auto const idx = header_index::index_t(i);
        if (idx == active) {
            tips.push_back({view->connected_tip_height, view->tip_hash, 0, "active"});
            continue;
        }
        if (has_child[i]) {
            continue;
        }
        // Down the branch to the connected chain, noting any block not held.
        auto const height = index.get_height(idx);
        auto fork = idx;
        bool all_data = true;
        while (fork != header_index::null_index) {
            auto const fork_height = index.get_height(fork);
            if (fork_height <= connected_height && index.get_ancestor(active, fork_height) == fork) {
                break;
            }
            all_data = all_data && index.has_status(fork, blockchain::header_status::have_data);
            fork = index.get_parent_index(fork);
        }
        if (fork == header_index::null_index) {
            continue;
        }
        tips.push_back({std::size_t(height), index.get_hash(idx),
            std::size_t(height - index.get_height(fork)),
            all_data ? "valid-headers" : "headers-only"});
    }
    std::ranges::stable_sort(tips, std::ranges::greater{}, &chain_tip::height);
    co_return render_chain_tips(tips);
}

// getrawtransaction <txid> -> the wire-serialized transaction as hex.
// C-API counterpart: kth_chain_sync_transaction.
::asio::awaitable<std::expected<std::string, rpc_error>>
get_raw_transaction(method_context& ctx, request const& req) {
    auto const args = params_strings(req.params);
    if (args.empty() || args[0].empty()) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "getrawtransaction requires [txid]"));
    }
    auto const hash = decode_hash(args[0]);
    if ( ! hash) {
        co_return std::unexpected(make_error(error_code::invalid_params, "invalid txid"));
    }
    auto const result = co_await ctx.chain.fetch_transaction(*hash, /*require_confirmed*/ false);
    if ( ! result) {
        co_return std::unexpected(from_code(result.error()));
    }
    auto const& tx = std::get<0>(*result);
    writer w;
    w.value(transaction_to_hex(*tx));
    co_return w.str();
}

// getblock <blockhash> -> the wire-serialized block as hex.
// C-API counterpart: kth_chain_sync_block_by_hash.
::asio::awaitable<std::expected<std::string, rpc_error>>
get_block(method_context& ctx, request const& req) {
    auto const args = params_strings(req.params);
    if (args.empty() || args[0].empty()) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "getblock requires [blockhash]"));
    }
    auto const hash = decode_hash(args[0]);
    if ( ! hash) {
        co_return std::unexpected(make_error(error_code::invalid_params, "invalid block hash"));
    }
    auto const result = co_await ctx.chain.fetch_block(*hash);
    if ( ! result) {
        co_return std::unexpected(from_code(result.error()));
    }
    auto const& block = std::get<0>(*result);
    writer w;
    w.value(block_to_hex(*block));
    co_return w.str();
}

// getblockheader <blockhash> [verbose=true] -> the header fields with its
// height and confirmations, or with verbose=false the 80-byte serialized
// header as hex.
// C-API counterpart: kth_chain_sync_block_header_by_hash.
::asio::awaitable<std::expected<std::string, rpc_error>>
get_block_header(method_context& ctx, request const& req) {
    auto const args = params_strings(req.params);
    if (args.empty() || args[0].empty()) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "getblockheader requires [blockhash]"));
    }
    auto const hash = decode_hash(args[0]);
    if ( ! hash) {
        co_return std::unexpected(make_error(error_code::invalid_params, "invalid block hash"));
    }
    auto const verbose = params_bool(req.params, 1, /*fallback*/ true);
    if ( ! verbose) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "verbose must be a boolean"));
    }
    auto const result = co_await ctx.chain.fetch_block_header(*hash);
    if ( ! result) {
        co_return std::unexpected(from_code(result.error()));
    }
    if ( ! *verbose) {
        writer w;
        w.value(header_to_hex(*std::get<0>(*result)));
        co_return w.str();
    }

    auto const& [header, height] = *result;
    co_return render_block_header(*header, height, *hash,
        confirmations(ctx.chain, height, *hash));
}

// sendrawtransaction <hexstring> -> the accepted txid, or a reject error.
// C-API counterpart: kth_chain_sync_organize_transaction.
::asio::awaitable<std::expected<std::string, rpc_error>>
send_raw_transaction(method_context& ctx, request const& req) {
    auto const args = params_strings(req.params);
    if (args.empty() || args[0].empty()) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "sendrawtransaction requires [hexstring]"));
    }
    auto const raw = decode_base16(args[0]);
    if ( ! raw) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "hexstring is not valid hex"));
    }
    byte_reader reader(*raw);
    auto tx = domain::chain::transaction::from_data(reader, /*wire*/ true);
    if ( ! tx) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "transaction decode failed"));
    }

    auto const txid = encode_hash(tx->hash());
    auto const pooled = std::make_shared<domain::message::transaction>(std::move(*tx));
    auto const ec = co_await ctx.chain.organize(pooled);
    if (ec) {
        co_return std::unexpected(make_error(error_code::misc_error, ec.message()));
    }

    writer w;
    w.value(txid);
    co_return w.str();
}

// validateaddress <address> -> whether it is a P2PKH or P2SH address of this
// node's network and, if it is, its cashaddr form and the scriptPubKey it pays.
// C-API counterpart: kth_wallet_payment_address_parse_from.
::asio::awaitable<std::expected<std::string, rpc_error>>
validate_address(method_context& ctx, request const& req) {
    auto const args = params_strings(req.params);
    if (args.empty() || args[0].empty()) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "validateaddress requires [address]"));
// decoderawtransaction <hexstring> -> the transaction's fields, decoded for
// this node's network and current script rules.
// C-API counterpart: kth_chain_transaction_construct_from_data (+ its accessors).
::asio::awaitable<std::expected<std::string, rpc_error>>
decode_raw_transaction(method_context& ctx, request const& req) {
    auto const args = params_strings(req.params);
    if (args.empty() || args[0].empty()) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "decoderawtransaction requires [hexstring]"));
    }
    auto const raw = decode_base16(args[0]);
    if ( ! raw) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "hexstring is not valid hex"));
    }
    byte_reader reader(*raw);
    auto const tx = domain::chain::transaction::from_data(reader, /*wire*/ true);
    if ( ! tx || ! reader.is_exhausted()) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "transaction decode failed"));
    }
    auto const view = ctx.chain.chain_view();
    if ( ! view || ! view->state) {
        co_return std::unexpected(make_error(error_code::internal_error,
            "chain state unavailable"));
    }
    co_return render_validate_address(args[0], view->state->network());
// estimatefee [nblocks] -> the fee per kB, in BCH, that the size part of this
// node's transaction price asks for (chain.byte_fee_satoshis). The price also
// charges per signature operation (chain.sigop_fee_satoshis), which a per-kB
// rate cannot carry. kth has no fee estimator, so nblocks, which older clients
// send, does not change the answer.
// C-API counterpart: kth_blockchain_settings.byte_fee_satoshis.
::asio::awaitable<std::expected<std::string, rpc_error>>
estimate_fee(method_context& ctx, request const& /*req*/) {
    co_return render_estimate_fee(ctx.chain.chain_settings().byte_fee_satoshis);
    co_return render_transaction(*tx, view->state->network(), view->state->enabled_flags());
}

// gettxout <txid> <n> [include_mempool=true] -> the output if it is unspent,
// else null. With include_mempool, an output that a mempool transaction spends
// is null, and an output of a mempool transaction has 0 confirmations.
// C-API counterpart: none yet; the C-API has no UTXO reader.
::asio::awaitable<std::expected<std::string, rpc_error>>
get_tx_out(method_context& ctx, request const& req) {
    auto const args = params_strings(req.params);
    auto const index = params_uint(req.params, 1);
    if (args.empty() || args[0].empty() || ! index ||
        *index > std::numeric_limits<std::uint32_t>::max()) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "gettxout requires [txid, n]"));
    }
    auto const hash = decode_hash(args[0]);
    if ( ! hash) {
        co_return std::unexpected(make_error(error_code::invalid_params, "invalid txid"));
    }
    auto const include_mempool = params_bool(req.params, 2, /*fallback*/ true);
    if ( ! include_mempool) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "include_mempool must be a boolean"));
    }
    auto const view = ctx.chain.chain_view();
    if ( ! view || ! view->state) {
        co_return std::unexpected(make_error(error_code::internal_error,
            "chain state unavailable"));
    }
    auto const net = view->state->network();
    auto const flags = view->state->enabled_flags();
    domain::chain::output_point const outpoint{*hash, static_cast<std::uint32_t>(*index)};
    writer none;
    none.value_null();

    if (*include_mempool) {
        auto const& pool = ctx.chain.mempool_ref();
        if (pool.spender(outpoint)) {
            co_return none.str();
        }
        if (auto const output = pool.resolve(outpoint)) {
            co_return render_tx_out(view->tip_hash, 0, *output, /*coinbase*/ false, net, flags);
        }
    }

    // The confirmed UTXO set at the connected tip. "Not resolved" means only
    // that the active versions cannot answer: the older ones are asked with a
    // request of one, and only their miss is absence.
    auto const tip = view->connected_tip_height;
    auto utxo = ctx.chain.get_utxo(outpoint, tip);
    if ( ! utxo && utxo.error() == database::result_code::not_resolved) {
        auto const height = database::to_store_height(tip);
        if ( ! height) {
            co_return std::unexpected(make_error(error_code::internal_error,
                "tip height does not fit the UTXO store"));
        }
        auto const& txid = outpoint.hash();
        std::array<utxoz::lookup_request, 1> const own{utxoz::lookup_request{
            utxoz::make_outpoint(std::span<uint8_t const, 32>{txid.data(), 32}, outpoint.index()),
            *height}};
        auto const resolved = ctx.chain.utxo_resolve(own);
        if ( ! resolved) {
            co_return std::unexpected(make_error(error_code::internal_error,
                "the UTXO store could not be read"));
        }
        auto const it = resolved->found.find(own[0].key);
        if (it == resolved->found.end()) {
            co_return none.str();
        }
        utxo = blockchain::block_chain::output_info{
            it->second.output(), it->second.height(),
            it->second.median_time_past(), it->second.coinbase()};
    }
    if ( ! utxo) {
        co_return std::unexpected(make_error(error_code::internal_error,
            "the UTXO store could not be read"));
    }
    co_return render_tx_out(view->tip_hash,
        static_cast<std::int64_t>(tip - utxo->height + 1), utxo->output, utxo->coinbase, net, flags);
// getnetworkinfo -> this node's version and user agent, protocol version,
// services, whether it relays transactions, its connection count, and the
// relay fee (the size part of its transaction price) in BCH per kB.
// C-API counterpart: kth_network_settings (user_agent, protocol_maximum,
// services, relay_transactions) + kth_blockchain_settings.byte_fee_satoshis.
::asio::awaitable<std::expected<std::string, rpc_error>>
get_network_info(method_context& ctx, request const& /*req*/) {
    if (ctx.network == nullptr) {
        co_return std::unexpected(make_error(error_code::internal_error,
            "network unavailable"));
    }
    auto const& settings = ctx.network->network_settings();
    co_return render_network_info({
        .version = kth::version,
        .subversion = settings.user_agent,
        .protocol_version = settings.protocol_maximum,
        .services = settings.services,
        .relay = settings.relay_transactions,
        .connections = ctx.network->connection_count(),
        .byte_fee_satoshis = ctx.chain.chain_settings().byte_fee_satoshis});
}

// ---- mempool query methods -----------------------------------------------

// getrawmempool -> the txids currently in the mempool.
// C-API counterpart: kth_chain_get_mempool_txids.
::asio::awaitable<std::expected<std::string, rpc_error>>
get_raw_mempool(method_context& ctx, request const& /*req*/) {
    co_return render_hash_list(ctx.chain.get_mempool_txids());
}

// getmempoolinfo -> mempool size, byte total, and fee total.
// C-API counterpart: kth_chain_get_mempool_info.
::asio::awaitable<std::expected<std::string, rpc_error>>
get_mempool_info(method_context& ctx, request const& /*req*/) {
    co_return render_mempool_info(ctx.chain.get_mempool_info());
}

// getmempoolentry <txid> -> the entry's fee/size/time plus depends/spentby.
// C-API counterpart: kth_chain_get_mempool_entry.
::asio::awaitable<std::expected<std::string, rpc_error>>
get_mempool_entry(method_context& ctx, request const& req) {
    auto const args = params_strings(req.params);
    if (args.empty() || args[0].empty()) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "getmempoolentry requires [txid]"));
    }
    auto const hash = decode_hash(args[0]);
    if ( ! hash) {
        co_return std::unexpected(make_error(error_code::invalid_params, "invalid txid"));
    }
    auto const entry = ctx.chain.get_mempool_entry(*hash);
    if ( ! entry) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "transaction not in mempool"));
    }
    co_return render_mempool_entry(*entry,
        ctx.chain.get_mempool_depends(*hash), ctx.chain.get_mempool_spentby(*hash));
}

// getmempoolancestors <txid> -> in-mempool ancestor txids.
// C-API counterpart: kth_chain_get_mempool_ancestors.
::asio::awaitable<std::expected<std::string, rpc_error>>
get_mempool_ancestors(method_context& ctx, request const& req) {
    auto const args = params_strings(req.params);
    if (args.empty() || args[0].empty()) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "getmempoolancestors requires [txid]"));
    }
    auto const hash = decode_hash(args[0]);
    if ( ! hash) {
        co_return std::unexpected(make_error(error_code::invalid_params, "invalid txid"));
    }
    co_return render_hash_list(ctx.chain.get_mempool_ancestors(*hash));
}

// getmempooldescendants <txid> -> in-mempool descendant txids.
// C-API counterpart: kth_chain_get_mempool_descendants.
::asio::awaitable<std::expected<std::string, rpc_error>>
get_mempool_descendants(method_context& ctx, request const& req) {
    auto const args = params_strings(req.params);
    if (args.empty() || args[0].empty()) {
        co_return std::unexpected(make_error(error_code::invalid_params,
            "getmempooldescendants requires [txid]"));
    }
    auto const hash = decode_hash(args[0]);
    if ( ! hash) {
        co_return std::unexpected(make_error(error_code::invalid_params, "invalid txid"));
    }
    co_return render_hash_list(ctx.chain.get_mempool_descendants(*hash));
}

// help [command] -> the commands this server answers, one per line; with a
// command, its name, or that it is unknown. There is no per-command help text
// yet. `commands` is taken by value so the coroutine frame owns its copy.
// C-API counterpart: none; it describes this server.
::asio::awaitable<std::expected<std::string, rpc_error>>
help(std::vector<std::string> commands, request const& req) {
    auto const args = params_strings(req.params);
    std::string text;
    if (args.empty() || args[0].empty()) {
        for (auto const& command : commands) {
            if ( ! text.empty()) {
                text += '\n';
            }
            text += command;
        }
    } else if (std::ranges::binary_search(commands, args[0])) {
        text = args[0];
    } else {
        text = "help: unknown command: " + args[0];
    }

    writer w;
    w.value(text);
    co_return w.str();
}

} // namespace

void register_builtin_methods(dispatcher& d) {
    d.add("getblockcount", get_block_count);
    d.add("getbestblockhash", get_best_block_hash);
    d.add("getblockhash", get_block_hash);
    d.add("getblockheader", get_block_header);
    d.add("getdifficulty", get_difficulty);
    d.add("getblockchaininfo", get_blockchain_info);
    d.add("getnetworkhashps", get_network_hash_ps);
    d.add("getnetworkinfo", get_network_info);
    d.add("getchaintips", get_chain_tips);
    d.add("getrawtransaction", get_raw_transaction);
    d.add("getblock", get_block);
    d.add("sendrawtransaction", send_raw_transaction);
    d.add("validateaddress", validate_address);
    d.add("decoderawtransaction", decode_raw_transaction);
    d.add("gettxout", get_tx_out);
    d.add("getrawmempool", get_raw_mempool);
    d.add("getmempoolinfo", get_mempool_info);
    d.add("estimatefee", estimate_fee);
    d.add("getmempoolentry", get_mempool_entry);
    d.add("getmempoolancestors", get_mempool_ancestors);
    d.add("getmempooldescendants", get_mempool_descendants);
    d.add("getblocktemplatelight", get_block_template_light);
    d.add("getmininginfo", get_mining_info);
    d.add("submitblocklight", submit_block_light);

    // Last, so the list it answers with holds every method above, and itself.
    auto commands = d.methods();
    commands.emplace_back("help");
    std::ranges::sort(commands);
    d.add("help", [commands = std::move(commands)](method_context&, request const& req) {
        return help(commands, req);
    });
}

} // namespace kth::node::rpc
