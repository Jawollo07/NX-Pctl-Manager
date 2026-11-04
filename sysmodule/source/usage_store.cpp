#include "usage_store.hpp"
#include "secure_crypto.hpp"

#include <stratosphere.hpp>
#include <cstddef>
#include <cstring>
#include <limits>

namespace nxpctl::usage_store {
namespace {
    namespace fs = ams::fs;
    namespace hos = ams::hos;
    namespace spl = ams::spl;

    constexpr char Slot0Path[] = "nxpctl:/usage0.bin";
    constexpr char Slot1Path[] = "nxpctl:/usage1.bin";
    constexpr char RecordMagic[8] = {'N','X','P','U','S','G','0','1'};
    constexpr u32 RecordVersion = 1;

    constexpr spl::KeySource UsageKeySource = {
        .data = {
            0x4e, 0x58, 0x50, 0x43, 0x54, 0x4c, 0x2d, 0x55,
            0x53, 0x41, 0x47, 0x45, 0x2d, 0x56, 0x31, 0x01
        }
    };

    struct UsageData {
        s64 local_day_key;
        u64 active_seconds;
    };

#pragma pack(push, 1)
    struct EncryptedRecord {
        char magic[8];
        u32 version;
        u64 sequence;
        u8 nonce[crypto::GcmNonceSize];
        u8 ciphertext[sizeof(UsageData)];
        u8 tag[crypto::GcmTagSize];
    };
#pragma pack(pop)

    static_assert(offsetof(EncryptedRecord, ciphertext) == 32);
    static_assert(sizeof(EncryptedRecord) == 64);

    void Wipe(void* data, std::size_t size) {
        volatile u8* p = static_cast<volatile u8*>(data);
        while (size--) *p++ = 0;
    }

    bool DeriveKey(u8 out_key[crypto::Aes128KeySize]) {
        spl::AccessKey access_key{};
        u64 generation = 0;
        if (hos::GetVersion() >= hos::Version_5_0_0 &&
            R_FAILED(spl::GetConfig(&generation, spl::ConfigItem::DeviceUniqueKeyGeneration))) {
            Wipe(&access_key, sizeof(access_key));
            return false;
        }
        if (generation > static_cast<u64>(std::numeric_limits<s32>::max())) {
            Wipe(&access_key, sizeof(access_key));
            return false;
        }
        const auto kek = spl::GenerateAesKek(
            &access_key, &UsageKeySource, sizeof(UsageKeySource),
            static_cast<s32>(generation), 1);
        if (R_FAILED(kek)) {
            Wipe(&access_key, sizeof(access_key));
            return false;
        }
        const auto key = spl::GenerateAesKey(
            out_key, crypto::Aes128KeySize, access_key,
            &UsageKeySource, sizeof(UsageKeySource));
        Wipe(&access_key, sizeof(access_key));
        return R_SUCCEEDED(key);
    }

    const char* SlotPath(unsigned slot) {
        return slot == 0 ? Slot0Path : Slot1Path;
    }

    bool ReadSlot(unsigned slot, usage::Snapshot* out) {
        EncryptedRecord record{};
        UsageData data{};
        u8 key[crypto::Aes128KeySize]{};
        fs::FileHandle file{};
        size_t bytes_read = 0;
        bool valid = false;

        if (R_FAILED(fs::OpenFile(&file, SlotPath(slot), fs::OpenMode_Read)))
            return false;
        s64 file_size = 0;
        const ams::Result size_result = fs::GetFileSize(&file_size, file);
        ams::Result read_result = size_result;
        if (R_SUCCEEDED(size_result) && file_size == static_cast<s64>(sizeof(record))) {
            read_result = fs::ReadFile(
                &bytes_read, file, 0, &record, sizeof(record), fs::ReadOption::None);
        }
        fs::CloseFile(file);

        if (R_SUCCEEDED(size_result) && file_size == static_cast<s64>(sizeof(record)) &&
            R_SUCCEEDED(read_result) && bytes_read == sizeof(record) &&
            std::memcmp(record.magic, RecordMagic, sizeof(RecordMagic)) == 0 &&
            record.version == RecordVersion && record.sequence != 0 &&
            DeriveKey(key) &&
            crypto::Aes128GcmDecrypt(
                key, record.nonce,
                reinterpret_cast<const u8*>(&record),
                offsetof(EncryptedRecord, ciphertext),
                record.ciphertext, sizeof(record.ciphertext), record.tag,
                reinterpret_cast<u8*>(&data)) &&
            data.local_day_key >= 0 &&
            data.active_seconds <= usage::MaxPersistedActiveSeconds) {
            out->local_day_key = data.local_day_key;
            out->active_seconds = data.active_seconds;
            out->sequence = record.sequence;
            out->valid = true;
            valid = true;
        }
        Wipe(key, sizeof(key));
        Wipe(&data, sizeof(data));
        Wipe(&record, sizeof(record));
        return valid;
    }

    bool WriteSlot(unsigned slot, const usage::Snapshot& snapshot, u64 sequence) {
        EncryptedRecord record{};
        UsageData data{snapshot.local_day_key, snapshot.active_seconds};
        u8 key[crypto::Aes128KeySize]{};
        std::memcpy(record.magic, RecordMagic, sizeof(RecordMagic));
        record.version = RecordVersion;
        record.sequence = sequence;
        crypto::GenerateNonce(record.nonce);

        if (!DeriveKey(key) ||
            !crypto::Aes128GcmEncrypt(
                key, record.nonce,
                reinterpret_cast<const u8*>(&record),
                offsetof(EncryptedRecord, ciphertext),
                reinterpret_cast<const u8*>(&data), sizeof(data),
                record.ciphertext, record.tag)) {
            Wipe(key, sizeof(key));
            Wipe(&data, sizeof(data));
            Wipe(&record, sizeof(record));
            return false;
        }

        // Replace only the older/inactive slot. The newest valid slot survives
        // any interruption before the new flushed record is complete.
        const char* path = SlotPath(slot);
        (void)fs::DeleteFile(path);
        bool success = false;
        if (R_SUCCEEDED(fs::CreateFile(path, sizeof(record)))) {
            fs::FileHandle file{};
            if (R_SUCCEEDED(fs::OpenFile(&file, path, fs::OpenMode_Write))) {
                const ams::Result result = fs::WriteFile(
                    file, 0, &record, sizeof(record), fs::WriteOption::Flush);
                fs::CloseFile(file);
                success = R_SUCCEEDED(result);
            }
            if (!success) (void)fs::DeleteFile(path);
        }
        Wipe(key, sizeof(key));
        Wipe(&data, sizeof(data));
        Wipe(&record, sizeof(record));
        return success;
    }
}

bool Load(usage::Snapshot* out) {
    if (out == nullptr) return false;
    usage::Snapshot slots[2]{};
    (void)ReadSlot(0, &slots[0]);
    (void)ReadSlot(1, &slots[1]);
    const usage::Snapshot* selected = usage::SelectNewest(slots[0], slots[1]);
    if (selected == nullptr) return false;
    *out = *selected;
    Wipe(slots, sizeof(slots));
    return true;
}

bool Save(const usage::Snapshot& snapshot) {
    if (!snapshot.valid || snapshot.local_day_key < 0 ||
        snapshot.active_seconds > usage::MaxPersistedActiveSeconds) return false;
    usage::Snapshot slots[2]{};
    (void)ReadSlot(0, &slots[0]);
    (void)ReadSlot(1, &slots[1]);
    const usage::Snapshot* newest = usage::SelectNewest(slots[0], slots[1]);
    const u64 current = newest == nullptr ? 0 : newest->sequence;
    if (current == std::numeric_limits<u64>::max()) {
        Wipe(slots, sizeof(slots));
        return false;
    }
    const u64 next = current + 1;
    usage::Snapshot to_write = snapshot;
    to_write.sequence = next;
    to_write.valid = true;
    const unsigned target = static_cast<unsigned>(next & 1u);
    const bool saved = WriteSlot(target, to_write, next);
    Wipe(slots, sizeof(slots));
    Wipe(&to_write, sizeof(to_write));
    return saved;
}
}
