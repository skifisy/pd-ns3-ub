// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_APP_H
#define UB_APP_H

#include <vector>
#include <set>
#include <map>
#include <tuple>
#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/ptr.h"
#include "ns3/ipv4-address.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-ldst-api.h"
#include "ub-tp-connection-manager.h"
#include "ub-network-address.h"
#include "ub-traffic-gen.h"

using namespace utils;
namespace ns3 {

/**
 * @brief 任务图应用,管理多个wqe任务及依赖关系
 */
class UbApp : public Application {
public:
    static TypeId GetTypeId(void);

    UbApp();
    virtual ~UbApp();

    void SendTraffic(TrafficRecord record);
    void SendTraffic(UbTrafficGen::RuntimeTask task);
    void SendTrafficForTest(TrafficRecord record);
    void SendCtpUrmaTraffic(TrafficRecord record);
    void SendCtpUrmaTraffic(UbTrafficGen::RuntimeTask task);
    Ptr<UbJetty> GetOrCreateCtpBoundJetty(uint32_t sourceNode,
                                          uint32_t destNode,
                                          uint32_t srcEntityId,
                                          uint32_t dstEntityId,
                                          uint8_t vl,
                                          uint32_t* jettyNum);
    Ptr<UbJetty> GetOrCreateCtpUnboundJetty(uint32_t sourceNode,
                                            uint32_t srcEntityId,
                                            uint8_t vl,
                                            uint32_t* jettyNum);

    void SetNode(Ptr<Node> node); // 设置当前节点

    void SetTransportMode(TransportMode mode);
    void SetLocalEntityId(uint32_t localEntityId);
    void SetPeerEntityId(uint32_t peerEntityId);

    /**
     * @brief Get the configured transport mode.
     * @return Transport mode used by this application.
     */
    TransportMode GetTransportMode() const
    {
        return m_transportMode;
    }

    /**
     * @brief Get whether shortest paths are required.
     * @return True when the application is configured to use shortest paths.
     */
    bool GetUseShortestPaths() const
    {
        return m_useShortestPaths;
    }

    void SetGetTpnRule(GetTpnRuleT type)
    {
        m_getTpnRule = type;
    }

    void SetUseShortestPaths(bool useShortestPaths)
    {
        m_useShortestPaths = useShortestPaths;
    }

    void SetUsePacketSpray(bool usePacketSpray)
    {
        m_usePacketSpray = usePacketSpray;
    }

    /**
     * @brief 任务完成回调
     */
    void OnTaskCompleted(uint32_t taskId, uint32_t jettyNum);
    void OnTestTaskCompleted(uint32_t taskId, uint32_t jettyNum);
    void OnMemTaskCompleted(uint32_t taskId);

    // ========== 回调函数 ==========
    void SetFinishCallback(Callback<void, uint32_t, uint32_t> cb, Ptr<UbJetty> jetty);
    void SetFinishCallback(Callback<void, uint32_t> cb, Ptr<UbLdstInstance> ubLdstInstance);

protected:
    void DoDispose(void) override;

private:
    TracedCallback<uint32_t, uint32_t> m_traceMemTaskStartsNotify;
    TracedCallback<uint32_t, uint32_t> m_traceMemTaskCompletesNotify;
    TracedCallback<uint32_t, uint32_t, uint32_t> m_traceWqeTaskStartsNotify;
    TracedCallback<uint32_t, uint32_t, uint32_t> m_traceWqeTaskCompletesNotify;

    void MemTaskStartsNotify(uint32_t nodeId, uint32_t taskId);
    void MemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId);
    void WqeTaskStartsNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId);
    void WqeTaskCompletesNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId);
    void WriteNotifyTaskStarts(uint32_t nodeId, uint32_t jettyNum, uint32_t baseTaskId);
    void WriteNotifyTaskCompletes(uint32_t nodeId, uint32_t jettyNum, uint32_t baseTaskId);

    // 控制器
    bool m_multiPathEnable = false;

    GetTpnRuleT m_getTpnRule = GetTpnRuleT::BY_PEERNODE_PRIORITY;
    bool m_useShortestPaths = true;
    bool m_usePacketSpray = false;
    bool m_ctpUseUnboundSourceJetty = false;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint8_t>, uint32_t>
        m_ctpBoundJettyByFullKey;
    std::map<std::tuple<uint32_t, uint32_t, uint8_t>, uint32_t> m_ctpUnboundJettyBySourceEntityVl;

    Ptr<Node> m_node;              // 当前节点

    uint32_t m_jettyNum = 0;       // 当前节点维护的jettynum,不会重复
    TransportMode m_transportMode{TransportMode::RTP};
    uint32_t m_localEntityId{0};
    uint32_t m_peerEntityId{0};

};

} // namespace ns3

#endif // UB_APP_H
