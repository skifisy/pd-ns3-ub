// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_NETWORK_ADDRESS_H
#define UB_NETWORK_ADDRESS_H

#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include "ns3/core-module.h"
#include "ns3/ipv4-address.h"
using namespace std;
using namespace ns3;

namespace utils {

// brief
struct TrafficRecord
{
    int taskId;
    int sourceNode;
    int destNode;
    int dataSize;
    string opType;
    int priority;
    string delay;
    int phaseId;
    vector<uint32_t> dependOnPhases;
    uint32_t srcEntityId = 0;
    uint32_t dstEntityId = 0;
    bool hasSrcEntityId{false};
    bool hasDstEntityId{false};
};

// Lightweight CSV row view. string_view fields are valid only during the parser callback.
struct TrafficRecordView
{
    uint32_t taskId{0};
    uint32_t sourceNode{0};
    uint32_t destNode{0};
    uint32_t dataSize{0};
    std::string_view opType{};
    uint8_t priority{0};
    std::string_view delay{};
    uint32_t phaseId{0};
    std::string_view dependOnPhases{};
    uint32_t srcEntityId{0};
    uint32_t dstEntityId{0};
    bool hasSrcEntityId{false};
    bool hasDstEntityId{false};
};

constexpr long DEFAULT_PORT_BUFFER_SIZE = 2097152;
// 根据NodeId转IPv4地址
constexpr int BYTE_RANGE = 256;
inline Ipv4Address NodeIdToIp(uint32_t id)
{
    return Ipv4Address(0x0a000000 + ((id / BYTE_RANGE) * 0x00010000) +
                     ((id % BYTE_RANGE) * 0x00000100));
}

inline uint32_t IpToNodeId(Ipv4Address ipAddress)
{
    // 0x0a000000 is the network prefix (10.0.0.0)
    uint32_t ip = ipAddress.Get();
    ip -= 0x0a000000;
    uint32_t high_byte = (ip & 0x00FF0000) >> 16;
    uint32_t low_byte = (ip & 0x0000FF00) >> 8;
    return high_byte * BYTE_RANGE + low_byte;
}

inline uint32_t IpToPortId(Ipv4Address ipAddress)
{
    uint32_t ip = ipAddress.Get();
    return ((ip & 0x000000FF) - 1);
}

inline Ipv4Address NodeIdToIp(uint32_t id, uint32_t portId)
{
    // 确保portId在0-255范围内（1字节）
    portId = (portId + 1) % BYTE_RANGE;

    return Ipv4Address(0x0a000000 +          // 基础网络10.0.0.0
                     ((id / BYTE_RANGE) * 0x00010000) +  // 第三字节偏移
                     ((id % BYTE_RANGE) * 0x00000100) +  // 原第四字节偏移（将被覆盖）
                     portId);               // 用portId替换最后字节
}

inline bool IsInSameSubnet(const Ipv4Address& a, const Ipv4Address& b, const Ipv4Mask& mask)
{
    return a.CombineMask(mask) == b.CombineMask(mask);
}

// Cna16
inline uint32_t NodeIdToCna16(uint32_t nodeId, uint32_t portId)
{
    portId = portId + 1;
    // 确保portId不超过4位（0-15）
    portId &= 0xF; // 只保留低4位

    // 确保nodeId不超过12位（0-4095）
    nodeId &= 0xFFF; // 只保留低12位

    // 将nodeId左移4位，空出低4位给portId
    return (nodeId << 4) | portId;
}

inline uint32_t NodeIdToCna16(uint32_t nodeId)
{
    // 确保nodeId不超过12位（0-4095）
    nodeId &= 0xFFF; // 只保留低12位

    // 将nodeId左移4位，低4位补0
    return nodeId << 4;
}

inline uint32_t Cna16ToNodeId(uint32_t cnaAddr)
{
    return (cnaAddr >> 4) & 0xFFF; // 右移4位后取低12位 (掩码 0x0FFF)
}

inline uint32_t Cna16ToPortId(uint32_t cnaAddr)
{
    return (cnaAddr & 0xF) - 1; // 取低4位 (掩码 0x0F)
}

inline Ipv4Address Cna16ToIp(uint32_t cnaAddr)
{
    // Convert CNA to NodeId, then NodeId to IP
    uint32_t node_id = Cna16ToNodeId(cnaAddr);
    return NodeIdToIp(node_id);
}

inline uint32_t IpToCna16(Ipv4Address ip)
{
    // Convert IP to NodeId, then NodeId to Cna16
    uint32_t node_id = IpToNodeId(ip);
    return NodeIdToCna16(node_id);
}

// Cna24
inline uint32_t NodeIdToCna24(uint32_t nodeId, uint32_t portId)
{
    portId = portId + 1;
    // 确保portId不超过8位
    portId &= 0xFF; // 只保留低8位

    // 确保nodeId不超过16位
    nodeId &= 0xFFFF; // 只保留低16位

    // 将nodeId左移8位，空出低8位给portId
    return (nodeId << 8) | portId;
}

inline uint32_t NodeIdToCna24(uint32_t nodeId)
{
    // 确保nodeId不超过16位
    nodeId &= 0xFFFF; // 只保留低16位

    // 将nodeId左移8位，低8位补0
    return nodeId << 8;
}
inline uint32_t Cna24ToNodeId(uint32_t cnaAddr)
{
    return (cnaAddr >> 8) & 0xFFFF; // 右移8位后取低16位 (掩码 0x0FFFF)
}

inline uint32_t Cna24ToPortId(uint32_t cnaAddr)
{
    return (cnaAddr & 0xFF) - 1; // 取低8位 (掩码 0x0FF)
}

inline Ipv4Address Cna24ToIp(uint32_t cnaAddr)
{
    // Convert CNA to NodeId, then NodeId to IP
    uint32_t node_id = Cna24ToNodeId(cnaAddr);
    return NodeIdToIp(node_id);
}

inline uint32_t IpToCna24(Ipv4Address ip)
{
    // Convert IP to NodeId, then NodeId to Cna24
    uint32_t node_id = IpToNodeId(ip);
    return NodeIdToCna24(node_id);
}

}
#endif
