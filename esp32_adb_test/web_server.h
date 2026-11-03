#pragma once
#include <WebServer.h>
#include <map>
#include "storage.h"
#include "adb_client.h"

extern std::map<String, DeviceStatus> deviceCache;

inline String escapeJson(const String& s) {
    String out = "";
    out.reserve(s.length() + 32);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') continue;
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

class WebServerManager {
public:
    WebServer server;
    WebServerManager() : server(80) {}

    void begin() {
        server.on("/", HTTP_GET, [this]() { handleRoot(); });
        server.on("/api/wifi", HTTP_POST, [this]() { handleSaveWiFi(); });
        server.on("/api/key", HTTP_POST, [this]() { handleSaveKey(); });
        server.on("/api/settings", HTTP_POST, [this]() { handleSaveSettings(); });
        server.on("/api/devices", HTTP_GET, [this]() { handleGetDevices(); });
        server.on("/api/device/add", HTTP_POST, [this]() { handleAddDevice(); });
        server.on("/api/device/del", HTTP_POST, [this]() { handleDelDevice(); });
        server.on("/api/device/test", HTTP_GET, [this]() { handleTestDevice(); });
        server.begin();
    }

    void handleClient() { server.handleClient(); }

private:
    void handleRoot() {
        String currentSsid = storage.getSSID();
        String currentKey = storage.getAdbKey();
        String currentUrl = storage.getNotifyUrl();
        int currentTh = storage.getLowBatteryThreshold();
        int currentInterval = storage.getPollInterval();
        int currentRetries = storage.getRetryCount();

        String statusWifi = (currentSsid.length() > 0) ? 
            ((WiFi.status() == WL_CONNECTED) ? ("已连入 (" + WiFi.SSID() + ")") : ("未连入 (" + currentSsid + ")")) : 
            "未配置 (当前为配网热点)";
        String ipAddr = (WiFi.getMode() == WIFI_AP) ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
        String statusKey = storage.hasAdbKey() ? "<span style='color:#34c759' id='lbl_key'>已保存</span>" : "<span style='color:#ff3b30' id='lbl_key'>未保存</span>";

        String html = "";
        html.reserve(7500);

        // 注入 PWA WebApp 标签、桌面图标及离线独立运行声明
        html += R"rawhtml(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="theme-color" content="#0071e3">
<link rel="manifest" href="data:application/manifest+json;base64,eyJuYW1lIjoiQURC55S16YeP566h5a62Iiwic2hvcnRfbmFtZSI6IueUleiHj+euoWdlIiwic3RhcnRfdXJsIjoiLyIsImRpc3BsYXkiOiJzdGFuZGFsb25lIiwiYmFja2dyb3VuZF9jb2xvciI6IiNmNWY1ZjciLCJ0aGVtZV9jb2xvciI6IiMwMDcxZTMifQ==">
<title>ADB 智能电量管家</title>
<style>
  :root{--primary:#0071e3;--success:#34c759;--warning:#ff9500;--danger:#ff3b30;--bg:#f5f5f7;--card:#fff;--border:#e5e5ea}
  body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;margin:0;padding:12px;background:var(--bg);color:#1d1d1f}
  .container{max-width:760px;margin:0 auto}
  .card{background:var(--card);border-radius:12px;padding:14px 18px;margin-bottom:14px;box-shadow:0 1px 3px rgba(0,0,0,0.06);border:1px solid var(--border)}
  h3{margin:0 0 8px;font-size:16px;display:flex;align-items:center;justify-content:space-between}
  input[type=text],input[type=password],input[type=number],textarea{width:100%;padding:9px;margin:6px 0 10px;border:1px solid var(--border);border-radius:8px;box-sizing:border-box;font-size:14px}
  textarea{font-family:monospace;font-size:11px;height:90px}
  button{background:var(--primary);color:#fff;border:none;padding:9px 15px;border-radius:8px;cursor:pointer;font-size:14px;font-weight:bold;transition:.15s}
  button:active{transform:scale(0.98)}
  button.del{background:var(--danger);padding:5px 8px;font-size:12px}
  button.test{background:var(--success);padding:5px 10px;font-size:12px;margin-right:4px}
  table{width:100%;border-collapse:collapse;margin-top:8px}
  th,td{text-align:left;padding:10px 4px;border-bottom:1px solid var(--border);font-size:13px}
  .badge{display:inline-block;padding:3px 8px;border-radius:12px;font-size:12px;font-weight:bold;color:#fff}
  .badge-gray{background:#8e8e93}
  .badge-green{background:var(--success)}
  .badge-orange{background:var(--warning)}
  .badge-red{background:var(--danger)}
  .status-box{display:flex;gap:8px;margin-bottom:14px;flex-wrap:wrap}
  .status-item{flex:1;min-width:160px;background:var(--card);padding:10px 14px;border-radius:8px;border:1px solid var(--border);font-size:13px}
  .tip-box{background:#f2f4f8;border-left:3px solid var(--primary);padding:8px 12px;margin:6px 0 10px;border-radius:0 6px 6px 0;font-size:12px;color:#555;line-height:1.5}
  .tip-box code{background:#e2e5ea;padding:1px 5px;border-radius:4px;color:#c7254e;font-family:monospace}
  /* Toast 轻量提示 */
  #toast{visibility:hidden;min-width:200px;background-color:#333;color:#fff;text-align:center;border-radius:20px;padding:10px 16px;position:fixed;z-index:99;left:50%;bottom:30px;transform:translateX(-50%);font-size:14px}
  #toast.show{visibility:visible;animation:fadein 0.3s,fadeout 0.3s 2s}
  @keyframes fadein{from{bottom:10px;opacity:0}to{bottom:30px;opacity:1}}
  @keyframes fadeout{from{bottom:30px;opacity:1}to{bottom:10px;opacity:0}}
</style>
</head><body><div class="container">
<h2>⚡ ESP32-C3 ADB 智能电量管家</h2>
<div id="toast"></div>
)rawhtml";

        // 状态栏
        html += "<div class=\"status-box\">";
        html += "<div class=\"status-item\">🌐 <b>WiFi 状态</b>: " + statusWifi + "</div>";
        html += "<div class=\"status-item\">📡 <b>当前 IP</b>: " + ipAddr + "</div>";
        html += "<div class=\"status-item\">🔑 <b>私钥状态</b>: " + statusKey + "</div>";
        html += "</div>";

        // 卡片 1：WiFi 网络配置
        html += "<div class=\"card\" style=\"border:2px solid var(--primary)\">";
        html += "<h3>📶 1. WiFi 网络配置 (请填写 2.4GHz WiFi)</h3>";
        html += "<form id=\"form_wifi\" onsubmit=\"saveWifi(event)\">";
        html += "<input type=\"text\" name=\"ssid\" placeholder=\"WiFi 名称 (SSID)\" value=\"" + currentSsid + "\" required>";
        html += "<input type=\"password\" name=\"pass\" placeholder=\"WiFi 密码 (无密码留空)\">";
        html += "<button type=\"submit\" style=\"width:100%;background:#0071e3;font-size:15px\">保存 WiFi 并重启连接</button>";
        html += "</form></div>";

        // 卡片 2：ADB 私钥配置（AJAX 无刷新保存）
        html += "<div class=\"card\">";
        html += "<h3>🔐 2. 电脑 ADB 私钥 (adbkey)</h3>";
        html += "<div class=\"tip-box\">";
        html += "• <b>Windows</b>：按 <code>Win + R</code> 输入 <code>%USERPROFILE%\\.android</code> 回车直达；<br>";
        html += "• <b>Linux/macOS</b>：运行 <code>cat ~/.android/adbkey</code> 复制；<br>";
        html += "⚠️ 复制名为 <code>adbkey</code> 的文件（<b>严禁选 .pub</b>），全选粘贴到下方：";
        html += "</div>";
        html += "<form id=\"form_key\" onsubmit=\"saveKey(event)\">";
        html += "<textarea id=\"txt_key\" name=\"key\" placeholder=\"-----BEGIN PRIVATE KEY-----\n...\n-----END PRIVATE KEY-----\">" + currentKey + "</textarea>";
        html += "<button type=\"submit\" style=\"width:100%;background:#34c759;font-size:15px\">保存私钥至固件 Flash</button>";
        html += "</form></div>";

        // 卡片 3：设备监控
        html += R"rawhtml(
<div class="card">
  <h3>📱 3. 设备电量监控</h3>
  <div style="display:flex;gap:8px;margin-bottom:10px">
    <input type="text" id="new_ip" placeholder="输入手机 IP (如 192.168.31.182)" style="margin:0">
    <button type="button" onclick="addIp()" style="white-space:nowrap">添加设备</button>
  </div>
  <table>
    <thead><tr><th>设备 IP</th><th>连接结果</th><th>当前电量</th><th>电量获取时间</th><th>操作</th></tr></thead>
    <tbody id="device_list"></tbody>
  </table>
</div>
)rawhtml";

        // 卡片 4：自动化配置
        html += "<div class=\"card\">";
        html += "<h3>⚙️ 4. 轮询、告警与重试配置</h3>";
        html += "<form id=\"form_cfg\" onsubmit=\"saveSettings(event)\">";
        html += "<label>Notify API 推送地址:</label>";
        html += "<input type=\"text\" name=\"url\" value=\"" + currentUrl + "\" placeholder=\"https://notify.yysresume.work/api/send\">";
        html += "<div style=\"display:flex;gap:12px\">";
        html += "<div style=\"flex:1\"><label>低电量阈值 (%):</label><input type=\"number\" name=\"threshold\" value=\"" + String(currentTh) + "\" min=\"5\" max=\"95\"></div>";
        html += "<div style=\"flex:1\"><label>轮询间隔 (分钟):</label><input type=\"number\" name=\"interval\" value=\"" + String(currentInterval) + "\" min=\"1\" max=\"1440\"></div>";
        html += "<div style=\"flex:1\"><label>失败重试次数 (次):</label><input type=\"number\" name=\"retries\" value=\"" + String(currentRetries) + "\" min=\"1\" max=\"10\"></div>";
        html += "</div><button type=\"submit\" style=\"width:100%\">保存自动化配置</button></form></div>";

        // 底部脚本：LocalStorage 极速渲染 + AJAX 无刷新提示
        html += R"rawhtml(
<script>
let lowThreshold = 30;

function toast(msg) {
  let t = document.getElementById("toast");
  t.innerText = msg;
  t.className = "show";
  setTimeout(() => { t.className = t.className.replace("show", ""); }, 2200);
}

// 核心优化：利用本地缓存 0 毫秒渲染表格，彻底消灭“加载中...”
function renderDeviceRows(data) {
  let tbody = document.getElementById('device_list');
  if(!data || data.length === 0) {
    tbody.innerHTML = '<tr><td colspan="5" style="color:#86868b">暂未添加任何设备 IP</td></tr>';
    return;
  }
  tbody.innerHTML = '';
  data.forEach(item => {
    let tr = document.createElement('tr');
    let id = item.ip.replace(/\./g,'_');

    let statusBadge = '<span class="badge badge-gray" id="conn_' + id + '">未检测</span>';
    if(item.tested) {
      statusBadge = item.connected ? 
        '<span class="badge badge-green" id="conn_' + id + '">连接成功</span>' : 
        '<span class="badge badge-red" id="conn_' + id + '">' + item.msg + '</span>';
    }

    let batteryBadge = '<span style="color:#8e8e93;font-weight:bold;padding-left:12px" id="bat_' + id + '">--</span>';
    let timeText = '--';

    if(item.battery >= 0) {
      timeText = item.lastTime;
      if(item.connected) {
        let bClass = 'badge badge-green';
        if(item.battery <= lowThreshold) bClass = 'badge badge-red';
        else if(item.battery < 50) bClass = 'badge badge-orange';
        batteryBadge = '<span class="' + bClass + '" id="bat_' + id + '">' + item.battery + '% 🔋</span>';
      } else {
        batteryBadge = '<span class="badge badge-gray" id="bat_' + id + '" title="此为离线前最后获取的电量">' + item.battery + '% (历史)</span>';
      }
    }

    tr.innerHTML = `
      <td><b>${item.ip}</b></td>
      <td>${statusBadge}</td>
      <td>${batteryBadge}</td>
      <td style="color:#6e6e73;font-size:12px" id="time_${id}">${timeText}</td>
      <td>
        <button class="test" onclick="testAdb('${item.ip}')">检测</button>
        <button class="del" onclick="delIp('${item.ip}')">删除</button>
      </td>`;
    tbody.appendChild(tr);
  });
}

function loadDevices() {
  // 1. 先读手机本地缓存秒开
  let localCache = localStorage.getItem('adb_cached_devs');
  if(localCache) {
    try { renderDeviceRows(JSON.parse(localCache)); } catch(e){}
  }

  // 2. 静默拉取最新数据
  fetch('/api/devices').then(r=>r.json()).then(data=>{
    localStorage.setItem('adb_cached_devs', JSON.stringify(data));
    renderDeviceRows(data);
  }).catch(()=>{});
}

// AJAX 无刷新保存私钥
function saveKey(e) {
  e.preventDefault();
  let keyVal = document.getElementById('txt_key').value.trim();
  if(!keyVal) return alert('私钥不能为空！');
  let fd = new FormData(document.getElementById('form_key'));
  fetch('/api/key', {method:'POST', body:fd}).then(r=>r.text()).then(res=>{
    toast("✅ ADB 私钥已保存并立即生效！");
    document.getElementById('lbl_key').innerHTML = "<span style='color:#34c759'>已保存</span>";
  });
}

// AJAX 保存自动化配置
function saveSettings(e) {
  e.preventDefault();
  let fd = new FormData(document.getElementById('form_cfg'));
  fetch('/api/settings', {method:'POST', body:fd}).then(r=>r.text()).then(res=>{
    toast("✅ 自动化参数已保存！");
  });
}

// WiFi 重启
function saveWifi(e) {
  e.preventDefault();
  if(!confirm("确定保存 WiFi 并重启 ESP32 连网吗？")) return;
  let fd = new FormData(document.getElementById('form_wifi'));
  fetch('/api/wifi', {method:'POST', body:fd}).then(r=>r.text()).then(res=>{
    alert("WiFi 已保存，ESP32 正在重启连网，请稍后在局域网内访问！");
  });
}

function addIp() {
  let ip = document.getElementById('new_ip').value.trim();
  if(!ip) return alert('请输入合法 IP');
  let fd = new FormData();
  fd.append('ip', ip);
  fetch('/api/device/add', {method:'POST', body:fd}).then(()=>{
    document.getElementById('new_ip').value = '';
    loadDevices();
  });
}

function delIp(ip) {
  if(!confirm('确定删除 '+ip+' 吗?')) return;
  let fd = new FormData();
  fd.append('ip', ip);
  fetch('/api/device/del', {method:'POST', body:fd}).then(()=>loadDevices());
}

function testAdb(ip) {
  let id = ip.replace(/\./g,'_');
  let connEl = document.getElementById('conn_' + id);
  let batEl = document.getElementById('bat_' + id);
  let timeEl = document.getElementById('time_' + id);

  connEl.className = 'badge badge-gray';
  connEl.innerText = '握手中...';

  fetch('/api/device/test?ip=' + ip).then(r=>r.json()).then(data=>{
    if(data.connected) {
      connEl.className = 'badge badge-green';
      connEl.innerText = '连接成功';

      let bClass = 'badge badge-green';
      if(data.battery <= lowThreshold) bClass = 'badge badge-red';
      else if(data.battery < 50) bClass = 'badge badge-orange';
      batEl.outerHTML = '<span class="' + bClass + '" id="bat_' + id + '">' + data.battery + '% 🔋</span>';
      timeEl.innerText = data.time;
    } else {
      connEl.className = 'badge badge-red';
      connEl.innerText = data.msg;
      if(data.battery >= 0) {
        batEl.outerHTML = '<span class="badge badge-gray" id="bat_' + id + '" title="此为离线前最后获取的电量">' + data.battery + '% (历史)</span>';
        timeEl.innerText = data.time;
      } else {
        batEl.outerHTML = '<span style="color:#8e8e93;font-weight:bold;padding-left:12px" id="bat_' + id + '">--</span>';
        timeEl.innerText = '--';
      }
    }
  }).catch(()=>{
    connEl.className = 'badge badge-red';
    connEl.innerText = '请求超时';
  });
}

window.onload = function() {
  loadDevices();
};
</script>
</div></body></html>
)rawhtml";

        server.send(200, "text/html; charset=utf-8", html);
    }

    void handleSaveWiFi() {
        storage.saveWiFi(server.arg("ssid"), server.arg("pass"));
        server.send(200, "text/plain", "OK");
        delay(1000);
        ESP.restart();
    }

    void handleSaveKey() {
        String key = server.arg("key");
        key.trim();
        storage.saveAdbKey(key);
        AdbClient::reloadPrivateKey(key);
        server.send(200, "text/plain", "OK");
    }

    void handleSaveSettings() {
        storage.saveSettings(
            server.arg("url"), 
            server.arg("threshold").toInt(), 
            server.arg("interval").toInt(),
            server.arg("retries").toInt()
        );
        server.send(200, "text/plain", "OK");
    }

    void handleGetDevices() {
        auto ips = storage.getIPList();
        String json = "[";
        for (size_t i = 0; i < ips.size(); i++) {
            String ip = ips[i];
            DeviceStatus st = deviceCache[ip];
            json += "{\"ip\":\"" + ip + "\",\"battery\":" + String(st.battery) + 
                    ",\"tested\":" + (st.tested ? "true" : "false") +
                    ",\"connected\":" + (st.connected ? "true" : "false") +
                    ",\"lastTime\":\"" + escapeJson(st.lastTime) + "\",\"msg\":\"" + escapeJson(st.errorMsg) + "\"}";
            if (i < ips.size() - 1) json += ",";
        }
        json += "]";
        server.send(200, "application/json", json);
    }

    void handleAddDevice() {
        String ip = server.arg("ip");
        ip.trim();
        if (ip.length() > 0) storage.addIP(ip);
        server.send(200, "text/plain", "OK");
    }

    void handleDelDevice() {
        String ip = server.arg("ip");
        ip.trim();
        storage.removeIP(ip);
        deviceCache.erase(ip);
        storage.saveStatusCache(deviceCache);
        server.send(200, "text/plain", "OK");
    }

    void handleTestDevice() {
        String ip = server.arg("ip");
        ip.trim();
        extern void checkDevice(const String& ip);
        checkDevice(ip);

        DeviceStatus st = deviceCache[ip];
        String json = "{";
        json += "\"connected\":" + String(st.connected ? "true" : "false") + ",";
        json += "\"battery\":" + String(st.battery) + ",";
        json += "\"msg\":\"" + escapeJson(st.errorMsg) + "\",";
        json += "\"time\":\"" + escapeJson(st.lastTime) + "\"";
        json += "}";
        server.send(200, "application/json", json);
    }
};

extern WebServerManager webManager;