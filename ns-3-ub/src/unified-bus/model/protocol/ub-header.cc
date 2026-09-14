// SPDX-License-Identifier: GPL-2.0-only
#include "ub-header.h"
#include "ns3/ub-datatype.h"
#include "ns3/log.h"

#include <stdexcept>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbHeader");
NS_OBJECT_ENSURE_REGISTERED(UbCtpHeader);

/*
 ***************************************************
 * UbDatalinkHeader implementation
 ***************************************************
 */

UbDatalinkHeader::UbDatalinkHeader()
    : m_unknown(0),
      m_config(0)
{
}

UbDatalinkHeader::UbDatalinkHeader(uint16_t unknown, uint8_t config)
    : m_unknown(unknown & 0xFFF), // Ensure 12 bits
      m_config(config & 0xF)      // Ensure 4 bits
{
}

UbDatalinkHeader::~UbDatalinkHeader()
{
}

// Setters
void UbDatalinkHeader::SetConfig(uint8_t config)
{
    m_config = config & 0xF; // Ensure 4 bits
}

// Getters
uint8_t UbDatalinkHeader::GetConfig() const
{
    return m_config;
}

bool UbDatalinkHeader::IsControlCreditHeader() const
{
    return m_config == static_cast<uint8_t>(UbDatalinkHeaderConfig::CONTROL);
}

bool UbDatalinkHeader::IsPacketIpv4Header() const
{
    return m_config == static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_IPV4);
}

bool UbDatalinkHeader::IsPacketIpv6Header() const
{
    return m_config == static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_IPV6);
}

bool UbDatalinkHeader::IsPacketUbMemHeader() const
{
    return m_config == static_cast<uint8_t>(UbDatalinkHeaderConfig::PACKET_UB_MEM);
}

TypeId UbDatalinkHeader::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbDatalinkHeader")
                            .SetParent<Header>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbDatalinkHeader>();
    return tid;
}

TypeId UbDatalinkHeader::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void UbDatalinkHeader::Print(std::ostream& os) const
{
}

uint32_t UbDatalinkHeader::GetSerializedSize(void) const
{
    return totalHeaderSize; // 2 bytes
}

void UbDatalinkHeader::Serialize(Buffer::Iterator start) const
{
    // 16 bits total: [Unknown:12][Config:4]
    // Pack into 2 bytes (big-endian)
    uint16_t packed = ((m_unknown & 0xFFF) << 4) | (m_config & 0xF);
    start.WriteHtonU16(packed);
}

uint32_t UbDatalinkHeader::Deserialize(Buffer::Iterator start)
{
    // Read 16 bits and unpack
    uint16_t packed = start.ReadNtohU16();
    m_unknown = (packed >> 4) & 0xFFF; // Extract 12 bits
    m_config = packed & 0xF;           // Extract 4 bits

    return totalHeaderSize;
}

/*
 ***************************************************
 * UbDatalinkControlCreditHeader implementation
 ***************************************************
 */

UbDatalinkControlCreditHeader::UbDatalinkControlCreditHeader()
{
    // Initialize all credit VLs to 0
    IntegerValue val;
    g_ub_priority_num.GetValue(val);
    int ubPriorityNum = val.Get();
    // Initialize all credit VLs to 0
    for (int i = 0; i < ubPriorityNum; ++i) {
        m_creditVL[i] = 0;
    }
}

UbDatalinkControlCreditHeader::UbDatalinkControlCreditHeader(const uint8_t credits[16])
{
    // Copy credit values (ensure 6-bit range)
    IntegerValue val;
    g_ub_priority_num.GetValue(val);
    int ubPriorityNum = val.Get();
    for (int i = 0; i < ubPriorityNum; ++i) {
        m_creditVL[i] = credits[i] & 0x3F; // Mask to 6 bits
    }
}

UbDatalinkControlCreditHeader::~UbDatalinkControlCreditHeader()
{
}

// Setters
void UbDatalinkControlCreditHeader::SetAllCreditsVL(const uint8_t credits[16])
{
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();
    for (int i = 0; i < ubVlNum; ++i) {
        m_creditVL[i] = credits[i] & 0x3F; // Mask to 6 bits
    }
}

void UbDatalinkControlCreditHeader::SetSD(bool sd)
{
    m_sd = sd;
}

void UbDatalinkControlCreditHeader::SetType(bool type)
{
    m_type = type;
}

void UbDatalinkControlCreditHeader::SetAckNumber(uint16_t ackNum)
{
    m_ackNumber = ackNum;
}

// Getters
void UbDatalinkControlCreditHeader::GetAllCreditsVL(uint8_t credits[16]) const
{
    IntegerValue val;
    g_ub_vl_num.GetValue(val);
    int ubVlNum = val.Get();
    for (int i = 0; i < ubVlNum; ++i) {
        credits[i] = m_creditVL[i];
    }
}

uint8_t UbDatalinkControlCreditHeader::GetLength() const
{
    return m_length;
}

uint8_t UbDatalinkControlCreditHeader::GetConfig() const
{
    return m_config;
}

uint8_t UbDatalinkControlCreditHeader::GetControl() const
{
    return m_controlType;
}

uint8_t UbDatalinkControlCreditHeader::GetSubControl() const
{
    return m_subControlType;
}

bool UbDatalinkControlCreditHeader::GetSD() const
{
    return m_sd;
}

bool UbDatalinkControlCreditHeader::GetType() const
{
    return m_type;
}

uint16_t UbDatalinkControlCreditHeader::GetAckNumber() const
{
    return m_ackNumber;
}

TypeId UbDatalinkControlCreditHeader::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbDatalinkControlCreditHeader")
                            .SetParent<Header>()
                            .AddConstructor<UbDatalinkControlCreditHeader>();
    return tid;
}

TypeId UbDatalinkControlCreditHeader::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void UbDatalinkControlCreditHeader::Print(std::ostream& os) const
{
    os << "UbDatalinkControl: length=" << static_cast<uint32_t>(m_length) <<
          " fixed=" << std::hex<< static_cast<uint32_t>(fixedPattern) <<
          std::dec << " config=" << static_cast<uint32_t>(m_config) <<
          " control=" << static_cast<uint32_t>(m_controlType) <<
          " subControl=" << static_cast<uint32_t>(m_subControlType) <<
          " sd=" << m_sd << " type=" << m_type << " ackNum=" << m_ackNumber << " credits=[";
    for (int i = 0; i < 16; ++i) {
        if (i > 0) {
            os << ",";
        }
        os << static_cast<uint32_t>(m_creditVL[i]);
    }
    os << "]";
}

uint32_t UbDatalinkControlCreditHeader::GetSerializedSize(void) const
{
    return totalHeaderSize; // 40 bytes
}

void UbDatalinkControlCreditHeader::Serialize(Buffer::Iterator start) const
{
    // 按照头文件注释中的报文格式进行序列化
    // 字节0: [FirstBit:1][Length:5][Fixed的前2位:2]
    uint8_t byte0 =
        (firstBit ? 0x80 : 0) | ((m_length & 0x1F) << 2) | ((fixedPattern >> 4) & 0x3);
    start.WriteU8(byte0);

    // 字节1: [Fixed的后4位:4][Config:4]
    uint8_t byte1 = ((fixedPattern & 0xF) << 4) | (m_config & 0xF);
    start.WriteU8(byte1);

    // 字节2: [Control:4][Sub Control:4]
    uint8_t byte2 = ((m_controlType & 0xF) << 4) | (m_subControlType & 0xF);
    start.WriteU8(byte2);

    // 字节3: [SD:1][Reserve:6][Type:1]
    uint8_t byte3 = (m_sd ? 0x80 : 0) | ((reservE1Value & 0x3F) << 1) | (m_type ? 0x1 : 0);
    start.WriteU8(byte3);

    // 字节4-5: Ack Number (16 bits)
    start.WriteHtonU16(m_ackNumber);

    // 字节6-17: Credit Number (96 bits = 16 VL * 6 bits each = 12字节)
    // 每4个VL打包成3字节
    for (int group = 0; group < 4; ++group) {
        // 每组处理4个VL (4 * 6 = 24 bits = 3 bytes)
        int vlBase = group * 4;
        uint32_t packed = 0;

        // 将4个6位值打包成24位
        for (int i = 0; i < 4; ++i) {
            uint8_t credit = m_creditVL[vlBase + i] & 0x3F;            // 确保6位
            packed |= (static_cast<uint32_t>(credit) << (18 - i * 6)); // 从高位开始放置
        }

        // 写入3字节
        start.WriteU8((packed >> 16) & 0xFF);
        start.WriteU8((packed >> 8) & 0xFF);
        start.WriteU8(packed & 0xFF);
    }

    // 字节18-39: Reserve字段 (22字节，填充0)
    for (uint32_t i = 0; i < reserveSize; ++i) {
        start.WriteU8(reserveFillValue);
    }
}

uint32_t UbDatalinkControlCreditHeader::Deserialize(Buffer::Iterator start)
{
    // 字节0: [FirstBit:1][Length:5][Fixed的前2位:2]
    uint8_t byte0 = start.ReadU8();
    bool tFirstBit = (byte0 & 0x80) != 0;
    (void)tFirstBit;
    m_length = (byte0 >> 2) & 0x1F;
    uint8_t fixedHigh = byte0 & 0x3;
    (void)fixedHigh;

    // 字节1: [Fixed的后4位:4][Config:4]
    uint8_t byte1 = start.ReadU8();
    uint8_t fixedLow = (byte1 >> 4) & 0xF;
    (void)fixedLow;
    m_config = byte1 & 0xF;

    // 字节2: [Control:4][Sub Control:4]
    uint8_t byte2 = start.ReadU8();
    m_controlType = (byte2 >> 4) & 0xF;
    m_subControlType = byte2 & 0xF;

    // 字节3: [SD:1][Reserve:6][Type:1]
    uint8_t byte3 = start.ReadU8();
    m_sd = (byte3 & 0x80) != 0;
    uint8_t reserve1 = (byte3 >> 1) & 0x3F; // 读取但不存储，因为是固定值
    (void)reserve1;
    m_type = (byte3 & 0x1) != 0;

    // 字节4-5: Ack Number
    m_ackNumber = start.ReadNtohU16();

    // 字节6-17: Credit Number (96 bits = 16 VL * 6 bits = 12字节)
    // 每3字节解出4个VL
    for (int group = 0; group < 4; ++group) {
        // 每组读取3字节，解出4个6位VL值
        uint32_t packed = (static_cast<uint32_t>(start.ReadU8()) << 16) |
                          (static_cast<uint32_t>(start.ReadU8()) << 8) |
                           static_cast<uint32_t>(start.ReadU8());
        int vlBase = group * 4;

        // 从24位中提取4个6位值
        for (int i = 0; i < 4; ++i) {
            m_creditVL[vlBase + i] = (packed >> (18 - i * 6)) & 0x3F;
        }
    }

    // 字节18-39: Reserve字段 (22字节，直接跳过)
    for (uint32_t i = 0; i < reserveSize; ++i) {
        start.ReadU8();
    }

    return totalHeaderSize;
}

/*
 ***************************************************
 * UbDatalinkPacketHeader implementation
 ***************************************************
 */
UbDatalinkPacketHeader::UbDatalinkPacketHeader()
{
}

UbDatalinkPacketHeader::~UbDatalinkPacketHeader()
{
}

// Setters
void UbDatalinkPacketHeader::SetCredit(bool credit)
{
    m_credit = credit;
}

void UbDatalinkPacketHeader::SetACK(bool ack)
{
    m_ack = ack;
}

void UbDatalinkPacketHeader::SetCreditTargetVL(uint8_t vlIndex)
{
    m_creditTargetVL = vlIndex & 0xF; // 4 bits: 0-15
}

void UbDatalinkPacketHeader::SetPacketVL(uint8_t vl)
{
    m_packetVL = vl & 0xF; // 4 bits: 0-15
}

void UbDatalinkPacketHeader::SetConfig(uint8_t config)
{
    m_config = config & 0xF; // Ensure 4 bits
}

void UbDatalinkPacketHeader::SetLoadBalanceMode(bool mode)
{
    m_loadBalanceMode = mode;
}

void UbDatalinkPacketHeader::SetRoutingPolicy(bool policy)
{
    m_routingPolicy = policy;
}

// Getters
bool UbDatalinkPacketHeader::GetCredit() const
{
    return m_credit;
}

bool UbDatalinkPacketHeader::GetACK() const
{
    return m_ack;
}

uint8_t UbDatalinkPacketHeader::GetCreditTargetVL() const
{
    return m_creditTargetVL;
}

uint8_t UbDatalinkPacketHeader::GetPacketVL() const
{
    return m_packetVL;
}

bool UbDatalinkPacketHeader::GetLoadBalanceMode() const
{
    return m_loadBalanceMode;
}

bool UbDatalinkPacketHeader::GetRoutingPolicy() const
{
    return m_routingPolicy;
}

uint8_t UbDatalinkPacketHeader::GetConfig() const
{
    return m_config;
}

bool UbDatalinkPacketHeader::IsUbDatalinkControlCreditHeader()
{
    return m_config == 0x00; // Use literal value instead of CREDIT_CONFIG
}

TypeId UbDatalinkPacketHeader::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbDatalinkPacketHeader")
                            .SetParent<Header>()
                            .AddConstructor<UbDatalinkPacketHeader>();
    return tid;
}

TypeId UbDatalinkPacketHeader::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void UbDatalinkPacketHeader::Print(std::ostream& os) const
{
    os << "UbDatalinkPacket: credit=" << m_credit << " ack=" << m_ack <<
          " creditTargetVL=" << static_cast<uint32_t>(m_creditTargetVL) <<
          " packetVL=" << static_cast<uint32_t>(m_packetVL) <<
          " config=" << static_cast<uint32_t>(m_config) <<
          " loadBalanceMode=" << m_loadBalanceMode <<
          " routingPolicy=" << m_routingPolicy;
}

uint32_t UbDatalinkPacketHeader::GetSerializedSize(void) const
{
    return totalHeaderSize; // 4 bytes
}

void UbDatalinkPacketHeader::Serialize(Buffer::Iterator start) const
{
    // 字节0: [Credit:1][ACK:1][Credit Target VL:4][Reserve1(fixed 0):1][PacketVL高1位:1]
    uint8_t byte0 = (m_credit ? 0x80 : 0) | (m_ack ? 0x40 : 0) | ((m_creditTargetVL & 0xF) << 2) |
                    ((reservE1Value & 0x1) << 1) | ((m_packetVL >> 3) & 0x1);
    start.WriteU8(byte0);

    // 字节1: [PacketVL低3位:3][Reserve2(fixed 0):1][Config(fixed 0011):4]
    uint8_t byte1 = ((m_packetVL & 0x7) << 5) | ((reservE2Value & 0x1) << 4) | (m_config & 0xF);
    start.WriteU8(byte1);

    // 字节2: [Load Balance Mode:1][Routing Policy:1][Packet Length in block (ignored):4][Last Block
    // Length高2位:2]
    uint8_t byte2 = (m_loadBalanceMode ? 0x80 : 0) | (m_routingPolicy ? 0x40 : 0) |
                    ((ignoredFieldValue & 0xF) << 2) | ((ignoredFieldValue & 0x3));
    start.WriteU8(byte2);

    // 字节3: [Last Block Length低3位:3][Tail Payload Length:5]
    uint8_t byte3 = ((ignoredFieldValue & 0x7) << 5) | (ignoredFieldValue & 0x1F);
    start.WriteU8(byte3);
}

uint32_t UbDatalinkPacketHeader::Deserialize(Buffer::Iterator start)
{
    // 字节0: [Credit:1][ACK:1][Credit Target VL:4][Reserve1:1][PacketVL高1位:1]
    uint8_t byte0 = start.ReadU8();
    m_credit = (byte0 & 0x80) != 0;
    m_ack = (byte0 & 0x40) != 0;
    m_creditTargetVL = (byte0 >> 2) & 0xF;
    uint8_t reserve1 = (byte0 >> 1) & 0x1; // 读取但不存储
    (void)reserve1;
    uint8_t packetVLHigh = byte0 & 0x1;

    // 字节1: [PacketVL低3位:3][Reserve2:1][Config:4]
    uint8_t byte1 = start.ReadU8();
    uint8_t packetVLLow = (byte1 >> 5) & 0x7;
    uint8_t reserve2 = (byte1 >> 4) & 0x1; // 读取但不存储
    (void)reserve2;
    uint8_t config = byte1 & 0xF;

    // 重构PacketVL (4位)
    m_packetVL = (packetVLHigh << 3) | packetVLLow;

    // 验证配置项
    if (config != uint8_t(UbDatalinkHeaderConfig::PACKET_IPV4) &&
        config != uint8_t(UbDatalinkHeaderConfig::PACKET_UB_MEM)) {
        NS_LOG_WARN("Invalid config value in UbDatalinkPacketHeader: got "
                    << static_cast<uint32_t>(config));
    }
    m_config = config;

    // 字节2: [Load Balance Mode:1][Routing Policy:1][Packet Length in block (ignored):4][Last Block
    // Length高2位:2]
    uint8_t byte2 = start.ReadU8();
    m_loadBalanceMode = (byte2 & 0x80) != 0;
    m_routingPolicy = (byte2 & 0x40) != 0;
    // 忽略的字段就不读取了

    // 字节3: [Last Block Length低3位:3][Tail Payload Length:5] - 完全忽略
    uint8_t byte3 = start.ReadU8();
    (void)byte3;
    return totalHeaderSize;
}

/*
 ***************************************************
 * UbIpBasedNetworkHeader implementation
 ***************************************************
 */

UbIpBasedNetworkHeader::UbIpBasedNetworkHeader()
{
    m_fields.raw13 = 0;
}

UbIpBasedNetworkHeader::~UbIpBasedNetworkHeader()
{
}

TypeId UbIpBasedNetworkHeader::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbIpBasedNetworkHeader")
                            .SetParent<Header>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbIpBasedNetworkHeader>();
    return tid;
}

TypeId UbIpBasedNetworkHeader::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void UbIpBasedNetworkHeader::Print(std::ostream& os) const
{
    os << "UbIpBasedNetworkHeader: "
       << "Mode=" << static_cast<uint32_t>(m_mode) << ", NPI=" << std::hex << m_npi << std::dec;

    switch (m_mode) {
        case 0: // mode0
            os << ", Location=" << m_fields.mode0.location <<
            ", Enable=" << m_fields.mode0.enable << ", C=" <<
            m_fields.mode0.c << ", I=" << m_fields.mode0.i <<
            ", Hint=" << static_cast<uint32_t>(m_fields.mode0.hint);
            break;
        case 2: // mode2
            os << ", Location=" << m_fields.mode2.location <<
            ", Timestamp=" << m_fields.mode2.timestamp <<
            ", FECN=" << static_cast<uint32_t>(m_fields.mode2.fecn);
            break;
        case 4: // mode4
            os << ", Location=" << m_fields.mode4.location <<
            ", FECN=" << static_cast<uint32_t>(m_fields.mode4.fecn);
            break;
        default:
            os << ", Raw13=" << std::hex << m_fields.raw13 << std::dec;
            break;
    }
}

void UbIpBasedNetworkHeader::Serialize(Buffer::Iterator start) const
{
    // 字节0-1: [Mode:3][Fields:13]
    uint16_t byte01 = ((static_cast<uint16_t>(m_mode & 0x07) << 13)) | (m_fields.raw13 & 0x1FFF);
    start.WriteHtonU16(byte01);

    // 字节2: reserved (7 bits) + NPI最高位 (1 bit)
    uint8_t byte2 = (m_reserved & 0x7F) | ((m_npi >> 24) & 0x01);
    start.WriteU8(byte2);

    // 字节3-5: NPI的低24位 (24 bits)
    start.WriteU8((m_npi >> 16) & 0xFF);
    start.WriteU8((m_npi >> 8) & 0xFF);
    start.WriteU8(m_npi & 0xFF);
}

uint32_t UbIpBasedNetworkHeader::Deserialize(Buffer::Iterator start)
{
    // 字节0-1: [Mode:3][Fields:13]
    uint16_t byte01 = start.ReadNtohU16();
    m_mode = (byte01 >> 13) & 0x07;
    m_fields.raw13 = byte01 & 0x1FFF;

    // 字节2: reserved (7 bits) + NPI最高位 (1 bit)
    uint8_t byte2 = start.ReadU8();
    m_reserved = (byte2 >> 1) & 0x7F;
    uint32_t npiHigh = byte2 & 0x01;

    // 字节3-5: NPI的低24位 (24 bits)
    m_npi = (npiHigh << 24) |
            (static_cast<uint32_t>(start.ReadU8()) << 16) |
            (static_cast<uint32_t>(start.ReadU8()) << 8) |
            static_cast<uint32_t>(start.ReadU8());
    return totalHeaderSize;
}

uint32_t UbIpBasedNetworkHeader::GetSerializedSize(void) const
{
    return totalHeaderSize;
}

// Setters
void UbIpBasedNetworkHeader::SetMode(uint8_t mode)
{
    m_mode = mode & 0x07; // 确保只有3位
    // 清空raw13，准备设置新的mode字段
    m_fields.raw13 = 0;
}

void UbIpBasedNetworkHeader::SetLocation(bool location)
{
    if (m_mode == 0 || m_mode == 2 || m_mode == 4) {
        if (location) {
            m_fields.raw13 |= (1 << 12); // 设置第12位
        } else {
            m_fields.raw13 &= ~(1 << 12); // 清除第12位
        }
    }
}

void UbIpBasedNetworkHeader::SetEnable(bool enable)
{
    if (m_mode == 0) {
        if (enable) {
            m_fields.raw13 |= (1 << 10); // 设置第10位
        } else {
            m_fields.raw13 &= ~(1 << 10); // 清除第10位
        }
    }
}

void UbIpBasedNetworkHeader::SetC(uint8_t c)
{
    if (m_mode == 0) {
        if (c) {
            m_fields.raw13 |= (1 << 9); // 设置第9位
        } else {
            m_fields.raw13 &= ~(1 << 9); // 清除第9位
        }
    }
}

void UbIpBasedNetworkHeader::SetI(uint8_t i)
{
    if (m_mode == 0) {
        if (i) {
            m_fields.raw13 |= (1 << 8); // 设置第8位
        } else {
            m_fields.raw13 &= ~(1 << 8); // 清除第8位
        }
    }
}

void UbIpBasedNetworkHeader::SetHint(uint8_t hint)
{
    if (m_mode == 0) {
        m_fields.raw13 &= ~0xFF;         // 清除低7位
        m_fields.raw13 |= (hint & 0xFF); // 设置低7位
    }
}

void UbIpBasedNetworkHeader::SetTimeStamp(uint16_t ts)
{
    if (m_mode == 2) {
        m_fields.raw13 &= ~(0x3FF << 2);       // 清除第2-11位
        m_fields.raw13 |= ((ts & 0x3FF) << 2); // 设置第2-11位
    }
}

void UbIpBasedNetworkHeader::SetFecn(uint8_t fecn)
{
    if (m_mode == 2 || m_mode == 4) {
        m_fields.raw13 &= ~0x03;         // 清除低2位
        m_fields.raw13 |= (fecn & 0x03); // 设置低2位
    }
}


// Getters - 直接从raw13读取
uint8_t UbIpBasedNetworkHeader::GetMode() const
{
    return m_mode;
}

bool UbIpBasedNetworkHeader::GetLocation() const
{
    if (m_mode == 0 || m_mode == 2 || m_mode == 4) {
        return (m_fields.raw13 & (1 << 12)) != 0;
    }
    return false;
}

bool UbIpBasedNetworkHeader::GetEnable() const
{
    if (m_mode == 0) {
        return (m_fields.raw13 & (1 << 10)) != 0;
    }
    return false;
}

uint8_t UbIpBasedNetworkHeader::GetC() const
{
    if (m_mode == 0) {
        return (m_fields.raw13 & (1 << 9)) != 0;
    }
    return 0;
}

uint8_t UbIpBasedNetworkHeader::GetI() const
{
    if (m_mode == 0) {
        return (m_fields.raw13 & (1 << 8)) != 0;
    }
    return 0;
}

uint8_t UbIpBasedNetworkHeader::GetHint() const
{
    if (m_mode == 0) {
        return m_fields.raw13 & 0xFF; // 提取低8位
    }
    return 0;
}

uint16_t UbIpBasedNetworkHeader::GetTimeStamp() const
{
    if (m_mode == 2) {
        return (m_fields.raw13 >> 2) & 0x3FF; // 提取第2-11位
    }
    return 0;
}

uint8_t UbIpBasedNetworkHeader::GetFecn() const
{
    if (m_mode == 2 || m_mode == 4) {
        return m_fields.raw13 & 0x03; // 提取低2位
    }
    return 0;
}

/*
 ***************************************************
 * UbCtpHeader implementation
 ***************************************************
 */
UbCtpHeader::UbCtpHeader() = default;

UbCtpHeader::~UbCtpHeader() = default;

TypeId
UbCtpHeader::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbCtpHeader")
                            .SetParent<Header>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbCtpHeader>();
    return tid;
}

TypeId
UbCtpHeader::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void
UbCtpHeader::Print(std::ostream& os) const
{
    os << "CTPH opcode=" << static_cast<uint32_t>(m_tpOpcode)
       << " padding=" << static_cast<uint32_t>(m_padding)
       << " nlp=" << static_cast<uint32_t>(m_nlp);
}

uint32_t
UbCtpHeader::GetSerializedSize(void) const
{
    return 1;
}

void
UbCtpHeader::Serialize(Buffer::Iterator start) const
{
    start.WriteU8(static_cast<uint8_t>(((m_tpOpcode & 0x3) << 6) |
                                       ((m_padding & 0x3) << 4) |
                                       (m_nlp & 0xF)));
}

uint32_t
UbCtpHeader::Deserialize(Buffer::Iterator start)
{
    const uint8_t packed = start.ReadU8();
    m_tpOpcode = (packed >> 6) & 0x3;
    m_padding = (packed >> 4) & 0x3;
    m_nlp = packed & 0xF;
    return GetSerializedSize();
}

void
UbCtpHeader::SetTPOpcode(uint8_t opcode)
{
    NS_ABORT_MSG_IF(opcode > 0x3, "CTPH TPOpcode must fit in 2 bits");
    m_tpOpcode = opcode;
}

void
UbCtpHeader::SetTPOpcode(CtpOpcode opcode)
{
    SetTPOpcode(static_cast<uint8_t>(opcode));
}

void
UbCtpHeader::SetPadding(uint8_t padding)
{
    NS_ABORT_MSG_IF(padding > 0x3, "CTPH padding must fit in 2 bits");
    m_padding = padding;
}

void
UbCtpHeader::SetNlp(uint8_t nlp)
{
    NS_ABORT_MSG_IF(nlp > 0xF, "CTPH NLP must fit in 4 bits");
    m_nlp = nlp;
}

uint8_t
UbCtpHeader::GetTPOpcode() const
{
    return m_tpOpcode;
}

uint8_t
UbCtpHeader::GetPadding() const
{
    return m_padding;
}

uint8_t
UbCtpHeader::GetNlp() const
{
    return m_nlp;
}

/*
 ***************************************************
 * UbCompactUpiHeader implementation
 ***************************************************
 */
UbCompactUpiHeader::UbCompactUpiHeader() = default;

UbCompactUpiHeader::~UbCompactUpiHeader() = default;

TypeId
UbCompactUpiHeader::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbCompactUpiHeader")
                            .SetParent<Header>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbCompactUpiHeader>();
    return tid;
}

TypeId
UbCompactUpiHeader::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void
UbCompactUpiHeader::Print(std::ostream& os) const
{
    os << "CompactUPIH upi=" << m_upi;
}

uint32_t
UbCompactUpiHeader::GetSerializedSize(void) const
{
    return 2;
}

void
UbCompactUpiHeader::Serialize(Buffer::Iterator start) const
{
    NS_ABORT_MSG_IF(m_upi > UB_COMPACT_UPI_MAX, "Compact UPI must fit in 15 bits");
    start.WriteHtonU16(m_upi);
}

uint32_t
UbCompactUpiHeader::Deserialize(Buffer::Iterator start)
{
    m_upi = start.ReadNtohU16() & UB_COMPACT_UPI_MAX;
    return GetSerializedSize();
}

void
UbCompactUpiHeader::SetUpi(uint16_t upi)
{
    NS_ABORT_MSG_IF(upi > UB_COMPACT_UPI_MAX, "Compact UPI must fit in 15 bits");
    m_upi = upi;
}

uint16_t
UbCompactUpiHeader::GetUpi() const
{
    return m_upi;
}

/*
 ***************************************************
 * UbCompactEidHeader implementation
 ***************************************************
 */
UbCompactEidHeader::UbCompactEidHeader() = default;

UbCompactEidHeader::~UbCompactEidHeader() = default;

TypeId
UbCompactEidHeader::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbCompactEidHeader")
                            .SetParent<Header>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbCompactEidHeader>();
    return tid;
}

TypeId
UbCompactEidHeader::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void
UbCompactEidHeader::Print(std::ostream& os) const
{
    os << "CompactEIDH seid=" << m_sourceEid << " deid=" << m_destinationEid;
}

uint32_t
UbCompactEidHeader::GetSerializedSize(void) const
{
    return 5;
}

void
UbCompactEidHeader::Serialize(Buffer::Iterator start) const
{
    NS_ABORT_MSG_IF(m_sourceEid > UB_COMPACT_EID_MAX, "Compact SEID must fit in 20 bits");
    NS_ABORT_MSG_IF(m_destinationEid > UB_COMPACT_EID_MAX, "Compact DEID must fit in 20 bits");

    const uint64_t packed = (static_cast<uint64_t>(m_sourceEid & UB_COMPACT_EID_MAX) << 20) |
                            static_cast<uint64_t>(m_destinationEid & UB_COMPACT_EID_MAX);
    start.WriteU8(static_cast<uint8_t>((packed >> 32) & 0xff));
    start.WriteU8(static_cast<uint8_t>((packed >> 24) & 0xff));
    start.WriteU8(static_cast<uint8_t>((packed >> 16) & 0xff));
    start.WriteU8(static_cast<uint8_t>((packed >> 8) & 0xff));
    start.WriteU8(static_cast<uint8_t>(packed & 0xff));
}

uint32_t
UbCompactEidHeader::Deserialize(Buffer::Iterator start)
{
    uint64_t packed = 0;
    packed |= static_cast<uint64_t>(start.ReadU8()) << 32;
    packed |= static_cast<uint64_t>(start.ReadU8()) << 24;
    packed |= static_cast<uint64_t>(start.ReadU8()) << 16;
    packed |= static_cast<uint64_t>(start.ReadU8()) << 8;
    packed |= static_cast<uint64_t>(start.ReadU8());
    m_sourceEid = static_cast<uint32_t>((packed >> 20) & UB_COMPACT_EID_MAX);
    m_destinationEid = static_cast<uint32_t>(packed & UB_COMPACT_EID_MAX);
    return GetSerializedSize();
}

void
UbCompactEidHeader::SetSourceEid(uint32_t eid)
{
    NS_ABORT_MSG_IF(eid > UB_COMPACT_EID_MAX, "Compact SEID must fit in 20 bits");
    m_sourceEid = eid;
}

void
UbCompactEidHeader::SetDestinationEid(uint32_t eid)
{
    NS_ABORT_MSG_IF(eid > UB_COMPACT_EID_MAX, "Compact DEID must fit in 20 bits");
    m_destinationEid = eid;
}

uint32_t
UbCompactEidHeader::GetSourceEid() const
{
    return m_sourceEid;
}

uint32_t
UbCompactEidHeader::GetDestinationEid() const
{
    return m_destinationEid;
}

/*
 ***************************************************
 * UbTransportHeader implementation
 ***************************************************
 */
UbTransportHeader::UbTransportHeader()
{
}

UbTransportHeader::~UbTransportHeader()
{
}

TypeId UbTransportHeader::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbTransportHeader").SetParent<Header>().AddConstructor<UbTransportHeader>();
    return tid;
}

TypeId UbTransportHeader::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void UbTransportHeader::Print(std::ostream& os) const
{
    os << "UbTransport: opcode=" << static_cast<uint32_t>(m_tpOpcode) <<
          " ver=" << static_cast<uint32_t>(m_tpVer) << " nlp=" <<
          static_cast<uint32_t>(m_nlp) << " srcTpn=" << m_srcTpn <<
          " destTpn=" << m_destTpn << " ackReq=" << m_ackRequest <<
          " errFlag=" << m_errorFlag <<
          " psn=" << m_psn << " rspSt=" << static_cast<uint32_t>(m_rspSt) << " rspInfo=" <<
          static_cast<uint32_t>(m_rspInfo) << " tpMsn=" << m_tpMsn;
}

uint32_t UbTransportHeader::GetSerializedSize(void) const
{
    return 16; // Total 16 bytes as per header format
}

void UbTransportHeader::Serialize(Buffer::Iterator start) const
{
    // First 64 bits: [Last packet:1][TPOpcode:7][TPVer:2][Pad:2][NLP:4][SrcTPN:24][DestTPN:24]
    uint8_t byte0 = (m_lastPacket ? 0x80 : 0) | (m_tpOpcode & 0x7F);
    start.WriteU8(byte0);

    // Pack TPVer(2) + Pad(2) + NLP(4) into one byte
    uint8_t byte1 = ((m_tpVer & 0x3) << 6) | ((m_nlp & 0xF));
    start.WriteU8(byte1);

    // Write SrcTPN (24 bits) and DestTPN (24 bits) - use 3 bytes each
    start.WriteU8((m_srcTpn >> 16) & 0xFF);
    start.WriteU8((m_srcTpn >> 8) & 0xFF);
    start.WriteU8(m_srcTpn & 0xFF);

    start.WriteU8((m_destTpn >> 16) & 0xFF);
    start.WriteU8((m_destTpn >> 8) & 0xFF);
    start.WriteU8(m_destTpn & 0xFF);

    // Second 32 bits: [Ack request:1][Error Flag:1][reserved:6][PSN:24]
    uint8_t byte8 = (m_ackRequest ? 0x80 : 0) | (m_errorFlag ? 0x40 : 0) |
                    ((m_reserve1 & 0x3F) << 0); 
    start.WriteU8(byte8);

    // Write PSN (24 bits) - use 3 bytes
    start.WriteU8((m_psn >> 16) & 0xFF);
    start.WriteU8((m_psn >> 8) & 0xFF);
    start.WriteU8(m_psn & 0xFF);

    // Third 32 bits: [RSPST:3][RSPINFO:5][TPMSN:24]
    uint8_t byte12 = ((m_rspSt & 0x7) << 5) | (m_rspInfo & 0x1F);
    start.WriteU8(byte12);

    // Write TPMSN (24 bits) - use 3 bytes
    start.WriteU8((m_tpMsn >> 16) & 0xFF);
    start.WriteU8((m_tpMsn >> 8) & 0xFF);
    start.WriteU8(m_tpMsn & 0xFF);
}

uint32_t UbTransportHeader::Deserialize(Buffer::Iterator start)
{
    // First 64 bits: [Last packet:1][TPOpcode:7][TPVer:2][Pad:2][NLP:4][SrcTPN:24][DestTPN:24]
    uint8_t byte0 = start.ReadU8();
    m_lastPacket = (byte0 & 0x80) != 0;
    m_tpOpcode = byte0 & 0x7F;

    // Unpack TPVer(2) + Pad(2) + NLP(4) from one byte
    uint8_t byte1 = start.ReadU8();
    m_tpVer = (byte1 >> 6) & 0x3;
    m_nlp = byte1 & 0xF;

    // Read SrcTPN (24 bits) and DestTPN (24 bits)
    m_srcTpn = (static_cast<uint32_t>(start.ReadU8()) << 16) |
               (static_cast<uint32_t>(start.ReadU8()) << 8) | static_cast<uint32_t>(start.ReadU8());
    m_destTpn = (static_cast<uint32_t>(start.ReadU8()) << 16) |
                (static_cast<uint32_t>(start.ReadU8()) << 8) |
                static_cast<uint32_t>(start.ReadU8());

    // Second 32 bits: [Ack request:1][Error Flag:1][reserved:2][Message number:4][PSN:24]
    uint8_t byte8 = start.ReadU8();
    m_ackRequest = (byte8 & 0x80) != 0;
    m_errorFlag = (byte8 & 0x40) != 0;
    m_reserve1 = byte8 & 0x3F;

    // Read PSN (24 bits)
    m_psn = (static_cast<uint32_t>(start.ReadU8()) << 16) |
            (static_cast<uint32_t>(start.ReadU8()) << 8) | static_cast<uint32_t>(start.ReadU8());

    // Third 32 bits: [RSPST:3][RSPINFO:5][TPMSN:24]
    uint8_t byte12 = start.ReadU8();
    m_rspSt = (byte12 >> 5) & 0x7;
    m_rspInfo = byte12 & 0x1F;

    // Read TPMSN (24 bits)
    m_tpMsn = (static_cast<uint32_t>(start.ReadU8()) << 16) |
              (static_cast<uint32_t>(start.ReadU8()) << 8) | static_cast<uint32_t>(start.ReadU8());

    return 16;
}

// Setters
void UbTransportHeader::SetLastPacket(bool lastPkt)
{
    m_lastPacket = lastPkt;
}

void UbTransportHeader::SetTPOpcode(TpOpcode opcode)
{
    m_tpOpcode = static_cast<uint8_t>(opcode);
}

void UbTransportHeader::SetTPOpcode(uint8_t opcode)
{
    m_tpOpcode = opcode;
}

void UbTransportHeader::SetNLP(NextLayerProtocol nlp)
{
    m_nlp = static_cast<uint8_t>(nlp) & 0xF; // Only 4 bits
}

void UbTransportHeader::SetNLP(uint8_t nlp)
{
    m_nlp = nlp & 0xF; // Only 4 bits
}

void UbTransportHeader::SetSrcTpn(uint32_t srcTpn)
{
    m_srcTpn = srcTpn & 0xFFFFFF; // Only 24 bits
}

void UbTransportHeader::SetDestTpn(uint32_t destTpn)
{
    m_destTpn = destTpn & 0xFFFFFF; // Only 24 bits
}

void UbTransportHeader::SetAckRequest(bool ackRequest)
{
    m_ackRequest = ackRequest;
}

void UbTransportHeader::SetErrorFlag(bool errorFlag)
{
    m_errorFlag = errorFlag;
}

void UbTransportHeader::SetPsn(uint32_t psn)
{
    m_psn = psn & 0xFFFFFF; // Only 24 bits
}

void UbTransportHeader::SetTpMsn(uint32_t tpMsn)
{
    m_tpMsn = tpMsn & 0xFFFFFF; // Only 24 bits
}

void UbTransportHeader::SetRspSt(uint8_t rspSt)
{
    m_rspSt = rspSt & 0x7;
}

void UbTransportHeader::SetRspInfo(uint8_t rspInfo)
{
    m_rspInfo = rspInfo & 0x1F;
}

// Getters
bool UbTransportHeader::GetLastPacket() const
{
    return m_lastPacket;
}

uint8_t UbTransportHeader::GetTPOpcode() const
{
    return m_tpOpcode;
}

uint8_t UbTransportHeader::GetNLP() const
{
    return m_nlp;
}

uint32_t UbTransportHeader::GetSrcTpn() const
{
    return m_srcTpn;
}

uint32_t UbTransportHeader::GetDestTpn() const
{
    return m_destTpn;
}

bool UbTransportHeader::GetAckRequest() const
{
    return m_ackRequest;
}

bool UbTransportHeader::GetErrorFlag() const
{
    return m_errorFlag;
}

uint32_t UbTransportHeader::GetPsn() const
{
    return m_psn;
}

uint32_t UbTransportHeader::GetTpMsn() const
{
    return m_tpMsn;
}

uint8_t UbTransportHeader::GetRspSt() const
{
    return m_rspSt;
}

uint8_t UbTransportHeader::GetRspInfo() const
{
    return m_rspInfo;
}

// Check validity methods
bool UbTransportHeader::IsValidOpcode() const
{
    return m_tpOpcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_UNRELIABLE_TA) ||
           m_tpOpcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_RELIABLE_TA) ||
           m_tpOpcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH) ||
           m_tpOpcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_ACK_WITH_CETPH) ||
           m_tpOpcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_SACK_WITHOUT_CETPH) ||
           m_tpOpcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_SACK_WITH_CETPH) ||
           m_tpOpcode == static_cast<uint8_t>(TpOpcode::TP_OPCODE_CNP);
}

bool UbTransportHeader::IsValidNLP() const
{
    return m_nlp <= static_cast<uint8_t>(NextLayerProtocol::NLP_CIP);
}

/*
 ***************************************************
 * UbSelectiveAckExtTph implementation
 ***************************************************
 */

UbSelectiveAckExtTph::UbSelectiveAckExtTph() = default;

UbSelectiveAckExtTph::~UbSelectiveAckExtTph() = default;

TypeId
UbSelectiveAckExtTph::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbSelectiveAckExtTph")
                            .SetParent<Header>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbSelectiveAckExtTph>();
    return tid;
}

TypeId
UbSelectiveAckExtTph::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void
UbSelectiveAckExtTph::Print(std::ostream& os) const
{
    os << "UbSelectiveAckExtTph: bitmapBits=" << GetBitmapBitCount()
       << " maxRcvPsn=" << m_maxRcvPsn;
}

uint32_t
UbSelectiveAckExtTph::GetSerializedSize(void) const
{
    return kFixedHeaderBytes + (GetBitmapBitCount() / 8);
}

void
UbSelectiveAckExtTph::Serialize(Buffer::Iterator start) const
{
    Buffer::Iterator i = start;
    i.WriteU8(m_bitmapSizeEncoded & 0x7);
    i.WriteU8((m_maxRcvPsn >> 16) & 0xFF);
    i.WriteU8((m_maxRcvPsn >> 8) & 0xFF);
    i.WriteU8(m_maxRcvPsn & 0xFF);

    const uint32_t bitmapBytes = GetBitmapBitCount() / 8;
    for (uint32_t index = 0; index < bitmapBytes; ++index)
    {
        i.WriteU8(m_bitmap[index]);
    }
}

uint32_t
UbSelectiveAckExtTph::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    const uint8_t sizeByte = i.ReadU8();
    if ((sizeByte & 0xF8) != 0)
    {
        throw std::invalid_argument("reserved SAETPH bitmap size high bits");
    }

    m_bitmapSizeEncoded = sizeByte & 0x7;
    if (!IsSupportedEncodedBitmapSize(m_bitmapSizeEncoded))
    {
        throw std::invalid_argument("reserved SAETPH bitmap size encoding");
    }

    m_maxRcvPsn = (static_cast<uint32_t>(i.ReadU8()) << 16) |
                  (static_cast<uint32_t>(i.ReadU8()) << 8) |
                  static_cast<uint32_t>(i.ReadU8());
    m_bitmap.fill(0);

    const uint32_t bitmapBytes = GetBitmapBitCount() / 8;
    for (uint32_t index = 0; index < bitmapBytes; ++index)
    {
        m_bitmap[index] = i.ReadU8();
    }

    return GetSerializedSize();
}

void
UbSelectiveAckExtTph::SetBitmapBitCount(uint32_t bitCount)
{
    uint8_t encoded = 0;
    if (!EncodeBitmapBitCount(bitCount, encoded))
    {
        throw std::invalid_argument("unsupported SAETPH bitmap width");
    }

    m_bitmapSizeEncoded = encoded;
    m_bitmap.fill(0);
}

uint32_t
UbSelectiveAckExtTph::GetBitmapBitCount() const
{
    return DecodeBitmapBitCount(m_bitmapSizeEncoded);
}

uint8_t
UbSelectiveAckExtTph::GetEncodedBitmapSize() const
{
    return m_bitmapSizeEncoded;
}

void
UbSelectiveAckExtTph::SetMaxRcvPsn(uint32_t psn)
{
    m_maxRcvPsn = psn & 0xFFFFFF;
}

uint32_t
UbSelectiveAckExtTph::GetMaxRcvPsn() const
{
    return m_maxRcvPsn;
}

void
UbSelectiveAckExtTph::SetBitmapBit(uint32_t offset, bool value)
{
    if (offset >= GetBitmapBitCount())
    {
        return;
    }

    const uint32_t byteIndex = offset / 8;
    const uint8_t bitMask = static_cast<uint8_t>(1U << (offset % 8));
    if (value)
    {
        m_bitmap[byteIndex] |= bitMask;
    }
    else
    {
        m_bitmap[byteIndex] &= static_cast<uint8_t>(~bitMask);
    }
}

bool
UbSelectiveAckExtTph::GetBitmapBit(uint32_t offset) const
{
    if (offset >= GetBitmapBitCount())
    {
        return false;
    }

    const uint32_t byteIndex = offset / 8;
    const uint8_t bitMask = static_cast<uint8_t>(1U << (offset % 8));
    return (m_bitmap[byteIndex] & bitMask) != 0;
}

bool
UbSelectiveAckExtTph::IsSupportedBitmapBitCount(uint32_t bitCount)
{
    uint8_t encoded = 0;
    return EncodeBitmapBitCount(bitCount, encoded);
}

bool
UbSelectiveAckExtTph::IsSupportedEncodedBitmapSize(uint8_t encoded)
{
    return encoded <= 4;
}

uint32_t
UbSelectiveAckExtTph::DecodeBitmapBitCount(uint8_t encoded)
{
    switch (encoded)
    {
    case 0:
        return 64;
    case 1:
        return 128;
    case 2:
        return 256;
    case 3:
        return 512;
    case 4:
        return 1024;
    default:
        return 0;
    }
}

bool
UbSelectiveAckExtTph::EncodeBitmapBitCount(uint32_t bitCount, uint8_t& encoded)
{
    switch (bitCount)
    {
    case 64:
        encoded = 0;
        return true;
    case 128:
        encoded = 1;
        return true;
    case 256:
        encoded = 2;
        return true;
    case 512:
        encoded = 3;
        return true;
    case 1024:
        encoded = 4;
        return true;
    default:
        return false;
    }
}

/*
 ***************************************************
 * UbTransactionHeader implementation
 ***************************************************
 */
UbTransactionHeader::UbTransactionHeader()
{
}

UbTransactionHeader::~UbTransactionHeader()
{
}

TypeId UbTransactionHeader::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbTransactionHeader")
                            .SetParent<Header>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbTransactionHeader>();
    return tid;
}

TypeId UbTransactionHeader::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void UbTransactionHeader::Print(std::ostream& os) const
{
    os << "UbTransactionHeader: "
       << "TaOpcode=0x" << std::hex << (uint32_t)m_taOpcode << " SrcTaSsn=" << std::dec
       << m_iniTaSsn << " Order=" << (uint32_t)m_order
       << " Exclusive=" << m_exclusive << " IniRcType=" << (uint32_t)m_iniRcIdType
       << " IniRcId(jetty num)=" << std::dec << m_iniRcIdId;
}

uint32_t UbTransactionHeader::GetSerializedSize(void) const
{
    return totalHeaderSize;
}

void UbTransactionHeader::Serialize(Buffer::Iterator start) const
{
    // 字节0: [TaOpcode:8]
    start.WriteU8(m_taOpcode);

    // 字节1: [TaVer(fixed 0x00):2][EE:2][TV_EN:1][POISON:1][reserved:1][UD_FLG:1]
    uint8_t byte1 = ((m_taVer & 0x3) << 6) | ((m_ee & 0x3) << 4) | (m_tvEn ? 0x08 : 0) |
                    (m_poison ? 0x04 : 0) | ((m_reserveByte1 & 0x1) << 1) | (m_udFlg ? 0x01 : 0);
    start.WriteU8(byte1);

    // 字节2-3: [Initiator TaSsn:16]
    start.WriteHtonU16(m_iniTaSsn);

    // 字节4: [noTAACK:1][Order:3][MT_EN en:1][FCE:1][Retry:1][Alloc:1]
    uint8_t byte4 = (m_noTaAck ? 0x80 : 0) | ((m_order & 0x7) << 4) | (m_mtEn ? 0x08 : 0) | (m_fce ? 0x04 : 0) |
                    (m_retry ? 0x02 : 0) | (m_alloc ? 0x01 : 0);
    start.WriteU8(byte4);

    // 字节5: [Reserve(fixed 0):1][Exclusive:1][Src Jetty type:2][Src Jetty高4位:4]
    uint8_t byte5 = ((m_reserveByte5 & 0x1) << 7) | (m_exclusive ? 0x40 : 0) |
                    ((m_iniRcIdType & 0x3) << 4) | ((m_iniRcIdId >> 16) & 0xF);
    start.WriteU8(byte5);

    // 字节6-7: [Src Jetty低16位:16]
    start.WriteHtonU16(m_iniRcIdId & 0xFFFF);
}

uint32_t UbTransactionHeader::Deserialize(Buffer::Iterator start)
{
    // 字节0: [TaOpcode:8]
    m_taOpcode = start.ReadU8();

    // 字节1: [TaVer(fixed 0x00):2][EE:2][TV_EN:1][POISON:1][reserved:1][UD_FLG:1]
    uint8_t byte1 = start.ReadU8();
    m_taVer = (byte1 >> 6) & 0x3;
    m_ee = (byte1 >> 4) & 0x3;
    m_tvEn = (byte1 & 0x08) != 0;
    m_poison = (byte1 & 0x04) != 0;
    m_reserveByte1 = (byte1 >> 1) & 0x1;
    m_udFlg = (byte1 & 0x01) != 0;

    // 字节2-3: [Initiator TaSsn:16]
    m_iniTaSsn = start.ReadNtohU16();

    // 字节4: [noTAACK:1][Order:3][MT_EN en:1][FCE:1][Retry:1][Alloc:1]
    uint8_t byte4 = start.ReadU8();
    m_noTaAck = (byte4 & 0x80) != 0;
    m_order = (byte4 >> 4) & 0x7;
    m_mtEn = (byte4 & 0x08) != 0;
    m_fce = (byte4 & 0x04) != 0;
    m_retry = (byte4 & 0x02) != 0;
    m_alloc = (byte4 & 0x01) != 0;

    // 字节5: [Reserve(fixed 0):1][Exclusive:1][Src Jetty type:2][Src Jetty高4位:4]
    uint8_t byte5 = start.ReadU8();
    m_reserveByte5 = (byte5 >> 7) & 0x1;
    m_exclusive = (byte5 & 0x40) != 0;
    m_iniRcIdType = (byte5 >> 4) & 0x3;
    uint32_t jettyHigh4 = byte5 & 0xF;

    // 字节6-7: [Src Jetty低16位:16]
    uint16_t jettyLow16 = start.ReadNtohU16();

    // 重构完整的20位Jetty
    m_iniRcIdId = (jettyHigh4 << 16) | jettyLow16;

    return totalHeaderSize;
}

// Setters
void UbTransactionHeader::SetTaOpcode(TaOpcode opcode)
{
    m_taOpcode = static_cast<uint8_t>(opcode);
}

void UbTransactionHeader::SetIniTaSsn(uint16_t ssn)
{
    m_iniTaSsn = ssn;
}

void UbTransactionHeader::SetOrder(OrderType order)
{
    m_order = static_cast<uint8_t>(order);
}

void UbTransactionHeader::SetOrder(uint8_t order)
{
    m_order = order;
}

void UbTransactionHeader::SetTcETahEn(bool enable)
{
    m_mtEn = enable;
}

void UbTransactionHeader::SetExclusive(bool exclusive)
{
    m_exclusive = exclusive;
}

void UbTransactionHeader::SetIniRcType(IniRcType type)
{
    m_iniRcIdType = static_cast<uint8_t>(type);
}

void UbTransactionHeader::SetIniRcType(uint8_t type)
{
    m_iniRcIdType = type;
}

void UbTransactionHeader::SetIniRcId(uint32_t jettyNum)
{
    m_iniRcIdId = jettyNum & 0xFFFFF; // 确保只使用低20位
}

// Getters
uint8_t UbTransactionHeader::GetTaOpcode() const
{
    return m_taOpcode;
}

uint16_t UbTransactionHeader::GetIniTaSsn() const
{
    return m_iniTaSsn;
}

uint8_t UbTransactionHeader::GetOrder() const
{
    return m_order;
}

bool UbTransactionHeader::GetExclusive() const
{
    return m_exclusive;
}

uint8_t UbTransactionHeader::GetIniRcType() const
{
    return m_iniRcIdType;
}

uint32_t UbTransactionHeader::GetIniRcId() const
{
    return m_iniRcIdId;
}

/*
 ***************************************************
 * UbCongestionExtTph class implementation
 ***************************************************
 */

UbCongestionExtTph::UbCongestionExtTph()
{
    NS_LOG_FUNCTION(this);
    // 初始化union为0
    m_congestionFields.raw = 0;
}

UbCongestionExtTph::~UbCongestionExtTph()
{
    NS_LOG_FUNCTION(this);
}

TypeId UbCongestionExtTph::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbCongestionExtTph")
                            .SetParent<Header>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbCongestionExtTph>();
    return tid;
}

TypeId UbCongestionExtTph::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void UbCongestionExtTph::Print(std::ostream& os) const
{
    os << "UbCongestionExtTph: " << "AckSeq=" << m_ackSequence <<
          " Location=" << GetLocation() << " I=" << GetI() << " C=" <<
          static_cast<uint32_t>(GetC()) << " Hint=" << GetHint();
}

uint32_t UbCongestionExtTph::GetSerializedSize(void) const
{
    return totalHeaderSize;
}

void UbCongestionExtTph::Serialize(Buffer::Iterator start) const
{
    Buffer::Iterator i = start;

    // 字节0-3: ACK Sequence (32位，网络字节序)
    i.WriteHtonU32(m_ackSequence);

    // 字节4-7: 直接写入raw字段 (32位，网络字节序)
    i.WriteHtonU32(m_congestionFields.raw);
}

uint32_t UbCongestionExtTph::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;

    // 字节0-3: ACK Sequence (32位，网络字节序)
    m_ackSequence = i.ReadNtohU32();

    // 字节4-7: 直接读取到raw字段 (32位，网络字节序)
    m_congestionFields.raw = i.ReadNtohU32();

    return GetSerializedSize();
}

// Common Setters/Getters
void UbCongestionExtTph::SetAckSequence(uint32_t ackSeq)
{
    m_ackSequence = ackSeq;
}

uint32_t UbCongestionExtTph::GetAckSequence(void) const
{
    return m_ackSequence;
}

// CAQM specific methods
void UbCongestionExtTph::SetLocation(bool location)
{
    m_congestionFields.caqm.location = location;
}

void UbCongestionExtTph::SetI(bool i)
{
    m_congestionFields.caqm.i = i;
}

void UbCongestionExtTph::SetC(uint8_t c)
{
    m_congestionFields.caqm.c = c;
}

void UbCongestionExtTph::SetHint(uint16_t hint)
{
    m_congestionFields.caqm.hint = hint;
}

bool UbCongestionExtTph::GetLocation(void) const
{
    return m_congestionFields.caqm.location;
}

bool UbCongestionExtTph::GetI(void) const
{
    return m_congestionFields.caqm.i;
}

uint8_t UbCongestionExtTph::GetC(void) const
{
    return m_congestionFields.caqm.c;
}

uint16_t UbCongestionExtTph::GetHint(void) const
{
    return m_congestionFields.caqm.hint;
}

// Raw access for future algorithm extensions
void UbCongestionExtTph::SetRawBytes4to7(uint32_t rawValue)
{
    m_congestionFields.raw = rawValue;
}

uint32_t UbCongestionExtTph::GetRawBytes4to7(void) const
{
    return m_congestionFields.raw;
}

UbCnpExtTph::UbCnpExtTph() = default;

UbCnpExtTph::~UbCnpExtTph() = default;

TypeId
UbCnpExtTph::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbCnpExtTph")
                            .SetParent<Header>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbCnpExtTph>();
    return tid;
}

TypeId
UbCnpExtTph::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void
UbCnpExtTph::Print(std::ostream& os) const
{
    os << "UbCnpExtTph: Ecn=" << static_cast<uint32_t>(m_ecn) <<
          " Location=" << m_location;
}

void
UbCnpExtTph::Serialize(Buffer::Iterator start) const
{
    Buffer::Iterator i = start;
    const uint32_t word0 =
        (static_cast<uint32_t>(m_ecn & 0x3U) << 30) |
        (static_cast<uint32_t>(m_location ? 1U : 0U) << 29);
    i.WriteHtonU32(word0);
    i.WriteHtonU32(0);
    i.WriteHtonU32(0);
    i.WriteHtonU32(0);
}

uint32_t
UbCnpExtTph::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    const uint32_t word0 = i.ReadNtohU32();
    m_ecn = static_cast<uint8_t>((word0 >> 30) & 0x3U);
    m_location = ((word0 >> 29) & 0x1U) != 0;
    i.ReadNtohU32();
    i.ReadNtohU32();
    i.ReadNtohU32();
    return GetSerializedSize();
}

uint32_t
UbCnpExtTph::GetSerializedSize(void) const
{
    return totalHeaderSize;
}

void
UbCnpExtTph::SetEcn(uint8_t ecn)
{
    m_ecn = ecn & 0x3U;
}

uint8_t
UbCnpExtTph::GetEcn() const
{
    return m_ecn;
}

void
UbCnpExtTph::SetLocation(bool location)
{
    m_location = location;
}

bool
UbCnpExtTph::GetLocation() const
{
    return m_location;
}

/*
 ***************************************************
 * UbCna16NetworkHeader implementation
 ***************************************************
 */
UbCna16NetworkHeader::UbCna16NetworkHeader() = default;
UbCna16NetworkHeader::~UbCna16NetworkHeader() = default;

TypeId UbCna16NetworkHeader::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::UbCna16NetworkHeader").SetParent<Header>().AddConstructor<UbCna16NetworkHeader>();
    return tid;
}

TypeId UbCna16NetworkHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t UbCna16NetworkHeader::GetSerializedSize() const
{
    return totalHeaderSize;
}

void UbCna16NetworkHeader::Print(std::ostream& os) const
{
    os << "SCNA=" << m_scna << " DCNA=" << m_dcna << " Mode=" << unsigned(m_mode);
    switch (m_mode) {
        case 0b000:
            os << " Loc=" << m_ccFields.mode0.location << " En=" <<
            m_ccFields.mode0.enable << " C=" << m_ccFields.mode0.c <<
            " I=" << m_ccFields.mode0.i << " Hint=" << unsigned(m_ccFields.mode0.hint);
            break;
        case 0b010:
            os << " Loc=" << m_ccFields.mode2.location << " TS=" <<
            m_ccFields.mode2.timestamp << " FECN=" << unsigned(m_ccFields.mode2.fecn);
            break;
        case 0b100:
            os << " Loc=" << m_ccFields.mode4.location <<
            " FECN=" << unsigned(m_ccFields.mode4.fecn);
            break;
        default:
            os << " raw13=0x" << std::hex << m_ccFields.raw13 << std::dec;
            break;
    }
    os << " LB=" << unsigned(m_lb) << " SL=" <<
    unsigned(m_serviceLevel) << " NLP=" << unsigned(m_nlp);
}

void UbCna16NetworkHeader::Serialize(Buffer::Iterator i) const
{
    i.WriteU16(m_scna);
    i.WriteU16(m_dcna);
    uint16_t ccField = ((m_mode & 0x7) << 13) | (m_ccFields.raw13 & 0x1FFF);
    i.WriteU16(ccField);
    i.WriteU8(m_lb);
    uint8_t b7 = ((m_serviceLevel & 0x0F) << 4) | ((m_management & 0x01) << 3) | (m_nlp & 0x07);
    i.WriteU8(b7);
}

uint32_t UbCna16NetworkHeader::Deserialize(Buffer::Iterator i)
{
    m_scna = i.ReadU16();
    m_dcna = i.ReadU16();
    uint16_t ccField = i.ReadU16();
    m_mode = (ccField >> 13) & 0x7;
    m_ccFields.raw13 = ccField & 0x1FFF;
    m_lb = i.ReadU8();
    uint8_t b7 = i.ReadU8();
    m_serviceLevel = (b7 >> 4) & 0x0F;
    m_management = (b7 >> 3) & 0x01;
    m_nlp = b7 & 0x07;
    return GetSerializedSize();
}

// Setters
void UbCna16NetworkHeader::SetScna(uint16_t v)
{
    m_scna = v;
}

void UbCna16NetworkHeader::SetDcna(uint16_t v)
{
    m_dcna = v;
}

void UbCna16NetworkHeader::SetMode(uint8_t m)
{
    m_mode = m & 0x7;
}

void UbCna16NetworkHeader::SetLocation(bool loc)
{
    switch (m_mode) {
        case 0b000:
            m_ccFields.mode0.location = loc;
            break;
        case 0b010:
            m_ccFields.mode2.location = loc;
            break;
        case 0b100:
            m_ccFields.mode4.location = loc;
            break;
        default:
            break;
    }
}

void UbCna16NetworkHeader::SetEnable(bool en)
{
    if (m_mode == 0b000) {
        m_ccFields.mode0.enable = en;
    }
}

void UbCna16NetworkHeader::SetC(bool c)
{
    if (m_mode == 0b000) {
        m_ccFields.mode0.c = c;
    }
}

void UbCna16NetworkHeader::SetI(bool v)
{
    if (m_mode == 0b000) {
        m_ccFields.mode0.i = v;
    }
}

void UbCna16NetworkHeader::SetHint(uint8_t h)
{
    if (m_mode == 0b000) {
        m_ccFields.mode0.hint = h & 0x7F;
    }
}

void UbCna16NetworkHeader::SetTimestamp(uint16_t ts)
{
    if (m_mode == 0b010) {
        m_ccFields.mode2.timestamp = ts & 0x03FF;
    }
}

void UbCna16NetworkHeader::SetFecn(uint8_t f)
{
    f &= 0x3;
    if (m_mode == 0b010) {
        m_ccFields.mode2.fecn = f;
    } else if (m_mode == 0b100) {
        m_ccFields.mode4.fecn = f;
    }
}

void UbCna16NetworkHeader::SetLb(uint8_t lb)
{
    m_lb = lb;
}

void UbCna16NetworkHeader::SetServiceLevel(uint8_t sl)
{
    m_serviceLevel = sl & 0x0F;
}

void UbCna16NetworkHeader::SetNlp(uint8_t nlp)
{
    m_nlp = nlp & 0x07;
}

// Getters
uint16_t UbCna16NetworkHeader::GetScna() const
{
    return m_scna;
}

uint16_t UbCna16NetworkHeader::GetDcna() const
{
    return m_dcna;
}

uint8_t UbCna16NetworkHeader::GetMode() const
{
    return m_mode;
}

bool UbCna16NetworkHeader::GetLocation() const
{
    switch (m_mode) {
        case 0b000:
            return m_ccFields.mode0.location;
        case 0b010:
            return m_ccFields.mode2.location;
        case 0b100:
            return m_ccFields.mode4.location;
        default:
            return false;
    }
}

bool UbCna16NetworkHeader::GetEnable() const
{
    return (m_mode == 0b000) ? m_ccFields.mode0.enable : false;
}

bool UbCna16NetworkHeader::GetC() const
{
    return (m_mode == 0b000) ? m_ccFields.mode0.c : false;
}

bool UbCna16NetworkHeader::GetI() const
{
    return (m_mode == 0b000) ? m_ccFields.mode0.i : false;
}

uint8_t UbCna16NetworkHeader::GetHint() const
{
    return (m_mode == 0b000) ? m_ccFields.mode0.hint : 0;
}

uint16_t UbCna16NetworkHeader::GetTimestamp() const
{
    return (m_mode == 0b010) ? m_ccFields.mode2.timestamp : 0;
}

uint8_t UbCna16NetworkHeader::GetFecn() const
{
    if (m_mode == 0b010) {
        return m_ccFields.mode2.fecn;
    }
    if (m_mode == 0b100) {
        return m_ccFields.mode4.fecn;
    }
    return 0;
}

uint8_t UbCna16NetworkHeader::GetLb() const
{
    return m_lb;
}

uint8_t UbCna16NetworkHeader::GetServiceLevel() const
{
    return m_serviceLevel;
}

uint8_t UbCna16NetworkHeader::GetNlp() const
{
    return m_nlp;
}

bool UbCna16NetworkHeader::IsValidMode() const
{
    switch (m_mode) {
        case 0b000:
        case 0b010:
        case 0b100:
            return true;
        default:
            return false;
    }
}

/*
 ***************************************************
 * UbCna24NetworkHeader implementation
 ***************************************************
 */
UbCna24NetworkHeader::UbCna24NetworkHeader() = default;
UbCna24NetworkHeader::~UbCna24NetworkHeader() = default;

TypeId UbCna24NetworkHeader::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::UbCna24NetworkHeader").SetParent<Header>().AddConstructor<UbCna24NetworkHeader>();
    return tid;
}

TypeId UbCna24NetworkHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

uint32_t UbCna24NetworkHeader::GetSerializedSize() const
{
    return totalHeaderSize;
}

void UbCna24NetworkHeader::Print(std::ostream& os) const
{
    os << "SCNA=" << m_scna << " DCNA=" << m_dcna << " Mode=" << unsigned(m_mode);
    switch (m_mode) {
        case 0b000:
            os << " Loc=" << m_ccFields.mode0.location << " En=" <<
            m_ccFields.mode0.enable << " C=" << m_ccFields.mode0.c <<
            " I=" << m_ccFields.mode0.i << " Hint=" << unsigned(m_ccFields.mode0.hint);
            break;
        case 0b010:
            os << " Loc=" << m_ccFields.mode2.location << " TS=" <<
            m_ccFields.mode2.timestamp << " FECN=" << unsigned(m_ccFields.mode2.fecn);
            break;
        case 0b100:
            os << " Loc=" << m_ccFields.mode4.location <<
            " FECN=" << unsigned(m_ccFields.mode4.fecn);
            break;
        default:
            os << " raw13=0x" << std::hex << m_ccFields.raw13 << std::dec;
            break;
    }
    os << " LB=" << unsigned(m_lb) << " SL=" <<
    unsigned(m_serviceLevel) << " NLP=" << unsigned(m_nlp);
}

void UbCna24NetworkHeader::Serialize(Buffer::Iterator i) const
{
    // Byte0-2: SCNA (24 bits, 3 bytes)
    i.WriteU8((m_scna >> 16) & 0xFF);
    i.WriteU8((m_scna >> 8) & 0xFF);
    i.WriteU8(m_scna & 0xFF);
    
    // Byte3-5: DCNA (24 bits, 3 bytes)
    i.WriteU8((m_dcna >> 16) & 0xFF);
    i.WriteU8((m_dcna >> 8) & 0xFF);
    i.WriteU8(m_dcna & 0xFF);
    
    // Byte6-7: Congestion Control (16 bits)
    uint16_t ccField = ((m_mode & 0x7) << 13) | (m_ccFields.raw13 & 0x1FFF);
    i.WriteU16(ccField);
    
    // Byte8: LB (8 bits)
    i.WriteU8(m_lb);
    
    // Byte9: [ServiceLevel:4][reserved:1][NLP:3]
    uint8_t byte9 = ((m_serviceLevel & 0x0F) << 4) | ((m_reserve ? 1 : 0) << 3) | (m_nlp & 0x07);
    i.WriteU8(byte9);
}

uint32_t UbCna24NetworkHeader::Deserialize(Buffer::Iterator i)
{
    // Byte0-2: SCNA (24 bits, 3 bytes)
    m_scna = (static_cast<uint32_t>(i.ReadU8()) << 16) |
             (static_cast<uint32_t>(i.ReadU8()) << 8) |
             static_cast<uint32_t>(i.ReadU8());
    
    // Byte3-5: DCNA (24 bits, 3 bytes)
    m_dcna = (static_cast<uint32_t>(i.ReadU8()) << 16) |
             (static_cast<uint32_t>(i.ReadU8()) << 8) |
             static_cast<uint32_t>(i.ReadU8());
    
    // Byte6-7: Congestion Control (16 bits)
    uint16_t ccField = i.ReadU16();
    m_mode = (ccField >> 13) & 0x7;
    m_ccFields.raw13 = ccField & 0x1FFF;
    
    // Byte8: LB (8 bits)
    m_lb = i.ReadU8();
    
    // Byte9: [ServiceLevel:4][reserved:1][NLP:3]
    uint8_t byte9 = i.ReadU8();
    m_serviceLevel = (byte9 >> 4) & 0x0F;
    m_reserve = (byte9 >> 3) & 0x01;
    m_nlp = byte9 & 0x07;
    
    return GetSerializedSize();
}

// Setters
void UbCna24NetworkHeader::SetScna(uint32_t v)
{
    m_scna = v & 0xFFFFFF;  // 确保只使用低24位
}

void UbCna24NetworkHeader::SetDcna(uint32_t v)
{
    m_dcna = v & 0xFFFFFF;  // 确保只使用低24位
}

void UbCna24NetworkHeader::SetMode(uint8_t m)
{
    m_mode = m & 0x7;
}

void UbCna24NetworkHeader::SetLocation(bool loc)
{
    switch (m_mode) {
        case 0b000:
            m_ccFields.mode0.location = loc;
            break;
        case 0b010:
            m_ccFields.mode2.location = loc;
            break;
        case 0b100:
            m_ccFields.mode4.location = loc;
            break;
        default:
            break;
    }
}

void UbCna24NetworkHeader::SetEnable(bool en)
{
    if (m_mode == 0b000) {
        m_ccFields.mode0.enable = en;
    }
}

void UbCna24NetworkHeader::SetC(bool c)
{
    if (m_mode == 0b000) {
        m_ccFields.mode0.c = c;
    }
}

void UbCna24NetworkHeader::SetI(bool v)
{
    if (m_mode == 0b000) {
        m_ccFields.mode0.i = v;
    }
}

void UbCna24NetworkHeader::SetHint(uint8_t h)
{
    if (m_mode == 0b000) {
        m_ccFields.mode0.hint = h & 0x7F;
    }
}

void UbCna24NetworkHeader::SetTimestamp(uint16_t ts)
{
    if (m_mode == 0b010) {
        m_ccFields.mode2.timestamp = ts & 0x03FF;
    }
}

void UbCna24NetworkHeader::SetFecn(uint8_t f)
{
    f &= 0x3;
    if (m_mode == 0b010) {
        m_ccFields.mode2.fecn = f;
    } else if (m_mode == 0b100) {
        m_ccFields.mode4.fecn = f;
    }
}

void UbCna24NetworkHeader::SetLb(uint8_t lb)
{
    m_lb = lb;
}

void UbCna24NetworkHeader::SetServiceLevel(uint8_t sl)
{
    m_serviceLevel = sl & 0x0F;
}

void UbCna24NetworkHeader::SetNlp(uint8_t nlp)
{
    m_nlp = nlp & 0x07;
}

// Getters
uint32_t UbCna24NetworkHeader::GetScna() const
{
    return m_scna;
}

uint32_t UbCna24NetworkHeader::GetDcna() const
{
    return m_dcna;
}

uint8_t UbCna24NetworkHeader::GetMode() const
{
    return m_mode;
}

bool UbCna24NetworkHeader::GetLocation() const
{
    switch (m_mode) {
        case 0b000:
            return m_ccFields.mode0.location;
        case 0b010:
            return m_ccFields.mode2.location;
        case 0b100:
            return m_ccFields.mode4.location;
        default:
            return false;
    }
}

bool UbCna24NetworkHeader::GetEnable() const
{
    return (m_mode == 0b000) ? m_ccFields.mode0.enable : false;
}

bool UbCna24NetworkHeader::GetC() const
{
    return (m_mode == 0b000) ? m_ccFields.mode0.c : false;
}

bool UbCna24NetworkHeader::GetI() const
{
    return (m_mode == 0b000) ? m_ccFields.mode0.i : false;
}

uint8_t UbCna24NetworkHeader::GetHint() const
{
    return (m_mode == 0b000) ? m_ccFields.mode0.hint : 0;
}

uint16_t UbCna24NetworkHeader::GetTimestamp() const
{
    return (m_mode == 0b010) ? m_ccFields.mode2.timestamp : 0;
}

uint8_t UbCna24NetworkHeader::GetFecn() const
{
    if (m_mode == 0b010) {
        return m_ccFields.mode2.fecn;
    }
    if (m_mode == 0b100) {
        return m_ccFields.mode4.fecn;
    }
    return 0;
}

uint8_t UbCna24NetworkHeader::GetLb() const
{
    return m_lb;
}

uint8_t UbCna24NetworkHeader::GetServiceLevel() const
{
    return m_serviceLevel;
}

uint8_t UbCna24NetworkHeader::GetNlp() const
{
    return m_nlp;
}

bool UbCna24NetworkHeader::IsValidMode() const
{
    switch (m_mode) {
        case 0b000:
        case 0b010:
        case 0b100:
            return true;
        default:
            return false;
    }
}

/*
 ***************************************************
 * UbMAExtTah implementation
 ***************************************************
 */

UbMAExtTah::UbMAExtTah()
{
    NS_LOG_FUNCTION(this);
}

UbMAExtTah::~UbMAExtTah()
{
    NS_LOG_FUNCTION(this);
}

TypeId UbMAExtTah::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbMAExtTah")
                            .SetParent<Header>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbMAExtTah>();
    return tid;
}

TypeId UbMAExtTah::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void UbMAExtTah::Print(std::ostream& os) const
{
    os << "UbMAExtTah: "
       << "VirtualAddr=0x" << std::hex << m_virtualAddress << std::dec
       << " TokenId=" << m_tokenId
       << " Length=" << m_length;
}

uint32_t UbMAExtTah::GetSerializedSize(void) const
{
    return totalHeaderSize; // 16 bytes
}

void UbMAExtTah::Serialize(Buffer::Iterator start) const
{
    NS_LOG_FUNCTION(this << &start);
    
    Buffer::Iterator i = start;

    // 字节0-7: Virtual Address (64位，网络字节序)
    i.WriteHtonU64(m_virtualAddress);

    // 字节8-11: [Reserved:4][Token ID:20][Reserved:8]
    // 需要将20位Token ID嵌入到32位字段中
    uint32_t tokenField = ((m_reserved1 & 0xF) << 28) |  // 高4位保留
                          ((m_tokenId & 0xFFFFF) << 8) | // 20位Token ID
                          (m_reserved2 & 0xFF);          // 低8位保留
    i.WriteHtonU32(tokenField);

    // 字节12-15: Length (32位，网络字节序)
    i.WriteHtonU32(m_length);
}

uint32_t UbMAExtTah::Deserialize(Buffer::Iterator start)
{
    NS_LOG_FUNCTION(this << &start);
    
    Buffer::Iterator i = start;

    // 字节0-7: Virtual Address (64位，网络字节序)
    m_virtualAddress = i.ReadNtohU64();

    // 字节8-11: [Reserved:4][Token ID:20][Reserved:8]
    uint32_t tokenField = i.ReadNtohU32();
    m_reserved1 = (tokenField >> 28) & 0xF;        // 提取高4位保留字段
    m_tokenId = (tokenField >> 8) & 0xFFFFF;       // 提取20位Token ID
    m_reserved2 = tokenField & 0xFF;               // 提取低8位保留字段

    // 字节12-15: Length (32位，网络字节序)
    m_length = i.ReadNtohU32();

    return GetSerializedSize();
}

// Setters
void UbMAExtTah::SetVirtualAddress(uint64_t virtualAddr)
{
    m_virtualAddress = virtualAddr;
}

void UbMAExtTah::SetTokenId(uint32_t tokenId)
{
    m_tokenId = tokenId & maxTokenId; // 确保只使用低20位
}

void UbMAExtTah::SetLength(uint32_t length)
{
    m_length = length;
}

// Getters
uint64_t UbMAExtTah::GetVirtualAddress() const
{
    return m_virtualAddress;
}

uint32_t UbMAExtTah::GetTokenId() const
{
    return m_tokenId;
}

uint32_t UbMAExtTah::GetLength() const
{
    return m_length;
}

// Validation methods
bool UbMAExtTah::IsValidTokenId() const
{
    return m_tokenId <= maxTokenId;
}

/*
 ***************************************************
 * UbCompactMAExtTah implementation
 ***************************************************
 */

UbCompactMAExtTah::UbCompactMAExtTah()
{
    NS_LOG_FUNCTION(this);
}

UbCompactMAExtTah::~UbCompactMAExtTah()
{
    NS_LOG_FUNCTION(this);
}

TypeId UbCompactMAExtTah::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbCompactMAExtTah")
                            .SetParent<Header>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbCompactMAExtTah>();
    return tid;
}

TypeId UbCompactMAExtTah::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void UbCompactMAExtTah::Print(std::ostream& os) const
{
    os << "UbCompactMAExtTah: "
       << "VirtualAddr=0x" << std::hex << m_virtualAddress << std::dec
       << " TokenId=" << m_tokenId;
}

uint32_t UbCompactMAExtTah::GetSerializedSize(void) const
{
    return totalHeaderSize; // 12 bytes
}

void UbCompactMAExtTah::Serialize(Buffer::Iterator start) const
{
    NS_LOG_FUNCTION(this << &start);
    
    Buffer::Iterator i = start;

    // 字节0-7: [Virtual Address:58][Affinity Hint:2][Strong Order:1][Length:3]
    uint64_t packedValue = ((m_virtualAddress & 0x3FFFFFFFFFFFFFF) << 6) |  // Virtual Address (58 bits)
                       ((m_affinityHint & 0x3) << 4) |                   // Affinity Hint (2 bits)
                       ((m_strongOrder ? 1 : 0) << 3) |                  // Strong Order (1 bit)
                       ((m_length & 0x7));                               // Length (3 bits)
    i.WriteHtonU64(packedValue);

    // 字节8-11: [Reserved:4][Token ID:20][Reserved:8]
    // 需要将20位Token ID嵌入到32位字段中
    uint32_t tokenField = ((m_reserved1 & 0xF) << 28) |  // 高4位保留
                          ((m_tokenId & 0xFFFFF) << 8) | // 20位Token ID
                          (m_reserved2 & 0xFF);          // 低8位保留
    i.WriteHtonU32(tokenField);
}

uint32_t UbCompactMAExtTah::Deserialize(Buffer::Iterator start)
{
    NS_LOG_FUNCTION(this << &start);
    
    Buffer::Iterator i = start;

    // 字节0-7: [Virtual Address:58][Affinity Hint:2][Strong Order:1][Length:3]
    uint64_t packedValue = i.ReadNtohU64();
    
    // 提取各个字段
    m_virtualAddress = (packedValue >> 6) & 0x3FFFFFFFFFFFFFF;  // 提取高58位
    m_affinityHint = (packedValue >> 4) & 0x3;                  // 提取2位Affinity Hint
    m_strongOrder = (packedValue >> 3) & 0x1;                   // 提取1位Strong Order
    m_length = packedValue & 0x7;                               // 提取低3位Length

    // 字节8-11: [Reserved:4][Token ID:20][Reserved:8]
    uint32_t tokenField = i.ReadNtohU32();
    m_reserved1 = (tokenField >> 28) & 0xF;        // 提取高4位保留字段
    m_tokenId = (tokenField >> 8) & 0xFFFFF;       // 提取20位Token ID
    m_reserved2 = tokenField & 0xFF;               // 提取低8位保留字段

    return GetSerializedSize();
}

// Setters
void UbCompactMAExtTah::SetVirtualAddress(uint64_t virtualAddr)
{
    m_virtualAddress = virtualAddr;
}

void UbCompactMAExtTah::SetTokenId(uint32_t tokenId)
{
    m_tokenId = tokenId & maxTokenId; // 确保只使用低20位
}

void UbCompactMAExtTah::SetStrongOrder(bool strongOrder)
{
    m_strongOrder = strongOrder;
}

void UbCompactMAExtTah::SetLength(uint8_t length)
{
    m_length = length;
}

// Getters
uint64_t UbCompactMAExtTah::GetVirtualAddress() const
{
    return m_virtualAddress;
}

uint32_t UbCompactMAExtTah::GetTokenId() const
{
    return m_tokenId;
}

bool UbCompactMAExtTah::GetStrongOrder() const
{
    return m_strongOrder;
}

uint8_t UbCompactMAExtTah::GetLength() const
{
    return m_length;
}

// Validation methods
bool UbCompactMAExtTah::IsValidTokenId() const
{
    return m_tokenId <= maxTokenId;
}

/*
 ***************************************************
 * UbAckTransactionHeader implementation
 ***************************************************
 */
UbAckTransactionHeader::UbAckTransactionHeader()
{
    NS_LOG_FUNCTION (this);
}

UbAckTransactionHeader::~UbAckTransactionHeader()
{
    NS_LOG_FUNCTION (this);
}

TypeId UbAckTransactionHeader::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbAckTransactionHeader")
        .SetParent<Header>()
        .SetGroupName("UnifiedBus")
        .AddConstructor<UbAckTransactionHeader>();
    return tid;
}

TypeId UbAckTransactionHeader::GetInstanceTypeId(void) const
{
    return GetTypeId ();
}

void UbAckTransactionHeader::Print(std::ostream &os) const
{
    os << "UbAckTransactionHeader: " << "OpCode=" <<
    static_cast<uint32_t>(m_taOpcode) << ", Version=" <<
    static_cast<uint32_t>(m_taVersion) << ", SV=" <<
    static_cast<uint32_t>(m_sv) << ", Poison=" <<
    (m_poison ? "true" : "false") << ", TASSN=" <<
    m_iniTaSsn << ", RspStatus=" << static_cast<uint32_t>(m_rspStatus) <<
    ", RspInfo=" << static_cast<uint32_t>(m_rspInfo) <<
    ", RcType=" << static_cast<uint32_t>(m_iniRcType) << ", RcId=" << m_iniRcId;
}

void UbAckTransactionHeader::Serialize(Buffer::Iterator start) const
{
    Buffer::Iterator i = start;
    
    // 字节0: [TA OpCode:8]
    i.WriteU8 (m_taOpcode);
    
    // 字节1: [TA Version:2][Reserved:2][SV:1][Poison:1][Reserved:2]
    uint8_t byte1 = 0;
    byte1 |= (m_taVersion & 0x03) << 6;        // bits 7-6: TA Version
    byte1 |= (m_reserved1 & 0x03) << 4;       // bits 5-4: Reserved
    byte1 |= (m_sv & 0x01) << 3;              // bit 3: SV
    byte1 |= (m_poison ? 1 : 0) << 2;         // bit 2: Poison
    byte1 |= (m_reserved2 & 0x03);            // bits 1-0: Reserved
    i.WriteU8 (byte1);
    
    // 字节2-3: [Initiator TASSN:16]
    i.WriteHtonU16 (m_iniTaSsn);
    
    // 字节4: [RSP Status:3][RSP INFO:5]
    uint8_t byte4 = 0;
    byte4 |= (m_rspStatus & 0x07) << 5;       // bits 7-5: RSP Status
    byte4 |= (m_rspInfo & 0x1F);              // bits 4-0: RSP INFO
    i.WriteU8 (byte4);
    
    // 字节5-7: [Reserved:2][INI RC TYPE:2][INI RC ID:12]
    uint32_t bytes567 = 0;
    bytes567 |= (m_reserved3 & 0x03) << 14;   // bits 15-14: Reserved
    bytes567 |= (m_iniRcType & 0x03) << 12;   // bits 13-12: INI RC TYPE
    bytes567 |= (m_iniRcId & 0x0FFF);         // bits 11-0: INI RC ID
    
    // 写入3字节 (高位在前)
    i.WriteU8 ((bytes567 >> 16) & 0xFF);      // 字节5
    i.WriteU8 ((bytes567 >> 8) & 0xFF);       // 字节6
    i.WriteU8 (bytes567 & 0xFF);              // 字节7
}

uint32_t UbAckTransactionHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    
    // 字节0: [TA OpCode:8]
    m_taOpcode = i.ReadU8 ();
    
    // 字节1: [TA Version:2][Reserved:2][SV:1][Poison:1][Reserved:2]
    uint8_t byte1 = i.ReadU8 ();
    m_taVersion = (byte1 >> 6) & 0x03;
    m_reserved1 = (byte1 >> 4) & 0x03;
    m_sv = (byte1 >> 3) & 0x01;
    m_poison = ((byte1 >> 2) & 0x01) == 1;
    m_reserved2 = byte1 & 0x03;
    
    // 字节2-3: [Initiator TASSN:16]
    m_iniTaSsn = i.ReadNtohU16 ();
    
    // 字节4: [RSP Status:3][RSP INFO:5]
    uint8_t byte4 = i.ReadU8 ();
    m_rspStatus = (byte4 >> 5) & 0x07;
    m_rspInfo = byte4 & 0x1F;
    
    // 字节5-7: [Reserved:2][INI RC TYPE:2][INI RC ID:12]
    uint32_t bytes567 = 0;
    bytes567 |= static_cast<uint32_t>(i.ReadU8 ()) << 16;  // 字节5
    bytes567 |= static_cast<uint32_t>(i.ReadU8 ()) << 8;   // 字节6
    bytes567 |= static_cast<uint32_t>(i.ReadU8 ());        // 字节7
    
    m_reserved3 = (bytes567 >> 14) & 0x03;
    m_iniRcType = (bytes567 >> 12) & 0x03;
    m_iniRcId = bytes567 & 0x0FFF;
    
    return GetSerializedSize ();
}

uint32_t UbAckTransactionHeader::GetSerializedSize(void) const
{
    return totalHeaderSize;
}

// Setters
void UbAckTransactionHeader::SetTaOpcode(TaOpcode opcode)
{
    m_taOpcode = static_cast<uint8_t>(opcode);
}

void UbAckTransactionHeader::SetTaOpcode(uint8_t opcode)
{
    m_taOpcode = opcode;
}

void UbAckTransactionHeader::SetTaVersion(uint8_t version)
{
    m_taVersion = version & 0x03;  // 确保只有2位
}

void UbAckTransactionHeader::SetSV(uint8_t sv)
{
    m_sv = sv & 0x01;
}

void UbAckTransactionHeader::SetPoison(bool poison)
{
    m_poison = poison;
}

void UbAckTransactionHeader::SetIniTaSsn(uint16_t tassn)
{
    m_iniTaSsn = tassn;
}

void UbAckTransactionHeader::SetRspStatus(uint8_t status)
{
    m_rspStatus = status & 0x07;  // 确保只有3位
}

void UbAckTransactionHeader::SetRspInfo(uint8_t info)
{
    m_rspInfo = info & 0x1F;  // 确保只有5位
}

void UbAckTransactionHeader::SetIniRcType(uint8_t type)
{
    m_iniRcType = type & 0x03;  // 确保只有2位
}

void UbAckTransactionHeader::SetIniRcId(uint32_t id)
{
    m_iniRcId = id & 0x0FFFFF;  // 确保只有20位
}

// Getters
uint8_t UbAckTransactionHeader::GetTaOpcode() const
{
    return m_taOpcode;
}

uint8_t UbAckTransactionHeader::GetTaVersion() const
{
    return m_taVersion;
}

uint8_t UbAckTransactionHeader::GetSV() const
{
    return m_sv;
}

bool UbAckTransactionHeader::GetPoison() const
{
    return m_poison;
}

uint16_t UbAckTransactionHeader::GetIniTaSsn() const
{
    return m_iniTaSsn;
}

uint8_t UbAckTransactionHeader::GetRspStatus() const
{
    return m_rspStatus;
}

uint8_t UbAckTransactionHeader::GetRspInfo() const
{
    return m_rspInfo;
}

uint8_t UbAckTransactionHeader::GetIniRcType() const
{
    return m_iniRcType;
}

uint32_t UbAckTransactionHeader::GetIniRcId() const
{
    return m_iniRcId;
}

// 验证方法
bool UbAckTransactionHeader::IsValidOpcode() const
{
    // 根据注释，应该是0x11~0x13范围内的操作码
    return (m_taOpcode >= static_cast<uint8_t>(TaOpcode::TA_OPCODE_TRANSACTION_ACK) &&
            m_taOpcode <= static_cast<uint8_t>(TaOpcode::TA_OPCODE_ATOMIC_RESPONSE));
}

bool UbAckTransactionHeader::IsValidRspStatus() const
{
    // 3位字段，有效范围0-7
    return (m_rspStatus <= 0x07);
}

bool UbAckTransactionHeader::IsValidIniRcType() const
{
    // 2位字段，有效范围0-3
    return (m_iniRcType <= 0x03);
}

/*
 ***************************************************
 * UbCompactAckTransactionHeader implementation
 ***************************************************
 */
UbCompactAckTransactionHeader::UbCompactAckTransactionHeader()
{
    NS_LOG_FUNCTION(this);
}

UbCompactAckTransactionHeader::~UbCompactAckTransactionHeader()
{
    NS_LOG_FUNCTION(this);
}

TypeId UbCompactAckTransactionHeader::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbCompactAckTransactionHeader")
        .SetParent<Header>()
        .SetGroupName ("UnifiedBus")
        .AddConstructor<UbCompactAckTransactionHeader>();
    return tid;
}

TypeId UbCompactAckTransactionHeader::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void UbCompactAckTransactionHeader::Print(std::ostream &os) const
{
    os << "UbCompactAckTransactionHeader: " << "OpCode=" <<
    static_cast<uint32_t>(m_taOpcode) << ", Version=" <<
    static_cast<uint32_t>(m_taVersion) << ", Poison=" <<
    (m_poison ? "true" : "false") << ", TASSN=" << m_iniTaSsn;
}

void UbCompactAckTransactionHeader::Serialize(Buffer::Iterator start) const
{
    Buffer::Iterator i = start;
    
    // 字节0: [TA OpCode:8]
    i.WriteU8 (m_taOpcode);
    
    // 字节1: [TA Version:2][Status:2][reserved:1][Poison:1][Reserved:2]
    uint8_t byte1 = 0;
    byte1 |= (m_taVersion & 0x03) << 6;        // bits 7-6: TA Version
    byte1 |= (m_status & 0x03) << 4;           // bits 5-4: Status
    byte1 |= (m_reserved1 & 0x01) << 3;        // bit 3: reserved
    byte1 |= (m_poison ? 1 : 0) << 2;          // bit 2: Poison
    byte1 |= (m_reserved2 & 0x03);             // bits 1-0: Reserved
    i.WriteU8 (byte1);
    
    // 字节2-3: [Initiator TASSN:16]
    i.WriteHtonU16 (m_iniTaSsn);
}

uint32_t UbCompactAckTransactionHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;
    
    // 字节0: [TA OpCode:8]
    m_taOpcode = i.ReadU8();
    
    // 字节1: [TA Version:2][Status:2][reserved:1][Poison:1][Reserved:2]
    uint8_t byte1 = i.ReadU8();
    m_taVersion = (byte1 >> 6) & 0x03;      // bits 7-6: TA Version (2 bits)
    m_status = (byte1 >> 4) & 0x03;            // bits 5-4: Status (2 bits) 
    m_reserved1 = (byte1 >> 3) & 0x01; // bit 3: reserved (1 bit)
    m_poison = ((byte1 >> 2) & 0x01) == 1;  // bit 2: Poison (1 bit)
    m_reserved2 = byte1 & 0x03;             // bits 1-0: Reserved (2 bits)
    
    // 字节2-3: [Initiator TASSN:16]
    m_iniTaSsn = i.ReadNtohU16();
    
    return GetSerializedSize();
}

uint32_t UbCompactAckTransactionHeader::GetSerializedSize(void) const
{
    return totalHeaderSize;
}

// Setters
void UbCompactAckTransactionHeader::SetTaOpcode(TaOpcode opcode)
{
    m_taOpcode = static_cast<uint8_t>(opcode);
}

void UbCompactAckTransactionHeader::SetTaOpcode(uint8_t opcode)
{
    m_taOpcode = opcode;
}

void UbCompactAckTransactionHeader::SetTaVersion(uint8_t version)
{
    m_taVersion = version & 0x03;  // 确保只有2位
}

void UbCompactAckTransactionHeader::SetPoison(bool poison)
{
    m_poison = poison;
}

void UbCompactAckTransactionHeader::SetIniTaSsn(uint16_t tassn)
{
    m_iniTaSsn = tassn;
}

// Getters
uint8_t UbCompactAckTransactionHeader::GetTaOpcode() const
{
    return m_taOpcode;
}

uint8_t UbCompactAckTransactionHeader::GetTaVersion() const
{
    return m_taVersion;
}

bool UbCompactAckTransactionHeader::GetPoison() const
{
    return m_poison;
}

uint16_t UbCompactAckTransactionHeader::GetIniTaSsn() const
{
    return m_iniTaSsn;
}

// 验证方法
bool UbCompactAckTransactionHeader::IsValidOpcode() const
{
    // 根据注释，应该是0x11~0x13范围内的操作码
    return (m_taOpcode >= static_cast<uint8_t>(TaOpcode::TA_OPCODE_TRANSACTION_ACK) &&
            m_taOpcode <= static_cast<uint8_t>(TaOpcode::TA_OPCODE_ATOMIC_RESPONSE));
}

/*
 ***************************************************
 * UbCompactTransactionHeader implementation
 ***************************************************
 */
UbCompactTransactionHeader::UbCompactTransactionHeader()
{
}

UbCompactTransactionHeader::~UbCompactTransactionHeader()
{
}

TypeId UbCompactTransactionHeader::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbCompactTransactionHeader")
                            .SetParent<Header>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbCompactTransactionHeader>();
    return tid;
}

TypeId UbCompactTransactionHeader::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void UbCompactTransactionHeader::Print(std::ostream& os) const
{
    os << "UbCompactTransactionHeader: "
       << "TaOpcode=0x" << std::hex << (uint32_t)m_taOpcode << std::dec
       << " IniTaSsn=" << m_iniTaSsn;
}

uint32_t UbCompactTransactionHeader::GetSerializedSize(void) const
{
    return totalHeaderSize;
}

void UbCompactTransactionHeader::Serialize(Buffer::Iterator start) const
{
    // 字节0: [TaOpcode:8]
    start.WriteU8(m_taOpcode);

    // 字节1: [TaVer(fixed 0x00):2][EE(ignore):2][TK_VLD(ignore):1][POISON(ignore):1][reserved:1][UDF_FLG(ignore):1]
    uint8_t byte1 = ((m_taVer & 0x3) << 6) | ((m_ee & 0x3) << 4) | (m_tvEn ? 0x08 : 0) |
                    (m_poison ? 0x04 : 0) | ((m_reserved & 0x1) << 1) | (m_udFlg ? 0x01 : 0);
    start.WriteU8(byte1);

    // 字节2-3: [INI TaSsn:16]
    start.WriteHtonU16(m_iniTaSsn);
}

uint32_t UbCompactTransactionHeader::Deserialize(Buffer::Iterator start)
{
    // 字节0: [TaOpcode:8]
    m_taOpcode = start.ReadU8();

    // 字节1: [TaVer(fixed 0x00):2][EE(ignore):2][TK_VLD(ignore):1][POISON(ignore):1][reserved:1][UDF_FLG(ignore):1]
    uint8_t byte1 = start.ReadU8();
    m_taVer = (byte1 >> 6) & 0x3;
    m_ee = (byte1 >> 4) & 0x3;
    m_tvEn = (byte1 & 0x08) != 0;
    m_poison = (byte1 & 0x04) != 0;
    m_reserved = (byte1 >> 1) & 0x1;
    m_udFlg = (byte1 & 0x01) != 0;

    // 字节2-3: [INI TaSsn:16]
    m_iniTaSsn = start.ReadNtohU16();

    return totalHeaderSize;
}

// Setters
void UbCompactTransactionHeader::SetTaOpcode(TaOpcode opcode)
{
    m_taOpcode = static_cast<uint8_t>(opcode);
}

void UbCompactTransactionHeader::SetTaOpcode(uint8_t opcode)
{
    m_taOpcode = opcode;
}

void UbCompactTransactionHeader::SetIniTaSsn(uint16_t taSsn)
{
    m_iniTaSsn = taSsn;
}

// Getters
uint8_t UbCompactTransactionHeader::GetTaOpcode() const
{
    return m_taOpcode;
}

uint16_t UbCompactTransactionHeader::GetIniTaSsn() const
{
    return m_iniTaSsn;
}

// 验证方法
bool UbCompactTransactionHeader::IsValidOpcode() const
{
    return m_taOpcode < static_cast<uint8_t>(TaOpcode::TA_OPCODE_MAX);
}

/*
 ***************************************************
 * UbDummyTransactionHeader implementation
 ***************************************************
 */

UbDummyTransactionHeader::UbDummyTransactionHeader()
{
}

UbDummyTransactionHeader::~UbDummyTransactionHeader()
{
}

TypeId UbDummyTransactionHeader::GetTypeId(void)
{
    static TypeId tid = TypeId("ns3::UbDummyTransactionHeader")
                            .SetParent<Header>()
                            .SetGroupName("UnifiedBus")
                            .AddConstructor<UbDummyTransactionHeader>();
    return tid;
}

TypeId UbDummyTransactionHeader::GetInstanceTypeId(void) const
{
    return GetTypeId();
}

void UbDummyTransactionHeader::Print(std::ostream &os) const
{
    os << "UbDummyTransactionHeader: " << "TaOpcode=" <<
    static_cast<uint32_t>(m_taOpcode);
}

void UbDummyTransactionHeader::Serialize(Buffer::Iterator start) const
{
    start.WriteU8(m_taOpcode);
}

uint32_t UbDummyTransactionHeader::Deserialize(Buffer::Iterator start)
{
    m_taOpcode = start.ReadU8();
    return totalHeaderSize;
}

uint32_t UbDummyTransactionHeader::GetSerializedSize(void) const
{
    return totalHeaderSize;
}

void UbDummyTransactionHeader::SetTaOpcode(TaOpcode opcode)
{
    m_taOpcode = static_cast<uint8_t>(opcode);
}

void UbDummyTransactionHeader::SetTaOpcode(uint8_t opcode)
{
    m_taOpcode = opcode;
}

uint8_t UbDummyTransactionHeader::GetTaOpcode() const
{
    return m_taOpcode;
}

} // namespace ns3
