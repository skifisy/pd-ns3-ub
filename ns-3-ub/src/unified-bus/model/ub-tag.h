// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TAG_H
#define UB_TAG_H
#include <unordered_map>
#include <vector>
#include <ns3/tag.h>

namespace ns3 {

enum class PacketType {
    PACKET,
    ACK,
    NAK,
    SACK,
    CONTROL_FRAME
};

/**
  * @brief UbPacketTraceTag use this struct to record path.
  */
struct PortTrace {
    uint32_t recvPort;
    uint64_t recvTime;
    uint32_t sendPort;
    uint64_t sendTime;

    bool operator()(const PortTrace& p1, const PortTrace& p2) const
    {
        if (p1.recvTime == p2.recvTime) {
            return p1.sendTime < p2.sendTime;
        }
        return p1.recvTime < p2.recvTime;
    }
};
/**
  * @brief Tag for recording packet transmission path.
  */
class UbPacketTraceTag : public Tag {
public:
    /**
     * @brief Constructor
     */
    UbPacketTraceTag() {}

    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::UbPacketTraceTag")
                                .SetParent<Tag>()
                                .AddConstructor<UbPacketTraceTag>();
        return tid;
    }

    TypeId GetInstanceTypeId() const override
    {
        return GetTypeId();
    }

    /**
     * @returns the number of bytes required to serialize the data of the tag.
     */
    uint32_t GetSerializedSize() const override
    {
        return m_pathLenth * (m_portTraceSize + sizeof(uint32_t)) + sizeof(m_pathLenth);
    }

    /**
     * @param i the buffer to write data into.
     *
     * Write the content of the tag in the provided tag buffer.
     * DO NOT attempt to write more bytes than you requested
     * with Tag::GetSerializedSize.
     */
    void Serialize(TagBuffer i) const override
    {
        i.WriteU32(m_pathLenth);
        for (uint32_t idx = 0; idx < m_pathLenth; idx++) {
            uint32_t node = m_nodeTrace[idx];
            i.WriteU32(node);
            i.WriteU32(m_portTrace.find(node)->second.recvPort);
            i.WriteU64(m_portTrace.find(node)->second.recvTime);
            i.WriteU32(m_portTrace.find(node)->second.sendPort);
            i.WriteU64(m_portTrace.find(node)->second.sendTime);
        }
    }

    /**
     * @param i the buffer to read data from.
     *
     * Read the content of the tag from the provided tag buffer.
     * DO NOT attempt to read more bytes than you wrote with
     * Tag::Serialize.
     */
    void Deserialize(TagBuffer i) override
    {
        m_pathLenth = (uint32_t)i.ReadU32();
        for (uint32_t idx = 0; idx < m_pathLenth; idx++) {
            uint32_t node = (uint32_t)i.ReadU32();
            m_nodeTrace.push_back(node);
            PortTrace trace;
            trace.recvPort = (uint32_t)i.ReadU32();
            trace.recvTime = (uint64_t)i.ReadU64();
            trace.sendPort = (uint32_t)i.ReadU32();
            trace.sendTime = (uint64_t)i.ReadU64();
            m_portTrace[node] = trace;
        }
    }

    /**
     * @param os the stream to print to
     *
     * This method is typically invoked from the Packet::PrintByteTags
     * or Packet::PrintPacketTags methods.
     */
    void Print(std::ostream& os) const override
    {
        os << "Print trace. Lenth:" << m_pathLenth << std::endl;
        for (uint32_t i = 0; i < m_pathLenth; i++) {
            uint32_t node = m_nodeTrace[i];
            os << "node:" << node
               << " inport:" << m_portTrace.find(node)->second.recvPort
               << " intime:" << m_portTrace.find(node)->second.recvTime
               << " outport:" << m_portTrace.find(node)->second.sendPort
               << " outtime:" << m_portTrace.find(node)->second.sendTime << std::endl;
        }
    }

    void AddPortSendTrace(uint32_t node, uint32_t sendPort, uint64_t time)
    {
        // 找到了该键值，仅更新其send记录
        if (m_portTrace.find(node) != m_portTrace.end()) {
            m_portTrace[node].sendPort = sendPort;
            m_portTrace[node].sendTime = time;
        } else { // 未找到则新建记录
            m_pathLenth += 1;
            m_nodeTrace.push_back(node);
            PortTrace trace;
            trace.sendPort = sendPort;
            trace.sendTime = time;
            m_portTrace[node] = trace;
        }
    }

    void AddPortRecvTrace(uint32_t node, uint32_t recvPort, uint64_t time)
    {
        // 找到了该键值，仅更新其recv记录
        if (m_portTrace.find(node) != m_portTrace.end()) {
            m_portTrace[node].recvPort = recvPort;
            m_portTrace[node].recvTime = time;
        } else { // 未找到则新建记录
            m_pathLenth += 1;
            m_nodeTrace.push_back(node);
            PortTrace trace;
            trace.recvPort = recvPort;
            trace.recvTime = time;
            m_portTrace[node] = trace;
        }
    }

    uint32_t GetTraceLenth() {return m_pathLenth;}

    uint32_t GetNodeTrace(uint32_t i)
    {
        return m_nodeTrace[i];
    }

    PortTrace GetPortTrace(uint32_t node)
    {
        if (m_portTrace.find(node) != m_portTrace.end()) {
            return m_portTrace[node];
        } else {
            return {0};
        }
    }

private:
    uint32_t m_pathLenth = 0;
    std::vector<uint32_t> m_nodeTrace;
    std::unordered_map<uint32_t, PortTrace> m_portTrace;
    const uint32_t m_portTraceSize = 24;
};

class UbFlowTag : public Tag {
    /**
      * @brief Tag for
      */
public:
    /**
     * @brief Constructor
     */
    UbFlowTag()
        : Tag()
    {
    }

    /**
     * @brief Constructor
     */
    UbFlowTag(uint32_t flowId, uint32_t size)
        : Tag(),
          m_flowId(flowId),
          m_flowSize(size)
    {
    }

    /**
     * @brief Get the type ID.
     * @return the object TypeId
     */
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::UbFlowTag")
                                .SetParent<Tag>()
                                .AddConstructor<UbFlowTag>();
        return tid;
    }

    TypeId GetInstanceTypeId() const override
    {
        return GetTypeId();
    }

    /**
     * @returns the number of bytes required to serialize the data of the tag.
     */
    uint32_t GetSerializedSize() const override
    {
        return 8;
    }

    /**
     * @param i the buffer to write data into.
     *
     * Write the content of the tag in the provided tag buffer.
     * DO NOT attempt to write more bytes than you requested
     * with Tag::GetSerializedSize.
     */
    void Serialize(TagBuffer i) const override
    {
        i.WriteU32(m_flowId);
        i.WriteU32(m_flowSize);
    }

    /**
     * @param i the buffer to read data from.
     *
     * Read the content of the tag from the provided tag buffer.
     * DO NOT attempt to read more bytes than you wrote with
     * Tag::Serialize.
     */
    void Deserialize(TagBuffer i) override
    {
        m_flowId = (uint32_t)i.ReadU32();
        m_flowSize = (uint32_t)i.ReadU32();
    }

    /**
     * @param os the stream to print to
     *
     * This method is typically invoked from the Packet::PrintByteTags
     * or Packet::PrintPacketTags methods.
     */
    void Print(std::ostream& os) const override
    {
        os << "FlowId:" << m_flowId << " FlowSize:" << m_flowSize << std::endl;
    }

    void SetFlowId(uint32_t flowId) { m_flowId = flowId; }
    uint32_t GetFlowId() { return m_flowId; }

    void SetFlowSize(uint32_t size) { m_flowSize = size; }
    uint32_t GetFlowSize() { return m_flowSize; }

private:
    uint32_t m_flowId{0};
    uint32_t m_flowSize{0};
};

}
#endif
