#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>

namespace v3d {

// Hardware threads, at least 1.
unsigned hardwareThreads();

// Runs fn(i) for every i in [0, n) and returns once all have finished. Work
// goes to one process-wide pool of hardwareThreads() - 1 workers, created on
// first use; the calling thread executes tasks too while it waits. Calls may
// nest (a task may call parallelFor): waiting threads keep running queued
// tasks, so nesting neither deadlocks nor adds threads.
void parallelFor(size_t n, const std::function<void(size_t)>& fn);

// Runs independent jobs concurrently on the same pool.
void parallelInvoke(std::initializer_list<std::function<void()>> jobs);

// Creates the pool now instead of on first use. Thread creation blocks while
// another thread is inside dlopen() (glibc serialises them), so a program that
// loads big libraries concurrently, like the viewer loading the GL driver,
// should start the pool before that.
void startParallelPool();

}  // namespace v3d
