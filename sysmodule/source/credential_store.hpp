#pragma once

#include <stratosphere.hpp>
#include <cstddef>

namespace nxpctl::credential_store {

    struct CredentialData {
        char api_url[254]{};
        char device_id[65]{};
        char device_credential[129]{};
    };

    static_assert(sizeof(CredentialData) == 448);

    /* Mounts this sysmodule's own device save data and initializes the device key service. */
    bool Initialize();
    void Finalize();

    /* Only the long-lived device credential and routing identity are persisted. */
    bool Load(CredentialData *out);
    bool Save(const CredentialData &data);

}
