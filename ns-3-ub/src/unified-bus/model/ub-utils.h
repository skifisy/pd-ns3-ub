// SPDX-License-Identifier: GPL-2.0-only
#ifndef UTILS_H
#define UTILS_H

#include <iostream>
#include <sstream>
#include <chrono>
#include <functional>
#include <map>
#include <fstream>
#include <mutex>
#include <tuple>
#include <vector>
#include "ns3/core-module.h"
#include "ns3/singleton.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-transport.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"
#include "ns3/ptr.h"
#include "ns3/object-factory.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-traffic-gen.h"
#include "ns3/ub-app.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/config-store.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-network-address.h"
#include "ub-tp-connection-manager.h"
#include "ns3/random-variable-stream.h"
#include "ns3/enum.h"
#include "ns3/ub-fault.h"

class UbLinkDelayOffsetTest;

namespace utils {
class UbTraceFileConcurrencyTest;
class UbQueueSamplerEventRetentionTest;
/**
 *  @brief UbUtils单例类
 */
class UbUtils : public ns3::Singleton<UbUtils> {
public:
    // Runtime / trace lifecycle helpers used by examples and tests.
    void PrintTimestamp(const std::string &message);

    void ParseTrace(bool isTest = false);

    void Destroy();

    void CreateTraceDir();

    static std::string PrepareTraceDir(const std::string &configPath);

    static void SetTracePathForTest(const std::string& tracePath);

    // MPI locality helpers used by config-driven builder/tests.
    static uint32_t ExtractMpiRank(uint32_t systemId);

    static bool IsSameMpiRank(uint32_t lhsSystemId, uint32_t rhsSystemId);

    static bool IsSystemOwnedByRank(uint32_t systemId, uint32_t currentRank);

    // Config-driven builder surface used by ub-quick-example and white-box tests.
    void CreateNode(const std::string &filename);

    // Loads traffic records and initializes phase-dependency state in UbTrafficGen.
    std::vector<TrafficRecord> LoadTrafficConfig(const std::string &filename);

    struct TrafficLoadStats
    {
        uint64_t recordCount{0};
        uint32_t maxTaskId{0};
    };

    // Streams traffic records without retaining a full vector in memory.
    void ForEachTrafficRecord(const std::string& filename,
                              const std::function<void(const TrafficRecord&)>& callback);

    // Callback-only row view; string_view fields must not be stored after callback return.
    void ForEachTrafficRecordView(const std::string& filename,
                                  const std::function<void(const TrafficRecordView&)>& callback);

    // Pre-registers phase membership before streaming AddTask calls.
    void RegisterTrafficPhaseDependencies(const std::string& filename);

    TrafficLoadStats RegisterTrafficPhaseDependenciesAndGetStats(const std::string& filename);

    struct LinkDelayOffsetStats
    {
        uint64_t positiveLinkCount{0};
        uint64_t zeroDelayLinkCount{0};
        uint64_t distinctOffsetCount{0};
        uint64_t offsetReuseCount{0};
        uint64_t maxLinksPerOffset{0};
    };

    void CreateTopo(const std::string& filename);

    LinkDelayOffsetStats CreateTopo(const std::string& filename,
                                    ns3::Time offsetWindow,
                                    uint32_t offsetSeed);

    void AddRoutingTable(const std::string &filename);

    void CreateTp(const std::string &filename);

    void SetComponentsAttribute(const std::string &filename);

    // Runtime trace wiring / attribute query / fault helpers.
    void TopoTraceConnect();

    void SingleTpTraceConnect(uint32_t nodeId, uint32_t tpn);

    void ClientTraceConnect(int srcNode);

    static void PfcStateNotify(uint32_t nodeId,
                               uint32_t portId,
                               const std::string& action,
                               uint32_t priority,
                               uint64_t ingressBytes);

    static void PfcDynamicStateNotify(uint32_t nodeId,
                                      uint32_t portId,
                                      uint32_t priority,
                                      uint64_t ingressTotalBytes,
                                      uint64_t sharedUsedBytes,
                                      uint64_t headroomUsedBytes,
                                      uint64_t xoffBytes,
                                      uint64_t xonBytes,
                                      bool pause);

    static void CbfcStateNotify(uint32_t nodeId,
                                uint32_t portId,
                                const std::string& action,
                                uint32_t priority,
                                int32_t availableCredits,
                                uint32_t nextPacketBytes);

    static void CbfcCreditRestoreTraceNotify(uint32_t nodeId,
                                             uint32_t portId,
                                             const std::vector<uint8_t>& credits);

    static void CbfcCreditLevelNotify(uint32_t nodeId,
                                      uint32_t portId,
                                      const std::string& reason,
                                      uint32_t priority,
                                      int32_t availableCredits,
                                      int32_t deltaCredits);

    static void CbfcControlSendNotify(uint32_t nodeId,
                                      uint32_t portId,
                                      const std::string& reason,
                                      int32_t triggerThresholdCells,
                                      int32_t emitMinPendingCells,
                                      const std::vector<int32_t>& pendingCredits,
                                      const std::vector<uint8_t>& sendCredits);

    static void DcqcnMarkNotify(uint32_t nodeId,
                                uint32_t outPort,
                                uint64_t totalQueueBytes,
                                double markProbability);

    static void DcqcnCnpNotify(uint32_t nodeId,
                               uint32_t tpn,
                               const std::string& action,
                               uint8_t ecn,
                               bool location);

    static void DcqcnSenderStateNotify(uint32_t nodeId,
                                       uint32_t tpn,
                                       const std::string& reason,
                                       double alpha,
                                       uint64_t currentRateBps,
                                       uint64_t targetRateBps,
                                       uint64_t bytesSinceLastIncrease,
                                       uint32_t timeRecoveryEvents,
                                       uint32_t byteRecoveryEvents);

    static void CaqmAckNotify(uint32_t nodeId,
                              uint32_t tpn,
                              uint32_t psnStart,
                              uint32_t psnEnd,
                              uint8_t cE,
                              uint8_t iE,
                              uint16_t hintE);

    static void CaqmSenderStateNotify(uint32_t nodeId,
                                      uint32_t tpn,
                                      uint32_t psn,
                                      uint32_t sequence,
                                      uint32_t inFlight,
                                      uint32_t cwnd,
                                      uint8_t cE,
                                      bool iE,
                                      uint16_t hint);

    static bool IsTraceEnabled();

    static bool IsFlowControlTraceEnabled();

    static bool IsCongestionControlTraceEnabled();

    static bool IsQueueTraceEnabled();

    static bool IsTpDebugEnabledFor(uint32_t nodeId, uint32_t tpn);

    static void TpDebugStateNotify(uint32_t nodeId,
                                   uint32_t tpn,
                                   const std::string& reason,
                                   uint64_t psnSndNxt,
                                   uint64_t psnSndUna,
                                   uint64_t inflightPackets,
                                   uint64_t maxInflightPackets,
                                   bool inflightLimited,
                                   bool ccLimited,
                                   bool sendWindowLimited,
                                   uint32_t activeSegments,
                                   uint32_t totalSegments,
                                   uint32_t ackQueueLen,
                                   uint32_t cnpQueueLen);

    void StartQueueSampler();

    bool QueryAttributeInfo(int argc, char *argv[]);

    bool IsFaultEnabled() const;

    void InitFaultMoudle(const std::string &FaultConfigFile);

    static void ResetRuntimeDropDiagnostics();

    static void RecordRuntimePacketDrop(const std::string& reason);

    static uint64_t GetRuntimePacketDropCount();

    static std::string GetRuntimePacketDropReason();

private:
    friend class ::utils::UbTraceFileConcurrencyTest;
    friend class ::utils::UbQueueSamplerEventRetentionTest;
    friend class ::UbLinkDelayOffsetTest;

    static ns3::Time ResolveLinkDelayWithOffset(ns3::Time baseDelay,
                                                ns3::Time offsetWindow,
                                                uint32_t offsetSeed,
                                                uint32_t node1,
                                                uint32_t port1,
                                                uint32_t node2,
                                                uint32_t port2);

    struct TraceFileState
    {
        std::ofstream stream;
        std::string pending;
        std::mutex mutex;
    };

    static constexpr std::size_t TRACE_FLUSH_THRESHOLD_BYTES = 16 * 1024;

    // Runtime trace state shared by current process.
    inline static std::string trace_path;

    inline static std::map<std::string, TraceFileState> files;  // 存储文件名和对应的文件句柄/缓冲
    inline static std::mutex files_mutex;
    inline static std::mutex runtime_drop_mutex;
    inline static uint64_t runtime_packet_drop_count = 0;
    inline static std::string runtime_packet_drop_reason;
    inline static bool queue_sampler_started = false;
    inline static std::map<std::pair<uint32_t, uint32_t>, ns3::EventId> queue_sampler_events;

    ns3::GlobalValue g_fault_enable =
    ns3::GlobalValue("UB_FAULT_ENABLE", "Enable the fault injection module.", ns3::BooleanValue(false), ns3::MakeBooleanChecker());

    // 读取Traffic配置文件
    enum class FIELDCOUNT : int {
       TASKID = 0,
       SOURCENODE = 1,
       DESTNODE = 2,
       DATASIZE,
       OPTYPE,
       PRIORITY,
       DELAY,
       PHASEID,
       DEPENDONPHASES
    };

    struct NodeEle {
        std::string nodeIdStr;

        std::string nodeTypeStr;

        std::string portNumStr;

        std::string forwardDelay;

        std::string allocationDelay;

        std::string systemIdStr;
    };

    std::map<uint32_t, NodeEle> nodeEle_map;

    std::string g_config_path;

    bool TraceEnable = false;
    bool TaskTraceEnable = true;
    bool PacketTraceEnable = true;
    bool PortTraceEnable = true;
    bool RecordTraceEnabled = false;

    // 设置Trace全局变量
    ns3::GlobalValue g_trace_enable = ns3::GlobalValue("UB_TRACE_ENABLE",
                                                       "Master switch for all traces",
                                                       ns3::BooleanValue(true),
                                                       ns3::MakeBooleanChecker());

    ns3::GlobalValue g_task_trace_enable = ns3::GlobalValue("UB_TASK_TRACE_ENABLE",
                                                            "Enable task and WQE level traces",
                                                            ns3::BooleanValue(true),
                                                            ns3::MakeBooleanChecker());

    ns3::GlobalValue g_packet_trace_enable = ns3::GlobalValue("UB_PACKET_TRACE_ENABLE",
                                                              "Enable packet send/ack/receive traces",
                                                              ns3::BooleanValue(true),
                                                              ns3::MakeBooleanChecker());

    ns3::GlobalValue g_port_trace_enable = ns3::GlobalValue("UB_PORT_TRACE_ENABLE",
                                                            "Enable port-level Tx/Rx traces (very noisy)",
                                                            ns3::BooleanValue(true),
                                                            ns3::MakeBooleanChecker());

    ns3::GlobalValue g_parse_enable = ns3::GlobalValue("UB_PARSE_TRACE_ENABLE",
                                                       "Run the Python trace-parsing script after simulation ends.",
                                                       ns3::BooleanValue(true),
                                                       ns3::MakeBooleanChecker());

    ns3::GlobalValue g_record_pkt_trace_enable = ns3::GlobalValue("UB_RECORD_PKT_TRACE",
                                                                  "Record per-hop packet path traces (high overhead).",
                                                                  ns3::BooleanValue(false),
                                                                  ns3::MakeBooleanChecker());

    ns3::GlobalValue g_flow_control_trace_enable =
    ns3::GlobalValue("UB_FLOW_CONTROL_TRACE_ENABLE",
                     "Enable algorithm-emitted flow-control traces such as PFC/CBFC trace files.",
                     ns3::BooleanValue(false),
                     ns3::MakeBooleanChecker());

    ns3::GlobalValue g_congestion_control_trace_enable =
    ns3::GlobalValue("UB_CONGESTION_CONTROL_TRACE_ENABLE",
                     "Enable algorithm-emitted congestion-control traces such as DCQCN/CAQM trace files.",
                     ns3::BooleanValue(false),
                     ns3::MakeBooleanChecker());

    ns3::GlobalValue g_queue_trace_enable =
    ns3::GlobalValue("UB_QUEUE_TRACE_ENABLE",
                     "Enable queue trace files such as QueueTrace_* from event-driven updates and sampling.",
                     ns3::BooleanValue(false),
                     ns3::MakeBooleanChecker());

    ns3::GlobalValue g_python_script_path =
    ns3::GlobalValue("UB_PYTHON_SCRIPT_PATH",
                     "Path to parse_trace.py script (REQUIRED - must be set by user)",
                     ns3::StringValue("/path/to/ns-3-ub-tools/trace_analysis/parse_trace.py"),
                     ns3::MakeStringChecker());

    ns3::GlobalValue g_queue_sample_interval =
    ns3::GlobalValue("UB_QUEUE_SAMPLE_INTERVAL_NS",
                     "Periodic queue sampling interval in ns. 0 disables periodic queue samples.",
                     ns3::UintegerValue(0),
                     ns3::MakeUintegerChecker<uint32_t>());

    ns3::GlobalValue g_tp_debug_enable =
    ns3::GlobalValue("UB_TP_DEBUG_ENABLE",
                     "Enable focused TP sender-state debug trace.",
                     ns3::BooleanValue(false),
                     ns3::MakeBooleanChecker());

    ns3::GlobalValue g_tp_debug_node_id =
    ns3::GlobalValue("UB_TP_DEBUG_NODE_ID",
                     "Focused TP debug trace source node id. UINT32_MAX matches none.",
                     ns3::UintegerValue(std::numeric_limits<uint32_t>::max()),
                     ns3::MakeUintegerChecker<uint32_t>());

    ns3::GlobalValue g_tp_debug_tpn =
    ns3::GlobalValue("UB_TP_DEBUG_TPN",
                     "Focused TP debug trace TP number. UINT32_MAX matches none.",
                     ns3::UintegerValue(std::numeric_limits<uint32_t>::max()),
                     ns3::MakeUintegerChecker<uint32_t>());

    ns3::GlobalValue g_tp_debug_start_ns =
    ns3::GlobalValue("UB_TP_DEBUG_START_NS",
                     "Focused TP debug trace start time in ns.",
                     ns3::UintegerValue(0),
                     ns3::MakeUintegerChecker<uint32_t>());

    ns3::GlobalValue g_tp_debug_end_ns =
    ns3::GlobalValue("UB_TP_DEBUG_END_NS",
                     "Focused TP debug trace end time in ns. 0 means no upper bound.",
                     ns3::UintegerValue(0),
                     ns3::MakeUintegerChecker<uint32_t>());

    static std::string Among(std::string s, std::string ts);

    void SetRecord(int fieldCount, std::string field, TrafficRecord &record);

    void ForEachTrafficRecordInternal(const std::string& filename,
                                      const std::string& action,
                                      const std::function<void(const TrafficRecord&)>& callback);

    void ForEachTrafficRecordViewInternal(
        const std::string& filename,
        const std::string& action,
        const std::function<void(const TrafficRecordView&)>& callback);

    void ForEachTrafficPhaseIndexRecord(const std::string& filename,
                                        const std::function<void(uint32_t, uint32_t)>& callback);

    static void PrintTraceInfo(const std::string& fileName, const std::string& info);

    static void PrintTraceInfoNoTs(const std::string& fileName, const std::string& info);

    static TraceFileState& GetTraceFile(const std::string& fileName);

    static void FlushTraceFile(TraceFileState& fileState);

    static void QueueSampleNotify(uint32_t nodeId, uint32_t portId);
    static void QueueSampleTick(uint32_t nodeId, uint32_t portId, uint32_t intervalNs);

    static void TpFirstPacketSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn,
                                         uint32_t tpMsn, uint32_t psnSndNxt, uint32_t sPort);

    static void TpLastPacketSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn,
                                        uint32_t tpMsn, uint32_t psnSndNxt, uint32_t sPort);

    static void TpLastPacketACKsNotify(uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn,
                                       uint32_t tpMsn, uint32_t psn, uint32_t sPort);

    static void TpLastPacketReceivesNotify(uint32_t nodeId, uint32_t srcTpn, uint32_t dstTpn,
                                           uint32_t tpMsn, uint32_t psn, uint32_t dPort);

    static void TpWqeSegmentSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn);

    static void TpWqeSegmentCompletesNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn);

    static void TpRecvNotify(uint32_t packetUid, uint32_t psn, uint32_t src, uint32_t dst, uint32_t srcTpn,
                             uint32_t dstTpn, ns3::PacketType type, uint32_t size, uint32_t taskId,
                             std::string ackInfo, ns3::UbPacketTraceTag traceTag);

    static void CtpFirstPacketSendsNotify(uint32_t nodeId,
                                          uint32_t taskId,
                                          uint32_t srcNodeId,
                                          uint32_t dstNodeId,
                                          uint32_t srcEntityId,
                                          uint32_t dstEntityId,
                                          uint32_t vl,
                                          uint32_t taSsn,
                                          uint32_t outPort,
                                          uint32_t payloadBytes,
                                          uint32_t opcode);

    static void CtpLastPacketACKsNotify(uint32_t nodeId,
                                        uint32_t taskId,
                                        uint32_t srcNodeId,
                                        uint32_t dstNodeId,
                                        uint32_t srcEntityId,
                                        uint32_t dstEntityId,
                                        uint32_t vl,
                                        uint32_t taSsn,
                                        uint32_t outPort,
                                        uint32_t payloadBytes,
                                        uint32_t opcode);

    static void LdstRecvNotify(uint32_t packetUid,
                               uint32_t src,
                               uint32_t dst,
                               ns3::PacketType type,
                               uint32_t size,
                               uint32_t taskId,
                               ns3::UbPacketTraceTag traceTag);

    static void LdstFirstPacketSendsNotify(uint32_t nodeId, uint32_t taskId);

    static void DagMemTaskStartsNotify(uint32_t nodeId, uint32_t taskId);

    static void DagMemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId);

    static void DagWqeTaskStartsNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId);

    static void DagWqeTaskCompletesNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId);

    static void PortTxNotify(uint32_t nodeId, uint32_t portId, uint32_t size);

    static void PortRxNotify(uint32_t nodeId, uint32_t portId, uint32_t size);

    static void QueueVoqNotify(uint32_t nodeId, uint32_t portId, uint64_t voqBytes);

    static void QueueIngressOccupancyNotify(uint32_t nodeId,
                                            uint32_t inPort,
                                            uint32_t priority,
                                            uint64_t bytes);

    static void QueueEgressEnqueueNotify(uint32_t nodeId,
                                         uint32_t portId,
                                         ns3::Ptr<const ns3::Packet> packet,
                                         uint32_t egressBytes);

    static void QueueEgressDequeueNotify(uint32_t nodeId,
                                         uint32_t portId,
                                         ns3::Ptr<const ns3::Packet> packet,
                                         uint32_t egressBytes);

    static void LdstThreadMemTaskStartsNotify(uint32_t nodeId, uint32_t memTaskId);

    static void LdstMemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId);

    static void LdstThreadFirstPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId);

    static void LdstThreadLastPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId);

    static void LdstLastPacketACKsNotify(uint32_t nodeId, uint32_t taskId);

    static void LdstPeerSendFirstPacketACKsNotify(uint32_t nodeId, uint32_t taskId, uint32_t type);

    static void SwitchLastPacketTraversesNotify(uint32_t nodeId, ns3::UbTransportHeader ubTpHeader);

    // 解析节点范围（如 "1..4"）
    inline void ParseNodeRange(const std::string &rangeStr, NodeEle nodeEle);

    // 读取TP配置文件
    void ParseLine(const std::string &line, Connection &conn);
};

}  // namespace utils

#endif  // UTILS_H
