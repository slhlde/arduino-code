#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <vector>
#include <map>

// 设备状态数据结构
struct DeviceStatus {
    int battery = -1;
    String lastTime = "--";
    String errorMsg = "未检测";
    bool connected = false;
    bool tested = false;
    bool notified = false;
};

class Storage {
private:
    Preferences prefs;

public:
    void begin() {
        prefs.begin("adb_store", false);
    }

    // --- WiFi ---
    String getSSID() { return prefs.getString("wifi_ssid", ""); }
    String getPass() { return prefs.getString("wifi_pass", ""); }
    void saveWiFi(const String& ssid, const String& pass) {
        prefs.putString("wifi_ssid", ssid);
        prefs.putString("wifi_pass", pass);
    }

    // --- ADB 私钥 ---
    String getAdbKey() { return prefs.getString("adb_key", ""); }
    bool hasAdbKey() { return getAdbKey().indexOf("PRIVATE KEY") != -1; }
    void saveAdbKey(const String& key) { prefs.putString("adb_key", key); }
    void clearAdbKey() { prefs.remove("adb_key"); }

    // --- 告警与轮询参数 ---
    String getNotifyUrl() { return prefs.getString("notif_url", "https://notify.yysresume.work/api/send"); }
    int getLowBatteryThreshold() { return prefs.getInt("low_th", 30); }
    int getPollInterval() { return prefs.getInt("poll_int", 60); }
    int getRetryCount() { return prefs.getInt("retry_cnt", 3); }

    void saveSettings(const String& url, int threshold, int interval, int retries) {
        prefs.putString("notif_url", url);
        prefs.putInt("low_th", threshold);
        prefs.putInt("poll_int", interval);
        prefs.putInt("retry_cnt", (retries < 1) ? 1 : ((retries > 10) ? 10 : retries));
    }

    // --- IP 列表 ---
    std::vector<String> getIPList() {
        std::vector<String> list;
        String raw = prefs.getString("dev_ips", "");
        if (raw.length() == 0) return list;

        int start = 0, end = raw.indexOf(',');
        while (end != -1) {
            String ip = raw.substring(start, end);
            ip.trim();
            if (ip.length() > 0) list.push_back(ip);
            start = end + 1;
            end = raw.indexOf(',', start);
        }
        String last = raw.substring(start);
        last.trim();
        if (last.length() > 0) list.push_back(last);
        return list;
    }

    bool addIP(const String& ip) {
        std::vector<String> list = getIPList();
        for (const auto& item : list) if (item == ip) return false;
        list.push_back(ip);
        saveIPList(list);
        return true;
    }

    void removeIP(const String& ip) {
        std::vector<String> list = getIPList();
        std::vector<String> newList;
        for (const auto& item : list) if (item != ip) newList.push_back(item);
        saveIPList(newList);
    }

    // --- 各设备状态持久化写入 Flash (重启不丢数据) ---
    void saveStatusCache(const std::map<String, DeviceStatus>& cache) {
        String out = "";
        for (const auto& pair : cache) {
            out += pair.first + "#" +
                   String(pair.second.battery) + "#" +
                   (pair.second.connected ? "1" : "0") + "#" +
                   (pair.second.tested ? "1" : "0") + "#" +
                   pair.second.lastTime + "#" +
                   pair.second.errorMsg + "\n";
        }
        prefs.putString("st_cache", out);
    }

    // --- 从 Flash 读取历史状态 ---
    void loadStatusCache(std::map<String, DeviceStatus>& cache) {
        String raw = prefs.getString("st_cache", "");
        if (raw.length() == 0) return;

        int start = 0;
        int end = raw.indexOf('\n');
        while (end != -1) {
            String line = raw.substring(start, end);
            line.trim();
            if (line.length() > 0) {
                int p1 = line.indexOf('#');
                int p2 = line.indexOf('#', p1 + 1);
                int p3 = line.indexOf('#', p2 + 1);
                int p4 = line.indexOf('#', p3 + 1);
                int p5 = line.indexOf('#', p4 + 1);

                if (p1 != -1 && p2 != -1 && p3 != -1 && p4 != -1 && p5 != -1) {
                    String ip = line.substring(0, p1);
                    DeviceStatus st;
                    st.battery = line.substring(p1 + 1, p2).toInt();
                    st.connected = (line.substring(p2 + 1, p3) == "1");
                    st.tested = (line.substring(p3 + 1, p4) == "1");
                    st.lastTime = line.substring(p4 + 1, p5);
                    st.errorMsg = line.substring(p5 + 1);
                    cache[ip] = st;
                }
            }
            start = end + 1;
            end = raw.indexOf('\n', start);
        }
    }

private:
    void saveIPList(const std::vector<String>& list) {
        String joined = "";
        for (size_t i = 0; i < list.size(); i++) {
            joined += list[i];
            if (i < list.size() - 1) joined += ",";
        }
        prefs.putString("dev_ips", joined);
    }
};

extern Storage storage;