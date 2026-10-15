// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TOE_CONTROLLER_H
#define UB_TOE_CONTROLLER_H

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/ptr.h"
#include "ub-toe-solver.h"
#include "ub-toe-reconfiguration.h"

#include <map>
#include <string>
#include <vector>

namespace ns3 {

struct RoutingKey;
class UbPort;

/** Runtime topology engineering controller for the fixed Mooncake OCS case. */
class UbToeController
{
  public:
    enum class Mode { ONLY, WITH_TE };
    enum class ReconfigurationMode { AUTO, GLOBAL, SELECTIVE };
    static UbToeController& Get();
    void Configure(const std::string& casePath, Mode mode, double intervalSeconds,
                   uint32_t historyWindows, double mluSlack, double switchDelaySeconds,
                   const std::string& outputDir,
                   ReconfigurationMode reconfigurationMode = ReconfigurationMode::AUTO);
    void Disable();
    bool Enabled() const { return m_enabled; }
    bool UsesEcmp() const { return m_enabled && m_mode == Mode::ONLY; }
    bool PauseNewBusinessData(uint32_t sourceHost, uint16_t sourcePort,
                              uint32_t destinationHost, uint16_t destinationPort) const;
    void OnNewDataPacket(uint32_t sourceHost, uint16_t sourcePort,
                         uint32_t destinationHost, uint16_t destinationPort,
                         uint32_t payloadBytes);
    int SelectEcmpOutPort(uint32_t leaf, const RoutingKey& key, uint64_t flowHash,
                          uint16_t inPort, bool& selectedShortestPath) const;

  private:
    using Od = UbToeSolver::Od;
    using Windows = std::map<uint64_t, std::map<Od, uint64_t>>;
    using PortPair = UbToeReconfiguration::Circuit;
    UbToeController() = default;
    void Recompute();
    void SampleWindow();
    void BeginReconfiguration(const UbToeSolver::Topology& target,
                              const UbToeSolver::Result& result);
    void PollDrain();
    void ApplyReconfiguration();
    void ResumeTraffic();
    void AbortReconfiguration();
    bool IsDrained() const;
    std::vector<uint16_t> PortsBetween(uint32_t src, uint32_t dst) const;
    std::map<Od, uint32_t> CurrentTopology() const;
    void WriteSummary(const UbToeSolver::Result& result, const char* status);
    void WriteTopology(const UbToeSolver::Topology& topology, const char* status) const;
    void WriteReconfiguration(const char* status) const;
    void WriteCircuits(const std::vector<PortPair>& circuits, const char* status) const;
    bool VerifyPhysicalTopology() const;

    bool m_enabled{false};
    bool m_paused{false};
    bool m_selective{true};
    Mode m_mode{Mode::ONLY};
    std::string m_casePath;
    std::string m_outputDir;
    double m_intervalSeconds{300.0};
    uint32_t m_historyWindows{20};
    double m_mluSlack{0.01};
    Time m_switchDelay{Seconds(0)};
    Time m_linkDelay{NanoSeconds(0)};
    Time m_maxLinkDelay{NanoSeconds(0)};
    uint64_t m_epoch{0};
    uint32_t m_drainPolls{0};
    Time m_pauseStart{Seconds(0)};
    Time m_drainFinish{Seconds(0)};
    UbToeSolver::Result m_pendingResult;
    std::map<std::pair<uint32_t, uint16_t>, uint32_t> m_hostPortToLeaf;
    std::map<uint32_t, uint32_t> m_groups;
    std::map<uint32_t, std::vector<Ptr<UbPort>>> m_leafPorts;
    std::map<Od, std::vector<uint16_t>> m_linkPorts;
    std::vector<PortPair> m_currentCircuits;
    UbToeReconfiguration::Plan m_plan;
    uint64_t m_unpausedPayloadBytes{0};
    std::map<std::pair<uint32_t, uint16_t>, uint64_t> m_guardTxPackets;
    Windows m_windows;
    std::map<uint64_t, std::map<Od, uint64_t>> m_history;
    UbToeSolver::Topology m_pendingTopology;
    EventId m_windowEvent;
    EventId m_recomputeEvent;
    EventId m_drainEvent;
    EventId m_timeoutEvent;
    EventId m_applyEvent;
};

} // namespace ns3

#endif // UB_TOE_CONTROLLER_H
