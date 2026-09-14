// SPDX-License-Identifier: GPL-2.0-only
#include "ub-utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>
#include <unordered_map>
#ifdef NS3_MPI
#include "ub-remote-link.h"
#include "ns3/mpi-interface.h"
#endif

using namespace std;
using namespace ns3;

namespace
{
struct LegacyNetworkAttributeKey
{
    const char* legacy;
    const char* replacement;
    const char* note;
};

constexpr std::array<LegacyNetworkAttributeKey, 6> kLegacyNetworkAttributeKeys = {{
    {"ns3::UbQueueManager::ResumeOffset",
     "ns3::UbQueueManager::DynamicPfcResumeGapBytes",
     "DynamicPfcResumeGapBytes is the current dynamic-PFC XON/XOFF resume gap."},
    {"ns3::UbSwitch::EnableCBFC",
     "ns3::UbSwitch::FlowControl \"CBFC\"",
     "FlowControl now selects the CBFC/PFC policy directly."},
    {"ns3::UbSwitch::EnablePFC",
     "ns3::UbSwitch::FlowControl \"PFC_FIXED\" or \"PFC_DYNAMIC\"",
     "FlowControl now selects the CBFC/PFC policy directly."},
    {"ns3::UbApiThread::",
     "ns3::UbLdstThread::",
     "LD/ST thread attributes moved to the UbLdstThread TypeId."},
    {"ns3::UbJetty::UbInflightMax",
     "ns3::UbJetty::UbJettyInflightMax",
     "Jetty inflight control was renamed."},
    {"ns3::UbCtpTransportService::MaxOutstandingTransactions",
     "ns3::UbJetty::UbJettyInflightMax",
     "CTP no longer owns a separate send-admission window; outstanding back-pressure belongs to Jetty."},
}};

struct NetworkAttributeAlias
{
    const char* legacy;
    const char* replacement;
};

constexpr std::array<NetworkAttributeAlias, 2> kNetworkAttributeAliases = {{
    {"ns3::UbTransportChannel::InitialRTO", "ns3::UbTransportChannel::BaseRTO"},
    {"ns3::UbTransportChannel::EnableFastSelectiveRetrans",
     "ns3::UbTransportChannel::EnableFastRetrans"},
}};

std::string
DisplayFilename(const std::string& filename)
{
    return std::filesystem::path(filename).filename().string();
}

uint64_t
MixLinkDelayOffsetHash(uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

uint32_t
ParseUint32Field(const std::string& line, size_t valueStart, size_t valueEnd)
{
    uint32_t value = 0;
    for (size_t pos = valueStart; pos < valueEnd; ++pos)
    {
        const char c = line[pos];
        NS_ABORT_MSG_IF(c < '0' || c > '9',
                        "Invalid uint32 field in traffic.csv; signed values are not supported");
        const uint32_t digit = static_cast<uint32_t>(c - '0');
        NS_ABORT_MSG_IF(value > (std::numeric_limits<uint32_t>::max() - digit) / 10,
                        "traffic.csv uint32 field overflow");
        value = value * 10 + digit;
    }
    return value;
}

uint32_t
ParseUint32Field(std::string_view field)
{
    NS_ABORT_MSG_IF(field.empty(), "Missing uint32 field in traffic.csv");
    uint32_t value = 0;
    for (char c : field)
    {
        NS_ABORT_MSG_IF(c < '0' || c > '9',
                        "Invalid uint32 field in traffic.csv; signed values are not supported");
        const uint32_t digit = static_cast<uint32_t>(c - '0');
        NS_ABORT_MSG_IF(value > (std::numeric_limits<uint32_t>::max() - digit) / 10,
                        "traffic.csv uint32 field overflow");
        value = value * 10 + digit;
    }
    return value;
}

uint8_t
ParseTrafficPriorityField(std::string_view field)
{
    const uint32_t priority = ParseUint32Field(field);
    NS_ABORT_MSG_IF(priority > UB_PRIORITY_MAX,
                    "Invalid priority field in traffic.csv; valid range is 0.."
                        << static_cast<uint32_t>(UB_PRIORITY_MAX));
    return static_cast<uint8_t>(priority);
}

std::string_view
TrimField(std::string_view field)
{
    while (!field.empty() &&
           (field.front() == ' ' || field.front() == '\t' || field.front() == '\r' ||
            field.front() == '\n'))
    {
        field.remove_prefix(1);
    }
    while (!field.empty() &&
           (field.back() == ' ' || field.back() == '\t' || field.back() == '\r' ||
            field.back() == '\n'))
    {
        field.remove_suffix(1);
    }
    return field;
}

std::string
TrimFieldCopy(std::string_view field)
{
    field = TrimField(field);
    return std::string(field.data(), field.size());
}

std::vector<std::string>
SplitCsvRow(const std::string& line)
{
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ','))
    {
        fields.push_back(TrimFieldCopy(field));
    }
    if (!line.empty() && line.back() == ',')
    {
        fields.emplace_back();
    }
    return fields;
}

std::map<std::string, std::size_t>
BuildCsvHeaderIndex(const std::vector<std::string>& header)
{
    std::map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < header.size(); ++i)
    {
        index.emplace(header[i], i);
    }
    return index;
}

std::string
GetRequiredCsvField(const std::vector<std::string>& fields,
                    const std::map<std::string, std::size_t>& headerIndex,
                    const std::string& column,
                    const std::string& filename)
{
    const auto it = headerIndex.find(column);
    NS_ABORT_MSG_IF(it == headerIndex.end(),
                    DisplayFilename(filename) << " missing required column: " << column);
    NS_ABORT_MSG_IF(it->second >= fields.size(),
                    DisplayFilename(filename) << " row missing required field: " << column);
    return fields[it->second];
}

std::string
GetOptionalCsvField(const std::vector<std::string>& fields,
                    const std::map<std::string, std::size_t>& headerIndex,
                    const std::string& column)
{
    const auto it = headerIndex.find(column);
    if (it == headerIndex.end() || it->second >= fields.size())
    {
        return "";
    }
    return fields[it->second];
}

bool
IsLegacyNodeForwardDelayHeader(const std::vector<std::string>& header)
{
    return header.size() == 4 && header[0] == "nodeId" && header[1] == "nodeType" &&
           header[2] == "portNum" && header[3] == "forwardDelay";
}

void
AppendDependencyPhases(std::string_view field, TrafficRecord& record)
{
    while (!field.empty())
    {
        while (!field.empty() &&
               (field.front() == ' ' || field.front() == '\t' || field.front() == '\r'))
        {
            field.remove_prefix(1);
        }
        if (field.empty())
        {
            break;
        }

        size_t tokenEnd = 0;
        while (tokenEnd < field.size() && field[tokenEnd] != ' ' && field[tokenEnd] != '\t' &&
               field[tokenEnd] != '\r')
        {
            ++tokenEnd;
        }

        record.dependOnPhases.push_back(ParseUint32Field(field.substr(0, tokenEnd)));
        field.remove_prefix(tokenEnd);
    }
}

void
SetTrafficRecordField(int fieldCount, std::string_view rawField, TrafficRecord& record)
{
    const std::string_view field = TrimField(rawField);
    switch (fieldCount)
    {
    case 0:
        record.taskId = static_cast<int>(ParseUint32Field(field));
        break;
    case 1:
        record.sourceNode = static_cast<int>(ParseUint32Field(field));
        break;
    case 2:
        record.destNode = static_cast<int>(ParseUint32Field(field));
        break;
    case 3:
        record.dataSize = static_cast<int>(ParseUint32Field(field));
        break;
    case 4:
        record.opType.assign(field.data(), field.size());
        break;
    case 5:
        record.priority = static_cast<int>(ParseTrafficPriorityField(field));
        break;
    case 6:
        record.delay.assign(field.data(), field.size());
        break;
    case 7:
        record.phaseId = static_cast<int>(ParseUint32Field(field));
        break;
    case 8:
        AppendDependencyPhases(field, record);
        break;
    case 9:
        record.srcEntityId = field.empty() ? 0 : ParseUint32Field(field);
        record.hasSrcEntityId = true;
        break;
    case 10:
        record.dstEntityId = field.empty() ? 0 : ParseUint32Field(field);
        record.hasDstEntityId = true;
        break;
    }
}

std::pair<uint32_t, uint32_t>
ParseUint32RangeField(const std::string& field)
{
    const size_t dotPos = field.find("..");
    if (dotPos == std::string::npos)
    {
        const uint32_t value = static_cast<uint32_t>(std::stoul(field));
        return {value, value};
    }

    const uint32_t start = static_cast<uint32_t>(std::stoul(field.substr(0, dotPos)));
    const uint32_t end = static_cast<uint32_t>(std::stoul(field.substr(dotPos + 2)));
    NS_ABORT_MSG_IF(start > end, "invalid numeric range: " << field);
    return {start, end};
}

std::string
FormatUint32Range(uint32_t start, uint32_t end)
{
    if (start == end)
    {
        return std::to_string(start);
    }
    return std::to_string(start) + ".." + std::to_string(end);
}

bool
RangesOverlap(uint32_t leftStart, uint32_t leftEnd, uint32_t rightStart, uint32_t rightEnd)
{
    return leftStart <= rightEnd && rightStart <= leftEnd;
}

using RouteValueSignature = std::vector<std::pair<uint16_t, uint32_t>>;

RouteValueSignature
NormalizeRouteValue(const std::vector<uint16_t>& outports,
                    const std::vector<uint32_t>& metrics,
                    uint32_t rowNumber)
{
    NS_ABORT_MSG_IF(outports.size() != metrics.size(),
                    "routing_table.csv row " << rowNumber
                                             << " outports size not equal metrics size");
    NS_ABORT_MSG_IF(outports.empty(),
                    "routing_table.csv row " << rowNumber
                                             << " must contain at least one outPort/metric pair");

    std::map<uint16_t, uint32_t> metricByPort;
    for (auto index = 0u; index < outports.size(); ++index)
    {
        auto [it, inserted] = metricByPort.emplace(outports[index], metrics[index]);
        NS_ABORT_MSG_IF(!inserted && it->second != metrics[index],
                        "routing_table.csv row "
                            << rowNumber << " configures outPort " << outports[index]
                            << " with multiple metrics");
    }

    RouteValueSignature value;
    value.reserve(metricByPort.size());
    for (const auto& [outport, metric] : metricByPort)
    {
        value.emplace_back(outport, metric);
    }
    return value;
}

struct RouteRangeCsvRow
{
    uint32_t rowNumber;
    uint32_t nodeStart;
    uint32_t nodeEnd;
    uint32_t destStart;
    uint32_t destEnd;
    uint32_t destPort;
    RouteValueSignature value;
};

void
ValidateRouteRangeRowsNoConflicts(const std::vector<RouteRangeCsvRow>& rows)
{
    std::map<uint32_t, std::vector<const RouteRangeCsvRow*> > rowsByDestPort;
    for (const auto& row : rows)
    {
        rowsByDestPort[row.destPort].push_back(&row);
    }

    for (auto& [destPort, portRows] : rowsByDestPort)
    {
        std::sort(portRows.begin(),
                  portRows.end(),
                  [](const RouteRangeCsvRow* left, const RouteRangeCsvRow* right) {
                      if (left->nodeStart != right->nodeStart)
                      {
                          return left->nodeStart < right->nodeStart;
                      }
                      if (left->nodeEnd != right->nodeEnd)
                      {
                          return left->nodeEnd < right->nodeEnd;
                      }
                      return left->destStart < right->destStart;
                  });

        std::vector<const RouteRangeCsvRow*> activeRows;
        for (const auto* row : portRows)
        {
            activeRows.erase(std::remove_if(activeRows.begin(),
                                            activeRows.end(),
                                            [row](const RouteRangeCsvRow* active) {
                                                return active->nodeEnd < row->nodeStart;
                                            }),
                             activeRows.end());

            for (const auto* active : activeRows)
            {
                if (active->value == row->value)
                {
                    continue;
                }
                if (!RangesOverlap(active->destStart, active->destEnd, row->destStart, row->destEnd))
                {
                    continue;
                }

                const uint32_t nodeOverlapStart = std::max(active->nodeStart, row->nodeStart);
                const uint32_t nodeOverlapEnd = std::min(active->nodeEnd, row->nodeEnd);
                const uint32_t destOverlapStart = std::max(active->destStart, row->destStart);
                const uint32_t destOverlapEnd = std::min(active->destEnd, row->destEnd);
                NS_ABORT_MSG("routing_table.csv compressed route conflict between rows "
                             << active->rowNumber << " and " << row->rowNumber
                             << " at nodeId="
                             << FormatUint32Range(nodeOverlapStart, nodeOverlapEnd)
                             << ", dstNodeId="
                             << FormatUint32Range(destOverlapStart, destOverlapEnd)
                             << ", dstPortId=" << destPort);
            }
            activeRows.push_back(row);
        }
    }
}

bool
RewriteDefaultAttributeLine(std::string& line, const NetworkAttributeAlias& alias)
{
    const auto firstNonSpace = line.find_first_not_of(" \t");
    if (firstNonSpace == std::string::npos || line[firstNonSpace] == '#')
    {
        return false;
    }

    std::istringstream tokens(line.substr(firstNonSpace));
    std::string directive;
    std::string attribute;
    tokens >> directive >> attribute;
    if (directive != "default" || attribute != alias.legacy)
    {
        return false;
    }

    const auto attributePos = line.find(attribute, firstNonSpace + directive.size());
    NS_ASSERT_MSG(attributePos != std::string::npos, "Parsed network attribute missing from line");
    line.replace(attributePos, attribute.size(), alias.replacement);
    return true;
}

std::string
CreateConfigStoreInput(const std::string& filename, uint32_t& rewrittenAliasLines)
{
    std::ifstream input(filename.c_str());
    NS_ASSERT_MSG(input.good(), "Can not open File: " << filename);

    rewrittenAliasLines = 0;
    std::ostringstream rewritten;
    std::string line;
    while (std::getline(input, line))
    {
        for (const auto& alias : kNetworkAttributeAliases)
        {
            if (RewriteDefaultAttributeLine(line, alias))
            {
                ++rewrittenAliasLines;
                break;
            }
        }
        rewritten << line << '\n';
    }

    if (rewrittenAliasLines == 0)
    {
        return filename;
    }

    const auto uniqueSuffix =
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path rewrittenPath =
        std::filesystem::temp_directory_path() /
        ("ub-network-attribute-rewritten-" + uniqueSuffix + ".txt");

    std::ofstream output(rewrittenPath.c_str(), std::ios::trunc);
    NS_ASSERT_MSG(output.good(), "Can not create rewritten config file: " << rewrittenPath);
    output << rewritten.str();
    return rewrittenPath.string();
}

void
SetTrafficRecordViewField(int fieldCount, std::string_view rawField, TrafficRecordView& record)
{
    const std::string_view field = TrimField(rawField);
    switch (fieldCount)
    {
    case 0:
        record.taskId = ParseUint32Field(field);
        break;
    case 1:
        record.sourceNode = ParseUint32Field(field);
        break;
    case 2:
        record.destNode = ParseUint32Field(field);
        break;
    case 3:
        record.dataSize = ParseUint32Field(field);
        break;
    case 4:
        record.opType = field;
        break;
    case 5:
        record.priority = ParseTrafficPriorityField(field);
        break;
    case 6:
        record.delay = field;
        break;
    case 7:
        record.phaseId = ParseUint32Field(field);
        break;
    case 8:
        record.dependOnPhases = field;
        break;
    case 9:
        record.srcEntityId = field.empty() ? 0 : ParseUint32Field(field);
        record.hasSrcEntityId = true;
        break;
    case 10:
        record.dstEntityId = field.empty() ? 0 : ParseUint32Field(field);
        record.hasDstEntityId = true;
        break;
    }
}

void
ValidateLegacyNetworkAttributeKeys(const std::string& filename)
{
    std::ifstream file(filename.c_str());
    NS_ASSERT_MSG(file.good(), "Can not open File: " << filename);

    std::string line;
    uint32_t lineNumber = 0;
    while (std::getline(file, line))
    {
        ++lineNumber;
        const auto firstNonSpace = line.find_first_not_of(" \t");
        if (firstNonSpace == std::string::npos || line[firstNonSpace] == '#')
        {
            continue;
        }

        for (const auto& key : kLegacyNetworkAttributeKeys)
        {
            if (line.find(key.legacy) == std::string::npos)
            {
                continue;
            }

            NS_FATAL_ERROR("Legacy network_attribute.txt key: "
                           << key.legacy << " at " << filename << ":" << lineNumber
                           << ". Use " << key.replacement << " instead. " << key.note);
        }
    }
}

bool
IsNodeOwnedByCurrentRank(Ptr<Node> node)
{
#ifdef NS3_MPI
    if (!MpiInterface::IsEnabled() || MpiInterface::GetSize() <= 1)
    {
        return true;
    }
    return utils::UbUtils::IsSystemOwnedByRank(node->GetSystemId(), MpiInterface::GetSystemId());
#else
    return true;
#endif
}

void
PreloadLocalTpIfOwned(Ptr<Node> node,
                      uint32_t src,
                      uint32_t dest,
                      uint8_t sport,
                      uint8_t dport,
                      UbPriority priority,
                      uint32_t srcTpn,
                      uint32_t dstTpn)
{
    if (!IsNodeOwnedByCurrentRank(node))
    {
        return;
    }

    Ptr<UbController> ctrl = node->GetObject<UbController>();
    NS_ASSERT_MSG(ctrl != nullptr, "Preloaded TP endpoint must have UbController");
    if (ctrl->IsTPExists(srcTpn))
    {
        return;
    }

    auto congestionCtrl = UbCongestionControl::Create(UB_DEVICE);
    ctrl->CreateTp(src, dest, sport, dport, priority, srcTpn, dstTpn, congestionCtrl);
}

Ptr<UbLink>
CreateUbChannelBetween(Ptr<UbPort> p1, Ptr<UbPort> p2, Time delay)
{
    Ptr<UbLink> channel;
#ifdef NS3_MPI
    if (!utils::UbUtils::IsSameMpiRank(p1->GetNode()->GetSystemId(),
                                       p2->GetNode()->GetSystemId()))
    {
        channel = CreateObject<UbRemoteLink>();
        p1->EnableMpiReceive();
        p2->EnableMpiReceive();
    }
    else
#endif
    {
        channel = CreateObject<UbLink>();
    }

    channel->SetAttribute("Delay", TimeValue(delay));
    p1->Attach(channel);
    p2->Attach(channel);
    return channel;
}

} // namespace

namespace utils {

UbUtils::TraceFileState&
UbUtils::GetTraceFile(const std::string& fileName)
{
    namespace fs = std::filesystem;

    std::lock_guard<std::mutex> filesGuard(files_mutex);
    auto [it, inserted] = files.try_emplace(fileName);
    if (inserted)
    {
        std::error_code ec;
        const fs::path parent = fs::path(fileName).parent_path();
        if (!parent.empty())
        {
            fs::create_directories(parent, ec);
            NS_ASSERT_MSG(!ec, "Failed to create trace parent dir: " << parent << " err=" << ec.message());
        }
        it->second.stream.open(fileName.c_str(), std::ios::out | std::ios::app);
        NS_ASSERT_MSG(it->second.stream.is_open(), "Can not open File: " << fileName);
        it->second.pending.reserve(TRACE_FLUSH_THRESHOLD_BYTES);
    }
    return it->second;
}

void
UbUtils::FlushTraceFile(TraceFileState& fileState)
{
    std::lock_guard<std::mutex> fileGuard(fileState.mutex);
    if (fileState.pending.empty())
    {
        return;
    }
    fileState.stream.write(fileState.pending.data(),
                           static_cast<std::streamsize>(fileState.pending.size()));
    fileState.pending.clear();
}

bool
UbUtils::IsTraceEnabled()
{
    BooleanValue enabled;
    if (!GlobalValue::GetValueByNameFailSafe("UB_TRACE_ENABLE", enabled))
    {
        return false;
    }
    return enabled.Get();
}

bool
UbUtils::IsFlowControlTraceEnabled()
{
    if (!IsTraceEnabled())
    {
        return false;
    }

    BooleanValue enabled;
    if (!GlobalValue::GetValueByNameFailSafe("UB_FLOW_CONTROL_TRACE_ENABLE", enabled))
    {
        return false;
    }
    return enabled.Get();
}

bool
UbUtils::IsCongestionControlTraceEnabled()
{
    if (!IsTraceEnabled())
    {
        return false;
    }

    BooleanValue enabled;
    if (!GlobalValue::GetValueByNameFailSafe("UB_CONGESTION_CONTROL_TRACE_ENABLE", enabled))
    {
        return false;
    }
    return enabled.Get();
}

bool
UbUtils::IsQueueTraceEnabled()
{
    if (!IsTraceEnabled())
    {
        return false;
    }

    BooleanValue enabled;
    if (!GlobalValue::GetValueByNameFailSafe("UB_QUEUE_TRACE_ENABLE", enabled))
    {
        return false;
    }
    return enabled.Get();
}

uint32_t
UbUtils::ExtractMpiRank(uint32_t systemId)
{
#ifdef NS3_MTP
    return systemId & 0xFFFF;
#else
    return systemId;
#endif
}

bool
UbUtils::IsSameMpiRank(uint32_t lhsSystemId, uint32_t rhsSystemId)
{
    return ExtractMpiRank(lhsSystemId) == ExtractMpiRank(rhsSystemId);
}

bool
UbUtils::IsSystemOwnedByRank(uint32_t systemId, uint32_t currentRank)
{
    return ExtractMpiRank(systemId) == currentRank;
}

bool
UbUtils::IsFaultEnabled() const
{
    BooleanValue faultEnabled;
    g_fault_enable.GetValue(faultEnabled);
    return faultEnabled.Get();
}

void
UbUtils::ResetRuntimeDropDiagnostics()
{
    std::lock_guard<std::mutex> guard(runtime_drop_mutex);
    runtime_packet_drop_count = 0;
    runtime_packet_drop_reason.clear();
}

void
UbUtils::RecordRuntimePacketDrop(const std::string& reason)
{
    std::lock_guard<std::mutex> guard(runtime_drop_mutex);
    ++runtime_packet_drop_count;
    if (runtime_packet_drop_reason.empty())
    {
        runtime_packet_drop_reason = reason;
    }
}

uint64_t
UbUtils::GetRuntimePacketDropCount()
{
    std::lock_guard<std::mutex> guard(runtime_drop_mutex);
    return runtime_packet_drop_count;
}

std::string
UbUtils::GetRuntimePacketDropReason()
{
    std::lock_guard<std::mutex> guard(runtime_drop_mutex);
    return runtime_packet_drop_reason;
}

void UbUtils::PrintTimestamp(const std::string &message)
{
    auto now = std::chrono::system_clock::now();
    std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTime = *std::localtime(&nowTime);

    std::cout << "[" << std::put_time(&localTime, "%H:%M:%S") << "] " << message << std::endl;
}

void UbUtils::ParseTrace(bool isTest)
{
    BooleanValue val;
    g_parse_enable.GetValue(val);
    bool ParseEnable = val.Get();
    if (ParseEnable) {
        PrintTimestamp("[trace] Parse runlog into output artifacts.");

        // 从GlobalValue获取路径
        StringValue scriptPathValue;
        g_python_script_path.GetValue(scriptPathValue);
        string python_script_path = scriptPathValue.Get();

        string cmd = "python3 " + python_script_path + " " + trace_path;
        if (isTest) {
            cmd += " true";
        } else {
            cmd += " false";
        }
        int ret = system(cmd.c_str());
        if (ret == -1) {
            NS_ASSERT_MSG(0, "parse trace failed :" << cmd);
        }
    }
}

void UbUtils::Destroy()
{
    for (auto& event : queue_sampler_events)
    {
        event.second.Cancel();
    }
    queue_sampler_events.clear();
    queue_sampler_started = false;

    std::lock_guard<std::mutex> filesGuard(files_mutex);
    for (auto& pair : files) {
        std::lock_guard<std::mutex> fileGuard(pair.second.mutex);
        if (!pair.second.pending.empty())
        {
            pair.second.stream.write(pair.second.pending.data(),
                                     static_cast<std::streamsize>(pair.second.pending.size()));
            pair.second.pending.clear();
        }
        if (pair.second.stream.is_open()) {
            pair.second.stream.close();
        }
    }
    files.clear();
}

std::string UbUtils::PrepareTraceDir(const std::string &configPath)
{
    namespace fs = std::filesystem;

    fs::path caseDir = fs::path(configPath).parent_path();
    if (caseDir.empty()) {
        caseDir = fs::current_path();
    }

    std::string dirPath = caseDir.string();
    if (dirPath.empty()) {
        NS_ASSERT_MSG(0, "Not find testcase dir");
    }
    if (dirPath.back() != fs::path::preferred_separator) {
        dirPath.push_back(fs::path::preferred_separator);
    }

    fs::path runlog = caseDir / "runlog";
    std::error_code ec;
    if (fs::exists(runlog, ec)) {
        NS_ASSERT_MSG(!ec, "Failed to query runlog dir: " << ec.message());
        ec.clear();
        fs::remove_all(runlog, ec);
        NS_ASSERT_MSG(!ec, "Failed to remove runlog dir: " << ec.message());
        ec.clear();
    } else {
        NS_ASSERT_MSG(!ec, "Failed to query runlog dir: " << ec.message());
    }

    fs::create_directories(runlog, ec);
    NS_ASSERT_MSG(!ec, "Failed to recreate runlog dir: " << ec.message());
    return dirPath;
}

void
UbUtils::SetTracePathForTest(const std::string& tracePath)
{
    trace_path = tracePath;
    if (!trace_path.empty() && trace_path.back() != std::filesystem::path::preferred_separator)
    {
        trace_path.push_back(std::filesystem::path::preferred_separator);
    }
}

void UbUtils::CreateTraceDir()
{
    ResetRuntimeDropDiagnostics();
    trace_path = PrepareTraceDir(g_config_path);
    queue_sampler_started = false;
    queue_sampler_events.clear();
    PrintTimestamp("[setup] Prepare runlog directory: " + trace_path + "runlog");
}

void
UbUtils::QueueSampleNotify(uint32_t nodeId, uint32_t portId)
{
    if (trace_path.empty() || !IsQueueTraceEnabled())
    {
        return;
    }

    Ptr<Node> node = NodeList::GetNode(nodeId);
    if (node == nullptr)
    {
        return;
    }

    Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(portId));
    Ptr<UbSwitch> sw = node->GetObject<UbSwitch>();
    if (port == nullptr || sw == nullptr)
    {
        return;
    }

    const uint64_t voqBytes = sw->GetQueueManager()->GetTotalOutPortBufferUsed(portId);
    const uint64_t egressBytes = port->GetUbQueue()->GetCurrentBytes();
    const uint64_t totalBytes = voqBytes + egressBytes;

    std::ostringstream oss;
    oss << "Queue Update, source: SAMPLE"
        << " voqBytes: " << voqBytes
        << " egressBytes: " << egressBytes
        << " totalBytes: " << totalBytes;
    const std::string fileName =
        trace_path + "runlog/QueueTrace_node_" + to_string(nodeId) + "_port_" + to_string(portId) + ".tr";
    PrintTraceInfo(fileName, oss.str());
}

void
UbUtils::QueueSampleTick(uint32_t nodeId, uint32_t portId, uint32_t intervalNs)
{
    QueueSampleNotify(nodeId, portId);
    queue_sampler_events[{nodeId, portId}] =
        Simulator::Schedule(NanoSeconds(intervalNs),
                            &UbUtils::QueueSampleTick,
                            nodeId,
                            portId,
                            intervalNs);
}

void
UbUtils::StartQueueSampler()
{
    if (!IsQueueTraceEnabled())
    {
        return;
    }

    if (queue_sampler_started)
    {
        return;
    }

    UintegerValue intervalValue;
    g_queue_sample_interval.GetValue(intervalValue);
    const uint32_t intervalNs = intervalValue.Get();
    if (intervalNs == 0)
    {
        return;
    }

    const Time interval = NanoSeconds(intervalNs);
    for (uint32_t nodeId = 0; nodeId < NodeList::GetNNodes(); ++nodeId)
    {
        Ptr<Node> node = NodeList::GetNode(nodeId);
        if (node == nullptr)
        {
            continue;
        }
        Ptr<UbSwitch> sw = node->GetObject<UbSwitch>();
        if (sw == nullptr)
        {
            continue;
        }
        const uint32_t devicesNum = node->GetNDevices();
        for (uint32_t portId = 0; portId < devicesNum; ++portId)
        {
            Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(portId));
            if (port == nullptr)
            {
                continue;
            }
            queue_sampler_events[{nodeId, portId}] =
                Simulator::Schedule(interval,
                                    &UbUtils::QueueSampleTick,
                                    nodeId,
                                    portId,
                                    intervalNs);
        }
    }

    queue_sampler_started = true;
}

void UbUtils::PrintTraceInfo(const string& fileName, const string& info)
{
    auto& fileState = GetTraceFile(fileName);
    std::lock_guard<std::mutex> fileGuard(fileState.mutex);
    fileState.pending += "[";
    fileState.pending += std::to_string(Simulator::Now().GetSeconds() * 1e6);
    fileState.pending += "us] ";
    fileState.pending += info;
    fileState.pending.push_back('\n');
    if (fileState.pending.size() >= TRACE_FLUSH_THRESHOLD_BYTES) {
        fileState.stream.write(fileState.pending.data(),
                               static_cast<std::streamsize>(fileState.pending.size()));
        fileState.pending.clear();
    }
}

void UbUtils::PrintTraceInfoNoTs(const string& fileName, const string& info)
{
    auto& fileState = GetTraceFile(fileName);
    std::lock_guard<std::mutex> fileGuard(fileState.mutex);
    fileState.pending += info;
    fileState.pending.push_back('\n');
    if (fileState.pending.size() >= TRACE_FLUSH_THRESHOLD_BYTES) {
        fileState.stream.write(fileState.pending.data(),
                               static_cast<std::streamsize>(fileState.pending.size()));
        fileState.pending.clear();
    }
}

inline void UbUtils::TpFirstPacketSendsNotify(
    uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn, uint32_t tpMsn, uint32_t psnSndNxt, uint32_t sPort)
{
    // 使用 std::ostringstream 来减少字符串构造的次数
    std::ostringstream oss;
    oss << "First Packet Sends, taskId: " << taskId << " srcTpn: " << tpn << " destTpn: " << dstTpn
        << " tpMsn: " << tpMsn << " psn: " << psnSndNxt << " portId: " << sPort << " lastPacket: 0";
    string info = oss.str();
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::TpLastPacketSendsNotify(
    uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn, uint32_t tpMsn, uint32_t psnSndNxt, uint32_t sPort)
{
    std::ostringstream oss;
    oss << "Last Packet Sends,taskId: " << taskId << " srcTpn: " << tpn << " destTpn: " << dstTpn << " tpMsn: " << tpMsn
        << " psn: " << psnSndNxt << " portId: " << sPort << " lastPacket: 1";
    string info = oss.str();
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::TpLastPacketACKsNotify(
    uint32_t nodeId, uint32_t taskId, uint32_t tpn, uint32_t dstTpn, uint32_t tpMsn, uint32_t psn, uint32_t sPort)
{
    std::ostringstream oss;
    oss << "Last Packet ACKs,taskId: " << taskId << " srcTpn: " << tpn << " destTpn: " << dstTpn << " tpMsn: " << tpMsn
        << " psn: " << psn << " portId: " << sPort << " lastPacket: 1";
    string info = oss.str();
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::TpLastPacketReceivesNotify(
    uint32_t nodeId, uint32_t srcTpn, uint32_t dstTpn, uint32_t tpMsn, uint32_t psn, uint32_t dPort)
{
    std::ostringstream oss;
    oss << "Last Packet Receives,srcTpn: " << srcTpn << " destTpn: " << dstTpn << " tpMsn: " << tpMsn
        << " psn: " << psn << " inportId: " << dPort << " lastPacket: 1";
    string info = oss.str();
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void
UbUtils::CtpFirstPacketSendsNotify(uint32_t nodeId,
                                   uint32_t taskId,
                                   uint32_t srcNodeId,
                                   uint32_t dstNodeId,
                                   uint32_t srcEntityId,
                                   uint32_t dstEntityId,
                                   uint32_t vl,
                                   uint32_t taSsn,
                                   uint32_t outPort,
                                   uint32_t payloadBytes,
                                   uint32_t opcode)
{
    std::ostringstream oss;
    oss << "First Packet Sends, taskId: " << taskId << " transport: CTP"
        << " srcNode: " << srcNodeId << " dstNode: " << dstNodeId
        << " srcEntity: " << srcEntityId << " dstEntity: " << dstEntityId
        << " vl: " << vl << " taSsn: " << taSsn << " outPort: " << outPort
        << " payloadBytes: " << payloadBytes << " taOpcode: " << opcode << " lastPacket: 0";
    string info = oss.str();
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void
UbUtils::CtpLastPacketACKsNotify(uint32_t nodeId,
                                 uint32_t taskId,
                                 uint32_t srcNodeId,
                                 uint32_t dstNodeId,
                                 uint32_t srcEntityId,
                                 uint32_t dstEntityId,
                                 uint32_t vl,
                                 uint32_t taSsn,
                                 uint32_t outPort,
                                 uint32_t payloadBytes,
                                 uint32_t opcode)
{
    std::ostringstream oss;
    oss << "Last Packet ACKs, taskId: " << taskId << " transport: CTP"
        << " srcNode: " << srcNodeId << " dstNode: " << dstNodeId
        << " srcEntity: " << srcEntityId << " dstEntity: " << dstEntityId
        << " vl: " << vl << " taSsn: " << taSsn;
    if (outPort != UINT32_MAX)
    {
        oss << " outPort: " << outPort;
    }
    oss << " payloadBytes: " << payloadBytes << " taOpcode: " << opcode << " lastPacket: 1";
    string info = oss.str();
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::TpWqeSegmentSendsNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn)
{
    std::ostringstream oss;
    oss << "WQE Segment Sends,taskId: " << taskId << " TASSN: " << taSsn;
    string info = oss.str();
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::TpWqeSegmentCompletesNotify(uint32_t nodeId, uint32_t taskId, uint32_t taSsn)
{
    std::ostringstream oss;
    oss << "WQE Segment Completes,taskId: " << taskId << " TASSN: " << taSsn;
    string info = oss.str();
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline string UbUtils::Among(string s, string ts)
{
    string res = s;
    // 添加空格使字符串和时间戳对齐
    if (s.size() >= ts.size()) {
        res.insert(0, 1, ' ');
        res.insert(res.end(), 1, ' ');
    } else {
        res.insert(0, (ts.size() - s.size()) / 2 + 1, ' ');
        res.insert(res.end(), ts.size() - s.size() - (ts.size() - s.size()) / 2 + 1, ' ');
    }
    return res;
}

void UbUtils::TpRecvNotify(uint32_t packetUid, uint32_t psn, uint32_t src, uint32_t dst, uint32_t srcTpn,
                           uint32_t dstTpn, PacketType type, uint32_t size, uint32_t taskId,
                           std::string ackInfo, UbPacketTraceTag traceTag)
{
    const char* pktType = "CONTROL";
    switch (type) {
    case PacketType::PACKET:
        pktType = "PKT";
        break;
    case PacketType::ACK:
        pktType = "ACK";
        break;
    case PacketType::NAK:
        pktType = "NAK";
        break;
    case PacketType::SACK:
        pktType = "SACK";
        break;
    case PacketType::CONTROL_FRAME:
        break;
    }

    std::ostringstream oss;
    oss << "Uid:" << packetUid << " Psn:" << psn << " Src:" << src << " Dst:" << dst << " SrcTpn:" << srcTpn
        << " DstTpn:" << dstTpn << " Type:" << pktType;
    if (!ackInfo.empty()) {
        oss << " AckInfo:" << ackInfo;
    }
    oss << " Size:" << size << " TaskId:" << taskId << '\n';
    for (uint32_t i = 0; i < traceTag.GetTraceLenth(); i++) {
        uint32_t node = traceTag.GetNodeTrace(i);
        PortTrace trace = traceTag.GetPortTrace(node);
        if (i == 0) {
            oss << "[" << node << "][" << Among(std::to_string(trace.sendPort), std::to_string(trace.sendTime)) << "]"
                << "--->";
        } else if (i == traceTag.GetTraceLenth() - 1) {
            oss << "[" << Among(std::to_string(trace.recvPort), std::to_string(trace.recvTime)) << "][" << node << "]"
                << '\n';
        } else {
            oss << "[" << Among(std::to_string(trace.recvPort), std::to_string(trace.recvTime)) << "]"
                << "[" << node << "]"
                << "[" << Among(std::to_string(trace.sendPort), std::to_string(trace.sendTime)) << "]"
                << "--->";
        }
    }
    for (uint32_t i = 0; i < traceTag.GetTraceLenth(); i++) {
        uint32_t node = traceTag.GetNodeTrace(i);
        PortTrace trace = traceTag.GetPortTrace(node);
        if (i == 0) {
            oss << std::string(std::to_string(node).size() + 2, ' ') << "["
                << Among(std::to_string(trace.sendTime), std::to_string(trace.sendTime)) << "]" << std::string(4, ' ');
        } else if (i == traceTag.GetTraceLenth() - 1) {
            oss << "[" << Among(std::to_string(trace.recvTime), std::to_string(trace.recvTime)) << "]" << '\n';
        } else {
            oss << "[" << Among(std::to_string(trace.recvTime), std::to_string(trace.recvTime)) << "]"
                << std::string(std::to_string(node).size() + 2, ' ') << "["
                << Among(std::to_string(trace.sendTime), std::to_string(trace.sendTime)) << "]" << std::string(4, ' ');
        }
    }
    string info = oss.str();
    const char* fileType = (type == PacketType::NAK || type == PacketType::SACK) ? "ACK" : pktType;
    string fileName = trace_path + "runlog/AllPacketTrace_" + fileType + "_node_" + to_string(src) + ".tr";
    PrintTraceInfoNoTs(fileName, info);
}

void UbUtils::LdstRecvNotify(uint32_t packetUid, uint32_t src, uint32_t dst, PacketType type,
                             uint32_t size, uint32_t taskId, UbPacketTraceTag traceTag)
{
    const char* pktType = "CONTROL";
    switch (type) {
    case PacketType::PACKET:
        pktType = "PKT";
        break;
    case PacketType::ACK:
        pktType = "ACK";
        break;
    case PacketType::NAK:
        pktType = "NAK";
        break;
    case PacketType::SACK:
        pktType = "SACK";
        break;
    case PacketType::CONTROL_FRAME:
        break;
    }

    std::ostringstream oss;
    oss << "Uid:" << packetUid << " Src:" << src << " Dst:" << dst
        << " Type:" << pktType << " Size:" << size << " TaskId:" << taskId << '\n';
    for (uint32_t i = 0; i < traceTag.GetTraceLenth(); i++) {
        uint32_t node = traceTag.GetNodeTrace(i);
        PortTrace trace = traceTag.GetPortTrace(node);
        if (i == 0) {
            oss << "[" << node << "][" << Among(std::to_string(trace.sendPort), std::to_string(trace.sendTime)) << "]"
                << "--->";
        } else if (i == traceTag.GetTraceLenth() - 1) {
            oss << "[" << Among(std::to_string(trace.recvPort), std::to_string(trace.recvTime)) << "][" << node << "]"
                << '\n';
        } else {
            oss << "[" << Among(std::to_string(trace.recvPort), std::to_string(trace.recvTime)) << "]"
                << "[" << node << "]"
                << "[" << Among(std::to_string(trace.sendPort), std::to_string(trace.sendTime)) << "]"
                << "--->";
        }
    }
    for (uint32_t i = 0; i < traceTag.GetTraceLenth(); i++) {
        uint32_t node = traceTag.GetNodeTrace(i);
        PortTrace trace = traceTag.GetPortTrace(node);
        if (i == 0) {
            oss << std::string(std::to_string(node).size() + 2, ' ') << "["
                << Among(std::to_string(trace.sendTime), std::to_string(trace.sendTime)) << "]" << std::string(4, ' ');
        } else if (i == traceTag.GetTraceLenth() - 1) {
            oss << "[" << Among(std::to_string(trace.recvTime), std::to_string(trace.recvTime)) << "]" << '\n';
        } else {
            oss << "[" << Among(std::to_string(trace.recvTime), std::to_string(trace.recvTime)) << "]"
                << std::string(std::to_string(node).size() + 2, ' ') << "["
                << Among(std::to_string(trace.sendTime), std::to_string(trace.sendTime)) << "]" << std::string(4, ' ');
        }
    }
    string info = oss.str();
    const char* fileType = (type == PacketType::NAK || type == PacketType::SACK) ? "ACK" : pktType;
    string fileName = trace_path + "runlog/AllPacketTrace_" + fileType + "_node_" + to_string(src) + ".tr";
    PrintTraceInfoNoTs(fileName, info);
}

inline void UbUtils::LdstFirstPacketSendsNotify(uint32_t nodeId, uint32_t taskId)
{
    string info = "First Packet Sends,taskId: " + std::to_string(taskId);
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::DagMemTaskStartsNotify(uint32_t nodeId, uint32_t taskId)
{
    string info = "MEM Task Starts, taskId: " + std::to_string(taskId);
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::DagMemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId)
{
    string info = "MEM Task Completes, taskId: " + std::to_string(taskId);
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::DagWqeTaskStartsNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId)
{
    std::ostringstream oss;
    oss << "WQE Starts, jettyNum: " << jettyNum << " taskId: " << taskId;
    string info = oss.str();
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::DagWqeTaskCompletesNotify(uint32_t nodeId, uint32_t jettyNum, uint32_t taskId)
{
    std::ostringstream oss;
    oss << "WQE Completes, jettyNum: " << jettyNum << " taskId: " << taskId;
    string info = oss.str();
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::PortTxNotify(uint32_t nodeId, uint32_t portId, uint32_t size)
{
    std::ostringstream oss;
    oss << "Port Tx, port ID: " << portId << " PacketSize: " << size;
    string info = oss.str();
    string fileName = trace_path + "runlog/PortTrace_node_" + to_string(nodeId) + "_port_" + to_string(portId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::PortRxNotify(uint32_t nodeId, uint32_t portId, uint32_t size)
{
    std::ostringstream oss;
    oss << "Port Rx, port ID: " << portId << " PacketSize: " << size;
    string info = oss.str();
    string fileName = trace_path + "runlog/PortTrace_node_" + to_string(nodeId) + "_port_" + to_string(portId) + ".tr";
    PrintTraceInfo(fileName, info);
}

void UbUtils::PfcStateNotify(uint32_t nodeId,
                             uint32_t portId,
                             const std::string& action,
                             uint32_t priority,
                             uint64_t ingressBytes)
{
    if (trace_path.empty() || !IsFlowControlTraceEnabled())
    {
        return;
    }
    std::ostringstream oss;
    oss << "PFC " << action
        << ", priority: " << priority
        << " ingressBytes: " << ingressBytes;
    string info = oss.str();
    string fileName = trace_path + "runlog/PfcTrace_node_" + to_string(nodeId) + "_port_" + to_string(portId) + ".tr";
    PrintTraceInfo(fileName, info);
}

void UbUtils::PfcDynamicStateNotify(uint32_t nodeId,
                                    uint32_t portId,
                                    uint32_t priority,
                                    uint64_t ingressTotalBytes,
                                    uint64_t sharedUsedBytes,
                                    uint64_t headroomUsedBytes,
                                    uint64_t xoffBytes,
                                    uint64_t xonBytes,
                                    bool pause)
{
    if (trace_path.empty() || !IsFlowControlTraceEnabled())
    {
        return;
    }
    std::ostringstream oss;
    oss << "PFC_DYNAMIC " << (pause ? "PAUSE" : "RESUME")
        << ", priority: " << priority
        << " ingressTotalBytes: " << ingressTotalBytes
        << " sharedUsedBytes: " << sharedUsedBytes
        << " headroomUsedBytes: " << headroomUsedBytes
        << " xoffBytes: " << xoffBytes
        << " xonBytes: " << xonBytes;
    string info = oss.str();
    string fileName =
        trace_path + "runlog/PfcDynamicTrace_node_" + to_string(nodeId) + "_port_" + to_string(portId) + ".tr";
    PrintTraceInfo(fileName, info);
}

void UbUtils::CbfcStateNotify(uint32_t nodeId,
                              uint32_t portId,
                              const std::string& action,
                              uint32_t priority,
                              int32_t availableCredits,
                              uint32_t nextPacketBytes)
{
    if (trace_path.empty() || !IsFlowControlTraceEnabled())
    {
        return;
    }
    std::ostringstream oss;
    oss << "CBFC " << action
        << ", priority: " << priority
        << " availableCredits: " << availableCredits
        << " nextPacketBytes: " << nextPacketBytes;
    string info = oss.str();
    string fileName = trace_path + "runlog/CbfcTrace_node_" + to_string(nodeId) + "_port_" + to_string(portId) + ".tr";
    PrintTraceInfo(fileName, info);
}

void UbUtils::CbfcCreditRestoreTraceNotify(uint32_t nodeId,
                                           uint32_t portId,
                                           const std::vector<uint8_t>& credits)
{
    if (trace_path.empty() || !IsFlowControlTraceEnabled())
    {
        return;
    }
    std::ostringstream oss;
    oss << "CBFC CREDIT_RESTORE";
    for (size_t i = 0; i < credits.size(); ++i)
    {
        oss << (i == 0 ? ", credits:" : " ") << static_cast<uint32_t>(credits[i]);
    }
    string info = oss.str();
    string fileName = trace_path + "runlog/CbfcTrace_node_" + to_string(nodeId) + "_port_" + to_string(portId) + ".tr";
    PrintTraceInfo(fileName, info);
}

void UbUtils::CbfcCreditLevelNotify(uint32_t nodeId,
                                    uint32_t portId,
                                    const std::string& reason,
                                    uint32_t priority,
                                    int32_t availableCredits,
                                    int32_t deltaCredits)
{
    if (trace_path.empty() || !IsFlowControlTraceEnabled())
    {
        return;
    }
    std::ostringstream oss;
    oss << "CBFC CREDIT_LEVEL"
        << ", reason: " << reason
        << " priority: " << priority
        << " availableCredits: " << availableCredits
        << " deltaCredits: " << deltaCredits;
    string info = oss.str();
    string fileName = trace_path + "runlog/CbfcTrace_node_" + to_string(nodeId) + "_port_" + to_string(portId) + ".tr";
    PrintTraceInfo(fileName, info);
}

void UbUtils::CbfcControlSendNotify(uint32_t nodeId,
                                    uint32_t portId,
                                    const std::string& reason,
                                    int32_t triggerThresholdCells,
                                    int32_t emitMinPendingCells,
                                    const std::vector<int32_t>& pendingCredits,
                                    const std::vector<uint8_t>& sendCredits)
{
    if (trace_path.empty() || !IsFlowControlTraceEnabled())
    {
        return;
    }
    std::ostringstream oss;
    oss << "CBFC CONTROL_SEND"
        << ", reason: " << reason
        << " triggerThresholdCells: " << triggerThresholdCells
        << " emitMinPendingCells: " << emitMinPendingCells
        << " pendingCredits:";
    for (size_t i = 0; i < pendingCredits.size(); ++i)
    {
        oss << (i == 0 ? "" : " ") << pendingCredits[i];
    }
    oss << " sendCredits:";
    for (size_t i = 0; i < sendCredits.size(); ++i)
    {
        oss << (i == 0 ? "" : " ") << static_cast<uint32_t>(sendCredits[i]);
    }
    string info = oss.str();
    string fileName = trace_path + "runlog/CbfcTrace_node_" + to_string(nodeId) + "_port_" + to_string(portId) + ".tr";
    PrintTraceInfo(fileName, info);
}

void UbUtils::DcqcnMarkNotify(uint32_t nodeId,
                              uint32_t outPort,
                              uint64_t totalQueueBytes,
                              double markProbability)
{
    if (trace_path.empty() || !IsCongestionControlTraceEnabled())
    {
        return;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6)
        << "DCQCN MARK, outPort: " << outPort
        << " totalQueueBytes: " << totalQueueBytes
        << " markProbability: " << markProbability;
    string info = oss.str();
    string fileName =
        trace_path + "runlog/DcqcnMarkTrace_node_" + to_string(nodeId) + "_port_" + to_string(outPort) + ".tr";
    PrintTraceInfo(fileName, info);
}

void UbUtils::DcqcnCnpNotify(uint32_t nodeId,
                             uint32_t tpn,
                             const std::string& action,
                             uint8_t ecn,
                             bool location)
{
    if (trace_path.empty() || !IsCongestionControlTraceEnabled())
    {
        return;
    }
    std::ostringstream oss;
    oss << "DCQCN CNP " << action
        << ", ecn: " << static_cast<uint32_t>(ecn)
        << " location: " << static_cast<uint32_t>(location);
    string info = oss.str();
    string fileName = trace_path + "runlog/DcqcnCnpTrace_node_" + to_string(nodeId) + "_tpn_" + to_string(tpn) + ".tr";
    PrintTraceInfo(fileName, info);
}

void UbUtils::DcqcnSenderStateNotify(uint32_t nodeId,
                                     uint32_t tpn,
                                     const std::string& reason,
                                     double alpha,
                                     uint64_t currentRateBps,
                                     uint64_t targetRateBps,
                                     uint64_t bytesSinceLastIncrease,
                                     uint32_t timeRecoveryEvents,
                                     uint32_t byteRecoveryEvents)
{
    if (trace_path.empty() || !IsCongestionControlTraceEnabled())
    {
        return;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6)
        << "DCQCN SENDER " << reason
        << ", alpha: " << alpha
        << " currentRateBps: " << currentRateBps
        << " targetRateBps: " << targetRateBps
        << " bytesSinceLastIncrease: " << bytesSinceLastIncrease
        << " timeRecoveryEvents: " << timeRecoveryEvents
        << " byteRecoveryEvents: " << byteRecoveryEvents;
    string info = oss.str();
    string fileName =
        trace_path + "runlog/DcqcnSenderTrace_node_" + to_string(nodeId) + "_tpn_" + to_string(tpn) + ".tr";
    PrintTraceInfo(fileName, info);
}

void
UbUtils::CaqmAckNotify(uint32_t nodeId,
                       uint32_t tpn,
                       uint32_t psnStart,
                       uint32_t psnEnd,
                       uint8_t cE,
                       uint8_t iE,
                       uint16_t hintE)
{
    if (trace_path.empty() || !IsCongestionControlTraceEnabled())
    {
        return;
    }
    if (cE == 0 && iE == 0 && hintE == 0)
    {
        return;
    }
    std::ostringstream oss;
    oss << "CAQM ACK"
        << ", psnStart: " << psnStart
        << " psnEnd: " << psnEnd
        << " cE: " << static_cast<uint32_t>(cE)
        << " iE: " << static_cast<uint32_t>(iE)
        << " hintE: " << hintE;
    string info = oss.str();
    string fileName = trace_path + "runlog/CaqmAckTrace_node_" + to_string(nodeId) + "_tpn_" + to_string(tpn) + ".tr";
    PrintTraceInfo(fileName, info);
}

void
UbUtils::CaqmSenderStateNotify(uint32_t nodeId,
                               uint32_t tpn,
                               uint32_t psn,
                               uint32_t sequence,
                               uint32_t inFlight,
                               uint32_t cwnd,
                               uint8_t cE,
                               bool iE,
                               uint16_t hint)
{
    if (trace_path.empty() || !IsCongestionControlTraceEnabled())
    {
        return;
    }
    if (cE == 0 && iE && hint == 0)
    {
        return;
    }
    std::ostringstream oss;
    oss << "CAQM SENDER"
        << ", psn: " << psn
        << " sequence: " << sequence
        << " inFlight: " << inFlight
        << " cwnd: " << cwnd
        << " cE: " << static_cast<uint32_t>(cE)
        << " iE: " << static_cast<uint32_t>(iE)
        << " hint: " << hint;
    string info = oss.str();
    string fileName =
        trace_path + "runlog/CaqmSenderTrace_node_" + to_string(nodeId) + "_tpn_" + to_string(tpn) + ".tr";
    PrintTraceInfo(fileName, info);
}

bool UbUtils::IsTpDebugEnabledFor(uint32_t nodeId, uint32_t tpn)
{
    BooleanValue enabledValue;
    if (!GlobalValue::GetValueByNameFailSafe("UB_TP_DEBUG_ENABLE", enabledValue) || !enabledValue.Get())
    {
        return false;
    }

    UintegerValue debugNodeIdValue;
    UintegerValue debugTpnValue;
    UintegerValue debugStartNsValue;
    UintegerValue debugEndNsValue;
    GlobalValue::GetValueByName("UB_TP_DEBUG_NODE_ID", debugNodeIdValue);
    GlobalValue::GetValueByName("UB_TP_DEBUG_TPN", debugTpnValue);
    GlobalValue::GetValueByName("UB_TP_DEBUG_START_NS", debugStartNsValue);
    GlobalValue::GetValueByName("UB_TP_DEBUG_END_NS", debugEndNsValue);

    if (nodeId != debugNodeIdValue.Get() || tpn != debugTpnValue.Get())
    {
        return false;
    }

    const uint64_t nowNs = static_cast<uint64_t>(Simulator::Now().GetNanoSeconds());
    if (nowNs < debugStartNsValue.Get())
    {
        return false;
    }
    const uint32_t endNs = debugEndNsValue.Get();
    if (endNs != 0 && nowNs > endNs)
    {
        return false;
    }
    return !trace_path.empty();
}

void UbUtils::TpDebugStateNotify(uint32_t nodeId,
                                 uint32_t tpn,
                                 const std::string& reason,
                                 uint64_t psnSndNxt,
                                 uint64_t psnSndUna,
                                 uint64_t inflightPackets,
                                 uint64_t maxInflightPackets,
                                 bool inflightLimited,
                                 bool ccLimited,
                                 bool sendWindowLimited,
                                 uint32_t activeSegments,
                                 uint32_t totalSegments,
                                 uint32_t ackQueueLen,
                                 uint32_t cnpQueueLen)
{
    if (!IsTpDebugEnabledFor(nodeId, tpn))
    {
        return;
    }

    std::ostringstream oss;
    oss << "TP DEBUG " << reason
        << " psnSndNxt: " << psnSndNxt
        << " psnSndUna: " << psnSndUna
        << " inflightPackets: " << inflightPackets
        << " maxInflightPackets: " << maxInflightPackets
        << " inflightLimited: " << static_cast<uint32_t>(inflightLimited)
        << " ccLimited: " << static_cast<uint32_t>(ccLimited)
        << " sendWindowLimited: " << static_cast<uint32_t>(sendWindowLimited)
        << " activeSegments: " << activeSegments
        << " totalSegments: " << totalSegments
        << " ackQueueLen: " << ackQueueLen
        << " cnpQueueLen: " << cnpQueueLen;
    const string info = oss.str();
    const string fileName =
        trace_path + "runlog/TpDebugTrace_node_" + to_string(nodeId) + "_tpn_" + to_string(tpn) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::QueueVoqNotify(uint32_t nodeId, uint32_t portId, uint64_t voqBytes)
{
    Ptr<Node> node = NodeList::GetNode(nodeId);
    Ptr<UbPort> port = node != nullptr ? DynamicCast<UbPort>(node->GetDevice(portId)) : nullptr;
    const uint64_t egressBytes = port != nullptr ? port->GetUbQueue()->GetCurrentBytes() : 0;
    const uint64_t totalBytes = voqBytes + egressBytes;

    std::ostringstream oss;
    oss << "Queue Update, source: VOQ"
        << " voqBytes: " << voqBytes
        << " egressBytes: " << egressBytes
        << " totalBytes: " << totalBytes;
    string info = oss.str();
    string fileName = trace_path + "runlog/QueueTrace_node_" + to_string(nodeId) + "_port_" + to_string(portId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void
UbUtils::QueueIngressOccupancyNotify(uint32_t nodeId,
                                     uint32_t inPort,
                                     uint32_t priority,
                                     uint64_t bytes)
{
    std::ostringstream oss;
    oss << "Queue Update, source: ingress"
        << " inPort: " << inPort
        << " priority: " << priority
        << " bytes: " << bytes;
    string info = oss.str();
    string fileName =
        trace_path + "runlog/QueueTrace_node_" + to_string(nodeId) + "_port_" + to_string(inPort) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::QueueEgressEnqueueNotify(uint32_t nodeId,
                                              uint32_t portId,
                                              Ptr<const Packet> packet,
                                              uint32_t egressBytes)
{
    (void)packet;
    Ptr<Node> node = NodeList::GetNode(nodeId);
    Ptr<UbSwitch> sw = node != nullptr ? node->GetObject<UbSwitch>() : nullptr;
    const uint64_t voqBytes = sw != nullptr ? sw->GetQueueManager()->GetTotalOutPortBufferUsed(portId) : 0;
    const uint64_t totalBytes = voqBytes + egressBytes;

    std::ostringstream oss;
    oss << "Queue Update, source: EGRESS_ENQUEUE"
        << " voqBytes: " << voqBytes
        << " egressBytes: " << egressBytes
        << " totalBytes: " << totalBytes;
    string info = oss.str();
    string fileName = trace_path + "runlog/QueueTrace_node_" + to_string(nodeId) + "_port_" + to_string(portId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::QueueEgressDequeueNotify(uint32_t nodeId,
                                              uint32_t portId,
                                              Ptr<const Packet> packet,
                                              uint32_t egressBytes)
{
    (void)packet;
    Ptr<Node> node = NodeList::GetNode(nodeId);
    Ptr<UbSwitch> sw = node != nullptr ? node->GetObject<UbSwitch>() : nullptr;
    const uint64_t voqBytes = sw != nullptr ? sw->GetQueueManager()->GetTotalOutPortBufferUsed(portId) : 0;
    const uint64_t totalBytes = voqBytes + egressBytes;

    std::ostringstream oss;
    oss << "Queue Update, source: EGRESS_DEQUEUE"
        << " voqBytes: " << voqBytes
        << " egressBytes: " << egressBytes
        << " totalBytes: " << totalBytes;
    string info = oss.str();
    string fileName = trace_path + "runlog/QueueTrace_node_" + to_string(nodeId) + "_port_" + to_string(portId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::LdstThreadMemTaskStartsNotify(uint32_t nodeId, uint32_t memTaskId)
{
    string info = "Mem Task Starts,taskId: " + std::to_string(memTaskId);
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::LdstMemTaskCompletesNotify(uint32_t nodeId, uint32_t taskId)
{
    string info = "Mem Task Completes,taskId: " + std::to_string(taskId);
    string fileName = trace_path + "runlog/TaskTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::LdstThreadFirstPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId)
{
    string info = "First Packet Sends, taskId: " + std::to_string(memTaskId);
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::LdstThreadLastPacketSendsNotify(uint32_t nodeId, uint32_t memTaskId)
{
    string info = "Last Packet Sends, taskId: " + std::to_string(memTaskId);
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::LdstLastPacketACKsNotify(uint32_t nodeId, uint32_t taskId)
{
    string info = "Last Packet ACKs,taskId: " + std::to_string(taskId);
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::LdstPeerSendFirstPacketACKsNotify(uint32_t nodeId, uint32_t taskId, uint32_t type)
{
    std::ostringstream oss;
    oss << "Peer Send First Packet ACKs, taskId: " << taskId << " type: " << type;
    string info = oss.str();
    string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
    PrintTraceInfo(fileName, info);
}

inline void UbUtils::SwitchLastPacketTraversesNotify(uint32_t nodeId, UbTransportHeader ubTpHeader)
{
    if (ubTpHeader.GetLastPacket()) {
        std::ostringstream oss;
        oss << "Last Packet Traverses ,NodeId: " << nodeId << " srcTpn: " << ubTpHeader.GetSrcTpn()
            << " destTpn: " << ubTpHeader.GetDestTpn() << " tpMsn: " << ubTpHeader.GetTpMsn()
            << " psn:" << ubTpHeader.GetPsn();
        string info = oss.str();
        string fileName = trace_path + "runlog/PacketTrace_node_" + to_string(nodeId) + ".tr";
        PrintTraceInfo(fileName, info);
    }
}

Time
UbUtils::ResolveLinkDelayWithOffset(Time baseDelay,
                                    Time offsetWindow,
                                    uint32_t offsetSeed,
                                    uint32_t node1,
                                    uint32_t port1,
                                    uint32_t node2,
                                    uint32_t port2)
{
    NS_ABORT_MSG_IF(baseDelay.IsStrictlyNegative(), "link delay must be non-negative");
    NS_ABORT_MSG_IF(offsetWindow.IsStrictlyNegative(),
                    "link delay offset window must be non-negative");
    if (baseDelay.IsZero() || offsetWindow.IsZero())
    {
        return baseDelay;
    }

    std::pair<uint32_t, uint32_t> endpoint1{node1, port1};
    std::pair<uint32_t, uint32_t> endpoint2{node2, port2};
    if (endpoint2 < endpoint1)
    {
        std::swap(endpoint1, endpoint2);
    }

    uint64_t hash =
        MixLinkDelayOffsetHash(static_cast<uint64_t>(offsetSeed) ^ 0x6c696e6b2d646c79ULL);
    hash = MixLinkDelayOffsetHash(hash ^ endpoint1.first);
    hash = MixLinkDelayOffsetHash(hash ^ endpoint1.second);
    hash = MixLinkDelayOffsetHash(hash ^ endpoint2.first);
    hash = MixLinkDelayOffsetHash(hash ^ endpoint2.second);

    const auto slotCount = static_cast<uint64_t>(offsetWindow.GetTimeStep());
    return baseDelay + TimeStep(static_cast<int64_t>(hash % slotCount));
}

// 读取拓扑文件
void
UbUtils::CreateTopo(const string& filename)
{
    (void)CreateTopo(filename, Time(0), 1);
}

UbUtils::LinkDelayOffsetStats
UbUtils::CreateTopo(const string& filename, Time offsetWindow, uint32_t offsetSeed)
{
    NS_ABORT_MSG_IF(offsetWindow.IsStrictlyNegative(),
                    "link delay offset window must be non-negative");
    LinkDelayOffsetStats offsetStats;
    std::unordered_map<int64_t, uint64_t> slotOccupancy;

    PrintTimestamp("[setup] Load " + DisplayFilename(filename));
    ifstream file(filename);
    if (!file.is_open())
        NS_ASSERT_MSG(0, "Can not open File: " << filename);
    string line;
    getline(file, line);
    // node1,port1,node2,port2,bandwidth,delay 0,0,2,0,400Gbps,10ns
    while (getline(file, line)) {
        vector<string> row;
        stringstream ss(line);
        // 跳过空行、#开头行、纯空格行
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }
        string cell;
        uint32_t node1;
        uint32_t port1;
        uint32_t node2;
        uint32_t port2;
        string delay;
        string bandwidth;
        getline(ss, cell, ',');
        node1 = static_cast<uint32_t>(stoi(TrimFieldCopy(cell)));
        getline(ss, cell, ',');
        port1 = static_cast<uint32_t>(stoi(TrimFieldCopy(cell)));
        getline(ss, cell, ',');
        node2 = static_cast<uint32_t>(stoi(TrimFieldCopy(cell)));
        getline(ss, cell, ',');
        port2 = static_cast<uint32_t>(stoi(TrimFieldCopy(cell)));
        getline(ss, cell, ',');
        bandwidth = TrimFieldCopy(cell);
        getline(ss, cell, ',');
        delay = TrimFieldCopy(cell);
        Ptr<Node> n1 = NodeList::GetNode(node1);
        Ptr<Node> n2 = NodeList::GetNode(node2);

        Ptr<UbPort> p1 = DynamicCast<UbPort>(n1->GetDevice(port1));
        Ptr<UbPort> p2 = DynamicCast<UbPort>(n2->GetDevice(port2));
        p1->SetDataRate(DataRate(bandwidth));
        p2->SetDataRate(DataRate(bandwidth));
        const Time baseDelay(delay);
        const Time effectiveDelay = ResolveLinkDelayWithOffset(baseDelay,
                                                               offsetWindow,
                                                               offsetSeed,
                                                               node1,
                                                               port1,
                                                               node2,
                                                               port2);
        if (baseDelay.IsZero())
        {
            ++offsetStats.zeroDelayLinkCount;
        }
        else
        {
            ++offsetStats.positiveLinkCount;
            if (offsetWindow.IsStrictlyPositive())
            {
                const int64_t slot = (effectiveDelay - baseDelay).GetTimeStep();
                offsetStats.maxLinksPerOffset =
                    std::max(offsetStats.maxLinksPerOffset, ++slotOccupancy[slot]);
            }
        }
        CreateUbChannelBetween(p1, p2, effectiveDelay);
    }

    for (auto it = NodeList::Begin(); it != NodeList::End(); ++it) {
        Ptr<Node> node = *it;
        Ptr<UbCongestionControl> congestionCtrl = node->GetObject<ns3::UbSwitch>()->GetCongestionCtrl();
        if (congestionCtrl->GetCongestionAlgo() == CAQM) {
            Ptr<UbSwitchCaqm> swCaqm = DynamicCast<UbSwitchCaqm>(congestionCtrl);
            swCaqm->ResetLocalCc();
        }
    }
    file.close();

    if (offsetWindow.IsStrictlyPositive())
    {
        offsetStats.distinctOffsetCount = slotOccupancy.size();
        offsetStats.offsetReuseCount =
            offsetStats.positiveLinkCount - offsetStats.distinctOffsetCount;
    }
    return offsetStats;
}

// 解析节点范围（如 "1..4"）
inline void UbUtils::ParseNodeRange(const string &rangeStr, NodeEle nodeEle)
{
    size_t dotPos = rangeStr.find("..");
    if (dotPos != string::npos) {
        // 处理范围格式
        uint32_t start = stoi(rangeStr.substr(0, dotPos));
        uint32_t end = stoi(rangeStr.substr(dotPos + 2));
        for (auto i = start; i <= end; ++i) {
            nodeEle_map[i]=nodeEle;
        }
    } else {
        // 处理单个节点
        nodeEle_map[stoi(rangeStr)] = nodeEle;
    }
}

// 创建node
void UbUtils::CreateNode(const string &filename)
{
    PrintTimestamp("[setup] Load " + DisplayFilename(filename));
    nodeEle_map.clear();
    ifstream file(filename);
    if (!file.is_open()) {
        NS_ASSERT_MSG(0, "Can not open File: " << filename);
    }
    string line;
    NS_ABORT_MSG_IF(!getline(file, line), "node.csv must include a header");
    const std::vector<std::string> header = SplitCsvRow(line);
    const std::map<std::string, std::size_t> headerIndex = BuildCsvHeaderIndex(header);
    const bool legacyForwardDelayIsAllocationDelay = IsLegacyNodeForwardDelayHeader(header);
    while (getline(file, line)) {
        // 跳过空行、#开头行、纯空格行
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }
        const std::vector<std::string> fields = SplitCsvRow(line);

        NodeEle nodeEle = {};
        nodeEle.nodeIdStr = GetRequiredCsvField(fields, headerIndex, "nodeId", filename);
        nodeEle.nodeTypeStr = GetRequiredCsvField(fields, headerIndex, "nodeType", filename);
        nodeEle.portNumStr = GetRequiredCsvField(fields, headerIndex, "portNum", filename);
        if (legacyForwardDelayIsAllocationDelay)
        {
            nodeEle.forwardDelay = "";
            nodeEle.allocationDelay = GetOptionalCsvField(fields, headerIndex, "forwardDelay");
        }
        else
        {
            nodeEle.forwardDelay = GetOptionalCsvField(fields, headerIndex, "forwardDelay");
            nodeEle.allocationDelay = GetOptionalCsvField(fields, headerIndex, "allocationDelay");
        }
        nodeEle.systemIdStr = GetOptionalCsvField(fields, headerIndex, "systemId");

        // 解析节点ID（范围 or 单个节点）
        ParseNodeRange(nodeEle.nodeIdStr, nodeEle);
    }
    file.close();
    // 创建节点
    for (auto it: nodeEle_map) {
        string nodeIdStr = it.second.nodeIdStr;
        string nodeTypeStr = it.second.nodeTypeStr;
        string portNumStr = it.second.portNumStr;
        string forwardDelay = it.second.forwardDelay;
        string allocationDelay = it.second.allocationDelay;
        string systemIdStr = it.second.systemIdStr;
        int portNum = stoi(portNumStr);
        uint32_t systemId = systemIdStr.empty() ? 0 : static_cast<uint32_t>(stoul(systemIdStr));
        Ptr<Node> node = CreateObject<Node>(systemId);
        Ptr<UbSwitch> sw = CreateObject<UbSwitch>();
        node->AggregateObject(sw);
        Ptr<ns3::UbLdstInstance> ldst = CreateObject<UbLdstInstance>();
        node->AggregateObject(ldst);
        ldst->Init(node->GetId());
        if (nodeTypeStr == "DEVICE") {
            Ptr<UbController> ubCtrl = CreateObject<UbController>();
            node->AggregateObject(ubCtrl);
            ubCtrl->CreateUbFunction();
            ubCtrl->CreateUbTransaction();
            sw->SetNodeType(UB_DEVICE);
        } else if (nodeTypeStr == "SWITCH") {
            sw->SetNodeType(UB_SWITCH);
        } else {
            NS_ASSERT_MSG(0, "node type not support");
        }
        for (int i = 0; i < portNum; i++) {
            Ptr<UbPort> port = CreateObject<UbPort>();
            port->SetAddress(Mac48Address::Allocate());
            node->AddDevice(port);
        }
        sw->Init();
        auto cc = UbCongestionControl::Create(UB_SWITCH);
        cc->OnSwitchAttached(sw);
        if (!forwardDelay.empty()) {
            sw->SetAttribute("InPortProcessingDelay", StringValue(forwardDelay));
        }
        if (!allocationDelay.empty()) {
            auto allocator = sw->GetAllocator();
            allocator->SetAttribute("AllocationTime", StringValue(allocationDelay));
        }
    }
}

// 读取路由
void UbUtils::AddRoutingTable(const string &filename)
{
    PrintTimestamp("[setup] Load " + DisplayFilename(filename));
    // node_id,dest,outport,metric
    std::ifstream file(filename);
    if (!file.is_open()) {
        NS_ASSERT_MSG(0, "Can not open File: " << filename);
    }

    uint32_t nodeStart;
    uint32_t nodeEnd;
    uint32_t destStart;
    uint32_t destEnd;
    uint32_t destport;
    uint32_t outport;
    uint32_t metric;
    uint32_t rowNumber = 1;
    std::string line;
    std::vector<uint16_t> outports;
    std::vector<uint32_t> metrics;
    std::vector<RouteRangeCsvRow> rangeRows;

    std::unordered_map<uint32_t, std::unordered_map<uint32_t, std::map<uint32_t, std::vector<uint16_t>>>> rtTable;
    getline(file, line);
    while (std::getline(file, line)) {
        ++rowNumber;
        std::stringstream ss(line);

        // 跳过空行、#开头行、纯空格行
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }
        std::string cell;
        std::getline(ss, cell, ',');
        std::tie(nodeStart, nodeEnd) = ParseUint32RangeField(cell);
        std::getline(ss, cell, ',');
        std::tie(destStart, destEnd) = ParseUint32RangeField(cell);
        std::getline(ss, cell, ',');
        destport = static_cast<uint32_t>(std::stoi(cell));
        std::getline(ss, cell, ',');
        // read outports
        std::stringstream sOutports(cell);
        outports.clear();
        while (sOutports >> outport) {
            outports.push_back(outport);
        }
        std::getline(ss, cell, ',');
        std::stringstream sMetrics(cell);
        metrics.clear();
        while (sMetrics >> metric) {
            metrics.push_back(metric);
        }
        const auto routeValue = NormalizeRouteValue(outports, metrics, rowNumber);
        const bool isRangeRoute = nodeStart != nodeEnd || destStart != destEnd;
        if (!isRangeRoute) {
            Ipv4Address ip_node = NodeIdToIp(destStart);
            Ipv4Address ip_nodePort = NodeIdToIp(destStart, destport);
            for (auto i = 0u; i < outports.size(); i++) {
                rtTable[nodeStart][ip_node.Get()][metrics[i]].push_back(outports[i]);
                rtTable[nodeStart][ip_nodePort.Get()][metrics[i]].push_back(outports[i]);
            }
            continue;
        }

        rangeRows.push_back(RouteRangeCsvRow{rowNumber,
                                             nodeStart,
                                             nodeEnd,
                                             destStart,
                                             destEnd,
                                             destport,
                                             routeValue});
    }
    ValidateRouteRangeRowsNoConflicts(rangeRows);
    for (const auto& row : rangeRows) {
        const uint32_t nodeStart = row.nodeStart;
        const uint32_t nodeEnd = row.nodeEnd;
        const uint32_t destStart = row.destStart;
        const uint32_t destEnd = row.destEnd;
        const uint32_t destport = row.destPort;
        for (uint32_t nodeId = nodeStart;; ++nodeId) {
            auto rt = NodeList::GetNode(nodeId)->GetObject<ns3::UbSwitch>()->GetRoutingProcess();
            const auto minMetricIt =
                std::min_element(row.value.begin(),
                                 row.value.end(),
                                 [](const auto& left, const auto& right) {
                                     return left.second < right.second;
                                 });
            NS_ASSERT_MSG(minMetricIt != row.value.end(), "compressed route row must contain metrics");
            const uint32_t shortestMetric = minMetricIt->second;
            for (const auto& [outport, metric] : row.value) {
                std::vector<uint16_t> outPortGroup = {outport};
                if (metric == shortestMetric) {
                    rt->AddShortestRouteRange(destStart, destEnd, destport, outPortGroup);
                } else {
                    rt->AddOtherRouteRange(destStart, destEnd, destport, outPortGroup);
                }
            }
            if (nodeId == nodeEnd) {
                break;
            }
        }
    }
    for (auto &nodert : rtTable) {
        auto rt = NodeList::GetNode(nodert.first)->GetObject<ns3::UbSwitch>()->GetRoutingProcess();
        for (auto &destiprow : nodert.second) {
            auto ip = destiprow.first;
            int i = 0;
            for (auto &metricrow : destiprow.second) {
                if (i == 0) {
                    rt->AddShortestRoute(ip, metricrow.second);
                } else {
                    rt->AddOtherRoute(ip, metricrow.second);
                }
                i++;
            }
        }
    }
    file.close();
}

// 读取TP配置文件
void UbUtils::ParseLine(const std::string &line, Connection &conn)
{
    std::stringstream ss(line);
    std::string item;

    // 读取node1
    getline(ss, item, ',');
    conn.node1 = stoi(item);

    // 读取port1
    getline(ss, item, ',');
    conn.port1 = stoi(item);

    // 读取tpn1
    getline(ss, item, ',');
    conn.tpn1 = stoi(item);

    // 读取node2
    getline(ss, item, ',');
    conn.node2 = stoi(item);

    // 读取port2
    getline(ss, item, ',');
    conn.port2 = stoi(item);

    // 读取tpn2
    getline(ss, item, ',');
    conn.tpn2 = stoi(item);

    // 读取priority
    getline(ss, item, ',');
    conn.priority = stoi(item);

    // 读取metrics
    getline(ss, item, ',');
    if (!item.empty()) {
        conn.metrics = stoi(item);
    } else {
        conn.metrics = UINT32_MAX;
    }
}

void UbUtils::CreateTp(const string &filename)
{
    std::unordered_map<uint32_t, std::vector<uint32_t>> NodeTpns;
    // key1:node1 key2:node2 value:Connection
    ifstream file(filename);
    if (!file.is_open()) { // 没有TP文件则使用实时创建TP模式
        PrintTimestamp("[setup] Skip " + DisplayFilename(filename) +
                       " (not found; traffic will reserve TP connections before endpoints "
                       "materialize on demand).");
        return ;
    }
    PrintTimestamp("[setup] Load " + DisplayFilename(filename));
    string line;
    // 跳过标题行
    getline(file, line);

    while (getline(file, line)) {
        // 跳过空行、#开头行、纯空格行
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }
        Connection conn;
        ParseLine(line, conn);
        Ptr<Node> sendNode = NodeList::GetNode(conn.node1);
        Ptr<Node> recvNode = NodeList::GetNode(conn.node2);
        auto sendCtrl = sendNode->GetObject<UbController>();
        auto recvCtrl = recvNode->GetObject<UbController>();
        sendCtrl->GetTpConnManager()->AddUnilateralConnection(conn, conn.node1);
        recvCtrl->GetTpConnManager()->AddUnilateralConnection(conn, conn.node2);

        PreloadLocalTpIfOwned(sendNode,
                              conn.node1,
                              conn.node2,
                              conn.port1,
                              conn.port2,
                              conn.priority,
                              conn.tpn1,
                              conn.tpn2);
        PreloadLocalTpIfOwned(recvNode,
                              conn.node2,
                              conn.node1,
                              conn.port2,
                              conn.port1,
                              conn.priority,
                              conn.tpn2,
                              conn.tpn1);
    }
    file.close();
    return ;
}

void UbUtils::SetRecord(int fieldCount, string field, TrafficRecord &record)
{
    switch (static_cast<FIELDCOUNT>(fieldCount)) {
        case FIELDCOUNT::TASKID:
            record.taskId = stoi(field);
            break;
        case FIELDCOUNT::SOURCENODE:
            record.sourceNode = stoi(field);
            break;
        case FIELDCOUNT::DESTNODE:
            record.destNode = stoi(field);
            break;
        case FIELDCOUNT::DATASIZE:
            record.dataSize = stoi(field);
            break;
        case FIELDCOUNT::OPTYPE:
            record.opType = field;
            break;
        case FIELDCOUNT::PRIORITY:
            record.priority = static_cast<int>(ParseTrafficPriorityField(field));
            break;
        case FIELDCOUNT::DELAY:
            record.delay = field;
            break;
        case FIELDCOUNT::PHASEID:
            record.phaseId = stoi(field);
            break;
        case FIELDCOUNT::DEPENDONPHASES: {
            if (!field.empty()) {
                stringstream depStream(field);
                string dep;
                while (depStream >> dep) {
                    record.dependOnPhases.push_back(stoi(dep));
                }
            }
            break;
        }
    }
}

vector<TrafficRecord> UbUtils::LoadTrafficConfig(const string &filename)
{
    vector<TrafficRecord> records;
    ForEachTrafficRecordInternal(filename, "Load", [&](const TrafficRecord& record) {
        UbTrafficGen::Get()->SetPhaseDepend(record.phaseId, record.taskId);
        records.push_back(record);
    });
    return records;
}

void
UbUtils::ForEachTrafficRecord(const string& filename,
                              const std::function<void(const TrafficRecord&)>& callback)
{
    ForEachTrafficRecordInternal(filename, "Load", callback);
}

void
UbUtils::ForEachTrafficRecordView(const string& filename,
                                  const std::function<void(const TrafficRecordView&)>& callback)
{
    ForEachTrafficRecordViewInternal(filename, "Load", callback);
}

void
UbUtils::RegisterTrafficPhaseDependencies(const string& filename)
{
    (void)RegisterTrafficPhaseDependenciesAndGetStats(filename);
}

UbUtils::TrafficLoadStats
UbUtils::RegisterTrafficPhaseDependenciesAndGetStats(const string& filename)
{
    TrafficLoadStats stats;
    ForEachTrafficPhaseIndexRecord(filename, [&](uint32_t taskId, uint32_t phaseId) {
        UbTrafficGen::Get()->RegisterPhaseTaskDuringInitialLoad(phaseId);
        ++stats.recordCount;
        stats.maxTaskId = std::max<uint32_t>(stats.maxTaskId, taskId);
    });
    return stats;
}

void
UbUtils::ForEachTrafficPhaseIndexRecord(const string& filename,
                                        const std::function<void(uint32_t, uint32_t)>& callback)
{
    PrintTimestamp("[traffic] Index phase dependencies from " + DisplayFilename(filename));
    ifstream file(filename);
    if (!file.is_open()) {
        NS_ASSERT_MSG(0, "Can not open File: " << filename);
        return;
    }

    string line;
    getline(file, line);
    while (getline(file, line)) {
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }

        uint32_t taskId = 0;
        uint32_t phaseId = 0;
        int fieldCount = 0;
        size_t fieldStart = 0;
        bool hasTaskId = false;
        bool hasPhaseId = false;

        while (fieldStart <= line.size()) {
            size_t fieldEnd = line.find(',', fieldStart);
            if (fieldEnd == string::npos) {
                fieldEnd = line.size();
            }

            if (fieldCount == static_cast<int>(FIELDCOUNT::TASKID) ||
                fieldCount == static_cast<int>(FIELDCOUNT::PHASEID)) {
                size_t valueStart = fieldStart;
                while (valueStart < fieldEnd &&
                       (line[valueStart] == ' ' || line[valueStart] == '\t')) {
                    ++valueStart;
                }
                size_t valueEnd = fieldEnd;
                while (valueEnd > valueStart &&
                       (line[valueEnd - 1] == ' ' || line[valueEnd - 1] == '\t')) {
                    --valueEnd;
                }
                if (valueStart < valueEnd) {
                    const uint32_t value = ParseUint32Field(line, valueStart, valueEnd);
                    if (fieldCount == static_cast<int>(FIELDCOUNT::TASKID)) {
                        taskId = value;
                        hasTaskId = true;
                    } else {
                        phaseId = value;
                        hasPhaseId = true;
                    }
                }
            }

            if (hasTaskId && hasPhaseId) {
                break;
            }
            if (fieldEnd == line.size()) {
                break;
            }
            fieldStart = fieldEnd + 1;
            ++fieldCount;
        }

        if (hasTaskId && hasPhaseId) {
            callback(taskId, phaseId);
        }
    }
}

void
UbUtils::ForEachTrafficRecordInternal(const string& filename,
                                      const string& action,
                                      const std::function<void(const TrafficRecord&)>& callback)
{
    PrintTimestamp("[traffic] " + action + " " + DisplayFilename(filename));
    ifstream file(filename);
    if (!file.is_open()) {
        NS_ASSERT_MSG(0, "Can not open File: " << filename);
        return;
    }
    string line;
    getline(file, line);  // 跳过标题行
    TrafficRecord record;
    while (getline(file, line)) {
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }

        record.taskId = 0;
        record.sourceNode = 0;
        record.destNode = 0;
        record.dataSize = 0;
        record.opType.clear();
        record.priority = 0;
        record.delay.clear();
        record.phaseId = 0;
        record.dependOnPhases.clear();
        record.srcEntityId = 0;
        record.dstEntityId = 0;
        record.hasSrcEntityId = false;
        record.hasDstEntityId = false;

        int fieldCount = 0;
        size_t fieldStart = 0;
        while (fieldStart <= line.size()) {
            size_t fieldEnd = line.find(',', fieldStart);
            if (fieldEnd == string::npos) {
                fieldEnd = line.size();
            }

            SetTrafficRecordField(fieldCount,
                                  std::string_view(line).substr(fieldStart,
                                                                fieldEnd - fieldStart),
                                  record);
            ++fieldCount;

            if (fieldEnd == line.size()) {
                break;
            }
            fieldStart = fieldEnd + 1;
        }
        NS_ABORT_MSG_IF(
            fieldCount != 9 && fieldCount != 11,
            "traffic.csv row must have 9 base fields or 11 fields with srcEntityId,dstEntityId");
        callback(record);
    }
    file.close();
}

void
UbUtils::ForEachTrafficRecordViewInternal(
    const string& filename,
    const string& action,
    const std::function<void(const TrafficRecordView&)>& callback)
{
    PrintTimestamp("[traffic] " + action + " " + DisplayFilename(filename));
    ifstream file(filename);
    if (!file.is_open()) {
        NS_ASSERT_MSG(0, "Can not open File: " << filename);
        return;
    }
    string line;
    getline(file, line);  // 跳过标题行
    while (getline(file, line)) {
        if (line.empty() || line[0] == '#' || line.find_first_not_of(" \t") == string::npos) {
            continue;
        }

        TrafficRecordView record;
        int fieldCount = 0;
        size_t fieldStart = 0;
        while (fieldStart <= line.size()) {
            size_t fieldEnd = line.find(',', fieldStart);
            if (fieldEnd == string::npos) {
                fieldEnd = line.size();
            }

            SetTrafficRecordViewField(fieldCount,
                                      std::string_view(line).substr(fieldStart,
                                                                    fieldEnd - fieldStart),
                                      record);
            ++fieldCount;

            if (fieldEnd == line.size()) {
                break;
            }
            fieldStart = fieldEnd + 1;
        }
        NS_ABORT_MSG_IF(
            fieldCount != 9 && fieldCount != 11,
            "traffic.csv row must have 9 base fields or 11 fields with srcEntityId,dstEntityId");
        callback(record);
    }
    file.close();
}

// 从TXT文件加载配置
void UbUtils::SetComponentsAttribute(const string &filename)
{
    PrintTimestamp("[setup] Load " + DisplayFilename(filename));
    g_config_path = std::string(filename);
    std::ifstream file(filename.c_str());
    if (!file.good()) {
        NS_ASSERT_MSG(0, "Can not open File: " << filename);
    }
    ValidateLegacyNetworkAttributeKeys(filename);
    uint32_t rewrittenAliasLines = 0;
    const std::string configStoreFilename = CreateConfigStoreInput(filename, rewrittenAliasLines);
    if (rewrittenAliasLines > 0)
    {
        PrintTimestamp("[setup] Rewrite legacy network attribute aliases: " +
                       std::to_string(rewrittenAliasLines));
    }
    Config::SetDefault("ns3::ConfigStore::Filename", StringValue(configStoreFilename));
    Config::SetDefault("ns3::ConfigStore::FileFormat", StringValue("RawText"));
    Config::SetDefault("ns3::ConfigStore::Mode", StringValue("Load"));
    ConfigStore config;
    config.ConfigureDefaults();
    if (configStoreFilename != filename)
    {
        Config::SetDefault("ns3::ConfigStore::Filename", StringValue(filename));
        std::error_code ec;
        std::filesystem::remove(configStoreFilename, ec);
    }
}

void UbUtils::TopoTraceConnect()
{
    BooleanValue val;
    g_trace_enable.GetValue(val);
    TraceEnable = val.Get();

    if (!TraceEnable) {
        return; // 若不开启总开关则直接返回
    }

    g_task_trace_enable.GetValue(val);
    TaskTraceEnable = val.Get();

    g_packet_trace_enable.GetValue(val);
    PacketTraceEnable = val.Get();

    g_port_trace_enable.GetValue(val);
    PortTraceEnable = val.Get();

    g_record_pkt_trace_enable.GetValue(val);
    RecordTraceEnabled = val.Get();

    bool queueTraceEnabled = false;
    if (GlobalValue::GetValueByNameFailSafe("UB_QUEUE_TRACE_ENABLE", val))
    {
        queueTraceEnabled = val.Get();
    }

    bool flowControlTraceEnabled = false;
    if (GlobalValue::GetValueByNameFailSafe("UB_FLOW_CONTROL_TRACE_ENABLE", val))
    {
        flowControlTraceEnabled = val.Get();
    }

    bool congestionControlTraceEnabled = false;
    if (GlobalValue::GetValueByNameFailSafe("UB_CONGESTION_CONTROL_TRACE_ENABLE", val))
    {
        congestionControlTraceEnabled = val.Get();
    }

    NS_LOG_UNCOND("--- UnifiedBus Trace System Configuration ---");
    NS_LOG_UNCOND("UB_TRACE_ENABLE: " << (TraceEnable ? "ON" : "OFF"));
    if (TraceEnable) {
        NS_LOG_UNCOND("  UB_TASK_TRACE_ENABLE:   " << (TaskTraceEnable ? "ON" : "OFF") << "  (Task level events)");
        NS_LOG_UNCOND("  UB_PACKET_TRACE_ENABLE: " << (PacketTraceEnable ? "ON" : "OFF") << "  (Packet Send/ACK timestamps, essential for detailed task latency breakdown)");
        NS_LOG_UNCOND("  UB_PORT_TRACE_ENABLE:   " << (PortTraceEnable ? "ON" : "OFF") << "  (All port traffic, high volume, for throughput)");
        NS_LOG_UNCOND("  UB_RECORD_PKT_TRACE:    " << (RecordTraceEnabled ? "ON" : "OFF") << "  (Per-hop packet path tracking)");
        NS_LOG_UNCOND("  UB_QUEUE_TRACE_ENABLE:  "
                      << (queueTraceEnabled ? "ON" : "OFF")
                      << "  (QueueTrace_* files from queue events and periodic sampling)");
        NS_LOG_UNCOND("  UB_FLOW_CONTROL_TRACE_ENABLE: "
                      << (flowControlTraceEnabled ? "ON" : "OFF")
                      << "  (Algorithm-emitted PFC/CBFC trace files)");
        NS_LOG_UNCOND("  UB_CONGESTION_CONTROL_TRACE_ENABLE: "
                      << (congestionControlTraceEnabled ? "ON" : "OFF")
                      << "  (Algorithm-emitted DCQCN/CAQM trace files)");
    }
    NS_LOG_UNCOND("-------------------------------------------");

    for (uint32_t i = 0; i < NodeList::GetNNodes(); ++i) {
        Ptr<Node> node = NodeList::GetNode(i);
        Ptr<UbController> ubCtrl = node->GetObject<ns3::UbController>();
        Ptr<UbSwitch> sw = node->GetObject<ns3::UbSwitch>();
        
        if (PacketTraceEnable) {
            sw->TraceConnectWithoutContext("LastPacketTraversesNotify", MakeCallback(SwitchLastPacketTraversesNotify));
        }

        std::map<uint32_t, Ptr<UbTransportChannel>> tpnMap;
        if (ubCtrl) {
            tpnMap = ubCtrl->GetTpnMap();
            for (const auto &pair : tpnMap) { // 设置 TP的trace callback
                auto tp = pair.second;
                if (tp) {
                    if (PacketTraceEnable) {
                        tp->TraceConnectWithoutContext("FirstPacketSendsNotify", MakeCallback(TpFirstPacketSendsNotify));
                        tp->TraceConnectWithoutContext("LastPacketSendsNotify", MakeCallback(TpLastPacketSendsNotify));
                        tp->TraceConnectWithoutContext("LastPacketACKsNotify", MakeCallback(TpLastPacketACKsNotify));
                        tp->TraceConnectWithoutContext("LastPacketReceivesNotify", MakeCallback(TpLastPacketReceivesNotify));
                    }
                    if (TaskTraceEnable) {
                        tp->TraceConnectWithoutContext("WqeSegmentSendsNotify", MakeCallback(TpWqeSegmentSendsNotify));
                        tp->TraceConnectWithoutContext("WqeSegmentCompletesNotify", MakeCallback(TpWqeSegmentCompletesNotify));
                    }
                    if (RecordTraceEnabled) {
                        tp->TraceConnectWithoutContext("TpRecvNotify", MakeCallback(TpRecvNotify));
                    }
                } else {
                    NS_ASSERT_MSG(0, "TP is null");
                }
            }
            Ptr<UbLdstInstance> ldstInstance = node->GetObject<UbLdstInstance>();
            if (ldstInstance != nullptr && TaskTraceEnable) {
                ldstInstance->TraceConnectWithoutContext("MemTaskCompletesNotify", MakeCallback(LdstMemTaskCompletesNotify));
                ldstInstance->TraceConnectWithoutContext("LastPacketACKsNotify", MakeCallback(LdstLastPacketACKsNotify));
                ldstInstance->TraceConnectWithoutContext("MemTaskStartsNotify", MakeCallback(LdstThreadMemTaskStartsNotify));
                ldstInstance->TraceConnectWithoutContext("FirstPacketSendsNotify", MakeCallback(LdstThreadFirstPacketSendsNotify));
                ldstInstance->TraceConnectWithoutContext("LastPacketSendsNotify", MakeCallback(LdstThreadLastPacketSendsNotify));
            }
            if (RecordTraceEnabled) {
                auto ldstApi = ubCtrl->GetUbFunction()->GetUbLdstApi();
                ldstApi->TraceConnectWithoutContext("LdstRecvNotify", MakeCallback(LdstRecvNotify));
            }
            if (PacketTraceEnable) {
                ubCtrl->SetCtpPacketTimingTraceCallbacks(MakeCallback(CtpFirstPacketSendsNotify),
                                                         MakeCallback(CtpLastPacketACKsNotify));
            }
        }

        if (queueTraceEnabled) {
            uint32_t DevicesNum = node->GetNDevices();
            for (uint32_t i = 0; i < DevicesNum; i++) {
                Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(i));
                if (port) {
                    port->GetUbQueue()->TraceConnectWithoutContext(
                        "UbEnqueue",
                        MakeBoundCallback(&UbUtils::QueueEgressEnqueueNotify, node->GetId(), i));
                    port->GetUbQueue()->TraceConnectWithoutContext(
                        "UbDequeue",
                        MakeBoundCallback(&UbUtils::QueueEgressDequeueNotify, node->GetId(), i));
                } else {
                    NS_ASSERT_MSG(0, "port is null");
                }
            }
        }

        if (PortTraceEnable) {
            uint32_t DevicesNum = node->GetNDevices();
            for (uint32_t i = 0; i < DevicesNum; i++) { // 设置 port的trace callback
                Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(i));
                if (port) {
                    port->TraceConnectWithoutContext("PortTxNotify", MakeCallback(PortTxNotify));
                    port->TraceConnectWithoutContext("PortRxNotify", MakeCallback(PortRxNotify));
                } else {
                    NS_ASSERT_MSG(0, "port is null");
                }
            }
        }

        if (sw != nullptr && queueTraceEnabled) {
            sw->GetQueueManager()->TraceConnectWithoutContext(
                "OutPortBufferBytes",
                MakeBoundCallback(&UbUtils::QueueVoqNotify, node->GetId()));
            sw->GetQueueManager()->TraceConnectWithoutContext(
                "IngressQueueOccupancyBytes",
                MakeBoundCallback(&UbUtils::QueueIngressOccupancyNotify, node->GetId()));
        }
    }

    StartQueueSampler();
}

void UbUtils::SingleTpTraceConnect(uint32_t nodeId, uint32_t tpn)
{
    BooleanValue val;
    g_trace_enable.GetValue(val);
    TraceEnable = val.Get();

    if (!TraceEnable) {
        return; // 若不开启总开关则直接返回
    }

    g_task_trace_enable.GetValue(val);
    TaskTraceEnable = val.Get();

    g_packet_trace_enable.GetValue(val);
    PacketTraceEnable = val.Get();

    g_record_pkt_trace_enable.GetValue(val);
    RecordTraceEnabled = val.Get();

    Ptr<Node> node = NodeList::GetNode(nodeId);
    Ptr<UbController> ubCtrl = node->GetObject<ns3::UbController>();
    Ptr<UbTransportChannel> tp = ubCtrl->GetTpByTpn(tpn);
    if (tp) {
        if (PacketTraceEnable) {
            tp->TraceConnectWithoutContext("FirstPacketSendsNotify", MakeCallback(TpFirstPacketSendsNotify));
            tp->TraceConnectWithoutContext("LastPacketSendsNotify", MakeCallback(TpLastPacketSendsNotify));
            tp->TraceConnectWithoutContext("LastPacketACKsNotify", MakeCallback(TpLastPacketACKsNotify));
            tp->TraceConnectWithoutContext("LastPacketReceivesNotify", MakeCallback(TpLastPacketReceivesNotify));
        }
        if (TaskTraceEnable) {
            tp->TraceConnectWithoutContext("WqeSegmentSendsNotify", MakeCallback(TpWqeSegmentSendsNotify));
            tp->TraceConnectWithoutContext("WqeSegmentCompletesNotify", MakeCallback(TpWqeSegmentCompletesNotify));
        }
        if (RecordTraceEnabled) {
            tp->TraceConnectWithoutContext("TpRecvNotify", MakeCallback(TpRecvNotify));
        }
    }
}

void UbUtils::ClientTraceConnect(int srcNode)
{
    if (!TraceEnable || !TaskTraceEnable) {
        return; // 若不开启trace或不开启task trace则直接返回
    }
    Ptr<ns3::UbApp> client = DynamicCast<ns3::UbApp>(NodeList::GetNode(srcNode)->GetApplication(0));
    client->TraceConnectWithoutContext("MemTaskStartsNotify", MakeCallback(DagMemTaskStartsNotify));
    client->TraceConnectWithoutContext("MemTaskCompletesNotify", MakeCallback(DagMemTaskCompletesNotify));
    client->TraceConnectWithoutContext("WqeTaskStartsNotify", MakeCallback(DagWqeTaskStartsNotify));
    client->TraceConnectWithoutContext("WqeTaskCompletesNotify", MakeCallback(DagWqeTaskCompletesNotify));
}

bool UbUtils::QueryAttributeInfo(int argc, char *argv[])
{
    std::string className;
    std::string attrName;
    std::string globalName;
    std::string ignoredCasePath;
    bool printUbGlobals = false;

    CommandLine cmd;
    cmd.AddValue("ClassName", "Target class name", className);
    cmd.AddValue("AttributeName", "Target attribute name (optional)", attrName);
    cmd.AddValue("GlobalName", "Target Unified Bus global value name (optional)", globalName);
    cmd.AddValue("PrintUbGlobals", "Print Unified Bus global values with type metadata", printUbGlobals);
    cmd.AddValue("case-path", "Ignored case path for metadata-only queries", ignoredCasePath);
    cmd.AddNonOption("casePath", "Ignored case path for metadata-only queries", ignoredCasePath);
    cmd.Parse(argc, argv);

    auto isUbGlobal = [](const std::string& name) {
        return name.rfind("UB_", 0) == 0;
    };
    auto renderGlobalInfo = [](const GlobalValue& globalValue) {
        StringValue value;
        globalValue.GetValue(value);
        std::cout << "Global: " << globalValue.GetName() << '\n'
                  << "Description: " << globalValue.GetHelp() << '\n'
                  << "DataType: " << globalValue.GetChecker()->GetValueTypeName() << '\n'
                  << "Default: " << value.Get() << std::endl;
    };

    if (!globalName.empty()) {
        for (auto gvit = GlobalValue::Begin(); gvit != GlobalValue::End(); ++gvit) {
            if ((*gvit)->GetName() == globalName && isUbGlobal(globalName)) {
                renderGlobalInfo(*(*gvit));
                return true;
            }
        }
        std::cout << "Global not found!" << std::endl;
        return true;
    }

    if (printUbGlobals) {
        std::vector<GlobalValue*> ubGlobals;
        for (auto gvit = GlobalValue::Begin(); gvit != GlobalValue::End(); ++gvit) {
            if (isUbGlobal((*gvit)->GetName())) {
                ubGlobals.push_back(*gvit);
            }
        }
        std::sort(
            ubGlobals.begin(),
            ubGlobals.end(),
            [](const GlobalValue* lhs, const GlobalValue* rhs) {
                return lhs->GetName() < rhs->GetName();
            }
        );
        for (const auto* globalValue : ubGlobals) {
            renderGlobalInfo(*globalValue);
        }
        return true;
    }

    if (className == "" || className.empty()) {
        return false;
    }

    TypeId tid = TypeId::LookupByName(className);  // 获取TypeId

    if (!attrName.empty()) {  // attrName not empty
        struct TypeId::AttributeInformation info;
        if (tid.LookupAttributeByName(attrName, &info)) {  // 输出单个属性值
            std::cout << "Attribute: " << info.name << '\n'
                      << "Description: " << info.help << '\n'
                      << "DataType: " << info.checker->GetValueTypeName() << '\n'
                      << "Default: " << info.initialValue->SerializeToString(info.checker)
                      << std::endl;
        } else {
            std::cout << "Attribute not found!" << std::endl;
        }
    } else {  // 输出所有属性
        for (uint32_t i = 0; i < tid.GetAttributeN(); ++i) {
            TypeId::AttributeInformation info = tid.GetAttribute(i);
            std::cout << "Attribute: " << info.name << '\n'
                      << "Description: " << info.help << '\n'
                      << "DataType: " << info.checker->GetValueTypeName() << '\n'
                      << "Default: " << info.initialValue->SerializeToString(info.checker)
                      << std::endl;
        }
    }
    return true;  // 执行完后退出程序
}

void UbUtils::InitFaultMoudle(const string &FaultConfigFile)
{
    Ptr<UbFault> ubFault = CreateObject<UbFault>();
    for (auto it = NodeList::Begin(); it != NodeList::End(); ++it) {
        Ptr<Node> node = *it;
        uint16_t portNum = node->GetNDevices();
        for (int i = 0; i < portNum; i++) {
            Ptr<UbPort> port = DynamicCast<ns3::UbPort>(node->GetDevice(i));
            port->SetFaultCallBack(MakeCallback(&UbFault::FaultCallback, ubFault));
        }
    }

    ubFault->InitFault(FaultConfigFile);
}

} // namespace utils
