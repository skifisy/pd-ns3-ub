// SPDX-License-Identifier: GPL-2.0-only
#include "ub-app.h"
#include "ns3/enum.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/callback.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-ctp.h"
#include "ns3/ub-function.h"
#include "ns3/ub-transaction.h"
#include "ub-traffic-gen.h"
#include "ns3/ub-routing-process.h"
#include "ns3/ub-port.h"
#include "ns3/ub-utils.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbApp");

NS_OBJECT_ENSURE_REGISTERED(UbApp);
TypeId UbApp::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbApp")
            .SetParent<Application>()
            .SetGroupName("UnifiedBus")
            .AddConstructor<UbApp>()
            .AddAttribute("EnableMultiPath",
                          "Enable multi-path transport: create TPs on multiple source ports toward the same destination.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&UbApp::m_multiPathEnable),
                          MakeBooleanChecker())
            .AddAttribute("UseShortestPaths",
                          "If true, only create TPs on source ports that belong to shortest paths.",
                          BooleanValue(true),
                          MakeBooleanAccessor(&UbApp::m_useShortestPaths),
                          MakeBooleanChecker())
            .AddAttribute("UsePacketSpray",
                          "If true, packet-spray routing uses a per-packet salt instead of per-flow routing.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&UbApp::m_usePacketSpray),
                          MakeBooleanChecker())
            .AddAttribute("TransportMode",
                          "Transport mode used for URMA traffic.",
                          EnumValue(TransportMode::RTP),
                          MakeEnumAccessor<TransportMode>(&UbApp::m_transportMode),
                          MakeEnumChecker(TransportMode::RTP,
                                          "RTP",
                                          TransportMode::CTP,
                                          "CTP"))
            .AddAttribute("LocalEntityId",
                          "Default local entity id used for CTP URMA traffic.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&UbApp::m_localEntityId),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("PeerEntityId",
                          "Default peer entity id used for CTP URMA traffic.",
                          UintegerValue(0),
                          MakeUintegerAccessor(&UbApp::m_peerEntityId),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("CtpUseUnboundSourceJetty",
                          "When true, CTP reuses one unbound source Jetty per source entity and VL.",
                          BooleanValue(false),
                          MakeBooleanAccessor(&UbApp::m_ctpUseUnboundSourceJetty),
                          MakeBooleanChecker())
            .AddTraceSource("MemTaskStartsNotify",
                            "MEM Task Starts, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceMemTaskStartsNotify),
                            "ns3::UbApp::MemTaskStartsNotify")
            .AddTraceSource("MemTaskCompletesNotify",
                            "MEM Task Completes, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceMemTaskCompletesNotify),
                            "ns3::UbApp::MemTaskCompletesNotify")
            .AddTraceSource("WqeTaskStartsNotify",
                            "WQE Task Starts, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceWqeTaskStartsNotify),
                            "ns3::UbApp::WqeTaskStartsNotify")
            .AddTraceSource("WqeTaskCompletesNotify",
                            "WQE Task Completes, taskId",
                            MakeTraceSourceAccessor(&UbApp::m_traceWqeTaskCompletesNotify),
                            "ns3::UbApp::WqeTaskCompletesNotify");
    return tid;
}

UbApp::UbApp()
{
}

UbApp::~UbApp()
{
}

void UbApp::SetFinishCallback(Callback<void, uint32_t, uint32_t> cb, Ptr<UbJetty> jetty)
{
    jetty->SetClientCallback(cb);
}

void UbApp::SetFinishCallback(Callback<void, uint32_t> cb, Ptr<UbLdstInstance> ubLdstInstance)
{
    ubLdstInstance->SetClientCallback(cb);
}

void UbApp::DoDispose(void)
{
    Application::DoDispose();
}

void
UbApp::SetTransportMode(TransportMode mode)
{
    m_transportMode = mode;
}

void
UbApp::SetLocalEntityId(uint32_t localEntityId)
{
    m_localEntityId = localEntityId;
}

void
UbApp::SetPeerEntityId(uint32_t peerEntityId)
{
    m_peerEntityId = peerEntityId;
}

void UbApp::SendTraffic(TrafficRecord record)
{
    NS_ABORT_MSG_IF(record.priority < 0 ||
                        static_cast<uint32_t>(record.priority) > UB_PRIORITY_MAX,
                    "Invalid priority field in traffic.csv; valid range is 0.."
                        << static_cast<uint32_t>(UB_PRIORITY_MAX));

    UbTrafficGen::RuntimeTask task;
    task.taskId = static_cast<uint32_t>(record.taskId);
    task.sourceNode = static_cast<uint32_t>(record.sourceNode);
    task.destNode = static_cast<uint32_t>(record.destNode);
    task.dataSize = static_cast<uint32_t>(record.dataSize);
    task.priority = static_cast<uint8_t>(record.priority);
    task.delay = record.delay.empty() ? Time(0) : Time(record.delay);
    task.srcEntityId = record.srcEntityId;
    task.dstEntityId = record.dstEntityId;
    task.hasSrcEntityId = record.hasSrcEntityId;
    task.hasDstEntityId = record.hasDstEntityId;
    if (record.opType == "URMA_WRITE") {
        task.op = UbTrafficGen::RuntimeTaskOp::URMA_WRITE;
    } else if (record.opType == "URMA_READ") {
        task.op = UbTrafficGen::RuntimeTaskOp::URMA_READ;
    } else if (record.opType == "MEM_STORE") {
        task.op = UbTrafficGen::RuntimeTaskOp::MEM_STORE;
    } else if (record.opType == "MEM_LOAD") {
        task.op = UbTrafficGen::RuntimeTaskOp::MEM_LOAD;
    } else {
        NS_ASSERT_MSG(0, "TaOpcode Not Exist");
    }
    SendTraffic(task);
}

void
UbApp::SendCtpUrmaTraffic(TrafficRecord record)
{
    NS_ABORT_MSG_IF(record.priority < 0 ||
                        static_cast<uint32_t>(record.priority) > UB_PRIORITY_MAX,
                    "Invalid priority field in traffic.csv; valid range is 0.."
                        << static_cast<uint32_t>(UB_PRIORITY_MAX));

    UbTrafficGen::RuntimeTask task;
    task.taskId = static_cast<uint32_t>(record.taskId);
    task.sourceNode = static_cast<uint32_t>(record.sourceNode);
    task.destNode = static_cast<uint32_t>(record.destNode);
    task.dataSize = static_cast<uint32_t>(record.dataSize);
    task.priority = static_cast<uint8_t>(record.priority);
    task.srcEntityId = record.srcEntityId;
    task.dstEntityId = record.dstEntityId;
    task.hasSrcEntityId = record.hasSrcEntityId;
    task.hasDstEntityId = record.hasDstEntityId;
    if (record.opType == "URMA_WRITE") {
        task.op = UbTrafficGen::RuntimeTaskOp::URMA_WRITE;
    } else if (record.opType == "URMA_READ") {
        task.op = UbTrafficGen::RuntimeTaskOp::URMA_READ;
    } else {
        NS_ASSERT_MSG(0, "CTP URMA traffic requires URMA read/write op");
    }
    SendCtpUrmaTraffic(task);
}

void
UbApp::SendCtpUrmaTraffic(UbTrafficGen::RuntimeTask task)
{
    NS_ABORT_MSG_IF(task.op != UbTrafficGen::RuntimeTaskOp::URMA_WRITE &&
                        task.op != UbTrafficGen::RuntimeTaskOp::URMA_READ,
                    "CTP URMA traffic requires URMA read/write op");

    Ptr<UbController> controller = GetNode()->GetObject<UbController>();
    NS_ABORT_MSG_IF(controller == nullptr, "CTP transport mode requires UbController");
    Ptr<UbFunction> ubFunc = controller->GetUbFunction();
    NS_ABORT_MSG_IF(ubFunc == nullptr, "CTP transport mode requires UbFunction");
    NS_ABORT_MSG_IF(controller->GetUbTransaction() == nullptr,
                    "CTP transport mode requires UbTransaction");

    Ptr<UbCtpTransportService> service = controller->GetCtpTransportService();
    service->SetUseShortestPaths(m_useShortestPaths);
    service->SetUsePacketSpray(m_usePacketSpray);

    const uint32_t srcEntityId = task.hasSrcEntityId ? task.srcEntityId : m_localEntityId;
    const uint32_t dstEntityId = task.hasDstEntityId ? task.dstEntityId : m_peerEntityId;
    const uint8_t vl = task.priority;
    uint32_t jettyNum = m_jettyNum;
    Ptr<UbJetty> jetty;
    if (m_ctpUseUnboundSourceJetty)
    {
        jetty = GetOrCreateCtpUnboundJetty(task.sourceNode, srcEntityId, vl, &jettyNum);
    }
    else
    {
        jetty = GetOrCreateCtpBoundJetty(task.sourceNode,
                                         task.destNode,
                                         srcEntityId,
                                         dstEntityId,
                                         vl,
                                         &jettyNum);
    }
    service->PrepareJetty(jetty);

    NS_LOG_INFO("CTP WQE Starts, jettyNum: " << jettyNum << " taskId: " << task.taskId);
    WqeTaskStartsNotify(GetNode()->GetId(), jettyNum, task.taskId);

    TaOpcode type = task.op == UbTrafficGen::RuntimeTaskOp::URMA_READ
                        ? TaOpcode::TA_OPCODE_READ
                        : TaOpcode::TA_OPCODE_WRITE;
    Ptr<UbWqe> wqe =
        ubFunc->CreateWqe(task.sourceNode, task.destNode, task.dataSize, task.taskId, type);
    wqe->SetSrcEntityId(srcEntityId);
    wqe->SetDstEntityId(dstEntityId);
    wqe->SetPriority(task.priority);
    controller->GetUbTransaction()->SetTransactionServiceMode(jettyNum, TransactionServiceMode::ROI);
    jetty->SetNodeId(GetNode()->GetId());
    controller->GetUbTransaction()->AddWqe(jettyNum, wqe);
    jetty->PushWqe(wqe);

    UbCtpEntityKey key{.srcNodeId = task.sourceNode,
                       .srcEntityId = srcEntityId,
                       .dstNodeId = task.destNode,
                       .dstEntityId = dstEntityId,
                       .vl = vl};
    const UbCtpEntityKey jettyKey =
        m_ctpUseUnboundSourceJetty ? service->MakeSourceGroupKeyForRequest(key) : key;
    service->StartJetty(jetty, jettyKey);
}

Ptr<UbJetty>
UbApp::GetOrCreateCtpBoundJetty(uint32_t sourceNode,
                                uint32_t destNode,
                                uint32_t srcEntityId,
                                uint32_t dstEntityId,
                                uint8_t vl,
                                uint32_t* jettyNum)
{
    NS_ABORT_MSG_IF(jettyNum == nullptr, "CTP bound Jetty output number is required");
    Ptr<UbFunction> ubFunc = GetNode()->GetObject<UbController>()->GetUbFunction();
    const auto key = std::make_tuple(sourceNode, destNode, srcEntityId, dstEntityId, vl);
    auto it = m_ctpBoundJettyByFullKey.find(key);
    if (it != m_ctpBoundJettyByFullKey.end())
    {
        *jettyNum = it->second;
        Ptr<UbJetty> jetty = ubFunc->GetJetty(*jettyNum);
        NS_ABORT_MSG_IF(jetty == nullptr, "CTP bound Jetty table points to missing Jetty");
        return jetty;
    }

    while (ubFunc->IsJettyExists(m_jettyNum))
    {
        ++m_jettyNum;
    }
    ubFunc->CreateJetty(sourceNode, destNode, m_jettyNum);
    Ptr<UbJetty> jetty = ubFunc->GetJetty(m_jettyNum);
    NS_ABORT_MSG_IF(jetty == nullptr, "CTP URMA failed to create bound Jetty");
    SetFinishCallback(MakeCallback(&UbApp::OnTaskCompleted, this), jetty);
    *jettyNum = m_jettyNum;
    m_ctpBoundJettyByFullKey[key] = m_jettyNum;
    ++m_jettyNum;
    return jetty;
}

Ptr<UbJetty>
UbApp::GetOrCreateCtpUnboundJetty(uint32_t sourceNode,
                                  uint32_t srcEntityId,
                                  uint8_t vl,
                                  uint32_t* jettyNum)
{
    NS_ABORT_MSG_IF(jettyNum == nullptr, "CTP unbound Jetty output number is required");
    Ptr<UbFunction> ubFunc = GetNode()->GetObject<UbController>()->GetUbFunction();
    const auto key = std::make_tuple(sourceNode, srcEntityId, vl);
    auto it = m_ctpUnboundJettyBySourceEntityVl.find(key);
    if (it != m_ctpUnboundJettyBySourceEntityVl.end())
    {
        *jettyNum = it->second;
        Ptr<UbJetty> jetty = ubFunc->GetJetty(*jettyNum);
        NS_ABORT_MSG_IF(jetty == nullptr, "CTP unbound Jetty table points to missing Jetty");
        return jetty;
    }

    while (ubFunc->IsJettyExists(m_jettyNum))
    {
        ++m_jettyNum;
    }
    ubFunc->CreateJetty(sourceNode, m_jettyNum);
    Ptr<UbJetty> jetty = ubFunc->GetJetty(m_jettyNum);
    NS_ABORT_MSG_IF(jetty == nullptr, "CTP URMA failed to create unbound Jetty");
    SetFinishCallback(MakeCallback(&UbApp::OnTaskCompleted, this), jetty);
    *jettyNum = m_jettyNum;
    m_ctpUnboundJettyBySourceEntityVl[key] = m_jettyNum;
    ++m_jettyNum;
    return jetty;
}

void UbApp::SendTraffic(UbTrafficGen::RuntimeTask task)
{
    if (task.priority == 0) {
        NS_LOG_DEBUG("Task uses the highest priority, not recommended.");
    }

    if (task.op == UbTrafficGen::RuntimeTaskOp::MEM_STORE ||
        task.op == UbTrafficGen::RuntimeTaskOp::MEM_LOAD) {
        // 内存语义发送
        UbMemOperationType type = UbMemOperationType::STORE;
        if (task.op == UbTrafficGen::RuntimeTaskOp::MEM_LOAD) {
            type = UbMemOperationType::LOAD;
        }
        auto ldstInstance = GetNode()->GetObject<UbLdstInstance>();
        SetFinishCallback(MakeCallback(&UbApp::OnMemTaskCompleted, this), ldstInstance);
        NS_LOG_INFO("MEM Task Starts, taskId: " << task.taskId);
        MemTaskStartsNotify(GetNode()->GetId(), task.taskId);
        std::vector<uint32_t> threadIds = {0, 1};
        ldstInstance->HandleLdstTask(task.sourceNode,
                                     task.destNode,
                                     task.dataSize,
                                     task.taskId,
                                     task.priority,
                                     type,
                                     threadIds,
                                     0);
    } else if (task.op == UbTrafficGen::RuntimeTaskOp::URMA_WRITE ||
               task.op == UbTrafficGen::RuntimeTaskOp::URMA_READ) {
        if (m_transportMode == TransportMode::CTP)
        {
            SendCtpUrmaTraffic(task);
            return;
        }

        // URMA发送
        Ptr<UbFunction> ubFunc = GetNode()->GetObject<UbController>()->GetUbFunction();
        Ptr<UbTransaction> ubTa = GetNode()->GetObject<UbController>()->GetUbTransaction();
        bool jettyExist = ubFunc->IsJettyExists(m_jettyNum);
        if (jettyExist) {
            NS_LOG_ERROR("Jetty already exists");
            return;
        }
        ubFunc->CreateJetty(task.sourceNode, task.destNode, m_jettyNum);
        vector<uint32_t> tpns = GetNode()->GetObject<UbController>()->GetTpConnManager()->GetTpns(
            m_getTpnRule, m_useShortestPaths, m_multiPathEnable, task.sourceNode,
            task.destNode, UINT32_MAX, UINT32_MAX, task.priority);
        bool bindRst = ubTa->JettyBindTp(task.sourceNode, task.destNode, m_jettyNum, m_multiPathEnable, tpns);
        if (bindRst) {
            Ptr<UbJetty> curr_jetty = ubFunc->GetJetty(m_jettyNum);
            SetFinishCallback(MakeCallback(&UbApp::OnTaskCompleted, this), curr_jetty);
            NS_LOG_INFO("WQE Starts, jettyNum: " << m_jettyNum << " taskId: " << task.taskId);
            NS_LOG_INFO("Src: " << task.sourceNode << " Dst: " << task.destNode);
            WqeTaskStartsNotify(GetNode()->GetId(), m_jettyNum, task.taskId);
            NS_LOG_INFO("[APPLICATION INFO] taskId: " << task.taskId << ",start time:" <<
                Simulator::Now().GetNanoSeconds() << "ns");
            TaOpcode type = task.op == UbTrafficGen::RuntimeTaskOp::URMA_READ
                                ? TaOpcode::TA_OPCODE_READ
                                : TaOpcode::TA_OPCODE_WRITE;
            Ptr<UbWqe> wqe =
                ubFunc->CreateWqe(task.sourceNode, task.destNode, task.dataSize, task.taskId, type);
            ubFunc->PushWqeToJetty(wqe, m_jettyNum);
        }
        m_jettyNum++; // m_jettyNum 在client里是唯一的，不重复的
    } else {
            NS_ASSERT_MSG(0, "TaOpcode Not Exist");
    }
}

void UbApp::OnTaskCompleted(uint32_t taskId, uint32_t jettyNum)
{
    NS_LOG_FUNCTION(this << taskId);
    NS_LOG_INFO("WQE Completes, jettyNum: " << jettyNum << " taskId: " << taskId);
    WqeTaskCompletesNotify(GetNode()->GetId(), jettyNum, taskId);
    NS_LOG_INFO("[APPLICATION INFO] taskId: " << taskId << ",finish time:" << Simulator::Now().GetNanoSeconds() << "ns");
    if (m_transportMode == TransportMode::RTP)
    {
        // 删除无用tp
        auto cleanup = UbTrafficGen::Get()->GetTaskCleanupInfoById(taskId);
        GetNode()->GetObject<UbController>()->GetTpConnManager()->RemoveUselessTps(
            jettyNum, cleanup.sourceNode, cleanup.destNode, cleanup.priority);
    }
    UbTrafficGen::Get()->OnTaskCompleted(taskId);
}

void UbApp::OnTestTaskCompleted(uint32_t taskId, uint32_t jettyNum)
{
    NS_LOG_FUNCTION(this << taskId);
    NS_LOG_INFO("WQE Completes, jettyNum:" << jettyNum << " taskId:" << taskId);
    WqeTaskCompletesNotify(GetNode()->GetId(), jettyNum, taskId);
    NS_LOG_INFO("[APPLICATION INFO] taskId:" << taskId << ",finish time:" << Simulator::Now().GetNanoSeconds() << "ns");
    Ptr<UbFunction> ubFunc = GetNode()->GetObject<UbController>()->GetUbFunction();
    UbTrafficGen::Get()->OnTaskCompleted(taskId);
}

void UbApp::OnMemTaskCompleted(uint32_t taskId)
{
    NS_LOG_FUNCTION(this << taskId);
    NS_LOG_INFO("MEM Task Completes, taskId: " << taskId);
    MemTaskCompletesNotify(GetNode()->GetId(), taskId);
    NS_LOG_INFO("[APPLICATION INFO] taskId: " << taskId << ",finish time:" << Simulator::Now().GetNanoSeconds() << "ns");
    UbTrafficGen::Get()->OnTaskCompleted(taskId);
}

void UbApp::MemTaskStartsNotify(uint32_t nodeId, uint32_t taskId)
{
    m_traceMemTaskStartsNotify(nodeId, taskId);
}

void UbApp::MemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId)
{
    m_traceMemTaskCompletesNotify(nodeId, taskId);
}

void UbApp::WqeTaskStartsNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId)
{
    m_traceWqeTaskStartsNotify(nodeId, jettyNum, taskId);
}

void UbApp::WqeTaskCompletesNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId)
{
    m_traceWqeTaskCompletesNotify(nodeId, jettyNum, taskId);
}

} // namespace ns3
