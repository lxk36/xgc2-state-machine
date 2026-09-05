#include "state_machine/runtime/async_task_executor.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Executor = state_machine::runtime::AsyncTaskExecutor<int>;
using LambdaTask = state_machine::runtime::LambdaTask<int>;
using Task = state_machine::runtime::Task<int>;
using namespace std::chrono_literals;

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << std::endl;
        // Failure may involve a deliberately blocked worker. Do not hang while
        // unwinding an executor whose shutdown is the subject of the test.
        std::exit(EXIT_FAILURE);
    }
}

void await(std::future<void>& future, const char* message) {
    require(future.wait_for(3s) == std::future_status::ready, message);
    future.get();
}

void enqueue(Executor& executor, std::function<void(int&)> callback) {
    executor.pushTask(std::make_unique<LambdaTask>("test", std::move(callback)));
}

class Gate {
  public:
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return open_; });
    }
    void open() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
        }
        condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool open_{false};
};

class CleanupTask final : public Task {
  public:
    CleanupTask(std::function<void()> cleanup, std::atomic<bool>& executed)
        : cleanup_(std::move(cleanup)), executed_(executed) {}
    ~CleanupTask() override { cleanup_(); }
    void execute(int&) override { executed_.store(true); }
    std::string name() const override { return "cleanup"; }

  private:
    std::function<void()> cleanup_;
    std::atomic<bool>& executed_;
};

void selfStop() {
    int context = 0;
    Executor executor(context);
    std::promise<void> returned;
    auto done = returned.get_future();
    executor.start();
    enqueue(executor, [&](int&) {
        executor.stop();
        returned.set_value();
    });
    await(done, "worker stop must return without attempting self-join");
    executor.stop(); // Must join even though the worker already cleared running.
    require(!executor.isRunning(), "executor remains stopped");
    require(executor.executedCount() == 1, "self-stopping callback completed");
    require(executor.failedCount() == 0, "self-stop is not a callback failure");
}

void externalAndWorkerStop() {
    int context = 0;
    Executor executor(context);
    std::promise<void> entered;
    auto started = entered.get_future();
    Gate release;
    executor.start();
    enqueue(executor, [&](int&) {
        entered.set_value();
        release.wait();
        executor.stop();
    });
    await(started, "blocking callback started");
    std::promise<void> returned;
    auto stopped = returned.get_future();
    std::thread stopper([&] { executor.stop(); returned.set_value(); });
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (executor.isRunning() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    require(!executor.isRunning(), "external stop requested before worker stop");
    release.open();
    await(stopped, "external join and worker stop must not deadlock");
    stopper.join();
    require(executor.executedCount() == 1, "running callback finishes before stop returns");
}

void discardedTaskCleanup() {
    int context = 0;
    Executor executor(context);
    Gate release;
    std::promise<void> entered;
    auto started = entered.get_future();
    std::atomic<bool> executed{false};
    std::atomic<bool> cleaned{false};
    executor.start();
    enqueue(executor, [&](int&) { entered.set_value(); release.wait(); });
    await(started, "worker held while cleanup task is queued");
    executor.pushTask(std::make_unique<CleanupTask>([&] {
        require(executor.queueSize() == 0, "queue empty during discarded task cleanup");
        executor.stop(); // Exercises both queue and lifecycle lock re-entry.
        cleaned.store(true);
    }, executed));
    std::promise<void> returned;
    auto stopped = returned.get_future();
    std::thread stopper([&] { executor.stop(); returned.set_value(); });
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (executor.isRunning() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    require(!executor.isRunning(), "stop began");
    release.open();
    await(stopped, "discarded tasks must be destroyed outside executor locks");
    stopper.join();
    require(cleaned.load(), "queued task was destroyed");
    require(!executed.load(), "discarded task was not executed");
}

void destructorCleanup() {
    int context = 0;
    auto executor = std::make_unique<Executor>(context);
    Executor* raw = executor.get();
    Gate release;
    std::promise<void> entered;
    auto started = entered.get_future();
    std::atomic<bool> executed{false};
    std::atomic<bool> cleaned{false};
    raw->start();
    enqueue(*raw, [&](int&) { entered.set_value(); release.wait(); });
    await(started, "worker held before external destruction");
    raw->pushTask(std::make_unique<CleanupTask>([&] {
        raw->start();
        require(!raw->isRunning(), "cleanup cannot restart an executor being destroyed");
        require(raw->queueSize() == 0, "cleanup may inspect the queue during destruction");
        cleaned.store(true);
    }, executed));
    std::promise<void> returned;
    auto destroyed = returned.get_future();
    std::thread owner([&] { executor.reset(); returned.set_value(); });
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    // The blocked callback keeps destruction waiting, so raw is still valid.
    while (raw->isRunning() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    require(!raw->isRunning(), "destructor requested stop");
    release.open(); // Do not dereference raw after releasing the callback.
    await(destroyed, "external destruction finishes after reentrant cleanup");
    owner.join();
    require(cleaned.load() && !executed.load(), "pending task cleaned without executing");
}

void restartAfterSelfStop() {
    int context = 0;
    Executor executor(context);
    executor.start();
    std::promise<void> returned;
    auto stopped = returned.get_future();
    enqueue(executor, [&](int&) {
        executor.stop();
        executor.start(); // Worker-side restart is a documented no-op.
        require(!executor.isRunning(), "callback cannot revive its stopped worker");
        returned.set_value();
    });
    await(stopped, "first session self-stopped");
    executor.start(); // Joins the completed previous worker before replacement.
    std::promise<void> ran;
    auto done = ran.get_future();
    enqueue(executor, [&](int& value) { ++value; ran.set_value(); });
    await(done, "external restart accepted a new task");
    executor.stop();
    require(context == 1, "new session ran once");
    require(executor.executedCount() == 2, "both sessions completed");
}

void fifoAndExceptions() {
    int context = 0;
    Executor executor(context);
    enqueue(executor, [](int& value) { value = -100; }); // Prestart: rejected.
    executor.pushTask(nullptr);
    executor.start();
    executor.start(); // Idempotent while running.
    std::vector<int> order;
    for (int i = 0; i < 32; ++i) {
        enqueue(executor, [&, i](int& value) { order.push_back(i); ++value; });
    }
    enqueue(executor, [](int&) { throw std::runtime_error("expected"); });
    enqueue(executor, [](int&) { throw 42; });
    std::promise<void> ran;
    auto done = ran.get_future();
    enqueue(executor, [&](int&) { ran.set_value(); });
    await(done, "tasks continue after exceptions");
    executor.stop();
    executor.stop();
    require(context == 32 && order.size() == 32, "all accepted FIFO tasks ran");
    for (int i = 0; i < 32; ++i) {
        require(order[static_cast<std::size_t>(i)] == i, "FIFO order preserved");
    }
    require(executor.executedCount() == 33, "successful task count");
    require(executor.failedCount() == 2, "exception count");
    enqueue(executor, [](int& value) { value = -100; });
    require(executor.queueSize() == 0 && context == 32, "post-stop task rejected");
}

void concurrentLifecycle() {
    int context = 0;
    Executor executor(context);
    Gate begin;
    std::atomic<int> active{0};
    std::atomic<bool> overlap{false};
    std::vector<std::thread> callers;
    for (int t = 0; t < 4; ++t) {
        callers.emplace_back([&] {
            begin.wait();
            for (int i = 0; i < 100; ++i) {
                executor.start();
                enqueue(executor, [&](int&) {
                    if (active.fetch_add(1) != 0) { overlap.store(true); }
                    std::this_thread::yield();
                    active.fetch_sub(1);
                });
                executor.stop();
            }
        });
    }
    begin.open();
    for (auto& caller : callers) { caller.join(); }
    executor.stop();
    require(!executor.isRunning() && executor.queueSize() == 0, "all sessions stopped");
    require(!overlap.load() && active.load() == 0, "at most one worker executes callbacks");
}

} // namespace

int main(int argc, char** argv) {
    require(argc == 2, "provide one test case name");
    const std::string test = argv[1];
    if (test == "self_stop") { selfStop(); }
    else if (test == "external_worker_stop") { externalAndWorkerStop(); }
    else if (test == "discard_cleanup") { discardedTaskCleanup(); }
    else if (test == "destructor_cleanup") { destructorCleanup(); }
    else if (test == "restart") { restartAfterSelfStop(); }
    else if (test == "fifo_exceptions") { fifoAndExceptions(); }
    else if (test == "concurrent_lifecycle") { concurrentLifecycle(); }
    else { require(false, "unknown test case"); }
    std::cout << "PASS: " << test << '\n';
    return EXIT_SUCCESS;
}
