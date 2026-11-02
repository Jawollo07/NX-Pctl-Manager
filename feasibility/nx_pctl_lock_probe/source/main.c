#include <stdio.h>

#include <switch.h>

#define INNER_HEAP_SIZE 0x80000
#define HEARTBEAT_COUNT 12
#define HEARTBEAT_INTERVAL_NS 10000000000ULL

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void)
{
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

static bool write_heartbeat(unsigned count)
{
    FILE* file = fopen("sdmc:/nx_pctl_resident_probe.txt", "w");
    if (!file)
        return false;

    const int written = fprintf(file,
        "probe=nx_pctl_lock_probe\n"
        "heartbeat=%u/%u\n"
        "status=%s\n",
        count, HEARTBEAT_COUNT, count == HEARTBEAT_COUNT ? "complete" : "running");
    const int close_result = fclose(file);
    return written >= 0 && close_result == 0;
}

int main(void)
{
    if (R_FAILED(smInitialize()))
        return 1;

    if (R_FAILED(fsInitialize())) {
        smExit();
        return 2;
    }

    if (R_FAILED(fsdevMountSdmc())) {
        fsExit();
        smExit();
        return 3;
    }

    for (unsigned count = 0; count <= HEARTBEAT_COUNT; ++count) {
        if (!write_heartbeat(count))
            break;
        if (count < HEARTBEAT_COUNT)
            svcSleepThread(HEARTBEAT_INTERVAL_NS);
    }

    fsdevUnmountAll();
    fsExit();
    smExit();
    return 0;
}