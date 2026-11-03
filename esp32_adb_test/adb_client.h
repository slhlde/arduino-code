#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include "storage.h"
#include "mbedtls/version.h"
#include "mbedtls/pk.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

#define A_CNXN 0x4e584e43
#define A_OPEN 0x4e45504f
#define A_OKAY 0x59414b4f
#define A_WRTE 0x45545257
#define A_CLSE 0x45534c43
#define A_AUTH 0x48545541

#define ADB_AUTH_TOKEN         1
#define ADB_AUTH_SIGNATURE     2

struct __attribute__((packed)) AdbMessage {
    uint32_t command;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t data_length;
    uint32_t data_check;
    uint32_t magic;
};

class AdbClient {
private:
    static mbedtls_pk_context pk;
    static mbedtls_entropy_context entropy;
    static mbedtls_ctr_drbg_context ctr_drbg;
    static bool isKeyLoaded;

    static uint32_t calcChecksum(const uint8_t *data, size_t len) {
        uint32_t sum = 0;
        for (size_t i = 0; i < len; i++) sum += data[i];
        return sum;
    }

    static bool sendPacket(WiFiClient &client, uint32_t cmd, uint32_t arg0, uint32_t arg1, const uint8_t* payload, size_t len) {
        AdbMessage msg;
        msg.command = cmd;
        msg.arg0 = arg0;
        msg.arg1 = arg1;
        msg.magic = cmd ^ 0xFFFFFFFF;
        msg.data_length = len;
        msg.data_check = (payload && len > 0) ? calcChecksum(payload, len) : 0;

        if (client.write((const uint8_t*)&msg, sizeof(AdbMessage)) != sizeof(AdbMessage)) return false;
        if (len > 0 && payload != nullptr) {
            if (client.write(payload, len) != len) return false;
        }
        return true;
    }

public:
    // 重新加载私钥
    static void reloadPrivateKey(const String& privKeyStr) {
        if (isKeyLoaded) {
            mbedtls_pk_free(&pk);
            mbedtls_ctr_drbg_free(&ctr_drbg);
            mbedtls_entropy_free(&entropy);
            isKeyLoaded = false;
        }

        if (privKeyStr.indexOf("PRIVATE KEY") == -1) return;

        mbedtls_pk_init(&pk);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);

        const char *pers = "adb_esp32_sign";
        mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)pers, strlen(pers));

#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
        int ret = mbedtls_pk_parse_key(&pk, (const unsigned char *)privKeyStr.c_str(), privKeyStr.length() + 1, NULL, 0, mbedtls_ctr_drbg_random, &ctr_drbg);
#else
        int ret = mbedtls_pk_parse_key(&pk, (const unsigned char *)privKeyStr.c_str(), privKeyStr.length() + 1, NULL, 0);
#endif
        if (ret == 0) {
            isKeyLoaded = true;
            Serial.println("[ADB] 私钥已成功解析加载进加密引擎！");
        } else {
            Serial.printf("[ADB] 私钥解析失败，Code: -0x%04x\n", -ret);
        }
    }

    // 核心安全自愈机制：若开机未加载，自动去 Flash 读取
    static bool ensureKeyLoaded() {
        if (isKeyLoaded) return true;
        String savedKey = storage.getAdbKey();
        if (savedKey.length() > 0 && savedKey.indexOf("PRIVATE KEY") != -1) {
            reloadPrivateKey(savedKey);
            return isKeyLoaded;
        }
        return false;
    }

    static bool signToken(const uint8_t* token, size_t token_len, uint8_t* sig_out, size_t sig_buf_size, size_t* sig_len) {
        if (!ensureKeyLoaded()) return false;

#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
        int ret = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA1, token, token_len, sig_out, sig_buf_size, sig_len, mbedtls_ctr_drbg_random, &ctr_drbg);
#else
        int ret = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA1, token, token_len, sig_out, sig_len, mbedtls_ctr_drbg_random, &ctr_drbg);
#endif
        return (ret == 0);
    }

    // 单次连接执行（超时设为 2500ms，确保手机有充裕时间比对指纹）
    static int singleAttempt(const String& ip, uint16_t port, uint32_t timeoutMs = 2500) {
        WiFiClient client;
        client.setTimeout(3); 

        if (!client.connect(ip.c_str(), port, timeoutMs)) {
            return -1; // 端口未开放
        }

        const char* banner = "host::esp32";
        sendPacket(client, A_CNXN, 0x01000000, 4096, (const uint8_t*)banner, strlen(banner) + 1);

        AdbMessage header;
        uint32_t start = millis();
        bool handshaked = false;

        while (millis() - start < timeoutMs) {
            if (client.available() >= sizeof(AdbMessage)) {
                client.readBytes((char*)&header, sizeof(AdbMessage));

                if (header.command == A_AUTH && header.arg0 == ADB_AUTH_TOKEN) {
                    uint8_t token[header.data_length];
                    client.readBytes((char*)token, header.data_length);

                    if (!ensureKeyLoaded()) {
                        client.stop();
                        return -2; // 未配置私钥
                    }

                    uint8_t signature[256];
                    size_t sig_len = 0;
                    if (signToken(token, header.data_length, signature, sizeof(signature), &sig_len)) {
                        sendPacket(client, A_AUTH, ADB_AUTH_SIGNATURE, 0, signature, sig_len);
                    } else {
                        client.stop();
                        return -3; // 签名失败
                    }
                } 
                else if (header.command == A_CNXN) {
                    for (uint32_t i = 0; i < header.data_length; i++) client.read();
                    handshaked = true;
                    break;
                }
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }

        if (!handshaked) {
            client.stop();
            return -4; // 手机拒绝公钥/未勾选始终允许
        }

        uint32_t localId = 1;
        uint32_t remoteId = 0;
        const char* cmd = "shell:dumpsys battery";
        sendPacket(client, A_OPEN, localId, 0, (const uint8_t*)cmd, strlen(cmd) + 1);

        int batteryLevel = -5;
        start = millis();

        while (client.connected() && (millis() - start < timeoutMs)) {
            if (client.available() >= sizeof(AdbMessage)) {
                client.readBytes((char*)&header, sizeof(AdbMessage));

                if (header.command == A_OKAY) {
                    remoteId = header.arg0;
                } else if (header.command == A_WRTE) {
                    int len = header.data_length;
                    char* buf = (char*)malloc(len + 1);
                    if (buf) {
                        client.readBytes(buf, len);
                        buf[len] = '\0';
                        sendPacket(client, A_OKAY, localId, remoteId, nullptr, 0);

                        char* p = strstr(buf, "level: ");
                        if (p) {
                            batteryLevel = atoi(p + 7);
                        }
                        free(buf);
                    }
                    if (batteryLevel >= 0) break;
                } else if (header.command == A_CLSE) {
                    break;
                }
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }

        sendPacket(client, A_CLSE, localId, remoteId, nullptr, 0);
        client.stop();
        return batteryLevel;
    }

    static int getBatteryLevel(const String& ip, uint16_t port = 5555, int maxRetries = 2) {
        int res = -1;
        for (int i = 0; i < maxRetries; i++) {
            res = singleAttempt(ip, port, 2500);
            if (res >= 0) return res;
            if (res == -2 || res == -3) return res;
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        return res;
    }
};

inline mbedtls_pk_context AdbClient::pk;
inline mbedtls_entropy_context AdbClient::entropy;
inline mbedtls_ctr_drbg_context AdbClient::ctr_drbg;
inline bool AdbClient::isKeyLoaded = false;