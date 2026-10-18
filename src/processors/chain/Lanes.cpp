#include "Lanes.h"

namespace lanes
{
namespace
{
    struct LaneContext
    {
        int index = 0;
        Barrier* barrier = nullptr;
    };

    // a thread that isn't part of a group acts like a group with one lane
    thread_local LaneContext laneContext;
} // namespace

void Barrier::wait()
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

void LaneSync()
{
    if (laneContext.barrier != nullptr)
        laneContext.barrier->wait();
}

LaneGroup::~LaneGroup()
{
    stop();
}

void LaneGroup::start (int numLanes)
{
    stop();

    barrier = std::make_unique<Barrier> (numLanes);
    shouldStop = false;

    for (int laneIndex = 1; laneIndex < numLanes; ++laneIndex)
        threads.emplace_back ([this, laneIndex] { threadLoop (laneIndex); });
}

void LaneGroup::stop()
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

void LaneGroup::run (const std::function<void()>& taskToRun)
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

void LaneGroup::threadLoop (int laneIndex)
{
    int lastRunCount = 0;
    for (;;)
    {
        {
            std::unique_lock lock { mutex };
            wakeUp.wait (lock, [this, &lastRunCount] { return shouldStop || runCount != lastRunCount; });

            if (shouldStop)
                return;

            lastRunCount = runCount;
        }

        runTaskOnLane (laneIndex);
        numThreadsRunning.fetch_sub (1);
    }
}

void LaneGroup::runTaskOnLane (int laneIndex)
{
    laneContext = { laneIndex, barrier.get() };
    (*task)();
    laneContext = {};
}
} // namespace lanes
