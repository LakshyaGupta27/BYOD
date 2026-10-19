#include "Lanes.h"

#include <cassert>
#include <chrono>

namespace lanes
{
namespace
{
    struct LaneContext
    {
        int index = 0;
        int count = 1;
        Barrier* barrier = nullptr;
    };

    // a thread that isn't part of a group acts like a group with one lane
    thread_local LaneContext laneContext;

    // a sleeping lane is late to the first LaneSync(), which holds up every
    // other lane, so the sleep time needs to be short compared to an audio block
    constexpr int maxSpinsBeforeSleeping = 1000;
    constexpr auto sleepTime = std::chrono::microseconds (50);
} // namespace

void Barrier::wait() noexcept
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

int LaneIdx() noexcept
{
    return laneContext.index;
}

int LaneCount() noexcept
{
    return laneContext.count;
}

void LaneSync() noexcept
{
    if (laneContext.barrier != nullptr)
        laneContext.barrier->wait();
}

LaneGroup::~LaneGroup()
{
    stop();
}

void LaneGroup::start (int numLanesToUse)
{
    assert (numLanesToUse >= 1);
    stop();

    numLanes = numLanesToUse;
    barrier = std::make_unique<Barrier> (numLanes);
    shouldStop.store (false);

    // the new threads should only run tasks that come after this point
    const auto startRunCount = runCount.load();
    threads.reserve ((size_t) numLanes - 1);
    for (int laneIndex = 1; laneIndex < numLanes; ++laneIndex)
        threads.emplace_back ([this, laneIndex, startRunCount]
                              { threadLoop (laneIndex, startRunCount); });
}

void LaneGroup::stop()
{
    shouldStop.store (true);

    for (auto& thread : threads)
        thread.join();
    threads.clear();

    // without the threads, run() only has lane 0, so it mustn't wait for the other lanes
    numLanes = 1;
    barrier.reset();
}

void LaneGroup::run (const std::function<void()>& taskToRun)
{
    task = &taskToRun;
    numThreadsRunning.store ((int) threads.size());

    // this must come last, since the threads start running as soon as they see it change
    runCount.fetch_add (1);

    runTaskOnLane (0);

    while (numThreadsRunning.load() > 0)
        std::this_thread::yield();
}

void LaneGroup::threadLoop (int laneIndex, int lastRunCount)
{
    for (;;)
    {
        // spin for a little while in case the next task comes soon,
        // then back off to short sleeps so we don't hog the CPU
        int numSpins = 0;
        while (! shouldStop.load() && runCount.load() == lastRunCount)
        {
            if (++numSpins < maxSpinsBeforeSleeping)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for (sleepTime);
        }

        if (shouldStop.load())
            return;

        lastRunCount = runCount.load();

        runTaskOnLane (laneIndex);
        numThreadsRunning.fetch_sub (1);
    }
}

void LaneGroup::runTaskOnLane (int laneIndex)
{
    const auto previousContext = laneContext;
    laneContext = { laneIndex, numLanes, barrier.get() };
    (*task)();
    laneContext = previousContext;
}
} // namespace lanes
