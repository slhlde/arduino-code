#pragma once
#include <Arduino.h>

extern "C" {
  #include <libssh/libssh.h>
}
#include "libssh_esp32.h"
#include "storage.h"

extern String getCurrentTimeString();
extern void updateEspCpuLoad(unsigned long busyUs);

struct LinuxStatus {
    bool connected = false;
    String errorMsg = "未检测 (点击下方按钮开始检测)";
    String lastCheckTime = "-";
    String osInfo = "-";
    String uptime = "-";
    String cpuUsage = "0.0%";
    float cpuPct = 0.0f;
    String memUsage = "-";
    String diskUsage = "-";
};

class SshClient {
private:
    static volatile bool isTesting;

    static void asyncWorker(void* arg) {
        Storage st;
        st.begin();
        LinuxServerConfig cfg = st.getServerConfig();
        LinuxStatus* status = (LinuxStatus*)arg;

        status->lastCheckTime = getCurrentTimeString();

        if (cfg.ip.length() == 0 || cfg.user.length() == 0) {
            status->connected = false;
            status->errorMsg = "未配置 IP 或 用户名";
            isTesting = false;
            vTaskDelete(NULL);
            return;
        }

        ssh_session session = ssh_new();
        if (session == NULL) {
            status->connected = false;
            status->errorMsg = "内存不足";
            isTesting = false;
            vTaskDelete(NULL);
            return;
        }

        int port = cfg.port > 0 ? cfg.port : 22;
        long timeout_sec = 4;
        ssh_options_set(session, SSH_OPTIONS_HOST, cfg.ip.c_str());
        ssh_options_set(session, SSH_OPTIONS_PORT, &port);
        ssh_options_set(session, SSH_OPTIONS_USER, cfg.user.c_str());
        ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout_sec);

        if (ssh_connect(session) != SSH_OK) {
            status->connected = false;
            status->errorMsg = "连接失败: " + String(ssh_get_error(session));
            ssh_free(session);
            isTesting = false;
            vTaskDelete(NULL);
            return;
        }

        if (ssh_userauth_password(session, NULL, cfg.pass.c_str()) != SSH_AUTH_SUCCESS) {
            status->connected = false;
            status->errorMsg = "密码认证失败";
            ssh_disconnect(session);
            ssh_free(session);
            isTesting = false;
            vTaskDelete(NULL);
            return;
        }

        ssh_channel channel = ssh_channel_new(session);
        if (channel == NULL || ssh_channel_open_session(channel) != SSH_OK) {
            status->connected = false;
            status->errorMsg = "通道打开失败";
            if (channel) ssh_channel_free(channel);
            ssh_disconnect(session);
            ssh_free(session);
            isTesting = false;
            vTaskDelete(NULL);
            return;
        }

        const char* cmd = "uname -srm; uptime; free -h | awk '/Mem:/{print $3\"/\"$2}'; df -h / | awk 'NR==2{print $3\"/\"$2\" (\"$5\")\"}'; top -bn1 | awk -F',' '/Cpu/{for(i=1;i<=NF;i++)if($i~/id/){gsub(/[a-zA-Z% ]/,\"\",$i); printf \"%.1f%%\", 100-$i}}'";
        if (ssh_channel_request_exec(channel, cmd) != SSH_OK) {
            status->connected = false;
            status->errorMsg = "执行失败";
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            ssh_disconnect(session);
            ssh_free(session);
            isTesting = false;
            vTaskDelete(NULL);
            return;
        }

        char buffer[256];
        String output = "";
        int nbytes;
        while ((nbytes = ssh_channel_read(channel, buffer, sizeof(buffer) - 1, 0)) > 0) {
            buffer[nbytes] = '\0';
            output += buffer;
            if (output.length() > 1024) break;
        }

        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);

        if (output.length() > 0) {
            status->connected = true;
            status->errorMsg = "正常";

            int idx1 = output.indexOf('\n');
            int idx2 = output.indexOf('\n', idx1 + 1);
            int idx3 = output.indexOf('\n', idx2 + 1);
            int idx4 = output.indexOf('\n', idx3 + 1);

            if (idx1 != -1) status->osInfo = output.substring(0, idx1);
            if (idx2 != -1) status->uptime = output.substring(idx1 + 1, idx2);
            if (idx3 != -1) status->memUsage = output.substring(idx2 + 1, idx3);
            if (idx4 != -1) status->diskUsage = output.substring(idx3 + 1, idx4);
            if (idx4 != -1 && idx4 + 1 < (int)output.length()) {
                status->cpuUsage = output.substring(idx4 + 1);
                status->cpuUsage.trim();
                status->cpuPct = status->cpuUsage.toFloat();
            }
        }

        isTesting = false; // 标记测试完成
        vTaskDelete(NULL); // 自我销毁
    }

public:
    static void init() {
        libssh_begin();
    }

    static bool isBusy() {
        return isTesting;
    }

    // 核心改造：非阻塞触发，启动后台任务后立刻返回 true，绝不卡死 loop
    static bool triggerAsyncCheck(LinuxStatus& status) {
        if (isTesting) return false; // 已经在测试中
        isTesting = true;
        status.errorMsg = "正在测速检测中...";

        xTaskCreate(asyncWorker, "ssh_async", 16384, &status, 1, NULL);
        return true;
    }
};

volatile bool SshClient::isTesting = false;