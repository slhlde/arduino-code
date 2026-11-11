#pragma once
#include <Arduino.h>
#include <Preferences.h>

class Storage {
private:
    Preferences prefs;

public:
    void begin() {
        prefs.begin("ipixel_cfg", false);
    }

    String getSSID() { return prefs.getString("ssid", ""); }
    void setSSID(const String& val) { prefs.putString("ssid", val); }

    String getPass() { return prefs.getString("pass", ""); }
    void setPass(const String& val) { prefs.putString("pass", val); }

    String getBleMac() { return prefs.getString("ble_mac", "23:44:90:fe:4a:79"); }
    void setBleMac(const String& val) { prefs.putString("ble_mac", val); }

    // 时钟自动轮播周期 (秒, 0 为关闭)
    int getClockCycleSec() { return prefs.getInt("clk_cycle", 0); }
    void setClockCycleSec(int val) { prefs.putInt("clk_cycle", val); }

    // 定时开关机时间 (格式 "HH:MM"，留空为不启用)
    String getAutoOnTime() { return prefs.getString("auto_on", ""); }
    void setAutoOnTime(const String& val) { prefs.putString("auto_on", val); }

    String getAutoOffTime() { return prefs.getString("auto_off", ""); }
    void setAutoOffTime(const String& val) { prefs.putString("auto_off", val); }

    void clearWifi() {
        prefs.remove("ssid");
        prefs.remove("pass");
    }
};