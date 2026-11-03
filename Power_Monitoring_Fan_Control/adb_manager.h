#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <vector>
#include "mbedtls/version.h"
#include "mbedtls/pk.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

// 前向声明主程序中的继电器函数与状态
void setSocketRelay(int internalRelayNum, bool state);
extern bool relay2State; // 插座1 (GPIO6)
extern bool relay3State; // 插座2 (GPIO7)
String getCurrentTimeString(); // 获取当前时间字符串

#define A_CNXN 0x4e584e43
#define A_OPEN 0x4e45504f
#define A_OKAY 0x59414b4f
#define A_WRTE 0x45545257
#define A_CLSE 0x45534c43
#define A_AUTH 0x48545541

#define ADB_AUTH_TOKEN     1
#define ADB_AUTH_SIGNATURE 2

struct __attribute__((packed)) AdbMessage {
    uint32_t command;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t data_length;
    uint32_t data_check;
    uint32_t magic;
};

// 单台设备的数据结构
struct AdbDevice {
    String ip;
    int relayNum;      // 0: 不绑定, 2: 插座1(GPIO6), 3: 插座2(GPIO7), 后续可继续扩充
    int battery;       // -1 表示未获取
    bool connected;
    bool tested;
    String lastTime;
    String errorMsg;
};

// 充电定时任务结构
struct AutoChargeTimer {
    int relayNum;
    bool active;
    unsigned long expireTime;
};

class AdbManager {
private:
    Preferences prefs;
    mbedtls_pk_context pk;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool isKeyLoaded = false;

    // 排队轮询状态机
    bool isPollingQueue = false;
    size_t pollQueueIndex = 0;
    unsigned long lastStepMs = 0;
    unsigned long lastAutoPollMs = 0;

    // 各继电器的自动充电倒计时 (支持继电器2和3，后续可继续追加)
    AutoChargeTimer chargeTimers[2] = {
        {2, false, 0}, // 插座1
        {3, false, 0}  // 插座2
    };

    uint32_t calcChecksum(const uint8_t *data, size_t len) {
        uint32_t sum = 0;
        for (size_t i = 0; i < len; i++) sum += data[i];
        return sum;
    }

    bool sendPacket(WiFiClient &client, uint32_t cmd, uint32_t arg0, uint32_t arg1, const uint8_t* payload, size_t len) {
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
    std::vector<AdbDevice> devices;

    void begin() {
        prefs.begin("adb-ctrl", false);
        loadSettings();
        loadDevices();
        ensureKeyLoaded();
        Serial.printf("[ADB] 初始化完成，已加载设备: %d 台\n", devices.size());
    }

    // --- 密钥与参数持久化 ---
    String getAdbKey() { return prefs.getString("key", ""); }
    void saveAdbKey(const String& key) { prefs.putString("key", key); reloadPrivateKey(key); }
    int getLowThreshold() { return prefs.getInt("th", 30); }
    int getPollInterval() { return prefs.getInt("interval", 60); }
    int getRetries() { return prefs.getInt("retries", 3); }
    int getChargeDuration() { return prefs.getInt("duration", 40); } // 默认开启40分钟

    void saveSettings(int th, int interval, int retries, int duration) {
        prefs.putInt("th", th);
        prefs.putInt("interval", interval);
        prefs.putInt("retries", retries);
        prefs.putInt("duration", duration);
    }

    void reloadPrivateKey(const String& privKeyStr) {
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
        if (ret == 0) isKeyLoaded = true;
    }

    bool ensureKeyLoaded() {
        if (isKeyLoaded) return true;
        String key = getAdbKey();
        if (key.length() > 0) { reloadPrivateKey(key); return isKeyLoaded; }
        return false;
    }

    bool signToken(const uint8_t* token, size_t token_len, uint8_t* sig_out, size_t sig_buf_size, size_t* sig_len) {
        if (!ensureKeyLoaded()) return false;
#if defined(MBEDTLS_VERSION_NUMBER) && (MBEDTLS_VERSION_NUMBER >= 0x03000000)
        int ret = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA1, token, token_len, sig_out, sig_buf_size, sig_len, mbedtls_ctr_drbg_random, &ctr_drbg);
#else
        int ret = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA1, token, token_len, sig_out, sig_len, mbedtls_ctr_drbg_random, &ctr_drbg);
#endif
        return (ret == 0);
    }

    // --- 单次 ADB 读取底层 ---
    int singleAttempt(const String& ip, uint16_t port, uint32_t timeoutMs = 2500) {
        WiFiClient client;
        client.setTimeout(3);
        if (!client.connect(ip.c_str(), port, timeoutMs)) return -1;

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
                    if (!ensureKeyLoaded()) { client.stop(); return -2; }

                    uint8_t signature[256];
                    size_t sig_len = 0;
                    if (signToken(token, header.data_length, signature, sizeof(signature), &sig_len)) {
                        sendPacket(client, A_AUTH, ADB_AUTH_SIGNATURE, 0, signature, sig_len);
                    } else { client.stop(); return -3; }
                } 
                else if (header.command == A_CNXN) {
                    for (uint32_t i = 0; i < header.data_length; i++) client.read();
                    handshaked = true;
                    break;
                }
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }

        if (!handshaked) { client.stop(); return -4; }

        uint32_t localId = 1, remoteId = 0;
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
                        if (p) batteryLevel = atoi(p + 7);
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

    int readBattery(const String& ip, int retries) {
        int res = -1;
        for (int i = 0; i < retries; i++) {
            res = singleAttempt(ip, 5555, 2500);
            if (res >= 0) return res;
            if (res == -2 || res == -3) return res;
            vTaskDelay(80 / portTICK_PERIOD_MS);
        }
        return res;
    }

    // --- 设备增删与管理 ---
    void addDevice(const String& ip, int relayNum) {
        for (auto& d : devices) {
            if (d.ip == ip) { d.relayNum = relayNum; saveDevices(); return; }
        }
        AdbDevice dev;
        dev.ip = ip;
        dev.relayNum = relayNum;
        dev.battery = -1;
        dev.connected = false;
        dev.tested = false;
        dev.lastTime = "--";
        dev.errorMsg = "未检测";
        devices.push_back(dev);
        saveDevices();
    }

    void removeDevice(const String& ip) {
        for (auto it = devices.begin(); it != devices.end(); ++it) {
            if (it->ip == ip) { devices.erase(it); break; }
        }
        saveDevices();
    }

    // 检测单台设备并触发低电量连锁继电器开启 40 分钟
    int testDevice(const String& ip) {
        int idx = -1;
        for (size_t i = 0; i < devices.size(); i++) {
            if (devices[i].ip == ip) { idx = i; break; }
        }
        if (idx == -1) return -1;

        int res = readBattery(ip, getRetries());
        devices[idx].tested = true;

        if (res >= 0) {
            devices[idx].battery = res;
            devices[idx].connected = true;
            devices[idx].errorMsg = "连接成功";
            devices[idx].lastTime = getCurrentTimeString();
            checkAndTriggerCharge(devices[idx]); // 检查是否需要自动通电
        } else {
            devices[idx].connected = false;
            switch (res) {
                case -1: devices[idx].errorMsg = "失败(端口5555未开)"; break;
                case -2: devices[idx].errorMsg = "失败(未配置私钥)"; break;
                case -3: devices[idx].errorMsg = "失败(私钥损坏)"; break;
                case -4: devices[idx].errorMsg = "失败(未授权公钥)"; break;
                default: devices[idx].errorMsg = "失败(响应超时)"; break;
            }
        }
        saveDevices();
        return res;
    }

    // 智能联动逻辑：低于阈值且插座当前关闭时，开启 40 分钟
    void checkAndTriggerCharge(AdbDevice& dev) {
        if (dev.relayNum <= 0 || dev.battery < 0) return;
        int threshold = getLowThreshold();

        if (dev.battery <= threshold) {
            bool isCurrentlyOn = false;
            if (dev.relayNum == 2) isCurrentlyOn = relay2State;
            else if (dev.relayNum == 3) isCurrentlyOn = relay3State;

            // 只有当未开启时才触发，避免重复刷新定时器
            if (!isCurrentlyOn) {
                unsigned long durMs = (unsigned long)getChargeDuration() * 60UL * 1000UL;
                int timerIdx = (dev.relayNum == 2) ? 0 : 1;
                chargeTimers[timerIdx].active = true;
                chargeTimers[timerIdx].expireTime = millis() + durMs;

                setSocketRelay(dev.relayNum, true);
                Serial.printf("[ADB-AutoCharge] 设备 %s 电量 %d%% <= 阈值 %d%%, 开启插座 %d 充电 %d 分钟！\n",
                              dev.ip.c_str(), dev.battery, threshold, dev.relayNum - 1, getChargeDuration());
            }
        }
    }

    // 获取某个插座是否正处于由 ADB 触发的自动充电中，及剩余秒数
    long getChargeRemainSeconds(int relayNum) {
        int idx = (relayNum == 2) ? 0 : 1;
        if (chargeTimers[idx].active) {
            long rem = (long)(chargeTimers[idx].expireTime - millis()) / 1000L;
            return rem > 0 ? rem : 0;
        }
        return 0;
    }

    // --- 排队状态机 (完全不阻塞 loop，每次只读一个设备) ---
    void loop() {
        unsigned long now = millis();

        // 1. 检查自动充电 40 分钟到期关闭
        for (int i = 0; i < 2; i++) {
            if (chargeTimers[i].active && (long)(now - chargeTimers[i].expireTime) >= 0) {
                chargeTimers[i].active = false;
                setSocketRelay(chargeTimers[i].relayNum, false);
                Serial.printf("[ADB-AutoCharge] 插座 %d 自动充电结束，已自动切断电源。\n", chargeTimers[i].relayNum - 1);
            }
        }

        // 2. 检查是否到达后台轮询周期
        unsigned long pollIntervalMs = (unsigned long)getPollInterval() * 60UL * 1000UL;
        if (pollIntervalMs > 0 && WiFi.status() == WL_CONNECTED && !devices.empty()) {
            if (!isPollingQueue && (now - lastAutoPollMs >= pollIntervalMs)) {
                isPollingQueue = true;
                pollQueueIndex = 0;
                lastAutoPollMs = now;
                lastStepMs = now;
                Serial.println("[ADB-Queue] 启动后台逐一排队轮询电量...");
            }
        }

        // 3. 执行单步排队检测 (每台间隔 500ms，极省算力)
        if (isPollingQueue) {
            if (now - lastStepMs >= 500) {
                if (pollQueueIndex < devices.size()) {
                    testDevice(devices[pollQueueIndex].ip);
                    pollQueueIndex++;
                    lastStepMs = millis();
                } else {
                    isPollingQueue = false;
                    Serial.println("[ADB-Queue] 本轮排队检测全部完成。");
                }
            }
        }
    }

    // --- Flash 持久化 ---
    void saveDevices() {
        String data = "";
        for (const auto& d : devices) {
            data += d.ip + "#" + String(d.relayNum) + "#" + String(d.battery) + "#" +
                    (d.connected ? "1":"0") + "#" + (d.tested ? "1":"0") + "#" +
                    d.lastTime + "#" + d.errorMsg + "\n";
        }
        prefs.putString("devs", data);
    }

    void loadDevices() {
        devices.clear();
        String data = prefs.getString("devs", "");
        if (data.length() == 0) return;

        int start = 0, end = data.indexOf('\n');
        while (end != -1) {
            String line = data.substring(start, end);
            line.trim();
            if (line.length() > 0) {
                int p[6];
                p[0] = line.indexOf('#');
                for (int i = 1; i < 6; i++) p[i] = line.indexOf('#', p[i-1] + 1);

                if (p[0] != -1 && p[1] != -1 && p[2] != -1 && p[3] != -1 && p[4] != -1 && p[5] != -1) {
                    AdbDevice d;
                    d.ip = line.substring(0, p[0]);
                    d.relayNum = line.substring(p[0] + 1, p[1]).toInt();
                    d.battery = line.substring(p[1] + 1, p[2]).toInt();
                    d.connected = (line.substring(p[2] + 1, p[3]) == "1");
                    d.tested = (line.substring(p[3] + 1, p[4]) == "1");
                    d.lastTime = line.substring(p[4] + 1, p[5]);
                    d.errorMsg = line.substring(p[5] + 1);
                    devices.push_back(d);
                }
            }
            start = end + 1;
            end = data.indexOf('\n', start);
        }
    }

    void loadSettings() {}
};

extern AdbManager adbManager;