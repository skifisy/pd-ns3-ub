// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_LDST_THREAD_H
#define UB_LDST_THREAD_H
#include "ns3/ub-ldst-instance.h"

namespace ns3 {
class UbController;
/**
* @brief 发送内存语义包
*/
class UbLdstThread : public Object {
public:
    static TypeId GetTypeId(void);
    UbLdstThread();
    virtual ~UbLdstThread();
    void Init();
    void PushTaskSegment(Ptr<UbLdstTaskSegment> taskSegment);
    void HandleTaskSegment();
    void SetNode(uint32_t nodeId);
    void SetThreadId(uint32_t threadId);
    void HandleLoadTask();
    void HandleStoreTask();
    void UpdateTask(Ptr<UbLdstTaskSegment> taskSegment);
    void SetLoadReqSize(uint32_t size);
    void SetStoreReqLength(uint32_t length);
    void SetLoadRspLength(uint32_t length);
private:
    uint32_t CalcLength(uint32_t size);
    uint32_t m_nodeId;
    uint32_t m_threadId;
    std::queue<Ptr<UbLdstTaskSegment>> m_loadQueue;
    std::queue<Ptr<UbLdstTaskSegment>> m_storeQueue;

    uint32_t m_loadRspSize = 0;
    uint32_t m_storeReqSize = 0;
    uint32_t m_loadReqSize = 0;
    uint32_t m_loadRspLength = 0;
    uint32_t m_storeReqLength = 0;
    uint32_t m_storeOutstanding; // 发数据包--, 收ack ++
    uint32_t m_loadOutstanding; // 发数据包--, 收ack ++
    std::unordered_map<uint32_t, uint32_t> m_waitingAckNum;
};
} // namespace ns3

#endif /* UB_LDST_THREAD_H */