#pragma once
// B-2 IR - small-vector with inline storage (Rule 19).
//
// WHY THIS FILE EXISTS:
// Law 19 bans std::vector for data that usually has 1 to 4 elements - use-def
// chains, instruction operands, predecessor lists, guard dependency lists.
// This is the IR's SmallVector<T, N>. Element types are restricted to
// trivially-copyable value types (NodeId, Use records, small ids) so the
// inline storage never needs non-trivial construction or destruction and the
// growth path is a plain memcpy into pmr memory (Rule 7: arena allocation,
// no per-element malloc in the compiler hot path).
//
// Grown storage comes from a std::pmr::memory_resource (the Graph's
// monotonic arena in production). A null resource resolves lazily to
// std::pmr::get_default_resource() so tests and tools can default-construct;
// hot-path code must pass the arena explicitly.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <type_traits>

namespace b2::ir {

template <typename T, unsigned N>
class SmallVector {
  static_assert(N >= 1, "inline capacity must be at least 1");
  static_assert(std::is_trivially_copyable_v<T>,
                "SmallVector is for trivially-copyable element types only");

public:
  using value_type = T;

  explicit SmallVector(std::pmr::memory_resource* mr = nullptr) noexcept
      : mr_(mr) {}

  SmallVector(const SmallVector& other) : mr_(other.mr_) {
    copyFrom(other);
  }

  SmallVector& operator=(const SmallVector& other) {
    if (this != &other) {
      if (mr_ == nullptr) {
        mr_ = other.mr_;
      }
      copyFrom(other);
    }
    return *this;
  }

  SmallVector(SmallVector&& other) noexcept : mr_(other.mr_) {
    moveFrom(other);
  }

  SmallVector& operator=(SmallVector&& other) noexcept {
    if (this != &other) {
      releaseHeap();
      if (mr_ == nullptr) {
        mr_ = other.mr_;
      }
      moveFrom(other);
    }
    return *this;
  }

  ~SmallVector() { releaseHeap(); }

  [[nodiscard]] T* begin() noexcept {
    return heap_ != nullptr ? heap_ : inline_;
  }
  [[nodiscard]] const T* begin() const noexcept {
    return heap_ != nullptr ? heap_ : inline_;
  }
  [[nodiscard]] T* end() noexcept { return begin() + size_; }
  [[nodiscard]] const T* end() const noexcept { return begin() + size_; }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  [[nodiscard]] T& operator[](std::size_t i) noexcept { return begin()[i]; }
  [[nodiscard]] const T& operator[](std::size_t i) const noexcept {
    return begin()[i];
  }

  void push_back(const T& v) {
    if (size_ == capacity_) {
      grow();
    }
    begin()[size_] = v;
    ++size_;
  }

  // Removes the first element equal to `v`; keeps order. Returns true if one
  // was removed. Use-list maintenance is O(n) with n typically <= 4.
  [[nodiscard]] bool remove(const T& v) {
    for (std::size_t i = 0; i < size_; ++i) {
      if (begin()[i] == v) {
        std::memmove(begin() + i, begin() + i + 1,
                     (size_ - i - 1) * sizeof(T));
        --size_;
        return true;
      }
    }
    return false;
  }

  void clear() noexcept { size_ = 0; }

private:
  [[nodiscard]] std::pmr::memory_resource* resource() {
    if (mr_ == nullptr) {
      mr_ = std::pmr::get_default_resource();
    }
    return mr_;
  }

  void grow() {
    const std::size_t newCap = capacity_ * 2;
    T* fresh = static_cast<T*>(
        resource()->allocate(newCap * sizeof(T), alignof(T)));
    std::memcpy(fresh, begin(), size_ * sizeof(T));
    releaseHeap();
    heap_ = fresh;
    capacity_ = newCap;
  }

  void copyFrom(const SmallVector& other) {
    if (other.size_ <= capacity_ && heap_ == nullptr) {
      std::memcpy(inline_, other.begin(), other.size_ * sizeof(T));
      size_ = other.size_;
      return;
    }
    releaseHeap();
    heap_ = nullptr;
    capacity_ = N;
    size_ = 0;
    for (std::size_t i = 0; i < other.size_; ++i) {
      push_back(other.begin()[i]);
    }
  }

  void moveFrom(SmallVector& other) noexcept {
    if (other.heap_ != nullptr) {
      heap_ = other.heap_;
      capacity_ = other.capacity_;
      size_ = other.size_;
      other.heap_ = nullptr;
      other.capacity_ = N;
      other.size_ = 0;
      return;
    }
    std::memcpy(inline_, other.inline_, other.size_ * sizeof(T));
    size_ = other.size_;
    other.size_ = 0;
  }

  void releaseHeap() noexcept {
    if (heap_ != nullptr) {
      // The owning resource pointer cannot be null once heap exists.
      mr_->deallocate(heap_, capacity_ * sizeof(T), alignof(T));
      heap_ = nullptr;
      capacity_ = N;
    }
  }

  std::pmr::memory_resource* mr_;
  T inline_[N];
  T* heap_ = nullptr;
  std::size_t size_ = 0;
  std::size_t capacity_ = N;
};

} // namespace b2::ir
