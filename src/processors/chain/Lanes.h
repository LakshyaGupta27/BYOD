#pragma once

#include <atomic>
#include <thread>

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
};

// a thread that isn't part of a group acts like a group with one lane
inline thread_local LaneContext laneContext;

/** Returns the index of the lane that we're running on. */
inline int LaneIdx() noexcept { return laneContext.index; }
} // namespace lanes
