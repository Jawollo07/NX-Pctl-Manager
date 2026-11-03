/*
 * NX-Pctl-Manager — headless Stratosphere sysmodule runtime.
 *
 * Project-authored code, licensed under GPL-2.0-or-later.
 * This phase provides a resident worker, local HIPC provisioning/status service,
 * and encrypted HTTP policy synchronization. It does not yet enforce policies,
 * inspect running titles, mutate PCTL state, or access persistent storage.
 */
#include <stratosphere.hpp>
#include <cstring>
#include "../../protocol/runtime_status.hpp"
#include "../../protocol/worker_supervision.hpp"
#include "../../protocol/policy.hpp"
#include "../../protocol/provisioning.hpp"
#include "secure_sync.hpp"
#include "http_transport.hpp"

#define AMS_NXPCTL_I_RUNTIME_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 65000, Result, GetRuntimeStatus, \
        (sf::Out<u32> out_protocol_version, sf::Out<u32> out_worker_state, sf::Out<u32> out_worker_heartbeat, sf::Out<u32> out_worker_restart_count, sf::Out<u64> out_policy_revision), \
        (out_protocol_version, out_worker_state, out_worker_heartbeat, out_worker_restart_count, out_policy_revision)) \
    AMS_SF_METHOD_INFO(C, H, 65001, Result, ConfigureEnrollment, \
        (sf::InBuffer request), (request))

AMS_SF_DEFINE_INTERFACE(ams::nxpctl::impl, IRuntime, AMS_NXPCTL_I_RUNTIME_INTERFACE_INFO, 0x00000000)

namespace ams {

    namespace {

        constexpr s32 WorkerThreadPriority = 30;
        constexpr size_t WorkerStackSize = 0x8000;
        constexpr s32 SupervisorThreadPriority = 31;
        constexpr size_t SupervisorStackSize = 0x4000;
        constexpr s64 WorkerPollIntervalNs = 1'000'000'000LL;

        alignas(os::ThreadStackAlignment) u8 g_worker_stack[WorkerStackSize];
        alignas(os::ThreadStackAlignment) u8 g_supervisor_stack[SupervisorStackSize];
        os::ThreadType g_worker_thread;
        os::ThreadType g_supervisor_thread;
        os::EventType g_worker_stop_event;
        os::EventType g_supervisor_stop_event;
        util::Atomic<u32> g_worker_state;
        util::Atomic<u32> g_worker_heartbeat;
        util::Atomic<u32> g_worker_restart_count;
        ::nxpctl::policy::PolicyStore g_policy_store;
        bool g_worker_thread_created = false;
        bool g_supervisor_thread_created = false;

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
            /* Restore encrypted device credentials before the worker starts. */
            (void)::nxpctl::sync::Initialize();
        }

        void FinalizeSystemModule() {
            /* Stop supervision first so an intentional shutdown is not restarted. */
            StopSupervisorThread();
            StopWorkerThread();
            ::nxpctl::sync::Clear();
            ::nxpctl::sync::Finalize();
            ::nxpctl::net::Finalize();
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
