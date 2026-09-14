// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/log.h"
#include "ns3/simulator.h"

#include "ns3/ub-datatype.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-transaction.h"
#include "ns3/ub-function.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbFunction");

NS_OBJECT_ENSURE_REGISTERED(UbFunction);

TypeId UbFunction::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbFunction")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus");
    return tid;
}

UbFunction::UbFunction()
{
    NS_LOG_DEBUG("UbFunction created");
    m_ldstApi = CreateObject<UbLdstApi>();
}

UbFunction::~UbFunction()
{
    m_numToJetty.clear();
}

Ptr<UbLdstApi> UbFunction::GetUbLdstApi()
{
    return m_ldstApi;
}

void UbFunction::Init(uint32_t nodeId)
{
    m_nodeId = nodeId;
    m_ldstApi->SetNodeId(m_nodeId);
}

void UbFunction::CreateJetty(uint32_t src, uint32_t dest, uint32_t jettyNum)
{
    NS_LOG_DEBUG(this << src << dest << jettyNum);
    // 创建新的Jetty
    Ptr<UbJetty> jetty = CreateObject<UbJetty>();
    jetty->Init();
    jetty->SetNodeId(m_nodeId);
    jetty->SetJettyNum(jettyNum);
    jetty->SetSrc(src);
    jetty->SetDest(dest);
    m_numToJetty[jettyNum] = jetty;
    NS_LOG_DEBUG("Created jetty success");
}

void UbFunction::CreateJetty(uint32_t src, uint32_t jettyNum)
{
    NS_LOG_DEBUG(this << src << jettyNum);
    Ptr<UbJetty> jetty = CreateObject<UbJetty>();
    jetty->Init();
    jetty->SetNodeId(m_nodeId);
    jetty->SetJettyNum(jettyNum);
    jetty->SetSrc(src);
    jetty->ClearDest();
    m_numToJetty[jettyNum] = jetty;
    NS_LOG_DEBUG("Created unbound jetty success");
}

bool UbFunction::IsJettyExists(uint32_t jettyNum)
{
    if (m_numToJetty.find(jettyNum) != m_numToJetty.end()) {
        return true;
    }
    return false;
}

Ptr<UbJetty> UbFunction::GetJetty(uint32_t jettyNum)
{
    auto it = m_numToJetty.find(jettyNum);
    if (it != m_numToJetty.end()) {
        return it->second;
    }

    NS_LOG_DEBUG("Jetty not found");
    return nullptr;
}

Ptr<UbTransaction> UbFunction::GetTransaction()
{
    return NodeList::GetNode(m_nodeId)->GetObject<UbController>()->GetUbTransaction();
}

void UbFunction::DestroyJetty(uint32_t jettyNum)
{
    NS_LOG_DEBUG(this);
    auto it = m_numToJetty.find(jettyNum);
    if (it != m_numToJetty.end()) {
        m_numToJetty.erase(it);
        NS_LOG_DEBUG("Destroyed ");
    } else {
        NS_LOG_WARN("Jetty  not found for destruction");
    }

    // 删除m_jettyVector中元素
    for (size_t i = 0; i < m_jettyVector.size(); i++) {
        if (m_jettyVector[i]->GetJettyNum() == jettyNum) {
            m_jettyVector.erase(m_jettyVector.begin() + i);
            break;
        }
    }
}

Ptr<UbWqe> UbFunction::CreateWqe(uint32_t src,
                                 uint32_t dest,
                                 uint32_t size,
                                 uint32_t wqeId,
                                 TaOpcode type)
{
    NS_LOG_DEBUG(this);
    // 创建新的 WQE
    Ptr<UbWqe> ubWqe = CreateObject<UbWqe>();
    ubWqe->SetSrc(src);
    ubWqe->SetDest(dest);
    ubWqe->SetType(type);
    ubWqe->SetSize(size);
    ubWqe->SetWqeId(wqeId);
    ubWqe->SetSegmentKind(UbTransactionSegmentKind::REQUEST);
    // These fields are undefined before the WQE is enqueued into a concrete Jetty.
    ubWqe->SetOriginJettyNum(UINT32_MAX);
    ubWqe->SetRequestTassn(UINT32_MAX);
    ubWqe->SetRequestOpcode(type);
    ubWqe->SetResponseBytes(type == TaOpcode::TA_OPCODE_READ ? size : 0);
    ubWqe->SetRemoteAddress(0);
    ubWqe->SetNeedsTransactionResponse(type == TaOpcode::TA_OPCODE_WRITE ||
                                       type == TaOpcode::TA_OPCODE_READ);
    ubWqe->SetResLenBytes(size);
    ubWqe->SetPayloadBytes(type == TaOpcode::TA_OPCODE_READ ? 0 : size);
    ubWqe->SetCarrierBytes(type == TaOpcode::TA_OPCODE_READ ? ubWqe->GetTaSsnSize() : size);
    return ubWqe;
}

Ptr<UbWqe> UbFunction::CreateWqe(uint32_t src, uint32_t dest, uint32_t size, uint32_t wqeId)
{
    return CreateWqe(src, dest, size, wqeId, TaOpcode::TA_OPCODE_WRITE);
}

void UbFunction::PushWqeToJetty(Ptr<UbWqe> wqe, uint32_t jettyNum)
{
    NS_LOG_DEBUG(this);
    Ptr<UbJetty> ubJetty = GetJetty(jettyNum);
    if (ubJetty == nullptr) {
        NS_LOG_WARN("Get jetty failed");
        return;
    }
    ubJetty->SetNodeId(m_nodeId);
    // 将 WQE 添加到 Jetty
    ubJetty->PushWqe(wqe);
    GetTransaction()->AddWqe(jettyNum, wqe);
    GetTransaction()->TriggerScheduleWqeSegment(jettyNum);
}

void UbFunction::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_ldstApi = nullptr;
    for (auto i = m_jettyVector.begin(); i != m_jettyVector.end(); i++) {
        *i = nullptr;
    }
    m_jettyVector.clear();
    m_numToJetty.clear();

    Object::DoDispose();
}

NS_OBJECT_ENSURE_REGISTERED(UbJetty);

TypeId UbJetty::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbJetty")
                        .SetParent<Object>()
                        .SetGroupName("UnifiedBus")
                        .AddAttribute("JettyOooAckThreshold",
                                      "Maximum out-of-order ACK window tracked per Jetty (TASSN bitmap size).",
                                      UintegerValue(2048),
                                      MakeUintegerAccessor(&UbJetty::m_oooAckThreshold),
                                      MakeUintegerChecker<uint32_t>())
                        .AddAttribute("UbJettyInflightMax",
                                      "Maximum number of in-flight WQE segments per Jetty before back-pressure.",
                                      UintegerValue(512),
                                      MakeUintegerAccessor(&UbJetty::m_inflightMax),
                                      MakeUintegerChecker<uint32_t>())
                        .AddAttribute("TaSegmentBytes",
                                      "TA WQE segment size in bytes.",
                                      UintegerValue(UB_WQE_TA_SEGMENT_BYTE),
                                      MakeUintegerAccessor(&UbJetty::m_taSegmentBytes),
                                      MakeUintegerChecker<uint32_t>());
    return tid;
}

UbJetty::UbJetty()
{
}

void UbJetty::Init()
{
    ResetSsnAckBitset(m_oooAckThreshold);
}

void UbJetty::SetClientCallback(Callback<void, uint32_t, uint32_t> cb)
{
    FinishCallback = cb;
}

void UbJetty::SetCompletionObserver(Callback<void, uint32_t, uint32_t> cb)
{
    m_completionObserver = cb;
}

bool
UbJetty::CanAcceptWqe(Ptr<UbWqe> wqe) const
{
    if (wqe == nullptr)
    {
        return false;
    }
    if (wqe->GetSrc() != m_src)
    {
        return false;
    }
    if (HasBoundDest() && wqe->GetDest() != m_dest)
    {
        return false;
    }
    return true;
}

void UbJetty::SetTaSegmentBytes(uint32_t bytes)
{
    NS_ABORT_MSG_IF(bytes == 0 || bytes > UB_WQE_TA_SEGMENT_BYTE,
                    "TA segment bytes must be in [1, UB_WQE_TA_SEGMENT_BYTE]");
    m_taSegmentBytes = bytes;
}

uint32_t UbJetty::GetTaSegmentBytes() const
{
    return m_taSegmentBytes;
}


// ============================================================================
// UbJetty 实现
// ============================================================================
Ptr<UbTransaction> UbJetty::GetTransaction()
{
    return NodeList::GetNode(m_nodeId)->GetObject<UbController>()->GetUbTransaction();
}

Ptr<UbWqeSegment> UbJetty::GetNextWqeSegment()
{
    NS_LOG_FUNCTION(this);

    if (IsLimited()) {
        NS_LOG_WARN("Inflight reach limit");
        return nullptr;
    }

    if (m_wqeVector.empty()) {
        NS_LOG_DEBUG("No WQE available to send");
        return nullptr;
    }

    // 获取第一个未完成的 WQE
    Ptr<UbWqe> currentWqe = nullptr;
    for (auto it = m_wqeVector.begin(); it != m_wqeVector.end(); ++it) {
        if (*it && !(*it)->IsSentCompleted()) {
            currentWqe = *it;
            // WQE初始发送状态为False，当TA根据事务序判断当前WQE可以发送后，修改为True，即可发送。
            if (currentWqe->CanSend()) {
                break;
            } else {
                // 调用TA，判断currentWqe是否允许发送
                auto ubTa = GetTransaction();
                if (ubTa->IsOrderedByInitiator(m_jettyNum, currentWqe)) {
                    currentWqe->SetCanSend(true);
                    break;
                }
            }
        }
    }

    if (!currentWqe) {
        NS_LOG_DEBUG("No unfinished WQE available to send");
        return nullptr;
    }

    if (!currentWqe->CanSend()) {
        NS_LOG_DEBUG("No unfinished WQE available to send");
        return nullptr;
    }

    uint64_t segmentSize = currentWqe->GetBytesLeft();
    if (segmentSize > m_taSegmentBytes) {
        segmentSize = m_taSegmentBytes;
    }

    Ptr<UbWqeSegment> segment = GenWqeSegment(currentWqe, segmentSize);
    segment->SetTaskId(currentWqe->GetWqeId());
    segment->SetWqeSize(currentWqe->GetSize());
    currentWqe->UpdateSentBytes(segmentSize);
    IncreaseTaSsnSndNxt(); // 更新下一个待发送的分段序号

    return segment;
}

Ptr<UbWqeSegment> UbJetty::GenWqeSegment(Ptr<UbWqe> wqe, uint32_t segmentSize)
{
    NS_LOG_FUNCTION(this << segmentSize);

    if (!wqe) {
        NS_LOG_ERROR("WQE is null");
        return nullptr;
    }

    if (segmentSize == 0) {
        NS_LOG_ERROR("Segment size is zero");
        return nullptr;
    }

    // 创建新的 WQE 分段
    Ptr<UbWqeSegment> segment = CreateObject<UbWqeSegment>();

    // 设置任务描述信息（从WQE复制）
    segment->SetSrc(wqe->GetSrc());
    segment->SetDest(wqe->GetDest());
    if (wqe->HasSrcEntityId())
    {
        segment->SetSrcEntityId(wqe->GetSrcEntityId());
    }
    if (wqe->HasDstEntityId())
    {
        segment->SetDstEntityId(wqe->GetDstEntityId());
    }
    segment->SetSport(wqe->GetSport());
    segment->SetDport(wqe->GetDport());
    segment->SetType(wqe->GetType());
    segment->SetSize(segmentSize);
    segment->SetPriority(wqe->GetPriority());
    // 设置TA层信息
    segment->SetOrderType(wqe->GetOrderType());
    // 设置网络层信息（从WQE复制）
    segment->SetSip(wqe->GetSip());
    segment->SetDip(wqe->GetDip());

    // 设置TA层信息
    segment->SetJettyNum(wqe->GetJettyNum());
    segment->SetTaMsn(wqe->GetTaMsn());
    segment->SetTaSsn(m_taSsnSndNxt); // 使用当前的分段序号
    segment->SetSegmentKind(wqe->GetSegmentKind());
    segment->SetOriginJettyNum(wqe->GetOriginJettyNum());
    segment->SetRequestTassn(wqe->GetRequestTassn());
    segment->SetRequestOpcode(wqe->GetRequestOpcode());
    segment->SetResponseBytes(wqe->GetResponseBytes());
    segment->SetRemoteAddress(wqe->GetRemoteAddress());
    segment->SetNeedsTransactionResponse(wqe->NeedsTransactionResponse());
    segment->SetResLenBytes(segmentSize);
    segment->SetPayloadBytes(wqe->GetType() == TaOpcode::TA_OPCODE_READ ? 0 : segmentSize);
    segment->SetCarrierBytes(wqe->GetType() == TaOpcode::TA_OPCODE_READ ? 1 : segmentSize);

    // TP layer information will be set during subsequent TP layer scheduling

    NS_LOG_DEBUG("Generated WQE segment: MSN=" << std::to_string(wqe->GetTaMsn())
                                              << ", SSN=" << std::to_string(m_taSsnSndNxt)
                                              << ", size=" << std::to_string(segmentSize)
                                              << ", src=" << std::to_string(wqe->GetSrc())
                                              << ", dest=" << std::to_string(wqe->GetDest()));

    return segment;
}

void UbJetty::PushWqe(Ptr<UbWqe> ubWqe)
{
    NS_ABORT_MSG_IF(!CanAcceptWqe(ubWqe),
                    "WQE is not compatible with Jetty source/destination binding");
    ubWqe->SetJettyNum(m_jettyNum);
    ubWqe->SetTaMsn(m_taMsnCnt);
    ubWqe->SetTaSsnStart(m_taSsnCnt);
    uint64_t ssnSize = (ubWqe->GetSize() + m_taSegmentBytes - 1) / m_taSegmentBytes;
    ubWqe->SetTaSsnSize(ssnSize);
    // Assign request identity only after Jetty binding is known.
    ubWqe->SetSegmentKind(UbTransactionSegmentKind::REQUEST);
    ubWqe->SetOriginJettyNum(m_jettyNum);
    ubWqe->SetRequestTassn(ubWqe->GetTaSsnStart());
    ubWqe->SetRequestOpcode(ubWqe->GetType());
    ubWqe->SetResponseBytes(ubWqe->GetType() == TaOpcode::TA_OPCODE_READ ? ubWqe->GetSize() : 0);
    ubWqe->SetRemoteAddress(0);
    ubWqe->SetNeedsTransactionResponse(ubWqe->GetType() == TaOpcode::TA_OPCODE_WRITE ||
                                       ubWqe->GetType() == TaOpcode::TA_OPCODE_READ);
    ubWqe->SetResLenBytes(ubWqe->GetSize());
    ubWqe->SetPayloadBytes(ubWqe->GetType() == TaOpcode::TA_OPCODE_READ ? 0 : ubWqe->GetSize());
    ubWqe->SetCarrierBytes(ubWqe->GetType() == TaOpcode::TA_OPCODE_READ ? ssnSize
                                                                         : ubWqe->GetSize());

    // 更新 Jetty 的 MSN 和 SSN 计数器
    m_taMsnCnt++;
    m_taSsnCnt += ssnSize;

    // 加入队列
    m_wqeVector.push_back(ubWqe);
    NS_LOG_INFO("WQE Starts, jettyNum:{" << std::to_string(m_jettyNum)
                <<  "} taskId:{" << std::to_string(ubWqe->GetWqeId()) << "}");
}

bool UbJetty::IsValid()
{
    return true;
}

bool UbJetty::IsReadyToSend()
{
    return true;
}

/*
 * TP调用这个方法，传入taSsnFin。
 */
bool UbJetty::ProcessWqeSegmentComplete(uint32_t taSsnAck)
{
    NS_LOG_DEBUG(this << taSsnAck);
    // 检查 SSN 是否在有效范围内
    if (taSsnAck < m_taSsnSndUna) {
        NS_LOG_WARN("Received ACK for already processed SSN " << std::to_string(taSsnAck)
                                                              << ", current m_taSsnSndUna is "
                                                              << std::to_string(m_taSsnSndUna));
        return false;
    }

    if (taSsnAck >= m_taSsnSndNxt) {
        NS_LOG_WARN("Received ACK for future SSN " << std::to_string(taSsnAck)
                                                   << ", current m_taSsnSndNxt is "
                                                   << std::to_string(m_taSsnSndNxt));
        return false;
    }

    if (IsLimited()) {
        GetTransaction()->TriggerTpTransmit(m_jettyNum);
    }

    // 计算在 bitset 中的相对位置
    uint32_t bitIndex = taSsnAck - m_taSsnSndUna;

    // 检查是否超出 bitset 范围
    if (bitIndex >= m_ssnAckWindow.GetWindowSize()) {
        NS_LOG_ERROR("SSN " << std::to_string(taSsnAck)
                            << " exceeds bitset capacity, bitIndex=" << std::to_string(bitIndex));
        return false;
    }

    // 设置对应的 bit
    const bool marked = m_ssnAckWindow.Mark(taSsnAck);
    NS_ASSERT_MSG(marked, "Valid SSN ACK should always fit inside ACK window");

    NS_LOG_DEBUG("Set ACK bit for SSN " << std::to_string(taSsnAck) << " at bit index "
                                       << std::to_string(bitIndex));

    // 从最小未确认的 SSN 开始，连续更新已确认的分段
    uint32_t oldSndUna = m_taSsnSndUna;
    while (m_taSsnSndUna < m_taSsnSndNxt) {
        uint32_t currentBitIndex = m_taSsnSndUna - oldSndUna;
        if (currentBitIndex < m_ssnAckWindow.GetWindowSize() &&
            m_ssnAckWindow.Contains(m_taSsnSndUna)) {
            m_taSsnSndUna++;
        } else {
            break; // 遇到未确认的分段，停止
        }
    }

    // 如果 m_taSsnSndUna 有更新，需要清理 bitset
    if (m_taSsnSndUna > oldSndUna) {
        NS_LOG_DEBUG("Updated m_taSsnSndUna from " << std::to_string(oldSndUna) << " to "
                                                  << std::to_string(m_taSsnSndUna));

        // 手动右移 bitset
        uint32_t shiftCount = m_taSsnSndUna - oldSndUna;
        RightShiftBitset(shiftCount);

        // 检查并移除已完成的 WQE
        CheckAndRemoveCompletedWqe();
    }
    return true;
}

void UbJetty::RightShiftBitset(uint32_t shiftCount)
{
    (void)shiftCount;
    m_ssnAckWindow.AdvanceContiguous();
}

void UbJetty::CheckAndRemoveCompletedWqe()
{
    NS_LOG_DEBUG(this);
    // 检查并移除已完成的WQE
    for (auto it = m_wqeVector.begin(); it != m_wqeVector.end();) {
        Ptr<UbWqe> wqe = *it;
        uint32_t wqeId = wqe->GetWqeId();
        if (wqe && IsWqeCompleted(wqe)) {
            NS_LOG_INFO("WQE Finishes, jettyNum: {" << m_jettyNum  << "} taskId:{ " << std::to_string(wqeId) <<"}");
            auto ubTa = GetTransaction();
            ubTa->WqeFinish(m_jettyNum, *it);
            // 从vector中移除已完成的WQE
            it = m_wqeVector.erase(it);
            if (!m_completionObserver.IsNull()) {
                m_completionObserver(wqeId, m_jettyNum);
            }
            if (!FinishCallback.IsNull()) {
                FinishCallback(wqeId, m_jettyNum); // 调用应用层的回调
            }
            // trigger tp
            ubTa->TriggerTpTransmit(m_jettyNum);
        } else {
            ++it;
        }
    }

    // 检查Jetty是否还有未完成的工作
    if (m_wqeVector.empty()) {
        NS_LOG_DEBUG("All WQEs in Jetty " << std::to_string(m_jettyNum) << " are completed");
    }
}

bool UbJetty::IsWqeCompleted(Ptr<UbWqe> wqe)
{
    NS_LOG_DEBUG(this);
    if (!wqe) {
        return false;
    }

    // 检查WQE的所有分段是否都已确认
    uint32_t wqeStartSsn = wqe->GetTaSsnStart();
    uint32_t wqeSsnSize = wqe->GetTaSsnSize();
    uint32_t wqeEndSsn = wqeStartSsn + wqeSsnSize - 1;

    // 如果WQE的最后一个分段已经被确认，则WQE完成
    return (wqeEndSsn < m_taSsnSndUna);
}

void UbJetty::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_wqeVector.clear();
    m_ssnAckWindow.Resize(0);
    Object::DoDispose();
}


} // namespace ns3
