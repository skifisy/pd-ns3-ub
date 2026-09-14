// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TP_CONNECTION_MANAGER_H
#define UB_TP_CONNECTION_MANAGER_H

#include <unordered_map>
#include <vector>
#include <set>
#include <map>
#include "ub-network-address.h"
#include "ns3/log.h"
#include "ns3/random-variable-stream.h"
#include <mutex>

namespace ns3
{
class UbController;
}

namespace utils {
/**
 * @brief tp-config.csv 中定义的数据结构
 */
struct Connection {
    uint32_t node1;
    uint32_t port1;
    uint32_t tpn1;
    uint32_t node2;
    uint32_t port2;
    uint32_t tpn2;
    uint32_t priority;
    uint32_t metrics;
};

/**
 * @brief 获取TPN规制的枚举类
 */
enum class GetTpnRuleT {
    BY_PEERNODE = 0,
    BY_PEERNODE_PRIORITY,
    BY_PEERNODE_LOCALPORT,
    BY_PEERNODE_BOTHPORTS,
    BY_ALL,
    OTHER
};

/**
 * @brief 连接索引管理器，管理所有连接
 */
class TpConnectionManager : public Object {
public:
    static TypeId GetTypeId();

    TpConnectionManager();
    ~TpConnectionManager();
    // 添加连接（不需要nodeId参数）
    void AddConnection(const Connection& conn);

    // 添加单边连接
    void AddUnilateralConnection(const Connection& conn, const uint32_t src);

    // 获取指定节点的连接管理器视图
    Ptr<TpConnectionManager> GetConnectionManagerByNode(uint32_t nodeId);

    // 根据预设配置的规则选择获取tpns
    std::vector<uint32_t> GetTpns(GetTpnRuleT ruler,
                                  bool useShortestPath,
                                  bool useMultiPath,
                                  uint32_t localNodeId,
                                  uint32_t peerNodeId,
                                  uint32_t localPort = UINT32_MAX,
                                  uint32_t peerPort = UINT32_MAX,
                                  uint32_t priority = UINT32_MAX);

    /**
     * @brief Reserve connection records without materializing either TP endpoint.
     * @param useShortestPath Whether reserved paths must use shortest routes.
     * @param localNodeId Local node identifier.
     * @param peerNodeId Peer node identifier.
     * @param priority Traffic priority associated with the reservation.
     */
    void ReserveTpnsForTraffic(bool useShortestPath,
                               uint32_t localNodeId,
                               uint32_t peerNodeId,
                               uint32_t priority);

    // 1. 获取节点相关的所有连接
    std::vector<Connection> GetNodeConnections(uint32_t nodeId) const;

    // 2. 通过对端nodeId找到所有可用tpn
    std::vector<std::pair<uint32_t, uint32_t>> GetTpnsByPeerNode(uint32_t localNodeId, uint32_t peerNodeId) const;

    // 3. 通过对端nodeId+priority找到所有可用tpn
    std::vector<std::pair<uint32_t, uint32_t>> GetTpnsByPeerNodePriority(uint32_t localNodeId,
                                                    uint32_t peerNodeId,
                                                    uint32_t priority) const;

    // 4. 通过对端nodeId+本端port找到所有可用tpn
    std::vector<std::pair<uint32_t, uint32_t>> GetTpnsByPeerNodeLocalPort(uint32_t localNodeId,
                                                     uint32_t peerNodeId,
                                                     uint32_t localPort) const;

    // 5. 通过对端nodeId+本端port+对端port找到所有可用tpn
    std::vector<std::pair<uint32_t, uint32_t>> GetTpnsByPeerNodeBothPorts(uint32_t localNodeId,
                                                     uint32_t peerNodeId,
                                                     uint32_t localPort,
                                                     uint32_t peerPort) const;

    // 组合查询：通过完整条件找到所有可用tpn
    std::vector<std::pair<uint32_t, uint32_t>> GetTpnsByFullCriteria(uint32_t localNodeId,
                                               uint32_t peerNodeId,
                                               uint32_t localPort,
                                               uint32_t peerPort,
                                               uint32_t priority) const;

    // 获取某个节点的所有TPN
    std::vector<uint32_t> GetAllTpnsForNode(uint32_t nodeId) const;

    // 获取节点的邻居节点
    std::set<uint32_t> GetNeighborNodes(uint32_t nodeId) const;

    // 获取所有连接
    const std::vector<Connection>& GetAllConnections() const
    {
        return m_allConnections;
    }

    // 获取连接总数
    size_t GetConnectionCount() const
    {
        return m_allConnections.size();
    }

    // 获取与该tp相关的conn
    Connection GetConnection(uint32_t tpn, uint32_t src, uint32_t dst, uint32_t priority);

    // 获取与该tp相关的conn，所有优先级
    Connection GetConnection(uint32_t tpn, uint32_t src, uint32_t dst);

    /**
     * @brief Find a reservation by the local node and local TPN.
     * @param localNodeId Local node identifier.
     * @param localTpn Locally assigned transport path number.
     * @param connection Output connection arranged from the local node's perspective.
     * @return True when a matching reservation exists.
     */
    bool TryGetLocalConnection(uint32_t localNodeId,
                               uint32_t localTpn,
                               Connection& connection) const;

    // 清空指定节点的连接
    void ClearNodeConnections(uint32_t nodeId);

    // 清空所有连接
    void Clear();

    // 删除无用tp
    void RemoveUselessTps(uint32_t jettyNum, uint32_t src, uint32_t dst, uint32_t priority);

    uint32_t GetNextTpn();

    bool IsTpRemoveMode()
    {
        return m_removeUselessTp;
    }
private:
  friend class ns3::UbController;

  static std::mutex g_tpConnectionControlLock;

  uint32_t ReserveNextTpnLocked();

  // 为指定节点建立各种索引
  void BuildIndexesForNode(uint32_t localNodeId, Connection conn);

  // 从索引中清理指定节点
  void ClearNodeFromIndexes(uint32_t nodeId);

  // Reserve new TP connection records and return their local TPN/metric pairs.
  std::vector<std::pair<uint32_t, uint32_t>> ReserveNewTpConnections(uint32_t src,
                                                                     uint32_t dst,
                                                                     uint32_t priority,
                                                                     bool useShortestPath);

  // 重建tp对
  std::vector<uint32_t> ReconstructTPs(std::vector<uint32_t> tpns,
                                       uint32_t src,
                                       uint32_t dst,
                                       uint32_t priority,
                                       bool useShortestPath,
                                       bool useMultiPath);

  uint32_t ReconstructTp(Connection conn);

private:
    // 主存储：所有连接
    std::vector<Connection> m_allConnections;

    // 每个节点的相关连接
    std::unordered_map<uint32_t, std::vector<Connection>> m_nodeConnections;

    // 索引2: (localNodeId, peerNodeId) -> connections
    std::map<std::pair<uint32_t, uint32_t>, std::vector<Connection>> m_peerNodeIndex;

    // 索引3: (localNodeId, peerNodeId, priority) -> connections
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::vector<Connection>> m_peerNodePriorityIndex;

    // 索引4: (localNodeId, peerNodeId, localPort) -> connections
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::vector<Connection>> m_peerNodeLocalPortIndex;

    // 索引5: (localNodeId, peerNodeId, localPort, peerPort) -> connections
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, std::vector<Connection>> m_bothPortsIndex;

    // 索引6: (localNodeId, localTpn) -> connection
    std::map<std::pair<uint32_t, uint32_t>, Connection> m_localTpnIndex;

    std::set<uint32_t> m_tpnList;
    std::set<uint32_t> m_reservedTpnList;

    uint32_t m_nextTpn = 0;

    Ptr<UniformRandomVariable> m_random;

    bool m_removeUselessTp = false;

    mutable std::mutex m_stateLock;
};

} // namespace utils

#endif
