#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <memory>
#include <type_traits>
#include <stdexcept>
#include <tuple>
#include <utility>

class ThreadPool {
public:
    /**
     * @brief Construct a new Thread Pool.
     * 
     * @param threads Number of worker threads. If 0, defaults to std::thread::hardware_concurrency(),
     *                with a fallback of 2 threads if that returns 0.
     */
    explicit ThreadPool(size_t threads = 0) {
        if (threads == 0) {
            threads = std::thread::hardware_concurrency();
            if (threads == 0) {
                threads = 2; // Conservative fallback
            }
        }

        try {
            workers.reserve(threads);
            for (size_t i = 0; i < threads; ++i) {
                workers.emplace_back(&ThreadPool::worker_loop, this);
            }
        } catch (...) {
            // Exception safety: if thread creation fails partway, clean up successfully
            // created threads before propagating the exception.
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                stop_flag = true;
            }
            cv.notify_all();
            for (std::thread& worker : workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            throw; // Rethrow the exception
        }
    }

    /**
     * @brief Destroy the Thread Pool.
     * Ensures all worker threads finish their current tasks and join.
     */
    ~ThreadPool() {
        shutdown(true); // Default to graceful shutdown
    }

    // Disable copy constructor and copy assignment
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Disable move constructor and move assignment
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /**
     * @brief Enqueue a task for execution in the thread pool.
     * 
     * @tparam F Type of the callable.
     * @tparam Args Types of the arguments to pass to the callable.
     * @param f The callable to execute.
     * @param args The arguments to pass to the callable.
     * @return std::future to retrieve the result of the callable.
     */
    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args) 
        -> std::future<typename std::invoke_result<F, Args...>::type> {
        
        using return_type = typename std::invoke_result<F, Args...>::type;

        // Wrap the task and its arguments using std::tuple and std::apply.
        // This is modern C++17 and perfectly supports move-only arguments (e.g. std::unique_ptr).
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [f = std::forward<F>(f), args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                return std::apply(std::move(f), std::move(args));
            }
        );
        
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop_flag) {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            // Capture the shared pointer by value inside a copyable lambda,
            // which can then be stored inside std::function.
            tasks.emplace([task]() { (*task)(); });
        }
        cv.notify_one();
        return res;
    }

    /**
     * @brief Shutdown the Thread Pool explicitly.
     * 
     * @param graceful If true, finishes executing all tasks in the queue. 
     *                 If false, discards all pending tasks in the queue.
     */
    void shutdown(bool graceful = true) {
        std::unique_lock<std::mutex> shutdown_lock(shutdown_mutex);
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop_flag) {
                return; // Already shut down or shutting down
            }
            stop_flag = true;
            if (!graceful) {
                // Discard all queued tasks
                std::queue<std::function<void()>> empty_queue;
                std::swap(tasks, empty_queue);
            }
        }
        cv.notify_all();
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    /**
     * @brief Check if the pool is shut down or shutting down.
     */
    bool is_shutdown() const {
        std::unique_lock<std::mutex> lock(queue_mutex);
        return stop_flag;
    }

private:
    /**
     * @brief The loop executed by each worker thread.
     */
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                cv.wait(lock, [this]() {
                    return stop_flag || !tasks.empty();
                });
                if (stop_flag && tasks.empty()) {
                    return; // Exit loop to allow thread to join
                }
                task = std::move(tasks.front());
                tasks.pop();
            }
            task(); // Execute outside the lock
        }
    }

    // Worker threads
    std::vector<std::thread> workers;

    // Task queue (storing type-erased tasks)
    std::queue<std::function<void()>> tasks;

    // Synchronization
    mutable std::mutex queue_mutex;
    std::mutex shutdown_mutex; // Serializes concurrent calls to shutdown() and destructor
    std::condition_variable cv;
    bool stop_flag = false;
};
