// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_SWITCH_H
#define UB_SWITCH_H

#include <optional>
#include <vector>
#include "ns3/nstime.h"
#include "ns3/ub-routing-process.h"
#include "ns3/ub-queue-manager.h"
#include "ns3/node.h"
#include "ns3/traced-callback.h"
#include "ns3/ub-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"

namespace ns3 {
class UbSwitchCaqm;
class UbPort;
class UbSwitchAllocator;
class UbCongestionControl;

enum class FcType {
    CBFC,
    CBFC_SHARED,
    PFC_FIXED,
    PFC_DYNAMIC,
    PFC_DYNAMIC_PAPER,
    NONE  // No flow control
};

typedef enum {
    UB_SWITCH,
    UB_DEVICE
} UbNodeType_t;

typedef enum {
    UB_CONTROL_FRAME = 1,
    UB_URMA_DATA_PACKET,
    UB_LDST_DATA_PACKET,
    UB_CTP_DATA_PACKET,
    UNKOWN_TYPE
} UbPacketType_t;

/**
 * @brief Parsed URMA packet headers (for efficient single-pass parsing)
 */
struct ParsedURMAHeaders {
    UbDatalinkPacketHeader datalinkPacketHeader;
    UbIpBasedNetworkHeader networkHeader;  // Must remove to access inner headers
    Ipv4Header ipv4Header;
    UdpHeader udpHeader;
    UbTransportHeader transportHeader;
};

/**
 * @brief Parsed LDST packet headers (for efficient single-pass parsing)
 */
struct ParsedLdstHeaders {
    UbDatalinkPacketHeader datalinkPacketHeader;
    UbCna16NetworkHeader cna16NetworkHeader;
    UbDummyTransactionHeader dummyTransactionHeader;  // Compatible with both Compact and CompactAck
};

/**
 * @brief Parsed CTP packet headers used by switch dispatch/routing.
 */
struct ParsedCtpHeaders {
    UbDatalinkPacketHeader datalinkPacketHeader;
    UbCna16NetworkHeader cna16NetworkHeader;
    UbCna24NetworkHeader cna24NetworkHeader;
    UbCtpHeader ctpHeader;
    bool isCna24{false};
};

/**
 * @brief 交换机
 */
class UbSwitch : public Object {
public:
    UbSwitch();
    ~UbSwitch();
    void DoDispose() override;
    static TypeId GetTypeId (void);

    void SwitchHandlePacket(Ptr<UbPort> port, Ptr<Packet> packet);
    // 端口发送完毕，通知交换机出队列
    void NotifySwitchDequeue(uint16_t inPortId, uint32_t outPort, uint32_t priority, Ptr<Packet> p);

    void Init();
    void InitNodePortsFlowControl();
    uint32_t GetVLNum()
    {
        return m_vlNum;
    }
    void SetVLNum(uint32_t vlNum)
    {
        m_vlNum = vlNum;
    }
    UbNodeType_t GetNodeType() {return m_nodeType;}
    void SetNodeType(UbNodeType_t type) {m_nodeType = type;}
    uint32_t GetPortsNum() {return m_portsNum;}
    void SetPortsNum(uint32_t portsNum) {m_portsNum = portsNum;}
    void RegisterTpWithAllocator(Ptr<UbIngressQueue> tp, uint32_t outPort, uint32_t priority);
    void PushPacketToVoq(Ptr<Packet> p, uint32_t outPort, uint32_t priority, uint32_t inPort);
    static bool IsValidVoqIndices(uint32_t outPort, uint32_t priority, uint32_t inPort, uint32_t portsNum, uint32_t vlNum);
    uint64_t GetAllocatedVoqCountForTest() const;
    bool HasVoqForTest(uint32_t outPort, uint32_t priority, uint32_t inPort) const;
    void RemoveTpFromAllocator(Ptr<UbIngressQueue> tp);
    Ptr<UbSwitchAllocator> GetAllocator();
    Ipv4Address GetNodeIpv4Addr(){return m_Ipv4Addr;}
    Ptr<UbRoutingProcess> GetRoutingProcess() {return m_routingProcess;}
    bool IsCBFCEnable();
    bool IsCBFCSharedEnable();
    bool IsPFCEnable();

    // Internal/runtime override entry. Current user-facing configuration still comes from
    // network_attribute.txt via ConfigStore; these setters exist for tests and future bridge code.
    void SetReservePerQueueBytes(uint32_t bytes);
    void SetSharedPoolBytes(uint64_t bytes);
    void SetHeadroomPerPortBytes(uint32_t bytes);
    void SetDynamicPfcResumeGapBytes(uint32_t bytes);
    void SetDynamicThresholdAlphaShift(uint32_t shift);
    void SetPaperDynamicPfcBeta(uint32_t beta);
    void SetPfcThresholds(int32_t xoffBytes, int32_t xonBytes);
    void SetCbfcCellGeometry(uint8_t flitLenBytes, uint8_t flitsPerCell);
    void SetCbfcReturnCellGrain(uint8_t dataPacketCells, uint8_t controlPacketCells);
    void SetCbfcCredits(int32_t initCreditCells, int32_t sharedInitCreditCells);

    void SetCongestionCtrl(Ptr<UbCongestionControl> congestionCtrl);
    Ptr<UbCongestionControl> GetCongestionCtrl();
    Ptr<UbQueueManager> GetQueueManager();    // Queue Manage Unit
    void SendPacket(Ptr<Packet> p, uint32_t inPort, uint32_t outPort, uint32_t priority);
    void SendControlFrame(Ptr<Packet> packet, uint32_t portId);
    UbPacketType_t GetPacketTypeForTest(Ptr<Packet> packet) { return GetPacketType(packet); }
    RoutingKey GetLdstRoutingKeyForTest(Ptr<Packet> packet);
    RoutingKey GetCtpRoutingKeyForTest(Ptr<Packet> packet);
    uint32_t GetVoqPacketCountForTest(uint32_t outPort, uint32_t priority, uint32_t inPort) const;

protected:
    UbPacketType_t GetPacketType(Ptr<Packet> packet);

private:
    struct BufferOverrideConfig {
        std::optional<uint32_t> reservePerQueueBytes;
        std::optional<uint64_t> sharedPoolBytes;
        std::optional<uint32_t> headroomPerPortBytes;
        std::optional<uint32_t> dynamicPfcResumeGapBytes;
        std::optional<uint32_t> dynamicThresholdAlphaShift;
        std::optional<uint32_t> paperDynamicPfcBeta;
    };

    struct FlowControlOverrideConfig {
        std::optional<int32_t> pfcXoffBytes;
        std::optional<int32_t> pfcXonBytes;
        std::optional<uint8_t> cbfcFlitLenBytes;
        std::optional<uint8_t> cbfcFlitsPerCell;
        std::optional<uint8_t> cbfcReturnGrainDataCells;
        std::optional<uint8_t> cbfcReturnGrainControlCells;
        std::optional<int32_t> cbfcInitCreditCells;
        std::optional<int32_t> cbfcSharedInitCreditCells;
    };

    TracedCallback<uint32_t, UbTransportHeader> m_traceLastPacketTraversesNotify;

    using VirtualOutputQueueGroupKey = uint64_t;

    struct VirtualOutputQueueEntry
    {
        uint32_t inPort;
        Ptr<UbPacketQueue> queue;
    };

    struct VirtualOutputQueueStorage
    {
        std::vector<std::vector<VirtualOutputQueueEntry>> groups;
    };

    void LastPacketTraversesNotify(uint32_t nodeId, UbTransportHeader ubTpHeader);

    void VoqInit();
    Ptr<UbPacketQueue> GetOrCreateVoq(uint32_t outPort, uint32_t priority, uint32_t inPort);
    VirtualOutputQueueGroupKey BuildVoqGroupKey(uint32_t outPort, uint32_t priority) const;
    const Ptr<UbPacketQueue>* FindVoqSlot(VirtualOutputQueueGroupKey groupKey,
                                          uint32_t inPort) const;
    Ptr<UbPacketQueue>* GetOrCreateVoqSlot(VirtualOutputQueueGroupKey groupKey,
                                           uint32_t inPort);
    void ReceivePacket(Ptr<UbPort> port, Ptr<Packet> p);

    void HandleURMADataPacket(Ptr<UbPort> port, Ptr<Packet> packet);
    void HandleLdstDataPacket(Ptr<UbPort> port, Ptr<Packet> packet);
    void HandleCtpDataPacket(Ptr<UbPort> port, Ptr<Packet> packet);
    bool SinkTpDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedURMAHeaders &headers);
    bool SinkLdstDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedLdstHeaders &headers);
    bool SinkCtpDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedCtpHeaders &headers);
    void ParseURMAPacketHeader(Ptr<Packet> packet, ParsedURMAHeaders &headers);
    void ParseLdstPacketHeader(Ptr<Packet> packet, ParsedLdstHeaders &headers);
    void ParseCtpPacketHeader(Ptr<Packet> packet, ParsedCtpHeaders &headers);
    void GetURMARoutingKey(const ParsedURMAHeaders &headers, RoutingKey &rtKey);
    void GetLdstRoutingKey(const ParsedLdstHeaders &headers, RoutingKey &rtKey);
    void GetCtpRoutingKey(const ParsedCtpHeaders &headers, RoutingKey &rtKey);
    void ForwardDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedURMAHeaders &headers);
    void ForwardDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedLdstHeaders &headers);
    void ForwardDataPacket(Ptr<UbPort> port, Ptr<Packet> packet, const ParsedCtpHeaders &headers);
    void ForceShortestPathRouting(Ptr<Packet> packet, const UbDatalinkPacketHeader &parsedHeader);
    void ForwardDataPacketAfterAdmission(Ptr<Packet> packet,
                                         uint32_t inPort,
                                         uint32_t outPort,
                                         uint32_t priority);
    void CompleteInPortProcessing(Ptr<Packet> packet,
                                  uint32_t inPort,
                                  uint32_t outPort,
                                  uint32_t priority);
    void MoveInPortProcessingToVoq(Ptr<Packet> packet,
                                   uint32_t inPort,
                                   uint32_t outPort,
                                   uint32_t priority);
    void FinalizeForwardedPacketEnqueue(Ptr<Packet> packet,
                                        uint32_t inPort,
                                        uint32_t outPort,
                                        uint32_t priority);
    void InitAllocator(Ptr<Node> node);
    void InitQueueManager(Ptr<Node> node);
    void InitRoutingProcess(Ptr<Node> node);
    void ApplyLocalQueueManagerConfig();
    void ApplyLocalPortFlowControlConfig(Ptr<UbPort> port);

    Ptr<UbQueueManager> m_queueManager;   // Memory Management Unit
    Ptr<UbCongestionControl> m_congestionCtrl;
    UbNodeType_t m_nodeType;
    uint32_t m_portsNum = 1025;
    Ptr<UbSwitchAllocator> m_allocator;
    uint32_t m_vlNum = 16;
    Time m_inPortProcessingDelay {NanoSeconds(0)};
    std::vector<EventId> m_inPortProcessingEvents;
    VirtualOutputQueueStorage m_voq; // on-demand VOQ groups keyed by outPort/priority
    Ptr<UbRoutingProcess> m_routingProcess;   // Router Model

    Ipv4Address m_Ipv4Addr;
    bool m_isECNEnable;
    FcType m_flowControlType { FcType::NONE };
    BufferOverrideConfig m_bufferOverrides;
    FlowControlOverrideConfig m_flowControlOverrides;
    enum VlScheduler {
        SP = 0,
        DWRR = 1
    };
    VlScheduler m_vlScheduler {SP};
};

} // namespace ns3

#endif /* UB_SWITCH_H */
