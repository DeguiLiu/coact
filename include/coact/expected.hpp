// coact Expected<T, E> - lightweight error-or-value type.
// SPDX-License-Identifier: MIT
//
// Adapted from newosp include/osp/vocabulary.hpp (MIT License,
// Copyright (c) 2024 liudegui), itself inspired by iceoryx early versions.
// Requirements per design 14.2: supports move-only T, provides Expected<void,E>
// specialization, fixed inline storage, no exceptions, no heap.
#pragma once

#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "coact/assert.hpp"

namespace coact {

template <typename V, typename E>
class Expected final {
public:
    static Expected success(const V& val) noexcept {
        Expected e;
        e.has_value_ = true;
        ::new (&e.storage_) V(val);
        return e;
    }

    static Expected success(V&& val) noexcept {
        Expected e;
        e.has_value_ = true;
        ::new (&e.storage_) V(static_cast<V&&>(val));
        return e;
    }

    static Expected error(E err) noexcept {
        Expected e;
        e.has_value_ = false;
        e.err_ = err;
        return e;
    }

    // Move-only: the value payload must be movable; copying is intentionally
    // disabled so move-only payloads (e.g. UniqueEvent) are well-formed.
    Expected(Expected&& other) noexcept
        : storage_{}, err_(other.err_), has_value_(other.has_value_) {
        if (has_value_) {
            ::new (&storage_) V(static_cast<V&&>(other.value()));
        }
    }

    Expected& operator=(Expected&& other) noexcept {
        if (this != &other) {
            if (has_value_) {
                reinterpret_cast<V*>(&storage_)->~V();
            }
            has_value_ = other.has_value_;
            err_ = other.err_;
            if (has_value_) {
                ::new (&storage_) V(static_cast<V&&>(other.value()));
            }
        }
        return *this;
    }

    Expected(const Expected&) = delete;
    Expected& operator=(const Expected&) = delete;

    ~Expected() {
        if (has_value_) {
            reinterpret_cast<V*>(&storage_)->~V();
        }
    }

    bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }

    V& value() & noexcept {
        COACT_ASSERT(has_value_);
        return *reinterpret_cast<V*>(&storage_);
    }

    const V& value() const& noexcept {
        COACT_ASSERT(has_value_);
        return *reinterpret_cast<const V*>(&storage_);
    }

    E error() const noexcept {
        COACT_ASSERT(!has_value_);
        return err_;
    }

private:
    Expected() noexcept : storage_{}, err_{}, has_value_(false) {}

    typename std::aligned_storage<sizeof(V), alignof(V)>::type storage_{};
    E err_{};
    bool has_value_{false};
};

template <typename E>
class Expected<void, E> final {
public:
    static Expected success() noexcept {
        Expected e;
        e.has_value_ = true;
        return e;
    }

    static Expected error(E err) noexcept {
        Expected e;
        e.has_value_ = false;
        e.err_ = err;
        return e;
    }

    bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }

    E error() const noexcept {
        COACT_ASSERT(!has_value_);
        return err_;
    }

private:
    Expected() noexcept : err_{}, has_value_(false) {}

    E err_{};
    bool has_value_{false};
};

}  // namespace coact
