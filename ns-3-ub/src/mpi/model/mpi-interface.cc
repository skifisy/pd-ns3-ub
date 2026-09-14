/*
 *  Copyright 2013. Lawrence Livermore National Security, LLC.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Author: Steven Smith <smith84@llnl.gov>
 */

/**
 * @file
 * @ingroup mpi
 * Implementation of class ns3::MpiInterface.
 */

#include "mpi-interface.h"

#include "distributed-simulator-impl.h"
#include "granted-time-window-mpi-interface.h"
#ifdef NS3_MTP
#include "hybrid-simulator-impl.h"
#endif
#include "null-message-mpi-interface.h"

#include "ns3/abort.h"
#include "ns3/global-value.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/simulator-impl.h"
#include "ns3/string.h"

#include <utility>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("MpiInterface");

ParallelCommunicationInterface* MpiInterface::g_parallelCommunicationInterface = nullptr;

void
MpiInterface::Destroy()
{
    NS_ASSERT(g_parallelCommunicationInterface);
    g_parallelCommunicationInterface->Destroy();
}

uint32_t
MpiInterface::GetSystemId()
{
    if (g_parallelCommunicationInterface)
    {
        return g_parallelCommunicationInterface->GetSystemId();
    }
    else
    {
        return 0;
    }
}

uint32_t
MpiInterface::GetSize()
{
    if (g_parallelCommunicationInterface)
    {
        return g_parallelCommunicationInterface->GetSize();
    }
    else
    {
        return 1;
    }
}

bool
MpiInterface::IsEnabled()
{
    if (g_parallelCommunicationInterface)
    {
        return g_parallelCommunicationInterface->IsEnabled();
    }
    else
    {
        return false;
    }
}

void
MpiInterface::SetParallelSimulatorImpl()
{
    StringValue simulationTypeValue;
    bool useDefault = true;

    if (GlobalValue::GetValueByNameFailSafe("SimulatorImplementationType", simulationTypeValue))
    {
        std::string simulationType = simulationTypeValue.Get();

        // Set communication interface based on the simulation type being used.
        // Defaults to synchronous.
        if (simulationType == "ns3::NullMessageSimulatorImpl")
        {
            g_parallelCommunicationInterface = new NullMessageMpiInterface();
            useDefault = false;
        }
        else if (simulationType == "ns3::DistributedSimulatorImpl" ||
                 simulationType == "ns3::HybridSimulatorImpl")
        {
            g_parallelCommunicationInterface = new GrantedTimeWindowMpiInterface();
            useDefault = false;
        }
    }

    // User did not specify a valid parallel simulator; use the default.
    if (useDefault)
    {
        g_parallelCommunicationInterface = new GrantedTimeWindowMpiInterface();
        GlobalValue::Bind("SimulatorImplementationType",
                          StringValue("ns3::DistributedSimulatorImpl"));
        NS_LOG_WARN("SimulatorImplementationType was set to non-parallel simulator; setting type "
                    "to ns3::DistributedSimulatorImp");
    }
}

void
MpiInterface::Enable(int* pargc, char*** pargv)
{
    SetParallelSimulatorImpl();

    g_parallelCommunicationInterface->Enable(pargc, pargv);
}

void
MpiInterface::Enable(MPI_Comm communicator)
{
    SetParallelSimulatorImpl();
    g_parallelCommunicationInterface->Enable(communicator);
}

void
MpiInterface::SendPacket(Ptr<Packet> p, const Time& rxTime, uint32_t node, uint32_t dev)
{
    NS_ASSERT(g_parallelCommunicationInterface);
    g_parallelCommunicationInterface->SendPacket(p, rxTime, node, dev);
}

void
MpiInterface::SendTaskCompletion(uint32_t taskId, const Time& completionVisibleTs, uint32_t rank)
{
    NS_ASSERT(g_parallelCommunicationInterface);
    g_parallelCommunicationInterface->SendTaskCompletion(taskId, completionVisibleTs, rank);
}

void
MpiInterface::SetTaskCompletionHandler(
    ParallelCommunicationInterface::TaskCompletionHandler handler)
{
    NS_ASSERT(g_parallelCommunicationInterface);
    g_parallelCommunicationInterface->SetTaskCompletionHandler(std::move(handler));
}

void
MpiInterface::BoundLookAhead(Time lookAhead)
{
    Ptr<SimulatorImpl> impl = Simulator::GetImplementation();
    if (Ptr<DistributedSimulatorImpl> distributed = DynamicCast<DistributedSimulatorImpl>(impl))
    {
        distributed->BoundLookAhead(lookAhead);
        return;
    }
#ifdef NS3_MTP
    if (Ptr<HybridSimulatorImpl> hybrid = DynamicCast<HybridSimulatorImpl>(impl))
    {
        hybrid->BoundLookAhead(lookAhead);
        return;
    }
#endif
    NS_ABORT_MSG_IF(IsEnabled(),
                    "attempted to bind MPI lookahead while using a non-MPI simulator implementation");
    NS_LOG_WARN("attempted to bind MPI lookahead without an enabled MPI runtime");
}

MPI_Comm
MpiInterface::GetCommunicator()
{
    NS_ASSERT(g_parallelCommunicationInterface);
    return g_parallelCommunicationInterface->GetCommunicator();
}

void
MpiInterface::Disable()
{
    NS_ASSERT(g_parallelCommunicationInterface);
    g_parallelCommunicationInterface->Disable();
    delete g_parallelCommunicationInterface;
    g_parallelCommunicationInterface = nullptr;
}

} // namespace ns3
