// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TRANSACTION_H
#define UB_TRANSACTION_H

#include <ns3/node.h>
#include <vector>
#include <unordered_map>
#include "ns3/ub-datatype.h"
#include "ns3/ub-network-address.h"
#include "ns3/random-variable-stream.h"
#include "ns3/callback.h"

namespace ns3 {
    /**
     * @brief Transaction Service Mode enumeration
     */
    enum class TransactionServiceMode : uint8_t {
        ROI = 0,    // Reliable Ordered by Initiator
        ROT = 1,    // Reliable Ordered by Target
        ROL = 2,    // Reliable Ordered by Lower Layer
        UNO = 3     // Unreliable No Order
    };

    class UbController;
    class UbJetty;
    class UbFunction;
    class UbTransportChannel;
    /**
    * @brief
    */
    class UbTransaction : public Object {
    public:

        static TypeId GetTypeId(void);

        UbTransaction();
        UbTransaction(Ptr<Node> node);
        virtual ~UbTransaction();

        Ptr<UbFunction> GetFunction();

        Ptr<UbJetty> GetJetty(uint32_t jettyNum);

        bool JettyBindTp(uint32_t src, uint32_t dest, uint32_t jettyNum, bool multiPath, std::vector<uint32_t> tpns);

        void DestroyJettyTpMap(uint32_t jettyNum);

        const std::vector<Ptr<UbTransportChannel>> GetJettyRelatedTpVec(uint32_t jettyNum);

        std::vector<Ptr<UbJetty>> GetTpRelatedJettyVec(uint32_t tpn);

        // function层调用
        void TriggerScheduleWqeSegment(uint32_t jettyNum);

        // tp调用
        void ApplyScheduleWqeSegment(Ptr<UbTransportChannel> tp);

        bool ProcessWqeSegmentComplete(Ptr<UbWqeSegment> wqeSegment);
        void HandleInboundTaUnit(uint32_t localTpn, Ptr<UbWqeSegment> segment);
        Ptr<UbWqeSegment> ProcessInboundTaRequest(Ptr<UbWqeSegment> request);
        bool ProcessInboundTaResponse(Ptr<UbWqeSegment> response);
        uint64_t DeriveRemoteAddressForTest(const Ptr<UbWqeSegment>& request) const
        {
            return DeriveRemoteAddress(request);
        }

        void TriggerTpTransmit(uint32_t jettyNum);

        void TpInit(Ptr<UbTransportChannel> tp);

        void TpDeinit(uint32_t tpn);

        // 判断wqe是否能发送
        bool IsOrderedByInitiator(uint32_t jettyNum, Ptr<UbWqe> wqe);
        // TODO: support ROT
        bool IsOrderedByTarget(Ptr<UbWqe> wqe);
        // TODO: support Reliable
        bool IsReliable(Ptr<UbWqe> wqe);
        bool IsUnreliable(Ptr<UbWqe> wqe);

        void SetTransactionServiceMode(uint32_t jettyNum, TransactionServiceMode mode);
        TransactionServiceMode GetTransactionServiceMode(uint32_t jettyNum);

        // 新增wqe任务
        void AddWqe(uint32_t jettyNum, Ptr<UbWqe> wqe);

        // 某个wqe完成，刷新状态
        void WqeFinish(uint32_t jettyNum, Ptr<UbWqe> wqe);

        // 判断本地tp是否处于使用状态
        bool IsTpInUse(uint32_t tpn);

        // 判断与本地tp配对的远程tp是否处于使用状态
        bool IsPeerTpInUse(uint32_t tpn);

        std::vector<uint32_t> GetUselessTpns();
    private:

        void DoDispose() override;

        void ScheduleWqeSegment(Ptr<UbTransportChannel> tp);

        void OnScheduleWqeSegmentFinish(Ptr<UbWqeSegment> segment);

        bool IsUrmaReadWriteRequest(const Ptr<UbWqeSegment>& segment);

        void ValidateUrmaServiceModeOrDie(uint32_t jettyNum, const Ptr<UbWqeSegment>& segment);
        Ptr<UbWqeSegment> ExecuteRemoteWriteAndBuildAck(uint32_t localTpn,
                                                        Ptr<UbWqeSegment> request);
        Ptr<UbWqeSegment> ExecuteRemoteReadAndBuildResponse(uint32_t localTpn,
                                                            Ptr<UbWqeSegment> request);
        void CompleteLocalRequestFromResponse(Ptr<UbWqeSegment> response);
        uint64_t DeriveRemoteAddress(const Ptr<UbWqeSegment>& request) const;

        uint32_t m_nodeId;

        // Tpn和Tp的对应map
        std::map<uint32_t, Ptr<UbTransportChannel>> m_tpnMap;
        // Jetty和TP的绑定关系
        std::map<uint32_t, std::vector<Ptr<UbTransportChannel>>> m_jettyTpGroup;
        // 每个 Jetty 下次从哪个 TP 开始调度，避免短 WQE 总被第一个 TP 抢占。
        std::map<uint32_t, uint32_t> m_jettyTpNextIndex;
        // 新 Jetty 的初始 TP 轮转位置；一任务一 Jetty 的短流也能展开到多 TP。
        uint32_t m_nextJettyTpStartIndex = 0;
        // Tp与jetty的绑定关系
        std::map<uint32_t, std::vector<Ptr<UbJetty>>> m_tpRelatedJetties;
        //  TP收到的各个remote解析后存储的segment
        // tp1: remoteJetty1: [seg1, seg2, ...]
        //      remoteJetty2: [seg3, seg4, ...]
        // tp2: remoteJetty3: [seg5, seg6, ...]
        //      remoteJetty4: [seg7, seg8, ...]
        std::map<uint32_t, std::map<uint32_t, std::vector<Ptr<UbWqeSegment>>>> m_tpRelatedRemoteRequests;

        // 记录每个TP下次轮询的开始位置
        std::map<uint32_t, uint32_t> m_tpRRIndex;
        // 记录每个TP当前是否正处于调度WqeSegment的过程中
        std::map<uint32_t, bool> m_tpSchedulingStatus;
        Ptr<UniformRandomVariable> m_random;        //随机数产生工具，伪随机，多次仿真可复现

        Callback<void, Ptr<UbWqeSegment>> m_pushWqeSegmentToTpCb;
        // 记录每个jetty对应的serviceMode
        std::map<uint32_t, TransactionServiceMode> m_serviceMode;
        // 每个jetty存储wqe的id顺序
        std::map<uint32_t, std::vector<uint32_t>> m_jettyOrderedWqe;
        std::map<std::pair<uint32_t, uint64_t>, uint32_t> m_memoryStore;
    };

} // namespace ns3

#endif /* UB_TRANSACTION_H */
