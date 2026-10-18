#include "ProcessorChain.h"
#include "ProcessorChainActionHelper.h"
#include "ProcessorChainPortMagnitudesHelper.h"
#include "ProcessorChainStateHelper.h"
#include "processors/chain/ChainIOProcessor.h"

namespace ChainHelperFuncs
{
[[maybe_unused]] static void printBufferLevels (const AudioBuffer<float>& buffer)
{
    const auto numChannels = buffer.getNumChannels();
    const auto numSamples = buffer.getNumSamples();
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto level = buffer.getRMSLevel (ch, 0, numSamples);
        std::cout << "Level for channel: " << ch << ": " << level << std::endl;
    }
}

static const MidiBuffer& getMidiBufferToUse (const MidiBuffer& hostMidiBuffer, MidiBuffer& internalMidiBuffer, int osFactor)
{
    if (hostMidiBuffer.isEmpty() || osFactor == 1)
        return hostMidiBuffer;

    internalMidiBuffer.clear();
    for (const auto& midiEvent : hostMidiBuffer)
        internalMidiBuffer.addEvent (midiEvent.getMessage(), midiEvent.samplePosition * osFactor);

    return internalMidiBuffer;
}
} // namespace ChainHelperFuncs

ProcessorChain::ProcessorChain (ProcessorStore& store,
                                AudioProcessorValueTreeState& vts,
                                std::unique_ptr<chowdsp::PresetManager>& presetMgr,
                                std::unique_ptr<ParamForwardManager>& paramForwarder,
                                std::function<void (int)>&& latencyChangedCallback)
    : procStore (store),
      um (vts.undoManager),
      inputProcessor (um),
      outputProcessor (um),
      ioProcessor (vts, std::move (latencyChangedCallback)),
      presetManager (presetMgr),
      paramForwardManager (paramForwarder)
{
    actionHelper = std::make_unique<ProcessorChainActionHelper> (*this);
    stateHelper = std::make_unique<ProcessorChainStateHelper> (*this, mainThreadAction);
    portMagsHelper = std::make_unique<ProcessorChainPortMagnitudesHelper> (*this);

    procs.ensureStorageAllocated (100);
    connections.reserve (100);
    renderPlan.entries.reserve (100);

    renderTask = [this]
    { renderOnLane(); };

    // for debugging: set BYOD_LANE_LOG to a file path to log which lane each processor runs on
    if (const auto* laneLogPath = std::getenv ("BYOD_LANE_LOG"))
        laneLogFile = std::fopen (laneLogPath, "w");
}

ProcessorChain::~ProcessorChain()
{
    if (laneLogFile != nullptr)
        std::fclose (laneLogFile);
}

BaseProcessor* ProcessorChain::getProcessorByID (ProcessorID id)
{
    if (inputProcessor.getID() == id)
        return &inputProcessor;

    if (outputProcessor.getID() == id)
        return &outputProcessor;

    for (auto* proc : procs)
    {
        if (proc->getID() == id)
            return proc;
    }

    return nullptr;
}

void ProcessorChain::createParameters (Parameters& params)
{
    ChainIOProcessor::createParameters (params);
}

void ProcessorChain::initializeProcessors()
{
    const auto osFactor = ioProcessor.getOversamplingFactor();
    const double osSampleRate = mySampleRate * osFactor;
    const int osSamplesPerBlock = mySamplesPerBlock * osFactor;

    inputProcessor.prepareProcessing (osSampleRate, osSamplesPerBlock);
    outputProcessor.prepareProcessing (osSampleRate, osSamplesPerBlock);

    for (int i = procs.size() - 1; i >= 0; --i)
    {
        if (auto* proc = procs[i])
            proc->prepareProcessing (osSampleRate, osSamplesPerBlock);
    }

    deallocArena (arena.get_memory_resource());
    arena.get_memory_resource() = allocArena (getRequiredArenaSizeBytes());
}

void ProcessorChain::prepare (double sampleRate, int samplesPerBlock)
{
    mySampleRate = sampleRate;
    mySamplesPerBlock = samplesPerBlock;

    inputBuffer.setSize (2, samplesPerBlock * 16); // allocate extra space for upsampled buffers

    ioProcessor.prepare (sampleRate, samplesPerBlock);

    internalMidiBuffer.clear();
    internalMidiBuffer.ensureSize (256);

    if (! laneGroup.isRunning())
        laneGroup.start (numLanes);

    SpinLock::ScopedLockType scopedProcessingLock (processingLock);
    initializeProcessors();
}

void ProcessorChain::reset() noexcept
{
    ioProcessor.reset();
}

void ProcessorChain::runProcessor (const RenderPlanEntry& entry)
{
    TRACE_DSP();

    auto* proc = entry.processor;
    auto& laneArena = laneArenas[(size_t) lanes::LaneIdx()];
    proc->arena = &laneArena;

    if (isLoggingThisBlock)
        std::fprintf (laneLogFile, "Processing pedal %s on lane %d (thread %p)\n", proc->getName().toRawUTF8(), lanes::LaneIdx(), Thread::getCurrentThreadId());

    if (entry.inputPort < 0)
    {
        proc->processAudioBlock (inputBuffer);
        sendOutputsToNextProcessors (proc, inputBuffer, laneArena);
    }
    else
    {
        auto buffer = proc->getInputBufferNonConst (entry.inputPort);
        proc->processAudioBlock (buffer);
        sendOutputsToNextProcessors (proc, buffer, laneArena);
    }
}

void ProcessorChain::sendOutputsToNextProcessors (BaseProcessor* proc, AudioBuffer<float>& buffer, DSPArena& laneArena)
{
    const int numOutputs = proc->getNumOutputs();

    int nextNumProcs = 0;
    for (int i = 0; i < numOutputs; ++i)
        nextNumProcs += proc->getNumOutputConnections (i);

    for (int i = 0; i < numOutputs; ++i)
    {
        auto outBufferView = proc->getOutputBuffer (i);
        if (outBufferView.getNumSamples() == 0)
            outBufferView = buffer;
        auto outBuffer = outBufferView.toAudioBuffer();

        const int numOutProcs = proc->getNumOutputConnections (i);
        for (int j = numOutProcs - 1; j >= 0; --j)
        {
            const auto& connectionInfo = proc->getOutputConnection (i, j);
            auto* nextProc = connectionInfo.endProc;

            if (nextNumProcs == 1 && nextProc->getNumInputs() == 1)
            {
                // this is the last processor that needs this buffer, so it can have the buffer itself
                nextProc->getInputBufferView() = outBuffer;
            }
            else
            {
                // every other processor gets its own copy
                auto bufferView = laneArena.alloc_buffer (outBuffer);
                chowdsp::BufferMath::copyBufferData (outBuffer, bufferView);
                nextProc->getInputBufferView (connectionInfo.endPort) = bufferView;
            }

            nextNumProcs -= 1;
        }
    }
}

void ProcessorChain::renderOnLane()
{
    const juce::ScopedNoDenormals noDenormals {};

    if (lanes::LaneIdx() == 0)
        nextEntryToRun.store (0);
    lanes::LaneSync();

    for (int stageIndex = 0; stageIndex < renderPlan.getNumStages(); ++stageIndex)
    {
        const auto stageEnd = renderPlan.getStageEnd (stageIndex);

        // each lane keeps taking the next processor in the stage until there are none left
        for (;;)
        {
            auto entryIndex = nextEntryToRun.load();
            if (entryIndex >= stageEnd)
                break;

            if (nextEntryToRun.compare_exchange_weak (entryIndex, entryIndex + 1))
                runProcessor (renderPlan.entries[(size_t) entryIndex]);
        }

        // the next stage needs the output from this one, so wait for all the lanes to finish
        lanes::LaneSync();
    }
}

void ProcessorChain::processAudio (AudioBuffer<float>& buffer, const MidiBuffer& hostMidiBuffer)
{
    SpinLock::ScopedTryLockType tryProcessingLock (processingLock);
    if (! tryProcessingLock.isLocked())
        return;

    // process input (oversampling, input gain, etc)
    bool sampleRateChange = false;
    auto osBlock = ioProcessor.processAudioInput (buffer, sampleRateChange);
    if (sampleRateChange)
        initializeProcessors();

    // prepare port magnitudes
    portMagsHelper->preparePortMagnitudes();

    const auto osNumSamples = (int) osBlock.getNumSamples();
    const auto inputNumChannels = (int) osBlock.getNumChannels();

    // process mono or stereo buffer?
    {
        inputBuffer.setSize (inputNumChannels, osNumSamples, false, false, true);
        inputBuffer.clear();

        for (int ch = 0; ch < inputNumChannels; ++ch)
            inputBuffer.copyFrom (ch, 0, osBlock.getChannelPointer ((size_t) ch), osNumSamples);
    }

    const auto& processMidiBuffer = ChainHelperFuncs::getMidiBufferToUse (hostMidiBuffer, internalMidiBuffer, ioProcessor.getOversamplingFactor());

    for (auto* processor : procs)
        processor->midiBuffer = &processMidiBuffer;

    // each lane gets its own part of the arena
    const auto arenaMemory = arena.get_memory_resource();
    const auto arenaBytesPerLane = arenaMemory.size() / (size_t) numLanes;
    for (size_t i = 0; i < (size_t) numLanes; ++i)
        laneArenas[i].get_memory_resource() = arenaMemory.subspan (i * arenaBytesPerLane, arenaBytesPerLane);

    // writing to the log on every block would be way too slow, so we only log one block every so often
    isLoggingThisBlock = laneLogFile != nullptr && (++numBlocksProcessed % 200 == 0);
    if (isLoggingThisBlock)
        std::fprintf (laneLogFile, "--- block %d ---\n", numBlocksProcessed);

    // run processing chain
    if (renderPlan.hasParallelStage() && laneGroup.isRunning())
        laneGroup.run (renderTask);
    else
        renderOnLane();

    if (isLoggingThisBlock)
        std::fflush (laneLogFile);

    for (auto* processor : procs)
        processor->midiBuffer = nullptr;

    if (! renderPlan.processesInput)
        inputProcessor.resetLevels();

    if (! renderPlan.reachesOutput)
    {
        outputProcessor.resetLevels();
        inputBuffer.clear();
        ioProcessor.processAudioOutput (inputBuffer, buffer);
    }
    else
    {
        // do output processing (downsampling, output gain)
        if (auto outBuffer = outputProcessor.getOutputBuffer(); outBuffer.getNumSamples() > 0)
        {
            ioProcessor.processAudioOutput (outBuffer.toAudioBuffer(), buffer);
        }
        else
        {
            jassertfalse; // output buffer is null after output was processed?
        }
    }

    for (auto& laneArena : laneArenas)
        laneArena.clear();
}

void ProcessorChain::parameterChanged (const juce::String& /*parameterID*/, float /*newValue*/)
{
    mainThreadAction.call ([this]
                           {
                               jassert (presetManager != nullptr);

                               if (! presetManager->getIsDirty())
                                   presetManager->setIsDirty (true); },
                           true);
}

void ProcessorChain::fillPlan()
{
    renderPlan.entries.clear();
    renderPlan.stageStart.clear();
    renderPlan.reachesOutput = false;
    renderPlan.processesInput = false;

    const auto countConnectionsFrom = [this] (const BaseProcessor* proc)
    {
        return (int) std::count_if (connections.begin(), connections.end(), [id = proc->getID()] (const Connection& connection)
                                    { return connection.sourceProcessor == id; });
    };

    const auto countConnectionsTo = [this] (const BaseProcessor* proc)
    {
        return (int) std::count_if (connections.begin(), connections.end(), [id = proc->getID()] (const Connection& connection)
                                    { return connection.destinationProcessor == id; });
    };

    const auto isModulationOutputConnected = [this] (const BaseProcessor* proc)
    {
        if (proc->getProcessorType() != Modulation)
            return false;

        return std::any_of (connections.begin(), connections.end(), [proc] (const Connection& connection)
                            { return connection.sourceProcessor == proc->getID() && proc->getOutputPortType (connection.sourcePort) == PortType::modulation; });
    }; 

    // the first stage has the standalone modulation processors and the input processor
    std::vector<RenderPlanEntry> stage;
    for (auto* proc : procs)
    {
        const auto noInputsConnected = countConnectionsTo (proc) == 0;
        if (noInputsConnected && (isModulationOutputConnected (proc) || proc->onlyHasModulationOutput()))
            stage.push_back ({ proc, -1 });
    }
    stage.push_back ({ &inputProcessor, -1 });

    std::unordered_map<const BaseProcessor*, int> numInputsDelivered;
    std::vector<RenderPlanEntry> nextStage;

    while (! stage.empty())
    {
        const auto stageStartIndex = (int) renderPlan.entries.size();

        for (const auto& entry : stage)
        {
            auto* proc = entry.processor;

            if (proc == &outputProcessor) // we've reached the output processor, so we're done!
            {
                renderPlan.entries.push_back (entry);
                renderPlan.reachesOutput = true;
                continue;
            }

            const auto numOutputs = proc->getNumOutputs();
            if (numOutputs == 0) // this processor has no outputs, so after we process, we're done!
            {
                renderPlan.entries.push_back (entry);
                continue;
            }

            int numAudioOutputs = 0;
            for (int portIdx = 0; portIdx < numOutputs; ++portIdx)
                numAudioOutputs += proc->getOutputPortType (portIdx) == PortType::audio ? 1 : 0;

            if (countConnectionsFrom (proc) == 0 && numAudioOutputs > 0) // the output of this processor is connected to nothing, so it doesn't run
                continue;

            renderPlan.entries.push_back (entry);
            if (proc == &inputProcessor)
                renderPlan.processesInput = true;

            // a processor goes in the next stage once all of its inputs are ready
            for (const auto& connection : connections)
            {
                if (connection.sourceProcessor != proc->getID())
                    continue;

                auto* nextProc = getProcessorByID (connection.destinationProcessor);
                if (nextProc == nullptr)
                {
                    jassertfalse; // the connection table has a processor that isn't in the chain!
                    continue;
                }

                if (++numInputsDelivered[nextProc] < countConnectionsTo (nextProc))
                    continue; // not all the inputs are ready yet...

                nextStage.push_back ({ nextProc, connection.destinationPort });
            }
        }

        if ((int) renderPlan.entries.size() > stageStartIndex)
            renderPlan.stageStart.push_back (stageStartIndex);

        stage = nextStage;
        nextStage.clear();
    }
}

size_t ProcessorChain::getRequiredArenaSizeBytes()
{
    const auto osFactor = ioProcessor.getOversamplingFactor();
    const int osSamplesPerBlock = mySamplesPerBlock * osFactor;
    const auto osSamplesPerBlockPadded = chowdsp::Math::round_to_next_multiple (osSamplesPerBlock, 4);
    const auto bufferSizeBytes = osSamplesPerBlockPadded * 2 * sizeof (float);

    const auto numIOBuffers = chowdsp::Math::round_to_next_multiple (2 * connectionsCount + 1, 10);
    const auto ioBufferBytes = numIOBuffers * bufferSizeBytes;

    static constexpr size_t blockSize = 8192;
    const auto totalNumBytes = chowdsp::Math::round_to_next_multiple (ioBufferBytes, blockSize);

    // each lane needs enough space to do all of the processing by itself
    return totalNumBytes * (size_t) numLanes;
}

bool ProcessorChain::needsNewArena (size_t requiredBytes) const
{
    const auto currentArenaBytes = arena.get_memory_resource().size();

    // If the current arena is too small then we need a new one
    if (currentArenaBytes < requiredBytes)
        return true;

    // If the current arena if way too big (more than 20%), then we need a new one
    if (currentArenaBytes * 4 / 5 > requiredBytes)
        return true;

    return false;
}

std::span<std::byte> ProcessorChain::allocArena (size_t bytes)
{
    juce::Logger::writeToLog ("Allocating arena of size: " + juce::String { bytes });
    return {
        (std::byte*) chowdsp::aligned_alloc (16, bytes),
        bytes,
    };
}

void ProcessorChain::deallocArena (std::span<std::byte> bytes)
{
    if (bytes.empty())
        return;

    juce::Logger::writeToLog ("De-allocating arena of size: " + juce::String { bytes.size() });
    chowdsp::aligned_free (bytes.data());
}
