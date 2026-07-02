#include "thread_pool.hpp"
#include <iostream>
#include <vector>
#include <chrono>
#include <atomic>
#include <cassert>
#include <numeric>
#include <memory>
#include <sstream>

// Helper to log test progress safely from multiple threads
void safe_print(const std::string& msg) {
    static std::mutex print_mutex;
    std::lock_guard<std::mutex> lock(print_mutex);
    std::cout << msg << std::endl;
}

// -------------------------------------------------------------
// Test 1: Basic Function Submission and Return Values
// -------------------------------------------------------------
void test_basic_execution() {
    safe_print("[RUN] test_basic_execution");
    ThreadPool pool(4);

    auto f1 = pool.enqueue([]() { return 42; });
    auto f2 = pool.enqueue([](int x) { return x * 2; }, 21);
    
    assert(f1.get() == 42);
    assert(f2.get() == 42);
    safe_print("[PASS] test_basic_execution");
}

// -------------------------------------------------------------
// Test 2: Move-only arguments
// -------------------------------------------------------------
void test_move_only_arguments() {
    safe_print("[RUN] test_move_only_arguments");
    ThreadPool pool(2);

    auto p = std::make_unique<int>(100);
    auto f = pool.enqueue([](std::unique_ptr<int> ptr) {
        return *ptr + 50;
    }, std::move(p));

    assert(f.get() == 150);
    safe_print("[PASS] test_move_only_arguments");
}

// -------------------------------------------------------------
// Test 3: Exception Propagation
// -------------------------------------------------------------
void test_exception_propagation() {
    safe_print("[RUN] test_exception_propagation");
    ThreadPool pool(2);

    auto f = pool.enqueue([]() {
        throw std::runtime_error("Task-internal error");
        return 0;
    });

    try {
        f.get();
        assert(false && "Should have thrown an exception");
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        assert(msg == "Task-internal error");
    } catch (...) {
        assert(false && "Caught unexpected exception type");
    }

    safe_print("[PASS] test_exception_propagation");
}

// -------------------------------------------------------------
// Test 4: Graceful Shutdown
// -------------------------------------------------------------
void test_graceful_shutdown() {
    safe_print("[RUN] test_graceful_shutdown");
    std::atomic<int> completed_tasks{0};
    
    {
        ThreadPool pool(2);

        // Enqueue several tasks
        for (int i = 0; i < 10; ++i) {
            pool.enqueue([&completed_tasks, i]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                completed_tasks++;
            });
        }
        // Destructor of pool will be called here, triggering graceful shutdown.
        // It must block until all 10 tasks are finished.
    }

    assert(completed_tasks.load() == 10);
    safe_print("[PASS] test_graceful_shutdown");
}

// -------------------------------------------------------------
// Test 5: Immediate Shutdown (No-Graceful)
// -------------------------------------------------------------
void test_immediate_shutdown() {
    safe_print("[RUN] test_immediate_shutdown");
    std::atomic<int> completed_tasks{0};
    
    ThreadPool pool(2);

    // Enqueue many slow tasks
    for (int i = 0; i < 20; ++i) {
        pool.enqueue([&completed_tasks]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            completed_tasks++;
        });
    }

    // Give a short delay to let worker threads start processing the first 2 tasks
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Call immediate shutdown
    pool.shutdown(false); // graceful = false

    // Not all tasks should have completed. Specifically, only the ones currently
    // running when shutdown was called should complete.
    int completed = completed_tasks.load();
    std::stringstream ss;
    ss << "Completed " << completed << " of 20 tasks under immediate shutdown.";
    safe_print(ss.str());
    assert(completed < 20);

    // Enqueueing after shutdown should throw
    try {
        pool.enqueue([]() {});
        assert(false && "Should have thrown after shutdown");
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()).find("enqueue on stopped ThreadPool") != std::string::npos);
    }

    safe_print("[PASS] test_immediate_shutdown");
}

// -------------------------------------------------------------
// Test 6: Concurrency and Stress Test
// -------------------------------------------------------------
void test_concurrency_stress() {
    safe_print("[RUN] test_concurrency_stress");
    constexpr int NUM_THREADS = 8;
    constexpr int SUBMISSIONS_PER_THREAD = 1000;
    constexpr int TOTAL_TASKS = NUM_THREADS * SUBMISSIONS_PER_THREAD;
    
    ThreadPool pool(4);
    std::atomic<int> counter{0};

    auto start_time = std::chrono::high_resolution_clock::now();

    // Spawn multiple threads that concurrently submit tasks to the pool
    std::vector<std::thread> submitters;
    submitters.reserve(NUM_THREADS);

    for (int t = 0; t < NUM_THREADS; ++t) {
        submitters.emplace_back([&pool, &counter]() {
            for (int i = 0; i < SUBMISSIONS_PER_THREAD; ++i) {
                pool.enqueue([&counter]() {
                    counter.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }

    // Join submitter threads
    for (auto& t : submitters) {
        t.join();
    }

    // Shutdown pool gracefully to wait for all submitted tasks to complete
    pool.shutdown(true);

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    assert(counter.load() == TOTAL_TASKS);
    std::stringstream ss;
    ss << "Successfully processed " << TOTAL_TASKS << " tasks concurrently in " << elapsed << " ms.";
    safe_print(ss.str());
    safe_print("[PASS] test_concurrency_stress");
}

// -------------------------------------------------------------
// Test 7: Exception Safety on Thread Creation Failure
// -------------------------------------------------------------
void test_constructor_exception_safety() {
    safe_print("[RUN] test_constructor_exception_safety");

    // We try to request a ridiculously huge number of threads to trigger system resource limits (or std::system_error).
    // Note: On some systems, requesting a very large number of threads might take time or fail immediately.
    // To avoid hang, we catch std::system_error and check if the pool cleaned up properly.
    try {
        // Request 100,000 threads. This should fail on standard desktop operating systems.
        // If it succeeds, it's fine, we'll shut it down, but normally it throws std::system_error.
        ThreadPool huge_pool(100000);
        huge_pool.shutdown();
        safe_print("System was able to allocate 100,000 threads.");
    } catch (const std::system_error& e) {
        std::stringstream ss;
        ss << "Caught expected system_error during massive thread pool allocation: " << e.what();
        safe_print(ss.str());
    } catch (const std::bad_alloc& e) {
        std::stringstream ss;
        ss << "Caught expected bad_alloc during massive thread pool allocation: " << e.what();
        safe_print(ss.str());
    }

    safe_print("[PASS] test_constructor_exception_safety");
}

int main() {
    std::cout << "========================================\n";
    std::cout << "Starting ThreadPool Verification Suite\n";
    std::cout << "========================================\n";

    test_basic_execution();
    test_move_only_arguments();
    test_exception_propagation();
    test_graceful_shutdown();
    test_immediate_shutdown();
    test_concurrency_stress();
    test_constructor_exception_safety();

    std::cout << "========================================\n";
    std::cout << "ALL TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "========================================\n";
    return 0;
}
