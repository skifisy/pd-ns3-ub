// SPDX-License-Identifier: GPL-2.0-only
#include <algorithm>
#include <limits>
#include "ns3/ub-controller.h"
#include "ns3/ub-header.h"
#include "ns3/ub-network-address.h"
#include "ns3/ub-port.h"
#include "ns3/ub-queue-manager.h"
#include "ns3/ub-routing-process.h"
#include "ns3/udp-header.h"
#include "ns3/ipv4-header.h"
using namespace utils;

namespace ns3 {
NS_OBJECT_ENSURE_REGISTERED(UbRoutingProcess);
NS_LOG_COMPONENT_DEFINE("UbRoutingProcess");

namespace
{
constexpr uint32_t kPrimaryRangeRoutePort = std::numeric_limits<uint32_t>::max();
}

/*-----------------------------------------UbRoutingProcess----------------------------------------------*/
TypeId UbRoutingProcess::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbRoutingProcess")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbRoutingProcess>()
        .AddAttribute("RoutingAlgorithm",
                    "Routing algorithm applied by UbRoutingProcess.",
                    EnumValue(UbRoutingAlgorithm::HASH),
                    MakeEnumAccessor<UbRoutingProcess::UbRoutingAlgorithm>(
                                    &UbRoutingProcess::m_routingAlgorithm),
                    MakeEnumChecker(UbRoutingAlgorithm::HASH, "HASH",
                                    UbRoutingAlgorithm::ADAPTIVE, "ADAPTIVE"));
    return tid;
}

UbRoutingProcess::UbRoutingProcess()
{
}

std::shared_ptr<std::vector<uint16_t> >
UbRoutingProcess::GetOrCreatePortSet(const std::vector<uint16_t>& ports)
{
    std::vector<uint16_t> normalized = normalizePorts(ports);
    auto it = m_portSetPool.find(normalized);
    if (it != m_portSetPool.end())
    {
        return it->second;
    }

    auto sharedPorts = std::make_shared<std::vector<uint16_t>>(normalized);
    m_portSetPool[normalized] = sharedPorts;
    return sharedPorts;
}

void UbRoutingProcess::AddShortestRoute(const uint32_t destIP, const std::vector<uint16_t>& outPorts)
{
    // 标准化端口集合（排序去重）
    std::vector<uint16_t> target;
    auto itRt = m_rtShortest.find(destIP);
    if (itRt != m_rtShortest.end()) {
        target.insert(target.end(), (*(itRt->second)).begin(), (*(itRt->second)).end());
    }
    target.insert(target.end(), outPorts.begin(), outPorts.end());
    m_rtShortest[destIP] = GetOrCreatePortSet(target);
}

void UbRoutingProcess::AddOtherRoute(const uint32_t destIP, const std::vector<uint16_t>& outPorts)
{
    // 标准化端口集合（排序去重）
    std::vector<uint16_t> target;
    auto itRt = m_rtOther.find(destIP);
    if (itRt != m_rtOther.end()) {
        target.insert(target.end(), (*(itRt->second)).begin(), (*(itRt->second)).end());
    }
    target.insert(target.end(), outPorts.begin(), outPorts.end());
    m_rtOther[destIP] = GetOrCreatePortSet(target);
}

void
UbRoutingProcess::AddRouteRange(RouteRangeByPortMap& routeRangesByPort,
                                uint32_t startNodeId,
                                uint32_t endNodeId,
                                uint32_t dstPortId,
                                const std::vector<uint16_t>& outPorts)
{
    AddRouteRangeToMap(routeRangesByPort[dstPortId], startNodeId, endNodeId, outPorts);
}

void
UbRoutingProcess::AddRouteRangeToMap(RouteRangeMap& routeRanges,
                                     uint32_t startNodeId,
                                     uint32_t endNodeId,
                                     const std::vector<uint16_t>& outPorts)
{
    NS_ASSERT_MSG(startNodeId <= endNodeId, "route range start must not exceed end");

    auto valueAt = [&routeRanges](uint32_t nodeId) -> std::shared_ptr<std::vector<uint16_t> > {
        auto it = routeRanges.upper_bound(nodeId);
        if (it == routeRanges.begin())
        {
            return nullptr;
        }
        --it;
        return it->second;
    };

    if (routeRanges.find(startNodeId) == routeRanges.end())
    {
        routeRanges[startNodeId] = valueAt(startNodeId);
    }
    if (endNodeId != std::numeric_limits<uint32_t>::max())
    {
        const uint32_t afterEnd = endNodeId + 1;
        if (routeRanges.find(afterEnd) == routeRanges.end())
        {
            routeRanges[afterEnd] = valueAt(afterEnd);
        }
    }

    for (auto it = routeRanges.lower_bound(startNodeId);
         it != routeRanges.end() && it->first <= endNodeId;
         ++it)
    {
        std::vector<uint16_t> target;
        if (it->second != nullptr)
        {
            target.insert(target.end(), it->second->begin(), it->second->end());
        }
        target.insert(target.end(), outPorts.begin(), outPorts.end());
        it->second = GetOrCreatePortSet(target);
    }
}

void
UbRoutingProcess::AddShortestRouteRange(uint32_t startNodeId,
                                        uint32_t endNodeId,
                                        const std::vector<uint16_t>& outPorts)
{
    AddShortestRouteRange(startNodeId, endNodeId, 0, outPorts);
}

void
UbRoutingProcess::AddOtherRouteRange(uint32_t startNodeId,
                                     uint32_t endNodeId,
                                     const std::vector<uint16_t>& outPorts)
{
    AddOtherRouteRange(startNodeId, endNodeId, 0, outPorts);
}

void
UbRoutingProcess::AddShortestRouteRange(uint32_t startNodeId,
                                        uint32_t endNodeId,
                                        uint32_t dstPortId,
                                        const std::vector<uint16_t>& outPorts)
{
    AddRouteRange(m_rtShortestRanges,
                  startNodeId,
                  endNodeId,
                  kPrimaryRangeRoutePort,
                  outPorts);
    AddRouteRange(m_rtShortestRanges, startNodeId, endNodeId, dstPortId, outPorts);
}

void
UbRoutingProcess::AddOtherRouteRange(uint32_t startNodeId,
                                     uint32_t endNodeId,
                                     uint32_t dstPortId,
                                     const std::vector<uint16_t>& outPorts)
{
    AddRouteRange(m_rtOtherRanges,
                  startNodeId,
                  endNodeId,
                  kPrimaryRangeRoutePort,
                  outPorts);
    AddRouteRange(m_rtOtherRanges, startNodeId, endNodeId, dstPortId, outPorts);
}

bool
UbRoutingProcess::GetRangeOutPortsFromMap(const RouteRangeMap& routeRanges,
                                          uint32_t nodeId,
                                          std::vector<uint16_t>& outPorts)
{
    if (routeRanges.empty())
    {
        return false;
    }

    auto it = routeRanges.upper_bound(nodeId);
    if (it == routeRanges.begin())
    {
        return false;
    }
    --it;
    if (it->second == nullptr)
    {
        return false;
    }
    outPorts.insert(outPorts.end(), it->second->begin(), it->second->end());
    return true;
}

void
UbRoutingProcess::GetRangeOutPorts(const RouteRangeByPortMap& routeRangesByPort,
                                   const uint32_t destIP,
                                   std::vector<uint16_t>& outPorts)
{
    if (routeRangesByPort.empty())
    {
        return;
    }

    const Ipv4Address ip(destIP);
    const uint32_t nodeId = utils::IpToNodeId(ip);
    const uint32_t lastByte = destIP & 0x000000ff;
    const uint32_t portId = lastByte == 0 ? kPrimaryRangeRoutePort : lastByte - 1;
    auto portIt = routeRangesByPort.find(portId);
    if (portIt != routeRangesByPort.end())
    {
        GetRangeOutPortsFromMap(portIt->second, nodeId, outPorts);
    }
}

void UbRoutingProcess::GetShortestOutPorts(const uint32_t destIP, std::vector<uint16_t>& outPorts)
{
    outPorts.clear();
    auto it = m_rtShortest.find(destIP);
    if (it != m_rtShortest.end()) {
        outPorts.insert(outPorts.end(), (*(it->second)).begin(), (*(it->second)).end());
        return;
    }
    GetRangeOutPorts(m_rtShortestRanges, destIP, outPorts);
}

void UbRoutingProcess::GetOtherOutPorts(const uint32_t destIP, std::vector<uint16_t>& outPorts)
{
    outPorts.clear();
    auto it = m_rtOther.find(destIP);
    if (it != m_rtOther.end()) {
        outPorts.insert(outPorts.end(), (*(it->second)).begin(), (*(it->second)).end());
        return;
    }
    GetRangeOutPorts(m_rtOtherRanges, destIP, outPorts);
}

void UbRoutingProcess::GetShortestCandidates(uint32_t &dip, uint16_t inPortId, std::vector<uint16_t>& outPorts)
{
    // 1. 首先基于目的节点的port地址进行选择
    GetShortestOutPorts(dip, outPorts);
    if (outPorts.empty()) {
        // 2. 如果找不到，掩盖port地址，使用主机的primary地址进行寻址
        Ipv4Mask mask("255.255.255.0");
        uint32_t maskedDip = Ipv4Address(dip).CombineMask(mask).Get();
        if (maskedDip != dip) {
            GetShortestOutPorts(maskedDip, outPorts);
            dip = maskedDip;
        }
    }

    // 3. 过滤掉入端口
    if (inPortId != UINT16_MAX) {
        auto it = std::remove_if(outPorts.begin(), outPorts.end(), 
                                  [inPortId](uint16_t port) { return port == inPortId; });
        outPorts.erase(it, outPorts.end());
    }
}

void UbRoutingProcess::GetNonShortestCandidates(uint32_t &dip, uint16_t inPortId, std::vector<uint16_t>& outPorts)
{
    // 1. 首先基于目的节点的port地址进行选择
    GetOtherOutPorts(dip, outPorts);
    if (outPorts.empty()) {
        // 2. 如果找不到，掩盖port地址，使用主机的primary地址进行寻址
        Ipv4Mask mask("255.255.255.0");
        uint32_t maskedDip = Ipv4Address(dip).CombineMask(mask).Get();
        if (maskedDip != dip) {
            GetOtherOutPorts(maskedDip, outPorts);
            dip = maskedDip;
        }
    }

    // 3. 过滤掉入端口
    if (inPortId != UINT16_MAX) {
        auto it = std::remove_if(outPorts.begin(), outPorts.end(), 
                                  [inPortId](uint16_t port) { return port == inPortId; });
        outPorts.erase(it, outPorts.end());
    }
}

const std::vector<uint16_t> UbRoutingProcess::GetAllOutPorts(const uint32_t destIP)
{
    std::vector<uint16_t> res;
    auto it = m_rtOther.find(destIP);
    if (it != m_rtOther.end()) {
        res.insert(res.end(), (*(it->second)).begin(), (*(it->second)).end());
    } else {
        GetRangeOutPorts(m_rtOtherRanges, destIP, res);
    }
    it = m_rtShortest.find(destIP);
    if (it != m_rtShortest.end()) {
        res.insert(res.end(), (*(it->second)).begin(), (*(it->second)).end());
    } else {
        GetRangeOutPorts(m_rtShortestRanges, destIP, res);
    }
    return res;
}


// 删除路由条目
bool UbRoutingProcess::RemoveShortestRoute(const uint32_t destIP)
{
    return m_rtShortest.erase(destIP) > 0;
}

// 删除路由条目
bool UbRoutingProcess::RemoveOtherRoute(const uint32_t destIP)
{
    return m_rtOther.erase(destIP) > 0;
}

int UbRoutingProcess::SelectAdaptiveOutPort(RoutingKey &rtKey, const std::vector<uint16_t>& shortestPorts,
                                             const std::vector<uint16_t>& nonShortestPorts, bool &selectedShortestPath)
{
    auto node = NodeList::GetNode(m_nodeId);
    auto ubSwitch = node->GetObject<UbSwitch>();
    auto queueManager = ubSwitch->GetQueueManager();
    uint8_t priority = rtKey.priority;

    auto calcLoadScore = [&](uint16_t outPort) -> uint64_t {
        if (queueManager == nullptr) {
            return 0;
        }
        // 使用OutPort视图统计VOQ占用
        uint64_t voqLoad = queueManager->GetOutPortBufferUsed(outPort, static_cast<uint32_t>(priority));
        
        // 加上EgressQueue的字节占用
        Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(outPort));
        uint64_t egressLoad = port->GetUbQueue()->GetCurrentBytes();
        
        // 总负载 = VOQ + EgressQueue
        return voqLoad + egressLoad;
    };

    // 构造总候选列表：先 shortest，后 nonShortest
    std::vector<uint16_t> candidatePorts;
    candidatePorts.insert(candidatePorts.end(), shortestPorts.begin(), shortestPorts.end());
    candidatePorts.insert(candidatePorts.end(), nonShortestPorts.begin(), nonShortestPorts.end());

    if (candidatePorts.empty()) {
        return -1;
    }

    uint64_t bestScore = std::numeric_limits<uint64_t>::max();
    std::vector<uint16_t> bestPorts;
    size_t bestIndex = 0;
    for (size_t i = 0; i < candidatePorts.size(); ++i) {
        uint16_t port = candidatePorts[i];
        uint64_t score = calcLoadScore(port);
        if (score < bestScore) {
            bestScore = score;
            bestPorts.clear();
            bestPorts.push_back(port);
            bestIndex = i;
        } else if (score == bestScore) {
            bestPorts.push_back(port);
        }
    }

    if (bestPorts.empty()) {
        return -1;
    }

    // 通过索引判断是否选中最短路径
    selectedShortestPath = (bestIndex < shortestPorts.size());
    uint16_t selectedPort = bestPorts.front();
    return selectedPort;
}

uint64_t UbRoutingProcess::CalcHash(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint8_t priority, uint32_t salt)
{
    uint8_t buf[17];
    buf[0] = (sip >> 24) & 0xff;
    buf[1] = (sip >> 16) & 0xff;
    buf[2] = (sip >> 8) & 0xff;
    buf[3] = sip & 0xff;
    buf[4] = (dip >> 24) & 0xff;
    buf[5] = (dip >> 16) & 0xff;
    buf[6] = (dip >> 8) & 0xff;
    buf[7] = dip & 0xff;
    buf[8] = (sport >> 8) & 0xff;
    buf[9] = sport & 0xff;
    buf[10] = (dport >> 8) & 0xff;
    buf[11] = dport & 0xff;
    buf[12] = priority;
    buf[13] = (salt >> 24) & 0xff;
    buf[14] = (salt >> 16) & 0xff;
    buf[15] = (salt >> 8) & 0xff;
    buf[16] = salt & 0xff;
    std::string str(reinterpret_cast<const char*>(buf), sizeof(buf));
    uint64_t hash = Hash64(str);
    return hash;
}

int UbRoutingProcess::SelectOutPort(RoutingKey &rtKey, const std::vector<uint16_t>& shortestPorts, 
                                     const std::vector<uint16_t>& nonShortestPorts, bool &selectedShortestPath)
{
    uint32_t sip = rtKey.sip;
    uint32_t dip = rtKey.dip;
    uint16_t sport = rtKey.sport;
    uint16_t dport = rtKey.dport;
    uint8_t priority = rtKey.priority;
    bool usePacketSpray = rtKey.usePacketSpray;
    bool hashIncludesTransportPorts = rtKey.hashIncludesTransportPorts;
    // hash key用本地ip做盐值，使同一条流/包在不同交换机上会有不同的hash
    uint32_t salt = utils::NodeIdToIp(m_nodeId).Get();

    size_t totalSize = shortestPorts.size() + nonShortestPorts.size();

    if (totalSize == 0) {
        return -1;
    }

    uint64_t hash64 = 0;
    if (usePacketSpray) {
        // Packet spray should stay exactly even over time for each flow while still
        // randomizing the starting port across different flows.
        const uint64_t flowBase = CalcHash(sip, dip, 0, dport, priority, salt);
        const uint64_t packetSalt = CalcHash(0, 0, sport, 0, 0, salt);
        hash64 = flowBase + packetSalt;
    } else {
        // usePacketSpray == LB_MODE_PER_FLOW
        hash64 = hashIncludesTransportPorts ? CalcHash(sip, dip, sport, dport, priority, salt)
                                             : CalcHash(sip, dip, 0, 0, priority, salt);
    }
    
    size_t idx = hash64 % totalSize;
    
    // 通过索引判断是否选中最短路径，并直接返回对应集合中的端口
    if (idx < shortestPorts.size()) {
        selectedShortestPath = true;
        return shortestPorts[idx];
    } else {
        selectedShortestPath = false;
        return nonShortestPorts[idx - shortestPorts.size()];
    }
}

// 1. GetCandidatePorts基于useShortestPath选择可用的出端口集合
// 2. 基于用户设定的UbRoutingAlgorithm在candidatePorts中选择最终的出端口
// 2.1 如果是 HASH 算法，基于五元组哈希选择出端口(如果是usePacketSpray则使用完整五元组，否则掩盖sport和dport为0)
// 2.2 如果是 ADAPTIVE 算法，基于QueueManager信息选择负载最小的出端口
// 3. 如果找不到出端口，报错
int UbRoutingProcess::GetOutPort(RoutingKey &rtKey, bool &selectedShortestPath, uint16_t inPort)
{
    uint32_t sip = rtKey.sip;
    uint32_t dip = rtKey.dip;
    uint16_t sport = rtKey.sport;
    uint16_t dport = rtKey.dport;
    uint8_t priority = rtKey.priority;
    bool useShortestPath = rtKey.useShortestPath;
    bool usePacketSpray = rtKey.usePacketSpray;
    NS_LOG_DEBUG("[UbRoutingProcess GetOutPort]: sip: " << Ipv4Address(sip)
                << " dip: " << Ipv4Address(dip)
                << " sport: " << sport
                << " dport: " << dport
                << " priority: " << (uint16_t)priority
                << " useShortestPath: " << useShortestPath
                << " usePacketSpray: " << usePacketSpray);
    
    uint32_t tempDip = dip;
    
    // 分别获取最短路径和非最短路径候选端口
    std::vector<uint16_t> shortestPorts;
    std::vector<uint16_t> nonShortestPorts;
    GetShortestCandidates(tempDip, inPort, shortestPorts);
    if (!useShortestPath) {
        // 只有在不限制最短路径时，才获取非最短路径候选端口
        GetNonShortestCandidates(tempDip, inPort, nonShortestPorts);
    }
    
    // 检查是否有可用端口
    if (shortestPorts.empty() && nonShortestPorts.empty()) {
        NS_LOG_ERROR("No candidate ports found for dip: " << Ipv4Address(dip));
        return -1;
    }

    // 基于路由算法选择出端口，同时获得 selectedShortestPath 标记
    int outPortId = -1;
    if(m_routingAlgorithm == UbRoutingProcess::UbRoutingAlgorithm::HASH){
        outPortId = SelectOutPort(rtKey, shortestPorts, nonShortestPorts, selectedShortestPath);
    } else if(m_routingAlgorithm == UbRoutingProcess::UbRoutingAlgorithm::ADAPTIVE){
        outPortId = SelectAdaptiveOutPort(rtKey, shortestPorts, nonShortestPorts, selectedShortestPath);
    }

    // 若找不到出端口，报ASSERT
    NS_ASSERT_MSG(outPortId != -1, "No available output port found");
    
    return outPortId;
}
} // namespace ns3
