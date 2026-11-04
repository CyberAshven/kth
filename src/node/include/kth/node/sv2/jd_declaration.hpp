// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef KTH_NODE_SV2_JD_DECLARATION_HPP
#define KTH_NODE_SV2_JD_DECLARATION_HPP

#include <cstdint>
#include <optional>
#include <string>

#include <kth/node/mining/work.hpp>
#include <kth/node/sv2/jd_messages.hpp>
#include <kth/node/sv2/jd_session.hpp>

namespace kth::node::sv2 {

// One outstanding local selection. The owner supplies deadlines and queues
// complete frames; neither a pool reply nor missing transaction data can wait
// on a downstream miner or the block connection path.
struct jd_declaration {
    struct result {
        data_chunk mining;
        data_chunk declaration;
        bool progress = false;
        bool accepted = false;
        std::string error;
    };

    jd_declaration(std::string identity, bool full_template);

    [[nodiscard]] result start(blockchain::mining_template value,
        open_extended_mining_channel_success const& channel);
    [[nodiscard]] result process(jd_peer peer, message_header const& header, byte_span payload);
    // Ignored once a declaration is in flight. The attempt keeps the choice it started with.
    void use_payout_outputs(bool enabled);
    [[nodiscard]] bool pending() const;
    [[nodiscard]] std::optional<mining::declared_work> const& work() const;
    // The template last given to start(), kept across a refusal so fallback
    // can pay the solo address from the same selection.
    [[nodiscard]] bool has_template() const;
    [[nodiscard]] blockchain::mining_template const& template_value() const;
    // Full-template only, and only after DeclareMiningJob.Success. Spec 6.4.9
    // does not define PushSolution for coinbase-only.
    [[nodiscard]] bool can_push_solution() const;
    // Drop an in-flight declaration whose tip is no longer current. A late
    // reply is ignored instead of failing the upstream.
    void supersede();
    void note_target(hash_digest const& target);

private:
    enum class state { idle, allocating, requesting, declaring, installing, accepted, failed };

    [[nodiscard]] result fail(std::string reason);
    [[nodiscard]] result install(data_chunk token);
    [[nodiscard]] result send_allocation();
    [[nodiscard]] result retry_allocation();
    [[nodiscard]] result request_payout(data_chunk token, size_t reservation);
    [[nodiscard]] result declare_work(data_chunk token);
    [[nodiscard]] result note_stale(byte_span payload);
    [[nodiscard]] result process_payout(jd_peer peer, message_header const& header, byte_span payload);
    [[nodiscard]] result accept_payout(request_payout_outputs_success const& success);
    [[nodiscard]] result refuse_payout(request_payout_outputs_error const& error);

    std::string identity_;
    bool full_template_;
    bool has_template_ = false;
    bool superseded_ = false;
    bool payout_outputs_ = false;
    bool restart_ = false;
    state state_ = state::idle;
    uint32_t request_id_ = 0;
    uint32_t notice_epoch_ = 0;
    uint32_t token_epoch_ = 0;
    uint32_t unexplained_stale_ = 0;
    uint32_t restarts_ = 0;
    size_t reservation_ = 0;
    blockchain::mining_template template_{};
    open_extended_mining_channel_success channel_;
    std::optional<mining::declared_work> work_;
    data_chunk token_;
    data_chunk script_prefix_;
    std::vector<data_chunk> transactions_;
};

} // namespace kth::node::sv2

#endif // KTH_NODE_SV2_JD_DECLARATION_HPP
