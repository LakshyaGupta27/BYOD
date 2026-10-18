#include "UnitTests.h"
#include "processors/chain/ProcessorChainActionHelper.h"

class RenderPlanTest : public UnitTest
{
public:
    RenderPlanTest() : UnitTest ("Render Plan Test")
    {
    }

    static constexpr double testSampleRate = 48000.0;
    static constexpr int testBlockSize = 8192;

    /** Returns the stage that the processor will run in, or -1 if it won't run. */
    static int getStageFor (const RenderPlan& plan, const BaseProcessor* proc)
    {
        for (int stageIndex = 0; stageIndex < plan.getNumStages(); ++stageIndex)
        {
            for (int i = plan.stageStart[(size_t) stageIndex]; i < plan.getStageEnd (stageIndex); ++i)
            {
                if (plan.entries[(size_t) i].processor == proc)
                    return stageIndex;
            }
        }

        return -1;
    }

    /** Sets up: input -> splitter -> (gain, gain) -> merger -> output */
    static void setupParallelGains (BYOD& plugin)
    {
        auto* undoManager = plugin.getVTS().undoManager;
        auto& chain = plugin.getProcChain();
        auto& actionHelper = chain.getActionHelper();

        actionHelper.addProcessor (ProcessorStore::getStoreMap().at ("Stereo Splitter").factory (undoManager));
        actionHelper.addProcessor (ProcessorStore::getStoreMap().at ("Clean Gain").factory (undoManager));
        actionHelper.addProcessor (ProcessorStore::getStoreMap().at ("Clean Gain").factory (undoManager));
        actionHelper.addProcessor (ProcessorStore::getStoreMap().at ("Stereo Merger").factory (undoManager));

        auto* input = &chain.getInputProcessor();
        auto* splitter = chain.getProcessors()[0];
        auto* leftGain = chain.getProcessors()[1];
        auto* rightGain = chain.getProcessors()[2];
        auto* merger = chain.getProcessors()[3];
        auto* output = &chain.getOutputProcessor();

        actionHelper.removeConnection ({ input, 0, output, 0 });
        actionHelper.addConnection ({ input, 0, splitter, 0 });
        actionHelper.addConnection ({ splitter, 0, leftGain, 0 });
        actionHelper.addConnection ({ splitter, 1, rightGain, 0 });
        actionHelper.addConnection ({ leftGain, 0, merger, 0 });
        actionHelper.addConnection ({ rightGain, 0, merger, 1 });
        actionHelper.addConnection ({ merger, 0, output, 0 });
    }

    void defaultChainTest()
    {
        BYOD plugin;
        auto& chain = plugin.getProcChain();
        plugin.prepareToPlay (testSampleRate, testBlockSize);

        const auto& plan = chain.getRenderPlan();
        expectEquals (plan.getNumStages(), 2, "Number of stages is incorrect!");
        expectEquals (getStageFor (plan, &chain.getInputProcessor()), 0, "Input should be in the first stage!");
        expectEquals (getStageFor (plan, &chain.getOutputProcessor()), 1, "Output should be in the second stage!");
        expect (plan.reachesOutput, "Plan should reach the output!");
        expect (! plan.hasParallelStage(), "Default chain should not have any parallel stages!");
    }

    void parallelStageTest()
    {
        BYOD plugin;
        auto& chain = plugin.getProcChain();
        plugin.prepareToPlay (testSampleRate, testBlockSize);
        setupParallelGains (plugin);

        const auto& plan = chain.getRenderPlan();
        expectEquals (plan.getNumStages(), 5, "Number of stages is incorrect!");
        expectEquals ((int) plan.entries.size(), 6, "Number of processors in the plan is incorrect!");
        expect (plan.reachesOutput, "Plan should reach the output!");
        expect (plan.hasParallelStage(), "Plan should have a parallel stage!");

        expectEquals (getStageFor (plan, &chain.getInputProcessor()), 0, "Input stage is incorrect!");
        expectEquals (getStageFor (plan, chain.getProcessors()[0]), 1, "Splitter stage is incorrect!");
        expectEquals (getStageFor (plan, chain.getProcessors()[1]), 2, "Left gain stage is incorrect!");
        expectEquals (getStageFor (plan, chain.getProcessors()[2]), 2, "Right gain stage is incorrect!");
        expectEquals (getStageFor (plan, chain.getProcessors()[3]), 3, "Merger stage is incorrect!");
        expectEquals (getStageFor (plan, &chain.getOutputProcessor()), 4, "Output stage is incorrect!");

        // a processor should always come after the processors that feed it
        for (const auto& connection : chain.getConnections())
        {
            const auto sourceStage = getStageFor (plan, chain.getProcessorByID (connection.sourceProcessor));
            const auto destinationStage = getStageFor (plan, chain.getProcessorByID (connection.destinationProcessor));
            expectLessThan (sourceStage, destinationStage, "Source processor should run before the destination!");
        }
    }

    void unconnectedProcessorTest()
    {
        BYOD plugin;
        auto* undoManager = plugin.getVTS().undoManager;
        auto& chain = plugin.getProcChain();
        auto& actionHelper = chain.getActionHelper();
        plugin.prepareToPlay (testSampleRate, testBlockSize);

        actionHelper.addProcessor (ProcessorStore::getStoreMap().at ("Clean Gain").factory (undoManager));
        auto* gain = chain.getProcessors()[0];
        expectEquals (getStageFor (chain.getRenderPlan(), gain), -1, "A processor with no connections should not run!");

        // the gain has an input now, but its output still goes nowhere
        actionHelper.addConnection ({ &chain.getInputProcessor(), 0, gain, 0 });
        expectEquals (getStageFor (chain.getRenderPlan(), gain), -1, "A processor with nothing connected to its output should not run!");

        actionHelper.removeConnection ({ &chain.getInputProcessor(), 0, &chain.getOutputProcessor(), 0 });
        actionHelper.addConnection ({ gain, 0, &chain.getOutputProcessor(), 0 });
        expectEquals (getStageFor (chain.getRenderPlan(), gain), 1, "A connected processor should run!");
        expect (chain.getRenderPlan().reachesOutput, "Plan should reach the output!");
    }

    void parallelAudioTest()
    {
        BYOD plugin;
        auto& chain = plugin.getProcChain();
        plugin.prepareToPlay (testSampleRate, testBlockSize);
        setupParallelGains (plugin);

        // set to stereo
        plugin.getVTS().getParameter ("mono_mode")->setValueNotifyingHost (0.35f);
        MessageManager::getInstance()->runDispatchLoopUntil (100);

        MidiBuffer midi;
        AudioBuffer<float> buffer (2, testBlockSize);
        buffer.clear();
        chain.processAudio (buffer, midi);

        // the left and right channels go through different gain processors,
        // so if the two get mixed up we'll see it in the output
        for (int i = 0; i < 20; ++i)
        {
            FloatVectorOperations::fill (buffer.getWritePointer (0), 1.0f, testBlockSize);
            FloatVectorOperations::fill (buffer.getWritePointer (1), -1.0f, testBlockSize);
            chain.processAudio (buffer, midi);

            auto leftMinMax = FloatVectorOperations::findMinAndMax (buffer.getReadPointer (0) + testBlockSize / 2, testBlockSize / 2);
            auto rightMinMax = FloatVectorOperations::findMinAndMax (buffer.getReadPointer (1) + testBlockSize / 2, testBlockSize / 2);

            expectGreaterThan (leftMinMax.getStart(), 0.4f, "Left channel minimum should be > 0!");
            expectLessThan (rightMinMax.getEnd(), -0.4f, "Right channel maximum should be < 0!");
        }
    }

    void runTest() override
    {
        beginTest ("Default Chain Test");
        defaultChainTest();

        beginTest ("Parallel Stage Test");
        parallelStageTest();

        beginTest ("Unconnected Processor Test");
        unconnectedProcessorTest();

        beginTest ("Parallel Audio Test");
        parallelAudioTest();
    }
};

static RenderPlanTest renderPlanTest;
