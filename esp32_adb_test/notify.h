#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace Notify {

inline bool send(
    const String& apiUrl,
    const String& title,
    const String& message,
    const String& deviceIp,
    int batteryLevel,
    const String& level = "critical",
    const String& project = "手机电量监控",
    int ttl = 3600
) {
    if (WiFi.status() != WL_CONNECTED || apiUrl.length() < 8) {
        return false;
    }

    String boundary = "----ESP32NotifyBoundary" + String(millis());
    String body;

    auto addField = [&](const String& name, const String& value) {
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"" + name + "\"\r\n\r\n";
        body += value + "\r\n";
    };

    addField("title", title);
    addField("message", message);
    addField("level", level);
    addField("project", project);
    addField("ip", deviceIp + " (电量: " + String(batteryLevel) + "%)");
    addField("ttl", String(ttl));
    body += "--" + boundary + "--\r\n";

    HTTPClient http;
    int httpCode = -1;

    if (apiUrl.startsWith("https://")) {
        WiFiClientSecure client;
        client.setInsecure(); // 跳过证书校验
        if (!http.begin(client, apiUrl)) return false;
        http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
        http.setTimeout(8000);
        httpCode = http.POST(body);
    } else {
        WiFiClient client;
        if (!http.begin(client, apiUrl)) return false;
        http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
        http.setTimeout(8000);
        httpCode = http.POST(body);
    }

    Serial.printf("[Notify] 推送结果 Code: %d\n", httpCode);
    http.end();
    return (httpCode >= 200 && httpCode < 300);
}

} // namespace Notify