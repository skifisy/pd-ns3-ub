// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_CONGESTION_CTRL_H
#define UB_CONGESTION_CTRL_H

#include <stdexcept>
#include "ns3/object.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-header.h"
#include "ns3/ub-datatype.h"
namespace ns3 {

class UbTransportChannel;

enum CongestionCtrlAlgo {
    CAQM,
    DCQCN,
};

/**
 * @brief UB congestion control base class.
 */
class UbCongestionControl : public Object {
public:
    static TypeId GetTypeId();
    UbCongestionControl();
    ~UbCongestionControl() override;

    CongestionCtrlAlgo GetCongestionAlgo() {return m_algoType;}

    virtual uint32_t GetRestCwnd() {return UB_MTU_BYTE;}

    virtual bool IsCcLimited(uint32_t bytes)
    {
        return false;
    }

    // 发送端在统一的IP-based network header上打补丁
    virtual void OnSenderPrepareIpBasedNetworkHeader(UbIpBasedNetworkHeader& header)
    {
        (void)header;
    }

    // 发送端发包，更新数据
    virtual void OnSenderDataPacketSent(uint32_t psn, uint32_t size) {}

    // 发送端重传包，更新不会把重传当成新数据的发送侧控制状态
    virtual void OnSenderRetransmissionPacketSent(uint32_t psn, uint32_t size)
    {
        (void)psn;
        (void)size;
    }

    // 交换机在包进入目标出端口 backlog 时进行处理
    virtual void OnSwitchPostEnqueue(uint32_t inPort, uint32_t outPort, Ptr<Packet> p) {}

    // 交换机收到包进行转发，对其进行处理
    virtual void OnSwitchPostDequeue(uint32_t inPort, uint32_t outPort, Ptr<Packet> p) {}

    // 接收端接到数据包后记录数据
    virtual void OnReceiverDataPacketReceived(uint64_t psn,
                                              uint32_t size,
                                              UbIpBasedNetworkHeader header)
    {
        (void)psn;
        (void)size;
        (void)header;
    }

    // 接收端生成ack上的拥塞反馈头
    virtual UbCongestionExtTph OnReceiverPrepareAckCongestionHeader(uint64_t psnStart,
                                                                    uint64_t psnEnd)
    {
        throw std::runtime_error("Congestion Ctrl not available");
    }

    // 发送端收到拥塞通知，调整窗口、速率等数据
    virtual void OnSenderCongestionNotification(TpOpcode opcode,
                                                uint32_t psn,
                                                UbCongestionExtTph header)
    {
        (void)psn;
        (void)header;
        (void)opcode;
    }

    // TPSACK-CC 在同一个反馈里同时携带 CETPH 和选择性重传扣账摘要。
    // SAETPH bitmap 由重传层消费，CC 层只看到归一化后的 CETPH 与 missing bytes。
    virtual void OnSenderCongestionNotification(TpOpcode opcode,
                                                uint32_t psn,
                                                UbCongestionExtTph header,
                                                uint32_t retransmitBytes)
    {
        (void)retransmitBytes;
        OnSenderCongestionNotification(opcode, psn, header);
    }

    // 发送端TP在发送工作清空后进入空闲态
    virtual void OnSenderTransportIdle() {}

    // 绑定switch，初始化参数
    virtual void OnSwitchAttached(Ptr<UbSwitch> sw);

    // 绑定tp，初始化参数
    virtual void OnTpAttached(Ptr<UbTransportChannel> tp) {}

    static Ptr<UbCongestionControl> Create(UbNodeType_t nodeType);

    // 选择接收端发送ACK时使用的TP opcode
    virtual TpOpcode GetAckOpcode() const
    {
        if (m_congestionCtrlEnabled)
            return TpOpcode::TP_OPCODE_ACK_WITH_CETPH;
        else
            return TpOpcode::TP_OPCODE_ACK_WITHOUT_CETPH;
    };

protected:
    CongestionCtrlAlgo m_algoType;  // 拥塞控制算法类型
    bool m_congestionCtrlEnabled;   // 开关
};

}

#endif
