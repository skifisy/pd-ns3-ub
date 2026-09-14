// SPDX-License-Identifier: GPL-2.0-only
#include "ns3/ub-congestion-control.h"
#include "ns3/ub-caqm.h"
#include "ns3/ub-dcqcn.h"
#include "ns3/boolean.h"
#include "ns3/enum.h"
#include "ns3/ub-switch.h"
#include "ns3/ub-transport.h"
namespace ns3 {

NS_LOG_COMPONENT_DEFINE("UbCongestionControl");
NS_OBJECT_ENSURE_REGISTERED(UbCongestionControl);

GlobalValue g_congestionCtrlAlgo =
    GlobalValue("UB_CC_ALGO",
                "Host/switch congestion control algorithm.",
                EnumValue(CongestionCtrlAlgo::CAQM),
                MakeEnumChecker(CongestionCtrlAlgo::CAQM,
                                "CAQM",
                                CongestionCtrlAlgo::DCQCN,
                                "DCQCN"));

GlobalValue g_congestionCtrlEnabled =
    GlobalValue("UB_CC_ENABLED",
                "Enable end-to-end congestion control on all transport channels.",
                BooleanValue(false),
                MakeBooleanChecker());

TypeId UbCongestionControl::GetTypeId(void)
{
    static TypeId tid =
        TypeId("ns3::UbCongestionControl")
            .SetParent<ns3::Object>()
            .AddConstructor<UbCongestionControl>();
    return tid;
}

UbCongestionControl::UbCongestionControl()
{
    BooleanValue ccEnabledval;
    g_congestionCtrlEnabled.GetValue(ccEnabledval);
    m_congestionCtrlEnabled = ccEnabledval.Get();
    EnumValue<CongestionCtrlAlgo> algoValue;
    g_congestionCtrlAlgo.GetValue(algoValue);
    m_algoType = algoValue.Get();
    NS_LOG_DEBUG("enabled: " << m_congestionCtrlEnabled << " algo:" << m_algoType);
}

UbCongestionControl::~UbCongestionControl()
{
}

void
UbCongestionControl::OnSwitchAttached(Ptr<UbSwitch> sw)
{
    if (sw != nullptr)
    {
        sw->SetCongestionCtrl(this);
    }
}

Ptr<UbCongestionControl> UbCongestionControl::Create(UbNodeType_t nodeType)
{
    EnumValue<CongestionCtrlAlgo> val;
    g_congestionCtrlAlgo.GetValue(val);
    CongestionCtrlAlgo algo = val.Get();
    if (algo == CAQM && nodeType == UB_DEVICE) {
        return CreateObject<UbHostCaqm>();
    } else if (algo == CAQM && nodeType == UB_SWITCH) {
        return CreateObject<UbSwitchCaqm>();
    } else if (algo == DCQCN && nodeType == UB_DEVICE) {
        return CreateObject<UbHostDcqcn>();
    } else if (algo == DCQCN && nodeType == UB_SWITCH) {
        return CreateObject<UbSwitchDcqcn>();
    } else {
        // Other congestion control algorithms to be extended
        return nullptr;
    }
}
}
