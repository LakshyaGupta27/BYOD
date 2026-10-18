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
 * A group of threads all run the same code. Each thread in the group
 * is a "lane". The function names here are the same as in the article.
 */
namespace lanes
{
/** A point where all the lanes in a group meet before carrying on. */
class Barrier
{
public:
    explicit Barrier (int numLanesToWaitFor) : numLanes (numLanesToWaitFor) {}

    void wait()
    {
        const auto round = roundsDone.load();
        if (numArrived.fetch_add (1) + 1 == numLanes)
        {
            // we're the last lane to arrive, so let everyone go
            numArrived.store (0);
            roundsDone.fetch_add (1);
            return;
        }

        // spin instead of sleeping, since we might be on the audio thread
        while (roundsDone.load() == round)
            std::this_thread::yield();
    }

private:
    const int numLanes;
    std::atomic<int> numArrived { 0 };
    std::atomic<int> roundsDone { 0 };
};

struct LaneContext
{
    int index = 0;
    int count = 1;
    Barrier* barrier = nullptr;
};

// a thread that isn't part of a group acts like a group with one lane
inline thread_local LaneContext laneContext;

/** Returns the index of the lane that we're running on. */
inline int LaneIdx() noexcept
{
    return laneContext.index;
}

/** Returns the number of lanes in the group. */
inline int LaneCount() noexcept
{
    return laneContext.count;
}

/** Waits here until all the other lanes in the group have arrived. */
inline void LaneSync()
{
    if (laneContext.barrier != nullptr)
        laneContext.barrier->wait();
}

/**
 * A group of lanes. The threads are created once and then
 * sleep until there is a task to run.
 */
class LaneGroup
{
public:
    LaneGroup() = default;

    ~LaneGroup()
    {
        stop();
    }

    /** Starts the threads. The thread that calls run() is lane 0, so this makes (numLanes - 1) threads. */
    void start (int numLanesToUse)
    {
        stop();

        numLanes = numLanesToUse;
        barrier = std::make_unique<Barrier> (numLanes);
        shouldStop = false;

        for (int laneIndex = 1; laneIndex < numLanes; ++laneIndex)
            threads.emplace_back ([this, laneIndex]
                                  { threadLoop (laneIndex); });
    }

    void stop()
    {
        {
            const std::lock_guard lock { mutex };
            shouldStop = true;
        }
        wakeUp.notify_all();

        for (auto& thread : threads)
            thread.join();
        threads.clear();
    }

    bool isRunning() const noexcept
    {
        return ! threads.empty();
    }

    /**
     * Runs the task on every lane at the same time, and returns
     * once all the lanes have finished.
     *
     * The task must call LaneSync() the same number of times on every lane.
     */
    void run (const std::function<void()>& taskToRun)
    {
        task = &taskToRun;
        numThreadsRunning.store ((int) threads.size());

        {
            const std::lock_guard lock { mutex };
            ++runCount;
        }
        wakeUp.notify_all();

        runTaskOnLane (0);

        while (numThreadsRunning.load() > 0)
            std::this_thread::yield();
    }

private:
    void threadLoop (int laneIndex)
    {
        int lastRunCount = 0;
        for (;;)
        {
            {
                std::unique_lock lock { mutex };
                wakeUp.wait (lock, [this, lastRunCount]
                             { return shouldStop || runCount != lastRunCount; });

                if (shouldStop)
                    return;

                lastRunCount = runCount;
            }

            runTaskOnLane (laneIndex);
            numThreadsRunning.fetch_sub (1);
        }
    }

    void runTaskOnLane (int laneIndex)
    {
        const auto previousContext = laneContext;
        laneContext = { laneIndex, numLanes, barrier.get() };
        (*task)();
        laneContext = previousContext;
    }

    int numLanes = 1;
    std::unique_ptr<Barrier> barrier;
    std::vector<std::thread> threads;

    std::mutex mutex;
    std::condition_variable wakeUp;
    int runCount = 0;
    bool shouldStop = false;

    const std::function<void()>* task = nullptr;
    std::atomic<int> numThreadsRunning { 0 };
};
} // namespace lanes
