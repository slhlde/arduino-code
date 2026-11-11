#pragma once
#include <Arduino.h>
#include <Preferences.h>

struct LinuxServerConfig {
    String ip;
    int port;
    String user;
    String pass;
};

class Storage {
private:
    Preferences prefs;

public:
    void begin() {
        prefs.begin("ssh_cfg", false);
    }

    String getSSID() { return prefs.getString("ssid", ""); }
    void setSSID(const String& val) { prefs.putString("ssid", val); }

    String getPass() { return prefs.getString("pass", ""); }
    void setPass(const String& val) { prefs.putString("pass", val); }

    // 新增：长按按键时清空 Wi-Fi 凭据
    void clearWifi() {
        prefs.remove("ssid");
        prefs.remove("pass");
    }

    LinuxServerConfig getServerConfig() {
        LinuxServerConfig cfg;
        cfg.ip = prefs.getString("ssh_ip", "192.168.1.100");
        cfg.port = prefs.getInt("ssh_port", 22);
        cfg.user = prefs.getString("ssh_user", "root");
        cfg.pass = prefs.getString("ssh_pass", "");
        return cfg;
    }

    void setServerConfig(const LinuxServerConfig& cfg) {
        prefs.putString("ssh_ip", cfg.ip);
        prefs.putInt("ssh_port", cfg.port);
        prefs.putString("ssh_user", cfg.user);
        prefs.putString("ssh_pass", cfg.pass);
    }

    int getPollInterval() { return prefs.getInt("poll_int", 5); }
    void setPollInterval(int val) { prefs.putInt("poll_int", val); }
};