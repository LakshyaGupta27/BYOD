#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

/**
 * Multi-core helpers, based on Ryan Fleury's "Multi-Core By Default":
 * https://www.dgtlgrove.com/p/multi-core-by-default
 *
 * A group of threads all run the same code. Each thread in the group
 * is a "lane". The function names here are the same as in the article.
 *
 * Nothing here takes a lock, so it's all safe to use on the audio thread.
 */
namespace lanes
{
/** A point where all the lanes in a group meet before carrying on. */
class Barrier
{
public:
    explicit Barrier (int numLanesToWaitFor) noexcept : numLanes (numLanesToWaitFor) {}

    Barrier (const Barrier&) = delete;
    Barrier& operator= (const Barrier&) = delete;

    void wait() noexcept;

private:
    const int numLanes;
    std::atomic<int> numArrived { 0 };
    std::atomic<int> roundsDone { 0 };
};

/** Returns the index of the lane that we're running on. */
[[nodiscard]] int LaneIdx() noexcept;

/** Returns the number of lanes in the group. */
[[nodiscard]] int LaneCount() noexcept;

/** Waits here until all the other lanes in the group have arrived. */
void LaneSync() noexcept;

/**
 * A group of lanes. The threads are created once and then
 * wait until there is a task to run.
 */
class LaneGroup
{
public:
    LaneGroup() = default;
    ~LaneGroup();

    // the threads hold a pointer to the group, so it can't be copied or moved
    LaneGroup (const LaneGroup&) = delete;
    LaneGroup& operator= (const LaneGroup&) = delete;

    /** Starts the threads. The thread that calls run() is lane 0, so this makes (numLanes - 1) threads. */
    void start (int numLanesToUse);
    void stop();

    [[nodiscard]] bool isRunning() const noexcept { return ! threads.empty(); }

    /**
     * Runs the task on every lane at the same time, and returns
     * once all the lanes have finished.
     *
     * The task must call LaneSync() the same number of times on every lane.
     */
    void run (const std::function<void()>& taskToRun);

private:
    void threadLoop (int laneIndex, int lastRunCount);
    void runTaskOnLane (int laneIndex);

    int numLanes = 1;
    std::unique_ptr<Barrier> barrier;
    std::vector<std::thread> threads;

    std::atomic<int> runCount { 0 };
    std::atomic<bool> shouldStop { false };

    const std::function<void()>* task = nullptr;
    std::atomic<int> numThreadsRunning { 0 };
};
} // namespace lanes
