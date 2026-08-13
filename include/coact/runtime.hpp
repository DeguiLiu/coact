// coact Runtime - three-phase initialization and lifetime management.
// See design 14 and implementation contract 4.8.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "coact/ao.hpp"
#include "coact/assert.hpp"
#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/dispatcher.hpp"
#include "coact/monitor.hpp"
#include "coact/pool.hpp"
#include "coact/queue.hpp"
#include "coact/staging.hpp"

namespace coact {

namespace detail {

/* ThreadEntry trampoline so Dispatcher::run() can be passed to PAL. */
template <typename DispatcherT>
void dispatcher_trampoline(void* ctx) noexcept
{
    static_cast<DispatcherT*>(ctx)->run();
}

/* Starts the PAL dispatcher thread, propagating a status when the PAL reports
   one (RtThread returns pal::InitError, design §7.5). Posix keeps void: the
   if-constexpr discards the status branch so Posix still compiles. */
template <typename PalT>
inline bool pal_start_dispatcher(PalT& pal, pal::ThreadEntry entry,
                                 void* ctx) noexcept
{
    if constexpr (std::is_same<decltype(pal.start_dispatcher(entry, ctx)),
                               void>::value) {
        pal.start_dispatcher(entry, ctx);
        return true;
    }
    else {
        /* Status-returning PAL (RtThread, design §7.5): success == the enum's
           0 value. decltype(status) keeps the name dependent so this branch is
           only instantiated for PALs that actually return a status. */
        auto status = pal.start_dispatcher(entry, ctx);
        return status == decltype(status){0};
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Runtime: owns all framework singletons that are NOT the PAL.
// Template parameters:
//   Config  - a DefaultConfig-compatible struct providing capacity/timing
//             constants as scoped enums.
//   PalT    - concrete PAL type (e.g. pal::Posix, pal::RtThread).
//   Profile - board pool/staging/reclaimer/monitor profile (design §7.4).
//             Default HostSmpProfile keeps batched reclaim on host/POSIX.
//             Single-core RT-Thread boards pass pal::RtThread::Profile
//             (RttSingleCoreProfile), which the Dispatcher forwards to select
//             ImmediateReclaimer.
//
// Three-phase initialization:
//   Phase 1 (bind)      - register AOs, configure policy ops.
//   Phase 2 (initialize)- commit registry; validate uniqueness; init HSM.
//   Phase 3 (start)     - start Dispatcher thread; framework is live. Returns
//                         bool; started_ is set ONLY after the PAL reports a
//                         successful dispatcher start (design §7.5).
//   stop()              - request stop, join Dispatcher thread.
// ---------------------------------------------------------------------------
template <typename Config, typename PalT,
          typename Profile = coact::HostSmpProfile>
class Runtime
{
public:
    using ConfigType = Config;
    using StagingType = Staging<Config,
        PalT::template QueueBackend>;
    using DispatcherType    = Dispatcher<StagingType, PalT, Profile>;
    using CoordinatorType   = DispatchCoordinator<StagingType, PalT>;

    explicit Runtime(PalT& pal) noexcept
        : pal_(pal),
          cfg_{},
          staging_(make_critical_section(pal_)),
          dispatcher_(staging_, registry_, monitor_, breaker_, pal_),
          coordinator_(staging_, registry_, monitor_, breaker_, pal_),
          initialized_(false),
          started_(false)
    {
    }

    /* Phase 1: register an AO. Returns false if registry is full or prio
       conflicts; must be called before initialize(). */
    bool bind(AoBase* ao) noexcept
    {
        if (initialized_) {
            return false;  /* too late */
        }
        if (nullptr == ao) {
            return false;
        }
        return registry_.bind(ao, ao->logical_prio());
    }

    // Phase 1 variant for a board's constexpr domain table. The caller owns
    // the stable target assignment; Runtime still prevents changes after
    // initialize() and AoRegistry validates target/prio uniqueness.
    bool bind_at(TargetId target, AoBase& ao) noexcept
    {
        if (initialized_) {
            return false;
        }
        return registry_.bind_at(target, ao, ao.logical_prio());
    }

    /* Phase 2: validate and commit. Idempotent on the second call. */
    bool initialize() noexcept
    {
        if (initialized_) {
            return true;
        }
        initialized_ = true;
        return true;
    }

    /* Phase 3: start the Dispatcher thread. Returns false (leaving started_
       false) when the PAL reports a definite init/start error - the Runtime
       never enters started on a PAL failure (design §7.5). */
    bool start() noexcept
    {
        COACT_ASSERT(initialized_);
        if (started_) {
            return false;
        }
        pal_.set_dispatcher_stack_bytes(Config::kDispatcherStackBytes);
        const bool ok = detail::pal_start_dispatcher(
            pal_, &detail::dispatcher_trampoline<DispatcherType>, &dispatcher_);
        if (!ok) {
            return false;
        }
        started_ = true;
        return true;
    }

    void stop() noexcept
    {
        if (started_) {
            dispatcher_.request_stop();
            pal_.join_dispatcher();
            started_ = false;
        }
    }

    CoordinatorType& coordinator() noexcept { return coordinator_; }
    Monitor<Config>& monitor()     noexcept { return monitor_; }
    Breaker<Config>& breaker()     noexcept { return breaker_; }

private:
    PalT&           pal_;
    ConfigType      cfg_;
    AoRegistry<Config> registry_;
    Monitor<Config> monitor_;
    Breaker<Config> breaker_{cfg_};
    StagingType     staging_;
    DispatcherType  dispatcher_;
    CoordinatorType coordinator_;
    bool            initialized_;
    bool            started_;
};

}  // namespace coact
