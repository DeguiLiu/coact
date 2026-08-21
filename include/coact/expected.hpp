// coact Expected<T, E> - lightweight error-or-value type.
// SPDX-License-Identifier: MIT
//
// Adapted from newosp include/osp/vocabulary.hpp (MIT License,
// Copyright (c) 2024 liudegui), itself inspired by iceoryx early versions.
// Requirements per design 14.2: supports move-only T, provides Expected<void,E>
// specialization, fixed inline storage, no exceptions, no heap.
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "coact/assert.hpp"

namespace coact {

template <typename V, typename E>
class [[nodiscard]] Expected final {
    static_assert(std::is_nothrow_move_constructible<V>::value,
                  "Expected value must be nothrow move constructible");
    static_assert(std::is_nothrow_destructible<V>::value,
                  "Expected value must be nothrow destructible");

public:
    static Expected success(const V& val) noexcept {
        static_assert(std::is_nothrow_copy_constructible<V>::value,
                      "Expected copied value must be nothrow copy constructible");
        Expected e;
        e.has_value_ = true;
        ::new (static_cast<void*>(e.storage_.bytes)) V(val);
        return e;
    }

    static Expected success(V&& val) noexcept {
        Expected e;
        e.has_value_ = true;
        ::new (static_cast<void*>(e.storage_.bytes)) V(std::move(val));
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
        : storage_{}, err_(other.err_), has_value_(false) {
        if (other.has_value_) {
            ::new (static_cast<void*>(storage_.bytes)) V(std::move(other.value()));
            has_value_ = true;
            other.destroy_value();
        }
    }

    Expected& operator=(Expected&& other) noexcept {
        if (this != &other) {
            if (has_value_) {
                destroy_value();
            }
            err_ = other.err_;
            if (other.has_value_) {
                ::new (static_cast<void*>(storage_.bytes)) V(std::move(other.value()));
                has_value_ = true;
                other.destroy_value();
            }
        }
        return *this;
    }

    Expected(const Expected&) = delete;
    Expected& operator=(const Expected&) = delete;

    ~Expected() {
        if (has_value_) {
            destroy_value();
        }
    }

    bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }

    V& value() & noexcept {
        COACT_ASSERT(has_value_);
        return *value_ptr();
    }

    const V& value() const& noexcept {
        COACT_ASSERT(has_value_);
        return *value_ptr();
    }

    E error() const noexcept {
        COACT_ASSERT(!has_value_);
        return err_;
    }

private:
    Expected() noexcept : storage_{}, err_{}, has_value_(false) {}

    struct alignas(alignof(V)) Storage {
        std::byte bytes[sizeof(V)];
    };
    Storage storage_{};
    E err_{};
    bool has_value_{false};

    V* value_ptr() noexcept
    {
        return std::launder(reinterpret_cast<V*>(storage_.bytes));
    }

    const V* value_ptr() const noexcept
    {
        return std::launder(reinterpret_cast<const V*>(storage_.bytes));
    }

    void destroy_value() noexcept {
        value_ptr()->~V();
        (void)std::exchange(has_value_, false);
    }
};

template <typename E>
class [[nodiscard]] Expected<void, E> final {
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
