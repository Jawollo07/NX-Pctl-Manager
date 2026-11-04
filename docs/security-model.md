# Security model and trust boundaries

## Authority

The server is authoritative for durable policies, schedules, usage and audit history. The Switch trusts only responses that pass AES-128-GCM authentication, schema, device-binding, trusted timestamp and revision checks. HTTP metadata remains visible; message bodies are encrypted end-to-end. WSS is not part of the current implementation.

## Assets

- Per-device identity and credential.
- Server-issued policy and policy revision.
- Parent authentication/recovery material.
- Usage event integrity and sequence state.
- Integrity and availability of the Atmosphere module and enforcement adapters.

## Offline semantics

- Network interruption while the module remains alive: enforce the last verified policy in RAM.
- Module restart or power loss: RAM policy is lost; require authenticated bootstrap before unrestricted operation.
- If policy expiry occurs offline, do not relax restrictions merely because the expiry elapsed. Continue enforcing the last verified policy until a fresh authenticated snapshot is obtained, subject to a documented maximum offline interval if the product later adopts one.
- Revocation cannot reach a disconnected console until reconnect. This is an inherent limit of offline enforcement.

## Persistent credential

Unattended device authentication requires a persistent unique credential. This is not a local policy store. The SD card is user-modifiable and must not be treated as a secure secret vault. Validate the available storage/provisioning options against the target Atmosphere build and document the physical-access threat model.

## Mandatory controls

- End-to-end AES-128-GCM encryption/authentication for every application request and response body; no plaintext API fallback in normal operation.
- Unique per-device credentials; no embedded shared secret.
- Short-lived access tokens, server-side credential revocation and rotation.
- Strict input bounds and rejection of malformed/oversized payloads.
- Monotonic policy revisions and event sequence numbers.
- Bounded request/response buffers and bounded retries. Exponential reconnect/backoff and event batching on the Switch remain to be implemented.
- No arbitrary remote code, shell, filesystem or raw IPC command execution.
- Redact credentials, tokens, PINs, TOTP secrets and enrollment codes from logs.
- Fail safely on unsupported firmware and unknown enforcement capability.
