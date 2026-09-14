// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-ldst-thread.h"
#include "ns3/ub-ldst-api.h"

#include "ns3/ub-queue-manager.h"
#include "ns3/ub-routing-process.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-datatype.h"

namespace ns3 {
NS_LOG_COMPONENT_DEFINE("UbLdstThread");

NS_OBJECT_ENSURE_REGISTERED(UbLdstThread);

TypeId UbLdstThread::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbLdstThread")
                            .SetParent<Object>()
                            .SetGroupName("UnifiedBus")
                            .AddAttribute("StoreOutstanding",
                                          "Maximum number of outstanding STORE requests this thread may issue.",
                                          UintegerValue(64),
                                          MakeUintegerAccessor(&UbLdstThread::m_storeOutstanding),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("LoadOutstanding",
                                          "Maximum number of outstanding LOAD requests this thread may issue.",
                                          UintegerValue(64),
                                          MakeUintegerAccessor(&UbLdstThread::m_loadOutstanding),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("LoadResponseSize",
                                          "LOAD response payload size in bytes; rounded to nearest 64B * 2^n at Init().",
                                          UintegerValue(512),
                                          MakeUintegerAccessor(&UbLdstThread::m_loadRspSize),
                                          MakeUintegerChecker<uint32_t>(0, 8192))
                            .AddAttribute("StoreRequestSize",
                                          "STORE request payload size in bytes; rounded to nearest 64B * 2^n at Init().",
                                          UintegerValue(512),
                                          MakeUintegerAccessor(&UbLdstThread::m_storeReqSize),
                                          MakeUintegerChecker<uint32_t>(0, 8192))
                            .AddAttribute("LoadRequestSize",
                                          "Payload size (bytes) for each LOAD request.",
                                          UintegerValue(64),
                                          MakeUintegerAccessor(&UbLdstThread::m_loadReqSize),
                                          MakeUintegerChecker<uint32_t>());

    return tid;
}

UbLdstThread::UbLdstThread()
{
}

UbLdstThread::~UbLdstThread()
{
}

uint32_t UbLdstThread::CalcLength(uint32_t size)
{
    // size = 64B * (2 ^ length)
    if (size <= 64) return 0;
    size = (size - 1) / 64;
    int length = 0;
    while (size  > 0) {
        size = size >> 1;
        length++;
    }
    return length;
}

void UbLdstThread::Init()
{
    m_loadRspLength = CalcLength(m_loadRspSize);
    m_storeReqLength = CalcLength(m_storeReqSize);
    // Reset Size
    m_loadRspSize = 64 * (1 << m_loadRspLength);
    m_storeReqSize = 64 * (1 << m_storeReqLength);
}

void UbLdstThread::SetLoadReqSize(uint32_t size)
{
    m_loadReqSize = size;
}

void UbLdstThread::SetStoreReqLength(uint32_t length)
{
    m_storeReqLength = length;
    m_storeReqSize = 64 * (1 << m_storeReqLength);
}

void UbLdstThread::SetLoadRspLength(uint32_t length)
{
    m_loadRspLength = length;
    m_loadRspSize = 64 * (1 << m_loadRspLength);
}

void UbLdstThread::PushTaskSegment(Ptr<UbLdstTaskSegment> taskSegment)
{
    if (taskSegment->GetType() == UbMemOperationType::STORE) {
        taskSegment->SetPacketInfo(64 * (1 << m_storeReqLength), m_storeReqLength);
    } else if (taskSegment->GetType() == UbMemOperationType::LOAD) {
        taskSegment->SetPacketInfo(m_loadReqSize, m_loadRspLength);
    } else {
        NS_ASSERT_MSG(0, "task type is wrong");
    }
    auto taskSegmentId = taskSegment->GetTaskSegmentId();
    m_waitingAckNum[taskSegmentId] = taskSegment->GetPsnSize();
    NS_LOG_DEBUG("[UbLdstThread PushTaskSegment] m_waitingAckNum[" << taskSegmentId << "]: " <<
                 m_waitingAckNum[taskSegmentId]);
    if (taskSegment->GetType() == UbMemOperationType::LOAD) {
        m_loadQueue.push(taskSegment);
        Simulator::ScheduleNow(&UbLdstThread::HandleLoadTask, this);
    } else if (taskSegment->GetType() == UbMemOperationType::STORE) {
        m_storeQueue.push(taskSegment);
        Simulator::ScheduleNow(&UbLdstThread::HandleStoreTask, this);
    }
}

void UbLdstThread::HandleLoadTask()
{
    NS_LOG_DEBUG("[UbLdstThread HandleLoadTask]");
    while (m_loadOutstanding > 0) {
        Ptr<UbLdstTaskSegment> taskSegment = nullptr;
        while (!m_loadQueue.empty()) {
            taskSegment = m_loadQueue.front();
            if (taskSegment->PeekNextDataSize() == 0) {
                m_loadQueue.pop();
                taskSegment = nullptr;
            } else {
                break;
            }
        }
        if (taskSegment == nullptr) {
            return;
        }
        auto node = NodeList::GetNode(m_nodeId);
        auto ldstapi = node->GetObject<UbController>()->GetUbFunction()->GetUbLdstApi();
        m_loadOutstanding--;
        ldstapi->LdstProcess(taskSegment);
    }
}

void UbLdstThread::HandleStoreTask()
{
    NS_LOG_DEBUG("[UbLdstThread HandleStoreTask]");
    while (m_storeOutstanding > 0) {
        Ptr<UbLdstTaskSegment> taskSegment = nullptr;
        while (!m_storeQueue.empty()) {
            taskSegment = m_storeQueue.front();
            if (taskSegment->PeekNextDataSize() == 0) {
                m_storeQueue.pop();
                taskSegment = nullptr;
            } else {
                break;
            }
        }
        if (taskSegment == nullptr) {
            return;
        }
        auto node = NodeList::GetNode(m_nodeId);
        auto ldstapi = node->GetObject<UbController>()->GetUbFunction()->GetUbLdstApi();
        m_storeOutstanding--;
        ldstapi->LdstProcess(taskSegment);
    }
}

void UbLdstThread::SetNode(uint32_t nodeId)
{
    m_nodeId = nodeId;
}

void UbLdstThread::SetThreadId(uint32_t threadId)
{
    m_threadId = threadId;
}

void UbLdstThread::UpdateTask(Ptr<UbLdstTaskSegment> taskSegment)
{
    auto taskSegmentId = taskSegment->GetTaskSegmentId();
    m_waitingAckNum[taskSegmentId]--;
    NS_LOG_DEBUG("[UbLdstThread UpdateTask] m_waitingAckNum[" << taskSegmentId << "]"
                 << m_waitingAckNum[taskSegmentId]);
    if (m_waitingAckNum[taskSegmentId] == 0) {
        auto ldstInstance = NodeList::GetNode(m_nodeId)->GetObject<UbLdstInstance>();
        ldstInstance->OnTaskSegmentCompleted(taskSegment->GetTaskId());
    }
    if (taskSegment->GetType() == UbMemOperationType::LOAD) {
        m_loadOutstanding++;
        Simulator::ScheduleNow(&UbLdstThread::HandleLoadTask, this);
    } else if (taskSegment->GetType() == UbMemOperationType::STORE) {
        m_storeOutstanding++;
        Simulator::ScheduleNow(&UbLdstThread::HandleStoreTask, this);
    }
}
} // namespace ns3