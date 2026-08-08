// Minimal hierarchical state machine (HSM) for coact.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "coact/config.hpp"
#include "coact/event.hpp"

namespace coact {

// ---------------------------------------------------------------------------
// Transition kind. External leaves the active subtree and enters the target;
// Internal runs only the action; Self exits the active subtree up to the
// source, runs the action, then re-enters the source.
// ---------------------------------------------------------------------------
enum class TransitionKind : uint8_t {
    External,
    Internal,
    Self
};

template <typename Context>
struct StateDef {
    int8_t parent;                 // parent state index; 0 is the root; -1 = no parent
    void (*entry)(Context&);       // entry action, may be null
    void (*exit)(Context&);        // exit action, may be null
};

template <typename Context>
struct TransitionDef {
    int8_t source;                 // source state index; 0 is the root; no wildcard
    uint16_t signal;
    int8_t target;                 // target state index (used by External)
    TransitionKind kind;
    bool (*guard)(const Context&, const Event&);  // may be null (pass)
    void (*action)(Context&, const Event&);       // may be null (no-op)
};

// ---------------------------------------------------------------------------
// Run-time HSM over application-provided static tables. The Hsm stores only
// addresses, counts, the initial state and the maximum depth: it never copies
// the tables and never allocates. Dispatch resolves (state, signal) from the
// active leaf upward, bounded by max_depth parent hops.
// ---------------------------------------------------------------------------
template <typename Context>
class Hsm {
public:
    Hsm(const StateDef<Context>* states, uint16_t num_states,
        const TransitionDef<Context>* transitions, uint16_t num_transitions,
        int8_t initial_state, uint8_t max_depth) noexcept;

    void init(Context& ctx, const Event& evt) noexcept;   // enter initial_state, root first
    bool dispatch(Context& ctx, const Event& evt) noexcept;  // handled?
    int8_t current_state() const noexcept;

private:
    int8_t parent_of(int8_t state) const noexcept;
    int8_t chain_depth(int8_t state) const noexcept;
    int8_t ancestor_at_depth(int8_t state, int8_t depth) const noexcept;
    int8_t find_lca(int8_t a, int8_t b) const noexcept;
    const TransitionDef<Context>* find_transition(
        Context& ctx, const Event& evt, int8_t source) const noexcept;
    void execute_transition(Context& ctx, const Event& evt, int8_t source,
                            const TransitionDef<Context>& tran) noexcept;
    void exit_to_lca(Context& ctx, int8_t lca) noexcept;
    void enter_path(Context& ctx, int8_t lca, int8_t target) noexcept;

    const StateDef<Context>* states_;
    uint16_t num_states_;
    const TransitionDef<Context>* transitions_;
    uint16_t num_transitions_;
    int8_t initial_state_;
    uint8_t max_depth_;
    int8_t current_;               // -1 = not initialized
};

// ===========================================================================
// Inline definitions
// ===========================================================================

template <typename Context>
Hsm<Context>::Hsm(const StateDef<Context>* states, uint16_t num_states,
                  const TransitionDef<Context>* transitions,
                  uint16_t num_transitions, int8_t initial_state,
                  uint8_t max_depth) noexcept
    : states_(states),
      num_states_(num_states),
      transitions_(transitions),
      num_transitions_(num_transitions),
      initial_state_(initial_state),
      max_depth_(max_depth),
      current_(-1) {}

template <typename Context>
void Hsm<Context>::init(Context& ctx, const Event& evt) noexcept {
    (void)evt;
    if (initial_state_ < 0 || initial_state_ >= static_cast<int8_t>(num_states_)) {
        return;
    }
    // Enter along the parent chain from the root down to the initial state.
    enter_path(ctx, -1, initial_state_);
    current_ = initial_state_;
}

template <typename Context>
bool Hsm<Context>::dispatch(Context& ctx, const Event& evt) noexcept {
    if (current_ < 0) {
        return false;
    }
    int8_t state = current_;
    uint8_t hops = 0;
    for (;;) {
        const TransitionDef<Context>* tran = find_transition(ctx, evt, state);
        if (tran != nullptr) {
            execute_transition(ctx, evt, state, *tran);
            return true;
        }
        const int8_t parent = states_[state].parent;
        if (parent < 0) {
            return false;                       // root did not accept the event
        }
        if (hops >= max_depth_) {
            return false;                       // depth bound reached, stop safely
        }
        state = parent;
        ++hops;
    }
}

template <typename Context>
int8_t Hsm<Context>::current_state() const noexcept {
    return current_;
}

template <typename Context>
int8_t Hsm<Context>::parent_of(int8_t state) const noexcept {
    return states_[state].parent;
}

template <typename Context>
int8_t Hsm<Context>::chain_depth(int8_t state) const noexcept {
    int8_t depth = 0;
    while (state >= 0) {
        ++depth;
        state = parent_of(state);
    }
    return depth;
}

template <typename Context>
int8_t Hsm<Context>::ancestor_at_depth(int8_t state, int8_t depth) const noexcept {
    int8_t d = chain_depth(state);
    while (d > depth) {
        state = parent_of(state);
        --d;
    }
    return state;
}

template <typename Context>
int8_t Hsm<Context>::find_lca(int8_t a, int8_t b) const noexcept {
    int8_t da = chain_depth(a);
    int8_t db = chain_depth(b);
    while (da > db) {
        a = parent_of(a);
        --da;
    }
    while (db > da) {
        b = parent_of(b);
        --db;
    }
    while (a != b) {
        if (a < 0 || b < 0) {
            return -1;                          // disjoint trees, no LCA
        }
        a = parent_of(a);
        b = parent_of(b);
    }
    return a;
}

template <typename Context>
const TransitionDef<Context>* Hsm<Context>::find_transition(
    Context& ctx, const Event& evt, int8_t source) const noexcept {
    for (uint16_t i = 0U; i < num_transitions_; ++i) {
        const TransitionDef<Context>& tran = transitions_[i];
        if (tran.source == source && tran.signal == evt.signal) {
            if (tran.guard != nullptr && !tran.guard(ctx, evt)) {
                continue;                       // guard failed, try next candidate
            }
            return &tran;
        }
    }
    return nullptr;
}

template <typename Context>
void Hsm<Context>::execute_transition(
    Context& ctx, const Event& evt, int8_t source,
    const TransitionDef<Context>& tran) noexcept {
    switch (tran.kind) {
        case TransitionKind::Internal:
            if (tran.action != nullptr) {
                tran.action(ctx, evt);
            }
            break;

        case TransitionKind::Self: {
            // Exit from the active leaf up to and including the source, run
            // the action, then re-enter the source.
            int8_t state = current_;
            for (;;) {
                if (states_[state].exit != nullptr) {
                    states_[state].exit(ctx);
                }
                if (state == source) {
                    break;
                }
                state = parent_of(state);
            }
            if (tran.action != nullptr) {
                tran.action(ctx, evt);
            }
            if (states_[source].entry != nullptr) {
                states_[source].entry(ctx);
            }
            current_ = source;
            break;
        }

        case TransitionKind::External: {
            const int8_t target = tran.target;
            const int8_t lca = find_lca(current_, target);
            exit_to_lca(ctx, lca);              // exit up to (not including) LCA
            if (tran.action != nullptr) {
                tran.action(ctx, evt);
            }
            enter_path(ctx, lca, target);       // enter LCA's child down to target
            current_ = target;
            break;
        }

        default:
            break;
    }
}

template <typename Context>
void Hsm<Context>::exit_to_lca(Context& ctx, int8_t lca) noexcept {
    int8_t state = current_;
    while (state != lca) {
        if (states_[state].exit != nullptr) {
            states_[state].exit(ctx);
        }
        state = parent_of(state);
    }
}

template <typename Context>
void Hsm<Context>::enter_path(Context& ctx, int8_t lca, int8_t target) noexcept {
    const int8_t lca_depth = chain_depth(lca);     // chain_depth(-1) == 0
    const int8_t target_depth = chain_depth(target);
    for (int8_t level = lca_depth + 1; level <= target_depth; ++level) {
        const int8_t state = ancestor_at_depth(target, level);
        if (states_[state].entry != nullptr) {
            states_[state].entry(ctx);
        }
    }
}

}  // namespace coact
