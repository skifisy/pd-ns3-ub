// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-tp-connection-manager.h"
#include "ns3/ub-controller.h"
#include "ns3/node-list.h"
#include "ns3/ub-utils.h"

using namespace ns3;
namespace utils {

NS_LOG_COMPONENT_DEFINE("TpConnectionManager");
NS_OBJECT_ENSURE_REGISTERED(TpConnectionManager);

// Dynamic TP lifecycle operations may touch both endpoint LPs and must share one lock.
std::mutex TpConnectionManager::g_tpConnectionControlLock;

TypeId TpConnectionManager::GetTypeId()
{
    static TypeId tid = TypeId("ns3::TpConnectionManager")
        .SetParent<Object>()
        .SetGroupName("UnifiedBus")
        .AddAttribute("RemoveUselessTp",
                      "Automatically destroy idle transport channels that have completed all WQE segments.",
                      BooleanValue(false),
                      MakeBooleanAccessor(&TpConnectionManager::m_removeUselessTp),
                      MakeBooleanChecker());
    return tid;
}

TpConnectionManager::TpConnectionManager()
{
    m_random = CreateObject<UniformRandomVariable>();
    m_random->SetAttribute("Min", DoubleValue(0.0));
    m_random->SetAttribute("Max", DoubleValue(1.0));
}

TpConnectionManager::~TpConnectionManager()
{

}

// 添加连接（不需要nodeId参数）
void TpConnectionManager::AddConnection(const Connection& conn)
{
    std::lock_guard<std::mutex> lock(m_stateLock);
    // 存储连接
    m_allConnections.push_back(conn);

    // 为两个节点都建立索引
    BuildIndexesForNode(conn.node1, conn);
    BuildIndexesForNode(conn.node2, conn);
}

// 添加单边连接
void TpConnectionManager::AddUnilateralConnection(const Connection& conn, const uint32_t src)
{
    std::lock_guard<std::mutex> lock(m_stateLock);
    // 存储连接
    m_allConnections.push_back(conn);
    // 如果源节点既不是conn的node1也不是node2，则判错
    if (src != conn.node1 && src != conn.node2) {
        NS_ASSERT_MSG(0, "conn.src != src");
    }
    // 仅为源节点建立索引
    BuildIndexesForNode(src, conn);
}

// 获取指定节点的连接管理器视图
Ptr<TpConnectionManager> TpConnectionManager::GetConnectionManagerByNode(uint32_t nodeId)
{
    Ptr<TpConnectionManager> nodeManager = CreateObject<TpConnectionManager>();

    // 复制与该节点相关的连接
    std::lock_guard<std::mutex> lock(m_stateLock);
    auto it = m_nodeConnections.find(nodeId);
    if (it != m_nodeConnections.end()) {
        for (const auto& conn : it->second) {
            nodeManager->AddUnilateralConnection(conn, nodeId);
        }
    }

    return nodeManager;
}

// 根据预设配置的规则选择获取tpns
std::vector<uint32_t> TpConnectionManager::GetTpns(GetTpnRuleT ruler,
                                bool useShortestPath,
                                bool useMultiPath,
                                uint32_t localNodeId,
                                uint32_t peerNodeId,
                                uint32_t localPort,
                                uint32_t peerPort,
                                uint32_t priority)
{
    std::lock_guard<std::mutex> controlLock(g_tpConnectionControlLock);
    std::vector<std::pair<uint32_t, uint32_t>> resWithMetrics;
    std::vector<uint32_t> tpns;
    uint32_t minMetrics = UINT32_MAX;
    switch (ruler) {
        case GetTpnRuleT::BY_PEERNODE:
            resWithMetrics = GetTpnsByPeerNode(localNodeId, peerNodeId);
            break;
        case GetTpnRuleT::BY_PEERNODE_PRIORITY:
            resWithMetrics = GetTpnsByPeerNodePriority(localNodeId, peerNodeId, priority);
            break;
        case GetTpnRuleT::BY_PEERNODE_LOCALPORT:
            resWithMetrics = GetTpnsByPeerNodeLocalPort(localNodeId, peerNodeId, localPort);
            break;
        case GetTpnRuleT::BY_PEERNODE_BOTHPORTS:
            resWithMetrics = GetTpnsByPeerNodeBothPorts(localNodeId, peerNodeId, localPort, peerPort);
            break;
        case GetTpnRuleT::BY_ALL:
            resWithMetrics = GetTpnsByFullCriteria(localNodeId, peerNodeId, localPort, peerPort, priority);
            break;
        default:
            break;
    }
    if (resWithMetrics.empty())
    {
        resWithMetrics =
            ReserveNewTpConnections(localNodeId, peerNodeId, priority, useShortestPath);
    }

    for (auto p : resWithMetrics) {
        minMetrics = std::min(minMetrics, p.second);
    }
    for (auto p : resWithMetrics) {
        if (!useShortestPath) {
            tpns.push_back(p.first);
        } else if (minMetrics == p.second) {
            tpns.push_back(p.first);
        }
    }
    NS_ABORT_MSG_IF(tpns.empty(), "Could not reserve a TP connection for the requested traffic");
    return ReconstructTPs(tpns, localNodeId, peerNodeId, priority, useShortestPath, useMultiPath);
}

void
TpConnectionManager::ReserveTpnsForTraffic(bool useShortestPath,
                                           uint32_t localNodeId,
                                           uint32_t peerNodeId,
                                           uint32_t priority)
{
    std::lock_guard<std::mutex> controlLock(g_tpConnectionControlLock);
    if (!GetTpnsByPeerNodePriority(localNodeId, peerNodeId, priority).empty())
    {
        return;
    }
    ReserveNewTpConnections(localNodeId, peerNodeId, priority, useShortestPath);
}

// 1. 获取节点相关的所有连接
std::vector<Connection> TpConnectionManager::GetNodeConnections(uint32_t nodeId) const
{
    std::lock_guard<std::mutex> lock(m_stateLock);
    auto it = m_nodeConnections.find(nodeId);
    if (it != m_nodeConnections.end()) {
        return it->second;
    }
    return {};
}

// 2. 通过对端nodeId找到所有可用tpn
std::vector<std::pair<uint32_t, uint32_t>> TpConnectionManager::GetTpnsByPeerNode(
    uint32_t localNodeId, uint32_t peerNodeId) const
{
    std::vector<std::pair<uint32_t, uint32_t>> tpns;
    std::lock_guard<std::mutex> lock(m_stateLock);
    auto key = std::make_pair(localNodeId, peerNodeId);
    auto it = m_peerNodeIndex.find(key);
    if (it != m_peerNodeIndex.end()) {
        for (const auto& conn : it->second) {
            // 获取本地节点对应的TPN
            if (conn.node1 == localNodeId) {
                tpns.push_back(std::make_pair(conn.tpn1, conn.metrics));
            } else if (conn.node2 == localNodeId) {
                tpns.push_back(std::make_pair(conn.tpn2, conn.metrics));
            }
        }
    }
    return tpns;
}

// 3. 通过对端nodeId+priority找到所有可用tpn
std::vector<std::pair<uint32_t, uint32_t>> TpConnectionManager::GetTpnsByPeerNodePriority(
    uint32_t localNodeId, uint32_t peerNodeId, uint32_t priority) const
{
    std::vector<std::pair<uint32_t, uint32_t>> tpns;
    std::lock_guard<std::mutex> lock(m_stateLock);
    auto key = std::make_tuple(localNodeId, peerNodeId, priority);
    auto it = m_peerNodePriorityIndex.find(key);
    if (it != m_peerNodePriorityIndex.end()) {
        for (const auto& conn : it->second) {
            // 获取本地节点对应的TPN
            if (conn.node1 == localNodeId) {
                tpns.push_back(std::make_pair(conn.tpn1, conn.metrics));
            } else if (conn.node2 == localNodeId) {
                tpns.push_back(std::make_pair(conn.tpn2, conn.metrics));
            }
        }
    }
    return tpns;
}

// 4. 通过对端nodeId+本端port找到所有可用tpn
std::vector<std::pair<uint32_t, uint32_t>> TpConnectionManager::GetTpnsByPeerNodeLocalPort(
    uint32_t localNodeId, uint32_t peerNodeId, uint32_t localPort) const
{
    std::vector<std::pair<uint32_t, uint32_t>> tpns;
    std::lock_guard<std::mutex> lock(m_stateLock);
    auto key = std::make_tuple(localNodeId, peerNodeId, localPort);
    auto it = m_peerNodeLocalPortIndex.find(key);
    if (it != m_peerNodeLocalPortIndex.end()) {
        for (const auto& conn : it->second) {
            // 获取本地节点对应的TPN
            if (conn.node1 == localNodeId) {
                tpns.push_back(std::make_pair(conn.tpn1, conn.metrics));
            } else if (conn.node2 == localNodeId) {
                tpns.push_back(std::make_pair(conn.tpn2, conn.metrics));
            }
        }
    }
    return tpns;
}

// 5. 通过对端nodeId+本端port+对端port找到所有可用tpn
std::vector<std::pair<uint32_t, uint32_t>> TpConnectionManager::GetTpnsByPeerNodeBothPorts(
    uint32_t localNodeId, uint32_t peerNodeId, uint32_t localPort, uint32_t peerPort) const
{
    std::vector<std::pair<uint32_t, uint32_t>> tpns;
    std::lock_guard<std::mutex> lock(m_stateLock);
    auto key = std::make_tuple(localNodeId, peerNodeId, localPort, peerPort);
    auto it = m_bothPortsIndex.find(key);
    if (it != m_bothPortsIndex.end()) {
        for (const auto& conn : it->second) {
            // 获取本地节点对应的TPN
            if (conn.node1 == localNodeId) {
                tpns.push_back(std::make_pair(conn.tpn1, conn.metrics));
            } else if (conn.node2 == localNodeId) {
                tpns.push_back(std::make_pair(conn.tpn2, conn.metrics));
            }
        }
    }
    return tpns;
}

// 组合查询：通过完整条件找到所有可用tpn
std::vector<std::pair<uint32_t, uint32_t>> TpConnectionManager::GetTpnsByFullCriteria(
    uint32_t localNodeId, uint32_t peerNodeId, uint32_t localPort, uint32_t peerPort, uint32_t priority) const
{
    std::vector<std::pair<uint32_t, uint32_t>> tpns;
    std::lock_guard<std::mutex> lock(m_stateLock);

    // 遍历本节点的所有连接，找到匹配条件的
    auto it = m_nodeConnections.find(localNodeId);
    if (it != m_nodeConnections.end()) {
        for (const auto& conn : it->second) {
            // 检查是否匹配条件
            bool isMatch = false;
            uint32_t localTpn = 0;

            if (conn.node1 == localNodeId && conn.node2 == peerNodeId) {
                // localNodeId是node1
                if (conn.port1 == localPort && conn.port2 == peerPort &&
                    conn.priority == priority) {
                    isMatch = true;
                    localTpn = conn.tpn1;
                }
            } else if (conn.node2 == localNodeId && conn.node1 == peerNodeId) {
                // localNodeId是node2
                if (conn.port2 == localPort && conn.port1 == peerPort &&
                    conn.priority == priority) {
                    isMatch = true;
                    localTpn = conn.tpn2;
                }
            }

            if (isMatch) {
                tpns.push_back(std::make_pair(localTpn, conn.metrics));
            }
        }
    }
    return tpns;
}

// 获取某个节点的所有TPN
std::vector<uint32_t> TpConnectionManager::GetAllTpnsForNode(uint32_t nodeId) const
{
    std::vector<uint32_t> tpns;
    std::lock_guard<std::mutex> lock(m_stateLock);
    auto it = m_nodeConnections.find(nodeId);
    if (it != m_nodeConnections.end()) {
        for (const auto& conn : it->second) {
            if (conn.node1 == nodeId) {
                tpns.push_back(conn.tpn1);
            } else if (conn.node2 == nodeId) {
                tpns.push_back(conn.tpn2);
            }
        }
    }
    return tpns;
}

// 获取节点的邻居节点
std::set<uint32_t> TpConnectionManager::GetNeighborNodes(uint32_t nodeId) const
{
    std::set<uint32_t> neighbors;
    std::lock_guard<std::mutex> lock(m_stateLock);
    auto it = m_nodeConnections.find(nodeId);
    if (it != m_nodeConnections.end()) {
        for (const auto& conn : it->second) {
            if (conn.node1 == nodeId) {
                neighbors.insert(conn.node2);
            } else if (conn.node2 == nodeId) {
                neighbors.insert(conn.node1);
            }
        }
    }
    return neighbors;
}

// 获取与该tp相关的conn
Connection TpConnectionManager::GetConnection(uint32_t tpn, uint32_t src, uint32_t dst, uint32_t priority)
{
    std::lock_guard<std::mutex> lock(m_stateLock);
    auto key = std::make_tuple(src, dst, priority);
    auto it = m_peerNodePriorityIndex.find(key);
    bool exists = false;
    Connection connection;
    NS_ASSERT_MSG(it != m_peerNodePriorityIndex.end(), "Could not find (src, dst, priority) record.");
    for (const auto& conn : it->second) {
        if (conn.node1 == src && conn.tpn1 == tpn) {
            exists = true;
            connection = conn;
        }
    }
    NS_ASSERT_MSG(exists, "Could not find tpn record.");
    return connection;
}

// 获取与该tp相关的conn,不限优先级
Connection TpConnectionManager::GetConnection(uint32_t tpn, uint32_t src, uint32_t dst)
{
    std::lock_guard<std::mutex> lock(m_stateLock);
    auto key = std::make_pair(src, dst);
    auto it = m_peerNodeIndex.find(key);
    bool exists = false;
    Connection connection;
    NS_ASSERT_MSG(it != m_peerNodeIndex.end(), "Could not find (src, dst) record.");
    for (const auto& conn : it->second) {
        if (conn.node1 == src && conn.tpn1 == tpn) {
            exists = true;
            connection = conn;
            break;
        }
    }
    NS_ASSERT_MSG(exists, "Could not find tpn record.");
    return connection;
}

bool
TpConnectionManager::TryGetLocalConnection(uint32_t localNodeId,
                                           uint32_t localTpn,
                                           Connection& connection) const
{
    std::lock_guard<std::mutex> lock(m_stateLock);
    auto it = m_localTpnIndex.find({localNodeId, localTpn});
    if (it == m_localTpnIndex.end())
    {
        return false;
    }
    connection = it->second;
    return true;
}

// 清空指定节点的连接
void TpConnectionManager::ClearNodeConnections(uint32_t nodeId)
{
    std::lock_guard<std::mutex> lock(m_stateLock);
    m_nodeConnections.erase(nodeId);
    ClearNodeFromIndexes(nodeId);

    // 从allConnections_中移除相关连接
    m_allConnections.erase(
        std::remove_if(m_allConnections.begin(), m_allConnections.end(),
            [nodeId](const Connection& conn) {
                return conn.node1 == nodeId || conn.node2 == nodeId;
            }),
        m_allConnections.end()
    );
}

// 清空所有连接
void TpConnectionManager::Clear()
{
    std::lock_guard<std::mutex> lock(m_stateLock);
    m_allConnections.clear();
    m_nodeConnections.clear();
    m_peerNodeIndex.clear();
    m_peerNodePriorityIndex.clear();
    m_peerNodeLocalPortIndex.clear();
    m_bothPortsIndex.clear();
    m_localTpnIndex.clear();
    m_tpnList.clear();
    m_reservedTpnList.clear();
}

// 为指定节点建立各种索引
void TpConnectionManager::BuildIndexesForNode(uint32_t localNodeId, Connection conn)
{
    uint32_t peerNodeId;
    uint32_t localPort;
    uint32_t peerPort;
    uint32_t localTpn;
    uint32_t peerTpn;

    // 确定对端节点和端口
    if (conn.node1 == localNodeId) {
        peerNodeId = conn.node2;
        localPort = conn.port1;
        peerPort = conn.port2;
        localTpn = conn.tpn1;
        peerTpn = conn.tpn2;
    } else {
        peerNodeId = conn.node1;
        localPort = conn.port2;
        peerPort = conn.port1;
        localTpn = conn.tpn2;
        peerTpn = conn.tpn1;
    }
    conn.node1 = localNodeId;
    conn.node2 = peerNodeId;
    conn.port1 = localPort;
    conn.port2 = peerPort;
    conn.tpn1 = localTpn;
    conn.tpn2 = peerTpn;
    // 建立各种索引

    // 索引1: 添加到节点连接列表
    m_nodeConnections[localNodeId].push_back(conn);

    // 索引2: 对端节点
    m_peerNodeIndex[{localNodeId, peerNodeId}].push_back(conn);

    // 索引3: 对端节点+优先级
    m_peerNodePriorityIndex[{localNodeId, peerNodeId, static_cast<uint32_t>(conn.priority)}].push_back(conn);

    // 索引4: 对端节点+本端端口
    m_peerNodeLocalPortIndex[{localNodeId, peerNodeId, localPort}].push_back(conn);

    // 索引5: 对端节点+本端端口+对端端口
    m_bothPortsIndex[{localNodeId, peerNodeId, localPort, peerPort}].push_back(conn);

    // 索引6: 本地 TPN
    m_localTpnIndex[{localNodeId, localTpn}] = conn;

    if (m_reservedTpnList.erase(localTpn) == 0) {
        NS_ASSERT_MSG(m_tpnList.find(localTpn) == m_tpnList.end(), "Tpn already exists!");
    }
    m_tpnList.insert(localTpn);
}

// 从索引中清理指定节点
void TpConnectionManager::ClearNodeFromIndexes(uint32_t nodeId)
{
    // 清理各种索引中包含该节点的条目
    for (auto it = m_peerNodeIndex.begin(); it != m_peerNodeIndex.end();) {
        if (it->first.first == nodeId) {
            it = m_peerNodeIndex.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = m_peerNodePriorityIndex.begin(); it != m_peerNodePriorityIndex.end();) {
        if (std::get<0>(it->first) == nodeId) {
            it = m_peerNodePriorityIndex.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = m_peerNodeLocalPortIndex.begin(); it != m_peerNodeLocalPortIndex.end();) {
        if (std::get<0>(it->first) == nodeId) {
            it = m_peerNodeLocalPortIndex.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = m_bothPortsIndex.begin(); it != m_bothPortsIndex.end();) {
        if (std::get<0>(it->first) == nodeId) {
            it = m_bothPortsIndex.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = m_localTpnIndex.begin(); it != m_localTpnIndex.end();)
    {
        if (it->first.first == nodeId)
        {
            it = m_localTpnIndex.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

std::vector<std::pair<uint32_t, uint32_t>>
TpConnectionManager::ReserveNewTpConnections(uint32_t src,
                                             uint32_t dst,
                                             uint32_t priority,
                                             bool useShortestPath)
{
    std::vector<std::pair<uint32_t, uint32_t>> tpns;
    // 目标节点+端口的ip地址，出端口和跳数pair 的 vector
    std::map<Ipv4Address, std::vector<std::pair<uint16_t, uint32_t>>> routingEntries;
    // 查找路由，寻找从本节点到目的节点的路径
    Ptr<UbRoutingProcess> rt = NodeList::GetNode(src)->GetObject<UbSwitch>()->GetRoutingProcess();
    for (uint32_t dstPort = 0; dstPort < NodeList::GetNode(dst)->GetNDevices(); dstPort++) {
        // 查找去往该ip的出端口，若非空则记录
        Ipv4Address dstPortIp = NodeIdToIp(dst, dstPort);
        std::vector<uint16_t> shortestOutPortsVec, otherOutPortsVec;
        rt->GetShortestOutPorts(dstPortIp.Get(), shortestOutPortsVec);
        rt->GetOtherOutPorts(dstPortIp.Get(), otherOutPortsVec);
        if (!shortestOutPortsVec.empty()) {
            routingEntries[dstPortIp] = std::vector<std::pair<uint16_t, uint32_t>>();
            for (uint16_t outPort : shortestOutPortsVec) {
                routingEntries[dstPortIp].push_back(std::make_pair(outPort, 0));
            }
        }
        // 仅在开启非最短路模式的情况下加入非最短路由
        if (!otherOutPortsVec.empty() && !useShortestPath) {
            routingEntries[dstPortIp] = std::vector<std::pair<uint16_t, uint32_t>>();
            for (uint16_t outPort : otherOutPortsVec) {
                routingEntries[dstPortIp].push_back(std::make_pair(outPort, 1));
            }
        }
    }
    // 如果为空，则判错，路由有问题。不考虑省略host路由的情况
    if (routingEntries.empty()) {
        NS_ASSERT_MSG(0, "Invalid routing! Traffic can't arrive!");
    }
    for (auto it = routingEntries.begin(); it != routingEntries.end(); it++) {
        Ipv4Address dstIp = it->first;
        uint32_t dstPort = IpToPortId(dstIp);
        for (auto outPortIt = it->second.begin(); outPortIt != it->second.end(); outPortIt++) {
            // 无论是否创建TP，都建立connection记录，以供后续所有源目的节点相同的流使用
            Ptr<ns3::UbController> sendCtrl = NodeList::GetNode(src)->GetObject<ns3::UbController>();
            Ptr<ns3::UbController> recvCtrl = NodeList::GetNode(dst)->GetObject<ns3::UbController>();
            Connection conn;
            conn.node1 = src;
            conn.port1 = outPortIt->first;
            conn.tpn1 = GetNextTpn();
            conn.node2 = dst;
            conn.port2 = dstPort;
            conn.tpn2 = recvCtrl->GetTpConnManager()->GetNextTpn();
            conn.priority = priority;
            conn.metrics = outPortIt->second;
            // connection添加tpnConn
            AddUnilateralConnection(conn, src);
            recvCtrl->GetTpConnManager()->AddUnilateralConnection(conn, dst);
            tpns.emplace_back(conn.tpn1, conn.metrics);
        }
    }
    return tpns;
}

std::vector<uint32_t> TpConnectionManager::ReconstructTPs(std::vector<uint32_t> tpns, uint32_t src, uint32_t dst,
    uint32_t priority, bool useShortestPath, bool useMultiPath)
{
    std::vector<uint32_t> res;
    if (!useMultiPath) { // 单路径模式下仅创建一个
        uint32_t idx = (uint32_t)(m_random->GetValue() * tpns.size());
        NS_LOG_DEBUG("Connection num: " << tpns.size() << " Reconstruct idx: " << idx);
        Connection conn = GetConnection(tpns[idx], src, dst, priority);
        res.push_back(ReconstructTp(conn));
    } else { // 多路径模式下全部创建
        NS_LOG_DEBUG("Connection num: " << tpns.size() << " all reconstruct.");
        for (uint32_t i = 0; i < tpns.size(); i++) {
            Connection conn = GetConnection(tpns[i], src, dst, priority);
            res.push_back(ReconstructTp(conn));
        }
    }
    return res;
}

uint32_t TpConnectionManager::ReconstructTp(Connection conn)
{
    Ptr<UbController> sendCtrl = NodeList::GetNode(conn.node1)->GetObject<UbController>();
    if (!sendCtrl->IsTPExists(conn.tpn1)) { // 不存在，创建
        auto sendHostCaqm = UbCongestionControl::Create(UB_DEVICE);
        sendCtrl->CreateTp(conn.node1, conn.node2, conn.port1, conn.port2,
                           conn.priority, conn.tpn1, conn.tpn2, sendHostCaqm);
    }
    return conn.tpn1;
}

void TpConnectionManager::RemoveUselessTps(uint32_t jettyNum, uint32_t src, uint32_t dst, uint32_t priority)
{
    if (!m_removeUselessTp) { // 仅在开启了删除无用tp模式下才进行此操作
        return;
    }
    std::lock_guard<std::mutex> controlLock(g_tpConnectionControlLock);
    auto ctrl = NodeList::GetNode(src)->GetObject<UbController>();
    auto sendTa = ctrl->GetUbTransaction();
    // 事务层删除与该jetty绑定的tp记录
    sendTa->DestroyJettyTpMap(jettyNum);
    // 获取当前已经没有jetty与之绑定的tp
    auto tpns = sendTa->GetUselessTpns();
    for (uint32_t i = 0; i < tpns.size(); i++) {
        ctrl->DestroyTp(tpns[i]);
        Connection conn = GetConnection(tpns[i], src, dst);
        NodeList::GetNode(dst)->GetObject<UbController>()->DestroyTp(conn.tpn2);
        NS_LOG_DEBUG("Delete node: " << src << " tpn: " << tpns[i] <<
                        " node: " << dst << " tpn:" << conn.tpn2);
    }
}

uint32_t TpConnectionManager::GetNextTpn()
{
    std::lock_guard<std::mutex> lock(m_stateLock);
    return ReserveNextTpnLocked();
}

uint32_t TpConnectionManager::ReserveNextTpnLocked()
{
    while (m_tpnList.find(m_nextTpn) != m_tpnList.end() ||
           m_reservedTpnList.find(m_nextTpn) != m_reservedTpnList.end()) {
        ++m_nextTpn;
    }

    const uint32_t reservedTpn = m_nextTpn;
    m_reservedTpnList.insert(reservedTpn);
    ++m_nextTpn;
    return reservedTpn;
}

}
