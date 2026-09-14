// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-fault.h"
#include "ns3/node-list.h"
#include "ns3/node.h"
#include "ns3/udp-header.h"
#include "ns3/ub-header.h"

#include <cctype>
#include <stdexcept>

using namespace std;
using namespace utils;
namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(UbFault);
NS_LOG_COMPONENT_DEFINE("UbFault");

namespace {

string
Trim(const string& value)
{
    const size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

string
ToLowerCopy(string value)
{
    transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(tolower(ch));
    });
    return value;
}

vector<string>
SplitCsvRow(const string& line)
{
    vector<string> fields;
    string field;
    stringstream ss(line);
    while (getline(ss, field, ',')) {
        fields.push_back(Trim(field));
    }
    return fields;
}

bool
IsAnyField(const string& value)
{
    const string normalized = ToLowerCopy(Trim(value));
    return normalized.empty() || normalized == "any" || normalized == "*";
}

bool
ParseEnabled(const string& value)
{
    const string normalized = ToLowerCopy(Trim(value));
    return normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "y";
}

void
ParseOptionalUint(const string& value, bool& anyValue, uint32_t& parsedValue)
{
    anyValue = IsAnyField(value);
    if (anyValue) {
        parsedValue = 0;
        return;
    }
    parsedValue = static_cast<uint32_t>(stoul(value));
}

RetransFaultDirection
ParseRetransFaultDirection(const string& value)
{
    const string normalized = ToLowerCopy(Trim(value));
    if (normalized == "forward") {
        return RetransFaultDirection::FORWARD;
    }
    if (normalized == "reverse") {
        return RetransFaultDirection::REVERSE;
    }
    return RetransFaultDirection::ANY;
}

RetransFaultPacketType
ParseRetransFaultPacketType(const string& value)
{
    const string normalized = ToLowerCopy(Trim(value));
    if (normalized == "data") {
        return RetransFaultPacketType::DATA;
    }
    if (normalized == "tpack") {
        return RetransFaultPacketType::TPACK;
    }
    if (normalized == "tpsack") {
        return RetransFaultPacketType::TPSACK;
    }
    if (normalized == "tpnak") {
        return RetransFaultPacketType::TPNAK;
    }
    return RetransFaultPacketType::ANY;
}

RetransFaultTriState
ParseRetransFaultTriState(const string& value)
{
    const string normalized = ToLowerCopy(Trim(value));
    if (normalized == "true" || normalized == "1" || normalized == "yes") {
        return RetransFaultTriState::TRUE_VALUE;
    }
    if (normalized == "false" || normalized == "0" || normalized == "no") {
        return RetransFaultTriState::FALSE_VALUE;
    }
    return RetransFaultTriState::ANY;
}

string
BuildRetransFaultFilename(const string& faultFilename)
{
    const size_t slash = faultFilename.find_last_of("/\\");
    if (slash == string::npos) {
        return "retrans_fault.csv";
    }
    return faultFilename.substr(0, slash + 1) + "retrans_fault.csv";
}

RetransFaultPacketType
DecodeRetransFaultPacketType(uint8_t opcode, uint8_t rspSt)
{
    if (opcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_RELIABLE_TA) ||
        opcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_UNRELIABLE_TA)) {
        return RetransFaultPacketType::DATA;
    }
    if (opcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH) ||
        opcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITH_CETPH)) {
        return rspSt == 3 ? RetransFaultPacketType::TPNAK : RetransFaultPacketType::TPACK;
    }
    if (opcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH) ||
        opcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_SACK_WITH_CETPH)) {
        return RetransFaultPacketType::TPSACK;
    }
    return RetransFaultPacketType::ANY;
}

RetransFaultDirection
DirectionFromPacketType(RetransFaultPacketType packetType)
{
    if (packetType == RetransFaultPacketType::DATA) {
        return RetransFaultDirection::FORWARD;
    }
    if (packetType == RetransFaultPacketType::TPACK ||
        packetType == RetransFaultPacketType::TPSACK ||
        packetType == RetransFaultPacketType::TPNAK) {
        return RetransFaultDirection::REVERSE;
    }
    return RetransFaultDirection::ANY;
}

} // namespace

/*********************
 * UbFault
 ********************/

// UbFault
UbFault::UbFault()
{
    isInitFault = false;
}

UbFault::~UbFault()
{
    NS_LOG_FUNCTION(this);
}
TypeId UbFault::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbFault")
                            .SetParent<Object>()
                            .AddConstructor<UbFault>()
                            .AddAttribute("UbFaultUsePacketSpray",
                                          "When true, use per-packet spray for fault-injected flows; "
                                          "when false, use per-flow forwarding.",
                                          BooleanValue(false),
                                          MakeBooleanAccessor(&UbFault::isPacketFlow),
                                          MakeBooleanChecker());
    return tid;
}
vector<string> UbFault::Split(const string &s, char delim)
{
    vector<string> tokens;
    string token;
    istringstream tokenStream(s);
    while (getline(tokenStream, token, delim)) {
        if (!token.empty())
            tokens.push_back(token);
    }
    return tokens;
}

void UbFault::ReadCongestionOrLowerDataRateParams(map<uint32_t, FaultInfo> &faultMap, LowerDataRate &lowerDataRate,
                                                  const string &cell,uint32_t taskId)
{
    uint8_t congestionAndLowerDataRateParamsCount = 3;
    if (faultMap[taskId].faultType == FaultType::CONGESTION || faultMap[taskId].faultType == FaultType::LOWERDATARATE) {
        if (cell.find(' ') != string::npos) {
            vector<string> spaceParts = Split(cell, ' ');
            if (spaceParts.size() == congestionAndLowerDataRateParamsCount) {
                lowerDataRate.nodeId = static_cast<uint32_t>(stoi(spaceParts[0]));
                lowerDataRate.portId = static_cast<uint32_t>(stoi(spaceParts[1]));
                lowerDataRate.dataRate = DataRate(spaceParts[2] + "Mbps");
                // faultType==2
                // 先跑一遍流量，查看端口平均带宽信息。将最大带宽的端口在跑流量之前将带宽设置为最大带宽的一半
                // faultType==4
                // 在开始跑仿真前将拓扑的带宽减为一半（可以提前跑一遍流量看下端口级带宽，选择有流量的端口进行降lane）
                SetPortCongestion(lowerDataRate);
            } else {
                NS_LOG_WARN("Invalid fault injection config (LOWERDATARATE): incorrect number of parameters. Line ignored.");
            }
        }
    }
}

void UbFault::ReadShutDownParams(map<uint32_t, FaultInfo> &faultMap, const string &cell, uint32_t taskId)
{
    uint8_t shutDownParamsCount = 2;
    if (faultMap[taskId].faultType == FaultType::SHUTDOWNUP) {
        if (cell.find(' ') != string::npos) {
            vector<string> spaceParts = Split(cell, ' ');
            if (spaceParts.size() == shutDownParamsCount) {
                faultMap[taskId].shutDownPacketDrop = {static_cast<uint32_t>(stoi(spaceParts[0])),
                                                    static_cast<uint32_t>(stoi(spaceParts[1]))};
            } else {
                NS_LOG_WARN("Invalid fault injection config (SHUTDOWNUP): incorrect number of parameters. Line ignored.");
            }
        }
    }
}

void UbFault::InitFault(const string &filename)
{
    if (isInitFault)
        return;
    isInitFault = true;
    isPacketFlowValue = isPacketFlow.Get();
    NS_LOG_INFO("Init Fault moudle.");
    ifstream file(filename);
    if (!file.is_open()) {
        NS_LOG_DEBUG("Can not open File: " << filename);
    }

    string line;
    // 跳过标题行
    uint8_t percentSign = 100;
    getline(file, line);
    while (getline(file, line)) {
        // 跳过空行、#开头行、纯空格行
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }
        stringstream ss(line);
        string cell;

        getline(ss, cell, ',');
        uint32_t taskId = static_cast<uint32_t>(stoi(cell));
        faultMap[taskId];

        getline(ss, cell, ',');
        faultMap[taskId].faultType = static_cast<FaultType>(stoi(cell));

        getline(ss, cell, ',');
        faultMap[taskId].dropRate = static_cast<double>(stoi(cell)) / percentSign;

        getline(ss, cell, ',');
        faultMap[taskId].delay = static_cast<uint64_t>(stoi(cell));

        getline(ss, cell, ',');
        LowerDataRate lowerDataRate;
        ReadCongestionOrLowerDataRateParams(faultMap, lowerDataRate, cell, taskId);

        getline(ss, cell, ',');

        getline(ss, cell, ',');
        faultMap[taskId].erorDropRate = static_cast<double>(stoi(cell)) / percentSign;

        NS_LOG_DEBUG("taskId:" << taskId << ",faultType:" <<  static_cast<uint16_t>(faultMap[taskId].faultType)
                               << ",dropRate:" << faultMap[taskId].dropRate << ",delay:" << faultMap[taskId].delay
                               << ",lowerDataRate nodeId:" << lowerDataRate.nodeId << ",lowerDataRate portId:"
                               << lowerDataRate.portId << ",lowerDataRate dataRate:" << lowerDataRate.dataRate
                               << ",shutDownPacketDrop begin:" << faultMap[taskId].shutDownPacketDrop.begin
                               << ",shutDownPacketDrop end:" << faultMap[taskId].shutDownPacketDrop.end
                               << ",erorDropRate:" << faultMap[taskId].erorDropRate);
    }
    file.close();
    InitRetransFault(BuildRetransFaultFilename(filename));
}

void
UbFault::InitRetransFault(const string &filename)
{
    ifstream file(filename);
    if (!file.is_open()) {
        NS_LOG_DEBUG("Can not open retrans fault File: " << filename);
        return;
    }

    NS_LOG_INFO("Init retrans fault module: " << filename);
    string line;
    getline(file, line); // header
    uint32_t lineNo = 1;
    while (getline(file, line)) {
        lineNo++;
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }
        vector<string> fields = SplitCsvRow(line);
        if (fields.size() < 11) {
            NS_LOG_WARN("Invalid retrans fault config at line " << lineNo
                        << ": expected at least 11 columns, got " << fields.size());
            continue;
        }

        try {
            RetransFaultRule rule;
            rule.ruleId = fields[0];
            rule.enabled = ParseEnabled(fields[1]);
            ParseOptionalUint(fields[2], rule.anyTask, rule.taskId);
            ParseOptionalUint(fields[3], rule.anyNode, rule.nodeId);
            ParseOptionalUint(fields[4], rule.anyPort, rule.portId);
            rule.direction = ParseRetransFaultDirection(fields[5]);
            rule.packetType = ParseRetransFaultPacketType(fields[6]);
            ParseOptionalUint(fields[7], rule.anyPsn, rule.psn);
            rule.lastPacket = ParseRetransFaultTriState(fields[8]);
            rule.dropCount = static_cast<uint32_t>(stoul(fields[9]));
            if (fields.size() >= 13) {
                rule.delayNs = static_cast<uint32_t>(stoul(fields[10]));
                rule.delayCount = static_cast<uint32_t>(stoul(fields[11]));
                rule.comment = fields[12];
            } else {
                rule.comment = fields.size() > 10 ? fields[10] : "";
            }
            retransFaultRules.push_back(rule);
            NS_LOG_DEBUG("Loaded retrans fault rule: " << rule.ruleId
                         << " enabled:" << rule.enabled
                         << " dropCount:" << rule.dropCount
                         << " delayNs:" << rule.delayNs
                         << " delayCount:" << rule.delayCount);
        } catch (const std::exception& e) {
            NS_LOG_WARN("Invalid retrans fault config at line " << lineNo << ": " << e.what());
        }
    }
}
// compute packetSize
uint32_t UbFault::GetPacketSize(Ptr<Packet> packet)
{
    UbMAExtTah MAExtTaHeader;
    UbTransactionHeader TransactionHeader;
    UbTransportHeader TransportHeader;
    UdpHeader UHeader;
    Ipv4Header I4Header;
    UbDatalinkPacketHeader DataLinkPacketHeader;
    UbIpBasedNetworkHeader networkHeader;
    uint32_t MAExtTaHeaderSize = MAExtTaHeader.GetSerializedSize();
    uint32_t UbTransactionHeaderSize = TransactionHeader.GetSerializedSize();
    uint32_t UbTransportHeaderSize = TransportHeader.GetSerializedSize();
    uint32_t UdpHeaderSize = UHeader.GetSerializedSize();
    uint32_t Ipv4HeaderSize = I4Header.GetSerializedSize();
    uint32_t UbDataLinkPktSize = DataLinkPacketHeader.GetSerializedSize();
    uint32_t networkHeaderSize = networkHeader.GetSerializedSize();
    uint32_t headerSize = MAExtTaHeaderSize + UbTransactionHeaderSize + UbTransportHeaderSize + UdpHeaderSize +
                          Ipv4HeaderSize + UbDataLinkPktSize + networkHeaderSize;

    // cout<<"packetsize:"<<packet->GetSize()<<","<<headerSize<<endl;
    if (packet->GetSize() < headerSize)
        return 0;
    return packet->GetSize() - headerSize;
}

// Flow Following对第一跳交换机首个包设置时延 &&Packet Following 对第一跳交换机首个端口设置时延
int UbFault::SetPacketDelay(uint32_t taskId, uint32_t nodeId, uint32_t portId)
{
    if (!MapTaskFind(delayMap, taskId)) {
        delayMap[taskId] = {nodeId, portId};
        NS_LOG_DEBUG("nodeId" << nodeId << ",taskId:" << taskId << ",delay(ns):" << faultMap[taskId].delay);
        return faultMap[taskId].delay;
    } else if (isPacketFlowValue && delayMap[taskId].nodeId == nodeId && delayMap[taskId].portId == portId) {  // 逐包
        NS_LOG_DEBUG("nodeId" << nodeId << ",taskId:" << taskId << ",portId" << portId
                              << ",delay(ns):" << faultMap[taskId].delay);
        return faultMap[taskId].delay;
    }
    return 0;
}

// 按照丢包率丢包
int UbFault::SetPacketDrop(uint64_t packetSize, double dropRate, uint32_t taskId, uint32_t nodeId, uint32_t portId)
{
    int ret = 0;
    if (!MapTaskFind(dropMap, taskId)) {
        dropMap[taskId] = {nodeId, portId, packetSize, 0};
        ret = 0;
    } else if (dropMap[taskId].nodeId == nodeId && dropMap[taskId].portId == portId) {
        double curPacketDroprate =
            1.0 * (dropMap[taskId].dropSize) / (dropMap[taskId].dropSize + dropMap[taskId].sendSize);
        if (curPacketDroprate < dropRate) {
            dropMap[taskId].dropSize += packetSize;
            ret = -1;
        } else {
            dropMap[taskId].sendSize += packetSize;
            ret = 0;
        }
        curPacketDroprate = 1.0 * (dropMap[taskId].dropSize) / (dropMap[taskId].dropSize + dropMap[taskId].sendSize);
        if (isPacketFlowValue) {
            NS_LOG_DEBUG("taskId:" << taskId << ",nodeId:" << nodeId << ",portId:" << portId << ",dropsize:"
                                   << dropMap[taskId].dropSize << ",sendSize:" << dropMap[taskId].sendSize
                                   << ",curPacketDroprate:" << curPacketDroprate);
        } else {
            NS_LOG_DEBUG("taskId:" << taskId << ",nodeId:" << nodeId << ",dropsize:" << dropMap[taskId].dropSize
                                   << ",sendSize:" << dropMap[taskId].sendSize
                                   << ",curPacketDroprate:" << curPacketDroprate);
        }
    }

    return ret;
}

// 错包实现 随机丢包
int UbFault::GetRandomToDeterminePacketDrop(uint64_t packetSize, double lossProbability, uint32_t taskId,
                                            uint32_t nodeId, uint32_t portId)
{
    Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
    uv->SetAttribute("Min", DoubleValue(0.0));
    uv->SetAttribute("Max", DoubleValue(1.0));
    int ret = 0;
    if (uv->GetValue() < lossProbability) {
        errorDropMap[taskId].dropSize += packetSize;
        ret = -1;
    } else {
        errorDropMap[taskId].sendSize += packetSize;
    }
    double curPacketDroprate =
        1.0 * (errorDropMap[taskId].dropSize) / (errorDropMap[taskId].dropSize + errorDropMap[taskId].sendSize);
    if (isPacketFlowValue) {
        NS_LOG_DEBUG("taskId:" << taskId << ",nodeId:" << nodeId << ",portId" << portId << ",dropsize:"
                               << errorDropMap[taskId].dropSize << ",sendSize:" << errorDropMap[taskId].sendSize
                               << ",curPacketDroprate:" << curPacketDroprate);
    } else {
        NS_LOG_DEBUG("taskId:" << taskId << ",nodeId:" << nodeId << ",dropsize:" << errorDropMap[taskId].dropSize
                               << ",sendSize:" << errorDropMap[taskId].sendSize
                               << ",curPacketDroprate:" << curPacketDroprate);
    }

    return ret;
}

int UbFault::SetErrorPacket(uint64_t packetSize, double lossProbability, uint32_t taskId, uint32_t nodeId,
                            uint32_t portId)
{
    int ret = 0;
    if (!MapTaskFind(errorDropMap, taskId)) {
        errorDropMap[taskId] = {nodeId, portId, 0, 0};
        ret = GetRandomToDeterminePacketDrop(packetSize, lossProbability, taskId, nodeId, portId);
    } else if (errorDropMap[taskId].nodeId == nodeId && errorDropMap[taskId].portId == portId) {
        ret = GetRandomToDeterminePacketDrop(packetSize, lossProbability, taskId, nodeId, portId);
    }
    return ret;
}

void UbFault::SetPortCongestion(LowerDataRate lowerDataRate)
{
    Ptr<Node> node = NodeList::GetNode(lowerDataRate.nodeId);
    Ptr<UbPort> port = DynamicCast<ns3::UbPort>(node->GetDevice(lowerDataRate.portId));
    port->SetDataRate(lowerDataRate.dataRate);
}
int UbFault::JudgeShutdownRangeToDeterminePacketDrop(uint64_t packetSize, uint32_t taskId, uint32_t nodeId,
                                                     uint32_t portId)
{
    int ret = 0;
    shutdownDropMap[taskId].sendNum += 1;
    if ((shutdownDropMap[taskId].sendNum >= faultMap[taskId].shutDownPacketDrop.begin) &&
        (shutdownDropMap[taskId].sendNum <= faultMap[taskId].shutDownPacketDrop.end)) {
        shutdownDropMap[taskId].dropSize += packetSize;
        ret = -1;
    } else {
        shutdownDropMap[taskId].sendSize += packetSize;
        ret = 0;
    }
    double shutdownPacketDroprate = 1.0 * (shutdownDropMap[taskId].dropSize) /
                                    (shutdownDropMap[taskId].dropSize + shutdownDropMap[taskId].sendSize);

    if (isPacketFlowValue) {
        NS_LOG_DEBUG("taskId:" << taskId << ",nodeId:" << nodeId << ",portId:" << portId << ",dropsize:"
                               << shutdownDropMap[taskId].dropSize << ",sendSize:" << shutdownDropMap[taskId].sendSize
                               << ",shutdownPacketDroprate:" << shutdownPacketDroprate
                               << ",shutdownDropMap[taskId].sendNum:" << shutdownDropMap[taskId].sendNum);
    } else {
        NS_LOG_DEBUG("taskId:" << taskId << ",nodeId:" << nodeId << ",dropsize:" << shutdownDropMap[taskId].dropSize
                               << ",sendSize:" << shutdownDropMap[taskId].sendSize
                               << ",shutdownPacketDroprate:" << shutdownPacketDroprate
                               << ",shutdownDropMap[taskId].sendNum:" << shutdownDropMap[taskId].sendNum);
    }

    return ret;
}
int UbFault::SetPortShutdownAndUp(uint64_t packetSize, uint32_t taskId, uint32_t nodeId, uint32_t portId)
{
    int ret = 0;
    if (!MapTaskFind(shutdownDropMap, taskId)) {
        shutdownDropMap[taskId] = {nodeId, portId, 0, 0, 0};
        ret = JudgeShutdownRangeToDeterminePacketDrop(packetSize, taskId, nodeId, portId);
    } else if (shutdownDropMap[taskId].nodeId == nodeId && shutdownDropMap[taskId].portId == portId) {
        ret = JudgeShutdownRangeToDeterminePacketDrop(packetSize, taskId, nodeId, portId);
    }

    return ret;
}

bool
UbFault::TryGetRetransFaultPacketInfo(Ptr<Packet> packet,
                                      uint32_t nodeId,
                                      uint32_t portId,
                                      uint32_t taskId,
                                      RetransFaultPacketInfo &info)
{
    Ptr<Packet> copy = packet->Copy();
    UbDatalinkPacketHeader dataLinkHeader;
    if (copy->GetSize() < dataLinkHeader.GetSerializedSize()) {
        return false;
    }
    copy->RemoveHeader(dataLinkHeader);
    if (dataLinkHeader.GetConfig() != static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_IPV4)) {
        return false;
    }

    UbIpBasedNetworkHeader networkHeader;
    if (copy->GetSize() < networkHeader.GetSerializedSize()) {
        return false;
    }
    copy->RemoveHeader(networkHeader);

    Ipv4Header ipv4Header;
    if (copy->GetSize() < ipv4Header.GetSerializedSize()) {
        return false;
    }
    copy->RemoveHeader(ipv4Header);

    UdpHeader udpHeader;
    if (copy->GetSize() < udpHeader.GetSerializedSize()) {
        return false;
    }
    copy->RemoveHeader(udpHeader);

    UbTransportHeader transportHeader;
    if (copy->GetSize() < transportHeader.GetSerializedSize()) {
        return false;
    }
    copy->PeekHeader(transportHeader);

    RetransFaultPacketType packetType = DecodeRetransFaultPacketType(
        transportHeader.GetTPOpcode(), transportHeader.GetRspSt());
    if (packetType == RetransFaultPacketType::ANY) {
        return false;
    }

    info.taskId = taskId;
    info.nodeId = nodeId;
    info.portId = portId;
    info.packetType = packetType;
    info.direction = DirectionFromPacketType(packetType);
    info.psn = transportHeader.GetPsn();
    info.lastPacket = transportHeader.GetLastPacket();
    return true;
}

bool
UbFault::MatchRetransFaultRule(const RetransFaultRule &rule, const RetransFaultPacketInfo &info) const
{
    if (!rule.enabled || (rule.dropCount == 0 && rule.delayCount == 0)) {
        return false;
    }
    if (!rule.anyTask && rule.taskId != info.taskId) {
        return false;
    }
    if (!rule.anyNode && rule.nodeId != info.nodeId) {
        return false;
    }
    if (!rule.anyPort && rule.portId != info.portId) {
        return false;
    }
    if (rule.direction != RetransFaultDirection::ANY && rule.direction != info.direction) {
        return false;
    }
    if (rule.packetType != RetransFaultPacketType::ANY && rule.packetType != info.packetType) {
        return false;
    }
    if (!rule.anyPsn && rule.psn != info.psn) {
        return false;
    }
    if (rule.lastPacket == RetransFaultTriState::TRUE_VALUE && !info.lastPacket) {
        return false;
    }
    if (rule.lastPacket == RetransFaultTriState::FALSE_VALUE && info.lastPacket) {
        return false;
    }
    return true;
}

int
UbFault::SetRetransFaultPacketDrop(Ptr<Packet> packet, uint32_t taskId, uint32_t nodeId, uint32_t portId)
{
    if (retransFaultRules.empty()) {
        return 0;
    }

    RetransFaultPacketInfo info;
    if (!TryGetRetransFaultPacketInfo(packet, nodeId, portId, taskId, info)) {
        return 0;
    }

    for (auto &rule : retransFaultRules) {
        if (!MatchRetransFaultRule(rule, info)) {
            continue;
        }

        rule.matchCount++;
        if (rule.droppedCount < rule.dropCount) {
            rule.droppedCount++;
            NS_LOG_DEBUG("Retrans fault drop."
                         << " ruleId:" << rule.ruleId
                         << " taskId:" << taskId
                         << " nodeId:" << nodeId
                         << " portId:" << portId
                         << " psn:" << info.psn
                         << " matchCount:" << rule.matchCount
                         << " droppedCount:" << rule.droppedCount);
            return -1;
        }
        if (rule.delayedCount < rule.delayCount) {
            rule.delayedCount++;
            NS_LOG_DEBUG("Retrans fault detached delay."
                         << " ruleId:" << rule.ruleId
                         << " taskId:" << taskId
                         << " nodeId:" << nodeId
                         << " portId:" << portId
                         << " psn:" << info.psn
                         << " matchCount:" << rule.matchCount
                         << " delayedCount:" << rule.delayedCount
                         << " delayNs:" << rule.delayNs);
            return -static_cast<int>(rule.delayNs) - 2;
        }
    }

    return 0;
}

int UbFault::FaultDiagnosis(Ptr<Packet> packet, uint32_t nodeId, uint32_t portId, Ptr<UbPort> ubPort)
{
    uint64_t packetSize = GetPacketSize(packet);
    Ptr<Node> node = NodeList::GetNode(nodeId);
    Ptr<UbSwitch> retrieved_sw = node->GetObject<UbSwitch>();
    UbFlowTag flowTag;
    packet->PeekPacketTag(flowTag);
    uint32_t taskId = flowTag.GetFlowId();
    int ret = 0;
    if (retrieved_sw->GetNodeType() == UB_SWITCH) {
        ret = SetRetransFaultPacketDrop(packet, taskId, nodeId, portId);
        if (ret < 0) {
            return ret;
        }
    }
    if (retrieved_sw->GetNodeType() == UB_SWITCH && MapTaskFind(faultMap, taskId) && packetSize > 0) {
        switch (faultMap[taskId].faultType) {
            case FaultType::DROPPACKET:
                ret = SetPacketDrop(packetSize, faultMap[taskId].dropRate, taskId, nodeId, portId);
                break;
            case FaultType::ADDPACKETDELAY:
                ret = SetPacketDelay(taskId, nodeId, portId);
                break;
            case FaultType::SHUTDOWNUP:
                ret = SetPortShutdownAndUp(packetSize, taskId, nodeId, portId);
                break;
            case FaultType::ERRORPACKET:
                ret = SetErrorPacket(packetSize, faultMap[taskId].erorDropRate, taskId, nodeId, portId);
                break;
            default:
                break;
        }
    }
    return ret;
}

int UbFault::FaultCallback(Ptr<Packet> packet, uint32_t nodeId, uint32_t portId, Ptr<UbPort> ubPort)
{
    int ret = FaultDiagnosis(packet, nodeId, portId, ubPort);
    if (ret >= 0) {
        Simulator::ScheduleNow(&UbPort::TransmitPacket, ubPort, packet, Time(ret));
    } else if (ret < -1) {
        Time delay = NanoSeconds(static_cast<uint64_t>(-ret - 2));
        Simulator::Schedule(Time(0), &UbPort::TransmitComplete, ubPort);
        Simulator::Schedule(delay, &UbPort::TransmitPacketDetached, ubPort, packet);
    } else {
        Simulator::Schedule(Time(0), &UbPort::TransmitComplete, ubPort);
    }
    return 0;
}
}  // namespace ns3
