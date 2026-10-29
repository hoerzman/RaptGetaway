#include "web_security.h"

#include <Arduino.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/oid.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cstring>

namespace
{
constexpr size_t CERTIFICATE_BUFFER_SIZE = 2048;
constexpr size_t PRIVATE_KEY_BUFFER_SIZE = 1024;
constexpr size_t PASSWORD_SALT_SIZE = 16;
constexpr size_t PASSWORD_HASH_SIZE = 32;
constexpr uint32_t PASSWORD_HASH_ITERATIONS = 100000;
constexpr uint32_t SESSION_LIFETIME_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t CERTIFICATE_TASK_STACK_SIZE = 16384;

char certificatePem[CERTIFICATE_BUFFER_SIZE] = {};
char privateKeyPem[PRIVATE_KEY_BUFFER_SIZE] = {};
char hostname[40] = {};
char certificateFingerprint[96] = {};
char sessionToken[65] = {};
uint32_t sessionExpiresAt = 0;

struct InitializationContext
{
    Preferences* preferences;
    SemaphoreHandle_t completed;
    bool result;
};

bool constantTimeEqual(const uint8_t* left, const uint8_t* right, size_t length)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i)
    {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

bool derivePasswordHash(const String& password,
                        const uint8_t* salt,
                        uint8_t* output)
{
    const mbedtls_md_info_t* digest =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (digest == nullptr)
    {
        return false;
    }

    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    if (mbedtls_md_setup(&context, digest, 1) != 0 ||
        mbedtls_md_hmac_starts(
            &context,
            reinterpret_cast<const unsigned char*>(password.c_str()),
            password.length()) != 0)
    {
        mbedtls_md_free(&context);
        return false;
    }

    uint8_t blockInput[PASSWORD_SALT_SIZE + 4] = {};
    memcpy(blockInput, salt, PASSWORD_SALT_SIZE);
    blockInput[PASSWORD_SALT_SIZE + 3] = 1;
    uint8_t u[PASSWORD_HASH_SIZE] = {};
    if (mbedtls_md_hmac_update(&context, blockInput, sizeof(blockInput)) != 0 ||
        mbedtls_md_hmac_finish(&context, u) != 0)
    {
        mbedtls_md_free(&context);
        return false;
    }

    memcpy(output, u, sizeof(u));
    for (uint32_t iteration = 1; iteration < PASSWORD_HASH_ITERATIONS; ++iteration)
    {
        if (mbedtls_md_hmac_reset(&context) != 0 ||
            mbedtls_md_hmac_update(&context, u, sizeof(u)) != 0 ||
            mbedtls_md_hmac_finish(&context, u) != 0)
        {
            mbedtls_md_free(&context);
            return false;
        }
        for (size_t i = 0; i < sizeof(u); ++i)
        {
            output[i] ^= u[i];
        }
    }
    memset(u, 0, sizeof(u));
    memset(blockInput, 0, sizeof(blockInput));
    mbedtls_md_free(&context);
    return true;
}

void makeHex(const uint8_t* input, size_t length, char* output)
{
    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i)
    {
        output[i * 2] = HEX_DIGITS[input[i] >> 4];
        output[i * 2 + 1] = HEX_DIGITS[input[i] & 0x0f];
    }
    output[length * 2] = '\0';
}

bool generateCertificate()
{
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK)
    {
        return false;
    }

    snprintf(hostname, sizeof(hostname),
             "raptgetaway-%02x%02x%02x.local", mac[3], mac[4], mac[5]);
    char commonName[48] = {};
    snprintf(commonName, sizeof(commonName),
             "RaptGateway-%02X%02X%02X", mac[3], mac[4], mac[5]);

    mbedtls_pk_context key;
    mbedtls_x509write_cert certificate;
    mbedtls_mpi serial;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&certificate);
    mbedtls_mpi_init(&serial);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    bool success = false;
    const unsigned char personalization[] = "RaptGateway TLS key";
    uint8_t serialBytes[16] = {};
    uint8_t subjectAltName[64] = {};
    size_t sanLength = 0;
    const size_t hostnameLength = strlen(hostname);
    if (hostnameLength > 60)
    {
        goto cleanup;
    }

    subjectAltName[sanLength++] = 0x30;
    subjectAltName[sanLength++] = static_cast<uint8_t>(hostnameLength + 2);
    subjectAltName[sanLength++] = 0x82;
    subjectAltName[sanLength++] = static_cast<uint8_t>(hostnameLength);
    memcpy(subjectAltName + sanLength, hostname, hostnameLength);
    sanLength += hostnameLength;

    esp_fill_random(serialBytes, sizeof(serialBytes));
    serialBytes[0] &= 0x7f;
    serialBytes[0] |= 0x01;
    if (mbedtls_ctr_drbg_seed(
            &drbg, mbedtls_entropy_func, &entropy,
            personalization, sizeof(personalization) - 1) != 0 ||
        mbedtls_pk_setup(
            &key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,
                            mbedtls_pk_ec(key),
                            mbedtls_ctr_drbg_random,
                            &drbg) != 0)
    {
        goto cleanup;
    }

    if (mbedtls_mpi_read_binary(&serial, serialBytes, sizeof(serialBytes)) != 0)
    {
        goto cleanup;
    }

    mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&certificate, &key);
    mbedtls_x509write_crt_set_issuer_key(&certificate, &key);
    if (mbedtls_x509write_crt_set_subject_name(
            &certificate, (String("CN=") + commonName).c_str()) != 0 ||
        mbedtls_x509write_crt_set_issuer_name(
            &certificate, (String("CN=") + commonName).c_str()) != 0 ||
        mbedtls_x509write_crt_set_serial(&certificate, &serial) != 0 ||
        mbedtls_x509write_crt_set_validity(
            &certificate, "20200101000000", "20491231235959") != 0 ||
        mbedtls_x509write_crt_set_basic_constraints(
            &certificate, 0, -1) != 0 ||
        mbedtls_x509write_crt_set_key_usage(
            &certificate, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0 ||
        mbedtls_x509write_crt_set_extension(
            &certificate,
            MBEDTLS_OID_SUBJECT_ALT_NAME,
            MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME),
            0,
            subjectAltName,
            sanLength) != 0)
    {
        goto cleanup;
    }

    if (mbedtls_pk_write_key_pem(
            &key, reinterpret_cast<unsigned char*>(privateKeyPem),
            sizeof(privateKeyPem)) != 0 ||
        mbedtls_x509write_crt_pem(
            &certificate,
            reinterpret_cast<unsigned char*>(certificatePem),
            sizeof(certificatePem),
            mbedtls_ctr_drbg_random,
            &drbg) != 0)
    {
        goto cleanup;
    }
    success = true;

cleanup:
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&certificate);
    mbedtls_pk_free(&key);
    memset(serialBytes, 0, sizeof(serialBytes));
    return success;
}

bool calculateFingerprint()
{
    mbedtls_x509_crt parsedCertificate;
    mbedtls_x509_crt_init(&parsedCertificate);
    const int parseResult = mbedtls_x509_crt_parse(
        &parsedCertificate,
        reinterpret_cast<const unsigned char*>(certificatePem),
        strlen(certificatePem) + 1);
    if (parseResult != 0 || parsedCertificate.raw.p == nullptr)
    {
        mbedtls_x509_crt_free(&parsedCertificate);
        return false;
    }

    uint8_t digest[32] = {};
    const int hashResult = mbedtls_sha256_ret(
        parsedCertificate.raw.p, parsedCertificate.raw.len, digest, 0);
    if (hashResult == 0)
    {
        static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
        size_t outputOffset = 0;
        for (size_t i = 0; i < sizeof(digest); ++i)
        {
            if (i != 0)
            {
                certificateFingerprint[outputOffset++] = ':';
            }
            certificateFingerprint[outputOffset++] =
                HEX_DIGITS[digest[i] >> 4];
            certificateFingerprint[outputOffset++] =
                HEX_DIGITS[digest[i] & 0x0f];
        }
        certificateFingerprint[outputOffset] = '\0';
    }
    memset(digest, 0, sizeof(digest));
    mbedtls_x509_crt_free(&parsedCertificate);
    return hashResult == 0;
}

bool validateCertificateKeyPair()
{
    mbedtls_x509_crt parsedCertificate;
    mbedtls_pk_context parsedKey;
    mbedtls_x509_crt_init(&parsedCertificate);
    mbedtls_pk_init(&parsedKey);
    const int certResult = mbedtls_x509_crt_parse(
        &parsedCertificate,
        reinterpret_cast<const unsigned char*>(certificatePem),
        strlen(certificatePem) + 1);
    const int keyResult = mbedtls_pk_parse_key(
        &parsedKey,
        reinterpret_cast<const unsigned char*>(privateKeyPem),
        strlen(privateKeyPem) + 1,
        nullptr, 0);
    const bool valid = certResult == 0 && keyResult == 0 &&
                       mbedtls_pk_check_pair(&parsedCertificate.pk,
                                             &parsedKey) == 0;
    mbedtls_pk_free(&parsedKey);
    mbedtls_x509_crt_free(&parsedCertificate);
    return valid;
}

bool initializeWebSecurityWithLargeStack(Preferences& preferences)
{
    bool loaded = false;
    if (preferences.isKey("tlsCert") && preferences.isKey("tlsKey"))
    {
        const String savedCertificate = preferences.getString("tlsCert");
        const String savedKey = preferences.getString("tlsKey");
        if (savedCertificate.length() < sizeof(certificatePem) &&
            savedKey.length() < sizeof(privateKeyPem))
        {
            savedCertificate.toCharArray(certificatePem, sizeof(certificatePem));
            savedKey.toCharArray(privateKeyPem, sizeof(privateKeyPem));
            loaded = validateCertificateKeyPair();
        }
    }

    if (!loaded)
    {
        memset(certificatePem, 0, sizeof(certificatePem));
        memset(privateKeyPem, 0, sizeof(privateKeyPem));
        if (!generateCertificate())
        {
            Serial.println("HTTPS identity generation failed");
            return false;
        }
        if (preferences.putString("tlsCert", certificatePem) == 0 ||
            preferences.putString("tlsKey", privateKeyPem) == 0)
        {
            Serial.println("HTTPS identity could not be saved to NVS");
            return false;
        }
    }

    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK)
    {
        return false;
    }
    snprintf(hostname, sizeof(hostname),
             "raptgetaway-%02x%02x%02x.local", mac[3], mac[4], mac[5]);
    if (!calculateFingerprint())
    {
        Serial.println("HTTPS certificate fingerprint could not be calculated");
        return false;
    }
    Serial.printf("HTTPS hostname: %s\n", hostname);
    Serial.printf("HTTPS certificate SHA-256 fingerprint: %s\n",
                  certificateFingerprint);
    return true;
}

void initializeWebSecurityTask(void* argument)
{
    InitializationContext* context =
        static_cast<InitializationContext*>(argument);
    context->result =
        initializeWebSecurityWithLargeStack(*context->preferences);
    xSemaphoreGive(context->completed);
    vTaskDelete(nullptr);
}
} // namespace

bool initializeWebSecurity(Preferences& preferences)
{
    InitializationContext context = {};
    context.preferences = &preferences;
    context.completed = xSemaphoreCreateBinary();
    if (context.completed == nullptr)
    {
        Serial.println("Could not create HTTPS initialization semaphore");
        return false;
    }

    const BaseType_t taskCreated = xTaskCreate(
        initializeWebSecurityTask,
        "webSecurityInit",
        CERTIFICATE_TASK_STACK_SIZE,
        &context,
        1,
        nullptr);
    if (taskCreated != pdPASS)
    {
        vSemaphoreDelete(context.completed);
        Serial.println("Could not create HTTPS initialization task");
        return false;
    }

    if (xSemaphoreTake(context.completed, portMAX_DELAY) != pdTRUE)
    {
        vSemaphoreDelete(context.completed);
        Serial.println("HTTPS initialization task did not complete");
        return false;
    }
    vSemaphoreDelete(context.completed);
    return context.result;
}

const char* webServerCertificate()
{
    return certificatePem;
}

size_t webServerCertificateLength()
{
    return strlen(certificatePem) + 1;
}

const char* webServerPrivateKey()
{
    return privateKeyPem;
}

size_t webServerPrivateKeyLength()
{
    return strlen(privateKeyPem) + 1;
}

const char* webServerHostname()
{
    return hostname;
}

const char* webServerCertificateFingerprint()
{
    return certificateFingerprint;
}

bool hasAdminPassword(Preferences& preferences)
{
    return preferences.isKey("adminSalt") &&
           preferences.isKey("adminVerifier");
}

bool setAdminPassword(Preferences& preferences, const String& password)
{
    if (password.length() < 12 || password.length() > 64)
    {
        return false;
    }
    uint8_t salt[PASSWORD_SALT_SIZE] = {};
    uint8_t verifier[PASSWORD_HASH_SIZE] = {};
    esp_fill_random(salt, sizeof(salt));
    const bool derived =
        derivePasswordHash(password, salt, verifier);
    const bool stored = derived &&
        preferences.putBytes("adminSalt", salt, sizeof(salt)) == sizeof(salt) &&
        preferences.putBytes("adminVerifier", verifier, sizeof(verifier)) ==
            sizeof(verifier);
    memset(salt, 0, sizeof(salt));
    memset(verifier, 0, sizeof(verifier));
    return stored;
}

bool verifyAdminPassword(Preferences& preferences, const String& password)
{
    if (!hasAdminPassword(preferences) || password.length() > 64)
    {
        return false;
    }
    uint8_t salt[PASSWORD_SALT_SIZE] = {};
    uint8_t expected[PASSWORD_HASH_SIZE] = {};
    uint8_t actual[PASSWORD_HASH_SIZE] = {};
    const bool read =
        preferences.getBytes("adminSalt", salt, sizeof(salt)) == sizeof(salt) &&
        preferences.getBytes("adminVerifier", expected, sizeof(expected)) ==
            sizeof(expected);
    const bool derived = read && derivePasswordHash(password, salt, actual);
    const bool valid =
        derived && constantTimeEqual(expected, actual, sizeof(expected));
    memset(salt, 0, sizeof(salt));
    memset(expected, 0, sizeof(expected));
    memset(actual, 0, sizeof(actual));
    return valid;
}

String createAdminSession()
{
    uint8_t randomBytes[32] = {};
    esp_fill_random(randomBytes, sizeof(randomBytes));
    makeHex(randomBytes, sizeof(randomBytes), sessionToken);
    memset(randomBytes, 0, sizeof(randomBytes));
    sessionExpiresAt = millis() + SESSION_LIFETIME_MS;
    return String(sessionToken);
}

bool isValidAdminSession(const String& token)
{
    if (sessionToken[0] == '\0' || token.length() != 64 ||
        static_cast<int32_t>(sessionExpiresAt - millis()) <= 0 ||
        !constantTimeEqual(
            reinterpret_cast<const uint8_t*>(sessionToken),
            reinterpret_cast<const uint8_t*>(token.c_str()),
            sizeof(sessionToken) - 1))
    {
        return false;
    }
    sessionExpiresAt = millis() + SESSION_LIFETIME_MS;
    return true;
}

void clearAdminSession()
{
    memset(sessionToken, 0, sizeof(sessionToken));
    sessionExpiresAt = 0;
}
