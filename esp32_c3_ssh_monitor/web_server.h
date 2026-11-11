#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Update.h>
#include "storage.h"
#include "ssh_client.h"

extern Storage storage;
extern LinuxStatus currentStatus;
extern float espCpuUsage;

class WebServerManager {
private:
    WebServer server{80};

    String getEspUptime() {
        unsigned long sec = millis() / 1000;
        unsigned long d = sec / 86400;
        sec %= 86400;
        unsigned long h = sec / 3600;
        sec %= 3600;
        unsigned long m = sec / 60;
        unsigned long s = sec % 60;
        char buf[64];
        if (d > 0) {
            snprintf(buf, sizeof(buf), "%lud天 %02luh %02lum %02lus", d, h, m, s);
        } else {
            snprintf(buf, sizeof(buf), "%02luh %02lum %02lus", h, m, s);
        }
        return String(buf);
    }

    String getStatusJson() {
        LinuxServerConfig cfg = storage.getServerConfig();
        uint32_t totalHeap = ESP.getHeapSize();
        uint32_t freeHeap = ESP.getFreeHeap();
        uint32_t usedHeap = totalHeap - freeHeap;
        float heapPct = (totalHeap > 0) ? ((float)usedHeap * 100.0f / totalHeap) : 0;
        
        uint32_t flashTotal = ESP.getFlashChipSize();
        uint32_t flashUsed = ESP.getSketchSize();
        float flashPct = (flashTotal > 0) ? ((float)flashUsed * 100.0f / flashTotal) : 0;

        String json = "{";
        json += "\"isBusy\":" + String(SshClient::isBusy() ? "true" : "false") + ",";
        json += "\"esp\":{";
        json += "\"model\":\"" + String(ESP.getChipModel()) + " (" + String(ESP.getCpuFreqMHz()) + " MHz)\",";
        json += "\"cpuUsage\":\"" + String(espCpuUsage, 1) + "%\",";
        json += "\"cpuPct\":" + String(espCpuUsage, 1) + ",";
        json += "\"temp\":" + String(temperatureRead(), 1) + ",";
        json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
        json += "\"uptime\":\"" + getEspUptime() + "\",";
        json += "\"heapUsed\":" + String(usedHeap / 1024) + ",";
        json += "\"heapTotal\":" + String(totalHeap / 1024) + ",";
        json += "\"heapPct\":" + String(heapPct, 1) + ",";
        json += "\"flashUsed\":" + String(flashUsed / 1024) + ",";
        json += "\"flashTotal\":" + String(flashTotal / 1024 / 1024) + ",";
        json += "\"flashPct\":" + String(flashPct, 1);
        json += "},";
        json += "\"linux\":{";
        json += "\"connected\":" + String(currentStatus.connected ? "true" : "false") + ",";
        json += "\"host\":\"" + cfg.user + "@" + cfg.ip + ":" + String(cfg.port) + "\",";
        json += "\"os\":\"" + currentStatus.osInfo + "\",";
        json += "\"uptime\":\"" + currentStatus.uptime + "\",";
        json += "\"cpuUsage\":\"" + currentStatus.cpuUsage + "\",";
        json += "\"cpuPct\":" + String(currentStatus.cpuPct, 1) + ",";
        json += "\"memUsage\":\"" + currentStatus.memUsage + "\",";
        json += "\"diskUsage\":\"" + currentStatus.diskUsage + "\",";
        json += "\"errorMsg\":\"" + currentStatus.errorMsg + "\",";
        json += "\"lastCheck\":\"" + currentStatus.lastCheckTime + "\"";
        json += "}}";
        return json;
    }

    String getManifestJson() {
        return R"rawliteral({
  "name": "系统综合监控台",
  "short_name": "系统监控",
  "start_url": "/",
  "display": "standalone",
  "background_color": "#0b1120",
  "theme_color": "#0b1120",
  "icons": [
    {
      "src": "data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><rect width='100' height='100' rx='20' fill='%230284c7'/><text x='50%' y='68%' font-size='55' text-anchor='middle' fill='white'>🛰️</text></svg>",
      "sizes": "192x192 512x512",
      "type": "image/svg+xml"
    }
  ]
})rawliteral";
    }

    String getIndexHtml() {
        LinuxServerConfig cfg = storage.getServerConfig();
        String html = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
<title>系统综合监控台</title>
<link rel="manifest" href="/manifest.json">
<meta name="theme-color" content="#0b1120">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<style>
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #0b1120; color: #f8fafc; margin: 0; padding: 15px; }
  .container { max-width: 650px; margin: 0 auto; }
  .card { background: #1e293b; border-radius: 12px; padding: 18px; margin-bottom: 16px; box-shadow: 0 4px 6px -1px rgba(0,0,0,0.3); }
  h2 { margin-top: 0; color: #38bdf8; font-size: 20px; }
  h3 { margin-top: 0; color: #94a3b8; font-size: 15px; border-bottom: 1px solid #334155; padding-bottom: 8px; }
  .badge { display: inline-block; padding: 4px 10px; border-radius: 6px; font-weight: bold; font-size: 12px; }
  .badge-ok { background: #059669; color: #fff; }
  .badge-fail { background: #dc2626; color: #fff; }
  .item { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid #334155; font-size: 13.5px; }
  .item:last-child { border-bottom: none; }
  .item .val { font-family: monospace; color: #38bdf8; font-weight: 600; text-align: right; }
  .progress-bg { width: 100%; background: #334155; border-radius: 6px; height: 8px; margin: 6px 0 10px 0; overflow: hidden; }
  .progress-bar { height: 100%; background: #38bdf8; border-radius: 6px; transition: width 0.3s; }
  .btn { display: inline-block; width: 100%; box-sizing: border-box; background: #0284c7; color: white; border: none; padding: 12px; border-radius: 8px; font-size: 15px; font-weight: bold; cursor: pointer; text-align: center; text-decoration: none; margin-top: 8px; }
  .btn:active { transform: scale(0.98); }
  .btn-sub { background: #475569; }
  input { width: 100%; box-sizing: border-box; padding: 10px; margin: 6px 0 12px 0; background: #0f172a; border: 1px solid #334155; border-radius: 6px; color: #fff; font-size: 14px; }
  .tip-box { background: rgba(56, 189, 248, 0.1); border-left: 3px solid #38bdf8; padding: 8px 12px; font-size: 12px; color: #94a3b8; border-radius: 0 6px 6px 0; margin-bottom: 15px; }
</style>
</head>
<body>
<div class="container">
  <h2>🛰️ 系统综合监控台</h2>
  
  <div class="tip-box">
    📱 <b>PWA 独立 App 说明</b>：在浏览器菜单中选择 <b>“添加到主屏幕”</b> 即可生成无边框独立 App。
  </div>

  <!-- 1. ESP32-C3 本地状态 -->
  <div class="card">
    <div style="display:flex; justify-content:space-between; align-items:center;">
      <h3 style="margin:0; border:none;">⚡ ESP32-C3 核心状态</h3>
      <span class="badge badge-ok">运行极速</span>
    </div>
    <div class="item" style="margin-top:10px;"><span>芯片规格:</span><span class="val" id="espModel">-</span></div>
    <div class="item"><span>芯片核心温度:</span><span class="val" id="espTemp">-</span></div>
    <div class="item"><span>WiFi 信号 (RSSI):</span><span class="val" id="espRssi">-</span></div>
    <div class="item"><span>连续开机时长:</span><span class="val" id="espUptime">-</span></div>
    
    <div style="margin-top:10px; font-size:13px; color:#cbd5e1;">ESP32 CPU 占用率: <span id="espCpuTxt">-</span></div>
    <div class="progress-bg"><div class="progress-bar" id="espCpuBar" style="width: 0%; background:#10b981;"></div></div>

    <div style="font-size:13px; color:#cbd5e1;">SRAM 运行内存: <span id="espHeapTxt">-</span></div>
    <div class="progress-bg"><div class="progress-bar" id="espHeapBar" style="width: 0%;"></div></div>

    <div style="font-size:13px; color:#cbd5e1;">Flash 固件存储: <span id="espFlashTxt">-</span></div>
    <div class="progress-bg"><div class="progress-bar" id="espFlashBar" style="width: 0%; background:#a855f7;"></div></div>
  </div>

  <!-- 2. 远程 Linux 目标状态 -->
  <div class="card">
    <div style="display:flex; justify-content:space-between; align-items:center;">
      <h3 style="margin:0; border:none;">🐧 远程 Linux 状态</h3>
      <span class="badge" id="linuxBadge">就绪</span>
    </div>
    <div class="item" style="margin-top:10px;"><span>目标主机:</span><span class="val" id="linuxHost">-</span></div>
    <div class="item"><span>Linux 内核:</span><span class="val" id="linuxOs">-</span></div>
    <div class="item"><span>运行负载:</span><span class="val" id="linuxUptime">-</span></div>
    
    <div style="margin-top:10px; font-size:13px; color:#cbd5e1;">Linux CPU 占用率: <span id="linuxCpuTxt">-</span></div>
    <div class="progress-bg"><div class="progress-bar" id="linuxCpuBar" style="width: 0%; background:#f59e0b;"></div></div>

    <div class="item"><span>内存占用:</span><span class="val" id="linuxMem">-</span></div>
    <div class="item"><span>根盘占用:</span><span class="val" id="linuxDisk">-</span></div>
    <div class="item"><span>检测状态:</span><span class="val" id="linuxErr">-</span></div>
    <div class="item"><span>上次更新:</span><span class="val" id="linuxTime">-</span></div>
    <button class="btn" id="btnRefresh" onclick="manualRefresh()">⚡ 立即通过 SSH 测速刷新</button>
  </div>

  <!-- 3. Web OTA -->
  <div class="card">
    <h3>🚀 Web OTA 固件在线升级</h3>
    <input type="file" id="otaFile" accept=".bin" style="margin-bottom:8px;">
    <button class="btn" style="background:#8b5cf6;" onclick="uploadOTA()">📤 上传 .bin 刷机升级</button>
    <div id="otaProgress" style="display:none; margin-top:12px;">
      <div style="font-size:13px; color:#cbd5e1;">升级进度: <span id="otaPct">0%</span></div>
      <div class="progress-bg"><div class="progress-bar" id="otaBar" style="width:0%; background:#8b5cf6;"></div></div>
    </div>
  </div>

  <!-- 4. 配置中心 -->
  <div class="card">
    <h3>⚙️ 配置中心</h3>
    <div style="font-size:12px; color:#94a3b8; margin-bottom:10px;">💡 提示：物理按住板载 BOOT 键 3 秒，可直接清空 WiFi 恢复初始热点。</div>
    <form action="/save" method="POST">
      <label>WiFi 名称 (SSID):</label>
      <input type="text" name="ssid" value=")rawliteral" + storage.getSSID() + R"rawliteral(">
      
      <label>WiFi 密码:</label>
      <input type="password" name="pass" value=")rawliteral" + storage.getPass() + R"rawliteral(">

      <label>Linux 主机 IP / 域名:</label>
      <input type="text" name="ssh_ip" value=")rawliteral" + cfg.ip + R"rawliteral(">

      <label>SSH 端口 (默认 22):</label>
      <input type="number" name="ssh_port" value=")rawliteral" + String(cfg.port) + R"rawliteral(">

      <label>SSH 用户名:</label>
      <input type="text" name="ssh_user" value=")rawliteral" + cfg.user + R"rawliteral(">

      <label>SSH 密码:</label>
      <input type="password" name="ssh_pass" value=")rawliteral" + cfg.pass + R"rawliteral(">

      <button type="submit" class="btn btn-sub">💾 保存配置并重启</button>
    </form>
  </div>
</div>

<script>
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('/sw.js').catch(()=>{});
}

function updateUI(data) {
  document.getElementById('espModel').innerText = data.esp.model;
  document.getElementById('espCpuTxt').innerText = data.esp.cpuUsage;
  document.getElementById('espCpuBar').style.width = data.esp.cpuPct + '%';
  document.getElementById('espTemp').innerText = data.esp.temp + ' °C';
  document.getElementById('espRssi').innerText = data.esp.rssi + ' dBm';
  document.getElementById('espUptime').innerText = data.esp.uptime;
  document.getElementById('espHeapTxt').innerText = data.esp.heapUsed + ' KB / ' + data.esp.heapTotal + ' KB (' + data.esp.heapPct + '%)';
  document.getElementById('espHeapBar').style.width = data.esp.heapPct + '%';
  document.getElementById('espFlashTxt').innerText = data.esp.flashUsed + ' KB / ' + data.esp.flashTotal + ' MB (' + data.esp.flashPct + '%)';
  document.getElementById('espFlashBar').style.width = data.esp.flashPct + '%';

  const b = document.getElementById('linuxBadge');
  b.className = 'badge ' + (data.linux.connected ? 'badge-ok' : 'badge-fail');
  b.innerText = data.linux.connected ? '正常' : (data.isBusy ? '测速中' : '就绪/离线');

  document.getElementById('linuxHost').innerText = data.linux.host;
  document.getElementById('linuxOs').innerText = data.linux.os;
  document.getElementById('linuxUptime').innerText = data.linux.uptime;
  document.getElementById('linuxCpuTxt').innerText = data.linux.cpuUsage;
  document.getElementById('linuxCpuBar').style.width = Math.min(100, Math.max(0, data.linux.cpuPct)) + '%';
  document.getElementById('linuxMem').innerText = data.linux.memUsage;
  document.getElementById('linuxDisk').innerText = data.linux.diskUsage;
  document.getElementById('linuxErr').innerText = data.linux.errorMsg;
  document.getElementById('linuxTime').innerText = data.linux.lastCheck;

  const btn = document.getElementById('btnRefresh');
  if (data.isBusy) {
    btn.innerText = '🔄 正在建立 SSH 测速中 (后台非阻塞)...';
    btn.disabled = true;
  } else {
    btn.innerText = '⚡ 立即通过 SSH 测速刷新';
    btn.disabled = false;
  }
}

function fetchStatus() {
  fetch('/api/status')
    .then(r => r.json())
    .then(data => updateUI(data))
    .catch(()=>{});
}

function manualRefresh() {
  fetch('/api/trigger_ssh')
    .then(() => fetchStatus())
    .catch(() => {});
}

function uploadOTA() {
  const fileInput = document.getElementById('otaFile');
  if (!fileInput.files || fileInput.files.length === 0) {
    alert('请先选择 .bin 固件！');
    return;
  }
  const file = fileInput.files[0];
  const btn = event.target;
  btn.disabled = true;
  document.getElementById('otaProgress').style.display = 'block';

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/update');
  xhr.upload.onprogress = function(e) {
    if (e.lengthComputable) {
      const pct = Math.round((e.loaded / e.total) * 100);
      document.getElementById('otaPct').innerText = pct + '%';
      document.getElementById('otaBar').style.width = pct + '%';
    }
  };
  xhr.onload = function() {
    if (xhr.status === 200 && xhr.responseText === 'OK') {
      alert('固件升级成功！设备正在重启...');
      setTimeout(() => location.reload(), 4000);
    } else {
      alert('升级失败，请重试');
      btn.disabled = false;
    }
  };
  const fd = new FormData();
  fd.append('update', file);
  xhr.send(fd);
}

fetchStatus();
// 仅轻量拉取本机硬件状态，零算力损耗
setInterval(fetchStatus, 3000);
</script>
</body>
</html>
)rawliteral";
        return html;
    }

public:
    void begin() {
        server.on("/", HTTP_GET, [this]() {
            server.send(200, "text/html", getIndexHtml());
        });

        server.on("/manifest.json", HTTP_GET, [this]() {
            server.send(200, "application/json", getManifestJson());
        });

        server.on("/sw.js", HTTP_GET, [this]() {
            server.send(200, "application/javascript", "self.addEventListener('fetch', function(e){});");
        });

        server.on("/api/status", HTTP_GET, [this]() {
            server.send(200, "application/json", getStatusJson());
        });

        // 核心改造：点击测速后立即返回 OK，后台慢慢测，绝不卡网页！
        server.on("/api/trigger_ssh", HTTP_GET, [this]() {
            SshClient::triggerAsyncCheck(currentStatus);
            server.send(200, "text/plain", "OK");
        });

        server.on("/update", HTTP_POST, [this]() {
            server.sendHeader("Connection", "close");
            server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
            delay(500);
            ESP.restart();
        }, [this]() {
            HTTPUpload& upload = server.upload();
            if (upload.status == UPLOAD_FILE_START) {
                Update.begin(UPDATE_SIZE_UNKNOWN);
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                Update.write(upload.buf, upload.currentSize);
            } else if (upload.status == UPLOAD_FILE_END) {
                Update.end(true);
            }
        });

        server.on("/save", HTTP_POST, [this]() {
            if (server.hasArg("ssid")) storage.setSSID(server.arg("ssid"));
            if (server.hasArg("pass")) storage.setPass(server.arg("pass"));

            LinuxServerConfig cfg;
            cfg.ip = server.arg("ssh_ip");
            cfg.port = server.arg("ssh_port").toInt();
            cfg.user = server.arg("ssh_user");
            cfg.pass = server.arg("ssh_pass");
            storage.setServerConfig(cfg);

            server.send(200, "text/html; charset=utf-8", "<h3>保存成功！正在重启...</h3><script>setTimeout(function(){location.href='/';}, 3000);</script>");
            delay(500);
            ESP.restart();
        });

        server.begin();
    }

    void handleClient() {
        server.handleClient();
    }
};