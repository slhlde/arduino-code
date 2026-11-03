#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <map>
#include "esp_wifi.h"
#include "storage.h"
#include "adb_client.h"
#include "notify.h"
#include "web_server.h"

Storage storage;
WebServerManager webManager;
std::map<String, DeviceStatus> deviceCache;

bool isApMode = false;
unsigned long lastPollMs = 0;
unsigned long lastWifiCheckMs = 0; // WiFi 状态巡检时间戳

String getWifiFailReason(wl_status_t status) {
    switch (status) {
        case WL_NO_SSID_AVAIL: return "找不到该 WiFi 名字 (请检查是否是 5G 频段)";
        case WL_CONNECT_FAILED: return "密码错误";
        case WL_CONNECTION_LOST: return "连接中断";
        case WL_DISCONNECTED: return "路由器拒绝连接或超时";
        default: return "未连接 (Code: " + String(status) + ")";
    }
}

String getCurrentTimeString() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 100)) {
        return "已记录";
    }
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
    return String(buf);
}

void checkDevice(const String& ip) {
    int retryCount = storage.getRetryCount();
    if (retryCount < 1) retryCount = 1;

    int res = AdbClient::getBatteryLevel(ip, 5555, retryCount);
    DeviceStatus &st = deviceCache[ip];
    st.tested = true;

    if (res >= 0) {
        st.battery = res;
        st.connected = true;
        st.errorMsg = "连接成功";
        st.lastTime = getCurrentTimeString();

        int threshold = storage.getLowBatteryThreshold();
        String notifyUrl = storage.getNotifyUrl();

        if (notifyUrl.length() > 8) {
            if (res <= threshold) {
                if (!st.notified) {
                    Serial.printf("[Alert] 设备 %s 低电量 (%d%%)，触发告警！\n", ip.c_str(), res);
                    bool ok = Notify::send(
                        notifyUrl,
                        "🚨 手机低电量告警",
                        "手机 [" + ip + "] 当前电量仅剩 " + String(res) + "% (预警线: " + String(threshold) + "%)，请及时充电！",
                        ip,
                        res
                    );
                    if (ok) st.notified = true;
                }
            } else if (res > threshold + 5) {
                st.notified = false;
            }
        }
    } else {
        st.connected = false;
        switch(res) {
            case -1: st.errorMsg = "失败(端口5555未开)"; break;
            case -2: st.errorMsg = "失败(未配置私钥)"; break;
            case -3: st.errorMsg = "失败(私钥损坏)"; break;
            case -4: st.errorMsg = "失败(未授权公钥)"; break;
            default: st.errorMsg = "失败(响应超时)"; break;
        }
    }

    storage.saveStatusCache(deviceCache);
}

void pollAllDevices() {
    auto ips = storage.getIPList();
    if (ips.empty()) return;
    Serial.println("\n[AutoPoll] 定时批量检测设备电量...");
    for (const auto& ip : ips) {
        checkDevice(ip);
        delay(200);
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n=================================");
    Serial.println("  ESP32-C3 ADB 工业高稳版启动    ");
    Serial.println("=================================");

    storage.begin();
    storage.loadStatusCache(deviceCache);

    // 开机自动预加载私钥
    AdbClient::ensureKeyLoaded();

    String ssid = storage.getSSID();
    String pass = storage.getPass();
    bool wifiConnected = false;

    if (ssid.length() > 0) {
        Serial.printf("[WiFi] 正在连接路由器: [%s]\n", ssid.c_str());
        
        // 彻底复位 WiFi 状态
        WiFi.disconnect(true);
        delay(150);
        
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);            // 禁用节能休眠
        WiFi.setAutoReconnect(true);     // 开启硬件级自动重连
        WiFi.persistent(true);           // 开启配置持久记忆

        // ==============================================================
        // 【可选加速黑科技】：若想实现 0.8 秒秒连，取消下方 4 行的注释即可
        // IPAddress local_IP(192, 168, 31, 242);
        // IPAddress gateway(192, 168, 31, 1);
        // IPAddress subnet(255, 255, 255, 0);
        // WiFi.config(local_IP, gateway, subnet);
        // ==============================================================

        WiFi.begin(ssid.c_str(), pass.c_str());

        // 核心优化 1：放宽等待到 30 秒（60 个点），给路由器充足响应时间
        int retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries < 60) {
            delay(500);
            Serial.print(".");
            retries++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            wifiConnected = true;
            Serial.println("\n[WiFi] 100% 连入成功！局域网 IP: http://" + WiFi.localIP().toString());
            configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
        } else {
            Serial.println("\n[WiFi] 连入超时！原因: " + getWifiFailReason(WiFi.status()));
        }
    }

    // 未能连入路由器时，才开启独立兼容热点
    if (!wifiConnected) {
        isApMode = true;
        Serial.println("[AP] 启动全兼容配网热点...");
        WiFi.disconnect(true);
        delay(100);

        WiFi.mode(WIFI_AP);
        WiFi.setSleep(false);
        esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
        esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

        IPAddress apIP(192, 168, 4, 1);
        WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        WiFi.softAP("ESP32-C3-ADB-Setup", "", 6);

        Serial.println("[AP] 热点开启成功: ESP32-C3-ADB-Setup (http://192.168.4.1)");
    }

    webManager.begin();
    Serial.println("[HTTP] Web 服务就绪！");
}

void loop() {
    webManager.handleClient();
    delay(5);

    // 核心优化 2：后台 WiFi 掉线自愈看门狗（每 10 秒巡检一次，断线自连）
    if (!isApMode) {
        if (millis() - lastWifiCheckMs >= 10000) {
            lastWifiCheckMs = millis();
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[WiFi-Guard] 监测到网络断开，正在后台自动重连...");
                WiFi.reconnect();
            }
        }
    }

    // 定时轮询电量
    if (!isApMode && WiFi.status() == WL_CONNECTED) {
        unsigned long intervalMs = (unsigned long)storage.getPollInterval() * 60 * 1000;
        if (intervalMs > 0 && (millis() - lastPollMs >= intervalMs)) {
            lastPollMs = millis();
            pollAllDevices();
        }
    }
}