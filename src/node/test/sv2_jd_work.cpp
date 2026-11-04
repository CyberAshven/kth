// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test_helpers.hpp>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

#include <kth/domain/chain/block.hpp>
#include <kth/domain/chain/input.hpp>
#include <kth/domain/chain/output_point.hpp>
#include <kth/domain/chain/token_data.hpp>
#include <kth/domain/message/transaction.hpp>
#include <kth/infrastructure/math/hash.hpp>
#include <kth/infrastructure/utility/endian.hpp>
#include <kth/node/mining/block_submit.hpp>
#include <kth/node/mining/work.hpp>

using namespace kth;
using namespace kth::node::mining;

namespace {

blockchain::mining_template mining_template() {
    blockchain::mining_template value{};
    value.version = 0x20000000u;
    value.height = 100;
    value.bits = 0x207fffffu;
    value.min_time = 100;
    value.current_time = 101;
    value.coinbase_value = 5000000000;
    value.size_limit = 32000000;
    value.sigchecks_limit = 640000;
    return value;
}

domain::chain::output::list outputs(uint64_t value) {
    short_hash hash{};
    hash.fill(0x42);
    return {{value, domain::chain::script(
        domain::chain::script::to_pay_public_key_hash_pattern(hash)), std::nullopt},
        {0, domain::chain::script(domain::chain::script::to_null_data_pattern(
            data_chunk{0x24})), std::nullopt}};
}

domain::chain::transaction coinbase(declared_work const& work, data_chunk const& extra) {
    auto raw = work.coinbase_tx_prefix;
    raw.insert(raw.end(), extra.begin(), extra.end());
    raw.insert(raw.end(), work.coinbase_tx_suffix.begin(), work.coinbase_tx_suffix.end());
    byte_reader source(raw);
    auto tx = domain::chain::transaction::from_data(source, true);
    REQUIRE(tx.has_value());
    REQUIRE(source.is_exhausted());
    return std::move(*tx);
}

} // namespace

TEST_CASE("JD declaration and custom job describe the same BCH coinbase", "[sv2 jd]") {
    auto tmpl = mining_template();
    for (uint32_t i = 0; i < 3; ++i) {
        auto selected_outputs = outputs(1);
        selected_outputs.front().set_token_data(domain::chain::make_fungible(null_hash, i + 1));
        auto tx = std::make_shared<domain::message::transaction>(
            domain::chain::transaction{1, i, {}, std::move(selected_outputs)});
        tmpl.selection.total_size += static_cast<domain::chain::transaction const&>(*tx).serialized_size(true);
        tmpl.selection.txs.push_back(std::move(tx));
    }
    std::sort(tmpl.selection.txs.begin(), tmpl.selection.txs.end(),
        [](auto const& left, auto const& right) {
            return to_uint256(left->hash()) < to_uint256(right->hash());
        });
    auto const pool_outputs = outputs(tmpl.coinbase_value);
    data_chunk const pool_prefix(8, 0x42);
    auto const work = make_declared_work(tmpl, pool_outputs, 7, pool_prefix, 24);
    REQUIRE(work.has_value());
    REQUIRE(work->generation == 7);
    REQUIRE(work->tmpl.selection.txs == tmpl.selection.txs);
    REQUIRE(work->upstream_extranonce_prefix == pool_prefix);
    REQUIRE(work->upstream_extranonce_size == 24);
    auto extra = pool_prefix;
    extra.insert(extra.end(), 24, 0x24);
    auto const tx = coinbase(*work, extra);
    REQUIRE(tx.version() == 1);
    REQUIRE(tx.locktime() == 0);
    REQUIRE(tx.outputs() == pool_outputs);
    REQUIRE(tx.inputs().size() == 1);
    REQUIRE(tx.inputs().front().previous_output().is_null());
    REQUIRE(tx.inputs().front().sequence() == 0xffffffffu);
    REQUIRE(tx.serialized_size(true) >= 100);
    auto script = work->coinbase_prefix;
    REQUIRE(script.size() >= 2);
    REQUIRE(script[0] == 1);
    REQUIRE(script[1] == 100);
    script.insert(script.end(), extra.begin(), extra.end());
    REQUIRE(script == tx.inputs().front().script().bytes());
    REQUIRE(script.size() <= coinbase_script_limit);
    domain::chain::input input(domain::chain::output_point::null(),
        domain::chain::script(script, false), 0xffffffffu);
    domain::chain::transaction const custom(1, 0, {std::move(input)}, pool_outputs);
    REQUIRE(to_data_chunk(custom, true) == to_data_chunk(tx, true));
    REQUIRE(to_data_chunk(tx, true)[4] == 1);

    byte_reader output_source(work->coinbase_tx_outputs);
    auto const count = output_source.read_variable_little_endian();
    REQUIRE(count.has_value());
    REQUIRE(*count == pool_outputs.size());
    for (auto const& output : pool_outputs) {
        auto const parsed = domain::chain::output::from_data(output_source, true);
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == output);
    }
    REQUIRE(output_source.is_exhausted());
    auto root = tx.hash();
    for (auto const& sibling : work->merkle_path) root = bitcoin_hash(build_chunk({root, sibling}));
    auto const block = assemble_block(domain::chain::header{}, tx, tmpl.selection.txs);
    REQUIRE(root == block.generate_merkle_root());
}

TEST_CASE("JD coinbase generations and extranonce bounds preserve declared fields", "[sv2 jd]") {
    auto const tmpl = mining_template();
    auto const pool_outputs = outputs(tmpl.coinbase_value);
    auto const first = make_declared_work(tmpl, pool_outputs, 1, {}, 32);
    auto const second = make_declared_work(tmpl, pool_outputs, 2, {}, 32);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first->coinbase_tx_prefix != second->coinbase_tx_prefix);
    REQUIRE(first->coinbase_tx_suffix == second->coinbase_tx_suffix);
    REQUIRE(first->coinbase_tx_outputs == second->coinbase_tx_outputs);
    REQUIRE(first->merkle_path.empty());
    REQUIRE(coinbase(*first, data_chunk(32, 0x42)).hash()
        != coinbase(*second, data_chunk(32, 0x42)).hash());
    REQUIRE(make_declared_work(tmpl, pool_outputs, 1, data_chunk(8, 0x42), 24));
    for (auto const size : {uint16_t{0}, uint16_t{23}, uint16_t{33}, uint16_t{65535}}) {
        REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {}, size));
    }
    REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {0x42}, 32));
    REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, data_chunk(9, 0x42), 24));

    domain::chain::output::list tiny{{tmpl.coinbase_value,
        domain::chain::script(data_chunk{0x51}, false), std::nullopt}};
    auto const padded = make_declared_work(tmpl, tiny, 1, {}, 24);
    REQUIRE(padded.has_value());
    REQUIRE(coinbase(*padded, data_chunk(24, 0)).serialized_size(true) == 100);
}

TEST_CASE("JD coinbase refuses payout changes tokens and oversized output data", "[sv2 jd]") {
    auto const tmpl = mining_template();
    auto pool_outputs = outputs(tmpl.coinbase_value);
    REQUIRE_FALSE(make_declared_work(tmpl, {}, 1, {}, 24));
    pool_outputs.front().set_value(tmpl.coinbase_value - 1);
    REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {}, 24));
    pool_outputs.back().set_value(1);
    REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {}, 24));
    pool_outputs = outputs(tmpl.coinbase_value);
    pool_outputs.back().set_token_data(domain::chain::make_fungible(null_hash, 1));
    REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {}, 24));
    pool_outputs = outputs(tmpl.coinbase_value);
    pool_outputs.back().set_value(std::numeric_limits<uint64_t>::max());
    REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {}, 24));
    pool_outputs = outputs(tmpl.coinbase_value);
    pool_outputs.resize(256, pool_outputs.back());
    auto const maximum = make_declared_work(tmpl, pool_outputs, 1, {}, 24);
    REQUIRE(maximum.has_value());
    byte_reader source(maximum->coinbase_tx_outputs);
    auto const count = source.read_variable_little_endian();
    REQUIRE(count.has_value());
    REQUIRE(*count == 256);
    pool_outputs.push_back(pool_outputs.back());
    REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {}, 24));
    for (auto const script_size : {size_t{65520}, size_t{65536}}) {
        domain::chain::output::list large{{tmpl.coinbase_value,
            domain::chain::script(data_chunk(script_size, 0x51), false), std::nullopt}};
        REQUIRE_FALSE(make_declared_work(tmpl, large, 1, {}, 24));
    }
}

TEST_CASE("JD coinbase refuses invalid templates and respects exact block size", "[sv2 jd]") {
    auto tmpl = mining_template();
    auto const pool_outputs = outputs(tmpl.coinbase_value);
    auto const work = make_declared_work(tmpl, pool_outputs, 1, {}, 24);
    REQUIRE(work.has_value());
    auto const size = 80 + 1 + work->coinbase_tx_prefix.size()
        + 24 + work->coinbase_tx_suffix.size();
    tmpl.size_limit = size;
    REQUIRE(make_declared_work(tmpl, pool_outputs, 1, {}, 24));
    --tmpl.size_limit;
    REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {}, 24));
    tmpl.size_limit = 80;
    REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {}, 24));
    tmpl = mining_template();
    tmpl.selection.total_size = tmpl.size_limit;
    REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {}, 24));
    tmpl = mining_template();
    tmpl.current_time = tmpl.min_time - 1;
    REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {}, 24));
    for (auto const bits : {0u, 0x20800001u, 0x23000001u}) {
        tmpl = mining_template();
        tmpl.bits = bits;
        REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {}, 24));
    }
    tmpl = mining_template();
    tmpl.selection.txs.push_back({});
    REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {}, 24));
    tmpl.selection.txs.resize(65536);
    REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {}, 24));
    tmpl = mining_template();
    tmpl.height = std::numeric_limits<size_t>::max();
    REQUIRE_FALSE(make_declared_work(tmpl, pool_outputs, 1, {}, 24));
}

TEST_CASE("sv2 JD pool work nests in the upstream extranonce and solo work restores it", "[sv2 jd]") {
    // The pool reconstructs P || N. A downstream channel keeps the rollable
    // width from its Open*Success and puts its id in the bytes between P and
    // that roll. BCH has no witness commitment in this script.
    hash_digest const target = [] {
        hash_digest value;
        value.fill(0xff);
        return value;
    }();
    auto request = [&](uint32_t id, uint16_t min_size) {
        open_request value;
        value.request_id = id;
        value.kind = channel_kind::extended;
        value.max_target = target;
        value.min_extranonce_size = min_size;
        return value;
    };
    work_engine engine(0x0102030405060708ull);
    auto const nested_open = engine.open(request(1, 12));
    auto const wide_open = engine.open(request(2, 32));
    REQUIRE(nested_open.status == open_status::opened);
    REQUIRE(wide_open.status == open_status::opened);
    REQUIRE(nested_open.extranonce_prefix.size() == extranonce_prefix_size);
    REQUIRE(nested_open.extranonce_size == 12);
    REQUIRE(wide_open.extranonce_prefix.size() == extranonce_prefix_size);
    REQUIRE(wide_open.extranonce_size == 32);

    auto const tmpl = mining_template();
    auto const pool_outputs = outputs(tmpl.coinbase_value);
    short_hash solo_hash{};
    solo_hash.fill(0x43);
    domain::chain::output::list const solo_outputs{{tmpl.coinbase_value,
        domain::chain::script(domain::chain::script::to_pay_public_key_hash_pattern(solo_hash)),
        std::nullopt}};
    data_chunk const pool_prefix(8, 0x12);
    pool_binding binding{pool_prefix, 24, 7};
    auto const jobs = engine.on_pool_template(tmpl, pool_outputs, solo_outputs, binding);
    REQUIRE(jobs);
    REQUIRE(jobs->size() == 2);
    auto const& nested = jobs->front().channel_id == nested_open.channel_id
        ? jobs->front() : jobs->back();
    auto const& wide = jobs->front().channel_id == wide_open.channel_id
        ? jobs->front() : jobs->back();
    REQUIRE(nested.extranonce_prefix_update);
    REQUIRE(nested.extranonce_prefix_update->size() == 20);
    REQUIRE(std::equal(pool_prefix.begin(), pool_prefix.end(),
        nested.extranonce_prefix_update->begin()));
    auto const channel_id = to_little_endian(nested.channel_id);
    REQUIRE(std::equal(channel_id.begin(), channel_id.end(),
        nested.extranonce_prefix_update->begin() + static_cast<std::ptrdiff_t>(pool_prefix.size())));
    REQUIRE(std::all_of(nested.extranonce_prefix_update->end() - 4,
        nested.extranonce_prefix_update->end(), [](uint8_t byte) { return byte == 0; }));
    auto const declared = make_declared_work(tmpl, pool_outputs, binding.generation,
        pool_prefix, binding.extranonce_size);
    REQUIRE(declared);
    REQUIRE(nested.coinbase_tx_prefix == declared->coinbase_tx_prefix);
    REQUIRE(nested.coinbase_tx_suffix == declared->coinbase_tx_suffix);
    REQUIRE(nested.merkle_path == declared->merkle_path);
    REQUIRE_FALSE(wide.extranonce_prefix_update);
    REQUIRE(wide.coinbase_tx_prefix != declared->coinbase_tx_prefix);

    share_request nested_share;
    nested_share.extended = true;
    nested_share.channel_id = nested.channel_id;
    nested_share.job_id = nested.job_id;
    nested_share.ntime = nested.ntime_start;
    nested_share.version = nested.version;
    nested_share.extranonce.assign(nested_open.extranonce_size, 0x44);
    auto const proof = engine.submit(nested_share);
    REQUIRE(proof.status != share_status::rejected);
    REQUIRE(proof.pool);
    REQUIRE(proof.pool->upstream_extranonce.size() == binding.extranonce_size);
    REQUIRE(proof.pool->full_extranonce.size()
        == pool_prefix.size() + binding.extranonce_size);
    REQUIRE(std::equal(pool_prefix.begin(), pool_prefix.end(),
        proof.pool->full_extranonce.begin()));
    REQUIRE(proof.pool->upstream_extranonce == data_chunk(
        proof.pool->full_extranonce.begin() + static_cast<std::ptrdiff_t>(pool_prefix.size()),
        proof.pool->full_extranonce.end()));
    REQUIRE(proof.pool->prev_hash == tmpl.previous_block_hash);
    REQUIRE(proof.pool->bits == tmpl.bits);
    REQUIRE(proof.pool->nonce == nested_share.nonce);
    share_request wide_share = nested_share;
    wide_share.channel_id = wide.channel_id;
    wide_share.job_id = wide.job_id;
    wide_share.ntime = wide.ntime_start;
    wide_share.version = wide.version;
    wide_share.extranonce.assign(wide_open.extranonce_size, 0x44);
    auto const solo_share = engine.submit(wide_share);
    REQUIRE(solo_share.status != share_status::rejected);
    REQUIRE_FALSE(solo_share.pool);
    auto wide_coinbase = wide.coinbase_tx_prefix;
    wide_coinbase.insert(wide_coinbase.end(), wide_open.extranonce_prefix.begin(),
        wide_open.extranonce_prefix.end());
    wide_coinbase.insert(wide_coinbase.end(), wide_open.extranonce_size, 0);
    wide_coinbase.insert(wide_coinbase.end(), wide.coinbase_tx_suffix.begin(),
        wide.coinbase_tx_suffix.end());
    byte_reader wide_source(wide_coinbase);
    auto const wide_tx = domain::chain::transaction::from_data(wide_source, true);
    REQUIRE(wide_tx);
    REQUIRE(wide_source.is_exhausted());
    REQUIRE(wide_tx->outputs() == solo_outputs);

    engine.bump_pool_roll(nested.channel_id);
    auto const rolled = engine.on_channel(nested.channel_id, tmpl, solo_outputs);
    REQUIRE(rolled);
    REQUIRE(rolled->coinbase_tx_prefix == nested.coinbase_tx_prefix);
    REQUIRE(rolled->coinbase_tx_suffix == nested.coinbase_tx_suffix);
    REQUIRE(rolled->extranonce_prefix_update);
    REQUIRE(rolled->extranonce_prefix_update->size() == nested.extranonce_prefix_update->size());
    REQUIRE(*rolled->extranonce_prefix_update != *nested.extranonce_prefix_update);
    auto const roll = to_little_endian(uint32_t{1});
    REQUIRE(std::equal(roll.begin(), roll.end(), rolled->extranonce_prefix_update->end() - 4));

    auto const restored = engine.on_template(tmpl, solo_outputs);
    REQUIRE(restored.size() == 2);
    auto const& restored_nested = restored.front().channel_id == nested.channel_id
        ? restored.front() : restored.back();
    auto const& restored_wide = restored.front().channel_id == wide.channel_id
        ? restored.front() : restored.back();
    REQUIRE(restored_nested.extranonce_prefix_update);
    REQUIRE(*restored_nested.extranonce_prefix_update == nested_open.extranonce_prefix);
    REQUIRE_FALSE(restored_wide.extranonce_prefix_update);
    auto restored_coinbase = restored_nested.coinbase_tx_prefix;
    restored_coinbase.insert(restored_coinbase.end(), nested_open.extranonce_prefix.begin(),
        nested_open.extranonce_prefix.end());
    restored_coinbase.insert(restored_coinbase.end(), nested_open.extranonce_size, 0);
    restored_coinbase.insert(restored_coinbase.end(), restored_nested.coinbase_tx_suffix.begin(),
        restored_nested.coinbase_tx_suffix.end());
    byte_reader restored_source(restored_coinbase);
    auto const restored_tx = domain::chain::transaction::from_data(restored_source, true);
    REQUIRE(restored_tx);
    REQUIRE(restored_source.is_exhausted());
    REQUIRE(restored_tx->outputs() == solo_outputs);
    share_request restored_share;
    restored_share.extended = true;
    restored_share.channel_id = restored_nested.channel_id;
    restored_share.job_id = restored_nested.job_id;
    restored_share.ntime = restored_nested.ntime_start;
    restored_share.version = restored_nested.version;
    restored_share.extranonce.assign(nested_open.extranonce_size, 0x45);
    REQUIRE_FALSE(engine.submit(restored_share).pool);
}
