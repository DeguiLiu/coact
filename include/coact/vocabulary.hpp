// coact vocabulary types: NewType, ScopeGuard, optional, FixedFunction,
// function_ref, TruncateToCapacity, FixedString, FixedVector, Expected.
// SPDX-License-Identifier: MIT
//
// Adapted from newosp include/osp/vocabulary.hpp (MIT License,
// Copyright (c) 2024 liudegui), itself inspired by iceoryx early versions
// and MCCC containers. Every type here is stack-allocated with fixed inline
// storage, zero heap and compatible with -fno-exceptions -fno-rtti - the
// coact foundation contract (see docs/cpp_coding_conventions_zh.md 2.3).
//
// Why not std:: counterparts: std::optional / std::function / std::string /
// std::vector all allocate, throw, or carry heavyweight SSO machinery.
// Each type below documents its exact std divergence at the class comment.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

#include "coact/assert.hpp"

namespace coact {

// ===========================================================================
// NewType<T, Tag> - zero-overhead strong type wrapper.
// ===========================================================================

// Strong-typed wrapper that prevents accidental mixing of semantically
// different ids sharing the same underlying representation (a TaskId and a
// TimerTaskId are both uint32_t in memory, but must never be interchangeable
// at call sites). Explicit construction and the value() accessor keep bare
// integers from silently becoming an id; comparisons only match the same
// Tag instantiation.
//
// Why not std::: the standard library has no equivalent (no std strong
// typedef); the idiom is a template alias-free class with an empty Tag.
// Zero-overhead is a static_assert away: same size, trivially copyable.
//
// Example:
//   struct SessionIdTag {};
//   using SessionId = coact::NewType<uint32_t, SessionIdTag>;
template <typename T, typename Tag>
class NewType final {
public:
    constexpr explicit NewType(T val) noexcept : val_(val) {}
    constexpr NewType() noexcept : val_() {}

    constexpr T value() const noexcept { return val_; }
    constexpr T raw() const noexcept { return val_; }
    constexpr explicit operator bool() const noexcept { return T() != val_; }

    constexpr bool operator==(NewType rhs) const noexcept { return val_ == rhs.val_; }
    constexpr bool operator!=(NewType rhs) const noexcept { return val_ != rhs.val_; }
    constexpr bool operator<(NewType rhs) const noexcept { return val_ < rhs.val_; }

private:
    T val_;
};

// ===========================================================================
// ScopeGuard - RAII exit action.
// ===========================================================================

// Runs a cleanup action on scope exit unless dismissed. The cleanup object is
// stored directly in the guard, so no allocation is introduced by the guard
// itself. Move-construction transfers the armed state (the source
// disarms), so a guard can be returned or handed to an owner; assignment
// and copy are deleted to keep the exit contract single-owner.
//
// Why not std::: there is no std scope guard (scope_exit is C++ library
// fundamentals TS, not standard C++17); the RAII decorator rule
// (conventions 7 "RAII guard") requires exactly this shape.
template <typename Cleanup>
class ScopeGuard final {
public:
    explicit ScopeGuard(Cleanup cleanup) noexcept : cleanup_(std::move(cleanup)), armed_(true) {}

    ~ScopeGuard()
    {
        if (armed_) {
            cleanup_();
        }
    }

    // Suppress the exit action: the guarded operation completed normally
    // and the cleanup must not run.
    void dismiss() noexcept { armed_ = false; }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard& operator=(ScopeGuard&&) = delete;

    // Move-construction transfers armed ownership; the source disarms so
    // exactly one guard fires the action.
    ScopeGuard(ScopeGuard&& other) noexcept
        : cleanup_(std::move(other.cleanup_)), armed_(other.armed_)
    {
        other.armed_ = false;
    }

private:
    Cleanup cleanup_;
    bool armed_;
};

// ===========================================================================
// optional<T> - lightweight nullable value.
// ===========================================================================

// Holds either a value of type T or nothing, with fixed inline storage.
// Unlike std::optional this type never throws (value() asserts instead of
// throwing bad_optional_access - coact bans exceptions), and its interface
// surface is deliberately small: construct/inspect/value_or/reset. The
// union-storage layout keeps sizeof(optional<T>) == sizeof(T) + 1 byte on
// common ABIs, matching std::optional's space, without its exception
// machinery.
//
// Why not std::optional: std::optional<T>::value() throws on empty (a
// hidden exception path under -fno-exceptions becomes std::terminate) and
// std::optional move semantics assume nothrow; this type makes the
// no-throw contract explicit through noexcept everywhere plus COACT_ASSERT.
template <typename T>
class [[nodiscard]] optional final {
    static_assert(std::is_nothrow_move_constructible<T>::value,
                  "optional value must be nothrow move constructible");
    static_assert(std::is_nothrow_destructible<T>::value,
                  "optional value must be nothrow destructible");

public:
    optional() noexcept : has_value_(false) {}

    optional(const T& val) noexcept : has_value_(true)  // NOLINT
    {
        ::new (static_cast<void*>(storage_.bytes)) T(val);
    }

    optional(T&& val) noexcept : has_value_(true)  // NOLINT
    {
        ::new (static_cast<void*>(storage_.bytes)) T(std::move(val));
    }

    optional(const optional& other) noexcept : has_value_(other.has_value_)
    {
        if (has_value_) {
            ::new (static_cast<void*>(storage_.bytes)) T(other.value());
        }
    }

    optional& operator=(const optional& other) noexcept
    {
        if (this != &other) {
            reset();
            has_value_ = other.has_value_;
            if (has_value_) {
                ::new (static_cast<void*>(storage_.bytes)) T(other.value());
            }
        }
        return *this;
    }

    optional(optional&& other) noexcept : has_value_(other.has_value_)
    {
        if (has_value_) {
            ::new (static_cast<void*>(storage_.bytes)) T(std::move(other.value()));
        }
    }

    optional& operator=(optional&& other) noexcept
    {
        if (this != &other) {
            reset();
            has_value_ = other.has_value_;
            if (has_value_) {
                ::new (static_cast<void*>(storage_.bytes)) T(std::move(other.value()));
            }
        }
        return *this;
    }

    ~optional() { reset(); }

    bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }

    T& value() noexcept
    {
        COACT_ASSERT(has_value_);
        return *std::launder(reinterpret_cast<T*>(storage_.bytes));
    }

    const T& value() const noexcept
    {
        COACT_ASSERT(has_value_);
        return *std::launder(reinterpret_cast<const T*>(storage_.bytes));
    }

    T value_or(const T& default_val) const noexcept
    {
        return has_value_ ? value() : default_val;
    }

    void reset() noexcept
    {
        if (has_value_) {
            value().~T();
            has_value_ = false;
        }
    }

private:
    struct alignas(alignof(T)) Storage {
        std::byte bytes[sizeof(T)];
    };
    Storage storage_{};
    bool has_value_{false};
};

// ===========================================================================
// FixedFunction<Sig, BufferSize> - small-buffer type-erased callable.
// ===========================================================================

template <typename Signature, size_t BufferSize = 2 * sizeof(void*)>
class FixedFunction;

// Fixed-size callable wrapper with small-buffer optimization: the erased
// callable lives inside an inline BufferSize-byte buffer, so constructing
// the function never allocates. Oversized or over-aligned callables are
// rejected at compile time by static_assert (fail loudly at build, never
// silently fall back to the heap). Move-only; calling an empty wrapper
// asserts (no exceptions).
//
// Why not std::function: std::function heap-allocates when the callable
// exceeds its internal SSO buffer, which is a hidden malloc under the
// coact zero-heap rule; std::function's operator() may also throw
// bad_function_call. Here the buffer size is a visible template parameter
// and the capacity contract is a compile error.
template <typename Ret, typename... Args, size_t BufferSize>
class FixedFunction<Ret(Args...), BufferSize> final {
public:
    FixedFunction() noexcept = default;

    // NOLINTNEXTLINE(google-explicit-constructor)
    FixedFunction(std::nullptr_t) noexcept {}

    template <typename F,
              typename = typename std::enable_if<
                  !std::is_same<typename std::decay<F>::type, FixedFunction>::value &&
                  !std::is_same<typename std::decay<F>::type, std::nullptr_t>::value>::type>
    FixedFunction(F&& f) noexcept  // NOLINT
    {
        using Decay = typename std::decay<F>::type;
        static_assert(sizeof(Decay) <= BufferSize,
                      "Callable too large for FixedFunction buffer");
        static_assert(alignof(Decay) <= alignof(Storage),
                      "Callable alignment exceeds buffer alignment");
        ::new (static_cast<void*>(storage_.bytes)) Decay(std::forward<F>(f));
        invoker_ = [](Storage& s, Args... args) -> Ret {
            return (*std::launder(reinterpret_cast<Decay*>(s.bytes)))(
                std::forward<Args>(args)...);
        };
        mover_ = [](Storage& dst, Storage& src) {
            ::new (static_cast<void*>(dst.bytes)) Decay(
                std::move(*std::launder(reinterpret_cast<Decay*>(src.bytes))));
            std::launder(reinterpret_cast<Decay*>(src.bytes))->~Decay();
        };
        destroyer_ = [](Storage& s) {
            std::launder(reinterpret_cast<Decay*>(s.bytes))->~Decay();
        };
    }

    FixedFunction(FixedFunction&& other) noexcept
        : invoker_(other.invoker_), mover_(other.mover_), destroyer_(other.destroyer_)
    {
        if (nullptr != other.mover_) {
            mover_(storage_, other.storage_);
            other.invoker_ = nullptr;
            other.mover_ = nullptr;
            other.destroyer_ = nullptr;
        }
    }

    FixedFunction& operator=(FixedFunction&& other) noexcept
    {
        if (this != &other) {
            if (nullptr != destroyer_) {
                destroyer_(storage_);
            }
            invoker_ = other.invoker_;
            mover_ = other.mover_;
            destroyer_ = other.destroyer_;
            if (nullptr != other.mover_) {
                mover_(storage_, other.storage_);
                other.invoker_ = nullptr;
                other.mover_ = nullptr;
                other.destroyer_ = nullptr;
            }
        }
        return *this;
    }

    FixedFunction& operator=(std::nullptr_t) noexcept
    {
        if (nullptr != destroyer_) {
            destroyer_(storage_);
        }
        invoker_ = nullptr;
        mover_ = nullptr;
        destroyer_ = nullptr;
        return *this;
    }

    ~FixedFunction()
    {
        if (nullptr != destroyer_) {
            destroyer_(storage_);
        }
    }

    FixedFunction(const FixedFunction&) = delete;
    FixedFunction& operator=(const FixedFunction&) = delete;

    Ret operator()(Args... args)
    {
        COACT_ASSERT(nullptr != invoker_);
        return invoker_(storage_, std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept { return nullptr != invoker_; }

private:
    struct alignas(void*) Storage {
        std::byte bytes[BufferSize];
    };
    using Invoker = Ret (*)(Storage&, Args...);
    using Mover = void (*)(Storage& dst, Storage& src);
    using Destroyer = void (*)(Storage&);

    Storage storage_{};
    Invoker invoker_ = nullptr;
    Mover mover_ = nullptr;
    Destroyer destroyer_ = nullptr;
};

// ===========================================================================
// function_ref<Sig> - non-owning callable view.
// ===========================================================================

template <typename Sig>
class function_ref;

// Non-owning lightweight callable view: exactly two words (the referenced
// callable plus a static invoker thunk), no storage, no ownership. The
// referenced callable must outlive the view. Use it for callback parameters
// (callee-into-caller calls within one call stack) where a FixedFunction
// copy would be wasteful; use FixedFunction when the callable must be
// stored.
//
// Why not std::function: std::function owns (and may allocate); passing a
// callback parameter by std::function forces an unnecessary erase-and-copy
// at every call site. function_ref is the pass-by-view complement, in the
// same spirit as std::string_view vs std::string (which itself is absent
// here because it allocates).
template <typename Ret, typename... Args>
class function_ref<Ret(Args...)> final {
public:
    template <typename F,
              typename = typename std::enable_if<!std::is_same<
                  typename std::decay<F>::type, function_ref>::value>::type>
    function_ref(F&& f) noexcept  // NOLINT
        : callable_(std::addressof(f)), invoker_(nullptr)
    {
        using Fn = typename std::remove_reference<F>::type;
        invoker_ = [](const void* obj, Args... args) -> Ret {
            return (*static_cast<const Fn*>(obj))(std::forward<Args>(args)...);
        };
    }

    function_ref(Ret (*fn)(Args...)) noexcept  // NOLINT
        : callable_(reinterpret_cast<const void*>(fn)), invoker_(nullptr)
    {
        invoker_ = [](const void* obj, Args... args) -> Ret {
            return reinterpret_cast<Ret (*)(Args...)>(obj)(
                std::forward<Args>(args)...);
        };
    }

    Ret operator()(Args... args) const
    {
        return invoker_(callable_, std::forward<Args>(args)...);
    }

private:
    const void* callable_;
    Ret (*invoker_)(const void*, Args...);
};

// ===========================================================================
// TruncateToCapacity tag.
// ===========================================================================

// Explicit opt-in tag for the silently-truncating constructors / assign /
// append overloads of FixedString and FixedVector. Data loss must be
// visible at the call site: the default operations REJECT what does not
// fit, and only this tag switches them to truncate.
struct TruncateToCapacity_t {};
inline constexpr TruncateToCapacity_t TruncateToCapacity{};

// ===========================================================================
// FixedString<Capacity> - compile-time capacity string.
// ===========================================================================

// Fixed-capacity, stack-allocated, null-terminated string. The buffer is
// Capacity+1 bytes, fully inline, so the string can live in a struct that
// crosses an AO boundary without any ownership questions. Overlong input
// is REJECTED by default; the TruncateToCapacity overloads opt into
// truncation where the caller has decided dropping bytes is correct.
//
// Why not std::string: std::string allocates when content exceeds SSO
// (15 bytes on typical ABIs) and its growth model is fundamentally heap
// driven - both banned by the coact zero-heap rule. FixedString makes the
// capacity a compile-time contract like every other coact container.
template <uint32_t Capacity>
class FixedString {
    static_assert(Capacity > 0U, "FixedString capacity must be > 0");

public:
    constexpr FixedString() noexcept : buf_{'\0'}, size_(0U) {}

    template <uint32_t N,
              typename = typename std::enable_if<(N <= Capacity + 1U)>::type>
    FixedString(const char (&str)[N]) noexcept  // NOLINT
        : size_(N - 1U)
    {
        static_assert(N > 0U, "String literal must include null terminator");
        static_assert(N - 1U <= Capacity,
                      "String literal exceeds FixedString capacity");
        (void)std::memcpy(buf_, str, N);
    }

    FixedString(TruncateToCapacity_t /*tag*/, const char* str) noexcept
        : buf_{'\0'}, size_(0U)
    {
        if (nullptr != str) {
            uint32_t i = 0U;
            while ((i < Capacity) && ('\0' != str[i])) {
                buf_[i] = str[i];
                ++i;
            }
            size_ = i;
        }
        buf_[size_] = '\0';
    }

    FixedString(TruncateToCapacity_t /*tag*/, const char* str,
                uint32_t count) noexcept
        : buf_{'\0'}, size_(0U)
    {
        if (nullptr != str) {
            size_ = (count < Capacity) ? count : Capacity;
            (void)std::memcpy(buf_, str, size_);
        }
        buf_[size_] = '\0';
    }

    [[nodiscard]] const char* c_str() const noexcept { return buf_; }
    [[nodiscard]] uint32_t size() const noexcept { return size_; }
    static constexpr uint32_t capacity() noexcept { return Capacity; }
    [[nodiscard]] bool empty() const noexcept { return 0U == size_; }

    template <uint32_t N>
    bool operator==(const FixedString<N>& rhs) const noexcept
    {
        if (size_ != rhs.size()) {
            return false;
        }
        return 0 == std::memcmp(buf_, rhs.c_str(), size_);
    }

    template <uint32_t N>
    bool operator!=(const FixedString<N>& rhs) const noexcept
    {
        return !(*this == rhs);
    }

    template <uint32_t N>
    bool operator==(const char (&str)[N]) const noexcept
    {
        if (size_ != (N - 1U)) {
            return false;
        }
        return 0 == std::memcmp(buf_, str, size_);
    }

    template <uint32_t N>
    bool operator!=(const char (&str)[N]) const noexcept
    {
        return !(*this == str);
    }

    template <uint32_t N,
              typename = typename std::enable_if<(N <= Capacity + 1U)>::type>
    FixedString& operator=(const char (&str)[N]) noexcept
    {
        static_assert(N - 1U <= Capacity,
                      "String literal exceeds FixedString capacity");
        size_ = N - 1U;
        (void)std::memcpy(buf_, str, N);
        return *this;
    }

    // Replace the content with str, silently truncated to capacity.
    FixedString& assign(TruncateToCapacity_t /*tag*/, const char* str) noexcept
    {
        size_ = 0U;
        if (nullptr != str) {
            uint32_t i = 0U;
            while ((i < Capacity) && ('\0' != str[i])) {
                buf_[i] = str[i];
                ++i;
            }
            size_ = i;
        }
        buf_[size_] = '\0';
        return *this;
    }

    // Append a null-terminated str. Returns false WITHOUT modifying the
    // string when the content does not fit (default: reject, not truncate).
    bool append(const char* str) noexcept
    {
        if (nullptr == str) {
            return false;
        }
        uint32_t len = 0U;
        while ('\0' != str[len]) {
            ++len;
        }
        if (size_ + len > Capacity) {
            return false;
        }
        (void)std::memcpy(&buf_[size_], str, len + 1U);
        size_ += len;
        return true;
    }

    void clear() noexcept
    {
        size_ = 0U;
        buf_[0] = '\0';
    }

private:
    char buf_[Capacity + 1U];
    uint32_t size_;
};

// ===========================================================================
// FixedVector<T, Capacity> - compile-time capacity vector.
// ===========================================================================

// Fixed-capacity vector with fully inline storage: no allocation ever, and
// exhaustion is an honest false return (the coact busy-reject rule) instead
// of std::vector's heap growth or std::bad_alloc. Elements are constructed
// in place in the aligned byte storage (trivially-copyable types get the
// bulk-memcpy fast path on copy). erase_unordered removes in O(1) by
// swapping in the last element - order is not preserved by design.
//
// Why not std::vector: std::vector's entire purpose is dynamic growth via
// heap allocation, which the coact zero-heap rule bans (conventions 2.3);
// its throwing at()/push_back paths are equally banned. FixedVector makes
// capacity a compile-time contract and every failure a countable bool.
template <typename T, uint32_t Capacity>
class FixedVector final {
    static_assert(Capacity > 0U, "FixedVector capacity must be > 0");
    static_assert(std::is_nothrow_move_constructible<T>::value,
                  "FixedVector element must be nothrow move constructible");

public:
    using value_type = T;
    using size_type = uint32_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = T*;
    using const_iterator = const T*;

    FixedVector() noexcept = default;
    ~FixedVector() noexcept { clear(); }

    FixedVector(const FixedVector& other) noexcept
    {
        if constexpr (std::is_trivially_copyable<T>::value) {
            /* POD fast path: bulk-copy the raw storage. */
            (void)std::memcpy(storage_, other.storage_,
                              static_cast<size_t>(other.size_) * sizeof(T));
            size_ = other.size_;
        } else {
            for (uint32_t i = 0U; i < other.size_; ++i) {
                (void)push_back(other.at_unchecked(i));
            }
        }
    }

    FixedVector& operator=(const FixedVector& other) noexcept
    {
        if (this != &other) {
            clear();
            if constexpr (std::is_trivially_copyable<T>::value) {
                (void)std::memcpy(storage_, other.storage_,
                                  static_cast<size_t>(other.size_) * sizeof(T));
                size_ = other.size_;
            } else {
                for (uint32_t i = 0U; i < other.size_; ++i) {
                    (void)push_back(other.at_unchecked(i));
                }
            }
        }
        return *this;
    }

    FixedVector(FixedVector&& other) noexcept
    {
        for (uint32_t i = 0U; i < other.size_; ++i) {
            (void)emplace_back(std::move(other.at_unchecked(i)));
        }
        other.clear();
    }

    FixedVector& operator=(FixedVector&& other) noexcept
    {
        if (this != &other) {
            clear();
            for (uint32_t i = 0U; i < other.size_; ++i) {
                (void)emplace_back(std::move(other.at_unchecked(i)));
            }
            other.clear();
        }
        return *this;
    }

    reference operator[](uint32_t index) noexcept { return at_unchecked(index); }
    const_reference operator[](uint32_t index) const noexcept
    {
        return at_unchecked(index);
    }

    reference front() noexcept { return at_unchecked(0U); }
    const_reference front() const noexcept { return at_unchecked(0U); }
    reference back() noexcept { return at_unchecked(size_ - 1U); }
    const_reference back() const noexcept { return at_unchecked(size_ - 1U); }

    pointer data() noexcept { return std::launder(reinterpret_cast<T*>(storage_)); }
    const_pointer data() const noexcept
    {
        return std::launder(reinterpret_cast<const T*>(storage_));
    }

    iterator begin() noexcept { return data(); }
    const_iterator begin() const noexcept { return data(); }
    iterator end() noexcept { return data() + size_; }
    const_iterator end() const noexcept { return data() + size_; }

    [[nodiscard]] bool empty() const noexcept { return 0U == size_; }
    [[nodiscard]] uint32_t size() const noexcept { return size_; }
    static constexpr uint32_t capacity() noexcept { return Capacity; }
    [[nodiscard]] bool full() const noexcept { return size_ >= Capacity; }

    bool push_back(const T& value) noexcept { return emplace_back(value); }
    bool push_back(T&& value) noexcept
    {
        return emplace_back(std::move(value));
    }

    template <typename... CtorArgs>
    bool emplace_back(CtorArgs&&... args) noexcept
    {
        if (size_ >= Capacity) {
            return false;
        }
        ::new (static_cast<void*>(&storage_[size_ * sizeof(T)]))
            T(std::forward<CtorArgs>(args)...);
        ++size_;
        return true;
    }

    bool pop_back() noexcept
    {
        if (0U == size_) {
            return false;
        }
        --size_;
        at_unchecked(size_).~T();
        return true;
    }

    // O(1) unordered remove: the last element replaces the removed one.
    bool erase_unordered(uint32_t index) noexcept
    {
        if (index >= size_) {
            return false;
        }
        --size_;
        if (index != size_) {
            at_unchecked(index) = std::move(at_unchecked(size_));
        }
        at_unchecked(size_).~T();
        return true;
    }

    void clear() noexcept
    {
        while (size_ > 0U) {
            --size_;
            at_unchecked(size_).~T();
        }
    }

private:
    reference at_unchecked(uint32_t index) noexcept
    {
        return *(std::launder(reinterpret_cast<T*>(storage_)) + index);
    }
    const_reference at_unchecked(uint32_t index) const noexcept
    {
        return *(std::launder(reinterpret_cast<const T*>(storage_)) + index);
    }

    /* Raw storage for element-wise placement-new. data()/begin()/end()
       treat the laundered head as a T array for iteration; strictly the
       element objects do not form a T[] (CWG 2182), but this is the
       storage pattern libstdc++ used pre-P0593 and every supported
       compiler treats it as defined. Do not use data() for memcpy of
       the WHOLE vector - element-wise copy only. */
    alignas(T) unsigned char storage_[sizeof(T) * Capacity];
    uint32_t size_{0U};
};

// Expected<T, E> - fixed-storage error-or-value result.
template <typename V, typename E>
class [[nodiscard]] Expected final {
    static_assert(std::is_nothrow_move_constructible<V>::value,
                  "Expected value must be nothrow move constructible");
    static_assert(std::is_nothrow_destructible<V>::value,
                  "Expected value must be nothrow destructible");

public:
    static Expected success(const V& val) noexcept
    {
        static_assert(std::is_nothrow_copy_constructible<V>::value,
                      "Expected copied value must be nothrow copy constructible");
        Expected e;
        e.has_value_ = true;
        ::new (static_cast<void*>(e.storage_.bytes)) V(val);
        return e;
    }

    static Expected success(V&& val) noexcept
    {
        Expected e;
        e.has_value_ = true;
        ::new (static_cast<void*>(e.storage_.bytes)) V(std::move(val));
        return e;
    }

    static Expected error(E err) noexcept
    {
        Expected e;
        e.has_value_ = false;
        e.err_ = err;
        return e;
    }

    Expected(Expected&& other) noexcept
        : storage_{}, err_(other.err_), has_value_(false)
    {
        if (other.has_value_) {
            ::new (static_cast<void*>(storage_.bytes)) V(std::move(other.value()));
            has_value_ = true;
            other.destroy_value();
        }
    }

    Expected& operator=(Expected&& other) noexcept
    {
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

    ~Expected()
    {
        if (has_value_) {
            destroy_value();
        }
    }

    bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }

    V& value() & noexcept
    {
        COACT_ASSERT(has_value_);
        return *value_ptr();
    }

    const V& value() const& noexcept
    {
        COACT_ASSERT(has_value_);
        return *value_ptr();
    }

    E error() const noexcept
    {
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

    void destroy_value() noexcept
    {
        value_ptr()->~V();
        (void)std::exchange(has_value_, false);
    }
};

template <typename E>
class [[nodiscard]] Expected<void, E> final {
public:
    static Expected success() noexcept
    {
        Expected e;
        e.has_value_ = true;
        return e;
    }

    static Expected error(E err) noexcept
    {
        Expected e;
        e.has_value_ = false;
        e.err_ = err;
        return e;
    }

    bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }

    E error() const noexcept
    {
        COACT_ASSERT(!has_value_);
        return err_;
    }

private:
    Expected() noexcept : err_{}, has_value_(false) {}

    E err_{};
    bool has_value_{false};
};

}  // namespace coact
