// 必须在最顶端
#include "libssh_esp32.h"
#include <libssh/libssh.h>

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "esp_wifi.h"
#include "storage.h"
#include "ssh_client.h"
#include "web_server.h"

Storage storage;
WebServerManager webManager;
LinuxStatus currentStatus;

bool isApMode = false;
unsigned long lastWifiCheckMs = 0;
const int BUTTON_PIN = 9; // 板载 BOOT 按键

// 长按按键复位
void checkResetButton() {
    static unsigned long pressStart = 0;
    static bool isPressing = false;

    if (digitalRead(BUTTON_PIN) == LOW) {
        if (!isPressing) {
            isPressing = true;
            pressStart = millis();
        } else if (millis() - pressStart >= 3000) {
            Serial.println("\n[RESET] 长按 3 秒，抹除 WiFi 进入 AP 模式...");
            storage.clearWifi();
            delay(300);
            ESP.restart();
        }
    } else {
        isPressing = false;
    }
}

// 统计 ESP32 CPU 占用率
float espCpuUsage = 0.0f;
static unsigned long busyMicrosAcc = 0;
static unsigned long lastCalcMs = 0;

void updateEspCpuLoad(unsigned long busyUs) {
    busyMicrosAcc += busyUs;
    unsigned long now = millis();
    if (now - lastCalcMs >= 1000) {
        unsigned long elapsedUs = (now - lastCalcMs) * 1000;
        if (elapsedUs > 0) {
            espCpuUsage = (float)busyMicrosAcc * 100.0f / (float)elapsedUs;
            if (espCpuUsage > 100.0f) espCpuUsage = 100.0f;
            if (espCpuUsage < 1.0f) espCpuUsage = 1.0f;
        }
        busyMicrosAcc = 0;
        lastCalcMs = now;
    }
}

String getCurrentTimeString() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 100)) return "已记录";
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
    return String(buf);
}

void setup() {
    Serial.begin(115200);
    delay(500);

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    Serial.println("\n=================================");
    Serial.println(" ESP32-C3 极速低功耗稳定版启动   ");
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
        // 关键优化：调低峰值功率，避免拉垮充电器/电脑口，杜绝死机
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
            SshClient::init();
        } else {
            Serial.println("\n[WiFi] 连入失败，准备退回 AP 热点...");
        }
    }

    // 热点模式
    if (!wifiConnected) {
        isApMode = true;
        Serial.println("[AP] 启动纯净高稳配网热点...");
        
        WiFi.mode(WIFI_OFF);
        delay(150);

        WiFi.mode(WIFI_AP);
        WiFi.setSleep(false);
        WiFi.setTxPower(WIFI_POWER_17dBm); // 稳定射频输出

        IPAddress apIP(192, 168, 4, 1);
        WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
        WiFi.softAP("ESP32-C3-ADB-Setup", "", 6);

        esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
        esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

        Serial.println("[AP] 热点广播就绪: ESP32-C3-ADB-Setup (http://192.168.4.1)");
    }

    webManager.begin();
    Serial.println("[HTTP] Web 服务就绪！(完全异步非阻塞模式)");
}

void loop() {
    unsigned long loopStart = micros();

    webManager.handleClient();
    checkResetButton();

    // WiFi 断线重连
    if (!isApMode) {
        if (millis() - lastWifiCheckMs >= 15000) {
            lastWifiCheckMs = millis();
            if (WiFi.status() != WL_CONNECTED) {
                WiFi.reconnect();
            }
        }
    }

    // 统计 loop 占用的活跃 CPU 时间
    updateEspCpuLoad(micros() - loopStart);
    delay(2);
}