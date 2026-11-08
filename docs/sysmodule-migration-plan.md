# Headless sysmodule implementation plan

This plan is the authoritative implementation checklist for the current
backend-managed, global-only project. It explicitly excludes Nintendo PCTL,
per-title monitoring, and an on-console UI.

## Implemented in the feature branch

- [x] Remove the NRO/Borealis user interface from the active product tree.
- [x] Define a strict global-only policy schema and deterministic evaluator.
- [x] Implement global active-application time accounting and daily rollover.
- [x] Persist usage snapshots with authenticated encryption in sysmodule save
      data, using alternating records and sequence validation.
- [x] Implement bounded encrypted HTTP request/response envelopes with
      AES-GCM, HKDF key derivation and replay protection.
- [x] Implement device enrollment, authentication, short-lived access tokens,
      heartbeat, policy fetch/acknowledgement and event upload.
- [x] Implement backend-managed TOTP setup, verification, lockout, grant
      expiry and device revocation.
- [x] Implement backend admin API and browser dashboard.
- [x] Add HIPC runtime, usage, policy-decision, enforcement and connectivity
      diagnostics.
- [x] Add PGL process-start observer plus one-second accounting/enforcement
      fallback.
- [x] Add host tests and CI packaging of an experimental NSP.

## Remaining software and release gates

- [ ] Add cross-language fixtures covering enrollment, authentication,
      heartbeat, policy, parent-auth and error responses for both C++ and
      Python implementations.
- [ ] Add integration tests for all admin API routes, device revocation,
      re-enrollment and concurrent/replayed requests.
- [ ] Add a supported firmware/Atmosphère compatibility matrix and verify
      NPDM title ID, service permissions and capabilities against the exact
      pinned source.
- [x] Add a non-interactive HIPC bootstrap NRO and host-side payload generator
      for the one-time enrollment code. Physical transfer and console behavior
      still require hardware validation.
- [ ] Add documented key/database backup and restore automation.
- [ ] Add usage-history retention/cleanup and bounded event-log maintenance.
- [ ] Add release artifact checksum publication and versioned release notes.

## Mandatory physical-console validation

These checks cannot be completed by host CI:

- [ ] Verify sysmodule startup and shutdown on a real console.
- [ ] Verify global application detection, PGL event delivery and termination
      latency on each supported firmware.
- [ ] Verify accounting across HOME, application close/launch, user changes,
      sleep, wake, clock changes and midnight rollover.
- [ ] Verify restrictions after cold boot while offline and after server
      revocation/reconnect.
- [ ] Verify credential and usage save-data durability after abrupt power loss.
- [ ] Verify boot recovery, safe disable and uninstall before any enforcement
      deployment.

## Definition of done

The project is production-ready only after every software and hardware gate
above is complete. Until then, the NSP is an experimental build, not a
validated parental-control product. No PCTL service/API is part of the design.
