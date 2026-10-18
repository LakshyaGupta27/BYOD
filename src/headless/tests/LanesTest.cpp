#include "UnitTests.h"
#include "processors/chain/Lanes.h"

class LanesTest : public UnitTest
{
public:
    LanesTest() : UnitTest ("Lanes Test")
    {
    }

    void noGroupTest()
    {
        expectEquals (lanes::LaneIdx(), 0, "A thread without a group should be lane 0!");
        expectEquals (lanes::LaneCount(), 1, "A thread without a group should have a lane count of 1!");
        lanes::LaneSync(); // this shouldn't block
    }

    void everyLaneRunsTest()
    {
        static constexpr int numLanes = 4;
        static constexpr int numRuns = 200;

        lanes::LaneGroup group;
        group.start (numLanes);

        std::array<std::atomic<int>, numLanes> runsPerLane {};
        std::atomic<int> numBadLaneCounts { 0 };

        for (int i = 0; i < numRuns; ++i)
        {
            group.run ([&]
                       {
                           runsPerLane[(size_t) lanes::LaneIdx()]++;
                           if (lanes::LaneCount() != numLanes)
                               numBadLaneCounts++; });
        }

        for (auto& runs : runsPerLane)
            expectEquals (runs.load(), numRuns, "Every lane should run the task once for each run!");
        expectEquals (numBadLaneCounts.load(), 0, "Lane count is incorrect!");

        expectEquals (lanes::LaneCount(), 1, "Lane count should be reset after running!");
    }

    void laneSyncTest()
    {
        static constexpr int numLanes = 4;
        static constexpr int numSteps = 500;

        lanes::LaneGroup group;
        group.start (numLanes);

        std::atomic<int> counter { 0 };
        std::atomic<int> numBadCounts { 0 };

        group.run ([&]
                   {
                       for (int step = 0; step < numSteps; ++step)
                       {
                           counter++;
                           lanes::LaneSync();

                           // every lane has added to the counter by now
                           if (counter.load() != (step + 1) * numLanes)
                               numBadCounts++;
                           lanes::LaneSync();
                       } });

        expectEquals (numBadCounts.load(), 0, "A lane got past LaneSync() before the other lanes arrived!");
        expectEquals (counter.load(), numSteps * numLanes, "Counter is incorrect!");
    }

    void runTest() override
    {
        beginTest ("No Group Test");
        noGroupTest();

        beginTest ("Every Lane Runs Test");
        everyLaneRunsTest();

        beginTest ("Lane Sync Test");
        laneSyncTest();
    }
};

static LanesTest lanesTest;
