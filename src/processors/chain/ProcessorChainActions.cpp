#include "ProcessorChainActions.h"
#include "ProcessorChainActionHelper.h"

namespace ProcessorChainHelpers
{
void removeOutputConnectionsFromProcessor (ProcessorChain& chain, BaseProcessor* proc, UndoManager* um)
{
    for (int portIndex = 0; portIndex < proc->getNumOutputs(); ++portIndex)
    {
        auto numOutputConnections = proc->getNumOutputConnections (portIndex);
        while (numOutputConnections > 0)
        {
            auto connection = proc->getOutputConnection (portIndex, numOutputConnections - 1);
            um->perform (new AddOrRemoveConnection (chain, std::move (connection), true));
            numOutputConnections = proc->getNumOutputConnections (portIndex);
        }
    }
}
} // namespace ProcessorChainHelpers

class ProcChainActions
{
public:
    static void addProcessor (ProcessorChain& chain, BaseProcessor::Ptr newProc)
    {
        Logger::writeToLog (String ("Creating processor: ") + newProc->getName());

        newProc->playheadHelpers = &chain.playheadHelper;
        auto osFactor = chain.ioProcessor.getOversamplingFactor();
        newProc->prepareProcessing (osFactor * chain.mySampleRate, osFactor * chain.mySamplesPerBlock);

        BaseProcessor* newProcPtr = nullptr;
        {
            SpinLock::ScopedLockType scopedProcessingLock { chain.processingLock };
            newProcPtr = chain.procs.add (std::move (newProc));
            chain.fillPlan();
        }

        for (auto* param : newProcPtr->getParameters())
        {
            if (auto* paramCast = dynamic_cast<juce::RangedAudioParameter*> (param))
                newProcPtr->getVTS().addParameterListener (paramCast->paramID, &chain);
        }

        chain.processorAddedBroadcaster (newProcPtr);
    }

    static void removeProcessor (ProcessorChain& chain, BaseProcessor* procToRemove, BaseProcessor::Ptr& saveProc)
    {
        Logger::writeToLog (String ("Removing processor: ") + procToRemove->getName());

        ProcessorChainHelpers::removeOutputConnectionsFromProcessor (chain, procToRemove, chain.um);

        chain.processorRemovedBroadcaster (procToRemove);

        for (auto* param : procToRemove->getParameters())
        {
            if (auto* paramCast = dynamic_cast<juce::RangedAudioParameter*> (param))
                procToRemove->getVTS().removeParameterListener (paramCast->paramID, &chain);
        }

        {
            SpinLock::ScopedLockType scopedProcessingLock { chain.processingLock };
            saveProc.reset (chain.procs.removeAndReturn (chain.procs.indexOf (procToRemove)));
            chain.fillPlan();
        }
        saveProc->freeInternalMemory();
    }

    static void addConnection (ProcessorChain& chain, const ConnectionInfo& info)
    {
        Logger::writeToLog (String ("Adding connection from ") + info.startProc->getName() + ", port #"
                            + String (info.startPort) + " to " + info.endProc->getName() + " port #"
                            + String (info.endPort));

        chain.connectionsCount++;
        const auto arenaBytes = chain.getRequiredArenaSizeBytes();
        const auto needsNewArena = chain.needsNewArena (arenaBytes);
        std::span<std::byte> arenaData {};
        if (needsNewArena)
            arenaData = chain.allocArena (arenaBytes);

        const auto connection = toConnection (info);
        {
            SpinLock::ScopedLockType scopedProcessingLock { chain.processingLock };
            info.startProc->addConnection (ConnectionInfo (info));
            chain.connections.push_back (connection);
            chain.fillPlan();
            if (needsNewArena)
                std::swap (arenaData, chain.arena.get_memory_resource());
        }
        chain.deallocArena (arenaData);
        chain.connectionAddedBroadcaster (info);
    }

    static void removeConnection (ProcessorChain& chain, const ConnectionInfo& info)
    {
        Logger::writeToLog (String ("Removing connection from ") + info.startProc->getName() + ", port #"
                            + String (info.startPort) + " to " + info.endProc->getName() + " port #"
                            + String (info.endPort));

        chain.connectionsCount--;
        const auto arenaBytes = chain.getRequiredArenaSizeBytes();
        const auto needsNewArena = chain.needsNewArena (arenaBytes);
        std::span<std::byte> arenaData {};
        if (needsNewArena)
            arenaData = chain.allocArena (arenaBytes);

        const auto connection = toConnection (info);
        {
            SpinLock::ScopedLockType scopedProcessingLock { chain.processingLock };
            info.startProc->removeConnection (info);

            if (auto iter = std::find (chain.connections.begin(), chain.connections.end(), connection); iter != chain.connections.end())
                chain.connections.erase (iter);
            else
                jassertfalse; // the connection table is out of sync with the processors!

            chain.fillPlan();
            if (needsNewArena)
                std::swap (arenaData, chain.arena.get_memory_resource());
        }
        chain.deallocArena (arenaData);
        chain.connectionRemovedBroadcaster (info);
    }

    static bool getPresetWasDirty (ProcessorChain& chain)
    {
        if (chain.presetManager == nullptr)
            return true;

        return chain.presetManager->getIsDirty();
    }

private:
    ProcChainActions() = default; // static use only!
};

//=========================================================
AddOrRemoveProcessor::AddOrRemoveProcessor (ProcessorChain& procChain, BaseProcessor::Ptr newProc) : chain (procChain),
                                                                                                     actionProc (std::move (newProc)),
                                                                                                     actionProcID (actionProc != nullptr ? actionProc->getID() : ProcessorID {}),
                                                                                                     isRemoving (false),
                                                                                                     wasDirty (ProcChainActions::getPresetWasDirty (chain))
{
}

AddOrRemoveProcessor::AddOrRemoveProcessor (ProcessorChain& procChain, BaseProcessor* procToRemove) : chain (procChain),
                                                                                                      actionProcID (procToRemove != nullptr ? procToRemove->getID() : ProcessorID {}),
                                                                                                      isRemoving (true),
                                                                                                      wasDirty (ProcChainActions::getPresetWasDirty (chain))
{
}

template <typename PointerType>
bool waitForPointerCheck (const PointerType& pointer, int waitCycles = 6)
{
    int count = 0;
    while (pointer == nullptr)
    {
        if (count >= waitCycles)
            return false;

        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        count++;
    }

    return true;
}

bool AddOrRemoveProcessor::perform()
{
    if (isRemoving)
    {
        auto* procToRemove = chain.getProcessorByID (actionProcID);
        if (procToRemove == nullptr)
            return false;

        ProcChainActions::removeProcessor (chain, procToRemove, actionProc);
    }
    else
    {
        if (! waitForPointerCheck (actionProc))
            return false;

        jassert (actionProc != nullptr);
        ProcChainActions::addProcessor (chain, std::move (actionProc));
    }

    if (! wasDirty)
        chain.presetManager->setIsDirty (true);

    return true;
}

bool AddOrRemoveProcessor::undo()
{
    if (isRemoving)
    {
        if (! waitForPointerCheck (actionProc))
            return false;

        jassert (actionProc != nullptr);
        ProcChainActions::addProcessor (chain, std::move (actionProc));
    }
    else
    {
        auto* procToRemove = chain.getProcessorByID (actionProcID);
        if (procToRemove == nullptr)
            return false;

        ProcChainActions::removeProcessor (chain, procToRemove, actionProc);
    }

    if (! wasDirty)
        chain.presetManager->setIsDirty (false);

    return true;
}

//=========================================================
AddOrRemoveConnection::AddOrRemoveConnection (ProcessorChain& procChain, ConnectionInfo&& cInfo, bool removing) : chain (procChain),
                                                                                                                  startProcID (cInfo.startProc->getID()),
                                                                                                                  startPort (cInfo.startPort),
                                                                                                                  endProcID (cInfo.endProc->getID()),
                                                                                                                  endPort (cInfo.endPort),
                                                                                                                  isRemoving (removing),
                                                                                                                  wasDirty (ProcChainActions::getPresetWasDirty (chain))
{
}

bool AddOrRemoveConnection::getConnectionInfo (ConnectionInfo& info) const
{
    auto* startProc = chain.getProcessorByID (startProcID);
    auto* endProc = chain.getProcessorByID (endProcID);
    if (startProc == nullptr || endProc == nullptr)
        return false;

    info = { startProc, startPort, endProc, endPort };
    return true;
}

bool AddOrRemoveConnection::perform()
{
    ConnectionInfo info {};
    if (! getConnectionInfo (info))
        return false;

    if (isRemoving)
    {
        ProcChainActions::removeConnection (chain, info);
    }
    else
    {
        ProcChainActions::addConnection (chain, info);
    }

    if (! wasDirty)
        chain.presetManager->setIsDirty (true);

    return true;
}

bool AddOrRemoveConnection::undo()
{
    ConnectionInfo info {};
    if (! getConnectionInfo (info))
        return false;

    if (isRemoving)
    {
        ProcChainActions::addConnection (chain, info);
    }
    else
    {
        ProcChainActions::removeConnection (chain, info);
    }

    if (! wasDirty)
        chain.presetManager->setIsDirty (false);

    return true;
}
