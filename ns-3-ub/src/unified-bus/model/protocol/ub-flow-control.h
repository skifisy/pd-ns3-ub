// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_FLOWCONTROL_H
#define UB_FLOWCONTROL_H

#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <string.h>
#include "ns3/ipv4-header.h"
#include "ns3/ipv4.h"
#include "ns3/udp-header.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/ub-transport.h"
#include "ns3/ub-link.h"
#include "ns3/packet.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/enum.h"
#include "ns3/ub-datalink.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-routing-process.h"
#include "ns3/ub-queue-manager.h"

namespace ns3 {
class UbSwitch;
class UbFlowControl;
class UbIngressQueue;
class UbCbfc;
class UbPfc;
class UbPort;

/**
 * @brief Flow-control 事件上下文
 *
 * 这不是一层新抽象，只是把事件发生点已经知道的事实打成一个小包，
 * 避免 On* hook 继续膨胀成一长串位置参数。
 *
 * 当前约定：
 * - packet: 事件对应的报文。运行时主路径应始终非空。
 * - ingressQueue: 只有 allocator 正在处理某个真实 ingress queue 时才有。
 *   典型是 VOQ -> egress queue 的 OnEgressEnqueued / OnIngressReleased 路径。
 *   对于 sink/local ingress 这类“只有报文、没有 queue 对象”的事件，这里可以为空。
 * - inPortId: 该报文当前 ingress ownership 对应的端口。
 * - outPortId: 该报文当前关联的 egress 端口；本地 sink 路径通常等于 inPortId。
 * - priority: 报文所在 VL / priority。
 */
struct UbFlowControlEventContext {
    Ptr<Packet> packet;
    Ptr<UbIngressQueue> ingressQueue;
    uint32_t inPortId {0};
    uint32_t outPortId {0};
    uint32_t priority {0};
};

struct UbPfcDecision {
    bool pause {false};
    bool resume {false};
    uint64_t ingress_total_bytes {0};
    uint64_t ingress_shared_bytes {0};
    uint64_t ingress_headroom_bytes {0};
    uint64_t switch_shared_pool_used_bytes {0};
    uint64_t switch_total_buffered_bytes {0};
    uint64_t xoff_threshold_bytes {0};
    uint64_t xon_threshold_bytes {0};
};

class UbPfcDecisionHook : public Object {
public:
    static TypeId GetTypeId(void);
    ~UbPfcDecisionHook() override = default;

    virtual UbPfcDecision Evaluate(Ptr<UbQueueManager> queueManager,
                                   uint32_t inPort,
                                   uint32_t priority) const = 0;
};

class UbPfcFixedDecisionHook : public UbPfcDecisionHook {
public:
    static TypeId GetTypeId(void);
    void Init(uint64_t xoffThresholdBytes, uint64_t xonThresholdBytes);
    UbPfcDecision Evaluate(Ptr<UbQueueManager> queueManager,
                           uint32_t inPort,
                           uint32_t priority) const override;

private:
    uint64_t m_xoffThresholdBytes {0};
    uint64_t m_xonThresholdBytes {0};
};

class UbPfcDynamicDecisionHook : public UbPfcDecisionHook {
public:
    static TypeId GetTypeId(void);
    UbPfcDecision Evaluate(Ptr<UbQueueManager> queueManager,
                           uint32_t inPort,
                           uint32_t priority) const override;
};

// Paper dynamic PFC mode for reproducing the threshold rule used with DCQCN in
// "Congestion Control for Large-Scale RDMA Deployments" (SIGCOMM 2015).
class UbPfcPaperDynamicDecisionHook : public UbPfcDecisionHook {
public:
    static TypeId GetTypeId(void);
    UbPfcDecision Evaluate(Ptr<UbQueueManager> queueManager,
                           uint32_t inPort,
                           uint32_t priority) const override;
};

/**
 * @brief 端口流量控制
 */
class UbFlowControl : public Object {
public:
    static TypeId GetTypeId(void);
    UbFlowControl() {}
    virtual ~UbFlowControl() {}
    // 发送 gating 判定：allocator 在选择某个 ingress queue 发送前调用。
    virtual bool IsFcLimited(Ptr<UbIngressQueue> ingressQ)
    {
        return false;
    }
    // ingress 占用已经由 queue-manager 真正释放后调用。
    virtual void OnIngressReleased(const UbFlowControlEventContext& context) {}
    // 报文已经进入 egress queue 后调用。
    virtual void OnEgressEnqueued(const UbFlowControlEventContext& context) {}
    // 对端发来的 flow-control 控制帧到达后调用。
    virtual void OnControlFrameReceived(Ptr<Packet> p) {}
    // 普通 DLLDP 数据包到达后调用；用于消费本跳 piggyback credit 等 data-link 本地语义。
    virtual void OnDataPacketReceived(Ptr<Packet> p) {}
    // 报文已经完成 ingress 入账后调用。
    virtual void OnIngressEnqueued(const UbFlowControlEventContext& context) {}
    virtual FcType GetFcType()
    {
        return FcType::NONE;
    }
};

/**
 * @brief 端口Cbfc
 */
class UbCbfc : public UbFlowControl {
public:
    static TypeId GetTypeId (void);
    UbCbfc() {}
    virtual ~UbCbfc() {}
    virtual FcType GetFcType() override;

    void Init(uint8_t flitLen, uint8_t nFlitPerCell, uint8_t retCellGrainDataPacket,
              uint8_t retCellGrainControlPacket, int32_t portTxfree, int32_t ctrlCrdRtrThldCells,
              uint32_t nodeId, uint32_t portId);

    virtual bool IsFcLimited(Ptr<UbIngressQueue> ingressQ) override;
    virtual void OnIngressReleased(const UbFlowControlEventContext& context) override;
    virtual void OnEgressEnqueued(const UbFlowControlEventContext& context) override;
    virtual void OnControlFrameReceived(Ptr<Packet> p) override;
    virtual void OnDataPacketReceived(Ptr<Packet> p) override;
    virtual void OnIngressEnqueued(const UbFlowControlEventContext& context) override;
    int32_t GetCrdToReturn(uint8_t vlId);
    void SetCrdToReturn(uint8_t vlId, int32_t consumeCell, Ptr<UbPort> targetPort);
    void UpdateCrdToReturn(uint8_t vlId, int32_t consumeCell, Ptr<UbPort> targetPort);
    bool CbfcConsumeCrd(Ptr<Packet> p);
    void SendCrdAck(Ptr<Packet> cbfcPkt, uint32_t targetPortId);

protected:
    void ControlCreditRestoreNotify(uint32_t nodeId,
                                    uint32_t portId,
                                    const std::vector<uint8_t>& credits);
    FcType m_fcType { FcType::CBFC };
    void DoDispose() override;
    uint32_t m_portId;
    uint32_t m_nodeId;
    TracedCallback<uint32_t, uint32_t, std::vector<uint8_t>> m_traceControlCreditRestoreNotify;

    /**
    * @brief cbfc相关参数配置
    */
    struct cbfcCfg_t {
        uint8_t  m_flitLen;                      // flit长度，默认 20 Bytes
        uint8_t  m_nFlitPerCell;                 // N值 {1, 2, 4, 8, 16, 32}
        uint8_t  m_retCellGrainDataPacket;       // 返回的CRD的粒度，通常从以下选项中选择 {1, 2, 4, 8, 16, 32, 64, 128}
        uint8_t  m_retCellGrainControlPacket;    // 返回的CRD的粒度，通常从以下选项中选择 {1, 2, 4, 8, 16, 32, 64, 128}
        int32_t  m_ctrlCrdRtrThldCells;          // 触发 control-frame credit return 的 per-VL pending-return 阈值（单位：cell）
    } m_cbfcCfg {};

    std::vector<int32_t>  m_crdTxfree;      // 发送端口每个vl信用证
    std::vector<int32_t>  m_crdToReturn;    // 用于记录每个vl需要返回的信用证
    std::vector<bool> m_creditBlockedLast;  // 上一次是否因 credit 不足而阻塞
    uint8_t m_lastPiggybackVl {0};

protected:
    bool TryAttachPiggybackCredit(Ptr<Packet> p);
    bool ShouldForceControlReturn() const;
    void AccumulateReturnedCredit(Ptr<Packet> p, uint32_t targetPortId);
    Ptr<Packet> BuildControlReturnPacket(uint32_t targetPortId,
                                         int32_t minPendingCells,
                                         const std::string& reason);
    Ptr<Packet> MaybeBuildControlReturnPacket(uint32_t targetPortId);
    void MaybeQueueControlReturn(uint32_t targetPortId);
    static constexpr uint8_t kMaxCtrlCreditGrainsPerFrame = 63;

private:
    bool CbfcRestoreCrd(Ptr<Packet> p);
    bool RestoreDataPacketCredit(Ptr<Packet> p);
    uint8_t SelectPiggybackCreditVl() const;
};


/**
 * @brief 端口Cbfc (Shared credit)
 */
class UbCbfcSharedCredit : public UbCbfc {
public:
    static TypeId GetTypeId (void);
    UbCbfcSharedCredit() {}
    ~UbCbfcSharedCredit() override {}

    FcType GetFcType() override;

    void Init(uint8_t flitLen, uint8_t nFlitPerCell, uint8_t retCellGrainDataPacket,
              uint8_t retCellGrainControlPacket, int32_t reservedPerVlCells,
              int32_t ctrlCrdRtrThldCells,
              int32_t sharedInitCells, uint32_t nodeId, uint32_t portId);

    bool IsFcLimited(Ptr<UbIngressQueue> ingressQ) override;
    void OnEgressEnqueued(const UbFlowControlEventContext& context) override;
    void OnControlFrameReceived(Ptr<Packet> p) override;
    void OnDataPacketReceived(Ptr<Packet> p) override;

private:
    bool CbfcSharedConsumeCrd(Ptr<Packet> p);
    bool CbfcSharedRestoreCrd(Ptr<Packet> p);
    bool RestoreSharedDataPacketCredit(Ptr<Packet> p);
    int32_t m_shareCrd {0};
    int32_t m_reservedPerVlCells {0};
};


/**
 * @brief 端口Pfc
 */
class UbPfc : public UbFlowControl {
public:
    static TypeId GetTypeId (void);
    UbPfc() = default;
    ~UbPfc() override = default;
    FcType GetFcType() override;

    void Init(FcType mode, int32_t portpfcUpThld, int32_t portpfcLowThld, uint32_t nodeId, uint32_t portId);

    bool IsFcLimited(Ptr<UbIngressQueue> ingressQ) override;
    void OnIngressReleased(const UbFlowControlEventContext& context) override;
    void OnEgressEnqueued(const UbFlowControlEventContext& context) override;
    void OnControlFrameReceived(Ptr<Packet> p) override;
    void OnIngressEnqueued(const UbFlowControlEventContext& context) override;
    void SendPfc(Ptr<Packet> pfcPacket, uint32_t targetPortId);
    Ptr<Packet> CheckPfcThreshold(Ptr<Packet> p, uint32_t portId);
    void DoDispose() override;

    FcType m_fcType { FcType::PFC_FIXED };
    uint32_t m_portId;
    uint32_t m_nodeId;

    /**
    * @brief pfc水线参数配置
    */
    struct pfcCfg_t {
        int32_t                         m_portpfcUpThld;        // 缓冲阈值以生成PFC
        int32_t                         m_portpfcLowThld;       // 缓冲阈值以生成PFC
    } m_pfcCfg {};
    /**
    * @brief pfc端口状态
    */
    struct pfcStatus_t {
        std::vector<uint8_t>    m_portCredits;           // pfc状态, 每个端口各优先级pfc状态
        std::vector<uint8_t>    m_pfcSndCredits;         // pfc状态, 用于发送pfc报文
        std::vector<uint8_t>    m_pfcLastSndCredits;     // pfc状态, 用于发送pfc报文
        std::vector<uint8_t>    m_pfcDynamicLastTracePause; // dynamic PFC 上次已记录的pause状态
        uint32_t                m_pfcSndCnt;             // 发送的pfc包统计
        uint32_t                m_pfcRcvCnt;             // 接收的pfc包统计
        pfcStatus_t(uint32_t totVlNum)
        {
            m_portCredits.resize(totVlNum, UB_CREDIT_MAX_VALUE);
            m_pfcSndCredits.resize(totVlNum, UB_CREDIT_MAX_VALUE);
            m_pfcLastSndCredits.resize(totVlNum, UB_CREDIT_MAX_VALUE);
            m_pfcDynamicLastTracePause.resize(totVlNum, 2);
            m_pfcSndCnt = 0;
            m_pfcRcvCnt = 0;
        }
    } m_pfcStatus {0};
    Ptr<UbPfcDecisionHook> m_decisionHook;

private:
    bool UpdatePfcStatus(Ptr<Packet> p);
    void InitRuntimeState(FcType mode,
                          int32_t portpfcUpThld,
                          int32_t portpfcLowThld,
                          uint32_t nodeId,
                          uint32_t portId,
                          uint32_t vlNum);
    void BindDecisionHook(Ptr<UbQueueManager> queueManager);
    void BindFixedDecisionHook();
    void BindDynamicDecisionHook(Ptr<UbQueueManager> queueManager);
    void BindPaperDynamicDecisionHook(Ptr<UbQueueManager> queueManager);
};

} // namespace ns3

#endif // UB_FLOWCONTROL_H
