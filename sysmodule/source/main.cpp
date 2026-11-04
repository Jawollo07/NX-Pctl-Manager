/*
 * NX-Pctl-Manager — headless Stratosphere sysmodule runtime.
 *
 * Project-authored code, licensed under GPL-2.0-or-later.
 * This phase provides a resident worker, local HIPC provisioning/status service,
 * encrypted HTTP policy synchronization and read-only global application-activity
 * monitoring for daily playtime. It does not enforce policies, retain title IDs,
 * or mutate PCTL state. Device credentials are persisted separately by secure sync.
 */
#include <stratosphere.hpp>
#include <stratosphere/pgl/pgl_shell_api.hpp>
#include <cstring>
#include "../../protocol/runtime_status.hpp"
#include "../../protocol/worker_supervision.hpp"
#include "../../protocol/policy.hpp"
#include "../../protocol/provisioning.hpp"
#include "../../protocol/usage_tracker.hpp"
#include "secure_sync.hpp"
#include "http_transport.hpp"
#include "usage_store.hpp"
#include "../../protocol/usage_snapshot.hpp"

#define AMS_NXPCTL_I_RUNTIME_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 65000, Result, GetRuntimeStatus, \
        (sf::Out<u32> out_protocol_version, sf::Out<u32> out_worker_state, sf::Out<u32> out_worker_heartbeat, sf::Out<u32> out_worker_restart_count, sf::Out<u64> out_policy_revision), \
        (out_protocol_version, out_worker_state, out_worker_heartbeat, out_worker_restart_count, out_policy_revision)) \
    AMS_SF_METHOD_INFO(C, H, 65001, Result, ConfigureEnrollment, \
        (sf::InBuffer request), (request)) \
    AMS_SF_METHOD_INFO(C, H, 65002, Result, GetUsageStatus, \
        (sf::Out<u32> out_monitor_state, sf::Out<u64> out_active_seconds, sf::Out<u32> out_daily_minutes), \
        (out_monitor_state, out_active_seconds, out_daily_minutes))

AMS_SF_DEFINE_INTERFACE(ams::nxpctl::impl, IRuntime, AMS_NXPCTL_I_RUNTIME_INTERFACE_INFO, 0x00000000)

namespace ams {

    namespace {

        constexpr s32 WorkerThreadPriority = 30;
        constexpr size_t WorkerStackSize = 0x8000;
        constexpr s32 SupervisorThreadPriority = 31;
        constexpr size_t SupervisorStackSize = 0x4000;
        constexpr s32 UsageMonitorThreadPriority = 32;
        constexpr size_t UsageMonitorStackSize = 0x4000;
        constexpr s64 WorkerPollIntervalNs = 1'000'000'000LL;
        constexpr u32 NoRunningApplicationResult = 0x6E4;

        alignas(os::ThreadStackAlignment) u8 g_worker_stack[WorkerStackSize];
        alignas(os::ThreadStackAlignment) u8 g_supervisor_stack[SupervisorStackSize];
        alignas(os::ThreadStackAlignment) u8 g_usage_monitor_stack[UsageMonitorStackSize];
        os::ThreadType g_worker_thread;
        os::ThreadType g_supervisor_thread;
        os::ThreadType g_usage_monitor_thread;
        os::EventType g_worker_stop_event;
        os::EventType g_supervisor_stop_event;
        os::EventType g_usage_monitor_stop_event;
        util::Atomic<u32> g_worker_state;
        util::Atomic<u32> g_worker_heartbeat;
        util::Atomic<u32> g_worker_restart_count;
        util::Atomic<u32> g_usage_monitor_state;
        util::Atomic<u64> g_usage_active_seconds;
        util::Atomic<u32> g_usage_daily_minutes;
        ::nxpctl::policy::PolicyStore g_policy_store;
        ::nxpctl::usage::DailyUsageTracker g_usage_tracker;
        bool g_worker_thread_created = false;
        bool g_supervisor_thread_created = false;
        bool g_usage_monitor_thread_created = false;
        bool g_pgl_initialized = false;
        bool g_usage_storage_ready = false;
        bool g_usage_persist_dirty = false;
        u64 g_last_usage_persist_seconds = 0;
        ::nxpctl::usage::Snapshot g_persisted_usage{};

        u64 GetMonotonicSeconds() {
            const s64 frequency = os::GetSystemTickFrequency();
            const s64 ticks = os::GetSystemTick().GetInt64Value();
            if (frequency <= 0 || ticks < 0) return 0;
            return static_cast<u64>(ticks / frequency);
        }

        void WipeProvisioningRequest(::nxpctl::protocol::ProvisioningRequest& request) {
            volatile u8* bytes = reinterpret_cast<volatile u8*>(std::addressof(request));
            for (std::size_t i = 0; i < sizeof(request); ++i) {
                bytes[i] = 0;
            }
        }

        void WorkerThreadMain(void *) {
            g_worker_state = static_cast<u32>(::nxpctl::protocol::WorkerState::Running);
            u64 uptime_seconds = 0;
            u32 sync_countdown = 0;

            /*
             * The timed wait is both the worker's idle cadence and its
             * cooperative shutdown point. Future work must remain bounded
             * and return to this wait regularly.
             */
            while (!os::TimedWaitEvent(
                std::addressof(g_worker_stop_event),
                TimeSpan::FromNanoSeconds(WorkerPollIntervalNs))) {
                ++g_worker_heartbeat;
                ++uptime_seconds;
                if (sync_countdown == 0) {
                    if (::nxpctl::sync::IsConfigured()) {
                        ::nxpctl::sync::RunCycle(g_policy_store, uptime_seconds);
                    }
                    sync_countdown = 60;
                }
                --sync_countdown;
            }

            g_worker_state = static_cast<u32>(::nxpctl::protocol::WorkerState::Stopped);
        }

        void StartWorkerThread() {
            AMS_ASSERT(!g_worker_thread_created);

            g_worker_state = static_cast<u32>(::nxpctl::protocol::WorkerState::Starting);
            g_worker_heartbeat = 0;

            R_ABORT_UNLESS(os::CreateThread(
                std::addressof(g_worker_thread),
                WorkerThreadMain,
                nullptr,
                g_worker_stack,
                sizeof(g_worker_stack),
                WorkerThreadPriority,
                3));

            os::SetThreadNamePointer(
                std::addressof(g_worker_thread),
                "ams.NxPctlManager.Worker");

            g_worker_thread_created = true;
            os::StartThread(std::addressof(g_worker_thread));
        }

        void WorkerSupervisorThreadMain(void *) {
            ::nxpctl::protocol::WorkerRestartBudget restart_budget{};

            while (!os::TimedWaitEvent(
                std::addressof(g_supervisor_stop_event),
                TimeSpan::FromNanoSeconds(WorkerPollIntervalNs))) {
                if (!g_worker_thread_created || !os::TryWaitThread(std::addressof(g_worker_thread))) {
                    continue;
                }

                /* Reap an unexpectedly terminated worker before restarting it. */
                os::DestroyThread(std::addressof(g_worker_thread));
                g_worker_thread_created = false;

                if (!restart_budget.TryConsume()) {
                    g_worker_state = static_cast<u32>(::nxpctl::protocol::WorkerState::Failed);
                    break;
                }

                g_worker_restart_count = restart_budget.restart_count;
                StartWorkerThread();
            }
        }

        void StartSupervisorThread() {
            AMS_ASSERT(!g_supervisor_thread_created);

            R_ABORT_UNLESS(os::CreateThread(
                std::addressof(g_supervisor_thread),
                WorkerSupervisorThreadMain,
                nullptr,
                g_supervisor_stack,
                sizeof(g_supervisor_stack),
                SupervisorThreadPriority,
                3));

            os::SetThreadNamePointer(
                std::addressof(g_supervisor_thread),
                "ams.NxPctlManager.Supervisor");

            g_supervisor_thread_created = true;
            os::StartThread(std::addressof(g_supervisor_thread));
        }

        void PersistUsageSnapshot(u64 monotonic_seconds) {
            if (!g_usage_storage_ready || !g_usage_tracker.HasBaseline()) return;
            const s64 day_key = g_usage_tracker.GetLocalDayKey();
            const u64 active_seconds = g_usage_tracker.GetActiveSeconds();
            if (day_key < 0 || active_seconds > ::nxpctl::usage::MaxPersistedActiveSeconds)
                return;
            const bool differs = !g_persisted_usage.valid ||
                g_persisted_usage.local_day_key != day_key ||
                g_persisted_usage.active_seconds != active_seconds;
            if (!differs) {
                g_usage_persist_dirty = false;
                return;
            }
            g_usage_persist_dirty = true;
            if (g_last_usage_persist_seconds != 0 &&
                monotonic_seconds >= g_last_usage_persist_seconds &&
                monotonic_seconds - g_last_usage_persist_seconds < 60)
                return;

            ::nxpctl::usage::Snapshot snapshot{};
            snapshot.local_day_key = day_key;
            snapshot.active_seconds = active_seconds;
            snapshot.valid = true;
            g_last_usage_persist_seconds = monotonic_seconds;
            if (::nxpctl::usage_store::Save(snapshot)) {
                g_persisted_usage = snapshot;
                g_usage_persist_dirty = false;
            }
        }

        void UsageMonitorThreadMain(void *) {
            g_usage_monitor_state = 1;

            while (!os::TimedWaitEvent(
                std::addressof(g_usage_monitor_stop_event),
                TimeSpan::FromNanoSeconds(WorkerPollIntervalNs))) {
                const u64 monotonic_seconds = GetMonotonicSeconds();
                if (monotonic_seconds == 0) continue;

                s64 local_day_key = 0;
                if (!::nxpctl::sync::GetTrustedLocalDayKey(
                        monotonic_seconds, std::addressof(local_day_key))) {
                    continue;
                }

                if (!g_usage_tracker.HasBaseline()) {
                    s64 restored_day = 0;
                    u64 restored_seconds = 0;
                    if (!::nxpctl::usage::ReconcileSnapshot(
                            g_persisted_usage, local_day_key,
                            std::addressof(restored_day),
                            std::addressof(restored_seconds))) {
                        // The authenticated clock is behind the stored day.
                        // Wait for a trustworthy clock instead of erasing usage.
                        continue;
                    }
                    if (!g_usage_tracker.Restore(
                            restored_day, restored_seconds, monotonic_seconds)) {
                        continue;
                    }
                    g_usage_persist_dirty =
                        !g_persisted_usage.valid ||
                        g_persisted_usage.local_day_key != restored_day ||
                        g_persisted_usage.active_seconds != restored_seconds;
                }

                const u64 previous_active_seconds = g_usage_tracker.GetActiveSeconds();
                os::ProcessId application_pid{};
                const ams::Result query_result =
                    pgl::GetRunningApplicationProcessId(std::addressof(application_pid));

                bool application_active = false;
                if (R_SUCCEEDED(query_result)) {
                    application_active = true;
                } else if (query_result.GetInnerValue() != NoRunningApplicationResult) {
                    // A service/IPC failure is unknown state, not evidence of
                    // an idle console. Skip this sample; the tracker rejects
                    // an excessive gap rather than inventing playtime.
                    continue;
                }

                const auto observe_result = g_usage_tracker.Observe(
                    application_active, monotonic_seconds, local_day_key);
                if (observe_result == ::nxpctl::usage::ObserveResult::DayRolledOver ||
                    g_usage_tracker.GetActiveSeconds() != previous_active_seconds) {
                    g_usage_persist_dirty = true;
                }
                g_usage_active_seconds = g_usage_tracker.GetActiveSeconds();
                g_usage_daily_minutes = g_usage_tracker.GetDailyUsageMinutes();
                if (g_usage_persist_dirty)
                    PersistUsageSnapshot(monotonic_seconds);
            }
            g_usage_monitor_state = 0;
        }

        void StartUsageMonitorThread() {
            if (!g_pgl_initialized || g_usage_monitor_thread_created) return;

            const ams::Result create_result = os::CreateThread(
                std::addressof(g_usage_monitor_thread),
                UsageMonitorThreadMain,
                nullptr,
                g_usage_monitor_stack,
                sizeof(g_usage_monitor_stack),
                UsageMonitorThreadPriority,
                3);
            if (R_FAILED(create_result)) {
                g_usage_monitor_state = 0;
                return;
            }

            os::SetThreadNamePointer(
                std::addressof(g_usage_monitor_thread),
                "ams.NxPctlManager.Usage");
            g_usage_monitor_thread_created = true;
            os::StartThread(std::addressof(g_usage_monitor_thread));
        }

        void StopUsageMonitorThread() {
            if (!g_usage_monitor_thread_created) return;
            os::SignalEvent(std::addressof(g_usage_monitor_stop_event));
            os::WaitThread(std::addressof(g_usage_monitor_thread));
            os::DestroyThread(std::addressof(g_usage_monitor_thread));
            g_usage_monitor_thread_created = false;
            g_usage_monitor_state = 0;
            PersistUsageSnapshot(GetMonotonicSeconds());
        }

        void StopSupervisorThread() {
            if (!g_supervisor_thread_created) {
                return;
            }

            os::SignalEvent(std::addressof(g_supervisor_stop_event));
            os::WaitThread(std::addressof(g_supervisor_thread));
            os::DestroyThread(std::addressof(g_supervisor_thread));
            g_supervisor_thread_created = false;
        }

        void StopWorkerThread() {
            if (!g_worker_thread_created) {
                return;
            }

            g_worker_state = static_cast<u32>(::nxpctl::protocol::WorkerState::Stopping);
            os::SignalEvent(std::addressof(g_worker_stop_event));
            os::WaitThread(std::addressof(g_worker_thread));
            os::DestroyThread(std::addressof(g_worker_thread));
            g_worker_thread_created = false;
        }


    }

    namespace nxpctl {

        class RuntimeService {
            public:
                Result GetRuntimeStatus(
                    sf::Out<u32> out_protocol_version,
                    sf::Out<u32> out_worker_state,
                    sf::Out<u32> out_worker_heartbeat,
                    sf::Out<u32> out_worker_restart_count,
                    sf::Out<u64> out_policy_revision) {
                    *out_protocol_version = ::nxpctl::protocol::RuntimeProtocolVersion;
                    *out_worker_state = g_worker_state;
                    *out_worker_heartbeat = g_worker_heartbeat;
                    *out_worker_restart_count = g_worker_restart_count;
                    *out_policy_revision = g_policy_store.GetRevision();
                    R_SUCCEED();
                }

                Result ConfigureEnrollment(sf::InBuffer request) {
                    if (request.GetSize() != sizeof(::nxpctl::protocol::ProvisioningRequest)) {
                        R_THROW(0xD401);
                    }
                    ::nxpctl::protocol::ProvisioningRequest config{};
                    std::memcpy(&config, request.GetPointer(), sizeof(config));
                    if (!::nxpctl::protocol::IsValidProvisioningRequest(config)) {
                        WipeProvisioningRequest(config);
                        R_THROW(0xD402);
                    }
                    if (::nxpctl::sync::IsEnrolled()) {
                        WipeProvisioningRequest(config);
                        R_THROW(0xD403);
                    }
                    ::nxpctl::sync::Configure(config);
                    WipeProvisioningRequest(config);
                    R_SUCCEED();
                }

                Result GetUsageStatus(
                    sf::Out<u32> out_monitor_state,
                    sf::Out<u64> out_active_seconds,
                    sf::Out<u32> out_daily_minutes) {
                    *out_monitor_state = g_usage_monitor_state;
                    *out_active_seconds = g_usage_active_seconds;
                    *out_daily_minutes = g_usage_daily_minutes;
                    R_SUCCEED();
                }
        };

        static_assert(impl::IsIRuntime<RuntimeService>);

    }

    namespace {

        using RuntimeServerOptions = sf::hipc::DefaultServerManagerOptions;

        constexpr sm::ServiceName RuntimeServiceName = sm::ServiceName::Encode(::nxpctl::protocol::ServiceName);
        constexpr size_t RuntimeMaxSessions = 4;

        sf::hipc::ServerManager<1, RuntimeServerOptions, RuntimeMaxSessions> g_runtime_server_manager;
        constinit sf::UnmanagedServiceObject<nxpctl::impl::IRuntime, nxpctl::RuntimeService> g_runtime_service;

    }

    namespace init {

        void InitializeSystemModule() {
            /* Connect to the Horizon service manager. */
            R_ABORT_UNLESS(sm::Initialize());

            /* Refuse to run against an incompatible libstratosphere API. */
            ams::CheckApiVersion();

            /* Initialize the cooperative worker shutdown event. */
            os::InitializeEvent(
                std::addressof(g_worker_stop_event),
                false,
                os::EventClearMode_AutoClear);
            os::InitializeEvent(
                std::addressof(g_supervisor_stop_event),
                false,
                os::EventClearMode_AutoClear);
            os::InitializeEvent(
                std::addressof(g_usage_monitor_stop_event),
                false,
                os::EventClearMode_AutoClear);

            // PGL is the supported application-process query interface on
            // Horizon 10.0.0+. It is read-only here; no process is modified.
            if (hos::GetVersion() >= hos::Version_10_0_0) {
                g_pgl_initialized = R_SUCCEEDED(pgl::Initialize());
            }

            /* Restore encrypted device credentials before the worker starts. */
            const bool storage_available = ::nxpctl::sync::Initialize();
            g_usage_storage_ready = storage_available;
            g_persisted_usage = {};
            if (storage_available)
                (void)::nxpctl::usage_store::Load(std::addressof(g_persisted_usage));
        }

        void FinalizeSystemModule() {
            /* Stop supervision first so an intentional shutdown is not restarted. */
            StopSupervisorThread();
            StopWorkerThread();
            StopUsageMonitorThread();
            if (g_pgl_initialized) {
                pgl::Finalize();
                g_pgl_initialized = false;
            }
            ::nxpctl::sync::Clear();
            ::nxpctl::sync::Finalize();
            ::nxpctl::net::Finalize();
            os::FinalizeEvent(std::addressof(g_usage_monitor_stop_event));
            os::FinalizeEvent(std::addressof(g_supervisor_stop_event));
            os::FinalizeEvent(std::addressof(g_worker_stop_event));
        }

        void Startup() {
            /* Startup hooks remain intentionally free of policy side effects. */
        }

    }

    void Main() {
        os::SetThreadNamePointer(
            os::GetCurrentThread(),
            "ams.NxPctlManager.Main");

        StartWorkerThread();
        StartSupervisorThread();
        StartUsageMonitorThread();

        /*
         * HIPC exposes runtime diagnostics plus a narrowly-scoped enrollment
         * bootstrap. It accepts only bounded host/device/code data; it has no
         * general-purpose policy mutation or enforcement command.
         */
        R_ABORT_UNLESS(g_runtime_server_manager.RegisterObjectForServer(
            g_runtime_service.GetShared(),
            RuntimeServiceName,
            RuntimeMaxSessions));

        g_runtime_server_manager.LoopProcess();
    }

}
