# Resident Probe Test

This probe is for the first feasibility milestone on Atmosphere 1.11.1 / Horizon 22.1.0E. It is not a lock: it does not block HOME, inspect titles, access parental-control services, or change system settings.

## Build

From the repository root, run `make probe`. The resulting `nx_pctl_lock_probe.nsp` is in `feasibility/nx_pctl_lock_probe/`. The build does not install it or create a boot flag.

## Manual Test

1. Back up the SD card and confirm that you can remove files from it using your recovery path.
2. Copy the NSP to `sdmc:/atmosphere/contents/01000000000001F0/exefs.nsp`.
3. Create the empty flag `sdmc:/atmosphere/contents/01000000000001F0/flags/boot2.flag`.
4. Boot Atmosphere and wait at least 20 seconds.
5. Close the Homebrew Menu/NRO, then check `sdmc:/nx_pctl_resident_probe.txt` from a computer. The `heartbeat` value should continue increasing every 10 seconds and eventually read `12/12` with `status=complete`.

The probe exits after 120 seconds. A changing heartbeat after closing the NRO demonstrates that this separate process continued running; it does not demonstrate qlaunch integration or enforcement.

## Rollback

Delete `sdmc:/atmosphere/contents/01000000000001F0/flags/boot2.flag` and `exefs.nsp`, then reboot. If the console cannot boot normally, power it off and remove those two files from the SD card using a computer before trying again. The probe creates or modifies only `sdmc:/nx_pctl_resident_probe.txt` in addition to its own install files.

## Remaining Milestone 1 Tests

The probe does not test title-state observation, a lock surface, HOME/B dismissal, sleep/wake, reboot-state policy, authenticator recovery, or safe uninstall of an enforcing service. Do not mark the system-wide lock as implemented until those are separately designed and verified on-device.