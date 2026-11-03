/*
 * NX-Pctl-Manager — headless Stratosphere sysmodule runtime.
 *
 * Project-authored code, licensed under GPL-2.0-or-later.
 * This phase establishes a resident worker and an explicit stop signal.
 * It does not yet enforce policies, inspect titles, mutate PCTL state,
 * access persistent storage, or communicate over the network.
 */
#include <stratosphere.hpp>

namespace ams {

    namespace {

        constexpr s32 WorkerThreadPriority = 50;
        constexpr size_t WorkerStackSize = 0x4000;
        constexpr s64 WorkerPollIntervalNs = 1'000'000'000LL;

        enum WorkerState : u32 {
            WorkerState_Stopped = 0,
            WorkerState_Starting = 1,
            WorkerState_Running = 2,
            WorkerState_Stopping = 3,
        };

        alignas(os::ThreadStackAlignment) u8 g_worker_stack[WorkerStackSize];
        os::ThreadType g_worker_thread;
        os::EventType g_worker_stop_event;
        util::Atomic<u32> g_worker_state;
        util::Atomic<u32> g_worker_heartbeat;
        bool g_worker_thread_created = false;

        void WorkerThreadMain(void *) {
            g_worker_state = WorkerState_Running;

            /*
             * The timed wait is both the worker's idle cadence and its
             * cooperative shutdown point. Future policy/network work must
             * remain bounded and return to this wait regularly.
             */
            while (!os::TimedWaitEvent(
                std::addressof(g_worker_stop_event),
                TimeSpan::FromNanoSeconds(WorkerPollIntervalNs))) {
                ++g_worker_heartbeat;
            }

            g_worker_state = WorkerState_Stopped;
        }

        void StartWorkerThread() {
            AMS_ASSERT(!g_worker_thread_created);

            g_worker_state = WorkerState_Starting;
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

        void StopWorkerThread() {
            if (!g_worker_thread_created) {
                return;
            }

            g_worker_state = WorkerState_Stopping;
            os::SignalEvent(std::addressof(g_worker_stop_event));
            os::WaitThread(std::addressof(g_worker_thread));
            os::DestroyThread(std::addressof(g_worker_thread));
            g_worker_thread_created = false;
        }

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
        }

        void FinalizeSystemModule() {
            StopWorkerThread();
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

        /*
         * The main thread remains resident. The worker is deliberately
         * isolated so future IPC handling can be added here without blocking
         * policy/network maintenance.
         */
        for (;;) {
            svcSleepThread(86'400'000'000'000LL);
        }
    }

}
