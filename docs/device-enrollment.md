# First-time Switch enrollment

Device management and policy configuration stay in the backend dashboard.
The small `nxpctl-bootstrap.nro` utility is only a one-shot HIPC bridge: it
has no settings screen, no Borealis dependency and no policy-management
features. It reads a human-readable configuration file from the SD-card root
and submits the validated values to the resident sysmodule.

## 1. Create an invitation

1. Open the backend dashboard at `https://YOUR-HOSTNAME/admin`.
2. Sign in with `NXPCTL_ADMIN_TOKEN`.
3. Choose **Gerät registrieren**, enter a device ID and create the one-time
   enrollment code.
4. Keep the code private. It is valid only for the configured invitation
   lifetime and can be used once.

## 2. Create the SD-card config

On a trusted computer, from the repository root run:

```sh
python3 tools/provisioner/create_provisioning.py --output nxpctl.conf
```

Enter the backend API hostname without a scheme, port or path, the same device
ID used in the dashboard, and the one-time code. The code is requested without
echo and the output file is written with owner-only permissions.

The resulting file has this format:

```ini
# NX-Pctl-Manager one-time device enrollment
api_host=api.example.com
device_id=switch-01
enrollment_code=PASTE_THE_ONE_TIME_CODE_HERE
```

The three keys are mandatory and must each occur exactly once. Blank lines and
lines beginning with `#` or `;` are ignored. Unknown keys, duplicate keys,
empty values, quotes and malformed values are rejected. Do not add a URL
scheme, port or path to `api_host`.

Copy `nxpctl.conf` to the root of the Switch SD card. The enrollment code is
plaintext in this file; do not share it or leave it on untrusted storage.

## 3. Submit it to the sysmodule

1. Ensure the headless sysmodule is installed and running under the supported
   Atmosphère build.
2. Launch `nxpctl-bootstrap.nro` once from hbmenu.
3. The helper parses and validates `nxpctl.conf`, then sends the fixed-size
   request to the `nxpctl` HIPC service. On acceptance it deletes
   `nxpctl.conf` and writes a non-secret result to
   `nxpctl-provision.status` on the SD-card root.
4. The sysmodule performs enrollment asynchronously on its next network cycle.
   Confirm the device appears in the dashboard and begins reporting heartbeats.

A successful helper exit means only that the sysmodule accepted the local
bootstrap request. It does not mean the backend enrollment has completed.
Keep the Switch connected until the dashboard shows the device.

If the HIPC service is unavailable or rejects the request, the config remains
so the issue can be corrected and retried. If enrollment fails after HIPC
acceptance, create a new invitation and config file; do not reuse an invitation
that the backend may already have consumed.

## 4. Build the helper

The CI workflow builds the NRO alongside the experimental NSP. For a local
build, initialize the pinned libnx submodule and run the project build script
first, then:

```sh
make -C tools/provisioner
```

The resulting file is `tools/provisioner/nxpctl-bootstrap.nro`.

## Security and limitations

The helper never receives or stores the administrator token, TOTP secret,
device credential or access token. It handles only the one-time invitation
code and erases the config and in-memory buffers after local HIPC acceptance.
Deleting a file from FAT does not guarantee forensic erasure; the invitation
is therefore short-lived and single-use.

The helper and HIPC ABI still require validation on a physical console. This
config-file workflow does not change the sysmodule's enforcement boundary and
does not make the project tamper-proof.
