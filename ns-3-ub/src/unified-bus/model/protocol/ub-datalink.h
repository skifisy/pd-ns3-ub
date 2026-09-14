// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_DATALINK_H
#define UB_DATALINK_H

#include <ns3/node.h>
#include <set>
#include <unordered_map>
#include "ub-header.h"
#include "ns3/ub-port.h"

namespace ns3 {
/**
 * \brief UB Data Link Layer
 */
class UbDataLink : public Object {
public:
    static TypeId GetTypeId(void);

    UbDataLink();
    virtual ~UbDataLink();

    static UbDatalinkControlCreditHeader ParseCreditHeader(Ptr<Packet> p, Ptr<UbPort> port);
    static UbDatalinkPacketHeader ParsePacketHeader(Ptr<Packet> p);
    static Ptr<Packet> GenControlCreditPacket(const uint8_t credits[16]);
    static void GenPacketHeader(Ptr<Packet> p, bool credit, bool ack, uint8_t vlIndex, uint8_t vl, bool mode,
                                bool policy, UbDatalinkHeaderConfig config);
};

} // namespace ns3

#endif /* UB_DATALINK_H */
