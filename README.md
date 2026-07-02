# A Correct, Thread-Safe, and Exception-Safe C++17 Thread Pool

[![Language](https://img.shields.io/badge/language-C%2B%2B17%20%2F%20C%2B%2B20-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20macOS-lightgrey.svg)](#)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](#)


A header-only, production-grade C++17 thread pool. While popular repositories like `progschj/ThreadPool` are excellent references, they frequently contain subtle concurrency bugs, thread leaks on construction failure, and lack full support for modern move-only semantics (such as forwarding `std::unique_ptr`). 

This implementation is built from the ground up to solve those hard edge cases.


---

## ⚡ Key Features

* **Header-Only**: Drop [thread_pool.hpp](file:///e:/chat%20app/thread_pool.hpp) straight into your project.

* **Modern C++ Semantics**: Full support for move-only callables and arguments (e.g., `std::unique_ptr`).

* **Exception Safe**: Guaranteed thread cleanup even if the constructor fails mid-way due to OS resource limits.

* **Safe Concurrent Shutdown**: Double-shutdown and destructor calls are serialized. Joining happens outside the task lock, eliminating deadlocks.

* **Type-Safe Futures**: Automatically deduces task return types and handles task-internal exception propagation.


---

## 🛠️ The Hard Problems & Design Decisions

Below is a deep dive into the engineering decisions made to ensure absolute correctness.


### 1. Storing Move-Only Callables in a Copyable Queue

> **The Problem:** `std::function<void()>` is copy-constructible. However, `std::packaged_task` (which wraps our tasks to connect with futures) is move-only.
> 
> **The Solution:** Rather than writing a custom type-erased queue, we wrap the `std::packaged_task` inside a `std::shared_ptr`. Since `std::shared_ptr` itself is copyable, the lambda capturing it by value is copy-constructible and can be stored cleanly in a standard queue:

```cpp
auto task = std::make_shared<std::packaged_task<return_type()>>( ... );
tasks.emplace([task]() { (*task)(); });
```


---

### 2. Forwarding Move-Only Arguments (`std::unique_ptr`)

> **The Problem:** Forwarding move-only arguments through standard wrappers like `std::bind` is notoriously fragile and often triggers compiler errors because `std::bind` copies or references its arguments.
> 
> **The Solution:** We capture the function and its arguments inside a `std::tuple` using perfect forwarding, then invoke it via `std::apply` inside a `mutable` lambda:

```cpp
template<typename F, typename... Args>
auto enqueue(F&& f, Args&&... args) {
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        [f = std::forward<F>(f), args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            return std::apply(std::move(f), std::move(args));
        }
    );
    // ...
}
```

This guarantees that arguments are moved exactly once into the deferred thread call.


---

### 3. Guaranteeing Clean Cleanup on Constructor Failure

> **The Problem:** If you instantiate a pool requesting 64 threads but the OS throws a resource limit error (e.g. `std::system_error` or `std::bad_alloc`) on the 32nd thread, the destructor of `ThreadPool` is never called. However, the vector of already-created threads will be destroyed, triggering `std::terminate()` because the threads are still active/joinable.
> 
> **The Solution:** We wrap the constructor allocation in a `try-catch` block. If thread creation fails, we flag the pool to stop, notify existing workers, join them cleanly, and then rethrow the exception:

```cpp
try {
    for (size_t i = 0; i < threads; ++i) {
        workers.emplace_back(&ThreadPool::worker_loop, this);
    }
} catch (...) {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop_flag = true;
    }
    cv.notify_all();
    for (std::thread& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    throw; // Escapes safely
}
```


---

### 4. Deadlock-Free Shutdown & Destructors

> **The Problem:** A naive pool destructor joins threads while holding the queue mutex. If a worker thread is running, finishes its task, and tries to lock the queue mutex to fetch the next task, it will block. This creates a classic deadlock: the shutdown thread waits to join the worker, and the worker waits for the shutdown thread to release the lock.
> 
> **The Solution:** We decouple queue operations from joining operations by utilizing a dedicated `shutdown_mutex` and locking the `queue_mutex` only during state changes:

```cpp
void shutdown(bool graceful = true) {
    std::unique_lock<std::mutex> shutdown_lock(shutdown_mutex); // Serialize concurrent shutdowns
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (stop_flag) return;
        stop_flag = true;
        if (!graceful) {
            std::queue<std::function<void()>> empty_queue;
            std::swap(tasks, empty_queue); // Discard pending tasks
        }
    }
    cv.notify_all(); // Wake up threads
    
    // Workers are joined safely outside of queue_mutex
    for (std::thread& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}
```


---

## 🚀 Quick Start


### 1. Scheduling Basic Tasks & Futures

```cpp
#include "thread_pool.hpp"
#include <iostream>

int main() {
    ThreadPool pool(4); // 4 worker threads

    // Schedule a task returning an int
    auto future = pool.enqueue([](int a, int b) {
        return a + b;
    }, 12, 30);

    std::cout << "Result: " << future.get() << "\n"; // Outputs: 42
}
```


### 2. Passing Move-Only Resources

```cpp
#include "thread_pool.hpp"
#include <memory>
#include <iostream>

void process(std::unique_ptr<int> val) {
    std::cout << "Processing: " << *val << "\n";
}

int main() {
    ThreadPool pool(2);
    auto ptr = std::make_unique<int>(100);

    // Moves ptr safely into the pool
    auto f = pool.enqueue(process, std::move(ptr));
    f.get();
}
```


### 3. Graceful vs. Immediate Shutdown

```cpp
ThreadPool pool(4);

// ... submit tasks ...

// Wait for all queued tasks to finish (Default on Destruction)
pool.shutdown(true);

// OR: Stop immediately and discard queued tasks
// pool.shutdown(false);
```


---

## 🧪 Verification & Testing

The code compiles warning-free under standard C++17 configurations.

To compile and run the comprehensive verification suite:

```bash
# Compiling
g++ -std=c++17 -O3 -Wall main.cpp -o test_thread_pool

# Running
./test_thread_pool
```
