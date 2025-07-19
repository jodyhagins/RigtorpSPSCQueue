/*
Copyright (c) 2020 Erik Rigtorp <erik@rigtorp.se>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory> // std::allocator
#include <new>    // std::hardware_destructive_interference_size
#include <stdexcept>
#include <type_traits> // std::enable_if, std::is_*_constructible

#ifdef __has_cpp_attribute
#if __has_cpp_attribute(nodiscard)
#define RIGTORP_NODISCARD [[nodiscard]]
#endif
#endif
#ifndef RIGTORP_NODISCARD
#define RIGTORP_NODISCARD
#endif

namespace rigtorp {

template <typename SlotT, typename Allocator = std::allocator<SlotT>>
class SPSCQueue {
  template <typename A, typename = void> struct real_type {
    using type = SlotT;
  };
  template <typename A>
  struct real_type<A, std::void_t<typename A::value_type::real_type>> {
    using type = typename A::value_type::real_type;
  };
  using T = typename real_type<Allocator>::type;

#if defined(__cpp_if_constexpr) && defined(__cpp_lib_void_t)
  template <typename Alloc2, typename = void>
  struct has_allocate_at_least : std::false_type {};

  template <typename Alloc2>
  struct has_allocate_at_least<
      Alloc2, std::void_t<typename Alloc2::value_type,
                          decltype(std::declval<Alloc2 &>().allocate_at_least(
                              size_t{}))>> : std::true_type {};
#endif
  inline static constexpr bool may_be_used_in_shared_memory =
      requires { typename Allocator::may_be_used_in_shared_memory; };
  static_assert(not may_be_used_in_shared_memory ||
                std::is_trivially_default_constructible_v<Allocator>);
  static_assert(not may_be_used_in_shared_memory ||
                std::is_trivially_destructible_v<Allocator>);

public:
  /**
   * The default constructor is trivial, which means it does nothing.
   *
   * @note  There is no other means of initialization aside from the constructor
   * that takes a capacity and allocator, and the class is neither moveable nor
   * copyable, so using this constructor is meaningless.  It's sole purpose is
   * to allow this type to qualify as an implicit lifetime type.  This
   * constructor should never be used as it is impossible to safely use such a
   * constructed object.
   *
   * @note  This constructor is only provided when Allocator contains a type
   * alias named may_be_used_in_shared_memory.
   */
  SPSCQueue()
    requires may_be_used_in_shared_memory
  = default;

  explicit SPSCQueue(const size_t capacity,
                     const Allocator &allocator = Allocator())
      : capacity_(capacity), allocator_(allocator), w_{}, r_{} {
    // The queue needs at least one element
    if (capacity_ < 1) {
      capacity_ = 1;
    }
    capacity_++; // Needs one slack element
    // Prevent overflowing size_t
    if (capacity_ > SIZE_MAX - 2 * kPadding) {
      capacity_ = SIZE_MAX - 2 * kPadding;
    }

#if defined(__cpp_if_constexpr) && defined(__cpp_lib_void_t)
    if constexpr (has_allocate_at_least<Allocator>::value) {
      auto res = allocator_.allocate_at_least(capacity_ + 2 * kPadding);
      r_.slots_ = w_.slots_ = res.ptr;
      capacity_ = res.count - 2 * kPadding;
    } else {
      r_.slots_ = w_.slots_ = std::allocator_traits<Allocator>::allocate(
          allocator_, capacity_ + 2 * kPadding);
    }
#else
    r_.slots_ = w_.slots_ = std::allocator_traits<Allocator>::allocate(
        allocator_, capacity_ + 2 * kPadding);
#endif

    static_assert(alignof(SPSCQueue<SlotT>) == kCacheLineSize, "");
    static_assert(sizeof(SPSCQueue<SlotT>) >= 3 * kCacheLineSize, "");
    assert(reinterpret_cast<char *>(&r_.readIdx_) -
               reinterpret_cast<char *>(&w_.writeIdx_) >=
           static_cast<std::ptrdiff_t>(kCacheLineSize));
  }

  /**
   * The trivial destructor does nothing.
   *
   * @note  This constructor is only provided when Allocator contains a type
   * alias named may_be_used_in_shared_memory.
   */
  ~SPSCQueue()
    requires may_be_used_in_shared_memory
  = default;

  /**
   * This user-provided destructor will only be present when the Allocator does
   * not contain a type alias named may_be_used_in_shared_memory.
   */
  ~SPSCQueue()
    requires(not may_be_used_in_shared_memory)
  {
    assert(r_.slots_ == w_.slots_);
    while (front()) {
      pop();
    }
    std::allocator_traits<Allocator>::deallocate(allocator_, r_.slots_,
                                                 capacity_ + 2 * kPadding);
  }

  // non-copyable and non-movable
  SPSCQueue(const SPSCQueue &) = delete;
  SPSCQueue &operator=(const SPSCQueue &) = delete;

  template <typename... Args>
  void emplace(Args &&...args) noexcept(
      std::is_nothrow_constructible<T, Args &&...>::value) {
    assert(w_.slots_);
    static_assert(std::is_constructible<T, Args &&...>::value,
                  "T must be constructible with Args&&...");
    auto const writeIdx = w_.writeIdx_.load(std::memory_order_relaxed);
    auto nextWriteIdx = writeIdx + 1;
    if (nextWriteIdx == capacity_) {
      nextWriteIdx = 0;
    }
    while (nextWriteIdx == w_.readIdxCache_) {
      w_.readIdxCache_ = r_.readIdx_.load(std::memory_order_acquire);
    }
    construct(&allocator_, &w_.slots_[writeIdx + kPadding], writeIdx,
              std::forward<Args>(args)...);
    w_.writeIdx_.store(nextWriteIdx, std::memory_order_release);
  }

  template <typename... Args>
  RIGTORP_NODISCARD bool try_emplace(Args &&...args) noexcept(
      std::is_nothrow_constructible<T, Args &&...>::value) {
    assert(w_.slots_);
    static_assert(std::is_constructible<T, Args &&...>::value,
                  "T must be constructible with Args&&...");
    auto const writeIdx = w_.writeIdx_.load(std::memory_order_relaxed);
    auto nextWriteIdx = writeIdx + 1;
    if (nextWriteIdx == capacity_) {
      nextWriteIdx = 0;
    }
    if (nextWriteIdx == w_.readIdxCache_) {
      w_.readIdxCache_ = r_.readIdx_.load(std::memory_order_acquire);
      if (nextWriteIdx == w_.readIdxCache_) {
        return false;
      }
    }
    construct(&allocator_, &w_.slots_[writeIdx + kPadding], writeIdx,
              std::forward<Args>(args)...);
    w_.writeIdx_.store(nextWriteIdx, std::memory_order_release);
    return true;
  }

  void push(const T &v) noexcept(std::is_nothrow_copy_constructible<T>::value) {
    static_assert(std::is_copy_constructible<T>::value,
                  "T must be copy constructible");
    emplace(v);
  }

  template <typename P, typename = typename std::enable_if<
                            std::is_constructible<T, P &&>::value>::type>
  void push(P &&v) noexcept(std::is_nothrow_constructible<T, P &&>::value) {
    emplace(std::forward<P>(v));
  }

  RIGTORP_NODISCARD bool
  try_push(const T &v) noexcept(std::is_nothrow_copy_constructible<T>::value) {
    static_assert(std::is_copy_constructible<T>::value,
                  "T must be copy constructible");
    return try_emplace(v);
  }

  template <typename P, typename = typename std::enable_if<
                            std::is_constructible<T, P &&>::value>::type>
  RIGTORP_NODISCARD bool
  try_push(P &&v) noexcept(std::is_nothrow_constructible<T, P &&>::value) {
    return try_emplace(std::forward<P>(v));
  }

  RIGTORP_NODISCARD SlotT *front() noexcept {
    assert(r_.slots_);
    auto const readIdx = r_.readIdx_.load(std::memory_order_relaxed);
    if (readIdx == r_.writeIdxCache_) {
      r_.writeIdxCache_ = w_.writeIdx_.load(std::memory_order_acquire);
      if (r_.writeIdxCache_ == readIdx) {
        return nullptr;
      }
    }
    return &r_.slots_[readIdx + kPadding];
  }

  void pop() noexcept {
    assert(r_.slots_);
    static_assert(std::is_nothrow_destructible<T>::value,
                  "T must be nothrow destructible");
    auto const readIdx = r_.readIdx_.load(std::memory_order_relaxed);
    assert(w_.writeIdx_.load(std::memory_order_acquire) != readIdx &&
           "Can only call pop() after front() has returned a non-nullptr");
    r_.slots_[readIdx + kPadding].~SlotT();
    auto nextReadIdx = readIdx + 1;
    if (nextReadIdx == capacity_) {
      nextReadIdx = 0;
    }
    r_.readIdx_.store(nextReadIdx, std::memory_order_release);
  }

  RIGTORP_NODISCARD size_t size() const noexcept {
    std::ptrdiff_t diff = w_.writeIdx_.load(std::memory_order_acquire) -
                          r_.readIdx_.load(std::memory_order_acquire);
    if (diff < 0) {
      diff += capacity_;
    }
    return static_cast<size_t>(diff);
  }

  RIGTORP_NODISCARD bool empty() const noexcept {
    return w_.writeIdx_.load(std::memory_order_acquire) ==
           r_.readIdx_.load(std::memory_order_acquire);
  }

  RIGTORP_NODISCARD size_t capacity() const noexcept { return capacity_ - 1; }

  /**
   * Attach the slots pointer for the reader to another memory location.
   *
   * This is useful in shared memory situations where the reader
   * lives in a different process than where the queue was created.  In such
   * cases, the memory address of the queue can be different in different
   * processes.
   */
  void reattach_reader(SlotT *slots)
    requires may_be_used_in_shared_memory
  {
    r_.slots_ = slots;
  }

  /**
   * Attach the slots pointer for the reader to another memory location.
   *
   * This is useful in shared memory situations where the reader
   * lives in a different process than where the queue was created.  In such
   * cases, the memory address of the queue can be different in different
   * processes.
   */
  void reattach_writer(SlotT *slots)
    requires may_be_used_in_shared_memory
  {
    w_.slots_ = slots;
  }

private:
#if defined(RIGTORP_SPSC_QUEUE_CACHE_LINE_SIZE)
  static constexpr size_t kCacheLineSize = RIGTORP_SPSC_QUEUE_CACHE_LINE_SIZE;
#elif defined(__cpp_lib_hardware_interference_size)
  static constexpr size_t kCacheLineSize =
      std::hardware_destructive_interference_size;
#elif defined(__APPLE__) && defined(__aarch64__)
  static constexpr size_t kCacheLineSize = 128;
#else
  static constexpr size_t kCacheLineSize = 64;
#endif

  // Padding to avoid false sharing between slots_ and adjacent allocations
  static constexpr size_t kPadding = (kCacheLineSize - 1) / sizeof(SlotT) + 1;

private:
  template <typename U> struct atomic {
    using type = std::atomic<U>;
  };
  template <typename U>
    requires requires { typename Allocator::Atomic; }
  struct atomic<U> {
    using type = typename Allocator::Atomic;
  };
  template <typename U> using atomic_t = typename atomic<U>::type;

  size_t capacity_;
#if defined(__has_cpp_attribute) && __has_cpp_attribute(no_unique_address)
  Allocator allocator_ [[no_unique_address]];
#else
  Allocator allocator_;
#endif

  // Align to cache line size in order to avoid false sharing
  // readIdxCache_ and writeIdxCache_ is used to reduce the amount of cache
  // coherency traffic
  alignas(kCacheLineSize) struct Writer {
    atomic_t<size_t> writeIdx_;
    size_t readIdxCache_;
    SlotT *slots_;
  } w_;
  alignas(kCacheLineSize) struct Reader {
    atomic_t<size_t> readIdx_;
    size_t writeIdxCache_;
    SlotT *slots_;
  } r_;

  template <typename A, typename... ArgTs>
  static auto construct(A *a, void *addr, std::size_t index, ArgTs &&...args)
      -> decltype(a->construct(addr, index, std::forward<ArgTs>(args)...)) {
    return a->construct(addr, index, std::forward<ArgTs>(args)...);
  }
  template <typename... ArgTs>
  static auto construct(void *, void *addr, std::size_t, ArgTs &&...args) {
    ::new (addr) T(std::forward<ArgTs>(args)...);
  }
};
} // namespace rigtorp
