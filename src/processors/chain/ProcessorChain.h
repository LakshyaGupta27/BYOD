#pragma once

#include "../ProcessorStore.h"
#include "RenderPlan.h"
#include "ChainIOProcessor.h"
#include "Lanes.h"

#include "../utility/InputProcessor.h"
#include "../utility/OutputProcessor.h"
#include "processors/PlayheadHelpers.h"

class ProcessorChainActionHelper;
class ProcessorChainPortMagnitudesHelper;
class ProcessorChainStateHelper;
class ParamForwardManager;
class ProcessorChain : private AudioProcessorValueTreeState::Listener
{
public:
    ProcessorChain (ProcessorStore& store,
                    AudioProcessorValueTreeState& vts,
                    std::unique_ptr<chowdsp::PresetManager>& presetMgr,
                    std::unique_ptr<ParamForwardManager>& paramForwardManager,
                    std::function<void (int)>&& latencyChangedCallback);
    ~ProcessorChain() override;

    static void createParameters (Parameters& params);
    void prepare (double sampleRate, int samplesPerBlock);
    void processAudio (AudioBuffer<float>& buffer, const MidiBuffer& hostMidiBuffer);
    void reset() noexcept;

    auto& getProcessors() { return procs; }
    const auto& getProcessors() const { return procs; }
    const auto& getConnections() const { return connections; }
    const auto& getRenderPlan() const { return renderPlan; }

    ProcessorStore& getProcStore() { return procStore; }

    InputProcessor& getInputProcessor() { return inputProcessor; }
    OutputProcessor& getOutputProcessor() { return outputProcessor; }

    /** Returns the processor with this ID if it is currently in the chain (including the input/output processors), or nullptr. */
    BaseProcessor* getProcessorByID (ProcessorID id);

    auto& getActionHelper() { return *actionHelper; }
    auto& getStateHelper() { return *stateHelper; }
    auto& getOversampling() { return ioProcessor.getOversampling(); }
    auto& getPlayheadHelper() { return playheadHelper; }

    chowdsp::Broadcaster<void (BaseProcessor*)> processorAddedBroadcaster;
    chowdsp::Broadcaster<void (const BaseProcessor*)> processorRemovedBroadcaster;
    chowdsp::Broadcaster<void()> refreshConnectionsBroadcaster;
    chowdsp::Broadcaster<void (const ConnectionInfo&)> connectionAddedBroadcaster;
    chowdsp::Broadcaster<void (const ConnectionInfo&)> connectionRemovedBroadcaster;

    size_t getRequiredArenaSizeBytes();
    bool needsNewArena (size_t requiredBytes) const;
    static std::span<std::byte> allocArena (size_t bytes);
    static void deallocArena (std::span<std::byte> bytes);

private:
    void initializeProcessors();
    void runProcessor (const RenderPlanEntry& entry);
    void sendOutputsToNextProcessors (BaseProcessor* proc, AudioBuffer<float>& buffer, DSPArena& laneArena);
    void renderOnLane();
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void fillPlan();

    double mySampleRate = 48000.0;
    int mySamplesPerBlock = 512;
    std::vector<Connection> connections;
    RenderPlan renderPlan;

    OwnedArray<BaseProcessor> procs;
    ProcessorStore& procStore;
    SpinLock processingLock;
    UndoManager* um;

    InputProcessor inputProcessor;
    AudioBuffer<float> inputBuffer;
    OutputProcessor outputProcessor;
    ChainIOProcessor ioProcessor;

    std::unique_ptr<chowdsp::PresetManager>& presetManager;

    friend class ProcChainActions;
    friend class AddOrRemoveProcessor;
    friend class AddOrRemoveConnection;

    friend class ProcessorChainActionHelper;
    std::unique_ptr<ProcessorChainActionHelper> actionHelper;

    friend class ProcessorChainStateHelper;
    std::unique_ptr<ProcessorChainStateHelper> stateHelper;

    friend class ProcessorChainPortMagnitudesHelper;
    std::unique_ptr<ProcessorChainPortMagnitudesHelper> portMagsHelper;

    chowdsp::DeferredAction mainThreadAction;
    std::unique_ptr<ParamForwardManager>& paramForwardManager;

    MidiBuffer internalMidiBuffer;
    PlayheadHelpers playheadHelper;

    int connectionsCount {};
    DSPArena arena {};

    // the number of threads that we use for processing (including the audio thread)
    static constexpr int numLanes = 4;
    lanes::LaneGroup laneGroup;
    std::array<DSPArena, (size_t) numLanes> laneArenas {};
    std::function<void()> renderTask;
    std::atomic<int> nextEntryToRun { 0 };

    // for debugging
    std::FILE* laneLogFile = nullptr;
    bool isLoggingThisBlock = false;
    int numBlocksProcessed = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProcessorChain)
};
