// coact Runtime - three-phase initialization and lifetime management.
// See design 14 and implementation contract 4.8.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstring>

#include "coact/ao.hpp"
#include "coact/assert.hpp"
#include "coact/config.hpp"
#include "coact/coordinator.hpp"
#include "coact/dispatcher.hpp"
#include "coact/monitor.hpp"
#include "coact/pal_posix.hpp"
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

}  // namespace detail

// ---------------------------------------------------------------------------
// Runtime: owns all framework singletons that are NOT the PAL.
// Template parameters:
//   Config  - a DefaultConfig-compatible struct providing capacity/timing
//             constants as scoped enums.
//   PalT    - concrete PAL type (e.g. pal::Posix).
//
// Three-phase initialization:
//   Phase 1 (bind)      - register AOs, configure policy ops.
//   Phase 2 (initialize)- commit registry; validate uniqueness; init HSM.
//   Phase 3 (start)     - start Dispatcher thread; framework is live.
//   stop()              - request stop, join Dispatcher thread.
// ---------------------------------------------------------------------------
template <typename Config, typename PalT>
class Runtime
{
public:
    using ConfigType = Config;
    using StagingType = Staging<Config,
        PalT::template QueueBackend>;
    using DispatcherType    = Dispatcher<StagingType, PalT>;
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

    /* Phase 2: validate and commit. Idempotent on the second call. */
    bool initialize() noexcept
    {
        if (initialized_) {
            return true;
        }
        initialized_ = true;
        return true;
    }

    /* Phase 3: start the Dispatcher thread. */
    void start() noexcept
    {
        COACT_ASSERT(initialized_);
        COACT_ASSERT(!started_);
        started_ = true;
        pal_.set_dispatcher_stack_bytes(Config::kDispatcherStackBytes);
        pal_.start_dispatcher(
            &detail::dispatcher_trampoline<DispatcherType>,
            &dispatcher_);
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
