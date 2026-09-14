// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_DATATYPE_H
#define UB_DATATYPE_H

#include "ns3/callback.h"
#include "ns3/ipv4-address.h"
#include "ns3/object.h"
#include "ns3/core-module.h"

#include <bitset>
#include <cstdint>

namespace ns3 {

extern GlobalValue g_ub_priority_num;
extern GlobalValue g_ub_vl_num;

// ============================================================================
// 常量定义
// ============================================================================

// 优先级相关常量
const uint8_t UB_PRIORITY_NUM_DEFAULT = 16;         // 支持的优先级数量默认值 (0-15)
const uint8_t UB_PRIORITY_HIGH = 0;                 // 最高优先级
const uint8_t UB_PRIORITY_LOW = 15;                 // 最低优先级
const uint8_t UB_PRIORITY_DEFAULT = 8;              // 默认优先级
const uint8_t UB_PRIORITY_MAX = UB_PRIORITY_LOW;    // 最大优先级值

// Jetty相关常量
const uint32_t UB_JETTY_TASSN_OOO_THRESHOLD = 2048; // Jetty分段乱序阈值（用于乱序缓存等）

// WQE相关常量
const uint32_t UB_WQE_TA_SEGMENT_BYTE = 64 * 1024;  // TA层分段大小 (64KB)
const uint32_t UB_MTU_BYTE = 4 * 1024;              // 最大传输单元（TP层）

// Credit相关常量
const uint8_t UB_CREDIT_MAX_VALUE = 63;             // CREDIT最大值

// 操作类型枚举
enum class UbOperationType : uint8_t {
    WRITE = 0, // 写操作
    READ = 1   // 读操作
};

enum class UbTransactionSegmentKind : uint8_t {
    REQUEST = 0,
    RESPONSE = 1
};

// 内存语义操作类型枚举
enum class UbMemOperationType : uint8_t {
    STORE = 0,
    LOAD = 1,
    MEM_STORE_ACK = 2,
    MEM_LOAD_RESP
};

// ============================================================================
// 类型定义
// ============================================================================

using UbPriority = uint8_t;

// ============================================================================
// 前向声明
// ============================================================================
struct TpgTag;

// ============================================================================
// UB Header相关字段定义
// ============================================================================
// 定义TaOpcode枚举，便于使用
enum class TaOpcode : uint8_t {
    TA_OPCODE_SEND = 0x00,             // send
    TA_OPCODE_SEND_IMMEDIATE = 0x01,   // send with immediate (8B)
    TA_OPCODE_SEND_INVALIDATE = 0x02,  // send with invalidate
    TA_OPCODE_WRITE = 0x03,            // write （最常用）
    TA_OPCODE_WRITE_IMMEDIATE = 0x04,  // write with immediate (8B)
    TA_OPCODE_WRITE_NOTIFY = 0x05,     // write with notify (8B)
    TA_OPCODE_READ = 0x06,             // read （最常用）
    TA_OPCODE_ATOMIC_CMP_SWAP = 0x07,  // atomic compare swap
    TA_OPCODE_ATOMIC_SWAP = 0x08,      // atomic swap
    TA_OPCODE_ATOMIC_STORE = 0x09,     // atomic store
    TA_OPCODE_ATOMIC_LOAD = 0x0A,      // atomic load
    TA_OPCODE_ATOMIC_FETCH_ADD = 0x0B, // atomic fetch add
    TA_OPCODE_ATOMIC_FETCH_SUB = 0x0C, // atomic fetch sub
    TA_OPCODE_ATOMIC_FETCH_AND = 0x0D, // atomic fetch and
    TA_OPCODE_ATOMIC_FETCH_OR = 0x0E,  // atomic fetch or
    TA_OPCODE_ATOMIC_FETCH_XOR = 0x0F, // atomic fetch xor
    TA_OPCODE_MESSAGE = 0x10,          // message
    TA_OPCODE_TRANSACTION_ACK = 0x11,  // transaction ACK
    TA_OPCODE_READ_RESPONSE = 0x12,    // read response
    TA_OPCODE_ATOMIC_RESPONSE = 0x13,  // atomic response
    TA_OPCODE_WRITE_BE = 0x14,         // write with BE
    TA_OPCODE_PREFETCH_TGT = 0x15,     // PrefetchTgt
    TA_OPCODE_DISCONNECT_SCID = 0x16,  // Disconnect SCID
    TA_OPCODE_WRITEBACK_FULL = 0x17,   // WriteBackFull
    TA_OPCODE_WRITEBACK_PTL = 0x18,    // WriteBackPtl
    TA_OPCODE_MAX = 0x19               // 无效的TA OPCODE
};

enum class UbDatalinkHeaderConfig : uint8_t {
    CONTROL = 0x00,         // Config value for control credit header
    PACKET_IPV4 = 0x03,     // Config value for ipv4 packet header
    PACKET_IPV6 = 0x04,     // Config value for ipv6 packet header
    PACKET_CNA16 = 0x06,    // Config value for ipv6 packet header
    PACKET_CNA24 = 0x07,    // Config value for ipv6 packet header
    PACKET_UB_MEM = 0x09    // Config value for UB memory packet header
};

// 定义Order枚举
enum class OrderType : uint8_t {
    ORDER_NO = 0x00,      // No Order: 与其它报文无保序要求
    ORDER_RELAX = 0x01,   // Relax Order: SO报文不能超越前面带RO的报文
    ORDER_STRONG = 0x02,  // Strong Order: SO报文不能超越前面带RO的报文
    ORDER_RESERVED = 0x03 // reserved
};

// 定义Initiator Requester Context类型枚举
enum class IniRcType : uint8_t {
    REQUESTER_CONTEXT = 0x01,                  // Initiator Context
    DESTINATION_SEQUENCE_CONTEXT = 0x02,       // Requester Context
    RESERVED = 0x03                             // Destination Sequence Context
};

// 定义TPOpcode常量，便于使用
enum class TpOpcode : uint8_t {
    TP_OPCODE_UNRELIABLE_TA = 0x0,      // 不可靠TA数据包
    TP_OPCODE_RELIABLE_TA = 0x1,        // 可靠TA数据包（典型数据包）
    TP_OPCODE_ACK_WITHOUT_CETPH = 0x2,  // 不带CETPH的TP ACK（典型ACK）
    TP_OPCODE_ACK_WITH_CETPH = 0x3,     // 带CETPH的TP ACK
    TP_OPCODE_RESERVED1 = 0x4,          // 保留
    TP_OPCODE_SACK_WITHOUT_CETPH = 0x5, // 不带CETPH的TP SACK
    TP_OPCODE_SACK_WITH_CETPH = 0x6,    // 带CETPH的TP SACK
    TP_OPCODE_RESERVED2 = 0x7,          // 保留
    TP_OPCODE_CNP = 0x8                 // CNP拥塞通知包
};

enum class UbRetransmissionMode : uint8_t {
    GBN = 0,
    SELECTIVE = 1,
};

enum class UbRetransTimeoutMode : uint8_t {
    STATIC = 0,
    DYNAMIC = 1,
};

enum class TransportMode : uint8_t {
    RTP = 0,
    CTP = 1,
};

enum class CtpOpcode : uint8_t {
    CTP_DATA = 0x0,
    CTP_CNP = 0x1,
};

constexpr uint8_t UB_CNA_NLP_CTPH = 0x0;
constexpr uint8_t UB_CNA_NLP_RTPH = 0x2;
constexpr uint8_t UB_CTPH_NLP_COMPACT_TAH = 0x0;
constexpr uint8_t UB_CTPH_NLP_UPI32_EID128_TAH = 0x1;
constexpr uint8_t UB_CTPH_NLP_UPI16_EID40_TAH = 0x2;
constexpr uint32_t UB_COMPACT_EID_MAX = 0xFFFFF;
constexpr uint16_t UB_COMPACT_UPI_MAX = 0x7FFF;

// 定义NLP常量，便于使用
enum class NextLayerProtocol : uint8_t {
    NLP_TAH = 0x0,      // TAH (事务头)
    NLP_UPI_UEID = 0x1, // UPI + UEID (虚拟化相关，忽略)
    NLP_RESERVED = 0x2, // 保留
    NLP_CIP = 0x3       // CIP (保密性和完整性保护，忽略)
};

/**
 * @brief 验证优先级是否有效
 * @param priority 要验证的优先级值
 * @return true 如果有效，false 否则
 */
inline bool IsValidPriority(UbPriority priority)
{
    return priority <= UB_PRIORITY_MAX;
}

/**
 * @brief
 *
 * 表示一个完整的内存语义任务，包含任务描述信息、网络信息。
 */
class UbLdstTaskSegment : public Object {
public:
    static TypeId GetTypeId(void);

    // ========== 构造函数 ==========
    UbLdstTaskSegment()
        : m_src(0),
          m_dest(0),
          m_type(UbMemOperationType::STORE),
          m_size(0),
          m_bytesLeft(0)
    {
    }

    UbLdstTaskSegment(uint32_t src,
              uint32_t dest,
              UbMemOperationType type,
              uint32_t size,
              UbPriority priority)
        : m_src(src),
          m_dest(dest),
          m_type(type),
          m_size(size),
          m_priority(priority),
          m_bytesLeft(size)
    {
    }

    ~UbLdstTaskSegment() = default;

    // ========== 仿真全局信息 ==========
    uint32_t GetTaskId() const
    {
        return m_taskId;
    }

    void SetTaskId(uint32_t taskId)
    {
        m_taskId = taskId;
    }

    uint32_t GetTaskSegmentId() const
    {
        return m_taskSegmentId;
    }

    void SetTaskSegmentId(uint32_t taskSegmentId)
    {
        m_taskSegmentId = taskSegmentId;
    }

    uint32_t GetThreadId() const
    {
        return m_threadId;
    }

    void SetThreadId(uint32_t threadId)
    {
        m_threadId = threadId;
    }

    // ========== 任务描述信息 ==========
    uint32_t GetSrc() const
    {
        return m_src;
    }

    uint32_t GetDest() const
    {
        return m_dest;
    }

    UbMemOperationType GetType() const
    {
        return m_type;
    }

    uint32_t GetSize() const
    {
        return m_size;
    }

    UbPriority GetPriority() const
    {
        return m_priority;
    }

    uint32_t GetPsnSize() const
    {
        return m_psnCnt;
    }

    void SetSrc(uint32_t src)
    {
        m_src = src;
    }

    void SetDest(uint32_t dest)
    {
        m_dest = dest;
    }

    void SetType(UbMemOperationType type)
    {
        m_type = type;
    }

    void SetSize(uint32_t size)
    {
        m_size = size;
        m_bytesLeft = size;
    }

    void SetPacketInfo(uint32_t packetSize, uint32_t length)
    {
        m_length = length;
        m_dataSize = 64 * (1 << length);
        m_packetSize = packetSize;
        // 计算要发的packet数目
        m_psnCnt = (m_size + m_dataSize - 1) / m_dataSize;
    }

    uint32_t GetLength()
    {
        return m_length;
    }

    uint32_t GetDataSize()
    {
        return m_dataSize;
    }

    uint32_t GetPacketSize()
    {
        return m_packetSize;
    }

    void SetPriority(UbPriority priority)
    {
        m_priority = priority;
    }

    // ========== 动态信息 ==========
    uint64_t GetBytesLeft() const
    {
        return m_bytesLeft;
    }

    // ========== 业务逻辑方法 ==========
    // 判断MEM是否已经发送完成（即剩余字节为0）
    bool IsSentCompleted() const
    {
        // 已发送完成：所有数据都已发送，剩余字节为0
        return m_bytesLeft == 0;
    }

    /**
     * @brief 重置MEM发送状态，可用于重传
     */
    void Reset()
    {
        m_bytesLeft = m_size;
    }

    /**
     * @brief 查看下一个packet的大小
     * @return 对应分段的大小
     */
    uint32_t PeekNextDataSize() const
    {
        return std::min(m_dataSize, m_bytesLeft);
    }

    /**
     * @brief 检查MEM配置是否有效
     * @return true 如果配置有效，false 否则
     */
    bool IsValid() const
    {
        return m_size > 0 && IsValidPriority(m_priority) && m_src != m_dest;
    }

    /**
     * @brief 更新已发送的字节数
     * @param sentBytes 本次发送的字节数
     * @return 实际发送的字节数（不会超过剩余字节数）
     */
    uint64_t UpdateSentBytes(uint32_t sentBytes)
    {
        uint64_t actualSent = std::min(sentBytes, m_bytesLeft);
        m_bytesLeft -= actualSent;
        return actualSent;
    }

private:
    // ========== 全局信息 ==========
    uint32_t m_taskId;
    uint32_t m_threadId;
    uint32_t m_taskSegmentId;       // 用于识别tasksegment
    // ========== 任务描述信息 ==========
    uint32_t m_src;                 // 源节点标识符
    uint32_t m_dest;                // 目的节点标识符
    UbMemOperationType m_type = UbMemOperationType::STORE; // 操作类型
    uint32_t m_size = 0;                                     // MemTask数据大小 (字节)
    UbPriority m_priority = UB_PRIORITY_DEFAULT;             // MemTask优先级 (0-15, 0最高)

    uint64_t m_address = 0;    // 待访问的内存起始地址
    uint32_t m_length = 0;     // 单个切片要访问的内存段数据长度， 64 * (2 ^ length)
    uint32_t m_dataSize = 0;   // 单个切片要访问的内存段数据长度
    uint32_t m_psnCnt = 0;          // 总计获取的数据包个数计数
    uint32_t m_bytesLeft = 0;       // 剩余的字节数
    uint32_t m_msn = 0;
    uint32_t m_packetSize = 0; // 请求包的payload size
};

// ============================================================================
// 数据结构定义
//
//  仿真各层次数据结构关系：
//  数据结构：     WQE(message)         WQE->WQESegment          WQESegment -> Packet
//  角色功能：  维护WQE完成信息        保证WQESegment可靠完成       保证Packet可靠完成，
//                                       上报WQE完成信息          上报WQESegment完成信息
//  角色：         Client --------------> TA(jetty) --------------> TP
// ============================================================================

/**
 * @brief Work Queue Entry (WQE) 类
 *
 * 表示一个完整的工作队列项（消息），包含任务描述信息、网络信息和TA层调度信息。
 * WQE 会被分解为多个 WQE Segment 进行细粒度调度。
 */
class UbWqe : public Object {
public:
    static TypeId GetTypeId(void);

    // ========== 构造函数 ==========
    UbWqe()
        : m_src(0),
          m_dest(0),
          m_sport(0),
          m_dport(0),
          m_type(TaOpcode::TA_OPCODE_WRITE),
          m_size(0),
          m_priority(UB_PRIORITY_DEFAULT),
          m_taMsn(0),
          m_taSsnStart(0),
          m_taSsnSize(0),
          m_bytesLeft(0)
    {
    }

    UbWqe(uint32_t src,
          uint32_t dest,
          uint8_t sport,
          uint8_t dport,
          uint32_t win,
          uint32_t baseRtt,
          TaOpcode type,
          uint32_t size,
          UbPriority priority)
        : m_src(src),
          m_dest(dest),
          m_sport(sport),
          m_dport(dport),
          m_type(type),
          m_size(size),
          m_priority(priority),
          m_taMsn(0),
          m_taSsnStart(0),
          m_taSsnSize((m_size + UB_WQE_TA_SEGMENT_BYTE - 1) / UB_WQE_TA_SEGMENT_BYTE),
          m_bytesLeft(size)
    {
    }

    ~UbWqe() = default;

    // ========== 仿真全局信息 ==========
    uint32_t GetWqeId() const
    {
        return m_wqeId;
    }

    void SetWqeId(uint32_t wqeId)
    {
        m_wqeId = wqeId;
    }

    // ========== 任务描述信息 ==========
    uint32_t GetSrc() const
    {
        return m_src;
    }

    uint32_t GetDest() const
    {
        return m_dest;
    }

    bool HasSrcEntityId() const
    {
        return m_hasSrcEntityId;
    }

    bool HasDstEntityId() const
    {
        return m_hasDstEntityId;
    }

    uint32_t GetSrcEntityId() const
    {
        return m_srcEntityId;
    }

    uint32_t GetDstEntityId() const
    {
        return m_dstEntityId;
    }

    uint8_t GetSport() const
    {
        return m_sport;
    }

    uint8_t GetDport() const
    {
        return m_dport;
    }

    TaOpcode GetType() const
    {
        return m_type;
    }

    uint32_t GetSize() const
    {
        return m_size;
    }

    UbPriority GetPriority() const
    {
        return m_priority;
    }

    void SetSrc(uint32_t src)
    {
        m_src = src;
    }

    void SetDest(uint32_t dest)
    {
        m_dest = dest;
    }

    void SetSrcEntityId(uint32_t srcEntityId)
    {
        m_srcEntityId = srcEntityId;
        m_hasSrcEntityId = true;
    }

    void SetDstEntityId(uint32_t dstEntityId)
    {
        m_dstEntityId = dstEntityId;
        m_hasDstEntityId = true;
    }

    void SetSport(uint8_t sport)
    {
        m_sport = sport;
    }

    void SetDport(uint8_t dport)
    {
        m_dport = dport;
    }

    void SetType(TaOpcode type)
    {
        m_type = type;
    }

    void SetSize(uint32_t size)
    {
        m_size = size;
        m_bytesLeft = size;
        // 计算TA层分段数
        m_taSsnSize = (m_size + UB_WQE_TA_SEGMENT_BYTE - 1) / UB_WQE_TA_SEGMENT_BYTE;
    }

    void SetPriority(UbPriority priority)
    {
        m_priority = priority;
    }

    // ========== 网络层信息 ==========
    Ipv4Address GetSip() const
    {
        return m_sip;
    }

    Ipv4Address GetDip() const
    {
        return m_dip;
    }

    void SetSip(const Ipv4Address& sip)
    {
        m_sip = sip;
    }

    void SetDip(const Ipv4Address& dip)
    {
        m_dip = dip;
    }

    uint32_t GetJettyNum() const
    {
        return m_jettyNum;
    }

    uint64_t GetTaMsn() const
    {
        return m_taMsn;
    }

    uint64_t GetTaSsnStart() const
    {
        return m_taSsnStart;
    }

    uint64_t GetTaSsnSize() const
    {
        return m_taSsnSize;
    }

    void SetJettyNum(uint32_t jettyNum)
    {
        m_jettyNum = jettyNum;
    }

    void SetTaMsn(uint64_t msn)
    {
        m_taMsn = msn;
    }

    void SetTaSsnStart(uint64_t start)
    {
        m_taSsnStart = start;
    }

    void SetTaSsnSize(uint64_t size)
    {
        m_taSsnSize = size;
    }

    // ========== 动态信息 ==========
    uint64_t GetBytesLeft() const
    {
        return m_bytesLeft;
    }

    // ========== 业务逻辑方法 ==========
    /**
     * @brief 检查WQE是否已发送完成
     * @return true 如果所有数据都已发送，false 否则
     */
    // 判断WQE是否已经发送完成（即剩余字节为0）
    bool IsSentCompleted() const
    {
        // 已发送完成：所有数据都已发送，剩余字节为0
        return m_bytesLeft == 0;
    }

    /**
     * @brief 重置WQE发送状态，可用于重传
     */
    void Reset()
    {
        m_bytesLeft = m_carrierBytes == 0 ? m_size : m_carrierBytes;
    }

    /**
     * @brief 获取下一个TA分段大小，并更新WQE剩余字节数
     * @return 对应分段的大小
     */
    uint32_t GetNextSegmentSize()
    {
        uint32_t actualSent = PeekNextSegmentSize();
        m_bytesLeft -= actualSent;
        return actualSent;
    }

    /**
     * @brief 查看下一个segment的大小
     * @return 对应分段的大小
     */
    uint32_t PeekNextSegmentSize() const
    {
        return std::min(UB_WQE_TA_SEGMENT_BYTE, m_bytesLeft);
    }

    /**
     * @brief 检查WQE配置是否有效
     * @return true 如果配置有效，false 否则
     */
    bool IsValid() const
    {
        return m_size > 0 && IsValidPriority(m_priority) && m_src != m_dest;
    }

    bool CanSend() const
    {
        return m_canSend;
    }

    void SetCanSend(bool status)
    {
        m_canSend = status;
    }

    /**
     * @brief 更新已发送的字节数
     * @param sentBytes 本次发送的字节数
     * @return 实际发送的字节数（不会超过剩余字节数）
     */
    uint64_t UpdateSentBytes(uint32_t sentBytes)
    {
        uint64_t actualSent = std::min(sentBytes, m_bytesLeft);
        m_bytesLeft -= actualSent;
        return actualSent;
    }

    void SetOrderType(OrderType type) { m_order = type; }
    OrderType GetOrderType() { return m_order; }

    void SetSegmentKind(UbTransactionSegmentKind segmentKind)
    {
        m_segmentKind = segmentKind;
    }

    UbTransactionSegmentKind GetSegmentKind() const
    {
        return m_segmentKind;
    }

    void SetOriginJettyNum(uint32_t originJettyNum)
    {
        m_originJettyNum = originJettyNum;
    }

    uint32_t GetOriginJettyNum() const
    {
        return m_originJettyNum;
    }

    void SetRequestTassn(uint32_t requestTassn)
    {
        m_requestTassn = requestTassn;
    }

    uint32_t GetRequestTassn() const
    {
        return m_requestTassn;
    }

    void SetRequestOpcode(TaOpcode requestOpcode)
    {
        m_requestOpcode = requestOpcode;
    }

    TaOpcode GetRequestOpcode() const
    {
        return m_requestOpcode;
    }

    void SetResponseBytes(uint32_t responseBytes)
    {
        m_responseBytes = responseBytes;
    }

    uint32_t GetResponseBytes() const
    {
        return m_responseBytes;
    }

    void SetRemoteAddress(uint64_t remoteAddress)
    {
        m_remoteAddress = remoteAddress;
    }

    uint64_t GetRemoteAddress() const
    {
        return m_remoteAddress;
    }

    void SetNeedsTransactionResponse(bool needsTransactionResponse)
    {
        m_needsTransactionResponse = needsTransactionResponse;
    }

    bool NeedsTransactionResponse() const
    {
        return m_needsTransactionResponse;
    }

    void SetResLenBytes(uint32_t resLenBytes)
    {
        // TODO: Rename ResLen/resLenBytes to ResponseLen/responseLenBytes in a dedicated
        // mechanical cleanup. Keep the current API stable in the retransmission MR.
        m_resLenBytes = resLenBytes;
    }

    uint32_t GetResLenBytes() const
    {
        return m_resLenBytes;
    }

    void SetLogicalBytes(uint32_t logicalBytes)
    {
        SetResLenBytes(logicalBytes);
    }

    uint32_t GetLogicalBytes() const
    {
        return GetResLenBytes();
    }

    void SetPayloadBytes(uint32_t payloadBytes)
    {
        m_payloadBytes = payloadBytes;
    }

    uint32_t GetPayloadBytes() const
    {
        return m_payloadBytes;
    }

    void SetCarrierBytes(uint32_t carrierBytes)
    {
        m_carrierBytes = carrierBytes;
    }

    uint32_t GetCarrierBytes() const
    {
        return m_carrierBytes;
    }
private:
    // ========== 全局信息 ==========
    uint32_t m_wqeId;   // WQE标识符（Node范围内唯一）。仅用于数据收集。
    // ========== 任务描述信息 ==========
    uint32_t m_src;     // 源节点标识符
    uint32_t m_dest;    // 目的节点标识符
    bool m_hasSrcEntityId{false};
    bool m_hasDstEntityId{false};
    uint32_t m_srcEntityId{0};
    uint32_t m_dstEntityId{0};
    uint8_t m_sport;    // 源端口号
    uint8_t m_dport;    //< 目的端口号
    TaOpcode m_type = TaOpcode::TA_OPCODE_WRITE;           // 操作类型 (READ/WRITE)
    uint32_t m_size = 0;                         // WQE数据大小 (字节)
    UbPriority m_priority = UB_PRIORITY_DEFAULT; // WQE优先级 (0-15, 0最高)

    // ========== 网络层信息 ==========
    Ipv4Address m_sip = Ipv4Address("0.0.0.0"); // 源IP地址
    Ipv4Address m_dip = Ipv4Address("0.0.0.0"); // 目的IP地址

    // ========== TA层静态信息 (调度时设置，之后不变) ==========
    bool m_canSend = false;
    uint32_t m_jettyNum;
    uint64_t m_taMsn;      // TA层消息序号 (Message Sequence Number)
    uint32_t m_taSsnStart; // TA层起始分段序号 (Segment Sequence Number)
    uint32_t m_taSsnSize;  // TA层分段数量
    OrderType m_order = OrderType::ORDER_NO;
    UbTransactionSegmentKind m_segmentKind = UbTransactionSegmentKind::REQUEST;
    uint32_t m_originJettyNum = UINT32_MAX;
    uint32_t m_requestTassn = UINT32_MAX;
    TaOpcode m_requestOpcode = TaOpcode::TA_OPCODE_WRITE;
    uint32_t m_responseBytes = 0;
    uint64_t m_remoteAddress = 0;
    bool m_needsTransactionResponse = true;
    uint32_t m_resLenBytes = 0;
    uint32_t m_payloadBytes = 0;
    uint32_t m_carrierBytes = 0;

    // ========== TA层动态信息 (追踪WQE segment的完成) ==========
    uint32_t m_bytesLeft = 0; // 剩余的字节数
};

/**
 * @brief Work Queue Entry Segment 类
 *
 * 表示WQE被分段后的单个分段，用于更细粒度的调度和流控。
 * 包含完整的任务信息、网络信息、TA层信息和TP层信息。
 *
 * 一个WQE Segment会进一步被分解为多个网络包进行传输。
 */
class UbWqeSegment : public Object {
public:
    static TypeId GetTypeId(void);

    // ========== 构造函数 ==========
    UbWqeSegment()
    {
    }

    UbWqeSegment(uint32_t src,
                 uint32_t dest,
                 uint8_t sport,
                 uint8_t dport,
                 uint32_t jettyNum,
                 TaOpcode type,
                 uint32_t size,
                 UbPriority priority,
                 uint32_t taskId)
        : m_src(src),
          m_dest(dest),
          m_sport(sport),
          m_dport(dport),
          m_type(type),
          m_size(size),
          m_priority(priority),
          m_taskId(taskId),
          m_jettyNum(jettyNum),
          m_bytesLeft(size)
    {
    }

    ~UbWqeSegment() = default;

    // ========== 任务描述信息 ==========
    uint32_t GetSrc() const
    {
        return m_src;
    }

    uint32_t GetDest() const
    {
        return m_dest;
    }

    bool HasSrcEntityId() const
    {
        return m_hasSrcEntityId;
    }

    bool HasDstEntityId() const
    {
        return m_hasDstEntityId;
    }

    uint32_t GetSrcEntityId() const
    {
        return m_srcEntityId;
    }

    uint32_t GetDstEntityId() const
    {
        return m_dstEntityId;
    }

    uint8_t GetSport() const
    {
        return m_sport;
    }

    uint8_t GetDport() const
    {
        return m_dport;
    }

    TaOpcode GetType() const
    {
        return m_type;
    }

    uint32_t GetSize() const
    {
        return m_size;
    }

    UbPriority GetPriority() const
    {
        return m_priority;
    }

    uint32_t GetTaskId() const
    {
        return m_taskId;
    }

    uint32_t GetWqeSize() const
    {
        return m_wqeSize;
    }

    void SetSrc(uint32_t src)
    {
        m_src = src;
    }

    void SetDest(uint32_t dest)
    {
        m_dest = dest;
    }

    void SetSrcEntityId(uint32_t srcEntityId)
    {
        m_srcEntityId = srcEntityId;
        m_hasSrcEntityId = true;
    }

    void SetDstEntityId(uint32_t dstEntityId)
    {
        m_dstEntityId = dstEntityId;
        m_hasDstEntityId = true;
    }

    void SetSport(uint8_t sport)
    {
        m_sport = sport;
    }

    void SetDport(uint8_t dport)
    {
        m_dport = dport;
    }

    void SetType(TaOpcode type)
    {
        m_type = type;
    }

    void SetSize(uint32_t size)
    {
        m_size = size;
        m_bytesLeft = size;
        // 计算TA层分段数
        m_psnSize = (m_size + UB_MTU_BYTE - 1) / UB_MTU_BYTE;
    }

    void SetPriority(UbPriority priority)
    {
        m_priority = priority;
    }

    void SetTaskId(uint32_t taskId)
    {
        m_taskId = taskId;
    }

    void SetWqeSize(uint32_t size)
    {
        m_wqeSize = size;
    }

    // ========== 网络层信息 ==========
    Ipv4Address GetSip() const
    {
        return m_sip;
    }

    Ipv4Address GetDip() const
    {
        return m_dip;
    }

    void SetSip(const Ipv4Address& sip)
    {
        m_sip = sip;
    }

    void SetDip(const Ipv4Address& dip)
    {
        m_dip = dip;
    }

    // ========== TA层信息 ==========
    uint32_t GetJettyNum() const
    {
        return m_jettyNum;
    }

    uint64_t GetTaMsn() const
    {
        return m_taMsn;
    }

    uint32_t GetTaSsn() const
    {
        return m_taSsn;
    }

    void SetJettyNum(const uint32_t n)
    {
        m_jettyNum = n;
    }

    void SetTaMsn(uint64_t msn)
    {
        m_taMsn = msn;
    }

    void SetTaSsn(uint32_t ssn)
    {
        m_taSsn = ssn;
    }

    // ========== TP层静态信息 ==========
    uint64_t GetTpMsn() const
    {
        return m_tpMsn;
    }

    uint64_t GetPsnStart() const
    {
        return m_psnStart;
    }

    uint32_t GetPsnSize() const
    {
        return m_psnSize;
    }

    void SetTpMsn(uint64_t msn)
    {
        m_tpMsn = msn;
    }

    void SetPsnStart(uint64_t start)
    {
        m_psnStart = start;
    }

    // ========== TP层动态信息 ==========
    uint64_t GetBytesLeft() const
    {
        return m_bytesLeft;
    }

    // ========== 业务逻辑方法 ==========
    /**
     * @brief 检查分段是否完全发送完成
     * @return true 如果所有字节都已发送，false 否则
     */
    bool IsSentCompleted() const
    {
        return m_bytesLeft == 0;
    }

    /**
     * @brief 更新已发送的字节数
     * @param sentBytes 本次发送的字节数
     * @return 实际发送的字节数（不会超过剩余字节数）
     */
    uint64_t UpdateSentBytes(uint32_t sentBytes)
    {
        uint64_t actualSent = std::min(sentBytes, m_bytesLeft);
        m_bytesLeft -= actualSent;
        return actualSent;
    }

    /**
     * @brief 重设已发送的字节数
     * @param
     * @return
     */
    void ResetSentBytes()
    {
        m_bytesLeft = m_carrierBytes == 0 ? m_size : m_carrierBytes;
    }

    /**
     * @brief 重设已发送的字节数
     * @param sentBytes 不需要重置的字节数
     * @return
     */
    void ResetSentBytes(uint32_t sentBytes)
    {
        const uint32_t progressBytes = m_carrierBytes == 0 ? m_size : m_carrierBytes;
        m_bytesLeft = sentBytes >= progressBytes ? 0 : progressBytes - sentBytes;
    }

    /**
     * @brief 获取下一个Packet分段大小，并更新WQE Segment剩余字节数
     * @return 对应Packet的大小
     */
    uint32_t GetNextPacketSize() // 移除const修饰符，因为函数修改了成员变量
    {
        uint64_t actualSent = std::min(UB_MTU_BYTE, m_bytesLeft); // 修正为使用UB_MTU_BYTE
        m_bytesLeft -= actualSent;
        return actualSent; // 添加显式类型转换
    }

    /**
     * @brief 查看下一个Packet的大小
     * @return 对应Packet的大小
     */
    uint32_t PeekNextPacketSize() const
    {
        return std::min(UB_MTU_BYTE, m_bytesLeft); // 添加显式类型转换
    }

    /**
     * @brief 重置分段状态（用于重传）
     */
    void Reset()
    {
        m_bytesLeft = m_size;
    }

    /**
     * @brief 检查分段是否有效
     * @return true 如果分段配置有效，false 否则
     */
    bool IsValid() const
    {
        return m_size > 0 && IsValidPriority(m_priority) && m_src != m_dest;
    }

    void SetOrderType(OrderType type) { m_orderType = type; }

    OrderType GetOrderType() { return m_orderType; }

    void SetTpn(uint32_t tpn) { m_tpn = tpn; }

    uint32_t GetTpn() { return m_tpn; }

    void SetSegmentKind(UbTransactionSegmentKind segmentKind)
    {
        m_segmentKind = segmentKind;
    }

    UbTransactionSegmentKind GetSegmentKind() const
    {
        return m_segmentKind;
    }

    void SetOriginJettyNum(uint32_t originJettyNum)
    {
        m_originJettyNum = originJettyNum;
    }

    uint32_t GetOriginJettyNum() const
    {
        return m_originJettyNum;
    }

    void SetRequestTassn(uint32_t requestTassn)
    {
        m_requestTassn = requestTassn;
    }

    uint32_t GetRequestTassn() const
    {
        return m_requestTassn;
    }

    void SetRequestOpcode(TaOpcode requestOpcode)
    {
        m_requestOpcode = requestOpcode;
    }

    TaOpcode GetRequestOpcode() const
    {
        return m_requestOpcode;
    }

    void SetResponseBytes(uint32_t responseBytes)
    {
        m_responseBytes = responseBytes;
    }

    uint32_t GetResponseBytes() const
    {
        return m_responseBytes;
    }

    void SetRemoteAddress(uint64_t remoteAddress)
    {
        m_remoteAddress = remoteAddress;
    }

    uint64_t GetRemoteAddress() const
    {
        return m_remoteAddress;
    }

    void SetNeedsTransactionResponse(bool needsTransactionResponse)
    {
        m_needsTransactionResponse = needsTransactionResponse;
    }

    bool NeedsTransactionResponse() const
    {
        return m_needsTransactionResponse;
    }

    void SetResLenBytes(uint32_t resLenBytes)
    {
        // TODO: Rename ResLen/resLenBytes to ResponseLen/responseLenBytes in a dedicated
        // mechanical cleanup. Keep the current API stable in the retransmission MR.
        m_resLenBytes = resLenBytes;
    }

    uint32_t GetResLenBytes() const
    {
        return m_resLenBytes;
    }

    void SetLogicalBytes(uint32_t logicalBytes)
    {
        SetResLenBytes(logicalBytes);
    }

    uint32_t GetLogicalBytes() const
    {
        return GetResLenBytes();
    }

    void SetPayloadBytes(uint32_t payloadBytes)
    {
        m_payloadBytes = payloadBytes;
    }

    uint32_t GetPayloadBytes() const
    {
        return m_payloadBytes;
    }

    void SetCarrierBytes(uint32_t carrierBytes)
    {
        m_carrierBytes = carrierBytes;
        m_bytesLeft = carrierBytes;
        m_psnSize = (carrierBytes + UB_MTU_BYTE - 1) / UB_MTU_BYTE;
    }

    uint32_t GetCarrierBytes() const
    {
        return m_carrierBytes;
    }
private:
    // ========== 任务描述信息 ==========
    uint32_t m_src{0};                              // 源节点标识符
    uint32_t m_dest{0};                             // 目的节点标识符
    bool m_hasSrcEntityId{false};
    bool m_hasDstEntityId{false};
    uint32_t m_srcEntityId{0};
    uint32_t m_dstEntityId{0};
    uint8_t m_sport{0};                             // 源端口号
    uint8_t m_dport{0};                             // 目的端口号
    TaOpcode m_type{TaOpcode::TA_OPCODE_WRITE};     // 操作类型 (READ/WRITE)
    uint32_t m_size{0};                             // 分段数据大小 (字节)
    UbPriority m_priority{UB_PRIORITY_DEFAULT};     // 分段优先级
    uint32_t m_taskId{0};                           // 流的任务ID
    uint32_t m_wqeSize{0};                          // 所属WQE的size
    OrderType m_orderType{OrderType::ORDER_NO};     // 所属WQE的order类型
    uint32_t m_tpn; // 当前WqeSegment由哪一个TP进行传输

    // ========== 网络层信息 ==========
    Ipv4Address m_sip; // 源IP地址
    Ipv4Address m_dip; // 目的IP地址

    // ========== TA层信息 ==========
    uint32_t m_jettyNum;                // 来源Jetty标号，报文头中需要携带
    uint64_t m_taMsn{0};                // 所属WQE的TA层消息序号
    uint32_t m_taSsn{0};                // 本分段的TA层分段序号
    UbTransactionSegmentKind m_segmentKind{UbTransactionSegmentKind::REQUEST};
    uint32_t m_originJettyNum{UINT32_MAX};
    uint32_t m_requestTassn{UINT32_MAX};
    TaOpcode m_requestOpcode{TaOpcode::TA_OPCODE_WRITE};
    uint32_t m_responseBytes{0};
    uint64_t m_remoteAddress{0};
    bool m_needsTransactionResponse{true};
    uint32_t m_resLenBytes{0};
    uint32_t m_payloadBytes{0};
    uint32_t m_carrierBytes{0};

    // ========== TP层静态信息 (调度时设置，之后不变) ==========
    uint64_t m_tpMsn{0};    // TP层消息序号
    uint64_t m_psnStart{0}; // TP层起始包序号 (Packet Sequence Number)
    uint32_t m_psnSize{0};  // TP层包数量

    // ========== TP层动态信息 (发送时实时更新) ==========
    uint32_t m_bytesLeft{0}; // 剩余字节数
};

/**
 * @brief Transport Group标签结构体
 *
 * 位字段格式: [src:16][dest:16][priority:4][type:2][id:4] = 42 bits
 * 用于标识传输组，管理多个相关的TP通道。
 */
struct TpgTag {
    struct Fields {
        uint64_t src : 18;
        uint64_t dest : 18;
        uint64_t priority : 4;
        uint64_t type : 2;
        uint64_t id : 4;
        uint64_t reserved : 18;
    };

    union {
        uint64_t value;
        Fields fields;
    };

    // 构造函数
    TpgTag()
        : value(0)
    {
    }

    explicit TpgTag(uint64_t v)
        : value(v)
    {
    }

    TpgTag(uint16_t src, uint16_t dest, uint8_t priority, uint8_t type, uint8_t id = 0)
        : value(0)
    {
        fields.src = src;
        fields.dest = dest;
        fields.priority = priority;
        fields.type = type;
        fields.id = id;
    }

    // 字段提取函数
    uint16_t GetSrc() const
    {
        return fields.src;
    }

    uint16_t GetDest() const
    {
        return fields.dest;
    }

    uint8_t GetPriority() const
    {
        return fields.priority;
    }

    uint8_t GetType() const
    {
        return fields.type;
    }

    uint8_t GetId() const
    {
        return fields.id;
    }

    // 字段设置函数
    void SetSrc(uint16_t src)
    {
        fields.src = src;
    }

    void SetDest(uint16_t dest)
    {
        fields.dest = dest;
    }

    void SetPriority(uint8_t priority)
    {
        fields.priority = priority;
    }

    void SetType(uint8_t type)
    {
        fields.type = type;
    }

    void SetId(uint8_t id)
    {
        fields.id = id;
    }

    // 工具函数
    bool IsValid() const
    {
        return value != 0;
    }

    void Reset()
    {
        value = 0;
    }

    // 操作符重载
    explicit operator uint64_t() const
    {
        return value;
    }

    bool operator==(const TpgTag& other) const
    {
        return value == other.value;
    }

    bool operator!=(const TpgTag& other) const
    {
        return value != other.value;
    }

    bool operator<(const TpgTag& other) const
    {
        return value < other.value;
    }
};

// ============================================================================
// 预定义常量
// ============================================================================
// const TpgTag INVALID_TPG_TAG{0};     // 无效TPG标签

// Load Balance Mode values
constexpr bool LB_MODE_PER_FLOW = false;  // 0: per flow
constexpr bool LB_MODE_PER_PACKET = true; // 1: per packet

// Routing Policy values
constexpr bool ROUTING_ALL_PATHS = false; // 0: all paths
constexpr bool ROUTING_SHORTEST = true;   // 1: shortest paths

} // namespace ns3

#endif /* UB_DATATYPE_H */
