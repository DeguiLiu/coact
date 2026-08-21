// Minimal hierarchical state machine (HSM) for coact.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "coact/assert.hpp"
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
    const char* name = nullptr;    // debug label; may be null
    int8_t initial_child = -1;     // direct initial child; -1 means this state is a leaf
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

    // Debug label of the active state, or nullptr when not initialized / the
    // state carries no name. Zero-cost: only reads the static table slot.
    const char* current_state_name() const noexcept;

private:
    void validate_topology() const noexcept;
    int8_t parent_of(int8_t state) const noexcept;
    uint16_t chain_depth(int8_t state) const noexcept;
    int8_t ancestor_at_depth(int8_t state, uint16_t depth) const noexcept;
    int8_t find_lca(int8_t a, int8_t b) const noexcept;
    const TransitionDef<Context>* find_transition(
        Context& ctx, const Event& evt, int8_t source) const noexcept;
    void execute_transition(Context& ctx, const Event& evt, int8_t source,
                            const TransitionDef<Context>& tran) noexcept;
    void exit_to_lca(Context& ctx, int8_t lca) noexcept;
    void enter_path(Context& ctx, int8_t lca, int8_t target) noexcept;
    int8_t enter_initial_descendants(Context& ctx, int8_t state) noexcept;

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
      current_(-1) {
    validate_topology();
}

template <typename Context>
void Hsm<Context>::validate_topology() const noexcept {
    COACT_ASSERT(num_states_ <= 128U);
    COACT_ASSERT((0U == num_states_) || (nullptr != states_));
    for (uint16_t index = 0U; index < num_states_; ++index) {
        COACT_ASSERT(states_[index].parent >= -1);
        const int8_t child = states_[index].initial_child;
        COACT_ASSERT(child >= -1);
        if (child >= 0) {
            COACT_ASSERT(static_cast<uint16_t>(child) < num_states_);
            COACT_ASSERT(states_[child].parent == static_cast<int8_t>(index));
        }
        int8_t state = static_cast<int8_t>(index);
        uint16_t hops = 0U;
        while (state >= 0) {
            COACT_ASSERT(static_cast<uint16_t>(state) < num_states_);
            COACT_ASSERT(hops < num_states_);
            state = states_[state].parent;
            ++hops;
        }
    }
}

template <typename Context>
void Hsm<Context>::init(Context& ctx, const Event& evt) noexcept {
    (void)evt;
    if (initial_state_ < 0 ||
        static_cast<uint16_t>(initial_state_) >= num_states_) {
        return;
    }
    // Enter along the parent chain from the root down to the initial state.
    enter_path(ctx, -1, initial_state_);
    current_ = enter_initial_descendants(ctx, initial_state_);
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
const char* Hsm<Context>::current_state_name() const noexcept {
    if (current_ < 0 || static_cast<uint16_t>(current_) >= num_states_) {
        return nullptr;
    }
    return states_[current_].name;
}

template <typename Context>
int8_t Hsm<Context>::parent_of(int8_t state) const noexcept {
    return states_[state].parent;
}

template <typename Context>
uint16_t Hsm<Context>::chain_depth(int8_t state) const noexcept {
    uint16_t depth = 0U;
    while (state >= 0) {
        ++depth;
        state = parent_of(state);
    }
    return depth;
}

template <typename Context>
int8_t Hsm<Context>::ancestor_at_depth(int8_t state, uint16_t depth) const noexcept {
    uint16_t current_depth = chain_depth(state);
    while (current_depth > depth) {
        state = parent_of(state);
        --current_depth;
    }
    return state;
}

template <typename Context>
int8_t Hsm<Context>::find_lca(int8_t a, int8_t b) const noexcept {
    uint16_t da = chain_depth(a);
    uint16_t db = chain_depth(b);
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
            current_ = enter_initial_descendants(ctx, source);
            break;
        }

        case TransitionKind::External: {
            const int8_t target = tran.target;
            COACT_ASSERT(target >= 0);
            COACT_ASSERT(static_cast<uint16_t>(target) < num_states_);
            int8_t lca = find_lca(source, target);
            // External semantics leave and re-enter a boundary state when it
            // is the declared source or the target. Use its parent as the
            // path boundary so composite source -> descendant transitions do
            // not degrade into local transitions.
            if ((lca == source) || (lca == target)) {
                lca = parent_of(lca);
            }
            exit_to_lca(ctx, lca);              // exit up to (not including) LCA
            if (tran.action != nullptr) {
                tran.action(ctx, evt);
            }
            enter_path(ctx, lca, target);       // enter LCA's child down to target
            current_ = enter_initial_descendants(ctx, target);
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
    const uint16_t lca_depth = chain_depth(lca);     // chain_depth(-1) == 0
    const uint16_t target_depth = chain_depth(target);
    for (uint16_t level = lca_depth + 1U; level <= target_depth; ++level) {
        const int8_t state = ancestor_at_depth(target, level);
        if (states_[state].entry != nullptr) {
            states_[state].entry(ctx);
        }
    }
}

template <typename Context>
int8_t Hsm<Context>::enter_initial_descendants(Context& ctx, int8_t state) noexcept {
    uint8_t hops = 0U;
    for (;;) {
        const int8_t child = states_[state].initial_child;
        if (child < 0) {
            break;
        }
        COACT_ASSERT(static_cast<uint16_t>(child) < num_states_);
        COACT_ASSERT(states_[child].parent == state);
        COACT_ASSERT(hops < max_depth_);
        if (states_[child].entry != nullptr) {
            states_[child].entry(ctx);
        }
        state = child;
        ++hops;
    }
    return state;
}

}  // namespace coact
