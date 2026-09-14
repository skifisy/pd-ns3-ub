// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_LDST_API_H
#define UB_LDST_API_H

#include <ns3/node.h>
#include <ns3/ub-ldst-thread.h>
#include "ns3/ub-datatype.h"
#include "ns3/ub-network-address.h"
#include "ns3/node-list.h"
#include "ns3/ub-tag.h"
namespace ns3 {
constexpr int MAX_LB = 255;
constexpr int MIN_LB = 0;
class UbController;
class UbLdstApi : public Object {
public:
    static TypeId GetTypeId(void);
    UbLdstApi();
    virtual ~UbLdstApi();

public:
    void SetNodeId(uint32_t nodeId);
    void RecvResponse(Ptr<Packet> packet);
    void SetUsePacketSpray(bool usePacketSpray);
    void SetUseShortestPaths(bool useShortestPaths);
    void RecvDataPacket(Ptr<Packet> packet);
    void LdstProcess(Ptr<UbLdstTaskSegment> taskSegment);

private:
    void SendPacket(Ptr<UbLdstTaskSegment> taskSegment, Ptr<Packet> packet);
    Ptr<Packet> GenDataPacket(Ptr<UbLdstTaskSegment> taskSegment);
    uint32_t m_nodeId = 0;
    uint32_t m_lbHashSalt = 0;
    bool m_usePacketSpray = false;
    bool m_useShortestPaths = true;
    bool m_pktTraceEnabled = false;
    void LdstRecvNotify(uint32_t packetUid, uint32_t src, uint32_t dst,
                        PacketType type, uint32_t size, uint32_t taskId, UbPacketTraceTag traceTag);
    TracedCallback<uint32_t, uint32_t, uint32_t,
                   PacketType, uint32_t, uint32_t, UbPacketTraceTag> m_ldstRecvNotify;
};
} // namespace ns3

#endif /* UB_LDST_API_H */
