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

// 调度引擎时间戳
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
// 极速低功耗自动化调度器 (零性能损耗，在 loop() 中按时隙比对)
// ==============================================================
void handleScheduler() {
    if (!BleController::isConnected) return;

    // 1. 时钟样式自动轮换
    int cycleSec = storage.getClockCycleSec();
    if (cycleSec > 0) {
        if (millis() - lastClockCycleMs >= ((unsigned long)cycleSec * 1000)) {
            lastClockCycleMs = millis();
            // 样式 0 ~ 7 循环切换
            uint8_t nextStyle = (BleController::currentClockStyle + 1) % 8;
            
            struct tm ti;
            if (getLocalTime(&ti, 20)) {
                BleController::setClockMode(nextStyle, ti.tm_hour, ti.tm_min, ti.tm_sec);
            } else {
                BleController::setClockMode(nextStyle);
            }
            Serial.printf("[调度] 自动轮换时钟样式 -> %d\n", nextStyle);
        }
    }

    // 2. 定时开/关机 (基于真实北京时间，每分钟只比对一次)
    struct tm ti;
    if (getLocalTime(&ti, 20)) {
        if (ti.tm_min != lastCheckedMinute) {
            lastCheckedMinute = ti.tm_min;

            char curTimeBuf[8];
            snprintf(curTimeBuf, sizeof(curTimeBuf), "%02d:%02d", ti.tm_hour, ti.tm_min);
            String curTimeStr = String(curTimeBuf);

            String autoOn = storage.getAutoOnTime();
            String autoOff = storage.getAutoOffTime();

            if (autoOn.length() == 5 && curTimeStr == autoOn) {
                Serial.println("[调度] 命中定时开机任务 -> 正在点亮屏幕");
                BleController::setPower(true);
            } else if (autoOff.length() == 5 && curTimeStr == autoOff) {
                Serial.println("[调度] 命中定时关机任务 -> 正在熄屏关机");
                BleController::setPower(false);
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    Serial.println("\n=================================");
    Serial.println("  iPixel 智能矩阵全功能中控系统   ");
    Serial.println("=================================");

    storage.begin();

    String ssid = storage.getSSID();
    String pass = storage.getPass();
    bool wifiConnected = false;

    if (ssid.length() > 0) {
        Serial.printf("[WiFi] 正在连接: [%s]\n", ssid.c_str());

        WiFi.disconnect(true);
        delay(100);

        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        WiFi.persistent(true);
        WiFi.setTxPower(WIFI_POWER_17dBm);
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
    Serial.println("[HTTP] Web 服务 & REST API 已就绪！");

    String targetMac = storage.getBleMac();
    BleController::init(targetMac);
}

void loop() {
    webManager.handleClient();
    BleController::loop();
    checkResetButton();
    handleScheduler(); // 执行自动化比对（无阻塞毫秒级判断）

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