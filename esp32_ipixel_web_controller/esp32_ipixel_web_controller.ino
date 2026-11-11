#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "esp_wifi.h"
#include "storage.h"
#include "ble_controller.h"
#include "web_server.h"

Storage storage;
WebServerManager webManager;

bool isApMode = false;
unsigned long lastWifiCheckMs = 0;
const int BUTTON_PIN = 9; // 板载 BOOT 键，长按 3 秒重置 Wi-Fi

// 调度引擎时间戳与状态
unsigned long lastClockCycleMs = 0;
int lastCheckedMinute = -1;

void checkResetButton() {
    static unsigned long pressStart = 0;
    static bool isPressing = false;

    if (digitalRead(BUTTON_PIN) == LOW) {
        if (!isPressing) {
            isPressing = true;
            pressStart = millis();
        } else if (millis() - pressStart >= 3000) {
            Serial.println("\n[RESET] 长按 3 秒，清空配网并重启...");
            storage.clearWifi();
            delay(300);
            ESP.restart();
        }
    } else {
        isPressing = false;
    }
}

// ==============================================================
// 核心算法：判断当前是否处于“关机~开机”睡觉时段 (自动处理跨午夜)
// ==============================================================
bool isInSleepWindow(int curH, int curM, const String& onStr, const String& offStr) {
    if (onStr.length() != 5 || offStr.length() != 5) return false;

    int onH = onStr.substring(0, 2).toInt();
    int onM = onStr.substring(3, 5).toInt();
    int offH = offStr.substring(0, 2).toInt();
    int offM = offStr.substring(3, 5).toInt();

    int curTotal = curH * 60 + curM;
    int onTotal = onH * 60 + onM;
    int offTotal = offH * 60 + offM;

    if (offTotal == onTotal) return false; // 时间相同无效

    if (offTotal > onTotal) {
        // 跨午夜睡觉: 例如 23:30 关机，08:00 开机
        // 睡觉区间: >= 23:30 或者 < 08:00
        return (curTotal >= offTotal || curTotal < onTotal);
    } else {
        // 同一天睡觉: 例如 01:00 关机，07:00 开机
        return (curTotal >= offTotal && curTotal < onTotal);
    }
}

// ==============================================================
// 低功耗智能调度引擎 (睡眠时段彻底静音，零性能损耗)
// ==============================================================
void handleScheduler() {
    if (!BleController::isConnected) return;

    // 1. 获取当前真实北京时间
    struct tm ti;
    bool timeValid = getLocalTime(&ti, 20);

    String autoOn = storage.getAutoOnTime();
    String autoOff = storage.getAutoOffTime();
    bool isSleepTime = false;

    if (timeValid) {
        isSleepTime = isInSleepWindow(ti.tm_hour, ti.tm_min, autoOn, autoOff);
    }

    // 2. 定时开/关机任务触发 (每分钟比对一次)
    if (timeValid && ti.tm_min != lastCheckedMinute) {
        lastCheckedMinute = ti.tm_min;

        char curTimeBuf[8];
        snprintf(curTimeBuf, sizeof(curTimeBuf), "%02d:%02d", ti.tm_hour, ti.tm_min);
        String curTimeStr = String(curTimeBuf);

        if (autoOn.length() == 5 && curTimeStr == autoOn) {
            Serial.println("[调度] 到达定时开机时间 -> 点亮屏幕");
            BleController::setPower(true);
        } else if (autoOff.length() == 5 && curTimeStr == autoOff) {
            Serial.println("[调度] 到达定时关机时间 -> 熄屏关机");
            BleController::setPower(false);
        }
    }

    // ==============================================================
    // 核心优化保护区：
    // 条件1: 屏幕处于关机状态 (currentPower == false)
    // 条件2: 正处于关机至开机的睡觉时间段 (isSleepTime == true)
    // 只要满足任意一条，坚决不触发轮巡，不发任何蓝牙包，安心睡觉！
    // ==============================================================
    if (!BleController::currentPower || isSleepTime) {
        return;
    }

    // 3. 时钟样式自动轮换 (仅在白天、开机状态下才会执行)
    int cycleSec = storage.getClockCycleSec();
    if (cycleSec > 0) {
        if (millis() - lastClockCycleMs >= ((unsigned long)cycleSec * 1000)) {
            lastClockCycleMs = millis();

            // 算法优化：严格在 1 ~ 8 循环，绝不触发会导致黑屏的样式 0
            uint8_t nextStyle = (BleController::currentClockStyle % 8) + 1;
            
            if (timeValid) {
                BleController::setClockMode(nextStyle, false, ti.tm_hour, ti.tm_min, ti.tm_sec);
            } else {
                BleController::setClockMode(nextStyle, false);
            }
            Serial.printf("[调度] 白天正常轮换时钟样式 -> %d\n", nextStyle);
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    Serial.println("\n=================================");
    Serial.println("  iPixel 智能中控 (睡眠保护增强版)  ");
    Serial.println("=================================");

    storage.begin();

    String ssid = storage.getSSID();
    String pass = storage.getPass();
    bool wifiConnected = false;

    // 连接家庭 Wi-Fi
    if (ssid.length() > 0) {
        Serial.printf("[WiFi] 正在连接: [%s]\n", ssid.c_str());

        WiFi.disconnect(true);
        delay(100);

        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        WiFi.persistent(true);
        WiFi.setTxPower(WIFI_POWER_17dBm); // 稳定供电
        WiFi.begin(ssid.c_str(), pass.c_str());

        int retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries < 40) {
            delay(500);
            Serial.print(".");
            retries++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            wifiConnected = true;
            Serial.println("\n[WiFi] 连入成功！IP: http://" + WiFi.localIP().toString());
            configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
        }
    }

    // 失败退回 AP 配网热点
    if (!wifiConnected) {
        isApMode = true;
        Serial.println("[AP] 启动配网热点: ESP32-iPixel-Setup");
        WiFi.mode(WIFI_OFF);
        delay(100);

        WiFi.mode(WIFI_AP);
        WiFi.setSleep(false);
        WiFi.setTxPower(WIFI_POWER_17dBm);

        IPAddress apIP(192, 168, 4, 1);
        WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        WiFi.softAP("ESP32-iPixel-Setup", "", 6);

        esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
        esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    }

    webManager.begin();
    Serial.println("[HTTP] Web 服务就绪！");

    String targetMac = storage.getBleMac();
    BleController::init(targetMac);
}

void loop() {
    webManager.handleClient();
    BleController::loop();
    checkResetButton();
    handleScheduler(); // 自动化调度（内置关机/夜间睡眠静音保护）

    // WiFi 断线自愈
    if (!isApMode) {
        if (millis() - lastWifiCheckMs >= 15000) {
            lastWifiCheckMs = millis();
            if (WiFi.status() != WL_CONNECTED) {
                WiFi.reconnect();
            }
        }
    }

    delay(2);
}