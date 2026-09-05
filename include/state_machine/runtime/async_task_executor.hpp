#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>

namespace state_machine {
namespace runtime {

template <typename ExecutionContext> class Task {
  public:
    virtual ~Task() = default;
    virtual void execute(ExecutionContext& ctx) = 0;
    virtual std::string name() const = 0;
};

template <typename ExecutionContext> class LambdaTask final : public Task<ExecutionContext> {
  public:
    using Callback = std::function<void(ExecutionContext&)>;

    LambdaTask(std::string task_name, Callback callback)
        : task_name_(std::move(task_name)), callback_(std::move(callback)) {}

    void execute(ExecutionContext& ctx) override {
        if (callback_) {
            callback_(ctx);
        }
    }

    std::string name() const override { return task_name_; }

  private:
    std::string task_name_;
    Callback callback_;
};

template <typename ExecutionContext> class AsyncTaskExecutor {
  public:
    explicit AsyncTaskExecutor(ExecutionContext& context) : context_(context) {}

    // The executor and context must outlive every callback. Destruction from
    // this executor's own worker is not supported; worker-side stop() is safe.
    ~AsyncTaskExecutor() {
        in_destructor_.store(true);
        stop();
    }

    AsyncTaskExecutor(const AsyncTaskExecutor&) = delete;
    AsyncTaskExecutor& operator=(const AsyncTaskExecutor&) = delete;
    AsyncTaskExecutor(AsyncTaskExecutor&&) = delete;
    AsyncTaskExecutor& operator=(AsyncTaskExecutor&&) = delete;

    void start() {
        // A worker cannot join/restart itself, or wait on a lifecycle lock
        // held by a caller that is joining it. Restart from an external thread.
        if (in_destructor_.load() || isWorkerThread()) {
            return;
        }
        TaskQueue discarded;
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (running_.load()) {
            return;
        }
        // A callback may have requested stop without joining the old worker.
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        running_.store(true);
        try {
            worker_thread_ = std::thread(&AsyncTaskExecutor::workerLoop, this);
        } catch (...) {
            requestStop(discarded);
            throw;
        }
        // discarded is destroyed after lifecycle_lock, including on failure.
    }

    void stop() {
        TaskQueue discarded;
        if (isWorkerThread()) {
            // Never take lifecycle_mutex_ here: an external stop may hold it
            // while waiting for this callback to finish.
            requestStop(discarded);
            return;
        }
        {
            std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
            requestStop(discarded);
            // Join even if running_ was already cleared by a worker-side stop.
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }
        }
        // User task/capture destructors may re-enter stop() or queueSize().
        // Destroy discarded tasks only after releasing both executor locks.
    }

    void pushTask(std::unique_ptr<Task<ExecutionContext>> task) {
        if (!task) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!running_.load()) {
                return;
            }
            queue_.push(std::move(task));
        }
        queue_cv_.notify_one();
    }

    std::size_t queueSize() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return queue_.size();
    }

    uint64_t executedCount() const { return tasks_executed_.load(); }
    uint64_t failedCount() const { return tasks_failed_.load(); }
    bool isRunning() const { return running_.load(); }

  private:
    using TaskQueue = std::queue<std::unique_ptr<Task<ExecutionContext>>>;

    bool isWorkerThread() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return worker_id_ == std::this_thread::get_id();
    }

    void requestStop(TaskQueue& discarded) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            running_.store(false);
            queue_.swap(discarded);
        }
        queue_cv_.notify_all();
    }

    void workerLoop() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            worker_id_ = std::this_thread::get_id();
        }
        while (true) {
            std::unique_ptr<Task<ExecutionContext>> task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this] {
                    return !queue_.empty() || !running_.load();
                });
                if (!running_.load() && queue_.empty()) {
                    worker_id_ = std::thread::id{};
                    break;
                }
                task = std::move(queue_.front());
                queue_.pop();
            }

            try {
                task->execute(context_);
                tasks_executed_.fetch_add(1);
            } catch (const std::exception&) {
                tasks_failed_.fetch_add(1);
            } catch (...) {
                tasks_failed_.fetch_add(1);
            }
        }
    }

    ExecutionContext& context_;
    TaskQueue queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread worker_thread_;
    std::atomic<bool> running_{false};
    std::mutex lifecycle_mutex_;
    std::thread::id worker_id_{}; // Protected by queue_mutex_.
    std::atomic<bool> in_destructor_{false};
    std::atomic<uint64_t> tasks_executed_{0};
    std::atomic<uint64_t> tasks_failed_{0};
};

} // namespace runtime
} // namespace state_machine
