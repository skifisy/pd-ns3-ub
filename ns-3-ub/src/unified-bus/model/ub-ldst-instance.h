// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_LDST_INSTANCE_H
#define UB_LDST_INSTANCE_H

#include "ns3/ub-datatype.h"
#include "ns3/ub-network-address.h"
#include "ns3/node-list.h"
#include "ns3/node.h"

namespace ns3 {
class UbLdstThread;
class UbLdstInstance : public Object {
public:
    static TypeId GetTypeId(void);
    UbLdstInstance();
    virtual ~UbLdstInstance();
    void DoDispose(void) override;
    void Init(uint32_t nodeId);
    // 接收任务接口，分配给thread
    void HandleLdstTask(uint32_t src, uint32_t dest, uint32_t size, uint32_t taskId, uint32_t priority,
                        UbMemOperationType type, const std::vector<uint32_t> &threadIds, uint64_t address);

    void SetClientCallback(Callback<void, uint32_t> cb);
    Ptr<UbLdstThread> GetLdstThread(uint32_t threadId);
    Callback<void, uint32_t> FinishCallback;
    void OnRecvAck(uint32_t taskSegmentId);
    void OnTaskSegmentCompleted(uint32_t taskId);

private:
    void MemTaskStartsNotify(uint32_t nodeId, uint32_t memTaskId);
    void LastPacketACKsNotify(uint32_t nodeId, uint32_t taskId);
    void MemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId);
    void FirstPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId);
    void LastPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId);
    std::unordered_map<uint32_t, std::vector<Ptr<UbLdstTaskSegment>>> m_taskToSegmentMap;  // taskid -> taskSegments
    std::vector<Ptr<UbLdstThread>> m_threads;
    std::unordered_map<uint32_t, uint32_t> m_taskSegmentCompletedNum;
    std::unordered_map<uint32_t, Ptr<UbLdstTaskSegment>> m_taskSegmentsMap;
    uint32_t m_currentTaskId = 0;
    uint32_t m_threadNum = 0;
    uint32_t m_queuePriority = 0;
    
    
    TracedCallback<uint32_t, uint32_t> m_traceLastPacketACKsNotify;
    TracedCallback<uint32_t, uint32_t> m_traceMemTaskCompletesNotify;
    TracedCallback<uint32_t, uint32_t> m_traceMemTaskStartsNotify;
    TracedCallback<uint32_t, uint32_t> m_traceFirstPacketSendsNotify;
    TracedCallback<uint32_t, uint32_t> m_traceLastPacketSendsNotify;
}; 
} 

#endif /* UB_LDST_INSTANCE_H*/