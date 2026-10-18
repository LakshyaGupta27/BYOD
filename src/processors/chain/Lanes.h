#pragma once

#include <atomic>
#include <condition_variable>
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
    Barrier* barrier = nullptr;
};

// a thread that isn't part of a group acts like a group with one lane
inline thread_local LaneContext laneContext;

/** Returns the index of the lane that we're running on. */
inline int LaneIdx() noexcept { return laneContext.index; }

/** Waits here until all the other lanes in the group have arrived. */
inline void LaneSync()
{
    if (laneContext.barrier != nullptr)
        laneContext.barrier->wait();
}

/** A group of lanes. The threads are created once and then sleep until there is a task to run. */
class LaneGroup
{
public:
    ~LaneGroup() { stop(); }

    /** The thread that calls run() is lane 0, so this makes (numLanes - 1) threads. */
    void start (int numLanes)
    {
        stop();

        barrier = std::make_unique<Barrier> (numLanes);
        shouldStop = false;

        for (int laneIndex = 1; laneIndex < numLanes; ++laneIndex)
            threads.emplace_back ([this, laneIndex] { threadLoop (laneIndex); });
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

    bool isRunning() const noexcept { return ! threads.empty(); }

private:
    void threadLoop (int laneIndex) // laneIndex gets used in the next step
    {
        std::unique_lock lock { mutex };
        wakeUp.wait (lock, [this] { return shouldStop; });
    }

    std::unique_ptr<Barrier> barrier;
    std::vector<std::thread> threads;

    std::mutex mutex;
    std::condition_variable wakeUp;
    bool shouldStop = false;
};
} // namespace lanes
