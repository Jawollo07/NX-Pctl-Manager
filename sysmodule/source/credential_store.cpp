#include "credential_store.hpp"
#include "secure_crypto.hpp"

#include <cstring>
#include <cstddef>
#include <limits>

namespace nxpctl::credential_store {
namespace {

    constexpr char MountName[] = "nxpctl";
    constexpr char Slot0Path[] = "nxpctl:/credential0.bin";
    constexpr char Slot1Path[] = "nxpctl:/credential1.bin";
    constexpr u32 RecordVersion = 1;

    /*
     * This is an application-owned key source, not a Nintendo title/content key.
     * SPL combines it with the console's device key ladder. The resulting AES
     * key is never written to storage.
     */
    constexpr spl::KeySource CredentialKeySource = {
        .data = {
            0x4e, 0x58, 0x50, 0x43, 0x54, 0x4c, 0x2d, 0x43,
            0x52, 0x45, 0x44, 0x2d, 0x56, 0x31, 0x00, 0x01
        }
    };

    #pragma pack(push, 1)
    struct EncryptedRecord {
        char magic[8];
        u32 version;
        u64 sequence;
        u8 nonce[crypto::GcmNonceSize];
        u8 ciphertext[sizeof(CredentialData)];
        u8 tag[crypto::GcmTagSize];
    };
    #pragma pack(pop)

    static_assert(offsetof(EncryptedRecord, ciphertext) == 32);
    static_assert(sizeof(EncryptedRecord) == 496);

    constexpr char RecordMagic[8] = {'N','X','P','C','R','D','0','1'};

    bool g_initialized = false;
    bool g_spl_initialized = false;
    bool g_mounted = false;

    void Wipe(void *data, size_t size) {
        volatile u8 *p = static_cast<volatile u8 *>(data);
        while (size--) *p++ = 0;
    }

    bool DeriveStorageKey(u8 out_key[crypto::Aes128KeySize]) {
        spl::AccessKey access_key{};
        u64 generation = 0;
        if (R_FAILED(spl::GetConfig(&generation, spl::ConfigItem::DeviceUniqueKeyGeneration)) ||
            generation > static_cast<u64>(std::numeric_limits<s32>::max())) {
            Wipe(&access_key, sizeof(access_key));
            return false;
        }

        const Result kek_result = spl::GenerateAesKek(
            &access_key, &CredentialKeySource, sizeof(CredentialKeySource),
            static_cast<s32>(generation), 1);
        if (R_FAILED(kek_result)) {
            Wipe(&access_key, sizeof(access_key));
            return false;
        }

        const Result key_result = spl::GenerateAesKey(
            out_key, crypto::Aes128KeySize, access_key,
            &CredentialKeySource, sizeof(CredentialKeySource));
        Wipe(&access_key, sizeof(access_key));
        return R_SUCCEEDED(key_result);
    }

    bool IsValidCredentialData(const CredentialData &data) {
        const auto length = [](const char *value, size_t capacity) {
            for (size_t i = 0; i < capacity; ++i) if (value[i] == '\0') return i;
            return capacity;
        };
        const size_t host_size = length(data.api_host, sizeof(data.api_host));
        const size_t id_size = length(data.device_id, sizeof(data.device_id));
        const size_t credential_size = length(data.device_credential, sizeof(data.device_credential));
        if (host_size == 0 || host_size >= sizeof(data.api_host) ||
            id_size == 0 || id_size >= sizeof(data.device_id) ||
            credential_size < 16 || credential_size >= sizeof(data.device_credential)) return false;

        for (size_t i = 0; i < host_size; ++i) {
            const char c = data.api_host[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
        }
        for (size_t i = 0; i < id_size; ++i) {
            const char c = data.device_id[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
        }
        for (size_t i = 0; i < credential_size; ++i) {
            const char c = data.device_credential[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        }
        return true;
    }

    const char *GetSlotPath(unsigned slot) {
        return slot == 0 ? Slot0Path : Slot1Path;
    }

    bool ReadSlot(unsigned slot, CredentialData *out, u64 *out_sequence) {
        EncryptedRecord record{};
        CredentialData candidate{};
        u8 key[crypto::Aes128KeySize]{};
        size_t bytes_read = 0;
        bool valid = false;
        fs::FileHandle file{};

        if (R_FAILED(fs::OpenFile(&file, GetSlotPath(slot), fs::OpenMode_Read))) {
            return false;
        }

        s64 file_size = 0;
        const Result size_result = fs::GetFileSize(&file_size, file);
        const Result read_result = R_SUCCEEDED(size_result) && file_size == static_cast<s64>(sizeof(record))
            ? fs::ReadFile(&bytes_read, file, 0, &record, sizeof(record), fs::ReadOption::None)
            : ResultSuccess();
        fs::CloseFile(file);

        if (R_SUCCEEDED(size_result) && file_size == static_cast<s64>(sizeof(record)) &&
            R_SUCCEEDED(read_result) && bytes_read == sizeof(record) &&
            std::memcmp(record.magic, RecordMagic, sizeof(RecordMagic)) == 0 &&
            record.version == RecordVersion && record.sequence != 0 &&
            DeriveStorageKey(key) &&
            crypto::Aes128GcmDecrypt(
                key, record.nonce,
                reinterpret_cast<const u8 *>(&record), offsetof(EncryptedRecord, ciphertext),
                record.ciphertext, sizeof(record.ciphertext), record.tag,
                reinterpret_cast<u8 *>(&candidate)) &&
            IsValidCredentialData(candidate)) {
            *out = candidate;
            *out_sequence = record.sequence;
            valid = true;
        }

        Wipe(key, sizeof(key));
        Wipe(&candidate, sizeof(candidate));
        Wipe(&record, sizeof(record));
        return valid;
    }

    bool WriteSlot(unsigned slot, const CredentialData &data, u64 sequence) {
        EncryptedRecord record{};
        u8 key[crypto::Aes128KeySize]{};
        bool success = false;
        const char *path = GetSlotPath(slot);

        std::memcpy(record.magic, RecordMagic, sizeof(RecordMagic));
        record.version = RecordVersion;
        record.sequence = sequence;
        crypto::GenerateNonce(record.nonce);

        if (!DeriveStorageKey(key) ||
            !crypto::Aes128GcmEncrypt(
                key, record.nonce,
                reinterpret_cast<const u8 *>(&record), offsetof(EncryptedRecord, ciphertext),
                reinterpret_cast<const u8 *>(&data), sizeof(data),
                record.ciphertext, record.tag)) {
            Wipe(key, sizeof(key));
            Wipe(&record, sizeof(record));
            return false;
        }

        /* Write the inactive slot; the previous valid slot remains recoverable. */
        (void)fs::DeleteFile(path);
        if (R_SUCCEEDED(fs::CreateFile(path, sizeof(record)))) {
            fs::FileHandle file{};
            if (R_SUCCEEDED(fs::OpenFile(&file, path, fs::OpenMode_Write))) {
                const Result write_result = fs::WriteFile(
                    file, 0, &record, sizeof(record), fs::WriteOption::Flush);
                fs::CloseFile(file);
                success = R_SUCCEEDED(write_result);
            }
            if (!success) (void)fs::DeleteFile(path);
        }

        Wipe(key, sizeof(key));
        Wipe(&record, sizeof(record));
        return success;
    }

}

bool Initialize() {
    if (g_initialized) return g_mounted;
    g_initialized = true;

    fs::InitializeForSystem();
    spl::InitializeForCrypto();
    g_spl_initialized = true;

    g_mounted = R_SUCCEEDED(fs::MountDeviceSaveData(MountName));
    return g_mounted;
}

void Finalize() {
    if (g_mounted) {
        fs::Unmount(MountName);
        g_mounted = false;
    }
    if (g_spl_initialized) {
        spl::Finalize();
        g_spl_initialized = false;
    }
    g_initialized = false;
}

bool Load(CredentialData *out) {
    if (out == nullptr || !g_mounted) return false;

    CredentialData candidates[2]{};
    u64 sequences[2]{};
    const bool valid0 = ReadSlot(0, &candidates[0], &sequences[0]);
    const bool valid1 = ReadSlot(1, &candidates[1], &sequences[1]);

    bool loaded = false;
    if (valid0 || valid1) {
        const unsigned selected = valid1 && (!valid0 || sequences[1] > sequences[0]) ? 1 : 0;
        *out = candidates[selected];
        loaded = true;
    }

    Wipe(candidates, sizeof(candidates));
    Wipe(sequences, sizeof(sequences));
    return loaded;
}

bool Save(const CredentialData &data) {
    if (!g_mounted || !IsValidCredentialData(data)) return false;

    CredentialData ignored{};
    u64 sequences[2]{};
    const bool valid0 = ReadSlot(0, &ignored, &sequences[0]);
    Wipe(&ignored, sizeof(ignored));
    const bool valid1 = ReadSlot(1, &ignored, &sequences[1]);
    Wipe(&ignored, sizeof(ignored));

    const u64 current = valid0 && valid1
        ? (sequences[0] > sequences[1] ? sequences[0] : sequences[1])
        : (valid0 ? sequences[0] : (valid1 ? sequences[1] : 0));
    if (current == std::numeric_limits<u64>::max()) {
        Wipe(sequences, sizeof(sequences));
        return false;
    }

    const u64 next = current + 1;
    const unsigned target = static_cast<unsigned>(next & 1);
    const bool saved = WriteSlot(target, data, next);
    Wipe(sequences, sizeof(sequences));
    return saved;
}

}
