// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_FUNCTION_H
#define UB_FUNCTION_H

#include <ns3/node.h>
#include <set>
#include <unordered_map>
#include "ns3/ub-sliding-bitmap-window.h"
#include "ub-transport.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-network-address.h"
#include "ns3/random-variable-stream.h"
#include "ub-ldst-api.h"
#include "ub-transaction.h"

namespace ns3 {
    class UbController;
    /**
    * @brief 管理jetty
    */
    class UbFunction : public Object {
    public:

        static TypeId GetTypeId(void);

        UbFunction();
        virtual ~UbFunction();
        Ptr<UbLdstApi> GetUbLdstApi();

        // Jetty management
        /**
         * @brief Create a new jetty
         * @param src Source identifier
         * @param dest Destination identifier
         * @param jettyNum Jetty identifier defined by user
         * @return Pointer to created jetty
         */
        void CreateJetty(uint32_t src, uint32_t dest, uint32_t jettyNum);
        void CreateJetty(uint32_t src, uint32_t jettyNum);

        void Init(uint32_t nodeId);

        Ptr<UbTransaction> GetUbTransaction();

        bool IsJettyExists(uint32_t jettyNum);

        Ptr<UbJetty> GetJetty(uint32_t jettyNum);

        /**
         * @brief Destroy jetty by parameters
         * @param jettyNum Jetty identifier
         */
        void DestroyJetty(uint32_t jettyNum);

        /**
         * @brief Assemble wqe
         * @param Client传入URMA任务
         */
        Ptr<UbWqe> CreateWqe(uint32_t src,
                             uint32_t dest,
                             uint32_t size,
                             uint32_t wqeId,
                             TaOpcode type);

        Ptr<UbWqe> CreateWqe(uint32_t src, uint32_t dest, uint32_t size, uint32_t wqeId);

        /**
         * @brief Assemble wqe and push back in Jetty
         * @param Client传入URMA任务
         */
        void PushWqeToJetty(Ptr<UbWqe> wqe, uint32_t jettyNum);
    private:
        void DoDispose() override;
        Ptr<UbTransaction> GetTransaction();
        Ptr<UbLdstApi> m_ldstApi;
        std::vector<Ptr<UbJetty>> m_jettyVector;
        uint32_t m_nodeId;
        std::map<uint32_t, Ptr<UbJetty>> m_numToJetty;
    
    };

    /**
     * @brief Jetty 数据结构
     *
     * 表示一个逻辑传输通道，负责管理特定源-目的对之间的WQE队列和传输状态。
     * 每个Jetty有唯一的标识符，并维护相关的传输参数和回调函数。
     */
    class UbJetty : public Object {
    public:
        static TypeId GetTypeId(void);
        UbJetty();
        Ptr<UbWqeSegment> GetNextWqeSegment();
        bool ProcessWqeSegmentComplete(uint32_t taSsnFin);
        void RightShiftBitset(uint32_t shiftCount);
        void SetTaSegmentBytes(uint32_t bytes);
        uint32_t GetTaSegmentBytes() const;

        Ptr<UbWqeSegment> GenWqeSegment(Ptr<UbWqe> wqe, uint32_t segment_size);

        void Init();

        void IncreaseTaSsnSndNxt()
        {
            m_taSsnSndNxt++;
        }

        void PushWqe(Ptr<UbWqe> ubQqe);

        uint32_t GetJettyNum()
        {
            return m_jettyNum;
        }

        void SetJettyNum(const uint32_t jettyNum)
        {
            m_jettyNum = jettyNum;
        }

        bool IsValid(); // Jetty是否可用
        bool IsReadyToSend();

        uint32_t GetSrc() const
        {
            return m_src;
        }

        uint32_t GetDest() const
        {
            return m_dest;
        }

        bool HasBoundDest() const
        {
            return m_dest != UINT32_MAX;
        }

        uint8_t GetSport() const
        {
            return m_sport;
        }

        uint8_t GetDport() const
        {
            return m_dport;
        }

        bool IsLimited()
        {
            return (m_taSsnSndNxt - m_taSsnSndUna) >= m_inflightMax;
        }

        void SetSrc(uint32_t src)
        {
            m_src = src;
        }

        void SetDest(uint32_t dest)
        {
            m_dest = dest;
        }

        void ClearDest()
        {
            m_dest = UINT32_MAX;
        }

        bool CanAcceptWqe(Ptr<UbWqe> wqe) const;

        void SetSport(uint8_t sport)
        {
            m_sport = sport;
        }

        void SetDport(uint8_t dport)
        {
            m_dport = dport;
        }

        void ResetSsnAckBitset(uint32_t taOooAckThreshold)
        {
            m_ssnAckWindow.Resize(taOooAckThreshold);
            m_ssnAckWindow.Reset(m_taSsnSndUna);
        }

        void SetClientCallback(Callback<void, uint32_t, uint32_t> cb);
        void SetCompletionObserver(Callback<void, uint32_t, uint32_t> cb);
        Callback<void, uint32_t, uint32_t> FinishCallback;

        void SetNodeId(uint32_t nodeId) {m_nodeId = nodeId; }

        uint32_t GetNodeId() { return m_nodeId; }

        void SetTaSsnSendWindowForTest(uint32_t sndUna, uint32_t sndNxt)
        {
            m_taSsnSndUna = sndUna;
            m_taSsnSndNxt = sndNxt;
            m_ssnAckWindow.Reset(m_taSsnSndUna);
        }

        uint32_t GetTaSsnSndUnaForTest() const
        {
            return m_taSsnSndUna;
        }

        uint32_t GetTaSsnSndNxtForTest() const
        {
            return m_taSsnSndNxt;
        }

    private:
        Ptr<UbTransaction> GetTransaction();
        void DoDispose() override;
        std::vector<Ptr<UbWqe>> m_wqeVector;
        // ========== Jetty标识信息 ==========
        uint32_t m_jettyNum; // JettyNum UB协议报文头携带（24位）用于标识

        // ========== 传输参数 ==========
        uint32_t m_nodeId;
        uint32_t m_src;  // 源节点标识符
        uint32_t m_dest{UINT32_MAX}; // 目的节点标识符；UINT32_MAX 表示 unbound Jetty
        uint8_t m_sport;
        uint8_t m_dport;
        uint32_t m_inflightMax = 512;
        uint32_t m_taSegmentBytes = UB_WQE_TA_SEGMENT_BYTE;

        // ========== Jetty队列信息 ==========
        // 当前只在WQE推入jetty中才会更新
        uint64_t m_taMsnCnt = 0; // 当前jetty层WQE计数器，数字上等于目前已经从client拿到的所有WQE数量

        // 当前jetty WQE Segment计数器，数字上等于目前已经从client拿到的所有WQE产生的WQE segment数量
        uint32_t m_taSsnCnt = 0;

        uint32_t m_oooAckThreshold; // out of order ACK threshold
        uint32_t m_taSsnSndNxt = 0;                                 // TA层下一个待发送的分段序号
        uint32_t m_taSsnSndUna = 0;                                 // TA层未确认的最小分段序号
        UbSlidingBitmapWindow m_ssnAckWindow{UB_JETTY_TASSN_OOO_THRESHOLD}; // 接收维护的bitmap
        Callback<void, uint32_t, uint32_t> m_completionObserver;
        // 每个jetty统一编号，单调升，作为WqeSegment的编号统计
        //   |----ssn5----|------ssn4-------|------ssn3----->
        //   |----ssn2----|----ssn1---|---startssn0----->
        //                               taSsnNxt
        // 									taSsnUna
        // wqe.burst_size = 64*1024
        // m_messages = m_wqeVector
        // WQE0 = WqeSegment0 + WqeSegment1 + WqeSegment2
        // WQE1 = WqeSegment3 + WqeSegment4 + ...

        /**
         * @brief 检查并移除已完成的WQE
         */
        void CheckAndRemoveCompletedWqe();

        /**
         * @brief 检查指定WQE是否已完成
         * @param wqe 要检查的WQE
         * @return true如果WQE已完成，false否则
         */
        bool IsWqeCompleted(Ptr<UbWqe> wqe);
    };

} // namespace ns3

#endif /* UB_FUNCTION_H */
