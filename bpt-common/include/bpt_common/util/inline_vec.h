#pragma once

/// \file
/// \brief Fixed-capacity vector with inline storage. No heap allocation, ever.
///
/// Drop-in for std::vector<T> when the maximum element count is bounded
/// at compile time and known to be small. Use case: short, hot-path lists
/// rebuilt per call (e.g. order-book ladders up to depth N) where
/// std::vector's per-call malloc/free shows up in profiles.
///
/// Element type T must be default-constructible — backing storage is a
/// value array, not raw bytes. (Trade-off: simplicity over generality;
/// if you need non-default-constructible elements, reach for
/// boost::container::small_vector.)
///
/// emplace_back must not be called when size() == N. The class does not
/// guard against overflow — callers are responsible for the bound. This
/// matches std::vector's "you reserve enough or you overflow" contract,
/// minus the safety net of dynamic growth.

#include <cstddef>
#include <initializer_list>
#include <utility>

namespace bpt::common::util {

template <typename T, std::size_t N>
class InlineVec {
public:
    InlineVec() = default;
    InlineVec(std::initializer_list<T> il) { assign(il); }

    InlineVec& operator=(std::initializer_list<T> il) {
        assign(il);
        return *this;
    }

    void assign(std::initializer_list<T> il) {
        size_ = 0;
        for (const auto& v : il)
            data_[size_++] = v;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }

    void clear() noexcept { size_ = 0; }

    /// No-op — capacity is fixed at N. Provided for std::vector API
    /// symmetry so callers that reserve(depth) compile unchanged.
    void reserve(std::size_t /*n*/) noexcept {}

    template <typename... Args>
    T& emplace_back(Args&&... args) {
        data_[size_] = T(std::forward<Args>(args)...);
        return data_[size_++];
    }

    void push_back(const T& v) { data_[size_++] = v; }
    void push_back(T&& v) { data_[size_++] = std::move(v); }

    T& operator[](std::size_t i) noexcept { return data_[i]; }
    const T& operator[](std::size_t i) const noexcept { return data_[i]; }

    T& front() noexcept { return data_[0]; }
    const T& front() const noexcept { return data_[0]; }
    T& back() noexcept { return data_[size_ - 1]; }
    const T& back() const noexcept { return data_[size_ - 1]; }

    T* begin() noexcept { return data_; }
    T* end() noexcept { return data_ + size_; }
    const T* begin() const noexcept { return data_; }
    const T* end() const noexcept { return data_ + size_; }
    const T* cbegin() const noexcept { return data_; }
    const T* cend() const noexcept { return data_ + size_; }

private:
    T data_[N]{};
    std::size_t size_{0};
};

}  // namespace bpt::common::util
