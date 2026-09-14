// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_HEADER_H
#define UB_HEADER_H

#include "ns3/header.h"
#include "ns3/ub-datatype.h"

#include <array>

namespace ns3 {
/**
 * \ingroup ub-header
 * \brief Packet header for UB Datalink Header (the first two bytes)
 *
 * 报文头格式：total 16 bits (2 bytes)
 *              [Credit:1][ACK:1][Credit Target VL:4][Reverse:1]
 * 简易实现
 * 报文头格式：
 *              [Unknown:12][Config:4]
 * 用于分辨应该用UbDatalinkControlCreditHeader还是UbDatalinkPacketHeader继续解析
 *
 */
class UbDatalinkHeader : public Header {
public:
    UbDatalinkHeader();
    UbDatalinkHeader(uint16_t unknown, uint8_t config);
    virtual ~UbDatalinkHeader();

    // Setters
    void SetConfig(uint8_t config);  // 4 bits: Config field for header type determination

    // Getters
    uint8_t GetConfig() const;  // 4 bits: Returns config field value

    // Header type determination methods
    bool IsControlCreditHeader() const;
    bool IsPacketIpv4Header() const;
    bool IsPacketIpv6Header() const;
    bool IsPacketUbMemHeader() const;

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream &os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

private:
    // 报文头字段 - 总共16位 (2字节)
    // 字节0-1: [Unknown:12][Config:4]

    uint16_t m_unknown = 0;  // 12 bits: Unknown field (视具体config有不同的含义)
    uint8_t m_config = 0;    // 4 bits: Config field for header type identification

    // 常量定义
    uint32_t totalHeaderSize = 2;        // 2 bytes total
};

/**
 * \ingroup ub-header
 * \brief Packet header for UB Link Control Header (LCH) for Credit (Crd_Ack Block)
 *
 * 简易实现
 * 报文头格式：total 2 flits = 40 bytes
 *              [(fixed 0):1][Length(fixed 00001):5][Fixed 100000:6][Config(fixed 0000):4]
 *              [Control(fixed 0010):4][Sub Control(fixed 0100):4]
 *              [SD:1][Reserved:6][Type:1]
 *              [Ack Number:16][Credit Number:96][Reserved:(Length + 1) * 8 * 20 - used]
 *              Credit Number:96 = 6bit * vl_num:16
 */
class UbDatalinkControlCreditHeader : public Header {
public:
    UbDatalinkControlCreditHeader();
    explicit UbDatalinkControlCreditHeader(const uint8_t credits[16]);
    virtual ~UbDatalinkControlCreditHeader();

    void SetAllCreditsVL(const uint8_t credits[16]);
    void SetSD(bool sd);
    void SetType(bool type);
    void SetAckNumber(uint16_t ackNum);

    // Getters
    void GetAllCreditsVL(uint8_t credits[16]) const;
    uint8_t GetLength() const;      // 返回固定值 0x01
    uint8_t GetConfig() const;      // 返回固定值 0x00
    uint8_t GetControl() const;     // 返回固定值 0x02
    uint8_t GetSubControl() const;  // 返回固定值 0x04
    bool GetSD() const;
    bool GetType() const;
    uint16_t GetAckNumber() const;

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream &os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

private:
    // 报文头字段
    // 总长度: (Length + 1) * 8 * 20 = 320 bits = 40 bytes = 2 flits

    // 第1字节: [FirstBit:1][Length(fixed 00001):5][Fixed 100000前2位:2]
    // 第2字节: [Fixed 100000后4位:4][Config(fixed 0000):4]
    bool firstBit = false;         // 1 bit (always 0)
    uint8_t m_length = 0x01;       // 5 bits (fixed 00001)
    uint8_t fixedPattern = 0x20;   // 6 bits (fixed 100000)
    uint8_t m_config = 0x00;

    // 第3字节: [Control(fixed 0010):4][Sub Control(fixed 0100):4]
    uint8_t m_controlType = 0x02;     // 4 bits (fixed 0010)
    uint8_t m_subControlType = 0x04;  // 4 bits (fixed 0100)

    // 第4字节: [SD:1][Reserved:6][Type:1]
    bool m_sd = true;            // 1 bit: 1 for credit initiation
    uint8_t reservE1Value = 0;   // 6 bit:reserve字段的固定值
    bool m_type = true;          // 1 bit: when m_sd == 1, 1 means initiation completed

    // 字节5-6: [Ack Number:16]
    uint16_t m_ackNumber = 0;

    // 字节7-18: [Credit Number:96] (12字节)
    // 16个VL，每个VL 6位，总共96位
    // 实际存储时简化为每个VL用1字节，但只使用低6位
    uint8_t m_creditVL[UB_PRIORITY_NUM_DEFAULT];  // 每个VL的credit值 (0-63，6位)

    // 字节19-40: Reserve字段 (22字节) - 在序列化时填充0
    uint32_t totalHeaderSize = 40;
    uint32_t usedBytes = 18;
    uint32_t reserveSize = totalHeaderSize - usedBytes;
    uint8_t reserveFillValue = 0;  // reserve字段填充值
};

/**
 * \ingroup ub-header
 * \brief Packet header for UB DataLink Packet Header (LPH)
 *
 * 简易实现
 * 报文头格式：total 32 bits
 *              [Credit:1][ACK:1][Credit Target VL:4][Reverse:1]
 *              [VL of this packet:4][Reverse:1][Config(fixed 0011):4]
 *              [Load Balance Mode(0:per flow/1:per packet):1][Routing Policy(0: all paths/1: shortest paths):1]
 *              [Packet Length in block:4][Last Block Length in flit:5]
 *              [Tail Payload Length in bytes:5]
 *
 *              [Load Balance Mode][Routing Policy]合称RT字段
 */
class UbDatalinkPacketHeader : public Header {
public:
    UbDatalinkPacketHeader();
    virtual ~UbDatalinkPacketHeader();

    // Setters
    void SetCredit(bool credit);              // 报文是否返回信用证
    void SetACK(bool ack);                    // 报文是否释放retry buffer空间
    void SetCreditTargetVL(uint8_t vlIndex);  // 4 bits: 指定接收credit的VL
    void SetPacketVL(uint8_t vl);             // 4 bits: 数据包的VL
    void SetConfig(uint8_t config);
    void SetLoadBalanceMode(bool mode);  // 1 bit: 0=per flow, 1=per packet
    void SetRoutingPolicy(bool policy);  // 1 bit: 0=all paths, 1=shortest paths

    // Getters
    bool GetCredit() const;
    bool GetACK() const;
    uint8_t GetCreditTargetVL() const;  // 4 bits: 返回接收credit的VL索引
    uint8_t GetPacketVL() const;        // 4 bits: 返回数据包的VL
    bool GetLoadBalanceMode() const;    // 1 bit: 返回负载均衡模式
    bool GetRoutingPolicy() const;      // 1 bit: 返回路由策略
    uint8_t GetConfig() const;          // 返回固定值 0x03 (0011)
    bool IsUbDatalinkControlCreditHeader();

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream &os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

private:
    // 报文头字段 - 总共32位 (4字节)

    // 字节0: [Credit:1][ACK:1][Credit Target VL:4][Reserve1:1][PacketVL高1位:1]
    bool m_credit = 0;                                  // 1 bit: Credit indication
    bool m_ack = 0;                                     // 1 bit: ACK indication
    uint8_t m_creditTargetVL = UB_PRIORITY_DEFAULT;     // 4 bits: Target VL for credit (VL index 0-15)
    uint8_t reservE1Value = 0;                          // 1 bit: Reserved
    
    // 字节1: [PacketVL低3位:3][Reserve2:1][Config(fixed 0011):4]
    uint8_t m_packetVL = UB_PRIORITY_DEFAULT;        // 4 bits: VL of this packet (0-15)
    uint8_t reservE2Value = 0;                       // 1 bit: Reserved
    uint8_t m_config = 0b0011;                       // 4 bits: Config (fixed 0011)
    
    // 字节2: [Load Balance Mode:1][Routing Policy:1][Packet Length in block (ignored):4][Last Block Length高2位:2]
    bool m_loadBalanceMode = LB_MODE_PER_FLOW;       // 1 bit: 0=per flow, 1=per packet
    bool m_routingPolicy = ROUTING_SHORTEST;         // 1 bit: 0=all paths, 1=shortest paths
    // 未完成字段：Packet Length in block (4 bits) - 序列化时填充0
    // 未完成字段：Last Block Length高2位 (2 bits) - 序列化时填充0
    
    // 字节3: [Last Block Length低3位:3][Tail Payload Length:5]
    // 未完成字段：Last Block Length低3位 (3 bits) - 序列化时填充0
    // 未完成字段：Tail Payload Length (5 bits) - 序列化时填充0
    
    // 常量定义
    uint32_t totalHeaderSize = 4;   // 32 bits = 4 bytes
    uint8_t ignoredFieldValue = 0;  // 未完成字段的填充值
};

/**
 * \ingroup ub-header
 * \brief UB Network Header (kind of an extension of IP header)
 *
 * 报文位置：[Datalink Packet Header: 4bytes][Network Header: 6bytes][IPv4/v6 header][others]
 * IP头改动（sport = LB, dport = 4792）
 * 报文头格式：总计6字节
 *      字节0-1：(拥塞控制字段)
 *      [Mode:3][depend on mode:13]
 *      +[CAQM: 000][Location:1][reserved:1][enable:1][C:1][I:1][HINT:8]
 *      +[FECN_RTT: 010][Location:1][Time stamp:10][FECN:2]
 *      +[FECN: 100][Location:1][reserved:10][FECN:2]
 *
 *      字节2：
 *      [reserved:7]
 *      字节3-5：
 *      [NPI:25] (Network Partition Identifier)
 */
class UbIpBasedNetworkHeader : public Header {
public:
    UbIpBasedNetworkHeader();
    virtual ~UbIpBasedNetworkHeader();

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream &os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    // Setters
    void SetMode(uint8_t mode);
    void SetLocation(bool location);
    void SetEnable(bool enable);
    void SetC(uint8_t c);
    void SetI(uint8_t i);
    void SetHint(uint8_t hint);
    void SetTimeStamp(uint16_t ts);
    void SetFecn(uint8_t fecn);
    void SetNpi(uint32_t npi);

    // Getters
    uint8_t GetMode() const;
    bool GetLocation() const;
    bool GetEnable() const;
    uint8_t GetC() const;
    uint8_t GetI() const;
    uint8_t GetHint() const;
    uint16_t GetTimeStamp() const;
    uint8_t GetFecn() const;
    uint32_t GetNpi() const;

private:
    // 字节0-1: 拥塞控制字段
    uint8_t m_mode = 0;  // 3 bits
    union {
        struct {  // mode == 0b000 (CAQM)
            bool location;      // 1 bit
            bool reserved1;     // 1 bit
            bool enable;        // 1 bit
            bool c;            // 1 bit
            bool i;            // 1 bit
            uint8_t hint;      // 8 bits
        } mode0;
        struct {  // mode == 0b010 (FECN_RTT)
            bool location;          // 1 bit
            uint16_t timestamp;     // 10 bits
            uint8_t fecn;          // 2 bits
        } mode2;
        struct {  // mode == 0b100 (FECN)
            bool location;          // 1 bit
            uint16_t reserved2;     // 10 bits
            uint8_t fecn;          // 2 bits
        } mode4;
        uint16_t raw13;  // 13 bits as raw
    } m_fields;

    // 字节2: 保留字段
    uint8_t m_reserved = 0;  // 7 bits

    // 字节3-5: 网络分区标识符
    uint32_t m_npi = 0;  // 25 bits (Network Partition Identifier)

    // 头部总长度
    static const uint32_t totalHeaderSize = 6;
};

/**
 * \ingroup ub-header
 * \brief UB 16-Bit Network Header (a compact UB-specified network header)
 *
 * 报文位置：[Datalink Packet Header: 4bytes][Network Header: 8bytes][TAH][Payload]
 * 报文头格式：总计8字节
 *      字节0-1：[SCNA:16]
 *      字节2-3：[DCNA:16]
 *      字节4-5：[CC:16]
 *      [Mode:3][depend on mode:13]
 *      +[CAQM: 000][Location:1][reserved:1][enable:1][C:1][I:1][HINT:8]
 *      +[FECN_RTT: 010][Location:1][Time stamp:10][FECN:2]
 *      +[FECN: 100][Location:1][reserved:10][FECN:2]
 *      字节6：[LB:8]
 *      字节7：[Service Level:4][Management:1][NLP:3]
 *
 *      CNA[高位: nodeId][低4位: portId] CnaToIp IpToCna
 */
class UbCna16NetworkHeader : public Header {
public:
    UbCna16NetworkHeader();
    virtual ~UbCna16NetworkHeader();

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream &os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    // Setters
    void SetScna(uint16_t scna);
    void SetDcna(uint16_t dcna);
    void SetMode(uint8_t mode);
    void SetLocation(bool location);
    void SetEnable(bool enable);
    void SetC(bool c);
    void SetI(bool i);
    void SetHint(uint8_t hint);           // 7 bits
    void SetTimestamp(uint16_t ts);       // 10 bits
    void SetFecn(uint8_t fecn);           // 2 bits
    void SetLb(uint8_t lb);
    void SetServiceLevel(uint8_t sl);     // 4 bits
    void SetNlp(uint8_t nlp);             // 3 bits

    // Getters
    uint16_t GetScna() const;
    uint16_t GetDcna() const;
    uint8_t GetMode() const;
    bool GetLocation() const;
    bool GetEnable() const;
    bool GetC() const;
    bool GetI() const;
    uint8_t GetHint() const;
    uint16_t GetTimestamp() const;
    uint8_t GetFecn() const;
    uint8_t GetLb() const;
    uint8_t GetServiceLevel() const;
    uint8_t GetNlp() const;

    // 验证
    bool IsValidMode() const;

private:
    // Byte0-1
    uint16_t m_scna = 0;
    // Byte2-3
    uint16_t m_dcna = 0;

    // Byte4-5: Congestion Control (与 UbIpBasedNetworkHeader 复用结构思想)
    uint8_t m_mode = 0; // 3 bits
    union {
        struct {  // mode 000
            bool location;
            bool reserve1;
            bool enable;
            bool c;
            bool i;
            uint8_t hint;  // 7
        } mode0;
        struct {  // mode 010
            bool location;
            uint16_t timestamp;  // 10
            uint8_t fecn;        // 2
        } mode2;
        struct {                // mode 100
            bool location;
            uint16_t reserve3;  // 10
            uint8_t fecn;       // 2
        } mode4;
        uint16_t raw13;
    } m_ccFields;

    // Byte6
    uint8_t m_lb = 0; // hash时可以使用(SCNA, DCNA, LB)实现负载均衡

    // Byte7: [ServiceLevel:4][management:1][NLP:3]
    uint8_t m_serviceLevel = 0;
    uint8_t m_management = 0;
    uint8_t m_nlp = 0;

    static const uint32_t totalHeaderSize = 8;
};

/**
 * \ingroup ub-header
 * \brief UB 24-Bit Network Header (a compact UB-specified network header)
 *
 * 报文位置：[Datalink Packet Header: 4bytes][Network Header: 10bytes][TAH][Payload]
 * 报文头格式：总计10字节
 *      字节0-2：[SCNA:24]
 *      字节3-5：[DCNA:24]
 *      字节6-7：[CC:16]
 *      [Mode:3][depend on mode:13]
 *      +[CAQM: 000][Location:1][reserved:1][enable:1][C:1][I:1][HINT:8]
 *      +[FECN_RTT: 010][Location:1][Time stamp:10][FECN:2]
 *      +[FECN: 100][Location:1][reserved:10][FECN:2]
 *      字节8：[LB:8]
 *      字节9：[Service Level:4][reserved:1][NLP:3]
 *
 *      CNA[高位: nodeId][低8位: portId] CnaToIp IpToCna
 */
 class UbCna24NetworkHeader : public Header {
    public:
        UbCna24NetworkHeader();
        virtual ~UbCna24NetworkHeader();
    
        static TypeId GetTypeId(void);
        TypeId GetInstanceTypeId(void) const override;
        void Print(std::ostream &os) const override;
        void Serialize(Buffer::Iterator start) const override;
        uint32_t Deserialize(Buffer::Iterator start) override;
        uint32_t GetSerializedSize(void) const override;
    
        // Setters
        void SetScna(uint32_t scna);
        void SetDcna(uint32_t dcna);
        void SetMode(uint8_t mode);
        void SetLocation(bool location);
        void SetEnable(bool enable);
        void SetC(bool c);
        void SetI(bool i);
        void SetHint(uint8_t hint);           // 7 bits
        void SetTimestamp(uint16_t ts);       // 10 bits
        void SetFecn(uint8_t fecn);           // 2 bits
        void SetLb(uint8_t lb);
        void SetServiceLevel(uint8_t sl);     // 4 bits
        void SetNlp(uint8_t nlp);             // 3 bits
    
        // Getters
        uint32_t GetScna() const;
        uint32_t GetDcna() const;
        uint8_t GetMode() const;
        bool GetLocation() const;
        bool GetEnable() const;
        bool GetC() const;
        bool GetI() const;
        uint8_t GetHint() const;
        uint16_t GetTimestamp() const;
        uint8_t GetFecn() const;
        uint8_t GetLb() const;
        uint8_t GetServiceLevel() const;
        uint8_t GetNlp() const;
    
        // 验证
        bool IsValidMode() const;
    
    private:
        // Byte0-2
        uint32_t m_scna = 0;
        // Byte3-5
        uint32_t m_dcna = 0;
    
        // Byte6-7: Congestion Control (与 UbIpBasedNetworkHeader 复用结构思想)
        uint8_t m_mode = 0; // 3 bits
        union {
            struct {  // mode 000
                bool location;
                bool reserve1;
                bool enable;
                bool c;
                bool i;
                uint8_t hint;  // 7
            } mode0;
            struct {  // mode 010
                bool location;
                uint16_t timestamp;  // 10
                uint8_t fecn;        // 2
            } mode2;
            struct {                // mode 100
                bool location;
                uint16_t reserve3;  // 10
                uint8_t fecn;       // 2
            } mode4;
            uint16_t raw13;
        } m_ccFields;
    
        // Byte8
        uint8_t m_lb = 0; // hash时可以使用(SCNA, DCNA, LB)实现负载均衡
    
        // Byte9: [ServiceLevel:4][reserved:1][NLP:3]
        uint8_t m_serviceLevel = 0;
        bool m_reserve = false;
        uint8_t m_nlp = 0;
    
        static const uint32_t totalHeaderSize = 10;
    };

class UbCtpHeader : public Header
{
  public:
    UbCtpHeader();
    ~UbCtpHeader() override;

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream& os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    void SetTPOpcode(uint8_t opcode);
    void SetTPOpcode(CtpOpcode opcode);
    void SetPadding(uint8_t padding);
    void SetNlp(uint8_t nlp);
    uint8_t GetTPOpcode() const;
    uint8_t GetPadding() const;
    uint8_t GetNlp() const;

  private:
    uint8_t m_tpOpcode{0};
    uint8_t m_padding{0};
    uint8_t m_nlp{0};
};

/**
 * \ingroup ub-header
 * \brief UB compact UPI header, 16-bit form used by CTPH.NLP=0x2.
 */
class UbCompactUpiHeader : public Header
{
  public:
    UbCompactUpiHeader();
    ~UbCompactUpiHeader() override;

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream& os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    void SetUpi(uint16_t upi);
    uint16_t GetUpi() const;

  private:
    uint16_t m_upi{0};
};

/**
 * \ingroup ub-header
 * \brief UB compact EID header, 20-bit SEID followed by 20-bit DEID.
 */
class UbCompactEidHeader : public Header
{
  public:
    UbCompactEidHeader();
    ~UbCompactEidHeader() override;

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream& os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    void SetSourceEid(uint32_t eid);
    void SetDestinationEid(uint32_t eid);
    uint32_t GetSourceEid() const;
    uint32_t GetDestinationEid() const;

  private:
    uint32_t m_sourceEid{0};
    uint32_t m_destinationEid{0};
};

/**
 * \ingroup ub-header
 * \brief UB Transport Header (RTPH)
 *
 * 报文头格式：总计16字节 (128位)
 *      第一个64位块：
 *      [TPOpcode:8][TPVer:2][Padding:2][NLP:4][SrcTpn:24][DestTpn:24]
 *
 *      第二个32位块：
 *      [Ack request:1][Error Flag:1][reserved:6][PSN:24]
 *
 *      第三个32位块：
 *      [RS PST(ignored):3][RSP INFO(ignored):5][TP MSN:24]
 *
 *      TPOpcode[0:6]:
 *                    0x0: unreliable TA Packet
 *                    0x1: reliable TA Packet
 *                    0x2: TP ACK without CETPH (Congestion Extended TPH)
 *                    0x3: TP ACK with CETPH (Implies CETPH as NLP)
 *                    0x4: reserved
 *                    0x5: TP SACK without CETPH (Implies SA(Selective Acknowledge)ETPH as NLP)
 *                    0x6: TP SACK with CETPH (Implies CETPH as NLP)
 *                    0x7: reserved
 *                    0x8: CNP (Congestion Notification Packet) (Implies CNPETPH as NLP)
 *      NLP[0:6]: (Next layer protocol for TPOpcode[0:6]:0x0, 0x1, 0x2, 024, 0x7)
 *                    0x0: TAH (Transaction Header)
 *                    0x1: UPI + UEID (虚拟化相关, IGNORED)
 *                    0x2: reserved
 *                    0x3: CIP (confidentiality and integrity protection, IGNORED)
 */
class UbTransportHeader : public Header {
public:
    UbTransportHeader();
    virtual ~UbTransportHeader();

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream &os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    // 提供友好的设置TPOpcode和NLP的方法
    void SetLastPacket(bool lastPkt);
    void SetTPOpcode(TpOpcode opcode);
    void SetTPOpcode(uint8_t opcode);
    void SetNLP(NextLayerProtocol nlp);
    void SetNLP(uint8_t nlp);
    void SetSrcTpn(uint32_t srcTpn);
    void SetDestTpn(uint32_t destTpn);
    void SetAckRequest(bool ackRequest);
    void SetErrorFlag(bool errorFlag);
    void SetPsn(uint32_t psn);
    void SetTpMsn(uint32_t tpMsn);
    void SetRspSt(uint8_t rspSt);
    void SetRspInfo(uint8_t rspInfo);

    // Getters for header fields
    bool GetLastPacket() const;
    uint8_t GetTPOpcode() const;
    uint8_t GetNLP() const;
    uint32_t GetSrcTpn() const;
    uint32_t GetDestTpn() const;
    bool GetAckRequest() const;
    bool GetErrorFlag() const;
    uint32_t GetPsn() const;
    uint32_t GetTpMsn() const;
    uint8_t GetRspSt() const;
    uint8_t GetRspInfo() const;

    // 检查头字段是否有效的方法
    bool IsValidOpcode() const;
    bool IsValidNLP() const;

private:
    // First 64 bits: [Last packet:1][TPOpcode:7][TPVer:2][Pad:2][NLP:4][SrcTpn:24][DestTpn:24]
    bool m_lastPacket = false;
    uint8_t m_tpOpcode = static_cast<uint8_t>(TpOpcode::TP_OPCODE_RELIABLE_TA);  // 7 bits
    uint8_t m_tpVer = 0;                                               // 2 bits (stored in uint8_t)
    // 4 bits (stored in uint8_t): Next Layer Protocol
    uint8_t m_nlp = static_cast<uint8_t>(NextLayerProtocol::NLP_TAH);
    uint32_t m_srcTpn = 0xFFFFFF;
    uint32_t m_destTpn = 0xFFFFFF;

    // Second 32 bits: [Ack request:1][Error Flag:1][Message number:4][PSN:24]
    bool m_ackRequest = true;     // 1 bit: (default 1)
    bool m_errorFlag = false;     // 1 bit:
    uint8_t m_reserve1 = 0;       // 2 bit:
    uint32_t m_psn = 0xFFFFFF;    // 24 bits (stored in uint32_t)

    // Third 32 bits: [RSPST:3][RSPINFO:5][TPMSN:24]
    uint8_t m_rspSt = 0;          // 3 bits (stored in uint8_t)
    uint8_t m_rspInfo = 0;        // 5 bits (stored in uint8_t): TA层完成状态
    uint32_t m_tpMsn = 0xFFFFFF;  // 24 bits (stored in uint32_t)
};


/**
 * \ingroup ub-header
 * \brief UB Selective Acknowledge Extended Transport Header (SAETPH)
 */
class UbSelectiveAckExtTph : public Header {
public:
    UbSelectiveAckExtTph();
    virtual ~UbSelectiveAckExtTph();

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream& os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    void SetBitmapBitCount(uint32_t bitCount);
    uint32_t GetBitmapBitCount() const;
    uint8_t GetEncodedBitmapSize() const;
    void SetMaxRcvPsn(uint32_t psn);
    uint32_t GetMaxRcvPsn() const;
    void SetBitmapBit(uint32_t offset, bool value);
    bool GetBitmapBit(uint32_t offset) const;

    static bool IsSupportedBitmapBitCount(uint32_t bitCount);
    static bool IsSupportedEncodedBitmapSize(uint8_t encoded);

private:
    static constexpr uint32_t kMaxBitmapBits = 1024;
    static constexpr uint32_t kMaxBitmapBytes = kMaxBitmapBits / 8;
    static constexpr uint32_t kFixedHeaderBytes = 4;

    static uint32_t DecodeBitmapBitCount(uint8_t encoded);
    static bool EncodeBitmapBitCount(uint32_t bitCount, uint8_t& encoded);

    uint8_t m_bitmapSizeEncoded{0};
    uint32_t m_maxRcvPsn{0};
    std::array<uint8_t, kMaxBitmapBytes> m_bitmap{};
};

/**
 * \ingroup ub-header
 * \brief UB Congestion extend transport header (CETPH)
 *
 * 报文头格式：总计8字节
 *      字节0-3:[ACK Sequence:32]
 *      字节4-7:根据拥塞算法不同而不同
 *
 *      CAQM算法 (当前实现):
 *      字节4:[Reserved:6][Location:1][I:1]
 *      字节5:[C:8]
 *      字节6-7:[Hint:16]
 *
 *      其他算法可通过union扩展实现
 */
class UbCongestionExtTph : public Header {
public:
    UbCongestionExtTph();
    virtual ~UbCongestionExtTph();

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream &os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    // Common Setters/Getters for all algorithms
    void SetAckSequence(uint32_t ackSeq);  // 设置ACK序列号 (32位)
    uint32_t GetAckSequence() const;       // 获取ACK序列号

    // CAQM specific methods
    void SetLocation(bool location);  // 设置Location位 (1位) - CAQM
    void SetI(bool i);                // 设置I位 (1位) - CAQM
    void SetC(uint8_t c);             // 设置C字段 (8位) - CAQM
    void SetHint(uint16_t hint);      // 设置Hint字段 (16位) - CAQM
    bool GetLocation() const;         // 获取Location位 - CAQM
    bool GetI() const;                // 获取I位 - CAQM
    uint8_t GetC() const;             // 获取C字段 - CAQM
    uint16_t GetHint() const;         // 获取Hint字段 - CAQM

    // Raw access for future algorithm extensions
    void SetRawBytes4to7(uint32_t rawValue);  // 直接设置字节4-7 (用于其他算法)
    uint32_t GetRawBytes4to7() const;         // 直接获取字节4-7 (用于其他算法)

private:
    // 报文头字段 - 总共8字节 (64位)

    // 字节0-3: [ACK Sequence:32] - 所有算法通用
    uint32_t m_ackSequence = 0;                  // 32 bits: ACK序列号

    // 字节4-7: 根据拥塞算法不同而不同
    union {
        // CAQM算法字段布局
        struct {
            uint8_t reserved : 6;  // 6 bits: 保留字段 (固定为0)
            bool location : 1;     // 1 bit: Location标志位
            bool i : 1;            // 1 bit: I标志位
            uint8_t c;             // 8 bits: C字段
            uint16_t hint;         // 16 bits: Hint字段
        } caqm;

        // 原始32位访问，用于其他算法扩展
        uint32_t raw;  // 32 bits: 字节4-7的原始值

        // 字节级访问，便于调试和灵活操作
        uint8_t bytes[4];  // 4 bytes: 字节4-7的字节级访问
    } m_congestionFields;

    // 常量定义
    static const uint32_t totalHeaderSize = 8; // 总头部大小 (8字节)
};

/**
 * \ingroup ub-header
 * \brief UB CNP extended transport header
 *
 * 报文头格式：总计16字节
 *      字节0:[ECN:2][Location:1][Reserved:5]
 *      字节1-15: Reserved
 */
class UbCnpExtTph : public Header {
public:
    UbCnpExtTph();
    virtual ~UbCnpExtTph();

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream& os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    void SetEcn(uint8_t ecn);
    uint8_t GetEcn() const;
    void SetLocation(bool location);
    bool GetLocation() const;

private:
    uint8_t m_ecn{0};
    bool m_location{false};

    static const uint32_t totalHeaderSize = 16;
};

/**
 * \ingroup ub-header
 * \brief UB Transaction Header (TAH)
 *
 * 报文头格式：总计8字节 (64位)
 *      字节0: [TaOpcode:8]
 *      字节1: [TaVer(fixed 0x00):2][EE:2][TV_EN:1][POISON:1][reserved:1][UD_FLG:1]
 *      字节2-3: [Initiator TaSsn:16]
 *      字节4: [noTAACK:1][Order:3][MT_EN en:1][FCE:1][Retry:1][Alloc:1]
 *      字节5: [Reserved:1][Exclusive:1][Initiator RC Type:2][Initiator RC ID高4位:4]
 *      字节6-7: [Initiator RC ID低16位:16]
 *
 *      TaOpcode[0:255]: 操作码定义见枚举TaOpcode
 *      Order[0:3]: 保序要求定义见枚举OrderType
 *      Initiator RC Type[0:2]: 源Jetty类型定义见枚举IniRcType
 */
class UbTransactionHeader : public Header {
public:
    UbTransactionHeader();
    virtual ~UbTransactionHeader();

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream &os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    // Setters (只包含非ignore和非固定字段)
    void SetTaOpcode(TaOpcode opcode);
    void SetTaOpcode(uint8_t opcode);
    void SetIniTaSsn(uint16_t ssn);
    void SetOrder(OrderType order);
    void SetOrder(uint8_t order);
    void SetTcETahEn(bool enable);
    void SetExclusive(bool exclusive);
    void SetIniRcType(IniRcType type);
    void SetIniRcType(uint8_t type);
    void SetIniRcId(uint32_t jettyNum);

    // Getters (只包含非ignore和非固定字段)
    uint8_t GetTaOpcode() const;
    uint16_t GetIniTaSsn() const;
    uint8_t GetOrder() const;
    bool GetExclusive() const;
    uint8_t GetIniRcType() const;
    uint32_t GetIniRcId() const;

private:
    // 报文头字段 - 总共8字节 (64位)
    
    // 字节0: [TaOpcode:8]
    uint8_t m_taOpcode = static_cast<uint8_t>(TaOpcode::TA_OPCODE_WRITE);  // 8 bits: Transaction opcode

    // 字节1: [TaVer(fixed 0x00):2][EE:2][TV_EN:1][POISON:1][reserved:1][UD_FLG:1]
    uint8_t m_taVer = 0x00;      // 2 bits: Ta协议版本号
    uint8_t m_ee = 0x00;         // 2 bits: EE
    bool m_tvEn = false;         // 1 bit: 是否包含TVETAH扩展头
    bool m_poison = false;       // 1 bit: POISON
    uint8_t m_reserveByte1 = 0;  // 1 bit
    bool m_udFlg = false;        // 1 bit: 是否携带UDETAH

    // 字节2-3: [Src TaSsn:16]
    uint16_t m_iniTaSsn = 0xFFFF;  // 16 bits: Initiator Transaction Session Number

    // 字节4: [noTAACK:1][Order:3][MT_EN en:1][FCE:1][Retry:1][Alloc:1]
    bool m_noTaAck = false;      // 1 bit: 是否返回TaACK，仅传输层使用CTP有效
    uint8_t m_order = static_cast<uint8_t>(OrderType::ORDER_NO);  // 3 bits: Order type
    bool m_mtEn = false;     // 1 bit: 是否包含MTETAH扩展头
    bool m_fce = false;           // 1 bit: 用于Responder的CQE判断是否产生CE (not used currently)
    bool m_retry = false;        // 1 bit: 为0时表示请求为首次发送，否则为重传报文 (not used currently)
    bool m_alloc = false;        // 1 bit: (not used currently)

    // 字节5: [Reserved:1][Exclusive:1][Src Jetty type:2][Src Jetty高4位:4]
    uint8_t m_reserveByte5 = 0;  // 1 bit: 字节5中的Reserve字段
    bool m_exclusive = false;    // 1 bit: H2H场景时固定为0，H2D或D2H场景时固定为1 (not used currently)
    uint8_t m_iniRcIdType = static_cast<uint8_t>(IniRcType::REQUESTER_CONTEXT);  // 2 bits: Source Jetty Type (not used currently)

    // 字节6-7: [Src Jetty低16位:16] + 字节5的高4位组成完整的20位Jetty
    uint32_t m_iniRcIdId = 0xFFFFF;  // 20 bits: 对应发起端的 JettyNum

    // 尺寸和边界常量定义
    uint32_t totalHeaderSize = 8;  // 总头部大小 (8字节)
};

/**
 * \ingroup ub-header
 * \brief UB Memory Access Extended Transaction Header (MAETAH)
 *
 * 报文头格式：总计16字节 (128位)
 *      字节0-7: [Virtual Address:64]
 *      字节8-11: [Reserved:4][Token ID:20][Reserved:8]
 *      字节12-15: [Length:32]
 *
 */
class UbMAExtTah : public Header {
public:
    UbMAExtTah();
    virtual ~UbMAExtTah();

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream &os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    // Setters
    void SetVirtualAddress(uint64_t virtualAddr);  // 设置虚拟地址 (64位)
    void SetTokenId(uint32_t tokenId);             // 设置Token ID (20位)
    void SetLength(uint32_t length);               // 设置长度 (32位)

    // Getters
    uint64_t GetVirtualAddress() const;  // 获取虚拟地址
    uint32_t GetTokenId() const;         // 获取Token ID
    uint32_t GetLength() const;          // 获取长度

    // 验证方法
    bool IsValidTokenId() const;  // 验证Token ID是否在有效范围内

private:
    // 报文头字段 - 总共16字节 (128位)
    
    // 字节0-7: [Virtual Address:64]
    uint64_t m_virtualAddress = 0;                    // 64 bits: 虚拟地址
    
    // 字节8-11: [Reserved:4][Token ID:20][Reserved:8]
    uint8_t m_reserved1 = 0;                          // 4 bits: 保留字段1 (高4位)
    uint32_t m_tokenId = 0;                           // 20 bits: Token ID
    uint8_t m_reserved2 = 0;                          // 8 bits: 保留字段2
    
    // 字节12-15: [Length:32]
    uint32_t m_length = 0;                            // 32 bits: 访问target内存的数据长度
    
    // 常量定义
    static const uint32_t totalHeaderSize = 16;    // 总头部大小 (16字节)
    static const uint32_t maxTokenId = 0xFFFFF;    // Token ID最大值 (20位: 0xFFFFF)
};

/**
 * \ingroup ub-header
 * \brief UB Memory Access Extended Transaction Header (MAETAH)
 *
 * 报文头格式：总计12字节 (96位) - 精简版本
 *      字节0-7: [Virtual Address:58][Affinity Hint:2][Strong Order:1][Length:3]
 *      字节8-11: [Reserved:4][Token ID:20][Reserved:8]
 *
 */
class UbCompactMAExtTah : public Header {
public:
    UbCompactMAExtTah();
    virtual ~UbCompactMAExtTah();

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream &os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    // Setters
    void SetVirtualAddress(uint64_t virtualAddr);
    void SetTokenId(uint32_t tokenId);
    void SetStrongOrder(bool strongOrder);
    void SetLength(uint8_t length);

    // Getters
    uint64_t GetVirtualAddress() const;
    uint32_t GetTokenId() const;
    bool GetStrongOrder() const;
    uint8_t GetLength() const;

    // 验证方法
    bool IsValidTokenId() const;                     // 验证Token ID是否在有效范围内

private:
    // 报文头字段 - 总共12字节 (96位)
    
    // 字节0-7: [Virtual Address:58][Affinity Hint:2][Strong Order:1][Length:3]
    uint64_t m_virtualAddress = 0;                    // 58 bits: 虚拟地址
    uint8_t m_affinityHint = 0;                    // 2 bits: 指示Target数据访问或处理的亲和性
    bool m_strongOrder = 0;                    // 1 bits: 保序执行标记
    uint8_t m_length = 0;                    // 1 bits: 访问target内存的数据长度，计算为为64B*(2^m_length)
    
    // 字节8-11: [Reserved:4][Token ID:20][Reserved:8]
    uint8_t m_reserved1 = 0;                          // 4 bits: 保留字段1 (高4位)
    uint32_t m_tokenId = 0;                           // 20 bits: Token ID
    uint8_t m_reserved2 = 0;                          // 8 bits: 保留字段2
    
    // 常量定义
    static const uint32_t totalHeaderSize = 12;    // 总头部大小 (12字节)
    static const uint32_t maxTokenId = 0xFFFFF;    // Token ID最大值 (20位: 0xFFFFF)
};

/**
 * \ingroup ub-header
 * \brief UB Acknowledge Transaction Header (ATAH)
 *
 * 报文头格式：总计8字节 (64位)
 *      字节0: [TA OpCode:8]
 *      字节1: [TA Version:2][Reserved:2][SV:1][Poison:1][Reserved:2]
 *      字节2-3: [Initiator TASSN:8]
 *      字节4: [RSP Status:3][RSP INFO:5]
 *      字节5-7: [Reserved:2][INI RC TYPE:2][INI RC ID:20]
 *
 */
class UbAckTransactionHeader : public Header {
public:
    UbAckTransactionHeader();
    virtual ~UbAckTransactionHeader();

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream &os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    // Setters
    void SetTaOpcode(TaOpcode opcode);
    void SetTaOpcode(uint8_t opcode);
    void SetTaVersion(uint8_t version);
    void SetSV(uint8_t sv);
    void SetPoison(bool poison);
    void SetIniTaSsn(uint16_t tassn);
    void SetRspStatus(uint8_t status);
    void SetRspInfo(uint8_t info);
    void SetIniRcType(uint8_t type);
    void SetIniRcId(uint32_t id);

    // Getters
    uint8_t GetTaOpcode() const;
    uint8_t GetTaVersion() const;
    uint8_t GetSV() const;
    bool GetPoison() const;
    uint16_t GetIniTaSsn() const;
    uint8_t GetRspStatus() const;
    uint8_t GetRspInfo() const;
    uint8_t GetIniRcType() const;
    uint32_t GetIniRcId() const;

    // 验证方法
    bool IsValidOpcode() const;
    bool IsValidRspStatus() const;
    bool IsValidIniRcType() const;

private:
    // 报文头字段 - 总共8字节 (64位)
    
    // 字节0: [TA OpCode:8]
    uint8_t m_taOpcode = static_cast<uint8_t>(TaOpcode::TA_OPCODE_TRANSACTION_ACK);   // 8 bits: TaOpCode 0x11~0x13
    // 字节1: [TA Version:2][Reserved:2][SV:2][Poison:1][Reserved:1]
    uint8_t m_taVersion = 0;                          // 2 bits: TA版本
    uint8_t m_reserved1 = 0;                          // 2 bits: 保留字段1
    uint8_t m_sv = 0;                                 // 1 bits: SV字段
    bool m_poison = false;                            // 1 bit: Poison标志
    uint8_t m_reserved2 = 0;                          // 1 bit: 保留字段2
    
    // 字节2-3: [Initiator TASSN:16]
    uint16_t m_iniTaSsn = 0;                          // 16 bits
    
    // 字节4: [RSP Status:3][RSP INFO:5]
    uint8_t m_rspStatus = 0;                          // 3 bits: 响应状态
    uint8_t m_rspInfo = 0;                            // 5 bits: 响应信息
    
    // 字节5-7: [Reserved:2][INI RC TYPE:2][INI RC ID:12]
    uint8_t m_reserved3 = 0;                          // 2 bits: 保留字段3
    uint8_t m_iniRcType = 0;                          // 2 bits: INI RC类型
    uint16_t m_iniRcId = 0;                           // 12 bits: INI RC ID
    
    // 常量定义
    static const uint32_t totalHeaderSize = 8;        // 总头部大小 (8字节)
};


/**
 * \ingroup ub-header
 * \brief UB Compact Acknowledge Transaction Header (ATAH)
 *
 * 报文头格式：总计4字节 (32位)
 *      字节0: [TA OpCode:8]
 *      字节1: [TA Version:2][Status:2][reserved:1][Poison:1][Reserved:2]
 *      字节2-3: [Initiator TASSN:8]
 *
 */
class UbCompactAckTransactionHeader : public Header {
public:
    UbCompactAckTransactionHeader();
    virtual ~UbCompactAckTransactionHeader();

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream &os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    // Setters
    void SetTaOpcode(TaOpcode opcode);       // 设置TA操作码 (8位)
    void SetTaOpcode(uint8_t opcode);        // 设置TA操作码 (8位)
    void SetTaVersion(uint8_t version);      // 设置TA版本 (2位)
    void SetPoison(bool poison);             // 设置Poison位 (1位)
    void SetIniTaSsn(uint16_t tassn);  // 设置发起者TASSN (16位)

    // Getters
    uint8_t GetTaOpcode() const;         // 获取TA操作码
    uint8_t GetTaVersion() const;        // 获取TA版本
    bool GetPoison() const;              // 获取Poison位
    uint16_t GetIniTaSsn() const;  // 获取发起者TASSN

    // 验证方法
    bool IsValidOpcode() const;  // 验证操作码是否有效

private:
    // 报文头字段 - 总共4字节 (32位)
    
    // 字节0: [TA OpCode:8]
    uint8_t m_taOpcode = static_cast<uint8_t>(TaOpcode::TA_OPCODE_TRANSACTION_ACK);   // 8 bits: TaOpCode 0x11~0x13
    
    // 字节1: [TA Version:2][Status:2][reserved:1][Poison:1][Reserved:2]
    uint8_t m_taVersion = 0;                            // 2 bits: TA版本
    uint8_t m_status = 0;                               // 2 bits: 保留字段1
    uint8_t m_reserved1 = 0;                            // 1 bits: SV字段
    bool m_poison = false;                              // 1 bit: Poison标志
    uint8_t m_reserved2 = 0;                            // 2 bit: 保留字段2
    
    // 字节2-3: [Initiator TASSN:16]
    uint16_t m_iniTaSsn = 0;                    // 16 bits: 发起者Transaction Session Number
    
    // 常量定义
    static const uint32_t totalHeaderSize = 4;     // 总头部大小 (4字节)
};

/**
 * \ingroup ub-header
 * \brief UB Compact Basic TA Header
 *
 * 报文头格式：总计4字节 (32bit)
 *      字节0: [TaOpcode:8]
 *      字节1: [TaVer(fixed 0x00):2][EE:2][TV_EN:1][POISON:1][reserved:1][UD_FLG:1]
 *      字节2-3: [INI TaSsn:16]
 *
 */
class UbCompactTransactionHeader : public Header {
public:
    UbCompactTransactionHeader();
    virtual ~UbCompactTransactionHeader();

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream &os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    // Setters
    void SetTaOpcode(TaOpcode opcode);       // 设置TA操作码 (8位)
    void SetTaOpcode(uint8_t opcode);        // 设置TA操作码 (8位)
    void SetIniTaSsn(uint16_t taSsn);  // 设置发起者TaSsn (16位)

    // Getters
    uint8_t GetTaOpcode() const;         // 获取TA操作码
    uint16_t GetIniTaSsn() const;  // 获取发起者TaSsn

    // 验证方法
    bool IsValidOpcode() const;  // 验证操作码是否有效

private:
    // 报文头字段 - 总共4字节 (32位)
    
    // 字节0: [TaOpcode:8]
    uint8_t m_taOpcode = static_cast<uint8_t>(TaOpcode::TA_OPCODE_WRITE);             // 8 bits: Transaction opcode
    
    // 字节1: [TaVer(fixed 0x00):2][EE:2][TK_VLD:1][POISON:1][reserved:1][UDF_FLG:1]
    uint8_t m_taVer = 0x00;                           // 2 bits: Ta协议版本号 (固定0x00)
    uint8_t m_ee = 0x00;                              // 2 bits: EE (忽略)
    bool m_tvEn = false;                             // 1 bit: TK_VLD (忽略)
    bool m_poison = false;                            // 1 bit: POISON (忽略)
    uint8_t m_reserved = 0;                           // 1 bit: reserved
    bool m_udFlg = false;                            // 1 bit: UDF_FLG (忽略)
    
    // 字节2-3: [INI TaSsn:16]
    uint16_t m_iniTaSsn = 0xFFFF;                     // 16 bits: Initiator Transaction Segment Sequence Number
    
    // 常量定义
    static const uint32_t totalHeaderSize = 4;     // 总头部大小 (4字节)
};

/**
 * \ingroup ub-header
 * \brief UB TA Header Identifier
 *
 * 报文头格式：
 *      字节0: [TA OpCode:8]
 *
 */
class UbDummyTransactionHeader : public Header {
public:
    UbDummyTransactionHeader();
    virtual ~UbDummyTransactionHeader();

    static TypeId GetTypeId(void);
    TypeId GetInstanceTypeId(void) const override;
    void Print(std::ostream &os) const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize(void) const override;

    // Setters
    void SetTaOpcode(TaOpcode opcode);  // 设置TA操作码 (8位)
    void SetTaOpcode(uint8_t opcode);   // 设置TA操作码 (8位)

    // Getters
    uint8_t GetTaOpcode() const;  // 获取TA操作码

private:
    // 报文头字段 - 总共4字节 (32位)
    
    // 字节0: [TA OpCode:8]
    uint8_t m_taOpcode = static_cast<uint8_t>(TaOpcode::TA_OPCODE_MAX);
    // 常量定义
    static const uint32_t totalHeaderSize = 1;     // 总头部大小 (1字节)
};

} // namespace ns3

#endif /* UB_HEADER_H */
