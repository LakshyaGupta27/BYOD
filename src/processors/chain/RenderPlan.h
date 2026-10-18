#pragma once

#include "processors/BaseProcessor.h"

struct RenderPlanEntry
{
    BaseProcessor* processor = nullptr;

    // the input port with the buffer that the processor gets called with,
    // or -1 if it should be called with the chain's input buffer
    int inputPort = -1;
};

/**
 * The processors that need to run for each block.
 *
 * The entries are grouped into stages. A processor is always in a later
 * stage than the processors that feed it, so the processors in a single
 * stage don't depend on each other, and can be run at the same time.
 */
struct RenderPlan
{
    std::vector<RenderPlanEntry> entries;
    std::vector<int> stageStart; // the index of the first entry for each stage
    bool reachesOutput = false;
    bool processesInput = false;

    int getNumStages() const noexcept { return (int) stageStart.size(); }

    int getStageEnd (int stageIndex) const noexcept
    {
        if (stageIndex + 1 < getNumStages())
            return stageStart[(size_t) stageIndex + 1];

        return (int) entries.size();
    }

    /** Returns true if any of the stages have more than one processor. */
    bool hasParallelStage() const noexcept
    {
        for (int stageIndex = 0; stageIndex < getNumStages(); ++stageIndex)
        {
            if (getStageEnd (stageIndex) - stageStart[(size_t) stageIndex] > 1)
                return true;
        }

        return false;
    }
};
