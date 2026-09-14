// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-ldst-instance.h"
#include "ns3/ub-ldst-thread.h"

namespace ns3 {
NS_LOG_COMPONENT_DEFINE("UbLdstInstance");

NS_OBJECT_ENSURE_REGISTERED(UbLdstInstance);
TypeId UbLdstInstance::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbLdstInstance")
                            .SetParent<Object>()
                            .SetGroupName("UnifiedBus")
                            .AddAttribute("ThreadNum",
                                          "Number of LDST worker threads.",
                                          UintegerValue(48),
                                          MakeUintegerAccessor(&UbLdstInstance::m_threadNum),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("QueuePriority",
                                          "Queue (VOQ) priority for packets emitted by this thread.",
                                          UintegerValue(1),
                                          MakeUintegerAccessor(&UbLdstInstance::m_queuePriority),
                                          MakeUintegerChecker<uint32_t>())
                            .AddTraceSource("MemTaskStartsNotify",
                                            "Emitted when a memory task starts on this thread.",
                                            MakeTraceSourceAccessor(&UbLdstInstance::m_traceMemTaskStartsNotify),
                                            "ns3::UbLdstInstance::MemTaskStartsNotify")
                            .AddTraceSource("LastPacketACKsNotify",
                                            "Emitted when the last packet of a task is ACKed.",
                                            MakeTraceSourceAccessor(&UbLdstInstance::m_traceLastPacketACKsNotify),
                                            "ns3::UbLdstInstance::LastPacketACKsNotify")
                            .AddTraceSource("MemTaskCompletesNotify",
                                            "Emitted when a memory task completes.",
                                            MakeTraceSourceAccessor(&UbLdstInstance::m_traceMemTaskCompletesNotify),
                                            "ns3::UbLdstInstance::MemTaskCompletesNotify")
                            .AddTraceSource("FirstPacketSendsNotify",
                                            "Emitted when the first packet of a memory task is sent.",
                                            MakeTraceSourceAccessor(&UbLdstInstance::m_traceFirstPacketSendsNotify),
                                            "ns3::UbLdstInstance::FirstPacketSendsNotify")
                            .AddTraceSource("LastPacketSendsNotify",
                                            "Emitted when the last packet of a memory task is sent.",
                                            MakeTraceSourceAccessor(&UbLdstInstance::m_traceLastPacketSendsNotify),
                                            "ns3::UbLdstInstance::LastPacketSendsNotify");

    return tid;
}

UbLdstInstance::UbLdstInstance()
{
}

UbLdstInstance::~UbLdstInstance()
{
}

void UbLdstInstance::Init(uint32_t nodeId)
{
    for (uint32_t threadId = 0; threadId < m_threadNum; threadId++) {
        auto ldstThread = CreateObject<UbLdstThread>();
        ldstThread->Init();
        ldstThread->SetNode(nodeId);
        ldstThread->SetThreadId(threadId);
        m_threads.push_back(ldstThread);
    }
}

void UbLdstInstance::DoDispose()
{
    m_threads.clear();
}

void UbLdstInstance::SetClientCallback(Callback<void, uint32_t> cb)
{
    FinishCallback = cb;
}

void UbLdstInstance::HandleLdstTask(uint32_t src, uint32_t dest, uint32_t length, uint32_t taskId, uint32_t priority,
                                    UbMemOperationType type, const std::vector<uint32_t> &threadIds, uint64_t address)
{
    uint32_t threadsNum = threadIds.size();
    // 将数据均分下发给thread
    uint32_t partSize = length / threadsNum;

    FirstPacketSendsNotify(this->GetObject<Node>()->GetId(), taskId);
    MemTaskStartsNotify(this->GetObject<Node>()->GetId(), taskId);
    for (uint32_t i = 0; i < threadsNum; i++) {
        uint32_t segmentSize = partSize;
        if (i == threadsNum - 1) {
            segmentSize += length - partSize * threadsNum;
        }
        uint32_t threadId = threadIds[i];
        auto ldstThread = GetLdstThread(threadId);
        auto taskSegment = CreateObject<UbLdstTaskSegment>();
        taskSegment->SetSrc(src);
        taskSegment->SetDest(dest);
        taskSegment->SetPriority(static_cast<uint8_t>(priority)); 
        taskSegment->SetSize(segmentSize);
        taskSegment->SetTaskId(taskId);
        taskSegment->SetTaskSegmentId(m_currentTaskId);
        taskSegment->SetType(type);
        taskSegment->SetThreadId(threadId);
        m_taskToSegmentMap[taskId].push_back(taskSegment);
        m_taskSegmentsMap[m_currentTaskId] = taskSegment;
        m_taskSegmentCompletedNum[taskId] = 0;
        m_currentTaskId++;
        Simulator::ScheduleNow(&UbLdstThread::PushTaskSegment, ldstThread, taskSegment);
    }
}

void UbLdstInstance::OnRecvAck(uint32_t taskSegmentId)
{
    auto taskSegment = m_taskSegmentsMap[taskSegmentId];
    if (taskSegment == nullptr) {
        NS_ASSERT_MSG(0, "taskSegment invalid!");
    }
    uint32_t threadId = taskSegment->GetThreadId();
    auto ldstThread = GetLdstThread(threadId);
    Simulator::ScheduleNow(&UbLdstThread::UpdateTask, ldstThread, taskSegment);
}

void UbLdstInstance::OnTaskSegmentCompleted(uint32_t taskId)
{
    m_taskSegmentCompletedNum[taskId]++;
    if (m_taskSegmentCompletedNum[taskId] == m_taskToSegmentMap[taskId].size()) {
        LastPacketACKsNotify(this->GetObject<Node>()->GetId(), taskId);
        MemTaskCompletesNotify(this->GetObject<Node>()->GetId(), taskId);
        FinishCallback(taskId);
    }
}

Ptr<UbLdstThread> UbLdstInstance::GetLdstThread(uint32_t threadId)
{
    if (threadId > m_threads.size()) {
        NS_ASSERT_MSG(0, "Invalid threadId! Cannot Get Ldst Thread.");
    }
    return m_threads[threadId];
}

void UbLdstInstance::LastPacketACKsNotify(uint32_t nodeId, uint32_t taskId)
{
    m_traceLastPacketACKsNotify(nodeId, taskId);
}

void UbLdstInstance::MemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId)
{
    m_traceMemTaskCompletesNotify(nodeId, taskId);
}

void UbLdstInstance::MemTaskStartsNotify(uint32_t nodeId, uint32_t memTaskId)
{
    m_traceMemTaskStartsNotify(nodeId, memTaskId);
}

void UbLdstInstance::FirstPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId)
{
    m_traceFirstPacketSendsNotify(nodeId, memTaskId);
}
 
void UbLdstInstance::LastPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId)
{
    m_traceLastPacketSendsNotify(nodeId, memTaskId);
}
}