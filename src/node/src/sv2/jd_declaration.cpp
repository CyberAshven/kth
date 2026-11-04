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

// A stale notice on every reply must not allocate forever.
constexpr uint32_t max_payout_restarts = 8;

template <typename Message>
data_chunk frame(Message const& message) {
    auto const payload = to_data_chunk(message);
    auto bytes = to_data_chunk(message_header{framed_extension_type<Message>(),
        Message::message_type, static_cast<uint32_t>(payload.size())});
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

// The returned outputs already sum to the template revenue. Do not move that
// revenue into the first output or keep the allocation placeholders.
std::optional<domain::chain::output::list> payout_outputs(byte_span bytes, uint64_t revenue) {
    byte_reader source(bytes);
    auto const count = source.read_variable_little_endian();
    if ( ! count || *count == 0 || *count > 256 || *count > source.remaining_size() / 9) {
        return std::nullopt;
    }
    domain::chain::output::list outputs;
    outputs.reserve(static_cast<size_t>(*count));
    uint64_t sum = 0;
    for (size_t i = 0; i < *count; ++i) {
        auto output = domain::chain::output::from_data(source, true);
        if ( ! output || output->token_data()) return std::nullopt;
        if (sum > std::numeric_limits<uint64_t>::max() - output->value()) return std::nullopt;
        sum += output->value();
        outputs.push_back(std::move(*output));
    }
    if ( ! source.is_exhausted() || sum != revenue) return std::nullopt;
    return outputs;
}

} // namespace

jd_declaration::jd_declaration(std::string identity, bool full_template)
    : identity_(std::move(identity))
    , full_template_(full_template)
{}

bool jd_declaration::pending() const {
    return state_ == state::allocating || state_ == state::requesting
        || state_ == state::declaring || state_ == state::installing;
}

void jd_declaration::use_payout_outputs(bool enabled) {
    if (pending()) return;
    payout_outputs_ = enabled;
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
    if (state_ == state::allocating || state_ == state::requesting || state_ == state::declaring) {
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
    channel_ = channel;
    template_ = std::move(value);
    has_template_ = true;
    superseded_ = false;
    restarts_ = 0;
    restart_ = false;
    return send_allocation();
}

jd_declaration::result jd_declaration::send_allocation() {
    if (request_id_ == std::numeric_limits<uint32_t>::max()) {
        return fail("invalid declaration identity or exhausted request ids");
    }
    ++request_id_;
    restart_ = false;
    token_.clear();
    script_prefix_.clear();
    reservation_ = 0;
    work_.reset();
    state_ = state::allocating;
    result out;
    out.declaration = frame(allocate_mining_job_token{identity_, request_id_});
    out.progress = true;
    return out;
}

jd_declaration::result jd_declaration::retry_allocation() {
    if (restarts_ >= max_payout_restarts) return fail("payout output request was not settled");
    ++restarts_;
    return send_allocation();
}

jd_declaration::result jd_declaration::declare_work(data_chunk token) {
    if ( ! full_template_) return install(std::move(token));
    declare_mining_job message;
    message.request_id = request_id_;
    message.mining_job_token = std::move(token);
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

jd_declaration::result jd_declaration::request_payout(data_chunk token, size_t reservation) {
    if (template_.height > std::numeric_limits<uint32_t>::max()) {
        return fail("template height does not fit the payout request");
    }
    auto prefix = mining::payout_coinbase_prefix(template_.height, request_id_,
        channel_.extranonce_prefix.size(), channel_.extranonce_size);
    if ( ! prefix) return fail("coinbase prefix does not fit the payout request");
    token_ = std::move(token);
    reservation_ = reservation;
    script_prefix_ = std::move(*prefix);
    token_epoch_ = notice_epoch_;
    request_payout_outputs message;
    message.request_id = request_id_;
    message.mining_job_token = token_;
    message.available_payout_value = template_.coinbase_value;
    message.height = static_cast<uint32_t>(template_.height);
    message.prev_hash = template_.previous_block_hash;
    message.nbits = template_.bits;
    message.coinbase_prefix = script_prefix_;
    if (message.serialized_size() > jd_message_max_payload) {
        return fail("payout request exceeds the local frame limit");
    }
    state_ = state::requesting;
    result out;
    out.declaration = frame(message);
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
    message.coinbase_tx_version = work_->coinbase_version;
    message.coinbase_prefix = work_->coinbase_prefix;
    message.coinbase_tx_input_sequence = work_->coinbase_input_sequence;
    message.coinbase_tx_outputs = work_->coinbase_tx_outputs;
    message.coinbase_tx_locktime = work_->coinbase_locktime;
    message.merkle_path = work_->merkle_path;
    state_ = state::installing;
    result out;
    out.mining = frame(message);
    out.progress = true;
    return out;
}

jd_declaration::result jd_declaration::note_stale(byte_span payload) {
    if ( ! parse<payout_outputs_stale>(payload)) return fail("malformed payout stale notice");
    if (state_ == state::failed) return {};
    ++notice_epoch_;
    unexplained_stale_ = 0;
    // One request stays in flight. The reply that is already owed is consumed,
    // then a new token is allocated. A second request is not sent early.
    if (pending()) restart_ = true;
    return {};
}

jd_declaration::result jd_declaration::accept_payout(request_payout_outputs_success const& success) {
    if ((success.flags & ~exact_outputs_flag) != 0) return fail("payout outputs set unknown flags");
    bool const exact = (success.flags & exact_outputs_flag) != 0;
    uint32_t version = 1;
    uint32_t sequence = 0xffffffffu;
    uint32_t locktime = 0;
    if (exact) {
        version = success.coinbase_tx_version;
        sequence = success.coinbase_tx_input_sequence;
        locktime = success.coinbase_tx_locktime;
    } else if (success.coinbase_tx_version != 0 || success.coinbase_tx_input_sequence != 0
        || success.coinbase_tx_locktime != 0) {
        return fail("payout coinbase fields are not zero");
    }
    if (success.coinbase_tx_outputs.size() > reservation_) {
        return fail("payout outputs exceed the token reservation");
    }
    auto const outputs = payout_outputs(success.coinbase_tx_outputs, template_.coinbase_value);
    if ( ! outputs) return fail("invalid payout outputs");
    work_ = mining::make_payout_work(template_, *outputs, request_id_,
        channel_.extranonce_prefix, channel_.extranonce_size, script_prefix_,
        version, sequence, locktime);
    if ( ! work_) return fail("payout coinbase or template exceeds mining limits");
    work_->upstream_channel_id = channel_.channel_id;
    work_->upstream_target = channel_.target;
    return declare_work(token_);
}

jd_declaration::result jd_declaration::refuse_payout(request_payout_outputs_error const& error) {
    // A token allocated before the latest notice is the race that notice describes.
    // One stale answer after that notice is retried; the next is a refusal.
    if (error.error_code == error_stale_payout_outputs) {
        if (token_epoch_ < notice_epoch_) return retry_allocation();
        if (unexplained_stale_ >= 1) return fail("repeated stale payout outputs");
        ++unexplained_stale_;
        return retry_allocation();
    }
    return fail("payout outputs refused: " + error.error_code);
}

jd_declaration::result jd_declaration::process_payout(jd_peer peer,
    message_header const& header, byte_span payload) {
    if (peer != jd_peer::declaration || header.channel_msg()) {
        return fail("unexpected payout message or direction");
    }
    if (state_ == state::idle && superseded_) return {};
    if (state_ != state::requesting) return fail("unexpected payout response");
    if (header.msg_type == request_payout_outputs_success::message_type) {
        auto const message = parse<request_payout_outputs_success>(payload);
        if ( ! message || message->request_id != request_id_) return fail("invalid payout outputs success");
        if (restart_) return retry_allocation();
        return accept_payout(*message);
    }
    if (header.msg_type == request_payout_outputs_error::message_type) {
        auto const message = parse<request_payout_outputs_error>(payload);
        if ( ! message || message->request_id != request_id_) return fail("invalid payout outputs error");
        if (restart_) return retry_allocation();
        return refuse_payout(*message);
    }
    return fail("unexpected payout message");
}

jd_declaration::result jd_declaration::process(jd_peer peer,
    message_header const& header, byte_span payload) {
    if (header.msg_length != payload.size() || payload.size() > jd_message_max_payload) {
        return fail("invalid declaration frame length");
    }
    if (header.extension() == requested_payout_outputs_extension) {
        if ( ! payout_outputs_) return {};
        if (header.channel_msg() || peer != jd_peer::declaration) {
            return fail("unexpected payout message or direction");
        }
        if (header.msg_type == payout_outputs_stale::message_type) return note_stale(payload);
        return process_payout(peer, header, payload);
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
        // The notice arrived while this token was being allocated, so the token
        // is already stale. Ask for another before requesting outputs.
        if (restart_) return retry_allocation();
        if (payout_outputs_) {
            return request_payout(allocation->mining_job_token, allocation->coinbase_tx_outputs.size());
        }
        auto const outputs = pool_outputs(allocation->coinbase_tx_outputs, template_.coinbase_value);
        if ( ! outputs) return fail("invalid pool coinbase outputs");
        work_ = mining::make_declared_work(template_, *outputs, request_id_,
            channel_.extranonce_prefix, channel_.extranonce_size);
        if ( ! work_) return fail("declared coinbase or template exceeds mining limits");
        work_->upstream_channel_id = channel_.channel_id;
        work_->upstream_target = channel_.target;
        return declare_work(allocation->mining_job_token);
    }
    if (state_ == state::declaring && peer == jd_peer::declaration && ! header.channel_msg()) {
        if (header.msg_type == declare_mining_job_error::message_type) {
            auto const message = parse<declare_mining_job_error>(payload);
            if ( ! message || message->request_id != request_id_) return fail("invalid declaration refusal");
            if (restart_) return retry_allocation();
            return fail("JDS refused the local template: " + message->error_code);
        }
        if (header.msg_type == declare_mining_job_success::message_type) {
            auto const message = parse<declare_mining_job_success>(payload);
            if ( ! message || message->request_id != request_id_ || message->new_mining_job_token.empty()) {
                return fail("invalid declaration success");
            }
            if (restart_) return retry_allocation();
            return install(message->new_mining_job_token);
        }
        if (header.msg_type == provide_missing_transactions::message_type) {
            auto const message = parse<provide_missing_transactions>(payload);
            if ( ! message || message->request_id != request_id_) {
                return fail("invalid missing transaction request");
            }
            // The notice already retired this job. Do not supply its transactions
            // and do not open a second request until the declaration itself ends.
            if (restart_) return {};
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
            if (restart_) return retry_allocation();
            return fail("pool refused the local custom job: " + message->error_code);
        }
        if (header.msg_type == set_custom_mining_job_success::message_type) {
            auto const message = parse<set_custom_mining_job_success>(payload);
            if ( ! message || message->request_id != request_id_ || message->channel_id != channel_.channel_id) {
                return fail("invalid custom job success");
            }
            if (restart_) return retry_allocation();
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
