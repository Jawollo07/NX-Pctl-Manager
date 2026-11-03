/*
 * NX-Pctl-Manager — headless Stratosphere sysmodule runtime.
 *
 * Project-authored code, licensed under GPL-2.0-or-later.
 * This phase provides a resident worker and a read-only local HIPC status
 * service. It does not enforce policies, inspect titles, mutate PCTL state,
 * access persistent storage, or communicate over the network.
 */
#include <stratosphere.hpp>
#include "../../protocol/runtime_status.hpp"
#include "../../protocol/worker_supervision.hpp"
#include "../../protocol/policy.hpp"

#define AMS_NXPCTL_I_RUNTIME_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 65000, Result, GetRuntimeStatus, \
        (sf::Out<u32> out_protocol_version, sf::Out<u32> out_worker_state, sf::Out<u32> out_worker_heartbeat, sf::Out<u32> out_worker_restart_count, sf::Out<u64> out_policy_revision), \
        (out_protocol_version, out_worker_state, out_worker_heartbeat, out_worker_restart_count, out_policy_revision))

AMS_SF_DEFINE_INTERFACE(ams::nxpctl::impl, IRuntime, AMS_NXPCTL_I_RUNTIME_INTERFACE_INFO, 0x00000000)

namespace ams {

    namespace {

        constexpr s32 WorkerThreadPriority = 30;
        constexpr size_t WorkerStackSize = 0x4000;
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

        void WorkerThreadMain(void *) {
            g_worker_state = static_cast<u32>(::nxpctl::protocol::WorkerState::Running);

            /*
             * The timed wait is both the worker's idle cadence and its
             * cooperative shutdown point. Future work must remain bounded
             * and return to this wait regularly.
             */
            while (!os::TimedWaitEvent(
                std::addressof(g_worker_stop_event),
                TimeSpan::FromNanoSeconds(WorkerPollIntervalNs))) {
                ++g_worker_heartbeat;
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
        }

        void FinalizeSystemModule() {
            /* Stop supervision first so an intentional shutdown is not restarted. */
            StopSupervisorThread();
            StopWorkerThread();
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
         * This HIPC endpoint is deliberately read-only. It exposes only
         * runtime diagnostics; policy mutation and enforcement are not
         * available through this protocol version.
         */
        R_ABORT_UNLESS(g_runtime_server_manager.RegisterObjectForServer(
            g_runtime_service.GetShared(),
            RuntimeServiceName,
            RuntimeMaxSessions));

        g_runtime_server_manager.LoopProcess();
    }

}
