// SPDX-License-Identifier: GPL-2.0-only
#include "ub-queue-manager.h"
#include "ub-port.h"
#include "protocol/ub-header.h"
#include "ns3/node.h"
#include "ns3/uinteger.h"
#include <algorithm>

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(UbIngressQueue);
NS_OBJECT_ENSURE_REGISTERED(UbQueueManager);
NS_LOG_COMPONENT_DEFINE("UbQueueManager");

/*-----------------------------------------UbIngressQueue----------------------------------------------*/

UbIngressQueue::UbIngressQueue()
{
}

UbIngressQueue::~UbIngressQueue()
{
}

TypeId UbIngressQueue::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbIngressQueue")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbIngressQueue>();
    return tid;
}

bool UbIngressQueue::IsEmpty()
{
    return true;
}
Ptr<Packet> UbIngressQueue::GetNextPacket()
{
    return nullptr;
}
uint32_t UbIngressQueue::GetNextPacketSize()
{
    return 0;
}

bool UbIngressQueue::IsControlFrame()
{
    return m_ingressPriority == 0 && m_inPortId == m_outPortId;
}

bool UbIngressQueue::IsForwardedDataPacket()
{
    return m_inPortId != m_outPortId;
}

bool UbIngressQueue::IsGeneratedDataPacket()
{
    return m_ingressPriority != 0 && m_inPortId == m_outPortId;
}

/*----------------------------------------- UbPacketQueue ----------------------------------------------*/
bool UbPacketQueue::IsEmpty()
{
    return m_packetCount == 0;
}

UbPacketQueue::UbPacketQueue()
{
}

UbPacketQueue::~UbPacketQueue()
{
}

Ptr<Packet> UbPacketQueue::GetNextPacket()
{
    auto p = Front();
    Pop();
    return p;
}

Ptr<Packet>
UbPacketQueue::Front()
{
    NS_ASSERT_MSG(m_packetCount > 0, "Cannot read the front of an empty UbPacketQueue");
    return m_frontPacket;
}

TypeId UbPacketQueue::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbPacketQueue")
        .SetGroupName("UnifiedBus")
        .SetParent<UbIngressQueue>()
        .AddConstructor<UbPacketQueue>();
    return tid;
}

IngressQueueType UbPacketQueue::GetIngressQueueType()
{
    return m_ingressQueueType;
}

void
UbPacketQueue::Push(Ptr<Packet> p)
{
    if (m_packetCount == 0) {
        m_headArrivalTime = Simulator::Now();
        m_frontPacket = p;
    } else {
        m_overflowQueue.push(p);
    }
    ++m_packetCount;
}

void
UbPacketQueue::Pop()
{
    NS_ASSERT_MSG(m_packetCount > 0, "Cannot pop from an empty UbPacketQueue");

    if (m_packetCount == 1) {
        m_frontPacket = nullptr;
        m_packetCount = 0;
        return;
    }

    m_frontPacket = m_overflowQueue.front();
    m_overflowQueue.pop();
    --m_packetCount;
    m_headArrivalTime = Simulator::Now();
}

uint32_t UbPacketQueue::GetNextPacketSize()
{
    if (IsControlFrame()) { // crd报文等控制报文
        NS_LOG_DEBUG("[UbPacketQueue GetNextPacketSize] is ctrl pkt");
        UbDatalinkControlCreditHeader  DatalinkControlCreditHeader;
        uint32_t UbDataLinkCtrlSize = DatalinkControlCreditHeader.GetSerializedSize();
        return UbDataLinkCtrlSize;
    }
    Ptr<Packet> p = Front();
    uint32_t nextPktSize = p->GetSize();
    NS_LOG_DEBUG("[UbPacketQueue GetNextPacketSize] is forward pkt, nextPktSize:" << nextPktSize);

    return nextPktSize;
}

/*-----------------------------------------UbQueueManager----------------------------------------------*/
UbQueueManager::UbQueueManager()
{
}

TypeId UbQueueManager::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbQueueManager")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbQueueManager>()
        .AddTraceSource("OutPortBufferBytes",
        "Current total out-port VOQ occupancy in bytes after an update.",
        MakeTraceSourceAccessor(&UbQueueManager::m_traceOutPortBufferBytes),
        "ns3::UbQueueManager::OutPortBufferBytes")
        .AddTraceSource("IngressQueueOccupancyBytes",
        "Current ingress queue occupancy in bytes after an update.",
        MakeTraceSourceAccessor(&UbQueueManager::m_traceIngressQueueOccupancyBytes),
        "ns3::TracedCallback::Uint32Uint32Uint64")
        .AddAttribute("ReservePerQueueBytes",
        "Per-queue reserve in bytes. Queue means one ingress (port, VL) queue.",
        UintegerValue(DEFAULT_RESERVE_PER_QUEUE_BYTES),
        MakeUintegerAccessor(&UbQueueManager::m_reservePerQueueBytes),
        MakeUintegerChecker<uint32_t>())
        .AddAttribute("SharedPoolBytes",
        "Global shared ingress pool in bytes. Bytes beyond reserve consume this pool before headroom.",
        UintegerValue(DEFAULT_SHARED_POOL_BYTES),
        MakeUintegerAccessor(&UbQueueManager::m_sharedPoolBytes),
        MakeUintegerChecker<uint64_t>())
        .AddAttribute("AlphaShift",
        "Dynamic threshold shift for the global shared pool. "
        "xoff = (SharedPoolBytes - GlobalSharedUsedBytes) >> alpha. 0 = no per-queue fairness limit.",
        UintegerValue(DEFAULT_ALPHA_SHIFT),
        MakeUintegerAccessor(&UbQueueManager::m_alphaShift),
        MakeUintegerChecker<uint32_t>())
        .AddAttribute("HeadroomPerPortBytes",
        "Per-port headroom in bytes for absorbing in-flight packets during PFC pause.",
        UintegerValue(DEFAULT_HEADROOM_PER_PORT_BYTES),
        MakeUintegerAccessor(&UbQueueManager::m_headroomPerPortBytes),
        MakeUintegerChecker<uint32_t>())
        // TODO: expose dynamic-PFC config from UbPfc (or another flow-control config owner)
        // instead of QueueManager. QueueManager keeps these knobs for now because it owns the
        // shared-pool/headroom accounting and computes dynamic xon/xoff thresholds.
        .AddAttribute("DynamicPfcResumeGapBytes",
        "Dynamic PFC resume gap in bytes. xon = max(xoff - gap, 0). Larger gaps delay resume and reduce pause/resume oscillation.",
        UintegerValue(DEFAULT_DYNAMIC_PFC_RESUME_GAP_BYTES),
        MakeUintegerAccessor(&UbQueueManager::m_dynamicPfcResumeGapBytes),
        MakeUintegerChecker<uint32_t>())
        .AddAttribute("PaperDynamicPfcBeta",
        "Paper-style dynamic PFC beta for the DCQCN paper \"Congestion Control for Large-Scale RDMA Deployments\". "
        "Threshold = beta * max(SharedPoolBytes - globalOccupancy, 0) / priorities.",
        UintegerValue(DEFAULT_PAPER_DYNAMIC_PFC_BETA),
        MakeUintegerAccessor(&UbQueueManager::m_paperDynamicPfcBeta),
        MakeUintegerChecker<uint32_t>());
    return tid;
}

void UbQueueManager::Init()
{
    NS_ABORT_MSG_IF(m_alphaShift >= 64, "UbQueueManager AlphaShift must be < 64");

    m_inPortBuffer.assign(m_portsNum, std::vector<uint64_t>(m_vlNum, 0));
    m_outPortBuffer.assign(m_portsNum, std::vector<uint64_t>(m_vlNum, 0));
    m_inPortProcessingBytes.assign(m_portsNum, std::vector<uint64_t>(m_vlNum, 0));
    m_outPortProcessingBytes.assign(m_portsNum, std::vector<uint64_t>(m_vlNum, 0));
    m_totalOutPortVoqBufferedBytes.assign(m_portsNum, 0);
    m_hdrmBytes.assign(m_portsNum, std::vector<uint64_t>(m_vlNum, 0));
    m_ingressControlBytes.assign(m_portsNum, std::vector<uint64_t>(m_vlNum, 0));
    m_outPortControlBytes.assign(m_portsNum, std::vector<uint64_t>(m_vlNum, 0));

    m_sharedUsedBytes = 0;
    m_totalVoqBufferedBytes = 0;
    m_totalEgressBufferedBytes = 0;
    m_totalProcessingBytes = 0;
    m_totalHeadroomBytes = static_cast<uint64_t>(m_headroomPerPortBytes) * m_portsNum;
    m_totalReservedBytes = static_cast<uint64_t>(m_reservePerQueueBytes) * m_vlNum * m_portsNum;

    if (m_sharedPoolBytes == 0) {
        NS_LOG_DEBUG("SharedPoolBytes is 0; ingress admission degenerates to reserve + optional headroom");
    }

    if (m_headroomPerPortBytes == 0) {
        NS_LOG_DEBUG("HeadroomPerPortBytes is 0; packets beyond reserve/shared limits will be dropped immediately");
    }

    uint64_t maxXoffThresh = GetDynamicPauseThresholdBytes();

    if (m_sharedPoolBytes > 0 && m_dynamicPfcResumeGapBytes > maxXoffThresh) {
        NS_LOG_WARN("DynamicPfcResumeGapBytes (" << m_dynamicPfcResumeGapBytes
                    << ") > max xoff threshold (" << maxXoffThresh
                    << " = sharedPool " << m_sharedPoolBytes << " >> " << m_alphaShift
                    << "), PFC xon watermark is below zero — PFC will only resume when "
                    "sharedUsed drops to 0. Consider reducing DynamicPfcResumeGapBytes or AlphaShift.");
    }

    NS_LOG_DEBUG("UbQueueManager Init: reservePerQueue=" << m_reservePerQueueBytes
                 << " sharedPoolGlobal=" << m_sharedPoolBytes
                 << " headroomPerPort=" << m_headroomPerPortBytes
                 << " totalReserved=" << m_totalReservedBytes
                 << " totalHeadroom=" << m_totalHeadroomBytes
                 << " totalIngressBudget=" << (m_totalReservedBytes + m_totalHeadroomBytes + m_sharedPoolBytes)
                 << " alphaShift=" << m_alphaShift
                 << " dynamicPfcResumeGapBytes=" << m_dynamicPfcResumeGapBytes);
}

// ========== VOQ Dual-View Operations ==========

bool UbQueueManager::IsLocallyGeneratedControlFrame(uint32_t inPort,
                                                    uint32_t outPort,
                                                    uint32_t priority) const
{
    // Unified-bus reserves (inPort == outPort, priority == 0) for locally generated control
    // frames. This is a simulator tradeoff, not a UB protocol rule: it lets the switch/VOQ hot
    // path classify control traffic without repeatedly parsing packet headers after enqueue.
    return priority == 0 && inPort == outPort;
}

bool UbQueueManager::CheckVoqSpace(uint32_t inPort, uint32_t outPort, 
                                    uint32_t priority, uint32_t pSize)
{
    if (IsLocallyGeneratedControlFrame(inPort, outPort, priority)) {
        return true;
    }

    bool inPortOk = CheckInPortSpace(inPort, priority, pSize);
    bool outPortOk = CheckOutPortSpace(outPort, priority, pSize);
    return inPortOk && outPortOk;
}

bool UbQueueManager::CheckInPortSpace(uint32_t inPort, uint32_t priority, uint32_t pSize)
{
    return CheckIngressAdmission(inPort, priority, pSize);
}

bool UbQueueManager::CheckOutPortSpace(uint32_t outPort, uint32_t priority, uint32_t pSize)
{
    NS_LOG_DEBUG("CheckOutPortSpace: outPort=" << outPort 
                 << " pri=" << priority
                 << " currentUsed=" << m_outPortBuffer[outPort][priority]
                 << " newUsage=" << m_outPortBuffer[outPort][priority] + pSize);
    return true;  // OutPort视图无物理限制，不用于丢包决策
}

void UbQueueManager::PushToVoq(uint32_t inPort, uint32_t outPort,
                                uint32_t priority, uint32_t pSize)
{
    if (IsLocallyGeneratedControlFrame(inPort, outPort, priority)) {
        m_ingressControlBytes[inPort][priority] += pSize;
        m_outPortControlBytes[outPort][priority] += pSize;
        m_totalVoqBufferedBytes += pSize;
        m_totalOutPortVoqBufferedBytes[outPort] += pSize;
        NS_LOG_DEBUG("PushToVoq recorded local control frame outside data-plane admission: inPort="
                     << inPort << " outPort=" << outPort << " pri=" << priority
                     << " size=" << pSize);
        return;
    }

    UpdateIngressAdmission(inPort, priority, pSize);
    m_outPortBuffer[outPort][priority] += pSize;
    m_totalVoqBufferedBytes += pSize;
    m_totalOutPortVoqBufferedBytes[outPort] += pSize;
    m_traceIngressQueueOccupancyBytes(inPort, priority, GetQueueIngressTotalBytes(inPort, priority));
    m_traceOutPortBufferBytes(outPort, GetTotalOutPortBufferUsed(outPort));

    NS_LOG_DEBUG("PushToVoq: inPort=" << inPort << " outPort=" << outPort
                 << " pri=" << priority << " size=" << pSize
                 << " | inPortBuf=" << m_inPortBuffer[inPort][priority]
                 << " outPortBuf=" << m_outPortBuffer[outPort][priority]);
}

void UbQueueManager::PopFromVoq(uint32_t inPort, uint32_t outPort,
                                 uint32_t priority, uint32_t pSize)
{
    if (IsLocallyGeneratedControlFrame(inPort, outPort, priority)) {
        NS_ASSERT_MSG(m_ingressControlBytes[inPort][priority] >= pSize,
                      "Ingress control accounting underflow");
        NS_ASSERT_MSG(m_outPortControlBytes[outPort][priority] >= pSize,
                      "Out-port control accounting underflow");
        m_ingressControlBytes[inPort][priority] -= pSize;
        m_outPortControlBytes[outPort][priority] -= pSize;
        NS_ASSERT_MSG(m_totalVoqBufferedBytes >= pSize, "VOQ total accounting underflow");
        m_totalVoqBufferedBytes -= pSize;
        NS_ASSERT_MSG(m_totalOutPortVoqBufferedBytes[outPort] >= pSize,
                      "Out-port VOQ total accounting underflow");
        m_totalOutPortVoqBufferedBytes[outPort] -= pSize;
        NS_LOG_DEBUG("PopFromVoq drained local control-frame accounting: inPort="
                     << inPort << " outPort=" << outPort << " pri=" << priority
                     << " size=" << pSize);
        return;
    }

    RemoveFromIngressAdmission(inPort, priority, pSize);
    NS_ASSERT_MSG(m_totalVoqBufferedBytes >= pSize, "VOQ total accounting underflow");
    m_totalVoqBufferedBytes -= pSize;
    NS_ASSERT_MSG(m_totalOutPortVoqBufferedBytes[outPort] >= pSize,
                  "Out-port VOQ total accounting underflow");
    m_totalOutPortVoqBufferedBytes[outPort] -= pSize;
    m_outPortBuffer[outPort][priority] -= pSize;
    m_traceIngressQueueOccupancyBytes(inPort, priority, GetQueueIngressTotalBytes(inPort, priority));
    m_traceOutPortBufferBytes(outPort, GetTotalOutPortBufferUsed(outPort));

    NS_LOG_DEBUG("PopFromVoq: inPort=" << inPort << " outPort=" << outPort
                 << " pri=" << priority << " size=" << pSize
                 << " | inPortBuf=" << m_inPortBuffer[inPort][priority]
                 << " outPortBuf=" << m_outPortBuffer[outPort][priority]);
}

void
UbQueueManager::PushToInPortProcessing(uint32_t inPort,
                                       uint32_t outPort,
                                       uint32_t priority,
                                       uint32_t pSize)
{
    NS_ASSERT_MSG(!IsLocallyGeneratedControlFrame(inPort, outPort, priority),
                  "In-port processing is only for forwarded data packets");

    UpdateIngressAdmission(inPort, priority, pSize);
    m_inPortProcessingBytes[inPort][priority] += pSize;
    m_outPortProcessingBytes[outPort][priority] += pSize;
    m_totalProcessingBytes += pSize;
    m_traceIngressQueueOccupancyBytes(inPort, priority, GetQueueIngressTotalBytes(inPort, priority));
    m_traceOutPortBufferBytes(outPort, GetTotalOutPortBufferUsed(outPort));
}

void
UbQueueManager::MoveInPortProcessingToVoq(uint32_t inPort,
                                          uint32_t outPort,
                                          uint32_t priority,
                                          uint32_t pSize)
{
    NS_ASSERT_MSG(!IsLocallyGeneratedControlFrame(inPort, outPort, priority),
                  "In-port processing is only for forwarded data packets");
    NS_ASSERT_MSG(m_inPortProcessingBytes[inPort][priority] >= pSize,
                  "In-port processing accounting underflow");
    NS_ASSERT_MSG(m_outPortProcessingBytes[outPort][priority] >= pSize,
                  "Out-port processing accounting underflow");
    NS_ASSERT_MSG(m_totalProcessingBytes >= pSize,
                  "Total processing accounting underflow");

    m_inPortProcessingBytes[inPort][priority] -= pSize;
    m_outPortProcessingBytes[outPort][priority] -= pSize;
    m_totalProcessingBytes -= pSize;
    m_outPortBuffer[outPort][priority] += pSize;
    m_totalVoqBufferedBytes += pSize;
    m_totalOutPortVoqBufferedBytes[outPort] += pSize;
    m_traceIngressQueueOccupancyBytes(inPort, priority, GetQueueIngressTotalBytes(inPort, priority));
    m_traceOutPortBufferBytes(outPort, GetTotalOutPortBufferUsed(outPort));
}

// ========== 查询接口：InPort视图 ==========

uint64_t UbQueueManager::GetQueueIngressNonHeadroomBytes(uint32_t inPort, uint32_t priority) const
{
    return m_inPortBuffer[inPort][priority];
}

uint64_t UbQueueManager::GetPortIngressNonHeadroomBytes(uint32_t inPort) const
{
    uint64_t sum = 0;
    for (uint32_t i = 0; i < m_vlNum; i++) {
        sum += m_inPortBuffer[inPort][i];
    }
    return sum;
}

// ========== 查询接口：OutPort视图 ==========

uint64_t UbQueueManager::GetOutPortBufferUsed(uint32_t outPort, uint32_t priority)
{
    return m_outPortBuffer[outPort][priority] + m_outPortProcessingBytes[outPort][priority];
}

uint64_t UbQueueManager::GetTotalOutPortBufferUsed(uint32_t outPort) const
{
    uint64_t processingBytes = 0;
    for (uint32_t priority = 0; priority < m_vlNum; ++priority) {
        processingBytes += m_outPortProcessingBytes[outPort][priority];
    }
    return m_totalOutPortVoqBufferedBytes[outPort] + processingBytes;
}

void UbQueueManager::SetReservePerQueueBytes(uint32_t size)
{
    m_reservePerQueueBytes = size;
}

// ========== Three-Tier Buffer ==========

bool UbQueueManager::CheckIngressAdmission(uint32_t inPort, uint32_t priority, uint32_t pSize)
{
    const auto portOcc = GetIngressPortOccupancy(inPort);
    const uint64_t portHdrmUsed = portOcc.headroom_bytes;

    if (m_hdrmBytes[inPort][priority] > 0) {
        if (portHdrmUsed + pSize > m_headroomPerPortBytes) {
            NS_LOG_DEBUG("CheckIngressAdmission DROP (sticky hdrm full): inPort=" << inPort
                         << " pri=" << priority
                         << " portHdrmUsed=" << portHdrmUsed
                         << " hdrmLimit=" << m_headroomPerPortBytes);
            return false;
        }
        return true;
    }

    uint64_t newBytes = m_inPortBuffer[inPort][priority] + pSize;
    if (newBytes <= m_reservePerQueueBytes) {
        return true;
    }

    uint64_t xoffThresh = GetIngressAdmissionThresholdBytes(inPort, priority);
    uint64_t sharedAfter = newBytes - m_reservePerQueueBytes;
    bool hdrmFull = (portHdrmUsed + pSize > m_headroomPerPortBytes);
    bool willUseHdrm = (sharedAfter > xoffThresh);

    if (hdrmFull && willUseHdrm) {
        NS_LOG_DEBUG("CheckIngressAdmission DROP: inPort=" << inPort
                     << " pri=" << priority
                     << " portHdrmUsed=" << portHdrmUsed
                     << " hdrmLimit=" << m_headroomPerPortBytes
                     << " sharedAfter=" << sharedAfter
                     << " xoffThresh=" << xoffThresh);
        return false;
    }
    return true;
}

void UbQueueManager::UpdateIngressAdmission(uint32_t inPort, uint32_t priority, uint32_t pSize)
{
    if (m_hdrmBytes[inPort][priority] > 0) {
        m_hdrmBytes[inPort][priority] += pSize;
        return;
    }

    uint64_t newBytes = m_inPortBuffer[inPort][priority] + pSize;

    if (newBytes <= m_reservePerQueueBytes) {
        m_inPortBuffer[inPort][priority] += pSize;
    } else {
        uint64_t thresh = GetIngressAdmissionThresholdBytes(inPort, priority);
        uint64_t sharedAfter = newBytes - m_reservePerQueueBytes;
        if (sharedAfter > thresh) {
            NS_ASSERT_MSG(m_headroomPerPortBytes > 0,
                          "Headroom admission should only trigger when HeadroomPerPortBytes is positive");
            m_hdrmBytes[inPort][priority] += pSize;
        } else {
            uint64_t prevShared = GetQueueIngressSharedBytes(inPort, priority);
            m_inPortBuffer[inPort][priority] = newBytes;
            m_sharedUsedBytes += (sharedAfter - prevShared);
        }
    }
}

void UbQueueManager::RemoveFromIngressAdmission(uint32_t inPort, uint32_t priority, uint32_t pSize)
{
    uint64_t fromHdrm = std::min(m_hdrmBytes[inPort][priority], static_cast<uint64_t>(pSize));
    uint64_t fromIngress = pSize - fromHdrm;

    uint64_t prevShared = GetQueueIngressSharedBytes(inPort, priority);

    m_hdrmBytes[inPort][priority] -= fromHdrm;
    m_inPortBuffer[inPort][priority] -= fromIngress;

    uint64_t curShared = GetQueueIngressSharedBytes(inPort, priority);
    uint64_t freedShared = prevShared > curShared ? prevShared - curShared : 0;
    m_sharedUsedBytes -= std::min(freedShared, m_sharedUsedBytes);
}


uint64_t UbQueueManager::GetDynamicPauseThresholdBytes() const
{
    uint64_t remaining = m_sharedPoolBytes > m_sharedUsedBytes ? m_sharedPoolBytes - m_sharedUsedBytes : 0;
    return remaining >> m_alphaShift;
}

uint64_t UbQueueManager::GetDynamicResumeThresholdBytes() const
{
    uint64_t xoff = GetDynamicPauseThresholdBytes();
    return xoff > m_dynamicPfcResumeGapBytes ? xoff - m_dynamicPfcResumeGapBytes : 0;
}

uint64_t UbQueueManager::GetPaperPauseThresholdBytes(uint64_t totalBufferedBytes) const
{
    uint64_t remaining = m_sharedPoolBytes > totalBufferedBytes ? m_sharedPoolBytes - totalBufferedBytes : 0;
    uint32_t priorities = DEFAULT_PAPER_DYNAMIC_PFC_PRIORITY_COUNT;
    return (static_cast<uint64_t>(m_paperDynamicPfcBeta) * remaining) / priorities;
}

uint64_t UbQueueManager::GetPaperResumeThresholdBytes(uint64_t totalBufferedBytes) const
{
    uint64_t xoff = GetPaperPauseThresholdBytes(totalBufferedBytes);
    constexpr uint64_t kPaperResumeGapBytes = 2ull * UB_MTU_BYTE;
    return xoff > kPaperResumeGapBytes ? xoff - kPaperResumeGapBytes : 0;
}

uint64_t UbQueueManager::GetSwitchTotalBufferedBytes() const
{
    return m_totalVoqBufferedBytes + m_totalProcessingBytes + m_totalEgressBufferedBytes;
}

uint64_t UbQueueManager::GetIngressAdmissionThresholdBytes(uint32_t, uint32_t) const
{
    if (m_paperDynamicAdmissionEnabled) {
        return GetPaperPauseThresholdBytes(GetSwitchTotalBufferedBytes());
    }
    return GetDynamicPauseThresholdBytes();
}

uint64_t UbQueueManager::GetQueueIngressSharedBytes(uint32_t inPort, uint32_t priority) const
{
    uint64_t used = m_inPortBuffer[inPort][priority];
    return used > m_reservePerQueueBytes ? used - m_reservePerQueueBytes : 0;
}

uint64_t UbQueueManager::GetQueueIngressHeadroomBytes(uint32_t inPort, uint32_t priority) const
{
    return m_hdrmBytes[inPort][priority];
}

uint64_t UbQueueManager::GetQueueIngressTotalBytes(uint32_t inPort, uint32_t priority) const
{
    return GetQueueIngressNonHeadroomBytes(inPort, priority) + GetQueueIngressHeadroomBytes(inPort, priority);
}

UbIngressQueueOccupancy UbQueueManager::GetIngressQueueOccupancy(uint32_t inPort, uint32_t priority) const
{
    return {
        .total_bytes = GetQueueIngressTotalBytes(inPort, priority),
        .shared_bytes = GetQueueIngressSharedBytes(inPort, priority),
        .headroom_bytes = GetQueueIngressHeadroomBytes(inPort, priority),
    };
}

UbIngressPortOccupancy UbQueueManager::GetIngressPortOccupancy(uint32_t inPort) const
{
    uint64_t nonHeadroomBytes = GetPortIngressNonHeadroomBytes(inPort);
    uint64_t headroomBytes = 0;
    for (uint32_t priority = 0; priority < m_vlNum; ++priority) {
        headroomBytes += m_hdrmBytes[inPort][priority];
    }
    return {
        .non_headroom_bytes = nonHeadroomBytes,
        .headroom_bytes = headroomBytes,
        .total_bytes = nonHeadroomBytes + headroomBytes,
    };
}

UbSwitchBufferOccupancy UbQueueManager::GetSwitchBufferOccupancy() const
{
    return {
        .shared_pool_used_bytes = m_sharedUsedBytes,
        .total_buffered_bytes = GetSwitchTotalBufferedBytes(),
    };
}

UbBufferProfileView UbQueueManager::GetBufferProfileView() const
{
    return {
        .reserve_per_queue_bytes = m_reservePerQueueBytes,
        .shared_pool_bytes = m_sharedPoolBytes,
        .headroom_per_port_bytes = m_headroomPerPortBytes,
    };
}

uint64_t UbQueueManager::GetIngressControlBytes(uint32_t inPort, uint32_t priority) const
{
    return m_ingressControlBytes[inPort][priority];
}

uint64_t UbQueueManager::GetOutPortControlBytes(uint32_t outPort, uint32_t priority) const
{
    return m_outPortControlBytes[outPort][priority];
}

void UbQueueManager::AddEgressBufferedBytes(uint32_t bytes)
{
    m_totalEgressBufferedBytes += bytes;
}

void UbQueueManager::RemoveEgressBufferedBytes(uint32_t bytes)
{
    NS_ASSERT_MSG(m_totalEgressBufferedBytes >= bytes, "Egress total accounting underflow");
    m_totalEgressBufferedBytes -= bytes;
}

} // namespace ns3
