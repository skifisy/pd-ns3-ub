// SPDX-License-Identifier: GPL-2.0-only
#ifndef FAULT_H
#define FAULT_H

#include <iostream>
#include <sstream>
#include <chrono>
#include <map>
#include <fstream>
#include <vector>
#include <algorithm>
#include <string>
#include "ns3/ptr.h"
#include "ns3/log.h"
#include "ns3/net-device-container.h"
#include "ns3/tcp-header.h"
#include "ns3/data-rate.h"
#include "ns3/ub-port.h"
#include "ns3/flow-id-tag.h"
#include "ns3/packet.h"
#include "ns3/ipv4-header.h"
using namespace std;
namespace ns3 {

/**
 * @brief 故障类型说明
 * @enum class  FaultType
 * @param DROPPACKET 概率丢包
 * @param ADDPACKETDELAY 增加时延
 * @param CONGESTION 拥塞
 * @param SHUTDOWNUP 闪断
 * @param LOWERDATARATE 降lane
 * @param ERRORPACKET 错包
 */
enum class FaultType : uint16_t { DROPPACKET, ADDPACKETDELAY, CONGESTION, SHUTDOWNUP, LOWERDATARATE, ERRORPACKET };

/**
 * @brief 带宽设置
 * @struct LowerDataRate
 * @param nodeId 节点id
 * @param portId 节点端口id
 * @param dataRate 要设置的速率 Mbps
 */
struct LowerDataRate {
    uint32_t nodeId;
    uint32_t portId;
    DataRate dataRate;
};

/**
 * @brief 闪断丢包设置
 * @struct ShutDownPacketDrop
 * @param begin 开始丢包编号
 * @param end 结束丢包编号
 */
struct ShutDownPacketDrop {
    uint32_t begin;
    uint32_t end;
};

/**
 * @brief 故障类型参数读取结果
 * @struct FaultInfo
 * @param faultType  0丢包(%) 1时延(ns) 2拥塞(Mbps) 3闪断 4 降lane(Mbps) 5 错包
 * @param droprate faultType-0 对应丢包率
 * @param delay 时延增加 ns
 * @param erorDropRate 错包率 以一定概率丢包
 * @param shutDownPacketDrop 设置一段连续丢包
 */
struct FaultInfo {
    FaultType  faultType;
    double dropRate;
    uint64_t delay;
    double erorDropRate;
    ShutDownPacketDrop shutDownPacketDrop;
};

/**
 * @brief 记录时延增加的节点和端口
 * @struct PortDelay
 * @param nodeId 节点id
 * @param portId 节点端口id
 */
struct PortDelay {
    uint32_t nodeId;
    uint32_t portId;
};

/**
 * @brief 记录丢包节点 端口 已发送和丢包数据大小
 * @struct PortDrop
 * @param nodeId 节点id
 * @param portId 节点端口id
 * @param sendSize 已发送数据大小
 * @param dropSize 已丢包数据大小
 */
struct PortDrop {
    uint32_t nodeId;
    uint32_t portId;
    uint64_t sendSize;
    uint64_t dropSize;
};

/**
 * @brief 记录闪断丢包 端口 已发送和丢包数据大小
 * @struct PortShutdownDrop
 * @param nodeId 节点id
 * @param portId 节点端口id
 * @param sendSize 已发送数据大小
 * @param dropSize 已丢包数据大小
 * @param sendNum 已发送包个数
 */
struct PortShutdownDrop {
    uint32_t nodeId;
    uint32_t portId;
    uint64_t sendSize;
    uint64_t dropSize;
    uint64_t sendNum;
};

enum class RetransFaultDirection : uint8_t {
    ANY,
    FORWARD,
    REVERSE,
};

enum class RetransFaultPacketType : uint8_t {
    ANY,
    DATA,
    TPACK,
    TPSACK,
    TPNAK,
};

enum class RetransFaultTriState : uint8_t {
    ANY,
    FALSE_VALUE,
    TRUE_VALUE,
};

struct RetransFaultPacketInfo {
    uint32_t taskId = 0;
    uint32_t nodeId = 0;
    uint32_t portId = 0;
    RetransFaultDirection direction = RetransFaultDirection::ANY;
    RetransFaultPacketType packetType = RetransFaultPacketType::ANY;
    uint32_t psn = 0;
    bool lastPacket = false;
};

struct RetransFaultRule {
    string ruleId;
    bool enabled = false;
    bool anyTask = false;
    uint32_t taskId = 0;
    bool anyNode = false;
    uint32_t nodeId = 0;
    bool anyPort = false;
    uint32_t portId = 0;
    RetransFaultDirection direction = RetransFaultDirection::ANY;
    RetransFaultPacketType packetType = RetransFaultPacketType::ANY;
    bool anyPsn = false;
    uint32_t psn = 0;
    RetransFaultTriState lastPacket = RetransFaultTriState::ANY;
    uint32_t dropCount = 0;
    uint32_t delayNs = 0;
    uint32_t delayCount = 0;
    uint32_t matchCount = 0;
    uint32_t droppedCount = 0;
    uint32_t delayedCount = 0;
    string comment;
};

/**
 * \class UbFault
 * \brief fault injection
 */
class UbFault : public Object {
public:
    bool isInitFault;
    BooleanValue isPacketFlow;
    bool isPacketFlowValue;
    map<uint32_t, FaultInfo> faultMap;
    map<uint32_t, PortDelay> delayMap;
    map<uint32_t, PortDrop> dropMap;
    map<uint32_t, PortDrop> errorDropMap;
    map<uint32_t, PortShutdownDrop> shutdownDropMap;
    vector<RetransFaultRule> retransFaultRules;

    UbFault();

    virtual ~UbFault();
    static TypeId GetTypeId(void);
    vector<string> Split(const string &s, char delim);
    void ReadCongestionOrLowerDataRateParams(
        map<uint32_t, FaultInfo> &faultMap, LowerDataRate &lowerDataRate, const string &cell, uint32_t taskId);
    void ReadShutDownParams(map<uint32_t, FaultInfo> &faultMap, const string &cell, uint32_t taskId);
    void InitFault(const string &filename);
    void InitRetransFault(const string &filename);
    template <typename MapType, typename searchKey>
    bool MapTaskFind(MapType &srcMap, searchKey key)
    {
        auto it = srcMap.find(key);
        return it != srcMap.end();
    }

    uint32_t GetPacketSize(Ptr<Packet> packet);
    int GetRandomToDeterminePacketDrop(
        uint64_t packetSize, double lossProbability, uint32_t taskId, uint32_t nodeId, uint32_t portId);
    int SetErrorPacket(uint64_t packetSize, double lossProbability, uint32_t taskId, uint32_t nodeId, uint32_t portId);
    int SetPacketDelay(uint32_t taskId, uint32_t nodeId, uint32_t portId);
    int SetPacketDrop(uint64_t packetSize, double dropRate, uint32_t taskId, uint32_t nodeId, uint32_t portId);
    void SetPortCongestion(LowerDataRate lowerDataRate);
    int JudgeShutdownRangeToDeterminePacketDrop(uint64_t packetSize, uint32_t taskId, uint32_t nodeId, uint32_t portId);
    int SetPortShutdownAndUp(uint64_t packetSize, uint32_t taskId, uint32_t nodeId, uint32_t portId);
    bool TryGetRetransFaultPacketInfo(
        Ptr<Packet> packet, uint32_t nodeId, uint32_t portId, uint32_t taskId, RetransFaultPacketInfo &info);
    bool MatchRetransFaultRule(const RetransFaultRule &rule, const RetransFaultPacketInfo &info) const;
    int SetRetransFaultPacketDrop(Ptr<Packet> packet, uint32_t taskId, uint32_t nodeId, uint32_t portId);
    int FaultDiagnosis(Ptr<Packet> packet, uint32_t nodeId, uint32_t portId, Ptr<UbPort> ubPort);
    int FaultCallback(Ptr<Packet> packet, uint32_t nodeId, uint32_t portId, Ptr<UbPort> ubPort);
};
}  // namespace ns3
#endif
