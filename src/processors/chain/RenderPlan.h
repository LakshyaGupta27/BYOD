#pragma once

#include "processors/BaseProcessor.h"

struct RenderPlanEntry
{
    BaseProcessor* processor = nullptr;
};

struct RenderPlan
{
    std::vector<RenderPlanEntry> entries;
    bool reachesOutput = false;
};