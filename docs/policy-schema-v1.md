# Policy JSON schema v1 (strict profile)

This document defines the exact JSON representation accepted by
`protocol/policy_json.hpp`. It is a transport-decoding contract, not an
authentication mechanism. A decoded policy is untrusted until the encrypted
transport verifies its integrity. The model contains only console-wide rules.

## Rules

- Maximum document size: 16 KiB.
- Maximum parser tokens: 512; maximum nesting depth: 8.
- Every field listed below is required exactly once. Unknown and duplicate
  fields are rejected.
- Object keys and string values must be printable ASCII and must not contain
  JSON escapes. This intentionally strict profile avoids ambiguous identifier
  normalization; do not send escaped or non-ASCII device identifiers.
- All numeric values are base-10 integers. Fractions and exponent notation are
  rejected. Booleans must be JSON `true` or `false`.
- `offline_behavior`: 0 = continue using the last verified global policy;
  1 = block all console usage while offline. No title-specific behavior exists.
- `global_daily_limit_minutes` and each weekday entry are 0–1440, or 65535
  for no limit / inherit. Weekday order is Monday through Sunday.
- `timezone_offset_minutes` is a fixed UTC offset in the range -840..840.
  IANA timezone identifiers and DST transitions are not part of schema v1.
- Quiet-hours minute values are 0..1439. Equal start/end is invalid when
  quiet hours are enabled. A start later than end spans midnight.
## Required fields

| Field | JSON type | Meaning |
|---|---|---|
| `protocol_version` | integer | Must be 1 |
| `schema_version` | integer | Must be 1 |
| `device_id` | ASCII string | Exact device binding |
| `revision` | unsigned integer | Must be newer than active revision |
| `issued_at` | integer | Unix seconds |
| `not_after` | integer | Unix seconds |
| `server_time` | integer | Server Unix seconds |
| `enabled` | boolean | Whether policy is enabled |
| `require_parent_authentication` | boolean | Require a currently valid backend-issued 15-minute parent-auth grant for global application execution |
| `require_parent_authentication` | boolean | Parent-auth requirement |
| `offline_behavior` | integer | Enum above |
| `timezone_offset_minutes` | integer | Fixed UTC offset |
| `global_daily_limit_minutes` | integer | Global daily budget |
| `weekday_limit_minutes` | array of 7 integers | Monday–Sunday overrides |
| `quiet_hours_enabled` | boolean | Enable quiet hours |
| `quiet_hours_start_minute` | integer | Minute after local midnight |
| `quiet_hours_end_minute` | integer | Minute after local midnight |

The parser performs structural and primitive-range decoding. The policy validator
then checks device binding, version compatibility, revision monotonicity,
timestamps, expiry, lifetime, enum ranges and schedule limits. Title-related
fields are unknown and rejected.

No signature, MAC, token or credential is represented by this JSON document.
Never log credentials or authentication envelopes. Do not publish decoded data
to `PolicyStore` until the transport has authenticated the device/server and
verified the complete message integrity.
