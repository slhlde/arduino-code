#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Update.h>
#include <time.h>
#include "storage.h"
#include "ble_controller.h"

extern Storage storage;

class WebServerManager {
private:
    WebServer server{80};

    String getUptime() {
        unsigned long sec = millis() / 1000;
        unsigned long d = sec / 86400; sec %= 86400;
        unsigned long h = sec / 3600;  sec %= 3600;
        unsigned long m = sec / 60;    unsigned long s = sec % 60;
        char buf[48];
        snprintf(buf, sizeof(buf), "%lud %02luh %02lum %02lus", d, h, m, s);
        return String(buf);
    }

    String getStatusJson() {
        uint32_t totalHeap = ESP.getHeapSize();
        uint32_t freeHeap = ESP.getFreeHeap();
        uint32_t usedHeap = totalHeap - freeHeap;
        float heapPct = (totalHeap > 0) ? ((float)usedHeap * 100.0f / totalHeap) : 0;

        String json = "{";
        json += "\"ble_connected\":" + String(BleController::isConnected ? "true" : "false") + ",";
        json += "\"ble_rssi\":" + String((BleController::pClient && BleController::isConnected) ? BleController::pClient->getRssi() : BleController::targetRSSI) + ",";
        json += "\"power\":" + String(BleController::currentPower ? "true" : "false") + ",";
        json += "\"brightness\":" + String(BleController::currentBrightness) + ",";
        json += "\"clock_style\":" + String(BleController::currentClockStyle) + ",";
        json += "\"clock_cycle_sec\":" + String(storage.getClockCycleSec()) + ",";
        json += "\"auto_on\":\"" + storage.getAutoOnTime() + "\",";
        json += "\"auto_off\":\"" + storage.getAutoOffTime() + "\",";
        json += "\"ble_mac\":\"" + BleController::targetMacStr + "\",";
        json += "\"ble_log\":\"" + BleController::lastLog + "\",";
        json += "\"ble_notify\":\"" + BleController::lastNotifyHex + "\",";
        json += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
        json += "\"heap_pct\":" + String(heapPct, 1) + ",";
        json += "\"heap_kb\":" + String(usedHeap / 1024) + ",";
        json += "\"temp\":" + String(temperatureRead(), 1) + ",";
        json += "\"uptime\":\"" + getUptime() + "\"";
        json += "}";
        return json;
    }

    String getIndexHtml() {
        String html = R"rawliteral(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
<title>iPixel 像素屏中控台</title>
<link rel="manifest" href="/manifest.json">
<meta name="theme-color" content="#0b1120">
<style>
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #0b1120; color: #f8fafc; margin: 0; padding: 12px; }
  .container { max-width: 600px; margin: 0 auto; }
  .card { background: #1e293b; border-radius: 12px; padding: 16px; margin-bottom: 14px; box-shadow: 0 4px 6px -1px rgba(0,0,0,0.3); }
  h2 { margin: 0 0 10px 0; color: #38bdf8; font-size: 19px; display: flex; justify-content: space-between; align-items: center; }
  h3 { margin: 0 0 10px 0; color: #94a3b8; font-size: 15px; border-bottom: 1px solid #334155; padding-bottom: 6px; }
  .badge { display: inline-block; padding: 3px 8px; border-radius: 6px; font-weight: bold; font-size: 12px; }
  .badge-ok { background: #059669; color: #fff; }
  .badge-fail { background: #dc2626; color: #fff; }
  .grid-btn { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; margin: 8px 0; }
  .btn { background: #0284c7; color: white; border: none; padding: 10px; border-radius: 8px; font-size: 14px; font-weight: bold; cursor: pointer; text-align: center; }
  .btn:active { transform: scale(0.97); }
  .btn-danger { background: #dc2626; }
  .btn-success { background: #16a34a; }
  .btn-purple { background: #9333ea; }
  .btn-sub { background: #475569; width: 100%; margin-top: 8px; }
  .item { display: flex; justify-content: space-between; padding: 6px 0; border-bottom: 1px solid #334155; font-size: 13px; }
  .val { font-family: monospace; color: #38bdf8; }
  input[type=text], input[type=password], input[type=number], select { width: 100%; box-sizing: border-box; padding: 9px; margin: 4px 0 10px 0; background: #0f172a; border: 1px solid #334155; border-radius: 6px; color: #fff; }
  .slider-box { display: flex; align-items: center; gap: 10px; margin: 8px 0; }
  .slider-box input { flex: 1; }
  .log-box { background: #0f172a; border-radius: 6px; padding: 8px; font-family: monospace; font-size: 12px; color: #a5f3fc; word-break: break-all; min-height: 24px; }
  .row-flex { display: flex; gap: 8px; }
</style>
</head>
<body>
<div class="container">
  <h2>
    <span>✨ iPixel 像素屏中控</span>
    <span class="badge" id="bleBadge">连接中...</span>
  </h2>

  <!-- 1. 基础控制 -->
  <div class="card">
    <h3>📱 屏幕快速控制</h3>
    <div style="display:flex; gap:8px;">
      <button class="btn btn-success" style="flex:1;" onclick="callApi('/api/power?state=1')">💡 开机</button>
      <button class="btn btn-danger" style="flex:1;" onclick="callApi('/api/power?state=0')">🌑 关机</button>
      <button class="btn btn-purple" style="flex:1;" onclick="callApi('/api/sync_ntp')">🕒 NTP校时</button>
    </div>

    <div style="margin-top:12px; font-size:13px; color:#cbd5e1;">亮度调节: <span id="bVal">60%</span></div>
    <div class="slider-box">
      <input type="range" id="bSlider" min="1" max="100" value="60" onchange="callApi('/api/brightness?val=' + this.value)">
    </div>

    <div style="font-size:13px; color:#cbd5e1; margin-top:8px;">时钟样式切换 (32×32 专属 1-8):</div>
    <div class="grid-btn">
      <button class="btn" onclick="callApi('/api/clock?style=1')">样式 1</button>
      <button class="btn" onclick="callApi('/api/clock?style=2')">样式 2</button>
      <button class="btn" onclick="callApi('/api/clock?style=3')">样式 3</button>
      <button class="btn" onclick="callApi('/api/clock?style=4')">样式 4</button>
      <button class="btn" onclick="callApi('/api/clock?style=5')">样式 5</button>
      <button class="btn" onclick="callApi('/api/clock?style=6')">样式 6</button>
      <button class="btn" onclick="callApi('/api/clock?style=7')">样式 7</button>
      <button class="btn" onclick="callApi('/api/clock?style=8')">样式 8</button>
    </div>
  </div>

  <!-- 2. 发送自定义文字 -->
  <div class="card">
    <h3>💬 文字发生器</h3>
    <label style="font-size:12px; color:#94a3b8;">文本内容:</label>
    <input type="text" id="txtMsg" placeholder="IP:105" value="IP:105">

    <div class="row-flex">
      <div style="flex:1;">
        <label style="font-size:12px; color:#94a3b8;">字号选择:</label>
        <select id="txtSize">
          <option value="2" selected>2: 纵向满屏大字 (16×32)</option>
          <option value="1">1: 标准大小 (8×16)</option>
        </select>
      </div>
      <div style="flex:1;">
        <label style="font-size:12px; color:#94a3b8;">动画模式:</label>
        <select id="txtAnim">
          <option value="1">1: 向左跑马灯</option>
          <option value="2">2: 静态居中</option>
        </select>
      </div>
    </div>

    <div class="row-flex">
      <div style="flex:1;">
        <label style="font-size:12px; color:#94a3b8;">速度 (1-100):</label>
        <input type="number" id="txtSpeed" value="90" min="1" max="100">
      </div>
      <div style="flex:1;">
        <label style="font-size:12px; color:#94a3b8;">颜色 (HEX):</label>
        <input type="text" id="txtColor" value="FF5500">
      </div>
    </div>

    <button class="btn" style="width:100%;" onclick="sendTextApi()">🚀 发送文字上屏</button>
  </div>

  <!-- 3. 定时自动化任务 -->
  <div class="card">
    <h3>⏰ 自动化与定时调度</h3>
    <form action="/save_schedule" method="POST">
      <label style="font-size:12px; color:#94a3b8;">时钟样式自动轮换周期 (秒, 填 0 禁用):</label>
      <input type="number" name="clk_cycle" value=")rawliteral" + String(storage.getClockCycleSec()) + R"rawliteral(">

      <div class="row-flex">
        <div style="flex:1;">
          <label style="font-size:12px; color:#94a3b8;">定时开机 (如 08:00):</label>
          <input type="text" name="auto_on" placeholder="08:00" value=")rawliteral" + storage.getAutoOnTime() + R"rawliteral(">
        </div>
        <div style="flex:1;">
          <label style="font-size:12px; color:#94a3b8;">定时关机 (如 23:30):</label>
          <input type="text" name="auto_off" placeholder="23:30" value=")rawliteral" + storage.getAutoOffTime() + R"rawliteral(">
        </div>
      </div>
      <button type="submit" class="btn btn-sub">💾 保存调度规则</button>
    </form>
  </div>

  <!-- 4. 蓝牙 HEX 调试 -->
  <div class="card">
    <h3>🛠️ 蓝牙底层 HEX 调试</h3>
    <div style="display:flex; gap:6px;">
      <input type="text" id="rawHex" placeholder="05 00 07 01 01" style="margin:0;">
      <button class="btn" style="white-space:nowrap;" onclick="sendRawHex()">发送 HEX</button>
    </div>
    <div style="font-size:12px; color:#94a3b8; margin:8px 0 4px 0;">FA03 监听数据:</div>
    <div class="log-box" id="notifyBox">-</div>
    <div style="display:flex; justify-content:space-between; align-items:center; margin-top:8px;">
      <span style="font-size:12px; color:#94a3b8;">通信日志: <span id="bleLog">-</span></span>
      <button class="btn" style="padding:4px 8px; font-size:11px;" onclick="callApi('/api/reconnect')">重连</button>
    </div>
  </div>

  <!-- 5. 硬件与配网设置 -->
  <div class="card">
    <h3>⚙️ 设备设置 & OTA 升级</h3>
    <div class="item"><span>目标蓝牙 MAC:</span><span class="val" id="bleMac">-</span></div>
    <div class="item"><span>蓝牙信号:</span><span class="val" id="bleRssi">- dBm</span></div>
    <div class="item"><span>运行时间:</span><span class="val" id="uptime">-</span></div>

    <form action="/save_cfg" method="POST" style="margin-top:10px;">
      <input type="text" name="ble_mac" placeholder="屏幕蓝牙 MAC" value=")rawliteral" + storage.getBleMac() + R"rawliteral(">
      <input type="text" name="ssid" placeholder="WiFi 名称" value=")rawliteral" + storage.getSSID() + R"rawliteral(">
      <input type="password" name="pass" placeholder="WiFi 密码" value=")rawliteral" + storage.getPass() + R"rawliteral(">
      <button type="submit" class="btn btn-sub">💾 保存 WiFi & MAC</button>
    </form>

    <div style="margin-top:14px; border-top:1px solid #334155; padding-top:10px;">
      <input type="file" id="otaFile" accept=".bin" style="margin-bottom:6px;">
      <button class="btn btn-purple" style="width:100%;" onclick="uploadOTA()">无线 OTA 固件升级</button>
    </div>
  </div>
</div>

<script>
function callApi(url) { fetch(url).then(r=>r.text()).then(()=>fetchStatus()); }

function sendTextApi() {
  const msg = document.getElementById('txtMsg').value.trim();
  const size = document.getElementById('txtSize').value;
  const anim = document.getElementById('txtAnim').value;
  const speed = document.getElementById('txtSpeed').value;
  const color = document.getElementById('txtColor').value.trim();
  if(!msg) { alert('请输入内容'); return; }

  const url = '/api/text?msg=' + encodeURIComponent(msg) + 
              '&size=' + size + 
              '&anim=' + anim + 
              '&speed=' + speed + 
              '&color=' + color;

  fetch(url).then(r=>r.json()).then(res=>{
    alert(res.status === 'ok' ? '文字已成功上屏！' : '发送失败，请检查蓝牙');
    fetchStatus();
  });
}

function sendRawHex() {
  const hex = document.getElementById('rawHex').value.trim();
  if(!hex) return;
  fetch('/api/raw_ble?hex=' + encodeURIComponent(hex)).then(r=>r.text()).then(t=>alert(t));
}

function fetchStatus() {
  fetch('/api/status').then(r=>r.json()).then(d=>{
    const b = document.getElementById('bleBadge');
    b.className = 'badge ' + (d.ble_connected ? 'badge-ok' : 'badge-fail');
    b.innerText = d.ble_connected ? '蓝牙已连通' : '未连接/搜索中';

    document.getElementById('bleMac').innerText = d.ble_mac;
    document.getElementById('bleRssi').innerText = d.ble_rssi + ' dBm';
    document.getElementById('uptime').innerText = d.uptime;
    document.getElementById('bleLog').innerText = d.ble_log;
    document.getElementById('notifyBox').innerText = d.ble_notify;
  }).catch(()=>{});
}

document.getElementById('bSlider').oninput = function() {
  document.getElementById('bVal').innerText = this.value + '%';
};

function uploadOTA() {
  const file = document.getElementById('otaFile').files[0];
  if(!file) { alert('请选择固件'); return; }
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/update');
  xhr.onload = () => {
    alert(xhr.status == 200 ? '固件更新成功，正在重启...' : '更新失败');
    setTimeout(()=>location.reload(), 4000);
  };
  const fd = new FormData();
  fd.append('update', file);
  xhr.send(fd);
}

fetchStatus();
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
            server.send(200, "application/json", "{\"name\":\"iPixel中控\",\"short_name\":\"iPixel\",\"display\":\"standalone\",\"theme_color\":\"#0b1120\"}");
        });

        server.on("/api/status", HTTP_GET, [this]() {
            server.send(200, "application/json", getStatusJson());
        });

        server.on("/api/power", HTTP_GET, [this]() {
            if (server.hasArg("state")) {
                bool state = server.arg("state").toInt() == 1;
                BleController::setPower(state);
                server.send(200, "application/json", "{\"status\":\"ok\",\"power\":" + String(state ? "true" : "false") + "}");
            } else {
                server.send(400, "text/plain", "Missing state");
            }
        });

        server.on("/api/brightness", HTTP_GET, [this]() {
            if (server.hasArg("val")) {
                uint8_t val = (uint8_t)server.arg("val").toInt();
                BleController::setBrightness(val);
                server.send(200, "application/json", "{\"status\":\"ok\",\"brightness\":" + String(val) + "}");
            } else {
                server.send(400, "text/plain", "Missing val");
            }
        });

        server.on("/api/clock", HTTP_GET, [this]() {
            if (server.hasArg("style")) {
                uint8_t style = (uint8_t)server.arg("style").toInt();
                bool showDate = server.hasArg("date") ? (server.arg("date").toInt() == 1) : false;
                struct tm ti;
                if (getLocalTime(&ti, 50)) {
                    BleController::setClockMode(style, showDate, ti.tm_hour, ti.tm_min, ti.tm_sec);
                } else {
                    BleController::setClockMode(style, showDate);
                }
                server.send(200, "application/json", "{\"status\":\"ok\",\"clock_style\":" + String(BleController::currentClockStyle) + "}");
            } else {
                server.send(400, "text/plain", "Missing style");
            }
        });

        // 统一精简的 5 参数文本接口
        server.on("/api/text", [this]() {
            String msg = server.arg("msg");
            uint8_t anim = server.hasArg("anim") ? (uint8_t)server.arg("anim").toInt() : 1;
            uint8_t speed = server.hasArg("speed") ? (uint8_t)server.arg("speed").toInt() : 90;
            String color = server.hasArg("color") ? server.arg("color") : "FF5500";
            uint8_t size = server.hasArg("size") ? (uint8_t)server.arg("size").toInt() : 2; // 默认大字

            if (msg.length() == 0) {
                server.send(400, "text/plain", "Missing msg parameter");
                return;
            }

            bool ok = BleController::sendText(msg, anim, speed, color, size);
            server.send(200, "application/json", ok ? "{\"status\":\"ok\"}" : "{\"status\":\"fail\"}");
        });

        server.on("/api/schedule/clock_cycle", HTTP_GET, [this]() {
            if (server.hasArg("interval")) {
                int sec = server.arg("interval").toInt();
                storage.setClockCycleSec(sec);
                server.send(200, "application/json", "{\"status\":\"ok\",\"interval\":" + String(sec) + "}");
            } else {
                server.send(400, "text/plain", "Missing interval parameter");
            }
        });

        server.on("/api/sync_ntp", HTTP_GET, [this]() {
            struct tm ti;
            if (getLocalTime(&ti, 200)) {
                BleController::setClockMode(BleController::currentClockStyle, false, ti.tm_hour, ti.tm_min, ti.tm_sec);
                server.send(200, "application/json", "{\"status\":\"ok\",\"synced\":true}");
            } else {
                server.send(500, "text/plain", "NTP time not ready");
            }
        });

        server.on("/api/raw_ble", HTTP_GET, [this]() {
            bool ok = BleController::sendHex(server.arg("hex"));
            server.send(200, "text/plain", ok ? "发送成功" : "发送失败");
        });

        server.on("/api/reconnect", HTTP_GET, [this]() {
            BleController::reconnect();
            server.send(200, "text/plain", "OK");
        });

        server.on("/update", HTTP_POST, [this]() {
            server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
            delay(400);
            ESP.restart();
        }, [this]() {
            HTTPUpload& up = server.upload();
            if (up.status == UPLOAD_FILE_START) Update.begin(UPDATE_SIZE_UNKNOWN);
            else if (up.status == UPLOAD_FILE_WRITE) Update.write(up.buf, up.currentSize);
            else if (up.status == UPLOAD_FILE_END) Update.end(true);
        });

        server.on("/save_schedule", HTTP_POST, [this]() {
            if (server.hasArg("clk_cycle")) storage.setClockCycleSec(server.arg("clk_cycle").toInt());
            if (server.hasArg("auto_on")) storage.setAutoOnTime(server.arg("auto_on"));
            if (server.hasArg("auto_off")) storage.setAutoOffTime(server.arg("auto_off"));
            server.send(200, "text/html; charset=utf-8", "<h3>计划保存成功！</h3><script>setTimeout(()=>location.href='/', 1500);</script>");
        });

        server.on("/save_cfg", HTTP_POST, [this]() {
            if (server.hasArg("ssid")) storage.setSSID(server.arg("ssid"));
            if (server.hasArg("pass")) storage.setPass(server.arg("pass"));
            if (server.hasArg("ble_mac")) storage.setBleMac(server.arg("ble_mac"));
            server.send(200, "text/html; charset=utf-8", "<h3>配置已保存，正在重启...</h3><script>setTimeout(()=>location.href='/', 3500);</script>");
            delay(500);
            ESP.restart();
        });

        server.begin();
    }

    void handleClient() {
        server.handleClient();
    }
};