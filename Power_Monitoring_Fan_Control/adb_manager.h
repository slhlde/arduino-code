#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <NTPClient.h>
#include <vector>
#include "mbedtls/version.h"
#include "mbedtls/pk.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

void setSocketRelay(int internalRelayNum, bool state);
extern bool relay2State; // 插座1 (GPIO6)
extern bool relay3State; // 插座2 (GPIO7)
extern bool wifiAvailable;
extern NTPClient timeClient;
String getCurrentTimeString();

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

struct AdbDevice {
    String ip;
    String relayTarget; // "0": 不绑定, "2": 插座1, "3": 插座2, 其他: 外部插座IP
    int battery;
    bool isCharging;
    bool connected;
    bool tested;
    String lastTime;
    String errorMsg;

    bool chargeActive;
    unsigned long chargeExpireMs;
};

class AdbManager {
private:
    Preferences prefs;
    mbedtls_pk_context pk;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool isKeyLoaded = false;

    bool isPollingQueue = false;
    size_t pollQueueIndex = 0;
    unsigned long lastStepMs = 0;
    unsigned long lastAutoPollMs = 0;

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

    // 格式化纯 host (去除 http:// 与路径)
    String cleanHost(const String& target) {
        String host = target;
        host.replace("http://", "");
        host.replace("https://", "");
        int slash = host.indexOf('/');
        if (slash != -1) host = host.substring(0, slash);
        return host;
    }

    // 对标 Switch2400：同步时钟、清空旧关机任务、写入40分钟关机任务并开启继电器
    // 工业级无感执行：对标 Switch2400，带快速熔断机制，耗时低至数十毫秒
    void triggerExternalSocketSwitch2400(const String& targetIp, int durationMinutes) {
        if (WiFi.status() != WL_CONNECTED || targetIp.length() < 7) return;
        String host = cleanHost(targetIp);

        // 1. 获取当前时间与计算硬件关机时间
        String curTime = "12:00:00";
        String offTime = "12:40:00";

        if (wifiAvailable && timeClient.isTimeSet()) {
            time_t rawtime = timeClient.getEpochTime();
            struct tm *ptm = gmtime(&rawtime);
            char b1[12];
            snprintf(b1, sizeof(b1), "%02d:%02d:%02d", ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
            curTime = String(b1);

            time_t offEpoch = rawtime + (unsigned long)durationMinutes * 60UL;
            struct tm *ptmOff = gmtime(&offEpoch);
            char b2[12];
            snprintf(b2, sizeof(b2), "%02d:%02d:%02d", ptmOff->tm_hour, ptmOff->tm_min, ptmOff->tm_sec);
            offTime = String(b2);
        }

        HTTPClient http;
        http.setTimeout(500); // 局域网 500ms 极速超时，绝不拖泥带水

        // 2. 发送时间同步指令并抓取插座当前 HTML (ledPwm=2)
        String syncUrl = "http://" + host + "/LED-Control?ledPwm=2&time=" + curTime;
        String html = "";
        
        if (http.begin(syncUrl)) {
            int code = http.GET();
            if (code == 200) {
                html = http.getString();
            } else {
                // 【核心优化点：快速熔断】如果对方离线或无响应，立刻退出，绝不傻等后续请求！
                http.end();
                Serial.printf("[Switch2400] 外部插座 [%s] 离线或无响应 (Code: %d)，快速熔断退出！\n", host.c_str(), code);
                return; 
            }
            http.end();
        } else {
            return; // 连不上立即退出
        }

        vTaskDelay(10 / portTICK_PERIOD_MS); // 释放时间片给系统调度

        // 3. 解析插座中已存在的旧关机时间并删除 (ledPwm=5)
        if (html.length() > 0) {
            std::vector<String> oldOffTimes;

            // 情况A：解析 <ul id="off-list"> 中的 deleteSchedule("XX:XX:XX")
            int offUl = html.indexOf("id=\"off-list\"");
            if (offUl == -1) offUl = html.indexOf("id='off-list'");
            if (offUl != -1) {
                int ulEnd = html.indexOf("</ul>", offUl);
                if (ulEnd != -1) {
                    String offSection = html.substring(offUl, ulEnd);
                    int delIdx = 0;
                    while ((delIdx = offSection.indexOf("deleteSchedule(\"", delIdx)) != -1) {
                        delIdx += 16;
                        int qEnd = offSection.indexOf("\"", delIdx);
                        if (qEnd != -1) {
                            String t = offSection.substring(delIdx, qEnd);
                            t.trim();
                            if (t.length() >= 5) oldOffTimes.push_back(t);
                            delIdx = qEnd + 1;
                        }
                    }
                }
            }

            // 情况B：解析 关列表: ['XX:XX:XX']
            int offListIdx = html.indexOf("关列表:");
            if (offListIdx != -1) {
                int b1 = html.indexOf('[', offListIdx);
                int b2 = html.indexOf(']', b1);
                if (b1 != -1 && b2 != -1) {
                    String listStr = html.substring(b1 + 1, b2);
                    int s = 0, e = listStr.indexOf(',');
                    while (e != -1) {
                        String item = listStr.substring(s, e);
                        item.replace("'", ""); item.replace("\"", ""); item.trim();
                        if (item.length() >= 5) oldOffTimes.push_back(item);
                        s = e + 1;
                        e = listStr.indexOf(',', s);
                    }
                    String lastItem = listStr.substring(s);
                    lastItem.replace("'", ""); lastItem.replace("\"", ""); lastItem.trim();
                    if (lastItem.length() >= 5) oldOffTimes.push_back(lastItem);
                }
            }

            // 清理多余旧任务
            for (const auto& oldTime : oldOffTimes) {
                if (oldTime != offTime) {
                    String delUrl = "http://" + host + "/LED-Control?ledPwm=5&time=" + oldTime;
                    if (http.begin(delUrl)) {
                        http.GET();
                        http.end();
                        Serial.printf("[Switch2400] 成功清除旧关机计划: %s\n", oldTime.c_str());
                    }
                    vTaskDelay(10 / portTICK_PERIOD_MS);
                }
            }
        }

        // 4. 写入新的 40 分钟关机计划 (ledPwm=0)
        String addOffUrl = "http://" + host + "/LED-Control?ledPwm=0&time=" + offTime;
        if (http.begin(addOffUrl)) {
            http.GET();
            http.end();
            Serial.printf("[Switch2400] 成功写入硬件关机任务: %s\n", offTime.c_str());
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);

        // 5. 立即开启继电器供电 (ledPwm=3)
        String onUrl = "http://" + host + "/LED-Control?ledPwm=3";
        if (http.begin(onUrl)) {
            http.GET();
            http.end();
            Serial.printf("[Switch2400] 触发外部插座开启 (ledPwm=3)\n");
        }
    }

    // 辅助：发送立即关闭指令 (ledPwm=4)
    void sendExtOffCmd(const String& targetIp) {
        if (WiFi.status() != WL_CONNECTED || targetIp.length() < 7) return;
        String host = cleanHost(targetIp);
        HTTPClient http;
        http.setTimeout(800);
        String url = "http://" + host + "/LED-Control?ledPwm=4";
        if (http.begin(url)) {
            http.GET();
            http.end();
            Serial.printf("[Switch2400] 发送外部插座兜底关闭指令: %s\n", url.c_str());
        }
    }

public:
    std::vector<AdbDevice> devices;

    bool isAdbTriggered(int relayNum) {
        for (const auto& d : devices) {
            if (d.relayTarget == String(relayNum) && d.chargeActive) return true;
        }
        return false;
    }

    void begin() {
        prefs.begin("adb-ctrl", false);
        loadDevices();
        ensureKeyLoaded();
        Serial.printf("[ADB] 初始化完成，已加载设备: %d 台\n", devices.size());
    }

    String getAdbKey() { return prefs.getString("key", ""); }
    void saveAdbKey(const String& key) { prefs.putString("key", key); reloadPrivateKey(key); }
    int getLowThreshold() { return prefs.getInt("th", 30); }
    int getPollInterval() { return prefs.getInt("interval", 60); }
    int getRetries() { return prefs.getInt("retries", 3); }
    int getChargeDuration() { return prefs.getInt("duration", 40); }

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

    int singleAttempt(const String& ip, uint16_t port, bool &outCharging, uint32_t timeoutMs = 2500) {
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
        outCharging = false;
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
                        
                        char* ps = strstr(buf, "status: ");
                        if (ps) {
                            outCharging = (atoi(ps + 8) == 2);
                        } else {
                            outCharging = (strstr(buf, "powered: true") != nullptr);
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

    int readBattery(const String& ip, int retries, bool &outCharging) {
        int res = -1;
        for (int i = 0; i < retries; i++) {
            res = singleAttempt(ip, 5555, outCharging, 2500);
            if (res >= 0) return res;
            if (res == -2 || res == -3) return res;
            vTaskDelay(80 / portTICK_PERIOD_MS);
        }
        return res;
    }

    void addDevice(const String& ip, const String& relayTarget) {
        for (auto& d : devices) {
            if (d.ip == ip) { d.relayTarget = relayTarget; saveDevices(); return; }
        }
        AdbDevice dev;
        dev.ip = ip;
        dev.relayTarget = relayTarget;
        dev.battery = -1;
        dev.isCharging = false;
        dev.connected = false;
        dev.tested = false;
        dev.lastTime = "--";
        dev.errorMsg = "未检测";
        dev.chargeActive = false;
        dev.chargeExpireMs = 0;
        devices.push_back(dev);
        saveDevices();
    }

    void removeDevice(const String& ip) {
        for (auto it = devices.begin(); it != devices.end(); ++it) {
            if (it->ip == ip) { devices.erase(it); break; }
        }
        saveDevices();
    }

    int testDevice(const String& ip) {
        int idx = -1;
        for (size_t i = 0; i < devices.size(); i++) {
            if (devices[i].ip == ip) { idx = i; break; }
        }
        if (idx == -1) return -1;

        bool charging = false;
        int res = readBattery(ip, getRetries(), charging);
        devices[idx].tested = true;

        if (res >= 0) {
            devices[idx].battery = res;
            devices[idx].isCharging = charging;
            devices[idx].connected = true;
            devices[idx].errorMsg = "连接成功";
            devices[idx].lastTime = getCurrentTimeString();
            checkAndTriggerCharge(devices[idx]);
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

    // 智能充电触发：写入硬件关机任务 + 立即开启
    void checkAndTriggerCharge(AdbDevice& dev) {
        if (dev.relayTarget == "0" || dev.relayTarget.length() == 0 || dev.battery < 0) return;
        int threshold = getLowThreshold();

        if (dev.battery <= threshold) {
            unsigned long durMs = (unsigned long)getChargeDuration() * 60UL * 1000UL;

            // 1. 本地插座 1 (GPIO 6)
            if (dev.relayTarget == "2") {
                if (!relay2State) {
                    setSocketRelay(2, true);
                    dev.chargeActive = true;
                    dev.chargeExpireMs = millis() + durMs;
                    Serial.printf("[AutoCharge] 开启本地插座1，充电 %d 分钟\n", getChargeDuration());
                }
            }
            // 2. 本地插座 2 (GPIO 7)
            else if (dev.relayTarget == "3") {
                if (!relay3State) {
                    setSocketRelay(3, true);
                    dev.chargeActive = true;
                    dev.chargeExpireMs = millis() + durMs;
                    Serial.printf("[AutoCharge] 开启本地插座2，充电 %d 分钟\n", getChargeDuration());
                }
            }
            // 3. 外部插座 IP：执行 Switch2400 完整逻辑（清垃圾任务 + 设关机任务 + 立即开机）
            else {
                if (!dev.chargeActive) {
                    triggerExternalSocketSwitch2400(dev.relayTarget, getChargeDuration());
                    dev.chargeActive = true;
                    dev.chargeExpireMs = millis() + durMs;
                }
            }
        }
    }

    long getDeviceRemainSeconds(const AdbDevice& dev) {
        if (dev.chargeActive) {
            long rem = (long)(dev.chargeExpireMs - millis()) / 1000L;
            return rem > 0 ? rem : 0;
        }
        return 0;
    }

    void loop() {
        unsigned long now = millis();

        // 检查本地与外部插座的本地计时到期
        for (auto& dev : devices) {
            if (dev.chargeActive && (long)(now - dev.chargeExpireMs) >= 0) {
                dev.chargeActive = false;
                if (dev.relayTarget == "2") {
                    setSocketRelay(2, false);
                } else if (dev.relayTarget == "3") {
                    setSocketRelay(3, false);
                } else if (dev.relayTarget != "0") {
                    // 本地到期后补发一次关闭指令作为双重保障（插座硬件本身在此时也已到期自动切断）
                    sendExtOffCmd(dev.relayTarget);
                }
            }
        }

        // 定时排队轮询电量
        unsigned long pollIntervalMs = (unsigned long)getPollInterval() * 60UL * 1000UL;
        if (pollIntervalMs > 0 && WiFi.status() == WL_CONNECTED && !devices.empty()) {
            if (!isPollingQueue && (now - lastAutoPollMs >= pollIntervalMs)) {
                isPollingQueue = true;
                pollQueueIndex = 0;
                lastAutoPollMs = now;
                lastStepMs = now;
            }
        }

        if (isPollingQueue) {
            if (now - lastStepMs >= 500) {
                if (pollQueueIndex < devices.size()) {
                    testDevice(devices[pollQueueIndex].ip);
                    pollQueueIndex++;
                    lastStepMs = millis();
                } else {
                    isPollingQueue = false;
                }
            }
        }
    }

    void saveDevices() {
        String data = "";
        for (const auto& d : devices) {
            data += d.ip + "#" + d.relayTarget + "#" + String(d.battery) + "#" +
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
                    d.relayTarget = line.substring(p[0] + 1, p[1]);
                    d.battery = line.substring(p[1] + 1, p[2]).toInt();
                    d.isCharging = false;
                    d.connected = (line.substring(p[2] + 1, p[3]) == "1");
                    d.tested = (line.substring(p[3] + 1, p[4]) == "1");
                    d.lastTime = line.substring(p[4] + 1, p[5]);
                    d.errorMsg = line.substring(p[5] + 1);
                    d.chargeActive = false;
                    d.chargeExpireMs = 0;
                    devices.push_back(d);
                }
            }
            start = end + 1;
            end = data.indexOf('\n', start);
        }
    }
};

extern AdbManager adbManager;