#pragma once

#include "ProcessorChain.h"

namespace ProcessorChainHelpers
{
void removeOutputConnectionsFromProcessor (ProcessorChain& chain, BaseProcessor* proc, UndoManager* um);
}

class AddOrRemoveProcessor : public UndoableAction
{
public:
    AddOrRemoveProcessor (ProcessorChain& procChain, BaseProcessor::Ptr newProc);
    AddOrRemoveProcessor (ProcessorChain& procChain, BaseProcessor* procToRemove);

    bool perform() override;
    bool undo() override;
    int getSizeInUnits() override { return 100; } // allow for 500 add/remove actions

private:
    ProcessorChain& chain;
    BaseProcessor::Ptr actionProc; // holds the processor while it is out of the chain
    const ProcessorID actionProcID;

    const bool isRemoving;
    const bool wasDirty;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AddOrRemoveProcessor)
};

class AddOrRemoveConnection : public UndoableAction
{
public:
    AddOrRemoveConnection (ProcessorChain& procChain, ConnectionInfo&& info, bool removing = false);

    bool perform() override;
    bool undo() override;
    int getSizeInUnits() override { return 1; }

private:
    /** Looks up the connection's processors in the chain. Returns false if either one is not in the chain. */
    bool getConnectionInfo (ConnectionInfo& info) const;

    ProcessorChain& chain;
    const ProcessorID startProcID;
    const int startPort;
    const ProcessorID endProcID;
    const int endPort;
    const bool isRemoving;
    const bool wasDirty;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AddOrRemoveConnection)
};
