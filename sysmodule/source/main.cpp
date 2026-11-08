/*
 * NX-Pctl-Manager — headless Stratosphere sysmodule runtime.
 *
 * Project-authored code, licensed under GPL-2.0-or-later.
 * This phase provides a resident worker, local HIPC provisioning/status service,
 * encrypted HTTP policy synchronization, global application-activity monitoring,
 * and global policy enforcement through PGL process termination. The running PID
 * is used transiently and never retained; no title IDs or PCTL state are used.
 * Device credentials are persisted separately by secure sync.
 */
#include <stratosphere.hpp>
#include <stratosphere/pgl/pgl_shell_api.hpp>
#include <cstring>
#include "../../protocol/runtime_status.hpp"
#include "../../protocol/worker_supervision.hpp"
#include "../../protocol/policy.hpp"
#include "../../protocol/policy_evaluator.hpp"
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
        (out_monitor_state, out_active_seconds, out_daily_minutes)) \
    AMS_SF_METHOD_INFO(C, H, 65003, Result, GetPolicyDecision, \
        (sf::Out<u32> out_valid, sf::Out<u32> out_allowed, sf::Out<u32> out_reason), \
        (out_valid, out_allowed, out_reason)) \
    AMS_SF_METHOD_INFO(C, H, 65004, Result, GetEnforcementStatus, \
        (sf::Out<u32> out_state, sf::Out<u32> out_termination_attempts, sf::Out<u32> out_event_monitor_state), \
        (out_state, out_termination_attempts, out_event_monitor_state))

AMS_SF_DEFINE_INTERFACE(ams::nxpctl::impl, IRuntime, AMS_NXPCTL_I_RUNTIME_INTERFACE_INFO, 0x00000000)

namespace ams {

    namespace {

        constexpr s32 WorkerThreadPriority = 30;
        constexpr size_t WorkerStackSize = 0x8000;
        constexpr s32 SupervisorThreadPriority = 31;
        constexpr size_t SupervisorStackSize = 0x4000;
        constexpr s32 UsageMonitorThreadPriority = 32;
        constexpr size_t UsageMonitorStackSize = 0x4000;
        constexpr s32 PglEventThreadPriority = 31;
        constexpr size_t PglEventThreadStackSize = 0x4000;
        constexpr s64 WorkerPollIntervalNs = 1'000'000'000LL;
        constexpr u32 NoRunningApplicationResult = 0x6E4;

        alignas(os::ThreadStackAlignment) u8 g_worker_stack[WorkerStackSize];
        alignas(os::ThreadStackAlignment) u8 g_supervisor_stack[SupervisorStackSize];
        alignas(os::ThreadStackAlignment) u8 g_usage_monitor_stack[UsageMonitorStackSize];
        alignas(os::ThreadStackAlignment) u8 g_pgl_event_thread_stack[PglEventThreadStackSize];
        os::ThreadType g_worker_thread;
        os::ThreadType g_supervisor_thread;
        os::ThreadType g_usage_monitor_thread;
        os::ThreadType g_pgl_event_thread;
        os::EventType g_worker_stop_event;
        os::EventType g_supervisor_stop_event;
        os::EventType g_usage_monitor_stop_event;
        os::EventType g_pgl_event_stop_event;
        util::Atomic<u32> g_worker_state;
        util::Atomic<u32> g_worker_heartbeat;
        util::Atomic<u32> g_worker_restart_count;
        util::Atomic<u32> g_usage_monitor_state;
        util::Atomic<u32> g_pgl_event_monitor_state;
        util::Atomic<u64> g_usage_active_seconds;
        util::Atomic<s64> g_usage_local_day_key;
        util::Atomic<u32> g_usage_day_key_valid;
        util::Atomic<u32> g_usage_daily_minutes;
        util::Atomic<u32> g_policy_decision_valid;
        util::Atomic<u32> g_policy_decision_allowed;
        util::Atomic<u32> g_policy_decision_reason;
        // 0=inactive/unenrolled, 1=allowed, 2=blocked, 3=termination/query error,
        // 4=fail-closed because an enrolled device has no usable verified policy.
        util::Atomic<u32> g_enforcement_state;
        util::Atomic<u32> g_termination_attempts;
        ::nxpctl::policy::PolicyStore g_policy_store;
        ::nxpctl::usage::DailyUsageTracker g_usage_tracker;
        bool g_worker_thread_created = false;
        bool g_supervisor_thread_created = false;
        bool g_usage_monitor_thread_created = false;
        bool g_pgl_event_thread_created = false;
        util::Atomic<u32> g_pgl_initialized;
        bool g_usage_storage_ready = false;
        bool g_usage_storage_corrupt = false;
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
                        ::nxpctl::sync::RunCycle(
                            g_policy_store, uptime_seconds,
                            g_usage_day_key_valid
                                ? static_cast<s64>(g_usage_local_day_key)
                                : -1,
                            g_usage_active_seconds);
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

        void PersistUsageSnapshot(u64 monotonic_seconds, bool force = false) {
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
            if (!force && g_last_usage_persist_seconds != 0 &&
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

        void EnforceGlobalPolicy(
            bool policy_valid,
            bool allowed,
            bool enrolled,
            bool application_active,
            os::ProcessId application_pid) {
            if (!enrolled) {
                g_enforcement_state = 0;
                return;
            }

            if (policy_valid && allowed) {
                g_enforcement_state = 1;
                return;
            }

            if (!application_active) {
                g_enforcement_state = policy_valid ? 2 : 4;
                return;
            }

            // PGL's supported TerminateProcess path delegates to pm:shell.
            // The PID is used only for this call and is never stored or keyed.
            const ams::Result terminate_result = pgl::TerminateProcess(application_pid);
            ++g_termination_attempts;
            g_enforcement_state = R_SUCCEEDED(terminate_result)
                ? (policy_valid ? 2U : 4U)
                : 3U;
        }

        void EnforceStartedApplication(const pm::ProcessEventInfo &event_info) {
            if (event_info.GetProcessEvent() != pm::ProcessEvent::Started ||
                !::nxpctl::sync::IsEnrolled()) {
                return;
            }

            const bool policy_valid = g_policy_decision_valid != 0;
            const bool allowed = g_policy_decision_allowed != 0;
            if (policy_valid && allowed) return;

            // PGL events can include non-application processes. Only terminate
            // when the event PID matches the currently running application.
            os::ProcessId current_application_pid{};
            const ams::Result query_result =
                pgl::GetRunningApplicationProcessId(std::addressof(current_application_pid));
            if (R_FAILED(query_result) ||
                current_application_pid != event_info.process_id) {
                return;
            }

            const ams::Result terminate_result =
                pgl::TerminateProcess(event_info.process_id);
            ++g_termination_attempts;
            g_enforcement_state = R_SUCCEEDED(terminate_result)
                ? (policy_valid ? 2U : 4U)
                : 3U;
        }

        void PglEventMonitorThreadMain(void *) {
            g_pgl_event_monitor_state = 2;

            while (!os::TimedWaitEvent(
                std::addressof(g_pgl_event_stop_event),
                TimeSpan::FromNanoSeconds(WorkerPollIntervalNs))) {
                if (hos::GetVersion() < hos::Version_10_0_0 || !g_pgl_initialized) {
                    g_pgl_event_monitor_state = 2;
                    continue;
                }

                pgl::EventObserver observer;
                if (R_FAILED(pgl::CreateShellEvent(std::addressof(observer)))) {
                    g_pgl_event_monitor_state = 2;
                    continue;
                }

                os::SystemEvent shell_event;
                if (R_FAILED(observer.GetShellEvent(shell_event.GetBase()))) {
                    g_pgl_event_monitor_state = 2;
                    continue;
                }

                g_pgl_event_monitor_state = 1;
                while (!os::TimedWaitEvent(
                    std::addressof(g_pgl_event_stop_event),
                    TimeSpan::FromNanoSeconds(0))) {
                    if (!shell_event.TimedWait(
                        TimeSpan::FromNanoSeconds(WorkerPollIntervalNs))) {
                        continue;
                    }

                    // A single signal can represent multiple queued process events.
                    for (;;) {
                        pm::ProcessEventInfo event_info{};
                        if (R_FAILED(observer.GetShellEventInfo(std::addressof(event_info)))) {
                            break;
                        }
                        EnforceStartedApplication(event_info);
                    }
                }

                g_pgl_event_monitor_state = 0;
                return;
            }

            g_pgl_event_monitor_state = 0;
        }

        void StartPglEventMonitorThread() {
            if (g_pgl_event_thread_created) return;

            const ams::Result create_result = os::CreateThread(
                std::addressof(g_pgl_event_thread),
                PglEventMonitorThreadMain,
                nullptr,
                g_pgl_event_thread_stack,
                sizeof(g_pgl_event_thread_stack),
                PglEventThreadPriority,
                3);
            if (R_FAILED(create_result)) {
                g_pgl_event_monitor_state = 2;
                return;
            }

            os::SetThreadNamePointer(
                std::addressof(g_pgl_event_thread),
                "ams.NxPctlManager.PglEvents");
            g_pgl_event_thread_created = true;
            os::StartThread(std::addressof(g_pgl_event_thread));
        }

        void StopPglEventMonitorThread() {
            if (!g_pgl_event_thread_created) return;
            os::SignalEvent(std::addressof(g_pgl_event_stop_event));
            os::WaitThread(std::addressof(g_pgl_event_thread));
            os::DestroyThread(std::addressof(g_pgl_event_thread));
            g_pgl_event_thread_created = false;
            g_pgl_event_monitor_state = 0;
        }

        void UsageMonitorThreadMain(void *) {
            g_usage_monitor_state = g_usage_storage_corrupt ? 2U : 1U;

            while (!os::TimedWaitEvent(
                std::addressof(g_usage_monitor_stop_event),
                TimeSpan::FromNanoSeconds(WorkerPollIntervalNs))) {
                const u64 monotonic_seconds = GetMonotonicSeconds();
                if (monotonic_seconds == 0) continue;

                // PGL is available from Horizon 10.0.0 onward. Retry its
                // initialization if the boot-time connection was unavailable.
                if (hos::GetVersion() < hos::Version_10_0_0) {
                    if (::nxpctl::sync::IsEnrolled()) g_enforcement_state = 3;
                    continue;
                }
                if (!g_pgl_initialized) {
                    if (R_SUCCEEDED(pgl::Initialize())) {
                        g_pgl_initialized = true;
                    } else {
                        if (::nxpctl::sync::IsEnrolled()) g_enforcement_state = 3;
                        continue;
                    }
                }

                // Query PGL before clock/storage work so fail-closed conditions
                // can still terminate an application that is already running.
                os::ProcessId application_pid{};
                const ams::Result query_result =
                    pgl::GetRunningApplicationProcessId(std::addressof(application_pid));
                bool application_active = false;
                if (R_SUCCEEDED(query_result)) {
                    application_active = true;
                } else if (query_result.GetInnerValue() != NoRunningApplicationResult) {
                    if (::nxpctl::sync::IsEnrolled()) g_enforcement_state = 3;
                    continue;
                }

                const bool enrolled = ::nxpctl::sync::IsEnrolled();
                if (enrolled && (!g_usage_storage_ready || g_usage_storage_corrupt)) {
                    // A daily limit cannot be trusted without durable usage.
                    g_usage_day_key_valid = 0;
                    g_policy_decision_valid = 0;
                    EnforceGlobalPolicy(
                        false, false, enrolled, application_active, application_pid);
                    continue;
                }

                s64 local_day_key = 0;
                if (!::nxpctl::sync::GetTrustedLocalDayKey(
                        monotonic_seconds, std::addressof(local_day_key))) {
                    g_usage_day_key_valid = 0;
                    g_policy_decision_valid = 0;
                    EnforceGlobalPolicy(
                        false, false, enrolled, application_active, application_pid);
                    continue;
                }

                if (!g_usage_tracker.HasBaseline()) {
                    s64 restored_day = 0;
                    u64 restored_seconds = 0;
                    if (!::nxpctl::usage::ReconcileSnapshot(
                            g_persisted_usage, local_day_key,
                            std::addressof(restored_day),
                            std::addressof(restored_seconds))) {
                        // Never allow execution when the trusted clock regresses
                        // behind persisted usage; do not erase the saved counter.
                        g_policy_decision_valid = 0;
                        EnforceGlobalPolicy(
                            false, false, enrolled, application_active, application_pid);
                        continue;
                    }
                    if (!g_usage_tracker.Restore(
                            restored_day, restored_seconds, monotonic_seconds)) {
                        g_policy_decision_valid = 0;
                        EnforceGlobalPolicy(
                            false, false, enrolled, application_active, application_pid);
                        continue;
                    }
                    g_usage_persist_dirty =
                        !g_persisted_usage.valid ||
                        g_persisted_usage.local_day_key != restored_day ||
                        g_persisted_usage.active_seconds != restored_seconds;
                }

                const u64 previous_active_seconds = g_usage_tracker.GetActiveSeconds();
                const auto observe_result = g_usage_tracker.Observe(
                    application_active, monotonic_seconds, local_day_key);
                if (observe_result == ::nxpctl::usage::ObserveResult::DayRolledOver ||
                    g_usage_tracker.GetActiveSeconds() != previous_active_seconds) {
                    g_usage_persist_dirty = true;
                }
                g_usage_local_day_key = g_usage_tracker.GetLocalDayKey();
                g_usage_day_key_valid = 1;
                g_usage_active_seconds = g_usage_tracker.GetActiveSeconds();
                g_usage_daily_minutes = g_usage_tracker.GetDailyUsageMinutes();
                if (g_usage_persist_dirty)
                    PersistUsageSnapshot(monotonic_seconds);

                ::nxpctl::policy::Policy active_policy{};
                s64 trusted_local_time = 0;
                if (g_policy_store.CopyActive(&active_policy) &&
                    ::nxpctl::sync::GetTrustedLocalUnixTime(
                        monotonic_seconds, std::addressof(trusted_local_time))) {
                    const auto decision =
                        ::nxpctl::policy::EvaluateAtLocalUnixTime(
                            active_policy, trusted_local_time,
                            g_usage_daily_minutes, false, false);
                    g_policy_decision_allowed = decision.allowed ? 1 : 0;
                    g_policy_decision_reason = static_cast<u32>(decision.reason);
                    g_policy_decision_valid = 1;
                    EnforceGlobalPolicy(
                        true, decision.allowed, enrolled,
                        application_active, application_pid);
                } else {
                    g_policy_decision_valid = 0;
                    // An enrolled device without a current authenticated policy
                    // and trusted clock is fail-closed for application execution.
                    EnforceGlobalPolicy(
                        false, false, enrolled, application_active, application_pid);
                }
            }
            g_usage_monitor_state = 0;
        }

        void StartUsageMonitorThread() {
            if (g_usage_monitor_thread_created) return;

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
            PersistUsageSnapshot(GetMonotonicSeconds(), true);
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
                Result GetPolicyDecision(
                    sf::Out<u32> out_valid,
                    sf::Out<u32> out_allowed,
                    sf::Out<u32> out_reason) {
                    *out_valid = g_policy_decision_valid;
                    *out_allowed = g_policy_decision_allowed;
                    *out_reason = g_policy_decision_reason;
                    R_SUCCEED();
                }

                Result GetEnforcementStatus(
                    sf::Out<u32> out_state,
                    sf::Out<u32> out_termination_attempts,
                    sf::Out<u32> out_event_monitor_state) {
                    *out_state = g_enforcement_state;
                    *out_termination_attempts = g_termination_attempts;
                    *out_event_monitor_state = g_pgl_event_monitor_state;
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
            os::InitializeEvent(
                std::addressof(g_pgl_event_stop_event),
                false,
                os::EventClearMode_AutoClear);

            // PGL provides both application-process queries and the supported
            // TerminateProcess operation used for global policy enforcement.
            // This project does not initialize or call Nintendo PCTL.
            if (hos::GetVersion() >= hos::Version_10_0_0) {
                g_pgl_initialized = R_SUCCEEDED(pgl::Initialize());
            }

            /* Restore encrypted device credentials before the worker starts. */
            const bool storage_available = ::nxpctl::sync::Initialize();
            g_usage_storage_ready = storage_available;
            g_usage_storage_corrupt = false;
            g_persisted_usage = {};
            if (storage_available &&
                !::nxpctl::usage_store::Load(std::addressof(g_persisted_usage)) &&
                ::nxpctl::usage_store::HasAnyRecord()) {
                // Do not silently reset or overwrite a damaged saved counter.
                g_usage_storage_corrupt = true;
                g_usage_storage_ready = false;
            }
        }

        void FinalizeSystemModule() {
            /* Stop supervision first so an intentional shutdown is not restarted. */
            StopSupervisorThread();
            StopWorkerThread();
            StopUsageMonitorThread();
            StopPglEventMonitorThread();
            if (g_pgl_initialized) {
                pgl::Finalize();
                g_pgl_initialized = false;
            }
            ::nxpctl::sync::Clear();
            ::nxpctl::sync::Finalize();
            ::nxpctl::net::Finalize();
            os::FinalizeEvent(std::addressof(g_pgl_event_stop_event));
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
        StartPglEventMonitorThread();

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
