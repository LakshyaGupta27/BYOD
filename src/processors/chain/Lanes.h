#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

/**
 * Multi-core helpers, based on Ryan Fleury's "Multi-Core By Default":
 * https://www.dgtlgrove.com/p/multi-core-by-default
 *
 * A group of threads all run the same code. Each thread in the group is a "lane".
 * See the sections "Multi-Core By Default" and "Codebase Support > Thread-Local Group Data".
 */
namespace lanes
{
/** A point where all the lanes in a group meet before carrying on. */
class Barrier
{
public:
    explicit Barrier (int numLanesToWaitFor) : numLanes (numLanesToWaitFor) {}

    void wait();

private:
    const int numLanes;
    std::atomic<int> numArrived { 0 };
    std::atomic<int> roundsDone { 0 };
};

/** Returns the index of the lane that we're running on. */
int LaneIdx() noexcept;

/** Waits here until all the other lanes in the group have arrived. */
void LaneSync();

/** A group of lanes. The threads are created once and then sleep until there is a task to run. */
class LaneGroup
{
public:
    ~LaneGroup();

    /** The thread that calls run() is lane 0, so this makes (numLanes - 1) threads. */
    void start (int numLanes);
    void stop();

    bool isRunning() const noexcept { return ! threads.empty(); }

    /** Runs the task on every lane at the same time, and returns once all the lanes have finished. */
    void run (const std::function<void()>& taskToRun);

private:
    void threadLoop (int laneIndex);
    void runTaskOnLane (int laneIndex);

    std::unique_ptr<Barrier> barrier;
    std::vector<std::thread> threads;

    std::mutex mutex;
    std::condition_variable wakeUp;
    bool shouldStop = false;
    int runCount = 0;
    const std::function<void()>* task = nullptr;
    std::atomic<int> numThreadsRunning { 0 };
};
} // namespace lanes
