// SPDX-License-Identifier: GPL-2.0-only
#ifndef UB_TRAFFIC_GEN_H
#define UB_TRAFFIC_GEN_H

#include "ub-network-address.h"
#include "ub-tp-connection-manager.h"

#include "ns3/application.h"
#include "ns3/event-id.h"
#include "ns3/ipv4-address.h"
#include "ns3/nstime.h"
#include "ns3/ptr.h"
#include "ns3/ub-controller.h"
#include "ns3/ub-datatype.h"
#include "ns3/ub-ldst-api.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class UbTrafficGenPhaseDependencyMemoryTest;
class UbTrafficGenSparseTaskIdFallbackTest;
class UbTrafficGenReserveHintTest;
class UbTrafficGenDenseTaskStoreTest;
class UbTrafficGenDuplicateDependencyPhaseTest;
class UbTrafficGenRecordViewDependencyTest;
class UbTrafficGenReadyOrderTest;
class UbTrafficGenIntegerDelayParserTest;
class UbTrafficGenRuntimeTaskEntityFieldsTest;
class UbTrafficGenInitialTaskStartOffsetTest;

using namespace utils;
namespace ns3 {
    class UbApp;

/**
 * @brief 任务图应用,管理多个WQE任务及依赖关系
 */
class UbTrafficGen : public Object, public Singleton<UbTrafficGen> {
public:
    static UbTrafficGen& GetInstance() {
        static UbTrafficGen instance;
        return instance;
    }

    UbTrafficGen(const UbTrafficGen&) = delete;
    UbTrafficGen& operator = (const UbTrafficGen&) = delete;

public:
    static TypeId GetTypeId(void);

    UbTrafficGen();
    virtual ~UbTrafficGen();

    enum class RuntimeTaskOp : uint8_t
    {
        UNKNOWN,
        URMA_WRITE,
        URMA_READ,
        MEM_STORE,
        MEM_LOAD
    };

    struct RuntimeTask
    {
        uint32_t taskId{0};
        uint32_t sourceNode{0};
        uint32_t destNode{0};
        uint32_t dataSize{0};
        uint32_t phaseId{0};
        uint8_t priority{0};
        RuntimeTaskOp op{RuntimeTaskOp::UNKNOWN};
        Time delay{Time(0)};
        uint32_t srcEntityId{0};
        uint32_t dstEntityId{0};
        bool hasSrcEntityId{false};
        bool hasDstEntityId{false};
        bool hasPhaseDependencies{false};
    };

    struct TaskCleanupInfo
    {
        uint32_t sourceNode{0};
        uint32_t destNode{0};
        uint8_t priority{0};
    };

    static bool IsMultiProcessRuntimeUnsupported();

    static inline std::string GetMultiProcessUnsupportedMessage()
    {
        return "UbTrafficGen supports MPI multi-process traffic DAG usage when dependency "
               "visibility is configured for parallel runs.";
    }

    static constexpr std::size_t kMaxDelayParseCacheEntries = 4096;

    void AddTask(const TrafficRecord& record);

    void AddTaskDuringInitialLoad(const TrafficRecord& record);

    void AddTaskDuringInitialLoad(const TrafficRecordView& record);

    void SetPhaseDepend(uint32_t phaseId, uint32_t taskId);

    void SetPhaseDependDuringInitialLoad(uint32_t phaseId, uint32_t taskId);

    void RegisterPhaseTaskDuringInitialLoad(uint32_t phaseId);

    void ReserveTasksForTraffic(uint64_t recordCount, uint32_t maxTaskId);

    void RegisterSourceAppDuringInitialLoad(uint32_t sourceNode, Ptr<UbApp> app);

    TaskCleanupInfo GetTaskCleanupInfoById(uint32_t taskId);

    void MarkTaskCompleted(uint32_t taskId);

    void SetDependencyVisibilityDelay(Time delay);

    Time GetDependencyVisibilityDelay() const;

    void ValidateDependencyVisibilityDelay(bool requireStrictlyPositive) const;

    void ConsiderAutomaticDependencyVisibilityDelay(Time delay);

    /**
     * @brief Configure deterministic start offsets for tasks without phase dependencies.
     * @param window Half-open offset window; zero disables offsets.
     * @param seed Seed used for deterministic slot assignment.
     */
    void SetInitialTaskStartOffsetWindow(Time window, uint32_t seed);

    void ApplyTaskCompletion(uint32_t taskId);

    void RegisterMpiTaskCompletionHandler();

    void EnableCanonicalOutput(std::string path, uint32_t rank);

    void WriteCanonicalOutput() const;

    bool IsCompleted() const;

    uint32_t GetCompletedTaskCount() const;

    /**
     * @brief 任务完成回调
     */
    void OnTaskCompleted(uint32_t taskId);

    /**
     * @brief 调度下一批可执行的任务
     */
    void ScheduleNextTasks();

  private:
    friend class ::UbTrafficGenPhaseDependencyMemoryTest;
    friend class ::UbTrafficGenSparseTaskIdFallbackTest;
    friend class ::UbTrafficGenReserveHintTest;
    friend class ::UbTrafficGenDenseTaskStoreTest;
    friend class ::UbTrafficGenDuplicateDependencyPhaseTest;
    friend class ::UbTrafficGenRecordViewDependencyTest;
    friend class ::UbTrafficGenReadyOrderTest;
    friend class ::UbTrafficGenIntegerDelayParserTest;
    friend class ::UbTrafficGenRuntimeTaskEntityFieldsTest;
    friend class ::UbTrafficGenInitialTaskStartOffsetTest;

    enum class TaskState {
        PENDING,
        READY,
        RUNNING,
        COMPLETED
    };

    struct CanonicalEvent
    {
        uint64_t timeNs = 0;
        std::string type;
        uint32_t taskId = 0;
        uint32_t sourceNode = 0;
        uint32_t destNode = 0;
    };

    struct PhaseState {
        uint32_t totalTasks{0};
        uint32_t completedTasks{0};
        std::vector<uint32_t> dependentTasks{};
    };

    void RecordCanonicalEvent(const std::string& type, const RuntimeTask& task);

    void RecordCanonicalCompletionLocked(const RuntimeTask& task);

    bool ShouldRecordCanonicalEventLocked(const RuntimeTask& task) const;

    void StartTask(Ptr<UbApp> app, RuntimeTask task);

    struct ReadyTaskBatch
    {
        std::vector<RuntimeTask> tasks{};
        std::vector<Ptr<UbApp>> sourceApps{};
    };

    std::unordered_map<uint32_t, RuntimeTask> m_tasks{};
    bool m_useDenseTaskStore{true};
    std::vector<RuntimeTask> m_denseTasks{};
    std::unordered_map<uint32_t, TaskState> m_taskStates{};
    std::unordered_map<uint32_t, uint32_t> m_taskPhaseIds{};
    std::unordered_map<uint32_t, uint32_t> m_pendingPhaseCounts{};
    bool m_useDenseTaskState{true};
    std::vector<TaskState> m_denseTaskStates{};
    std::vector<uint32_t> m_densePendingPhaseCounts{};
    std::unordered_map<uint32_t, PhaseState> m_phaseStates{};
    std::vector<uint32_t> m_readyTasks{};
    bool m_readyTasksSorted{true};
    std::vector<Ptr<UbApp>> m_sourceApps{};
    std::unordered_map<std::string, Time> m_delayParseCache{};
    std::optional<Time> m_dependencyVisibilityDelay{};
    std::optional<Time> m_automaticDependencyVisibilityDelay{};
    bool m_canonicalOutputEnabled = false;
    std::string m_canonicalOutputPath;
    uint32_t m_canonicalRank = 0;
    std::vector<CanonicalEvent> m_canonicalEvents;
    Time m_initialTaskStartOffsetWindow{Time(0)};
    uint32_t m_initialTaskStartOffsetSeed{1};

    ReadyTaskBatch CollectReadyTaskBatchLocked();

    void ScheduleTasks(const ReadyTaskBatch& batch);

    uint64_t GetInitialTaskSourceOffsetHash(uint32_t sourceNode, uint32_t seed) const;

    std::vector<Time> GetInitialTaskStartOffsets(const ReadyTaskBatch& batch) const;

    void RegisterTaskPhaseLocked(uint32_t phaseId, uint32_t taskId);

    void AddTaskLocked(const TrafficRecord& record, bool phaseAlreadyIndexed);

    void AddTaskLocked(const TrafficRecordView& record, bool phaseAlreadyIndexed);

    void AddReadyTaskLocked(uint32_t taskId);

    RuntimeTask ConvertToRuntimeTask(const TrafficRecord& record);

    RuntimeTask ConvertToRuntimeTask(const TrafficRecordView& record);

    Time ParseDelayLocked(const std::string& delay);

    Time ParseDelayLocked(std::string_view delay);

    static bool TryParseIntegerDelay(const std::string& delay, Time* out);

    static bool TryParseIntegerDelay(std::string_view delay, Time* out);

    RuntimeTaskOp ParseOpLocked(const std::string& opType);

    RuntimeTaskOp ParseOpLocked(std::string_view opType);

    Ptr<UbApp> GetSourceAppLocked(uint32_t sourceNode) const;

    void SwitchDenseTaskStoreToMapLocked();

    bool HasTaskLocked(uint32_t taskId) const;

    RuntimeTask* FindTaskLocked(uint32_t taskId);

    const RuntimeTask* FindTaskLocked(uint32_t taskId) const;

    bool SetTaskLocked(RuntimeTask task);

    void SwitchDenseTaskStateToMapLocked();

    bool HasTaskStateLocked(uint32_t taskId) const;

    TaskState* FindTaskStateLocked(uint32_t taskId);

    void SetTaskStateLocked(uint32_t taskId, TaskState state);

    bool HasTaskPhaseLocked(uint32_t taskId) const;

    void SetTaskPhaseLocked(uint32_t taskId, uint32_t phaseId);

    uint32_t GetPendingPhaseCountLocked(uint32_t taskId) const;

    bool DecrementPendingPhaseCountLocked(uint32_t taskId);

    void SetPendingPhaseCountLocked(uint32_t taskId, uint32_t count);

    uint64_t GetDependencyReferenceCountForTesting() const;

    uint32_t GetPendingPhaseCountForTesting(uint32_t taskId) const;

    bool IsUsingDenseTaskStoreForTesting() const;

    uint32_t GetStoredTaskCountForTesting() const;

    std::vector<uint32_t> CollectReadyTaskIdsForTesting();

    bool HasDependenciesLocked() const;

    Time ResolveCompletionVisibleDelayLocked() const;

    void ScheduleTaskCompletionVisibility(uint32_t taskId, Time completionVisibleTs);

    mutable std::mutex m_mutex;
};

} // namespace ns3

#endif // UB_TRAFFIC_GEN_H
