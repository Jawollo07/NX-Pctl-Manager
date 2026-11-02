/*
 * NX-Pctl-Manager — initial headless Stratosphere sysmodule skeleton.
 *
 * This file is project-authored and licensed under GPL-2.0-or-later.
 * It intentionally contains no policy enforcement, network client, IPC
 * endpoint, PCTL writes, filesystem access, or system hooks yet.
 */
#include <stratosphere.hpp>

namespace ams {

    namespace init {

        void InitializeSystemModule() {
            /* Connect to the Horizon service manager. */
            R_ABORT_UNLESS(sm::Initialize());

            /* Refuse to run against an incompatible libstratosphere API. */
            ams::CheckApiVersion();
        }

        void FinalizeSystemModule() {
            /* No additional resources are owned in this bootstrap phase. */
        }

        void Startup() {
            /* Phase 1: intentionally empty. */
        }

    }

    void Main() {
        os::SetThreadNamePointer(os::GetCurrentThread(),
                         "ams.NxPctlManager.Main");

        /*
         * Resident idle loop. Later phases replace this with bounded workers
         * and an explicit shutdown event. No title/process hooks are installed
         * by this skeleton.
         */
        for (;;) {
            svcSleepThread(1'000'000'000LL);
        }
    }

}
