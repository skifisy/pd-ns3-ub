// SPDX-License-Identifier: GPL-2.0-only
#include "ub-case-runner.h"

#include "ns3/boolean.h"
#include "ns3/command-line.h"
#include "ns3/node-list.h"
#include "ns3/ub-app.h"
#include "ns3/ub-link.h"
#include "ns3/ub-port.h"
#include "ns3/ub-traffic-gen.h"
#include "ns3/ub-transport.h"
#include "ns3/ub-utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#ifdef NS3_MPI
#include "ns3/mpi-interface.h"
#include <mpi.h>
#endif

#ifdef NS3_MTP
#include "ns3/mtp-interface.h"
#endif

using namespace utils;

namespace ns3
{

namespace
{

struct QuickExampleOptions
{
    bool test = false;
    uint32_t mtpThreads = 0;
    uint32_t stopMs = 0;
    uint32_t rngRun = 10;
    std::string configPath;
    std::string dependencyVisibilityDelay;
    std::string initialTaskStartOffsetWindow = "0ps";
    std::string linkDelayOffsetWindow = "0ps";
    uint32_t timingOffsetSeed = 1;
    std::string canonicalOutputPath;
};

struct DropAbortState
{
    bool retransEnabled = false;
    bool triggered = false;
    std::string reason;
};

struct RuntimeSelection
{
    enum class Mode
    {
        LocalSingle,
        LocalMtp,
        MpiSingle,
        MpiMtp,
    };

    Mode mode = Mode::LocalSingle;
    bool enableMpi = false;
    uint32_t mpiRank = 0;
};

struct MpiLaunchProbe
{
    bool initializedHere = false;
    uint32_t rank = 0;
    uint32_t size = 1;
};

struct PhaseTiming
{
    std::chrono::high_resolution_clock::time_point programStart;
    std::chrono::high_resolution_clock::time_point simulationStart;
    std::chrono::high_resolution_clock::time_point simulationEnd;
    std::chrono::high_resolution_clock::time_point traceStart;
    std::chrono::high_resolution_clock::time_point programEnd;
};

std::string FormatTime(double time_us)
{
    double val = time_us;
    const char* unit = " us";
    int precision = 0;
    if (time_us >= 1e6)
    {
        val = time_us / 1e6;
        unit = " s";
        precision = 6;
    }
    else if (time_us >= 1e3)
    {
        val = time_us / 1e3;
        unit = " ms";
        precision = 3;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << val << unit;
    return oss.str();
}

std::string FormatSummaryLine(const std::string& label, double time_us)
{
    std::ostringstream oss;
    oss << "[summary]   " << std::left << std::setw(6) << label << " : " << FormatTime(time_us);
    return oss.str();
}

void CheckNoProgress(double sim_time_us, std::ostringstream& oss)
{
    static uint32_t last_completed_tasks = 0;
    static double last_progress_time_us = 0;
    uint32_t completed_tasks = UbTrafficGen::Get()->GetCompletedTaskCount();

    if (completed_tasks > last_completed_tasks)
    {
        last_completed_tasks = completed_tasks;
        last_progress_time_us = sim_time_us;
    }

    if (sim_time_us - last_progress_time_us > 10000 && sim_time_us > 10000)
    {
        oss << " [WARNING: No task completed for "
            << FormatTime(sim_time_us - last_progress_time_us) << "]";
    }
}

DropAbortState& GetDropAbortState()
{
    static DropAbortState state;
    return state;
}

void ResetDropAbortState()
{
    auto& state = GetDropAbortState();
    state = DropAbortState{};
}

bool IsRetransEnabledForRun()
{
    Ptr<UbTransportChannel> tp = CreateObject<UbTransportChannel>();
    BooleanValue value;
    tp->GetAttribute("EnableRetrans", value);
    return value.Get();
}

void CheckDropWithoutRetrans(std::ostringstream& oss)
{
    auto& state = GetDropAbortState();
    if (state.triggered || state.retransEnabled)
    {
        return;
    }

    if (UbUtils::Get()->GetRuntimePacketDropCount() == 0)
    {
        return;
    }

    state.triggered = true;
    state.reason = UbUtils::Get()->GetRuntimePacketDropReason();
    oss << " [ERROR: Packet dropped while retransmission is disabled; stopping run]";
    std::cout << std::endl;
    UbUtils::Get()->PrintTimestamp(
        "[error] Packet dropped while retransmission is disabled. "
        "This run cannot guarantee completion without end-to-end recovery.");
    if (!state.reason.empty())
    {
        UbUtils::Get()->PrintTimestamp("[error] First drop reason: " + state.reason);
    }
    UbUtils::Get()->PrintTimestamp(
        "[error] Suggested actions: enable ns3::UbTransportChannel::EnableRetrans; "
        "prefer CBFC for lossless backpressure, or tune PFC carefully if PFC is required.");
    Simulator::Stop();
}

void CheckExampleProcess()
{
    double sim_time_us = Simulator::Now().GetMicroSeconds();
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);

    std::ostringstream oss;
    oss << "[" << std::put_time(&tm_buf, "%H:%M:%S") << "] "
        << "Simulation time progress: " << FormatTime(sim_time_us);

    CheckNoProgress(sim_time_us, oss);
    CheckDropWithoutRetrans(oss);

    std::cout << "\r" << oss.str() << std::flush;
    if (GetDropAbortState().triggered)
    {
        return;
    }
    if (!UbTrafficGen::Get()->IsCompleted())
    {
        Simulator::Schedule(MicroSeconds(100), &CheckExampleProcess);
        return;
    }
    std::cout << std::endl;
    Simulator::Stop();
}

MpiLaunchProbe ProbeMpiWorld(int* argc, char*** argv, uint32_t mtpThreads)
{
    MpiLaunchProbe probe;
#ifdef NS3_MPI
    const int requestedThreadLevel =
        mtpThreads > 1 ? MPI_THREAD_SERIALIZED : MPI_THREAD_SINGLE;
    int providedThreadLevel = MPI_THREAD_SINGLE;
    int initialized = 0;
    MPI_Initialized(&initialized);
    if (!initialized)
    {
        const int rc = MPI_Init_thread(argc, argv, requestedThreadLevel, &providedThreadLevel);
        NS_ABORT_MSG_IF(rc != MPI_SUCCESS, "MPI_Init_thread failed while probing quick-entry runtime");
        probe.initializedHere = true;
    }
    else
    {
        MPI_Query_thread(&providedThreadLevel);
    }

    int mpiRank = 0;
    int mpiSize = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &mpiRank);
    MPI_Comm_size(MPI_COMM_WORLD, &mpiSize);
    NS_ABORT_MSG_IF(mpiSize > 1 && mtpThreads > 1 && providedThreadLevel < MPI_THREAD_SERIALIZED,
                    "MPI runtime does not provide the requested thread level");
    probe.rank = static_cast<uint32_t>(mpiRank);
    probe.size = static_cast<uint32_t>(mpiSize);
#else
    (void)argc;
    (void)argv;
    (void)mtpThreads;
#endif
    return probe;
}

void FinalizeMpiProbeIfNeeded(MpiLaunchProbe& probe)
{
#ifdef NS3_MPI
    if (!probe.initializedHere)
    {
        return;
    }

    int finalized = 0;
    MPI_Finalized(&finalized);
    if (!finalized)
    {
        MPI_Finalize();
    }
    probe.initializedHere = false;
#else
    (void)probe;
#endif
}

bool IsMtpRequested(uint32_t mtpThreads)
{
    return mtpThreads > 1;
}

RuntimeSelection::Mode ResolveRuntimeMode(bool enableMpi, uint32_t mtpThreads)
{
    [[maybe_unused]] const bool wantsMtp = IsMtpRequested(mtpThreads);
    if (enableMpi)
    {
#ifdef NS3_MTP
        return wantsMtp ? RuntimeSelection::Mode::MpiMtp : RuntimeSelection::Mode::MpiSingle;
#else
        return RuntimeSelection::Mode::MpiSingle;
#endif
    }

#ifdef NS3_MTP
    return wantsMtp ? RuntimeSelection::Mode::LocalMtp : RuntimeSelection::Mode::LocalSingle;
#else
    return RuntimeSelection::Mode::LocalSingle;
#endif
}

bool ModeUsesMtp(RuntimeSelection::Mode mode)
{
    return mode == RuntimeSelection::Mode::LocalMtp || mode == RuntimeSelection::Mode::MpiMtp;
}

void PrintTestResult(bool passed, bool enableMpi, uint32_t mpiRank)
{
    if (!passed)
    {
        std::cout << "TEST : 00000 : FAILED" << std::endl;
        return;
    }

#ifdef NS3_MPI
    if (enableMpi && mpiRank != 0)
    {
        return;
    }
#else
    (void)enableMpi;
    (void)mpiRank;
#endif

    std::cout << "TEST : 00000 : PASSED" << std::endl;
}

void PrepareSimulatorMode(const RuntimeSelection& runtime, uint32_t mtpThreads)
{
    switch (runtime.mode)
    {
    case RuntimeSelection::Mode::LocalSingle:
        return;
    case RuntimeSelection::Mode::LocalMtp:
#ifdef NS3_MTP
        Config::SetDefault("ns3::MultithreadedSimulatorImpl::MaxThreads",
                           UintegerValue(mtpThreads));
        GlobalValue::Bind("SimulatorImplementationType",
                          StringValue("ns3::MultithreadedSimulatorImpl"));
        return;
#else
        return;
#endif
    case RuntimeSelection::Mode::MpiSingle:
#ifdef NS3_MPI
        GlobalValue::Bind("SimulatorImplementationType",
                          StringValue("ns3::DistributedSimulatorImpl"));
        return;
#else
        return;
#endif
    case RuntimeSelection::Mode::MpiMtp:
#if defined(NS3_MPI) && defined(NS3_MTP)
        MtpInterface::Enable(mtpThreads);
        return;
#else
        return;
#endif
    }
}

std::string NormalizeCasePath(const std::string& path)
{
    return std::filesystem::absolute(std::filesystem::path(path)).lexically_normal().string();
}

std::string
Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool
IsDisabledOffsetWindow(const std::string& value)
{
    const std::string normalized = Lowercase(value);
    return normalized.empty() || normalized == "off" || normalized == "none" ||
           normalized == "false" || normalized == "0";
}

Time
ParseOffsetWindow(const std::string& value, const char* optionName)
{
    if (IsDisabledOffsetWindow(value))
    {
        return Time(0);
    }

    const Time window(value);
    if (window.IsStrictlyNegative())
    {
        std::cerr << optionName << " must be non-negative" << std::endl;
        std::exit(1);
    }
    return window;
}

void ValidateCasePathOrExit(const std::string& configPath)
{
    static const std::array<const char*, 5> kRequiredCaseFiles = {"network_attribute.txt",
                                                                   "node.csv",
                                                                   "topology.csv",
                                                                   "routing_table.csv",
                                                                   "traffic.csv"};

    const std::filesystem::path caseDir(configPath);
    if (!std::filesystem::exists(caseDir))
    {
        std::cerr << "case path does not exist: " << caseDir.string() << std::endl;
        std::exit(1);
    }
    if (!std::filesystem::is_directory(caseDir))
    {
        std::cerr << "case path is not a directory: " << caseDir.string() << std::endl;
        std::exit(1);
    }

    for (const char* filename : kRequiredCaseFiles)
    {
        const std::filesystem::path requiredFile = caseDir / filename;
        if (!std::filesystem::exists(requiredFile))
        {
            std::cerr << "missing required case file: " << requiredFile.string() << std::endl;
            std::exit(1);
        }
    }
}

void
BuildScenarioFromConfig(const QuickExampleOptions& options, const RuntimeSelection& runtime)
{
    const std::string& configPath = options.configPath;
    const Time linkOffsetWindow =
        ParseOffsetWindow(options.linkDelayOffsetWindow, "--link-delay-offset-window");
    UbUtils::Get()->SetComponentsAttribute(configPath + "/network_attribute.txt");
    UbUtils::Get()->CreateTraceDir();
    UbUtils::Get()->CreateNode(configPath + "/node.csv");
    const auto offsetStats = UbUtils::Get()->CreateTopo(configPath + "/topology.csv",
                                                        linkOffsetWindow,
                                                        options.timingOffsetSeed);
    UbUtils::Get()->AddRoutingTable(configPath + "/routing_table.csv");
    UbUtils::Get()->CreateTp(configPath + "/transport_channel.csv");
    UbUtils::Get()->TopoTraceConnect();

    const bool shouldPrint = !runtime.enableMpi || runtime.mpiRank == 0;
    if (shouldPrint && linkOffsetWindow.IsStrictlyPositive())
    {
        std::cout << "[INFO] Link delay offset enabled: window=" << options.linkDelayOffsetWindow
                  << ", seed=" << options.timingOffsetSeed
                  << ", positive-links=" << offsetStats.positiveLinkCount
                  << ", zero-delay-links-preserved=" << offsetStats.zeroDelayLinkCount
                  << ", distinct-offsets=" << offsetStats.distinctOffsetCount
                  << ", offset-reuses=" << offsetStats.offsetReuseCount
                  << ", max-links-per-offset=" << offsetStats.maxLinksPerOffset << "." << std::endl;
    }
    else if (shouldPrint && ModeUsesMtp(runtime.mode))
    {
        std::cout << "[INFO] Link delay offset is off (default). Enable the deterministic "
                     "positive-link workaround with --link-delay-offset-window=<Time> and record "
                     "--timing-offset-seed."
                  << std::endl;
    }
}

void ConfigureTrafficDependencyVisibility(const QuickExampleOptions& options,
                                          const RuntimeSelection& runtime)
{
    if (!options.dependencyVisibilityDelay.empty())
    {
        UbTrafficGen::Get()->SetDependencyVisibilityDelay(Time(options.dependencyVisibilityDelay));
    }

    const bool usePositiveLocalLinkDelays = ModeUsesMtp(runtime.mode);
    for (uint32_t nodeIndex = 0; nodeIndex < NodeList::GetNNodes(); ++nodeIndex)
    {
        Ptr<Node> node = NodeList::GetNode(nodeIndex);
        for (uint32_t deviceIndex = 0; deviceIndex < node->GetNDevices(); ++deviceIndex)
        {
            Ptr<UbPort> port = DynamicCast<UbPort>(node->GetDevice(deviceIndex));
            if (port == nullptr)
            {
                continue;
            }

            Ptr<UbLink> link = DynamicCast<UbLink>(port->GetChannel());
            if (link == nullptr)
            {
                continue;
            }

            const Time delay = link->GetDelay();
            if (link->IsRemote() ||
                (usePositiveLocalLinkDelays && delay.IsStrictlyPositive()))
            {
                UbTrafficGen::Get()->ConsiderAutomaticDependencyVisibilityDelay(delay);
            }
        }
    }
}

void ApplyDependencyVisibilityLookaheadBound(const RuntimeSelection& runtime)
{
    const Time delay = UbTrafficGen::Get()->GetDependencyVisibilityDelay();
    if (!delay.IsStrictlyPositive())
    {
        return;
    }

#ifdef NS3_MTP
    if (ModeUsesMtp(runtime.mode))
    {
        MtpInterface::BoundLookAhead(delay);
    }
#endif

#ifdef NS3_MPI
    if (!runtime.enableMpi)
    {
        return;
    }

    MpiInterface::BoundLookAhead(delay);
#else
    (void)runtime;
#endif
}

void ConfigureCanonicalOutput(const QuickExampleOptions& options, const RuntimeSelection& runtime)
{
    if (!options.canonicalOutputPath.empty())
    {
        UbTrafficGen::Get()->EnableCanonicalOutput(options.canonicalOutputPath, runtime.mpiRank);
    }
}

void
ConfigureInitialTaskStartOffset(const QuickExampleOptions& options,
                                const RuntimeSelection& runtime)
{
    const Time offsetWindow = ParseOffsetWindow(options.initialTaskStartOffsetWindow,
                                                "--initial-task-start-offset-window");

    const bool shouldPrint = !runtime.enableMpi || runtime.mpiRank == 0;
    if (offsetWindow.IsZero())
    {
        UbTrafficGen::Get()->SetInitialTaskStartOffsetWindow(Time(0), options.timingOffsetSeed);
        if (shouldPrint && ModeUsesMtp(runtime.mode))
        {
            std::cout << "[INFO] Initial task start offset is off (default). Enable it with "
                         "--initial-task-start-offset-window=<Time> and record "
                         "--timing-offset-seed."
                      << std::endl;
        }
        return;
    }

    UbTrafficGen::Get()->SetInitialTaskStartOffsetWindow(offsetWindow, options.timingOffsetSeed);
    if (shouldPrint)
    {
        std::cout << "[INFO] Initial task start offset enabled: window="
                  << options.initialTaskStartOffsetWindow << ", seed=" << options.timingOffsetSeed
                  << "." << std::endl;
    }
}

uint32_t ActivateTrafficFromConfig(const std::string& configPath,
                                   bool activateLocalOwnedTasksOnly,
                                   uint32_t mpiRank,
                                   bool requirePositiveDependencyVisibilityDelay)
{
    const std::string trafficPath = configPath + "/traffic.csv";
    const auto trafficStats =
        UbUtils::Get()->RegisterTrafficPhaseDependenciesAndGetStats(trafficPath);
    UbTrafficGen::Get()->ReserveTasksForTraffic(trafficStats.recordCount,
                                                trafficStats.maxTaskId);
    if (UbUtils::Get()->IsFaultEnabled())
    {
        UbUtils::Get()->InitFaultMoudle(configPath + "/fault.csv");
    }

    uint32_t localTaskCount = 0;
    std::vector<Ptr<UbApp>> sourceApps;
    Ptr<UbApp> appDefaults = CreateObject<UbApp>();
    const bool reserveRtpConnections = appDefaults->GetTransportMode() == TransportMode::RTP;
    const bool useShortestPaths = appDefaults->GetUseShortestPaths();
    UbUtils::Get()->PrintTimestamp("[traffic] Activate clients and enqueue tasks.");
    UbUtils::Get()->ForEachTrafficRecordView(trafficPath, [&](const TrafficRecordView& record) {
        Ptr<Node> sourceNode = NodeList::GetNode(record.sourceNode);
        if (reserveRtpConnections &&
            (record.opType == "URMA_WRITE" || record.opType == "URMA_READ"))
        {
            sourceNode->GetObject<UbController>()->GetTpConnManager()->ReserveTpnsForTraffic(
                useShortestPaths,
                record.sourceNode,
                record.destNode,
                record.priority);
        }

        const bool localOwned =
            !activateLocalOwnedTasksOnly ||
            UbUtils::ExtractMpiRank(sourceNode->GetSystemId()) == mpiRank;

        if (localOwned)
        {
            if (static_cast<size_t>(record.sourceNode) >= sourceApps.size())
            {
                sourceApps.resize(static_cast<size_t>(record.sourceNode) + 1);
            }

            Ptr<UbApp>& client = sourceApps[record.sourceNode];
            if (client == nullptr)
            {
                if (sourceNode->GetNApplications() == 0)
                {
                    client = CreateObject<UbApp>();
                    sourceNode->AddApplication(client);
                    UbUtils::Get()->ClientTraceConnect(record.sourceNode);
                }
                else
                {
                    client = DynamicCast<UbApp>(sourceNode->GetApplication(0));
                }
                UbTrafficGen::Get()->RegisterSourceAppDuringInitialLoad(record.sourceNode, client);
            }
            ++localTaskCount;
        }

        UbTrafficGen::Get()->AddTaskDuringInitialLoad(record);
    });

    UbTrafficGen::Get()->ValidateDependencyVisibilityDelay(
        requirePositiveDependencyVisibilityDelay);
    UbTrafficGen::Get()->ScheduleNextTasks();
    UbUtils::Get()->PrintTimestamp("[traffic] Scheduled local tasks: " +
                                   std::to_string(localTaskCount));
    CheckExampleProcess();
    return localTaskCount;
}

void ShutdownRuntime(const RuntimeSelection& runtime)
{
    Simulator::Destroy();
#ifdef NS3_MPI
    // Simulator::Destroy() lets the simulator implementation release MPI receive
    // buffers. MpiInterface::Disable() owns the duplicated communicator and MPI
    // finalization state.
    if (runtime.enableMpi && MpiInterface::IsEnabled())
    {
        MpiInterface::Disable();
    }
#else
    (void)runtime;
#endif
}

bool HandleAttributeQuery(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        std::string arg(argv[i]);
        if (arg.find("--ClassName") == 0 || arg.find("--GlobalName") == 0 ||
            arg.find("--PrintUbGlobals") == 0)
        {
            if (UbUtils::Get()->QueryAttributeInfo(argc, argv))
            {
                return true;
            }
            break;
        }
    }
    return false;
}

QuickExampleOptions ParseOptions(int argc, char* argv[])
{
    QuickExampleOptions options;
    std::string casePathArg;
    std::string positionalCasePath;
    CommandLine cmd;
    cmd.Usage("Unified-bus config-driven user entry.\n"
              "Typical usage:\n"
              "  recommended: python3.12 ./ns3 run --no-build 'scratch/ub-quick-example --case-path=<case-dir>'\n"
              "  example:     python3.12 ./ns3 run --no-build 'src/unified-bus/examples/ub-quick-example --case-path=<case-dir>'\n"
              "  node.csv:    allocationDelay maps to AllocationTime; forwardDelay maps to "
              "InPortProcessingDelay; legacy 4-column forwardDelay maps to AllocationTime.\n");
    cmd.AddValue("test", "Enable regression-test style output", options.test);
    cmd.AddValue("mtp-threads",
                 "Number of MTP threads (0-1 to disable, >=2 to enable)",
                 options.mtpThreads);
    cmd.AddValue("case-path",
                 "Required path to the unified-bus case directory",
                 casePathArg);
    cmd.AddValue("stop-ms", "Optional simulation stop time in milliseconds", options.stopMs);
    cmd.AddValue("rng-run", "Random seed value passed to RngSeedManager::SetSeed", options.rngRun);
    cmd.AddValue("dependency-visibility-delay",
                 "Advanced override for the dependency visibility delay inferred from UB links",
                 options.dependencyVisibilityDelay);
    cmd.AddValue("initial-task-start-offset-window",
                 "Add one deterministic per-source start offset to tasks with no phase "
                 "dependencies in [0, window); use 0ps or off to disable "
                 "(default: 0ps)",
                 options.initialTaskStartOffsetWindow);
    cmd.AddValue("link-delay-offset-window",
                 "Advanced workaround: add a deterministic per-link offset to positive topology "
                 "delays in [0, window); use 0ps or off to disable (default: 0ps)",
                 options.linkDelayOffsetWindow);
    cmd.AddValue("timing-offset-seed",
                 "Advanced: shared seed for deterministic initial-task and link-delay offset "
                 "assignment (default: 1)",
                 options.timingOffsetSeed);
    cmd.AddValue("canonical-output",
                 "Write deterministic UbTrafficGen canonical events to this output basename",
                 options.canonicalOutputPath);
    cmd.AddNonOption("casePath",
                     "Required unified-bus case directory when --case-path is omitted",
                     positionalCasePath);
    cmd.Parse(argc, argv);
    if (!casePathArg.empty() && !positionalCasePath.empty() &&
        NormalizeCasePath(casePathArg) != NormalizeCasePath(positionalCasePath))
    {
        std::cerr << "conflicting case paths provided via --case-path and casePath" << std::endl;
        std::exit(1);
    }

    options.configPath = casePathArg.empty() ? positionalCasePath : casePathArg;
    if (options.configPath.empty())
    {
        std::cerr << "missing required case path (--case-path or casePath)" << std::endl;
        std::exit(1);
    }
    options.configPath = NormalizeCasePath(options.configPath);
    ValidateCasePathOrExit(options.configPath);

    return options;
}

void EnableExampleLogging()
{
    Time::SetResolution(Time::PS);

    ns3::LogComponentEnableAll(LOG_PREFIX_TIME);

    LogComponentEnable("UbSwitchAllocator", LOG_LEVEL_WARN);
    LogComponentEnable("UbQueueManager", LOG_LEVEL_WARN);
    LogComponentEnable("UbCaqm", LOG_LEVEL_WARN);
    LogComponentEnable("UbTrafficGen", LOG_LEVEL_WARN);
    LogComponentEnable("UbApp", LOG_LEVEL_WARN);
    LogComponentEnable("UbCongestionControl", LOG_LEVEL_WARN);
    LogComponentEnable("UbController", LOG_LEVEL_WARN);
    LogComponentEnable("UbDataLink", LOG_LEVEL_WARN);
    LogComponentEnable("UbFlowControl", LOG_LEVEL_WARN);
    LogComponentEnable("UbHeader", LOG_LEVEL_WARN);
    LogComponentEnable("UbLink", LOG_LEVEL_WARN);
    LogComponentEnable("UbLdstInstance", LOG_LEVEL_WARN);
    LogComponentEnable("UbLdstThread", LOG_LEVEL_WARN);
    LogComponentEnable("UbLdstApi", LOG_LEVEL_WARN);
    LogComponentEnable("UbPort", LOG_LEVEL_WARN);
    LogComponentEnable("UbRoutingProcess", LOG_LEVEL_WARN);
    LogComponentEnable("UbSwitch", LOG_LEVEL_WARN);
    LogComponentEnable("UbFunction", LOG_LEVEL_WARN);
    LogComponentEnable("UbTransportChannel", LOG_LEVEL_WARN);
    LogComponentEnable("UbFault", LOG_LEVEL_WARN);
    LogComponentEnable("UbTransaction", LOG_LEVEL_WARN);
    LogComponentEnable("TpConnectionManager", LOG_LEVEL_WARN);
}

RuntimeSelection PrepareRuntime(int* argc,
                                char*** argv,
                                const QuickExampleOptions& options,
                                const MpiLaunchProbe& mpiProbe)
{
    RuntimeSelection runtime;
    runtime.enableMpi = mpiProbe.size > 1;
#ifndef NS3_MPI
    runtime.enableMpi = false;
#endif
    runtime.mode = ResolveRuntimeMode(runtime.enableMpi, options.mtpThreads);
    PrepareSimulatorMode(runtime, options.mtpThreads);

#ifdef NS3_MPI
    if (runtime.enableMpi)
    {
        MpiInterface::Enable(MPI_COMM_WORLD);
        runtime.mpiRank = MpiInterface::GetSystemId();
        UbTrafficGen::Get()->RegisterMpiTaskCompletionHandler();
    }
    (void)argc;
    (void)argv;
#else
    (void)argc;
    (void)argv;
#endif

    if (IsMtpRequested(options.mtpThreads))
    {
        if (ModeUsesMtp(runtime.mode))
        {
            std::cout << "[INFO] MTP enabled with " << options.mtpThreads << " threads."
                      << (runtime.enableMpi ? " (hybrid MPI mode)." : " (local mode).")
                      << std::endl;
            std::cout << "[INFO] Parallel runs preserve causal ordering and deterministic task "
                         "completion ordering, but events with the same simulation time may be "
                         "processed in a different order than single-thread runs. Compare task or "
                         "workload metrics using predefined acceptance criteria."
                      << std::endl;
        }
#ifndef NS3_MTP
        else
        {
            std::cerr << "[ERROR] MTP requested but not compiled. Reconfigure with --enable-mtp"
                      << std::endl;
            std::exit(1);
        }
#endif
    }

    return runtime;
}

PhaseTiming RunScenario(const QuickExampleOptions& options,
                        const RuntimeSelection& runtime,
                        const std::chrono::high_resolution_clock::time_point& programStart)
{
    PhaseTiming timing;
    timing.programStart = programStart;

    EnableExampleLogging();

    UbUtils::Get()->PrintTimestamp("[case] Run case: " + options.configPath);
    UbUtils::Get()->PrintTimestamp(
        "[case] node.csv delay fields: allocationDelay -> AllocationTime; "
        "forwardDelay -> InPortProcessingDelay; legacy 4-column forwardDelay -> AllocationTime.");
    RngSeedManager::SetSeed(options.rngRun);

    timing.simulationStart = std::chrono::high_resolution_clock::now();
    UbUtils::ResetRuntimeDropDiagnostics();
    BuildScenarioFromConfig(options, runtime);
    ConfigureTrafficDependencyVisibility(options, runtime);
    ApplyDependencyVisibilityLookaheadBound(runtime);
    ConfigureCanonicalOutput(options, runtime);
    ConfigureInitialTaskStartOffset(options, runtime);
    ResetDropAbortState();
    GetDropAbortState().retransEnabled = IsRetransEnabledForRun();
    ActivateTrafficFromConfig(options.configPath,
                              runtime.enableMpi,
                              runtime.mpiRank,
                              runtime.enableMpi || ModeUsesMtp(runtime.mode));
    if (options.stopMs > 0)
    {
        Simulator::Stop(MilliSeconds(options.stopMs));
    }
    Simulator::Run();
    timing.simulationEnd = std::chrono::high_resolution_clock::now();

    UbUtils::Get()->Destroy();
    UbTrafficGen::Get()->WriteCanonicalOutput();
    ShutdownRuntime(runtime);

    UbUtils::Get()->PrintTimestamp("[run] Simulation finished.");
    timing.traceStart = std::chrono::high_resolution_clock::now();
    UbUtils::Get()->ParseTrace(options.test);
    timing.programEnd = std::chrono::high_resolution_clock::now();
    return timing;
}

void ReportResult(const QuickExampleOptions& options,
                  const RuntimeSelection& runtime,
                  const PhaseTiming& timing)
{
    const double config_wall_us =
        std::chrono::duration_cast<std::chrono::microseconds>(timing.simulationStart -
                                                              timing.programStart)
            .count();
    const double run_wall_us =
        std::chrono::duration_cast<std::chrono::microseconds>(timing.simulationEnd -
                                                              timing.simulationStart)
            .count();
    const double trace_wall_us =
        std::chrono::duration_cast<std::chrono::microseconds>(timing.programEnd - timing.traceStart)
            .count();
    const double total_wall_us =
        std::chrono::duration_cast<std::chrono::microseconds>(timing.programEnd - timing.programStart)
            .count();

    UbUtils::Get()->PrintTimestamp("[summary] Program finished.");
    UbUtils::Get()->PrintTimestamp("[summary] Wall-clock:");
    UbUtils::Get()->PrintTimestamp(FormatSummaryLine("config", config_wall_us));
    UbUtils::Get()->PrintTimestamp(FormatSummaryLine("run", run_wall_us));
    UbUtils::Get()->PrintTimestamp(FormatSummaryLine("trace", trace_wall_us));
    UbUtils::Get()->PrintTimestamp(FormatSummaryLine("total", total_wall_us));
    if (options.test)
    {
        PrintTestResult(UbTrafficGen::Get()->IsCompleted(), runtime.enableMpi, runtime.mpiRank);
    }
}

} // namespace

int RunUbCaseRunner(int argc, char* argv[])
{
    if (HandleAttributeQuery(argc, argv))
    {
        return 0;
    }

    QuickExampleOptions options = ParseOptions(argc, argv);
    MpiLaunchProbe mpiProbe = ProbeMpiWorld(&argc, &argv, options.mtpThreads);
    if (mpiProbe.size <= 1)
    {
        FinalizeMpiProbeIfNeeded(mpiProbe);
    }
    const auto programStart = std::chrono::high_resolution_clock::now();
    RuntimeSelection runtime = PrepareRuntime(&argc, &argv, options, mpiProbe);
    PhaseTiming timing = RunScenario(options, runtime, programStart);
    ReportResult(options, runtime, timing);
    FinalizeMpiProbeIfNeeded(mpiProbe);
    return GetDropAbortState().triggered ? 1 : 0;
}

} // namespace ns3
