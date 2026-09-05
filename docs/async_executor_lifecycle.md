# Async output executor lifecycle

`state_machine::runtime::AsyncTaskExecutor<Context>` is an optional, header-only
output helper, separate from the deterministic `StateMachine` task registry.
It does not validate state-machine task handles or control-session ownership.

## Lifecycle contract

- External `start()` and `stop()` calls serialize changes to the worker handle.
  Repeated `start()` while running is a no-op. An external restart first joins
  any previous worker that requested its own stop.
- `stop()` called by a task requests shutdown, discards queued tasks, and
  returns without joining itself. The current task is allowed to finish.
  A later external `stop()`, restart, or destructor joins that worker even if
  `isRunning()` is already false.
- `start()` called from this executor's worker is a no-op, including after a
  worker-side stop. Restart must come from an external lifecycle owner.
- Pending task/capture destructors run outside the queue and lifecycle locks.
  Cleanup may therefore inspect `queueSize()` or request another `stop()`.
- `pushTask(nullptr)` and submissions while stopped are ignored, as before.
  Submissions racing with stop are either discarded or rejected. Concurrent
  start/stop calls have mutex acquisition order, not business-command priority.
- The context and executor must outlive all callbacks. Destroying the executor
  from its own callback is not supported. External destruction waits for the
  current callback, discards queued work, and prevents cleanup from restarting
  the executor.

`isRunning() == false` means shutdown was requested; it does not prove that
an in-flight callback has completed. External `stop()` provides that join,
unless another caller concurrently starts a new session afterwards.

## Boundaries that remain the consumer's responsibility

The executor remains a single FIFO worker. It has no queue capacity, deadline,
priority, or preemption guarantee. An indefinitely blocked callback still
blocks an external stop/join. Do not implement cancellation by detaching a
worker that still references the context.

Time-critical setpoint/zero-command publication must not share a blocking
service lane without an independently verified timing budget. This lifecycle
fix does not solve head-of-line blocking, cancel an in-flight service call,
or invalidate output produced for an obsolete controller session. Those need
explicit consumer-side output lanes, bounded operations, and session checks.

## Regression tests

The standalone C++17 test executable is registered as seven CTest cases with
per-case timeouts: worker self-stop, simultaneous external/worker stop,
reentrant discarded-task cleanup, cleanup during destruction, restart after
self-stop, FIFO/exception compatibility, and concurrent external lifecycle stress.

For a header-only local check without ROS, GTest, or the shared-library build:

```bash
g++ -std=c++17 -Wall -Wextra -Werror -pthread -Iinclude \
  test/async_task_executor_test.cpp -o /tmp/async_task_executor_test
for case in self_stop external_worker_stop discard_cleanup destructor_cleanup \
            restart fifo_exceptions concurrent_lifecycle; do
  timeout 15s /tmp/async_task_executor_test "$case" || exit 1
done
```

For the full project (GTest installed):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

ThreadSanitizer and AddressSanitizer/UndefinedBehaviorSanitizer must be built
and run separately; do not combine their instrumentation in one binary.
