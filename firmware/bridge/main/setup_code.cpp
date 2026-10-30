#include "setup_code.h"

#include <cstring>

#include <crypto/CHIPCryptoPAL.h>
#include <esp_matter_providers.h>
#include <setup_payload/SetupPayload.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

namespace setup_code {

namespace {

const char* TAG = "setup";
const char* NVS_NAMESPACE = "octo_setup";
constexpr uint32_t ITERATIONS = 1000;
constexpr size_t SALT_LENGTH = 32;

class Provider : public chip::DeviceLayer::CommissionableDataProvider {
public:
    void load_or_create() {
        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
            ESP_LOGE(TAG, "NVS unavailable, pairing code not stored");
            create();
            return;
        }
        size_t salt_length = sizeof(salt);
        if (nvs_get_u32(handle, "pin", &passcode) == ESP_OK && nvs_get_u16(handle, "disc", &discriminator) == ESP_OK &&
            nvs_get_blob(handle, "salt", salt, &salt_length) == ESP_OK && salt_length == SALT_LENGTH &&
            chip::SetupPayload::IsValidSetupPIN(passcode)) {
            verifier_length = sizeof(verifier);
            if (nvs_get_blob(handle, "ver", verifier, &verifier_length) != ESP_OK) {
                verifier_length = 0;  // computed when first needed
            }
        } else {
            create();
            nvs_set_u32(handle, "pin", passcode);
            nvs_set_u16(handle, "disc", discriminator);
            nvs_set_blob(handle, "salt", salt, SALT_LENGTH);
            nvs_commit(handle);
            ESP_LOGI(TAG, "New pairing code generated");
        }
        nvs_close(handle);
    }

    CHIP_ERROR GetSetupDiscriminator(uint16_t& value) override {
        value = discriminator;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

    CHIP_ERROR GetSpake2pIterationCount(uint32_t& value) override {
        value = ITERATIONS;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan& buffer) override {
        return chip::CopySpanToMutableSpan(chip::ByteSpan(salt, SALT_LENGTH), buffer);
    }

    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan& buffer, size_t& length) override {
        if (verifier_length == 0) {
            // Derived from the passcode once (PBKDF2), then kept in NVS
            chip::Crypto::Spake2pVerifier spake;
            ReturnErrorOnFailure(spake.Generate(ITERATIONS, chip::ByteSpan(salt, SALT_LENGTH), passcode));
            chip::MutableByteSpan out(verifier);
            ReturnErrorOnFailure(spake.Serialize(out));
            verifier_length = out.size();
            nvs_handle_t handle;
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
                nvs_set_blob(handle, "ver", verifier, verifier_length);
                nvs_commit(handle);
                nvs_close(handle);
            }
        }
        length = verifier_length;
        return chip::CopySpanToMutableSpan(chip::ByteSpan(verifier, verifier_length), buffer);
    }

    CHIP_ERROR GetSetupPasscode(uint32_t& value) override {
        value = passcode;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

private:
    uint32_t passcode = 0;
    uint16_t discriminator = 0;
    uint8_t salt[SALT_LENGTH] = {};
    uint8_t verifier[chip::Crypto::kSpake2p_VerifierSerialized_Length] = {};
    size_t verifier_length = 0;

    void create() {
        do {
            passcode = 1 + esp_random() % chip::kSetupPINCodeMaximumValue;
        } while (!chip::SetupPayload::IsValidSetupPIN(passcode));
        discriminator = esp_random() & 0x0FFF;
        esp_fill_random(salt, SALT_LENGTH);
        verifier_length = 0;
    }
};

Provider provider;

}  // namespace

void install() {
    provider.load_or_create();
    esp_matter::set_custom_commissionable_data_provider(&provider);
}

}  // namespace setup_code
