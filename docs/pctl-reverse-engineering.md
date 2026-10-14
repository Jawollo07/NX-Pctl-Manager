# pctl sysmodule notes (fw 22.5.0)

These are the findings behind the play-timer code in `source/pctl_ops.{h,c}`. They come from the `pctl` sysmodule (title `010000000000002E`) of firmware 22.5.0. Its NSO was converted to an ELF loaded at base 0, so every address below is an offset into that image.

No firmware, keys or decompiled code are included in this repository.

## PlayTimerSettings (0x44 bytes, fw 21.0.0+)

Evidence:

- **pctl's own settings file.** Its JSON field-descriptor table sits at `0x13b030`. Each entry is `{name*, 0, handler*, flags}`. The fields appear in this order:
  - `enabled`, `mode`, `weekUsed`
  - then, for each day: `bedtimeEnabled`, `bedtimeAlarmTimeHour`, `bedtimeAlarmTimeMinute`, `bedtimeAlarmResetTimeHour`, `bedtimeAlarmResetTimeMinute`, `limitTimeEnabled`, `limitTime`
  - then a `weekly` array of the same per-day fields
- **The parser handlers store each field at a fixed offset.** Their target object has 2 extra leading bytes. Subtracting those gives the offsets in the table below. Examples:
  - `enabled` is stored at +0.
  - `mode` is stored at +1 as `value != 0`.
  - `weekUsed` is stored at +2.
  - Daily `bedtimeAlarmResetTimeHour` is stored at +7 and clamped to 23.
  - Daily `limitTime` is stored at +10 as a u16 clamped to 0xFFFF.
  - Weekly `limitTime` for day n is stored at +18+8n.
- **The server-sync serializer confirms the enum values of the header bytes.**
  - Byte 1 is `restrictionMode`, written by `0x5bd70`. Its string table at `0x13c300` maps 0 to `ALARM` and 1 to `FORCED_TERMINATION`.
  - Byte 2 is `timerMode`, written by `0x5bd30`. Its string table at `0x13c2e0` maps 0 to `DAILY` and 1 to `EACH_DAY_OF_THE_WEEK`.

| Offset | Size | Field |
|---|---|---|
| 0x00 | u8 | enabled |
| 0x01 | u8 | restrictionMode: 0 = ALARM (notify only), 1 = FORCED_TERMINATION (suspend software) |
| 0x02 | u8 | timerMode / weekUsed: 0 = DAILY (use `daily`), 1 = EACH_DAY_OF_THE_WEEK (use `weekly`) |
| 0x03 | u8 | reserved |
| 0x04 | 8 | daily day-setting |
| 0x0C | 8 × 7 | weekly day-settings, Sunday through Saturday |

Each day-setting is 8 bytes:

| Offset | Size | Field |
|---|---|---|
| +0 | u8 | bedtimeEnabled |
| +1 | u8 | bedtimeAlarmTimeHour |
| +2 | u8 | bedtimeAlarmTimeMinute |
| +3 | u8 | bedtimeAlarmResetTimeHour |
| +4 | u8 | bedtimeAlarmResetTimeMinute |
| +5 | u8 | limitTimeEnabled |
| +6 | u16 | limitTime, in minutes |

The pre-21 struct is 0x34 bytes, which is 4 + 8 × 6. Each day-setting there is 6 bytes, because it lacks the two reset-time bytes that fw 20 added together with `GetBedtimeAlarmResetTime*`.

## SetPlayTimerSettingsForDebug (cmd 195101)

Dispatch path:

1. The command dispatcher is `sub_1F00`. For command 195101 it branches to `loc_DA50`.
2. That code calls through vtable slot `+0x418`, which resolves to `0x2be20`.
3. `0x2be20` calls the implementation, `sub_2EDF0`.

Checks in `sub_2EDF0`:

- If the session lacks capability bit 3, it returns `0x10A8E`.
- If the settings manager is not ready (`qword_181000 == 0`), it returns `0x10E8E`.
- **If no PIN is registered, the write is skipped but the call still returns 0.** The check reads the byte at settings base +1953, which is the first PIN digit. The PIN check function uses the same storage at +1945 from a base 8 bytes higher.

When the write goes through, `sub_52EF0` stores the 0x44 bytes and **clears `enabled`**. It then:

1. Reads the current time from the network clock (`sub_36620`, then `sub_DD850`, then `sub_DDF20`). `sub_DDF20` reads the context at offset 0x80 of the time shared memory, which is the NetworkSystemClock.
2. Converts that time to a calendar date.
3. Calls `sub_57270`, which re-derives the timer state.

If the network clock cannot supply the time, the timer stays disabled. This is the root cause of issue #2: offline consoles, or consoles that block Nintendo's servers, never set that clock.

## Result 0x1088E (issue #5)

`sub_41550` is the PIN check used by UnlockRestrictionTemporarily. When no PIN is registered (`pin[0] == 0`), it returns `0x1088E` (module 142, description 132).

It returns `0xF08E` for a wrong PIN or a badly formed one. The input must be 4 to 8 ASCII digits followed by a NUL, so the buffer length is digits + 1.
