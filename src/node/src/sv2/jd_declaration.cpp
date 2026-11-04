// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kth/node/sv2/jd_declaration.hpp>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#include <kth/domain/chain/output.hpp>
#include <kth/domain/chain/transaction.hpp>
#include <kth/node/sv2/primitives.hpp>

namespace kth::node::sv2 {

namespace {

template <typename Message>
data_chunk frame(Message const& message) {
    auto const payload = to_data_chunk(message);
    auto bytes = to_data_chunk(message_header{Message::channel_message
        ? message_header::channel_msg_flag : uint16_t{0}, Message::message_type,
        static_cast<uint32_t>(payload.size())});
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

template <typename Message>
std::optional<Message> parse(byte_span payload) {
    byte_reader source(payload);
    auto message = Message::from_data(source);
    if ( ! message || ! source.is_exhausted()) return std::nullopt;
    return std::move(*message);
}

std::optional<domain::chain::output::list> pool_outputs(byte_span bytes, uint64_t revenue) {
    byte_reader source(bytes);
    auto const count = source.read_variable_little_endian();
    // A serialized output needs at least a value and a script length. Bound
    // the count before any allocation, even for a short malicious response.
    if ( ! count || *count == 0 || *count > 256 || *count > source.remaining_size() / 9) {
        return std::nullopt;
    }
    domain::chain::output::list outputs;
    outputs.reserve(static_cast<size_t>(*count));
    for (size_t i = 0; i < *count; ++i) {
        auto output = domain::chain::output::from_data(source, true);
        if ( ! output || output->value() != 0 || output->token_data()) return std::nullopt;
        outputs.push_back(std::move(*output));
    }
    if ( ! source.is_exhausted()) return std::nullopt;
    outputs.front().set_value(revenue);
    return outputs;
}

} // namespace

jd_declaration::jd_declaration(std::string identity, bool full_template)
    : identity_(std::move(identity))
    , full_template_(full_template)
{}

bool jd_declaration::pending() const {
    return state_ == state::allocating || state_ == state::declaring || state_ == state::installing;
}

std::optional<mining::declared_work> const& jd_declaration::work() const {
    return work_;
}

bool jd_declaration::has_template() const {
    return has_template_;
}

blockchain::mining_template const& jd_declaration::template_value() const {
    return template_;
}

bool jd_declaration::can_push_solution() const {
    return full_template_ && (state_ == state::installing || state_ == state::accepted);
}

void jd_declaration::supersede() {
    if ( ! pending()) return;
    state_ = state::idle;
    superseded_ = true;
    work_.reset();
    transactions_.clear();
}

void jd_declaration::note_target(hash_digest const& target) {
    // A custom job's target is fixed when it is sent with nTime. Updates
    // received while obtaining its token still apply to that future job.
    if (state_ == state::allocating || state_ == state::declaring) {
        channel_.target = target;
        if (work_) work_->upstream_target = target;
    }
}

jd_declaration::result jd_declaration::fail(std::string reason) {
    state_ = state::failed;
    work_.reset();
    transactions_.clear();
    // template_ stays. Fallback republishes that selection with the solo payout.
    result out;
    out.error = std::move(reason);
    return out;
}

jd_declaration::result jd_declaration::start(blockchain::mining_template value,
    open_extended_mining_channel_success const& channel) {
    if (pending() || state_ == state::failed) return fail("declaration is not idle");
    if (identity_.empty() || identity_.size() > b0_255_max
        || request_id_ == std::numeric_limits<uint32_t>::max()) {
        return fail("invalid declaration identity or exhausted request ids");
    }
    if (channel.channel_id == channel.group_channel_id
        || channel.extranonce_size < jd_session::min_extranonce_size
        || channel.extranonce_prefix.size() + size_t{channel.extranonce_size} > 32
        || std::all_of(channel.target.begin(), channel.target.end(), [](uint8_t x) { return x == 0; })) {
        return fail("invalid declaration channel");
    }
    if (value.selection.txs.size() > b0_64k_max) return fail("too many declaration transactions");
    transactions_.clear();
    uint64_t size = 4 + 2;
    for (auto const& tx : value.selection.txs) {
        if ( ! tx) return fail("null declaration transaction");
        if (full_template_) {
            domain::chain::transaction const& transaction = *tx;
            auto const raw_size = transaction.serialized_size(true);
            if (raw_size > b0_16m_max || size + 3 + raw_size > jd_message_max_payload) {
                return fail("declaration transaction data exceeds the local frame limit");
            }
            size += 3 + raw_size;
            // These are the same BCH network bytes the local Template Provider
            // returns. Keep them once so missing-transaction replies use them intact.
            transactions_.push_back(to_data_chunk(transaction, true));
        }
    }
    ++request_id_;
    channel_ = channel;
    template_ = std::move(value);
    has_template_ = true;
    superseded_ = false;
    work_.reset();
    state_ = state::allocating;
    result out;
    out.declaration = frame(allocate_mining_job_token{identity_, request_id_});
    out.progress = true;
    return out;
}

jd_declaration::result jd_declaration::install(data_chunk token) {
    set_custom_mining_job message;
    message.channel_id = channel_.channel_id;
    message.request_id = request_id_;
    message.mining_job_token = std::move(token);
    message.version = template_.version;
    message.prev_hash = template_.previous_block_hash;
    message.ntime_start = template_.current_time;
    message.nbits = template_.bits;
    message.coinbase_tx_version = 1;
    message.coinbase_prefix = work_->coinbase_prefix;
    message.coinbase_tx_input_sequence = 0xffffffffu;
    message.coinbase_tx_outputs = work_->coinbase_tx_outputs;
    message.coinbase_tx_locktime = 0;
    message.merkle_path = work_->merkle_path;
    state_ = state::installing;
    result out;
    out.mining = frame(message);
    out.progress = true;
    return out;
}

jd_declaration::result jd_declaration::process(jd_peer peer,
    message_header const& header, byte_span payload) {
    if (header.msg_length != payload.size() || payload.size() > jd_message_max_payload) {
        return fail("invalid declaration frame length");
    }
    if (header.extension() != 0) return {};
    // The tip moved while this request was outstanding. The reply belongs to
    // work we are no longer installing, so it must not take the connection down.
    if (state_ == state::idle && superseded_) return {};
    if (state_ == state::failed || ! pending()) return fail("unexpected declaration response");
    if (state_ == state::allocating && peer == jd_peer::declaration && ! header.channel_msg()
        && header.msg_type == allocate_mining_job_token_success::message_type) {
        auto const allocation = parse<allocate_mining_job_token_success>(payload);
        if ( ! allocation || allocation->request_id != request_id_ || allocation->mining_job_token.empty()) {
            return fail("invalid token allocation response");
        }
        auto const outputs = pool_outputs(allocation->coinbase_tx_outputs, template_.coinbase_value);
        if ( ! outputs) return fail("invalid pool coinbase outputs");
        work_ = mining::make_declared_work(template_, *outputs, request_id_,
            channel_.extranonce_prefix, channel_.extranonce_size);
        if ( ! work_) return fail("declared coinbase or template exceeds mining limits");
        work_->upstream_channel_id = channel_.channel_id;
        work_->upstream_target = channel_.target;
        if ( ! full_template_) return install(allocation->mining_job_token);

        declare_mining_job message;
        message.request_id = request_id_;
        message.mining_job_token = allocation->mining_job_token;
        message.version = template_.version;
        message.coinbase_tx_prefix = work_->coinbase_tx_prefix;
        message.coinbase_tx_suffix = work_->coinbase_tx_suffix;
        // BCH has no SegWit: the spec's wtxid list carries ordinary transaction
        // ids in the Template Provider's CTOR order, with no witness commitment.
        for (auto const& tx : template_.selection.txs) message.wtxid_list.push_back(tx->hash());
        if (message.serialized_size() > jd_message_max_payload) {
            return fail("declaration exceeds the local frame limit");
        }
        state_ = state::declaring;
        result out;
        out.declaration = frame(message);
        out.progress = true;
        return out;
    }
    if (state_ == state::declaring && peer == jd_peer::declaration && ! header.channel_msg()) {
        if (header.msg_type == declare_mining_job_error::message_type) {
            auto const message = parse<declare_mining_job_error>(payload);
            if ( ! message || message->request_id != request_id_) return fail("invalid declaration refusal");
            return fail("JDS refused the local template: " + message->error_code);
        }
        if (header.msg_type == declare_mining_job_success::message_type) {
            auto const message = parse<declare_mining_job_success>(payload);
            if ( ! message || message->request_id != request_id_ || message->new_mining_job_token.empty()) {
                return fail("invalid declaration success");
            }
            return install(message->new_mining_job_token);
        }
        if (header.msg_type == provide_missing_transactions::message_type) {
            auto const message = parse<provide_missing_transactions>(payload);
            if ( ! message || message->request_id != request_id_) {
                return fail("invalid missing transaction request");
            }
            provide_missing_transactions_success response;
            response.request_id = request_id_;
            std::vector<bool> supplied(transactions_.size(), false);
            for (auto const index : message->unknown_tx_position_list) {
                if (index >= transactions_.size() || supplied[index]) {
                    return fail("invalid missing transaction position");
                }
                supplied[index] = true;
                response.transaction_list.push_back(transactions_[index]);
            }
            result out;
            out.declaration = frame(response);
            // Supplying data does not renew the declaration's response deadline.
            return out;
        }
    }
    if (state_ == state::installing && peer == jd_peer::mining && header.channel_msg()) {
        if (header.msg_type == set_custom_mining_job_error::message_type) {
            auto const message = parse<set_custom_mining_job_error>(payload);
            if ( ! message || message->request_id != request_id_ || message->channel_id != channel_.channel_id) {
                return fail("invalid custom job refusal");
            }
            return fail("pool refused the local custom job: " + message->error_code);
        }
        if (header.msg_type == set_custom_mining_job_success::message_type) {
            auto const message = parse<set_custom_mining_job_success>(payload);
            if ( ! message || message->request_id != request_id_ || message->channel_id != channel_.channel_id) {
                return fail("invalid custom job success");
            }
            work_->upstream_job_id = message->job_id;
            state_ = state::accepted;
            transactions_.clear();
            result out;
            out.progress = true;
            out.accepted = true;
            return out;
        }
    }
    return fail("unexpected declaration message or direction");
}

} // namespace kth::node::sv2
