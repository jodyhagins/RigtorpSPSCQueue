/*
Copyright (c) 2018 Erik Rigtorp <erik@rigtorp.se>

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

#undef NDEBUG

#include <cassert>
#include <chrono>
#include <iostream>
#include <rigtorp/SPSCQueue.h>
#include <set>
#include <thread>

// TestType tracks correct usage of constructors and destructors
struct TestType {
  static std::set<const TestType *> constructed;
  TestType() noexcept {
    assert(constructed.count(this) == 0);
    constructed.insert(this);
  };
  TestType(const TestType &other) noexcept {
    assert(constructed.count(this) == 0);
    assert(constructed.count(&other) == 1);
    constructed.insert(this);
  };
  TestType(TestType &&other) noexcept {
    assert(constructed.count(this) == 0);
    assert(constructed.count(&other) == 1);
    constructed.insert(this);
  };
  TestType &operator=(const TestType &other) noexcept {
    assert(constructed.count(this) == 1);
    assert(constructed.count(&other) == 1);
    return *this;
  };
  TestType &operator=(TestType &&other) noexcept {
    assert(constructed.count(this) == 1);
    assert(constructed.count(&other) == 1);
    return *this;
  }
  ~TestType() noexcept {
    assert(constructed.count(this) == 1);
    constructed.erase(this);
  };
};

std::set<const TestType *> TestType::constructed;

int main(int argc, char *argv[]) {
  (void)argc, (void)argv;

  using namespace rigtorp;

  // Functionality test
  {
    SPSCQueue<TestType> q(10);
    assert(q.front() == nullptr);
    assert(q.size() == 0);
    assert(q.empty() == true);
    assert(q.capacity() == 10);
    for (int i = 0; i < 10; i++) {
      q.emplace();
    }
    assert(q.front() != nullptr);
    assert(q.size() == 10);
    assert(q.empty() == false);
    assert(TestType::constructed.size() == 10);
    assert(q.try_emplace() == false);
    q.pop();
    assert(q.size() == 9);
    assert(TestType::constructed.size() == 9);
    q.pop();
    assert(q.try_emplace() == true);
    assert(TestType::constructed.size() == 9);
  }
  assert(TestType::constructed.size() == 0);

  // Copyable only type
  {
    struct Test {
      Test() {}
      Test(const Test &) {}
      Test(Test &&) = delete;
    };
    SPSCQueue<Test> q(16);
    // lvalue
    Test v;
    q.emplace(v);
    (void)q.try_emplace(v);
    q.push(v);
    (void)q.try_push(v);
    static_assert(noexcept(q.emplace(v)) == false, "");
    static_assert(noexcept(q.try_emplace(v)) == false, "");
    static_assert(noexcept(q.push(v)) == false, "");
    static_assert(noexcept(q.try_push(v)) == false, "");
    // xvalue
    q.push(Test());
    (void)q.try_push(Test());
    static_assert(noexcept(q.push(Test())) == false, "");
    static_assert(noexcept(q.try_push(Test())) == false, "");
  }

  // Copyable only type (noexcept)
  {
    struct Test {
      Test() noexcept {}
      Test(const Test &) noexcept {}
      Test(Test &&) = delete;
    };
    SPSCQueue<Test> q(16);
    // lvalue
    Test v;
    q.emplace(v);
    (void)q.try_emplace(v);
    q.push(v);
    (void)q.try_push(v);
    static_assert(noexcept(q.emplace(v)) == true, "");
    static_assert(noexcept(q.try_emplace(v)) == true, "");
    static_assert(noexcept(q.push(v)) == true, "");
    static_assert(noexcept(q.try_push(v)) == true, "");
    // xvalue
    q.push(Test());
    (void)q.try_push(Test());
    static_assert(noexcept(q.push(Test())) == true, "");
    static_assert(noexcept(q.try_push(Test())) == true, "");
  }

  // Movable only type
  {
    SPSCQueue<std::unique_ptr<int>> q(16);
    // lvalue
    // auto v = std::unique_ptr<int>(new int(1));
    // q.emplace(v);
    // q.try_emplace(v);
    // q.push(v);
    // q.try_push(v);
    // xvalue
    q.emplace(std::unique_ptr<int>(new int(1)));
    (void)q.try_emplace(std::unique_ptr<int>(new int(1)));
    q.push(std::unique_ptr<int>(new int(1)));
    (void)q.try_push(std::unique_ptr<int>(new int(1)));
    auto v = std::unique_ptr<int>(new int(1));
    static_assert(noexcept(q.emplace(std::move(v))) == true, "");
    static_assert(noexcept(q.try_emplace(std::move(v))) == true, "");
    static_assert(noexcept(q.push(std::move(v))) == true, "");
    static_assert(noexcept(q.try_push(std::move(v))) == true, "");
  }

  // capacity < 1
  {
    SPSCQueue<int> q(0);
    assert(q.capacity() == 1);
  }

  // Check that padding doesn't overflow capacity
  {
    bool throws = false;
    try {
      SPSCQueue<int> q(SIZE_MAX - 1);
    } catch (...) {
      throws = true;
    }
    assert(throws);
  }

  // Fuzz and performance test
  {
    const size_t iter = 100000;
    SPSCQueue<size_t> q(iter / 1000 + 1);
    std::atomic<bool> flag(false);
    std::thread producer([&] {
      while (!flag)
        ;
      for (size_t i = 0; i < iter; ++i) {
        q.emplace(i);
      }
    });

    size_t sum = 0;
    auto start = std::chrono::system_clock::now();
    flag = true;
    for (size_t i = 0; i < iter; ++i) {
      while (!q.front())
        ;
      sum += *q.front();
      q.pop();
    }
    auto end = std::chrono::system_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

    assert(q.front() == nullptr);
    assert(sum == iter * (iter - 1) / 2);

    producer.join();

    std::cout << duration.count() / iter << " ns/iter" << std::endl;
  }

  // Tests for possible shared memory usage
  {
    std::vector<int> v;
    struct Allocator {
      using value_type = int;
      using may_be_used_in_shared_memory = void;
      struct Atomic {
        std::size_t value;
        std::size_t load(std::memory_order) { return value; }
        void store(std::size_t v, std::memory_order) { value = v; }
      };
      int *allocate(std::size_t n) {
        v->resize(n);
        return v->data();
      }
      std::vector<int> *v;
    };
    using Q = SPSCQueue<int, Allocator>;
    static_assert(std::is_default_constructible_v<Q>);
    static_assert(std::is_trivially_default_constructible_v<Q>);
    static_assert(std::is_trivially_destructible_v<Q>);
    Q q(17, Allocator{&v});
    q.emplace(42);
    std::size_t padding =
        std::distance(v.begin(), std::find_if(v.begin(), v.end(),
                                              [](int x) { return x != 0; }));
    assert(padding < v.size());
    assert(42 == v[0 + padding]);
    q.emplace(86);
    assert(86 == v[1 + padding]);

    auto v2 = v;
    q.reattach_writer(v2.data());
    q.emplace(99);
    assert(99 == v2[2 + padding]);

    auto p = q.front();
    assert(p);
    q.pop();
    assert(42 == *p);

    p = q.front();
    assert(p);
    q.pop();
    assert(86 == *p);

    q.reattach_reader(v2.data());
    p = q.front();
    assert(p);
    q.pop();
    assert(99 == *p);
  }

  return 0;
}
