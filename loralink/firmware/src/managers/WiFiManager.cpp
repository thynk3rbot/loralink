#include "WiFiManager.h"
#include "BLEManager.h"
#include "CommandManager.h"
#include "DataManager.h"
#include "ESPNowManager.h"
#include "LoRaManager.h"
#include "PerformanceManager.h"
#include "PowerManager.h"
#include "ProductManager.h"
#include "ScheduleManager.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

WebServer server(80);

void (*_webCmdCallback)(const String &, CommInterface) = NULL;
void setWebCallback(void (*cb)(const String &, CommInterface)) {
  _webCmdCallback = cb;
}

WiFiManager::WiFiManager() {
  isConnected = false;
  serverStarted = false;
  lastWifiTry = 0;
  lastApiHit = 0;
  modemSleepEnabled = false;
  _wifiLostAt = 0;
}

void WiFiManager::init() {
  DataManager &data = DataManager::getInstance();
  if (data.wifiSsid.length() > 0) {
    Serial.println("WiFi: Configured (Staggered Start)...");
    WiFi.mode(WIFI_STA);
  } else {
    Serial.println("WiFi: No SSID saved.");
  }
}

void WiFiManager::handle() {
  DataManager &data = DataManager::getInstance();

  if (millis() < 5000)
    return;

  // Power-Miser Enforcement: Immediately kill radios if disallowed by policy
  if (!PowerManager::getInstance().isWifiAllowed()) {
    if (isConnected || serverStarted) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      isConnected = false;
      serverStarted = false;
      Serial.println(
          "POWER: Miser enforcing WiFi OFF (Critical Safe-Shutdown)");
    }
    return;
  }

  if (data.wifiSsid.length() > 0) {
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 10000) {
      static bool configured = false;
      if (!configured) {
        if (data.staticIp.length() > 0) {
          IPAddress ip, gw, sn;
          if (ip.fromString(data.staticIp)) {
            if (!gw.fromString(data.gateway)) {
              gw = ip; // Fallback to X.X.X.1
              gw[3] = 1;
            }
            if (!sn.fromString(data.subnet)) {
              sn = IPAddress(255, 255, 255, 0); // Fallback to /24
            }
            WiFi.config(ip, gw, sn);
            Serial.println("WiFi: Using Static IP " + ip.toString());
          }
        }
        WiFi.begin(data.wifiSsid.c_str(), data.wifiPass.c_str());
        configured = true;
        lastWifiTry = millis();
        Serial.println("WiFi: Attempting connection...");
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      if (!isConnected) {
        isConnected = true;
        Serial.println("WiFi: Connected! IP: " + WiFi.localIP().toString());
      }
      if (!serverStarted) {
        startServer();
        ArduinoOTA.begin();
        serverStarted = true;
        lastApiHit = millis();
        Serial.println("Web server & OTA started");
      }
      ArduinoOTA.handle();
      server.handleClient();
      data.traceLogging = (millis() - lastApiHit < 10000);

    } else {
      // Track when WiFi first dropped for downgrade timer
      if (isConnected) {
        _wifiLostAt = millis();
        isConnected = false;
      }
      // Sustained loss >30s while not already on LORA → downgrade
      if (_wifiLostAt > 0 && (millis() - _wifiLostAt > 30000UL) &&
          data.currentLink != LinkPreference::LINK_LORA) {
        onLinkDowngrade(data.currentLink, LinkPreference::LINK_LORA);
        _wifiLostAt = 0;
      }
      if (millis() - lastWifiTry > 10000) {
        lastWifiTry = millis();
        WiFi.disconnect();
        WiFi.begin(data.wifiSsid.c_str(), data.wifiPass.c_str());
      }
    }
  }
  // Probe backoff tick — fires when in LORA hold and backoff interval elapsed
  checkProbeBackoff();
}

void WiFiManager::startServer() {
  DataManager &data = DataManager::getInstance();
  String otaName = "LoRaLink-" + data.myId;
  ArduinoOTA.setHostname(otaName.c_str());
  ArduinoOTA.onStart([]() {
    DisplayManager::getInstance().SetDisplayActive(true);
    Heltec.display->clear();
    Heltec.display->drawString(0, 20, "OTA Updating...");
    Heltec.display->display();
  });

  // Dashboard
  server.on("/", HTTP_GET, [this]() {
    lastApiHit = millis();
    serveHome();
  });

  // Configuration page
  server.on("/config", HTTP_GET, [this]() {
    lastApiHit = millis();
    serveConfig();
  });
  server.on("/config", HTTP_POST, [this]() {
    lastApiHit = millis();
    serveConfigSave();
  });

  // Integration page
  server.on("/integration", HTTP_GET, [this]() {
    lastApiHit = millis();
    serveIntegration();
  });
  server.on("/integration", HTTP_POST, [this]() {
    lastApiHit = millis();
    serveIntegrationSave();
  });

  // Help page
  server.on("/help", HTTP_GET, [this]() {
    lastApiHit = millis();
    serveHelp();
  });

  // Scheduling page
  server.on("/scheduling", HTTP_GET, [this]() {
    lastApiHit = millis();
    serveScheduling();
  });

  // API
  server.on("/api/status", HTTP_GET, [this]() {
    lastApiHit = millis();
    serveApiStatus();
  });
  server.on("/api/config", HTTP_GET, [this]() {
    lastApiHit = millis();
    serveApiConfig();
  });
  server.on("/api/config/apply", HTTP_POST, [this]() {
    lastApiHit = millis();
    serveApiConfigApply();
  });
  server.on("/api/product/save", HTTP_POST, [this]() {
    lastApiHit = millis();
    serveApiProductSave();
  });
  server.on("/api/files/list", HTTP_GET, [this]() {
    lastApiHit = millis();
    serveApiFileList();
  });
  server.on("/api/files/read", HTTP_GET, [this]() {
    lastApiHit = millis();
    serveApiFileRead();
  });
  server.on("/api/cmd", HTTP_POST, [this]() {
    lastApiHit = millis();
    serveApiCmd();
  });
  server.on("/api/peers", HTTP_GET, [this]() {
    lastApiHit = millis();
    serveApiPeers();
  });
  server.on("/api/peers/add", HTTP_POST, [this]() {
    lastApiHit = millis();
    serveApiAddPeer();
  });
  server.on("/api/peers/remove", HTTP_POST, [this]() {
    lastApiHit = millis();
    serveApiRemovePeer();
  });

  // Schedule API
  server.on("/api/schedule", HTTP_GET, [this]() {
    lastApiHit = millis();
    serveApiSchedule();
  });
  server.on("/api/schedule/add", HTTP_POST, [this]() {
    lastApiHit = millis();
    serveApiScheduleAdd();
  });
  server.on("/api/schedule/remove", HTTP_POST, [this]() {
    lastApiHit = millis();
    serveApiScheduleRemove();
  });
  server.on("/api/schedule/clear", HTTP_POST, [this]() {
    lastApiHit = millis();
    serveApiScheduleClear();
  });
  server.on("/api/schedule/save", HTTP_POST, [this]() {
    lastApiHit = millis();
    serveApiScheduleSave();
  });
  server.on("/api/pins/name", HTTP_POST, [this]() {
    lastApiHit = millis();
    serveApiPinName();
  });
  server.on("/api/pins/enable", HTTP_POST, [this]() {
    lastApiHit = millis();
    serveApiPinEnable();
  });
  server.on("/api/transport/mode", HTTP_POST, [this]() {
    lastApiHit = millis();
    serveApiTransportMode();
  });
  server.on("/api/registry", HTTP_GET, [this]() {
    lastApiHit = millis();
    serveApiRegistry();
  });
  server.on("/api/trace/clear", HTTP_POST, [this]() {
    lastApiHit = millis();
    DataManager::getInstance().ClearTrace();
    server.send(200, "text/plain", "OK");
  });

  server.begin();

  // mDNS: advertise HTTP on <hostname>.local (same name as OTA)
  String mdnsName = "LoRaLink-" + DataManager::getInstance().myId;
  mdnsName.toLowerCase();
  if (MDNS.begin(mdnsName.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: http://" + mdnsName + ".local");
  }
}

// ============================================================================
//   DASHBOARD PAGE
// ============================================================================
void WiFiManager::serveHome() {
  String html = R"rawhtml(<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>LoRaLink )rawhtml" FIRMWARE_VERSION R"rawhtml(</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',sans-serif;background:#0f0f1a;color:#e0e0e0;height:100vh;display:flex;flex-direction:column;overflow:hidden}
.hdr{background:linear-gradient(135deg,#1a1a2e,#16213e);padding:8px 12px;border-bottom:1px solid #2a2a4a;display:flex;align-items:center;gap:8px;flex-shrink:0}
.hdr h1{font-size:0.95em;color:#00d4ff;font-weight:700;white-space:nowrap}
.hdr h1 span{font-size:0.72em;color:#555;font-weight:400;margin-left:3px}
.nav{margin-left:auto;display:flex;gap:4px}
.nav a{color:#555;text-decoration:none;font-size:0.78em;padding:3px 7px;border:1px solid #2a2a4a;border-radius:4px}
.nav a:hover{color:#00d4ff;border-color:#00d4ff}
.ifc{display:flex;gap:4px;padding:4px 10px;background:#16213e;border-bottom:1px solid #2a2a4a;flex-shrink:0}
.badge{padding:2px 7px;border-radius:8px;font-size:0.63em;font-weight:700;letter-spacing:0.4px;text-transform:uppercase}
.badge.on{background:#00ff8814;color:#00ff88;border:1px solid #00ff8844}
.badge.off{background:#ff444414;color:#ff4444;border:1px solid #ff444444}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:5px;padding:7px;flex-shrink:0}
.card{background:#1a1a2e;border-radius:6px;padding:6px 8px;border:1px solid #2a2a4a;position:relative}
.dbg-id{display:none;font-size:10px;font-weight:700;color:#00f2ff;background:rgba(0,242,255,0.2);border:1px solid rgba(0,242,255,0.5);padding:1px 4px;border-radius:4px;font-family:monospace;line-height:1;vertical-align:middle;margin:0 4px;pointer-events:none;opacity:1.0;text-shadow:0 0-4px rgba(0,242,255,0.8);z-index:100}
body.debug-on .dbg-id{display:inline-block!important}
.dbg-tgl{display:flex;align-items:center;gap:6px;font-size:0.65em;color:#555;cursor:pointer;padding:2px 8px;border-radius:12px;background:#16213e;border:1px solid #2a2a4a;transition:0.2s}
.dbg-tgl:hover{border-color:#00d4ff22}
.dbg-tgl.active{color:#00d4ff;background:#00d4ff14;border-color:#00d4ff44}
.dbg-tgl .dot{width:5px;height:5px;border-radius:50%;background:#444}
.dbg-tgl.active .dot{background:#00d4ff;box-shadow:0 0 5px #00d4ff}

.card .lbl{font-size:0.58em;color:#666;text-transform:uppercase;letter-spacing:0.8px}
.card .val{font-size:1.0em;font-weight:700;margin-top:1px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.ok{color:#00ff88}
.warn{color:#ffaa00}
.log{flex:1;overflow-y:auto;min-height:0;border-top:1px solid #1a1a2e}
.m{padding:3px 10px;font-size:0.73em;border-bottom:1px solid #111118;font-family:monospace;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.ts{color:#3a3a5a}
.src{color:#00d4ff}
.cmd{display:flex;gap:5px;padding:6px 8px;border-top:1px solid #2a2a4a;flex-shrink:0;background:#16213e}
.cmd input{flex:1;background:#0f0f1a;border:1px solid #2a2a4a;border-radius:5px;padding:5px 9px;color:#e0e0e0;font-size:0.8em;outline:none}
.cmd input:focus{border-color:#00d4ff}
.cmd button{background:#00d4ff;border:none;border-radius:5px;padding:5px 14px;color:#0f0f1a;font-weight:700;font-size:0.8em;cursor:pointer}
.cmd button:hover{background:#00b8d4}
</style></head><body>
<div class='hdr'>
  <h1>&#x1F4E1; <span id='did'>—</span><span id='fwv'></span></h1><span class='dbg-id'>DASH-HDR</span>
  <div id='dbg-btn' class='dbg-tgl' onclick='tD()'><div class='dot'></div>DEBUG</div>

  <div class='nav'>
    <a href='/'>&#x1F4E1; Dash</a>
    <a href='/config'>&#x2699; Config</a>
    <a href='/scheduling'>&#x23F1; Sched</a>
    <a href='/hardware'>&#x1F527; HW</a>
    <a href='/integration'>&#x1F50C;</a>
  </div>
</div>
<div class='ifc' id='ifc'></div>
<div class='grid' id='cards'></div>
<div id='perf-sect'></div><div id='periph-sect'></div><div id='pins-sect'></div>
<div class='log' id='log'></div>
<div class='cmd' style='position:relative'>
  <span class='dbg-id'>DASH-CMD</span>
  <input id='ci' placeholder='Command...' onkeydown="if(event.key==='Enter')send()">
  <button onclick='send()'>Send</button>
</div>
<script>
function up(){fetch('/api/status').then(function(r){return r.json();}).then(function(d){
document.getElementById('did').textContent=d.id||'—';
document.getElementById('fwv').textContent=d.version?' v'+d.version:'';
var ifcs=[['LoRa',d.lora],['BLE',d.ble],['WiFi',d.wifi],['EN',d.espnow]];
document.getElementById('ifc').innerHTML=ifcs.map(function(x){
return '<span class="badge '+(x[1]?'on':'off')+'">'+x[0]+'</span>';
}).join('');
var b=parseFloat(d.bat)||0;
var batStr=b<0.1?'USB':b.toFixed(2)+'V';
var c='<div class="card"><span class="dbg-id">DASH-BATT</span><div class="lbl">Battery</div><div class="val '+(b<0.1||b>3.5?'ok':'warn')+'">'+batStr+'</div></div>';
var pmStr = d.power_mode || 'NORMAL';
var pmColor = (pmStr === 'NORMAL') ? '#00ff88' : (pmStr === 'CONSERVE' ? '#ffaa00' : '#ff4444');
if (b >= 0.1 && b <= 4.1) {
  c += '<div class="card"><span class="dbg-id">DASH-POWER</span><div class="lbl">Power-Miser</div><div class="val" style="color:'+pmColor+'">'+pmStr+'</div></div>';
}
c+='<div class="card"><span class="dbg-id">DASH-RSSI</span><div class="lbl">LoRa RSSI</div><div class="val">'+(d.rssi||'—')+'</div></div>';
c+='<div class="card"><span class="dbg-id">DASH-NODES</span><div class="lbl">Nodes</div><div class="val ok">'+(d.nodes!=null?d.nodes:'—')+'</div></div>';
document.getElementById('cards').innerHTML=c;

// Performance Section
var psect=document.getElementById('perf-sect');
psect.textContent='';
if(d.loop_avg_ms !== undefined) {
  var toaS=(d.lora_toa_ms/1000).toFixed(1);
  var perfHtml = '<div style="padding:4px 10px;background:#1a1a2e;border-top:1px solid #2a2a4a;font-size:0.65em;color:#00d4ff;text-transform:uppercase;letter-spacing:1px;font-weight:700">Performance & Diagnostics</div><span class="dbg-id">DASH-PERF-MON</span>';
  perfHtml += '<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:6px;padding:8px">';
  perfHtml += '<div class="card"><div class="lbl">Loop Avg/Max</div><div class="val" style="font-size:0.8em">' + d.loop_avg_ms + ' / ' + d.loop_max_ms + ' ms</div></div>';
  perfHtml += '<div class="card"><div class="lbl">Total ToA</div><div class="val" style="font-size:0.8em">' + toaS + 's</div></div>';
  perfHtml += '<div class="card"><div class="lbl">Range Boost</div><div class="val ok" style="font-size:0.8em">+' + (d.bytes_saved || 0) + ' Bytes</div></div>';
  perfHtml += '<div class="card" style="grid-column: 1 / -1;"><div class="lbl">Last Reset</div><div class="val" style="font-size:0.75em;color:#ffaa00;white-space:normal;overflow:visible">' + (d.sys_reset || d.reset || 'UNKNOWN') + '</div></div>';
  perfHtml += '</div>';
  psect.insertAdjacentHTML('afterbegin', perfHtml);
}

// Peripherals Section
document.getElementById('periph-sect').textContent='';
if(d.peripherals && d.peripherals.length > 0) {
  var phtml = '<div style="padding:4px 10px;background:#1a1a2e;border-top:1px solid #2a2a4a;font-size:0.65em;color:#00d4ff;text-transform:uppercase;letter-spacing:1px;font-weight:700">Peripherals</div><span class="dbg-id">DASH-PERIPH-LIST</span>';
  phtml += '<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:6px;padding:8px">';
  d.peripherals.forEach(function(p){
    var dataStr = '';
    if(p.data) {
      for(var k in p.data) {
        dataStr += '<div style="display:flex;justify-content:space-between"><span>'+k+'</span><span style="color:#00ff88">'+p.data[k]+'</span></div>';
      }
    }
    phtml += '<div class="card" style="border-left:3px solid #00d4ff">';
    phtml += '<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:4px">';
    phtml += '<div class="lbl" style="font-size:0.65em">'+p.id+' ('+p.hw+')</div>';
    phtml += '<div style="font-size:0.5em;color:#666">v'+p.fw+'</div>';
    phtml += '</div>';
    phtml += '<div style="font-size:0.8em">'+dataStr+'</div>';
    phtml += '<div style="font-size:0.5em;color:#444;margin-top:4px;text-align:right">seen '+p.lastSeen+'s ago</div>';
    phtml += '</div>';
  });
  phtml += '</div>';
  document.getElementById('periph-sect').insertAdjacentHTML('afterbegin', phtml);
} else { document.getElementById('periph-sect').textContent=''; }

var l='';d.log.slice().reverse().forEach(function(m){
if(m)l+='<div class="m"><span class="ts">['+m.ts+'s]</span> <span class="src">'+m.src+'</span>: '+m.msg+'</div>';
});
document.getElementById('log').innerHTML=l;

// Compact Pin Monitor
document.getElementById('pins-sect').textContent='';
if(d.pins && d.pins.length > 0) {
  var phtml = '<div style="padding:10px;display:grid;grid-template-columns:1fr 1fr;gap:5px;position:relative;"><span class="dbg-id">DASH-PIN-MONITOR</span>';
  d.pins.forEach(function(p){
    var isRelay = p.n.indexOf('RL') === 0 || p.n === 'LED';
    phtml += '<div class="card" style="padding:5px 8px;border-color:'+(p.v?'#00ff8844':'#2a2a4a')+'">';
    phtml += '<div class="lbl" style="font-size:0.5em">'+p.n+'</div>';
    phtml += '<div style="display:flex;justify-content:space-between;align-items:center">';
    phtml += '<span style="font-size:0.8em;color:'+(p.v?'#00ff88':'#666')+'">'+(p.v?'HIGH':'LOW')+'</span>';
    if(isRelay) phtml += '<button onclick="tgl(\''+p.n+'\','+p.v+')" style="padding:2px 8px;font-size:0.6em;background:#2a2a4a;color:#fff;border:1px solid #444;border-radius:3px">Toggle</button>';
    phtml += '</div></div>';
  });
  phtml += '</div>';
  document.getElementById('pins-sect').insertAdjacentHTML('afterbegin', phtml);
} else { document.getElementById('pins-sect').textContent=''; }
});}
setInterval(up,3000);up();
function tD(f){
  var b=document.body,btn=document.getElementById('dbg-btn');
  var on=f!==undefined?f:!b.classList.contains('debug-on');
  b.classList.toggle('debug-on',on);
  if(btn)btn.classList.toggle('active',on);
  localStorage.setItem('dMode',on?'1':'0');
}
if(localStorage.getItem('dMode')==='1')tD(true);
function send(){var v=document.getElementById('ci').value;if(!v)return;

fetch('/api/cmd',{method:'POST',body:new URLSearchParams({'cmd':v})});
document.getElementById('ci').value='';}
function tgl(n,v){
  if(confirm('Toggle '+n+'?')) {
    fetch('/api/cmd',{method:'POST',body:new URLSearchParams({'cmd':'GPIO '+n+' '+(v?0:1)})});
    setTimeout(up, 500);
  }
}
</script></body></html>)rawhtml";
  server.send(200, "text/html", html);
}

// ============================================================================
//   CONFIGURATION PAGE
// ============================================================================
void WiFiManager::serveConfig() {
  DataManager &data = DataManager::getInstance();
  ESPNowManager &espnow = ESPNowManager::getInstance();

  String html = R"rawhtml(<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>LoRaLink Config )rawhtml" FIRMWARE_VERSION R"rawhtml(</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',sans-serif;background:#0f0f1a;color:#e0e0e0;min-height:100vh}
.hdr{background:linear-gradient(135deg,#1a1a2e,#16213e);padding:16px 20px;border-bottom:1px solid #2a2a4a;display:flex;justify-content:space-between;align-items:center}
.hdr h1{font-size:1.3em;color:#00d4ff;font-weight:600}
.hdr a{color:#888;text-decoration:none;font-size:0.85em;padding:6px 14px;border:1px solid #2a2a4a;border-radius:6px}
.hdr a:hover{color:#00d4ff;border-color:#00d4ff}
.sec{margin:16px;background:#1a1a2e;border-radius:10px;border:1px solid #2a2a4a;padding:20px;position:relative}
.dbg-id{display:none;font-size:10px;font-weight:700;color:#00f2ff;background:rgba(0,242,255,0.2);border:1px solid rgba(0,242,255,0.5);padding:1px 4px;border-radius:4px;font-family:monospace;line-height:1;vertical-align:middle;margin:0 4px;pointer-events:none;opacity:1.0;text-shadow:0 0-4px rgba(0,242,255,0.8);z-index:100}
body.debug-on .dbg-id{display:inline-block!important}
.dbg-tgl{display:flex;align-items:center;gap:6px;font-size:0.65em;color:#555;cursor:pointer;padding:2px 8px;border-radius:12px;background:#16213e;border:1px solid #2a2a4a;transition:0.2s}
.dbg-tgl:hover{border-color:#00d4ff22}
.dbg-tgl.active{color:#00d4ff;background:#00d4ff14;border-color:#00d4ff44}
.dbg-tgl .dot{width:5px;height:5px;border-radius:50%;background:#444}
.dbg-tgl.active .dot{background:#00d4ff;box-shadow:0 0 5px #00d4ff}

.sec h2{color:#00d4ff;font-size:1em;margin-bottom:12px;padding-bottom:8px;border-bottom:1px solid #2a2a4a}
.row{display:flex;gap:12px;margin-bottom:10px;align-items:center;flex-wrap:wrap}
.row label{width:120px;font-size:0.85em;color:#aaa;flex-shrink:0}
.row input,.row select{flex:1;background:#0f0f1a;border:1px solid #2a2a4a;border-radius:6px;padding:8px 12px;color:#fff;outline:none;min-width:140px}
.row input:focus,.row select:focus{border-color:#00d4ff}
.row .hint{font-size:0.7em;color:#666;width:100%;margin-left:132px}
.btn{background:#00d4ff;border:none;border-radius:8px;padding:10px 24px;color:#0f0f1a;cursor:pointer;font-weight:600;margin-top:12px}
.btn:hover{background:#00b8d4}
.btn.danger{background:#ff4444;color:#fff}
.btn.danger:hover{background:#cc3333}
.peer-row{display:flex;gap:8px;align-items:center;padding:6px 0;border-bottom:1px solid #1f1f3a}
.peer-row .mac{font-family:monospace;font-size:0.85em;color:#aaa}
.peer-row .name{font-weight:600;color:#e0e0e0}
.peer-row button{background:#ff444444;border:1px solid #ff4444;color:#ff4444;border-radius:4px;padding:2px 8px;cursor:pointer;font-size:0.75em}
.peer-row button:hover{background:#ff4444;color:#fff}
.add-peer{display:flex;gap:8px;margin-top:8px;flex-wrap:wrap}
.add-peer input{background:#0f0f1a;border:1px solid #2a2a4a;border-radius:6px;padding:8px 12px;color:#fff;outline:none}
.tag{display:inline-block;padding:2px 8px;border-radius:10px;font-size:0.7em;font-weight:600;margin-left:6px}
.tag.on{background:#00ff8822;color:#00ff88}
.tag.off{background:#ff444422;color:#ff4444}
.msg{background:#00ff8822;color:#00ff88;border:1px solid #00ff8844;border-radius:8px;padding:10px 16px;margin:16px;text-align:center;display:none}
</style></head><body>
<div class='hdr' style='position:relative'><span class='dbg-id' style='top:4px;right:20px'>CFG-HDR</span><h1>&#x2699; Configuration</h1><div id='dbg-btn' class='dbg-tgl' onclick='tD()'><div class='dot'></div>DEBUG</div><div><a href='/'>&#x1F4E1; Dash</a> <a href='/scheduling'>&#x1F4C5; Sched</a> <a href='/hardware'>&#x1F527; HW</a> <a href='/integration'>&#x1F50C; Plugins</a></div></div>
<div class='msg' id='msg'></div>
<form method='POST' action='/config'>

<div class='sec'>
<span class='dbg-id'>CFG-DEV</span>
<h2>&#x1F4BB; Device</h2>
<div class='row'><label>Device Name</label><input name='dev_name' value=')rawhtml";
  html += data.myId;
  html += R"rawhtml(' maxlength='14'></div>
<div class='row'><label>Repeater</label><select name='repeater'><option value='0')rawhtml";
  if (!data.repeaterEnabled)
    html += " selected";
  html += R"rawhtml(>OFF</option><option value='1')rawhtml";
  if (data.repeaterEnabled)
    html += " selected";
  html += R"rawhtml(>ON</option></select></div>
</div>

<div class='sec'>
<span class='dbg-id'>CFG-WIFI</span>
<h2>&#x1F4F6; WiFi</h2>
<div class='row'><label>SSID</label><input name='wifi_ssid' value=')rawhtml";
  html += data.wifiSsid;
  html += R"rawhtml('></div>
<div class='row'><label>Password</label><input name='wifi_pass' type='password' value=')rawhtml";
  html += data.wifiPass;
  html += R"rawhtml('></div>
<div class='row'><label>Static IP</label><input name='static_ip' value=')rawhtml";
  html += data.staticIp;
  html += R"rawhtml(' placeholder='Leave blank for DHCP'></div>
<div class='row'><label>Gateway</label><input name='gateway' value=')rawhtml";
  html += data.gateway;
  html += R"rawhtml('></div>
<div class='row'><label>Subnet</label><input name='subnet' value=')rawhtml";
  html += data.subnet;
  html += R"rawhtml('></div>
</div>

<div class='sec'>
<span class='dbg-id'>CFG-LORA</span>
<h2>&#x1F4E1; LoRa Radio</h2>
<div class='row'><label>Frequency</label><input value=')rawhtml";
  html += String(LORA_FREQ, 1);
  html +=
      R"rawhtml(' disabled><div class='hint'>Compile-time: change in config.h</div></div>
<div class='row'><label>Bandwidth</label><input value=')rawhtml";
  html += String(LORA_BW, 1);
  html += R"rawhtml(' disabled></div>
<div class='row'><label>Spread Factor</label><input value=')rawhtml";
  html += String(LORA_SF);
  html += R"rawhtml(' disabled></div>
<div class='row'><label>TX Power</label><input value=')rawhtml";
  html += String(LORA_PWR);
  html += R"rawhtml( dBm' disabled></div>
</div>

<div class='sec'>
<span class='dbg-id'>CFG-ESPNOW</span>
<h2>&#x26A1; ESP-NOW Peers</h2>
<div class='row'><label>Enabled</label><select name='espnow_en'><option value='0')rawhtml";
  if (!data.espNowEnabled)
    html += " selected";
  html += R"rawhtml(>OFF</option><option value='1')rawhtml";
  if (data.espNowEnabled)
    html += " selected";
  html += R"rawhtml(>ON</option></select></div>
<div class='row'><label>Channel</label><input name='espnow_ch' type='number' min='1' max='14' value=')rawhtml";
  html += String(data.espNowChannel);
  html += R"rawhtml('></div>
<h2 style='margin-top:16px'>Peers</h2>
<div id='peers'>Loading...</div>
<div class='add-peer'>
<input id='pmac' placeholder='AA:BB:CC:DD:EE:FF' style='width:180px'>
<input id='pname' placeholder='Name' style='width:120px'>
<button type='button' class='btn' onclick='addPeer()' style='margin-top:0'>Add Peer</button>
</div>
</div>

<div class='sec'>
<span class='dbg-id'>CFG-RST</span>
<h2>&#x1F527; Actions</h2>
<button type='submit' class='btn'>&#x1F4BE; Save & Reboot</button>
<button type='button' class='btn danger' onclick="if(confirm('Factory Reset?'))fetch('/api/cmd',{method:'POST',body:new URLSearchParams({cmd:'WIPECONFIG'})})">&#x26A0; Factory Reset</button>
</div>

</form>
<script>
function loadPeers(){fetch('/api/peers').then(r=>r.json()).then(d=>{
let h='';d.forEach((p,i)=>{
h+=`<div class='peer-row'><span class='name'>${p.name}</span><span class='mac'>${p.mac}</span><button onclick="removePeer('${p.mac}')">Remove</button></div>`;
});
if(!h)h='<div style="color:#666;font-size:0.85em">No peers configured</div>';
document.getElementById('peers').innerHTML=h;
})}
loadPeers();
function addPeer(){
let mac=document.getElementById('pmac').value;
let name=document.getElementById('pname').value;
if(!mac||!name)return;
fetch('/api/peers/add',{method:'POST',body:new URLSearchParams({mac:mac,name:name})}).then(()=>{
document.getElementById('pmac').value='';document.getElementById('pname').value='';loadPeers();});
}
function removePeer(mac){
fetch('/api/peers/remove',{method:'POST',body:new URLSearchParams({mac:mac})}).then(()=>loadPeers());
}
</script></body></html>)rawhtml";

  server.send(200, "text/html", html);
}

// ============================================================================
//   HELP PAGE
// ============================================================================
void WiFiManager::serveHelp() {
  String html = R"rawhtml(<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>LoRaLink Help </title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',sans-serif;background:#0f0f1a;color:#e0e0e0;min-height:100vh}
.hdr{background:linear-gradient(135deg,#1a1a2e,#16213e);padding:16px 20px;border-bottom:1px solid #2a2a4a;display:flex;justify-content:space-between;align-items:center}
.hdr h1{font-size:1.3em;color:#00d4ff;font-weight:600}
.hdr a{color:#888;text-decoration:none;font-size:0.85em;padding:6px 14px;border:1px solid #2a2a4a;border-radius:6px}
.hdr a:hover{color:#00d4ff;border-color:#00d4ff}
.content{padding:20px;max-width:800px;margin:0 auto}
h2{color:#00d4ff;margin-top:20px;margin-bottom:10px;font-size:1.2em;border-bottom:1px solid #2a2a4a;padding-bottom:5px}
p{margin-bottom:10px;line-height:1.5}
table{width:100%;border-collapse:collapse;margin-bottom:20px;background:#1a1a2e;border-radius:8px;overflow:hidden;box-shadow:0 4px 6px rgba(0,0,0,0.1)}
th,td{padding:12px 15px;text-align:left;border-bottom:1px solid #2a2a4a}
th{background:#16213e;color:#00d4ff;font-weight:600}
tr:last-child td{border-bottom:none}
pre{background:#000;padding:10px;border-radius:6px;overflow-x:auto;margin-bottom:15px;border:1px solid #333}
code{font-family:monospace;color:#00ff88;background:#00ff8811;padding:2px 6px;border-radius:4px}
ul{margin-left:20px;margin-bottom:15px}
li{margin-bottom:5px;line-height:1.4}
.dbg-id{display:none;position:absolute;right:8px;top:8px;font-size:10px;font-weight:700;color:#00f2ff;background:rgba(0,242,255,0.2);border:1px solid rgba(0,242,255,0.5);padding:1px 4px;border-radius:4px;font-family:monospace;line-height:1;text-shadow:0 0 4px rgba(0,242,255,0.8);z-index:100}
body.debug-on .dbg-id{display:inline-block!important}
.dbg-tgl{display:flex;align-items:center;gap:6px;font-size:0.65em;color:#555;cursor:pointer;padding:2px 8px;border-radius:12px;background:#16213e;border:1px solid #2a2a4a;transition:0.2s}
.dbg-tgl.active{color:#00d4ff;background:#00d4ff14;border-color:#00d4ff44}
.dbg-tgl .dot{width:5px;height:5px;border-radius:50%;background:#444}
.dbg-tgl.active .dot{background:#00d4ff;box-shadow:0 0 5px #00d4ff}
</style></head><body>
<div class='hdr' style='position:relative'><span class='dbg-id' style='top:4px;right:20px'>HELP-HDR</span><h1>&#x2753; Command Reference</h1><div id='dbg-btn' class='dbg-tgl' onclick='tD()'><div class='dot'></div>DEBUG</div><div><a href='/'>&#x1F4E1; Dashboard</a> <a href='/scheduling'>&#x1F4C5; Schedule</a> <a href='/config'>&#x2699; Config</a></div></div>
<div class='content'>
  <p>The <code>CommandManager</code> routes commands uniformly regardless of the interface they are received on (Serial, LoRa, BLE, or Web UI).</p>

  <h2>Command Routing</h2>
  <ul>
    <li><b>Local:</b> Type the command directly (e.g., <code>STATUS</code>).</li>
    <li><b>Targeted:</b> Prefix the command with a node's Name or MAC Suffix (e.g., <code>Master STATUS</code> or <code>E4 STATUS</code>).</li>
    <li><b>Broadcast:</b> Prefix the command with <code>ALL</code> (e.g., <code>ALL STATUS</code>). Every node will execute it.</li>
  </ul>

  <h2>Global / System Commands</h2>
  <div style='position:relative'><span class='dbg-id'>HELP-GLOBAL-TBL</span>
  <table>
    <tr><th>Command</th><th>Arguments</th><th>Description</th></tr>
    <tr><td><code>SETNAME</code></td><td><code>&lt;name&gt;</code></td><td>Sets friendly display name (1-14 chars). Reboots.</td></tr>
    <tr><td><code>SETWIFI</code></td><td><code>&lt;ssid&gt; &lt;pass&gt;</code></td><td>Sets WiFi credentials. Reboots.</td></tr>
    <tr><td><code>SETIP</code></td><td><code>&lt;ip&gt;</code> / <code>OFF</code></td><td>Sets static IP or returns to DHCP. Reboots.</td></tr>
    <tr><td><code>ESPNOW</code></td><td><code>ON</code> / <code>OFF</code></td><td>Enables/disables high-speed peer network. Reboots.</td></tr>
    <tr><td><code>REPEATER</code></td><td><code>ON</code> / <code>OFF</code></td><td>Enables/disables LoRa repeater mode.</td></tr>
    <tr><td><code>SLEEP</code></td><td><code>&lt;hours&gt;</code></td><td>Deep sleep for X hours.</td></tr>
    <tr><td><code>SETKEY</code></td><td><code>&lt;32_hex&gt;</code></td><td>Sets AES-128 key (32 hex chars). Reboots.</td></tr>
    <tr><td><code>WIPECONFIG</code></td><td><i>(none)</i></td><td>Factory resets all settings. Reboots.</td></tr>
  </table>
  </div>

  <h2>Action & Diagnostic Commands</h2>
  <div style='position:relative'><span class='dbg-id'>HELP-ACTION-TBL</span>
  <table>
    <tr><th>Command</th><th>Arguments</th><th>Description</th></tr>
    <tr><td><code>STATUS</code></td><td><i>(none)</i></td><td>Returns battery, IP, LoRa RSSI, etc.</td></tr>
    <tr><td><code>RADIO</code></td><td><i>(none)</i></td><td>Dumps LoRa diagnostic info to Serial.</td></tr>
    <tr><td><code>READMAC</code></td><td><i>(none)</i></td><td>Returns raw WiFi MAC address.</td></tr>
    <tr><td><code>BLINK</code></td><td><i>(none)</i></td><td>Blinks the onboard LED.</td></tr>
    <tr><td><code>LED</code></td><td><code>ON</code> / <code>OFF</code></td><td>Turns the built-in LED on/off.</td></tr>
    <tr><td><code>GPIO</code></td><td><code>&lt;pin/name&gt; &lt;1/0&gt;</code></td><td>Sets a specific GPIO pin HIGH/LOW.</td></tr>
    <tr><td><code>READ</code></td><td><code>&lt;pin/name&gt;</code></td><td>Reads digital state of a pin.</td></tr>
    <tr><td><code>GETSCHED</code></td><td><i>(none)</i></td><td>Dumps saved JSON schedule.</td></tr>
    <tr><td><code>SETSCHED</code></td><td><code>&lt;json&gt;</code></td><td>Uploads a new JSON schedule.</td></tr>
    <tr><td><code>HELP</code></td><td><i>(none)</i></td><td>Prints a short list of commands on Serial.</td></tr>
  </table>
  </div>

  <h2>Hidden / Debug Commands</h2>
  <ul>
    <li><code>INJECT &lt;cmd&gt;</code>: Simulates receiving a LoRa packet containing a command.</li>
  </ul>

  <h2>Chat Messaging</h2>
  <p>Any text that does not match a known command is treated as a Chat Message. It is displayed on the OLED and transmitted via LoRa to all nodes.</p>
</div>
<script>
function tD(f){
  var b=document.body,btn=document.getElementById('dbg-btn');
  var on=f!==undefined?f:!b.classList.contains('debug-on');
  b.classList.toggle('debug-on',on);
  if(btn)btn.classList.toggle('active',on);
  localStorage.setItem('dMode',on?'1':'0');
}
if(localStorage.getItem('dMode')==='1')tD(true);
</script>
</body></html>)rawhtml";
  server.send(200, "text/html", html);
}

// ============================================================================
//   CONFIG SAVE HANDLER
// ============================================================================
void WiFiManager::serveConfigSave() {
  DataManager &data = DataManager::getInstance();

  if (server.hasArg("dev_name")) {
    String name = server.arg("dev_name");
    name.trim();
    if (name.length() > 0 && name.length() < 15) {
      data.SetName(name);
    }
  }

  if (server.hasArg("repeater")) {
    data.SetRepeater(server.arg("repeater") == "1");
  }

  if (server.hasArg("wifi_ssid")) {
    data.SetWifi(server.arg("wifi_ssid"), server.arg("wifi_pass"));
  }

  if (server.hasArg("static_ip")) {
    data.SetStaticIp(server.arg("static_ip"), server.arg("gateway"),
                     server.arg("subnet"));
  }

  if (server.hasArg("espnow_en")) {
    data.SetESPNowEnabled(server.arg("espnow_en") == "1");
  }

  if (server.hasArg("espnow_ch")) {
    Preferences p;
    p.begin("loralink", false);
    p.putUChar("espnow_ch", server.arg("espnow_ch").toInt());
    p.end();
  }

  // Respond with redirect, then reboot
  server.send(
      200, "text/html",
      "<html><body "
      "style='background:#0f0f1a;color:#00ff88;font-family:sans-serif;"
      "display:flex;align-items:center;justify-content:center;height:100vh'>"
      "<div style='text-align:center'><h2>&#x2705; Settings Saved</h2>"
      "<p>Rebooting in 3 seconds...</p></div></body></html>");

  delay(3000);
  ESP.restart();
}

// ============================================================================
//   API ENDPOINTS
// ============================================================================
void WiFiManager::serveApiStatus() {
  DataManager &data = DataManager::getInstance();
  LoRaManager &lora = LoRaManager::getInstance();
  ESPNowManager &espnow = ESPNowManager::getInstance();

  float bat = (analogRead(PIN_BAT_ADC) / 4095.0f) * 3.3f * BAT_VOLT_MULTI;
  unsigned long s = millis() / 1000;
  String uptime = String(s / 3600) + "h " + String((s % 3600) / 60) + "m";

  PerformanceManager &perf = PerformanceManager::getInstance();
  perf.reportConfiguratorActivity();

  String json = "{";
  json += "\"id\":\"" + data.myId + "\",";
  json += "\"version\":\"" FIRMWARE_VERSION "\",";
  json += "\"uptime\":\"" + uptime + "\",";
  json += "\"espnow_ch\":" + String(data.espNowChannel) + ",";
  json += "\"espnow_rx\":" + String(espnow.rxCount) + ",";
  json += "\"espnow_tx\":" + String(espnow.txCount) + ",";
  json += "\"espnow_drop\":" + String(espnow.txDropCount) + ",";
  json += "\"espnow_ok\":" + String(espnow.lastSendSuccess) + ",";
  json += "\"espnow_peers\":" + String(data.numEspNowPeers) + ",";
  json += "\"hw\":\"" + data.getMacSuffix() + "\",";
  json += "\"reset\":\"" + data.getResetReason() + "\",";
  {
    String lc = lora.lastMsgReceived;
    lc.replace("\"", "\\\"");
    lc.replace("\n", " ");
    lc.replace("\r", "");
    json += "\"last_cmd\":\"" + lc + "\",";
  }
  json += "\"bat\":" + String(bat, 2) + ",";
  json += "\"rssi\":" + String(lora.lastRssi) + ",";
  json += "\"nodes\":" + String(data.numNodes) + ",";
  json += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"tp_mode\":\"" + String(data.transportMode) + "\",";
  json += "\"lora\":" + String(lora.loraActive ? "true" : "false") + ",";
  json += "\"lora_tx\":" + String(lora.getTxCount()) + ",";
  json += "\"lora_rx\":" + String(lora.getRxCount()) + ",";
  json += "\"lora_drop\":" + String(lora.getTxDropCount()) + ",";
  {
    const char *rs = "idle";
    switch (lora.getRadioState()) {
      case RadioState::RADIO_TX: rs = "tx"; break;
      case RadioState::RADIO_RX: rs = "rx"; break;
      default: break;
    }
    json += "\"radio_state\":\"" + String(rs) + "\",";
  }
  json += "\"ble\":true,";
  json += "\"wifi\":true,";
  json += "\"espnow\":" + String(espnow.espNowActive ? "true" : "false") + ",";
  json += "\"loop_avg_ms\":" + String(perf.getLoopAvgMs()) + ",";
  json += "\"loop_max_ms\":" + String(perf.getLoopMaxMs()) + ",";
  json += "\"lora_toa_ms\":" + String(perf.getTimeOnAir()) + ",";
  json += "\"bytes_saved\":" + String(perf.getBytesSaved()) + ",";
  json +=
      "\"power_mode\":\"" + PowerManager::getInstance().getModeString() + "\",";
  json += "\"sys_reset\":\"" + perf.getResetReason() + "\",";

  // Standard pins array for labeled diagnostics - FILTERED to "Usable" only
  json += "\"pins\":[";
  struct PinDef {
    const char *n;
    int p;
    bool a;
  };
  PinDef pins[] = {
      {"LED", PIN_LED_BUILTIN, false},    {"PRG", PIN_BUTTON_PRG, false},
      {"VEXT", PIN_VEXT_CTRL, false},     {"BAT", PIN_BAT_ADC, true},
      {"BAT_CTRL", PIN_BAT_CTRL, false},  {"RL110", PIN_RELAY_110V, false},
      {"RL12_1", PIN_RELAY_12V_1, false}, {"RL12_2", PIN_RELAY_12V_2, false},
      {"RL12_3", PIN_RELAY_12V_3, false}};

  bool pinFirst = true;
  for (int i = 0; i < 9; i++) {
    // Failsafe: only show pins that are enabled via APC OR are critical system
    // pins
    String friendlyName = data.GetPinName(String(pins[i].p));
    bool isEnabled = data.GetPinEnabled(pins[i].p);
    bool isUsable =
        isEnabled ||
        (i < 5); // Always show LED, PRG, VEXT, BAT, BAT_CTRL regardless of APC

    if (!isUsable)
      continue;

    if (!pinFirst)
      json += ",";
    json += "{\"n\":\"" +
            (friendlyName.length() ? friendlyName : String(pins[i].n)) +
            "\",\"p\":" + String(pins[i].p) +
            ",\"v\":" + String(digitalRead(pins[i].p));
    if (pins[i].a)
      json += ",\"a\":" + String(analogRead(pins[i].p));
    json += "}";
    pinFirst = false;
  }
  json += "],";

  // Comprehensive GPIO Bitmask (0-47)
  uint64_t mask = 0;
  for (int i = 0; i < 48; i++) {
    // Skip flash/invalid pins to prevent crashes
    if (i >= 26 && i <= 32)
      continue;
    if (digitalRead(i))
      mask |= (1ULL << i);
  }
  char hex[17];
  sprintf(hex, "%012llX", mask);
  json += "\"gp\":\"" + String(hex) + "\",";

  // Mesh neighbor table
  json += "\"mesh\":[";
  bool meshFirst = true;
  unsigned long now = millis();
  for (int i = 0; i < data.numNodes; i++) {
    if (!meshFirst)
      json += ",";
    json += "{";
    json += "\"id\":\"" + String(data.remoteNodes[i].id) + "\",";
    json += "\"bat\":" + String(data.remoteNodes[i].battery, 2) + ",";
    json += "\"rssi\":" + String(data.remoteNodes[i].rssi) + ",";
    json += "\"hops\":" + String(data.remoteNodes[i].hops) + ",";
    json += "\"ago\":" + String((now - data.remoteNodes[i].lastSeen) / 1000);
    json += "}";
    meshFirst = false;
  }
  json += "],";
  json += "\"peripherals\":" + data.GetPeripheralsJson() + ",";
  json += "\"log\":[";
  bool first = true;
  for (int i = 0; i < LOG_SIZE; i++) {
    int idx = (data.logIndex - 1 - i + LOG_SIZE) % LOG_SIZE;
    if (strlen(data.msgLog[idx].message) > 0) {
      String sanitized = String(data.msgLog[idx].message);
      sanitized.replace("\"", "'");
      String cleanStr = "";
      for (unsigned int j = 0; j < sanitized.length(); j++) {
        char c = sanitized[j];
        if (c >= 0x20 && c <= 0x7E) {
          cleanStr += c;
        }
      }
      if (!first)
        json += ",";

      json += "{";
      json += "\"ts\":" + String(data.msgLog[idx].timestamp) + ",";
      json += "\"src\":\"" + String(data.msgLog[idx].source) + "\",";
      json += "\"rssi\":" + String(data.msgLog[idx].rssi) + ",";
      json += "\"msg\":\"" + cleanStr + "\"";
      json += "}";

      first = false;
    }
  }
  json += "]}";

  server.send(200, "application/json", json);
}

void WiFiManager::serveApiPinEnable() {
  if (server.hasArg("pin") && server.hasArg("en")) {
    int pin = server.arg("pin").toInt();
    bool en = server.arg("en") == "1";
    DataManager::getInstance().SetPinEnabled(pin, en);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing pin or en");
  }
}

void WiFiManager::serveApiTransportMode() {
  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    if (mode.length() == 1) {
      DataManager::getInstance().SetTransportMode(mode.charAt(0));
      server.send(200, "text/plain", "OK");
      return;
    }
  }
  server.send(400, "text/plain", "Invalid mode");
}

void WiFiManager::serveApiRegistry() {
  String json = DataManager::getInstance().GetRegistryJson();
  server.send(200, "application/json", json);
}

void WiFiManager::serveApiProductSave() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"Missing body\"}");
    return;
  }
  String body = server.arg("plain");
  // Extract name for response before saving
  JsonDocument doc;
  String name = "";
  if (deserializeJson(doc, body) == DeserializationError::Ok)
    name = doc["name"] | "";
  bool ok = ProductManager::getInstance().saveProduct(body);
  if (ok) {
    server.send(200, "application/json",
                "{\"ok\":true,\"name\":\"" + name + "\"}");
  } else {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"Save failed — check JSON and 'name' "
                "field\"}");
  }
}

void WiFiManager::serveApiCmd() {
  if (server.hasArg("cmd")) {
    String cmd = server.arg("cmd");
    if (_webCmdCallback) {
      _webCmdCallback(cmd, CommInterface::COMM_WIFI);
    }
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing cmd");
  }
}

void WiFiManager::serveApiPeers() {
  DataManager &data = DataManager::getInstance();

  String json = "[";
  bool first = true;
  for (int i = 0; i < data.numEspNowPeers; i++) {
    if (data.espNowPeers[i].active) {
      if (!first)
        json += ",";
      char macStr[18];
      sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X",
              data.espNowPeers[i].mac[0], data.espNowPeers[i].mac[1],
              data.espNowPeers[i].mac[2], data.espNowPeers[i].mac[3],
              data.espNowPeers[i].mac[4], data.espNowPeers[i].mac[5]);
      json += "{\"mac\":\"" + String(macStr) + "\",\"name\":\"" +
              String(data.espNowPeers[i].name) + "\"}";
      first = false;
    }
  }
  json += "]";
  server.send(200, "application/json", json);
}

void WiFiManager::serveApiAddPeer() {
  if (!server.hasArg("mac") || !server.hasArg("name")) {
    server.send(400, "text/plain", "Missing mac or name");
    return;
  }

  String macStr = server.arg("mac");
  String name = server.arg("name");

  // Parse MAC "AA:BB:CC:DD:EE:FF"
  uint8_t mac[6];
  int parsed = sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0],
                      &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
  if (parsed != 6) {
    server.send(400, "text/plain", "Invalid MAC format");
    return;
  }

  bool ok = ESPNowManager::getInstance().addPeer(mac, name.c_str());
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Failed");
}

void WiFiManager::serveApiRemovePeer() {
  if (!server.hasArg("mac")) {
    server.send(400, "text/plain", "Missing mac");
    return;
  }

  String macStr = server.arg("mac");
  uint8_t mac[6];
  int parsed = sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0],
                      &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
  if (parsed != 6) {
    server.send(400, "text/plain", "Invalid MAC format");
    return;
  }

  bool ok = ESPNowManager::getInstance().removePeer(mac);
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Failed");
}

// ============================================================================
//   INTEGRATION PAGE
// ============================================================================
void WiFiManager::serveIntegration() {
  DataManager &data = DataManager::getInstance();

  String html = R"rawhtml(<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>LoRaLink Integrations</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',sans-serif;background:#0f0f1a;color:#e0e0e0;min-height:100vh}
.hdr{background:linear-gradient(135deg,#1a1a2e,#16213e);padding:16px 20px;border-bottom:1px solid #2a2a4a;display:flex;justify-content:space-between;align-items:center}
.hdr h1{font-size:1.3em;color:#00d4ff;font-weight:600}
.hdr a{color:#888;text-decoration:none;font-size:0.85em;padding:6px 14px;border:1px solid #2a2a4a;border-radius:6px}
.hdr a:hover{color:#00d4ff;border-color:#00d4ff}
.sec{margin:16px;background:#1a1a2e;border-radius:10px;border:1px solid #2a2a4a;padding:20px;position:relative}\n.dbg-id{position:absolute;right:8px;top:8px;font-size:9px;color:#555;opacity:0.6;pointer-events:none;font-family:monospace;letter-spacing:0.5px;z-index:10}
.sec h2{color:#00d4ff;font-size:1em;margin-bottom:12px;padding-bottom:8px;border-bottom:1px solid #2a2a4a}
.row{display:flex;gap:12px;margin-bottom:10px;align-items:center;flex-wrap:wrap}
.row label{width:160px;font-size:0.85em;color:#aaa;flex-shrink:0}
.row input,.row select{flex:1;background:#0f0f1a;border:1px solid #2a2a4a;border-radius:6px;padding:8px 12px;color:#fff;outline:none;min-width:140px}
.row input:focus,.row select:focus{border-color:#00d4ff}
.btn{background:#00d4ff;border:none;border-radius:8px;padding:10px 24px;color:#0f0f1a;cursor:pointer;font-weight:600;margin-top:12px}
.btn:hover{background:#00b8d4}
.msg{background:#00ff8822;color:#00ff88;border:1px solid #00ff8844;border-radius:8px;padding:10px 16px;margin:16px;text-align:center;display:none}
</style></head><body>
<div class='hdr'><h1>&#x1F50C; Integrations</h1><div><a href='/'>&#x1F4E1; Dashboard</a> <a href='/scheduling'>&#x1F4C5; Schedule</a> <a href='/config'>&#x2699; Config</a></div></div>
)rawhtml";

  if (server.hasArg("saved")) {
    html += "<div class='msg' style='display:block'>Settings saved. "
            "Rebooting...</div>";
  }

  html += R"rawhtml(<form method='POST' action='/integration'>

<div class='sec'>
<span class='dbg-id'>INT-STREAM</span>
<h2>&#x1F4CA; Excel Data Streamer</h2>
<p style='font-size:0.85em;color:#888;margin-bottom:12px'>Outputs live telemetry and messages to the USB Serial port in CSV format.</p>
<div class='row'><label>Serial CSV Stream</label><select name='stream'><option value='0')rawhtml";
  if (!data.streamToSerial)
    html += " selected";
  html += R"rawhtml(>OFF</option><option value='1')rawhtml";
  if (data.streamToSerial)
    html += " selected";
  html += R"rawhtml(>ON</option></select></div>
</div>

<div class='sec'>
<span class='dbg-id'>INT-MQTT</span>
<h2>&#x1F310; MQTT Broker</h2>
<p style='font-size:0.85em;color:#888;margin-bottom:12px'>Publishes telemetry to <code>loralink/telemetry/&lt;Node&gt;</code> and messages to <code>loralink/msg/&lt;Node&gt;</code>.</p>
<div class='row'><label>MQTT Enabled</label><select name='mqtt_en'><option value='0')rawhtml";
  if (!data.mqttEnabled)
    html += " selected";
  html += R"rawhtml(>OFF</option><option value='1')rawhtml";
  if (data.mqttEnabled)
    html += " selected";
  html +=
      R"rawhtml(>ON</option></select></div>
<div class='row'><label>Server Address</label><input name='mqtt_srv' value=')rawhtml" +
      data.mqttServer +
      R"rawhtml(' placeholder='e.g. 192.168.1.100 or broker.hivemq.com'></div>
<div class='row'><label>Server Port</label><input name='mqtt_port' type='number' value=')rawhtml" +
      String(data.mqttPort) + R"rawhtml('></div>
<div class='row'><label>Username (Optional)</label><input name='mqtt_user' value=')rawhtml" +
      data.mqttUser + R"rawhtml('></div>
<div class='row'><label>Password (Optional)</label><input name='mqtt_pass' type='password' value=')rawhtml" +
      data.mqttPass + R"rawhtml('></div>
</div>

<div class='sec'>
<button type='submit' class='btn'>&#x1F4BE; Save & Reboot</button>
</div>

</form>
</body></html>)rawhtml";

  server.send(200, "text/html", html);
}

void WiFiManager::serveScheduling() {
  DataManager &data = DataManager::getInstance();
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>LoRaLink - Scheduling</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.0.0/css/all.min.css">
    <style>
        :root {
            --primary: #00f2fe;
            --secondary: #4facfe;
            --bg: #0f172a;
            --card: rgba(30, 41, 59, 0.7);
            --text: #f8fafc;
            --text-dim: #94a3b8;
            --accent: #22d3ee;
            --glass: rgba(255, 255, 255, 0.05);
            --border: rgba(255, 255, 255, 0.1);
            --success: #10b981;
            --danger: #ef4444;
        }

        * { margin:0; padding:0; box-sizing:border-box; font-family: 'Inter', system-ui, sans-serif; }
        body { background: var(--bg); color: var(--text); min-height: 100vh; line-height: 1.6; }
        .glass { background: var(--card); backdrop-filter: blur(12px); border: 1px solid var(--border); border-radius: 16px; }

        header {
            padding: 2rem 1rem; text-align: center;
            background: linear-gradient(to bottom, rgba(15,23,42,0.8), transparent);
        }
        .container { max-width: 1200px; margin: 0 auto; padding: 0 1rem 4rem; }

        h1 { font-size: 2.5rem; font-weight: 800; background: linear-gradient(135deg, var(--primary), var(--secondary)); -webkit-background-clip: text; -webkit-text-fill-color: transparent; margin-bottom: 0.5rem; }
        .version { color: var(--text-dim); font-size: 0.8rem; letter-spacing: 2px; text-transform: uppercase; margin-top: 5px; }

        .nav-chips { display: flex; gap: 0.75rem; justify-content: center; margin: 2rem 0; flex-wrap: wrap; }
        .chip { padding: 0.6rem 1.2rem; border-radius: 100px; text-decoration: none; color: var(--text-dim); transition: 0.3s; font-weight: 500; font-size: 0.9rem; }
        .chip:hover { background: var(--glass); color: var(--text); }
        .chip.active { background: linear-gradient(135deg, var(--primary), var(--secondary)); color: var(--bg); }

        .dashboard-grid { display: grid; grid-template-columns: 350px 1fr; gap: 1.5rem; }
        @media (max-width: 900px) { .dashboard-grid { grid-template-columns: 1fr; } }

        .card { padding: 1.5rem; margin-bottom: 1.5rem; box-shadow: 0 8px 32px rgba(0,0,0,0.3); position: relative; overflow: hidden; }
        .dbg-id { position: absolute; right: 8px; top: 8px; font-size: 10px; color: #555; opacity: 0.6; pointer-events: none; font-family: monospace; letter-spacing: 0.5px; z-index: 10; }
        .card-header { display: flex; align-items: center; gap: 0.75rem; margin-bottom: 1.5rem; color: var(--accent); }
        .card-header i { font-size: 1.2rem; }
        .card-header h2 { font-size: 1rem; text-transform: uppercase; letter-spacing: 1px; }

        .input-group { margin-bottom: 1.25rem; }
        .input-group label { display: block; color: var(--text-dim); font-size: 0.75rem; margin-bottom: 0.5rem; text-transform: uppercase; letter-spacing: 0.5px; }
        .input-group input, .input-group select {
            width: 100%; padding: 0.8rem 1rem; background: rgba(0,0,0,0.3); border: 1px solid var(--border);
            border-radius: 10px; color: var(--text); transition: 0.3s; font-size: 0.9rem;
        }
        .input-group input:focus { border-color: var(--primary); outline: none; background: rgba(0,0,0,0.4); box-shadow: 0 0 15px rgba(0, 242, 254, 0.1); }

        .btn {
            width: 100%; padding: 0.85rem; border-radius: 10px; border: none; font-weight: 600; cursor: pointer;
            transition: 0.3s; display: flex; align-items: center; justify-content: center; gap: 0.6rem; margin-top: 0.5rem;
            font-size: 0.9rem;
        }
        .btn-primary { background: linear-gradient(135deg, var(--primary), var(--secondary)); color: var(--bg); }
        .btn-primary:hover { transform: translateY(-2px); box-shadow: 0 5px 20px rgba(0, 242, 254, 0.4); }
        .btn-secondary { background: var(--glass); color: var(--text); border: 1px solid var(--border); }
        .btn-secondary:hover { background: rgba(255,255,255,0.1); border-color: var(--text-dim); }
        .btn-danger { background: rgba(239, 68, 68, 0.1); color: var(--danger); border: 1px solid rgba(239, 68, 68, 0.2); }
        .btn-danger:hover { background: var(--danger); color: white; }

        .sched-list { display: flex; flex-direction: column; gap: 1rem; }
        .task-item {
            padding: 1.5rem; display: flex; justify-content: space-between; align-items: center;
            background: rgba(255,255,255,0.02); border-radius: 16px; border: 1px solid var(--border);
            transition: 0.3s; position: relative; overflow: hidden;
        }
        .task-item:hover { background: rgba(255,255,255,0.05); border-color: var(--accent); }

        .task-content { flex-grow: 1; }
        .task-header { display: flex; align-items: center; gap: 1rem; margin-bottom: 0.5rem; }
        .task-header h3 { font-size: 1.1rem; color: var(--primary); font-weight: 700; }
        .task-badge { padding: 0.2rem 0.6rem; border-radius: 6px; font-size: 0.65rem; background: var(--glass); color: var(--accent); text-transform: uppercase; font-weight: 700; }

        .task-meta { font-size: 0.85rem; color: var(--text-dim); display: flex; gap: 1.5rem; align-items: center; }
        .task-meta span { display: flex; align-items: center; gap: 0.4rem; }
        .task-meta i { font-size: 0.9rem; color: var(--secondary); }

        .live-indicator {
            width: 10px; height: 10px; border-radius: 50%; display: inline-block;
            box-shadow: 0 0 10px currentColor; position: relative;
        }
        .state-active { color: var(--success); background: var(--success); }
        .state-inactive { color: var(--danger); background: var(--danger); }
        .pulse-ring {
            position: absolute; width: 100%; height: 100%; border-radius: 50%;
            border: 2px solid currentColor; animation: pulse 2s infinite; opacity: 0;
        }
        @keyframes pulse { 0% { transform: scale(1); opacity: 0.8; } 100% { transform: scale(3); opacity: 0; } }

        .next-run { background: rgba(34, 211, 238, 0.1); padding: 0.3rem 0.6rem; border-radius: 6px; font-family: monospace; font-size: 0.8rem; color: var(--accent); }

        .help-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 1.5rem; }
        .help-card { padding: 1.25rem; background: rgba(0,0,0,0.2); border-radius: 12px; border: 1px solid var(--border); }
        .help-card h4 { font-size: 0.85rem; color: var(--primary); margin-bottom: 0.75rem; text-transform: uppercase; display: flex; align-items: center; gap: 0.5rem; }
        .help-card p { font-size: 0.8rem; color: var(--text-dim); }

        .toast {
            position: fixed; bottom: 2rem; left: 50%; transform: translate(-50%, 100px);
            padding: 1rem 2rem; border-radius: 12px; background: var(--card); color: white;
            box-shadow: 0 10px 40px rgba(0,0,0,0.5); border: 1px solid var(--border);
            transition: 0.5s cubic-bezier(0.18, 0.89, 0.32, 1.28); z-index: 1000;
            display: flex; align-items: center; gap: 0.75rem;
        }
        .toast.show { transform: translate(-50%, 0); }

        .drop-zone {
            width:100%; min-height:90px; padding:0.7rem;
            background:rgba(0,0,0,0.3); border:1.5px dashed var(--border);
            border-radius:10px; color:var(--text-dim); font-size:0.78rem;
            resize:vertical; font-family:monospace; transition:0.3s; line-height:1.4;
        }
        .drop-zone.drag-over { border-color:var(--primary); background:rgba(0,242,254,0.06); box-shadow:0 0 18px rgba(0,242,254,0.15); }
        .import-preview { margin-top:0.75rem; padding:0.6rem 0.8rem; border-radius:8px; font-size:0.78rem; line-height:1.6; display:none; }
        .import-preview.ok { background:rgba(16,185,129,0.1); border:1px solid rgba(16,185,129,0.3); color:var(--success); }
        .import-preview.err { background:rgba(239,68,68,0.1); border:1px solid rgba(239,68,68,0.3); color:var(--danger); }
    </style>
</head>
<body>
    <header>
        <div class="version">Infrastructure Grid Status</div>
        <h1 id="devName"><span class="dbg-id">SCH-DEVICE-HDR</span>)rawliteral" +
                data.myId + R"rawliteral(</h1>
        <div class="version">FIRMWARE )rawliteral" +
                String(FIRMWARE_VERSION) + R"rawliteral(</div>
    </header>

    <div class="container">
        <div class="nav-chips">
            <span class="dbg-id">SCH-NAV</span>
            <a href="/" class="chip">Dashboard</a>
            <a href="/config" class="chip">Hardware</a>
            <a href="/scheduling" class="chip active">Scheduling</a>
            <a href="/integration" class="chip">Integrations</a>
        </div>

        <div class="dashboard-grid">
            <div class="left-col">
                <div class="card glass">
                    <div class="card-header"><i class="fas fa-calendar-plus"></i><span class="dbg-id">SCH-NEW</span><h2>New Task</h2></div>
                    <div class="input-group">
                        <label>Identifier</label>
                        <input type="text" id="name" placeholder="e.g. HydroPump_01">
                    </div>
                    <div class="input-group">
                        <label>Operation Mode</label>
                        <select id="type">
                            <option value="TOGGLE">Recurring Toggle</option>
                            <option value="PULSE">Precision Pulse</option>
                            <option value="DHT_SAFETY">DHT Safety Interlock</option>
                        </select>
                    </div>
                    <div class="input-group">
                        <label>Hardware Pin / Alias</label>
                        <input type="text" id="pin" placeholder="e.g. 21 or RELAY1">
                    </div>
                    <div class="input-group">
                        <label>Interval (Seconds)</label>
                        <input type="number" id="interval" value="60">
                    </div>
                    <div class="input-group" id="durGroup" style="display:none;">
                        <label>Active Duration (Seconds)</label>
                        <input type="number" id="duration" value="5">
                    </div>
                    <button class="btn btn-primary" onclick="addTask()"><i class="fas fa-bolt"></i> Push to Memory</button>
                    <button class="btn btn-secondary" onclick="saveTasks()"><i class="fas fa-save"></i> Save Configuration</button>
                    <button class="btn btn-danger" onclick="clearTasks()"><i class="fas fa-trash-alt"></i> Wipe All Tasks</button>
                </div>

                <div class="card glass">
                    <div class="card-header"><i class="fas fa-tags"></i><span class="dbg-id">SCH-ALIASES</span><h2>Friendly Names</h2></div>
                    <p style="font-size: 0.75rem; color: var(--text-dim); margin-bottom: 1rem; padding: 0 0.5rem;">Map GPIO numbers to descriptive names permanently.</p>
                    <div class="input-group">
                        <label>GPIO Index</label>
                        <input type="number" id="pinNum" placeholder="e.g. 21">
                    </div>
                    <div class="input-group">
                        <label>Descriptive Label</label>
                        <input type="text" id="friendlyName" placeholder="e.g. OxygenFan">
                    </div>
                    <button class="btn btn-secondary" onclick="savePinName()"><i class="fas fa-id-card"></i> Persistent Map</button>
                </div>

                <div class="card glass">
                    <div class="card-header"><i class="fas fa-file-import"></i><span class="dbg-id">SCH-IMPORT</span><h2>Bulk Import</h2></div>
                    <p style="font-size:0.75rem;color:var(--text-dim);margin-bottom:1rem;padding:0 0.5rem;">Drop a <b>.json</b> or <b>.csv</b> file onto the box below, or paste content directly.</p>
                    <textarea id="importBox" class="drop-zone" rows="6" placeholder='JSON: [{"name":"pump1","type":"TOGGLE","pin":6,"interval":60,"duration":0}]&#10;&#10;CSV:&#10;name,type,pin,interval,duration&#10;pump1,TOGGLE,6,60,0&#10;pump2,PULSE,7,30,5'></textarea>
                    <div id="importPreview" class="import-preview"></div>
                    <button class="btn btn-secondary" style="margin-top:0.75rem;" onclick="previewImport()"><i class="fas fa-search"></i> Parse &amp; Preview</button>
                    <button class="btn btn-primary" id="importBtn" style="display:none;" onclick="importTasks()"><i class="fas fa-file-import"></i> Import <span id="importCount">0</span> Task(s)</button>
                </div>
            </div>

            <div class="right-col">
                <div class="card glass" style="min-height: 450px;">
                    <div class="card-header"><i class="fas fa-wave-square"></i><span class="dbg-id">SCH-LIST</span><h2>Active Tasks</h2></div>
                    <div id="taskList" class="sched-list">
                        <!-- Loaded via JS -->
                        <div style="text-align:center; color:var(--text-dim); margin-top:5rem;">
                            <i class="fas fa-satellite-dish fa-spin" style="font-size: 2rem; margin-bottom: 1rem;"></i>
                            <p>Polling node state...</p>
                        </div>
                    </div>
                </div>

                <div class="card glass">
                    <div class="card-header"><i class="fas fa-info-circle"></i><span class="dbg-id">SCH-HELP</span><h2>Help & Syntax Reference</h2></div>
                    <div class="help-grid">
                        <div class="help-card">
                            <h4><i class="fas fa-clock"></i> Time Units</h4>
                            <p>All intervals and durations are now defined in <b>Seconds</b>. Decimal values are supported via the API.</p>
                        </div>
                        <div class="help-card">
                            <h4><i class="fas fa-microchip"></i> Pin Addressing</h4>
                            <p>Use GPIO numbers or aliases like <b>RELAY1 (21)</b>. Mapping names makes logs and UI easier to read.</p>
                        </div>
                        <div class="help-card">
                            <h4><i class="fas fa-sync-alt"></i> Pulse Mode</h4>
                            <p>Activates pin, waits for <i>duration</i>, then deactivates. Ideal for solenoid valves or momentary relays.</p>
                        </div>
                        <div class="help-card">
                            <h4><i class="fas fa-shield-alt"></i> Persistence</h4>
                            <p><b>Push</b> applies changes immediately. <b>Save</b> writes to encrypted flash for survival across power cycles.</p>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    </div>

    <div id="toast" class="toast"><i class="fas fa-check-circle"></i><span id="toastMsg">Action Successful</span></div>

    <script>
        const toastEl = document.getElementById('toast');
        const toastMsg = document.getElementById('toastMsg');

        function showToast(msg, icon='check-circle', color='') {
            toastMsg.innerText = msg;
            toastEl.querySelector('i').className = `fas fa-${icon}`;
            toastEl.style.borderColor = color || 'var(--border)';
            toastEl.classList.add('show');
            setTimeout(() => toastEl.classList.remove('show'), 3000);
        }

        async function loadTasks() {
            try {
                const res = await fetch('/api/schedule');
                if(!res.ok) throw new Error();
                const data = await res.json();
                const list = document.getElementById('taskList');
                // Keep _liveTasks in sync so previewImport() can deduplicate
                _liveTasks = data.schedules.map(function(s) {
                    return { name: s.name, pin: String(s.pin) };
                });

                if (data.schedules.length === 0) {
                    list.innerHTML = '<div style="text-align:center; color:var(--text-dim); margin-top:5rem;"><i class="fas fa-clipboard" style="font-size:2rem; margin-bottom:1rem; opacity:0.3"></i><p>No dynamic tasks configured</p></div>';
                    return;
                }

                let newHtml = '';
                data.schedules.forEach(s => {
                    const pinLabel = s.pinName ? `${s.pinName} (${s.pin})` : `GPIO ${s.pin}`;
                    const stateClass = s.state == 1 ? 'state-active' : 'state-inactive';
                    const stateText = s.state == 1 ? 'HIGH' : 'LOW';

                    newHtml += `
                        <div class="task-item">
                            <div class="task-content">
                                <div class="task-header">
                                    <h3>${s.name}</h3>
                                    <span class="task-badge">${s.type}</span>
                                    <div class="live-indicator ${stateClass}">
                                        ${s.state == 1 ? '<div class="pulse-ring"></div>' : ''}
                                    </div>
                                    <span style="font-size:0.7rem; font-weight:700; color:${s.state == 1 ? 'var(--success)' : 'var(--text-dim)'}">${stateText}</span>
                                </div>
                                <div class="task-meta">
                                    <span><i class="fas fa-microchip"></i>${pinLabel}</span>
                                    <span><i class="fas fa-history"></i>${s.interval}s</span>
                                    ${s.duration > 0 ? `<span><i class="fas fa-hourglass-start"></i>${s.duration}s</span>` : ''}
                                    <span class="next-run" title="Next Activation Countdown">
                                        <i class="fas fa-stopwatch"></i> Run in ${Math.max(0, s.nextRun)}s
                                    </span>
                                </div>
                                <div style="font-size:0.65rem; color:var(--text-dim); margin-top:0.6rem; opacity:0.6">
                                    <i class="fas fa-user-edit"></i> ${s.updatedBy} &middot; <i class="fas fa-history"></i> ${s.lastUpdated}
                                </div>
                            </div>
                            <button class="btn btn-danger" style="width:40px; height:40px; padding:0; border-radius:50%;" onclick="removeTask('${s.name}')">
                                <i class="fas fa-times"></i>
                            </button>
                        </div>
                    `;
                });
                list.innerHTML = newHtml;
            } catch (err) {
                console.error(err);
            }
        }

        async function addTask() {
            const params = new URLSearchParams({
                name: document.getElementById('name').value,
                type: document.getElementById('type').value,
                pin: document.getElementById('pin').value,
                interval: document.getElementById('interval').value,
                duration: document.getElementById('duration').value,
                enabled: 1
            });
            const res = await fetch('/api/schedule/add', { method: 'POST', body: params });
            if(res.ok) {
                showToast('Task Injected Successfully');
                loadTasks();
            }
        }

        async function removeTask(name) {
            const res = await fetch('/api/schedule/remove', { method: 'POST', body: new URLSearchParams({ name }) });
            if(res.ok) {
                showToast('Task Removed', 'times', 'var(--danger)');
                loadTasks();
            }
        }

        async function clearTasks() {
            if(!confirm('This will wipe ALL dynamic tasks from memory. Proceed?')) return;
            const res = await fetch('/api/schedule/clear', { method: 'POST' });
            if(res.ok) {
                showToast('Schedule Wiped', 'trash', 'var(--danger)');
                loadTasks();
            }
        }

        async function saveTasks() {
            const res = await fetch('/api/schedule/save', { method: 'POST' });
            if(res.ok) showToast('Configuration Written to Flash', 'cloud-upload-alt', 'var(--primary)');
        }

        async function savePinName() {
            const pin = document.getElementById('pinNum').value;
            const name = document.getElementById('friendlyName').value;
            if(!pin || !name) return;
            const res = await fetch('/api/pins/name', { method: 'POST', body: new URLSearchParams({ pin, name }) });
            if(res.ok) {
                showToast(`Pin ${pin} mapped to ${name}`, 'tag');
                loadTasks();
            }
        }

        document.getElementById('type').onchange = (e) => {
            document.getElementById('durGroup').style.display = (e.target.value === 'PULSE') ? 'block' : 'none';
        };

        // ── Bulk Import ─────────────────────────────────────────────
        var _importTasks = [];
        var _liveTasks   = [];   // {name, pin} of currently-active firmware tasks

        // Wire drag-and-drop onto the textarea
        (function() {
            var box = document.getElementById('importBox');
            box.addEventListener('dragover', function(e) {
                e.preventDefault();
                box.classList.add('drag-over');
            });
            box.addEventListener('dragleave', function() { box.classList.remove('drag-over'); });
            box.addEventListener('drop', function(e) {
                e.preventDefault();
                box.classList.remove('drag-over');
                var file = e.dataTransfer.files[0];
                if (!file) return;
                var reader = new FileReader();
                reader.onload = function(ev) {
                    box.value = ev.target.result;
                    previewImport();
                };
                reader.readAsText(file);
            });
        })();

        function parseImport() {
            var raw = document.getElementById('importBox').value.trim();
            if (!raw) return [];
            // JSON branch
            if (raw.charAt(0) === '[' || raw.charAt(0) === '{') {
                try {
                    var parsed = JSON.parse(raw);
                    if (!Array.isArray(parsed)) parsed = [parsed];
                    return parsed.map(function(t) {
                        return {
                            name:     String(t.name     || ''),
                            type:     String(t.type     || 'TOGGLE').toUpperCase(),
                            pin:      String(t.pin      || ''),
                            interval: String(t.interval !== undefined ? t.interval : '60'),
                            duration: String(t.duration !== undefined ? t.duration : '0'),
                            enabled:  String(t.enabled  !== undefined ? t.enabled  : '1')
                        };
                    }).filter(function(t) { return t.name && t.pin; });
                } catch(e) { return null; }  // null = syntax error
            }
            // CSV branch — first row is header
            var lines = raw.split('\n').map(function(l) { return l.trim(); }).filter(Boolean);
            if (lines.length < 2) return [];
            var headers = lines[0].split(',').map(function(h) { return h.trim().toLowerCase(); });
            var tasks = [];
            for (var i = 1; i < lines.length; i++) {
                var cols = lines[i].split(',').map(function(c) { return c.trim(); });
                var t = {};
                headers.forEach(function(h, idx) { t[h] = cols[idx] || ''; });
                if (t.name && t.pin) {
                    tasks.push({
                        name:     t.name,
                        type:     (t.type || 'TOGGLE').toUpperCase(),
                        pin:      t.pin,
                        interval: t.interval || '60',
                        duration: t.duration || '0',
                        enabled:  t.enabled  || '1'
                    });
                }
            }
            return tasks;
        }

        function previewImport() {
            var preview = document.getElementById('importPreview');
            var btn     = document.getElementById('importBtn');
            var counter = document.getElementById('importCount');
            var tasks   = parseImport();

            if (tasks === null) {
                preview.className   = 'import-preview err';
                preview.style.display = 'block';
                preview.innerHTML   = '<i class="fas fa-exclamation-triangle"></i> JSON syntax error — check brackets and quotes';
                btn.style.display   = 'none';
                return;
            }
            if (!tasks.length) {
                preview.className   = 'import-preview err';
                preview.style.display = 'block';
                preview.innerHTML   = '<i class="fas fa-exclamation-triangle"></i> No valid tasks found — each row needs at least <b>name</b> and <b>pin</b>';
                btn.style.display   = 'none';
                return;
            }

            // ── Validation rules A-D ──────────────────────────────────────
            var VALID_TYPES = ['TOGGLE', 'PULSE', 'DHT_SAFETY'];
            var liveNames   = _liveTasks.map(function(t) { return t.name; });
            var errors = [], warnings = [];
            tasks.forEach(function(t) {
                // A — pin 14 is shared with LORA_DIO1; enabling both is a hw fault
                if (String(t.pin) === '14') {
                    errors.push('<b>' + t.name + '</b>: pin 14 forbidden (shared with LORA_DIO1)');
                }
                // B — firmware stores name in a fixed 8-char buffer
                if (t.name.length > 8) {
                    errors.push('<b>' + t.name + '</b>: name too long (' + t.name.length + ' chars, max 8)');
                }
                // D — firmware only handles known task types
                if (VALID_TYPES.indexOf(String(t.type)) === -1) {
                    errors.push('<b>' + t.name + '</b>: unknown type &ldquo;' + t.type + '&rdquo; (allowed: ' + VALID_TYPES.join(', ') + ')');
                }
                // C — duplicate name vs live tasks (warning, still importable)
                if (liveNames.indexOf(t.name) !== -1) {
                    warnings.push('<b>' + t.name + '</b> already in live tasks — will overwrite');
                }
            });

            if (errors.length) {
                preview.className   = 'import-preview err';
                preview.style.display = 'block';
                preview.innerHTML   = '<i class="fas fa-ban"></i> <b>Import blocked — fix '
                    + errors.length + ' error(s):</b><br>' + errors.join('<br>');
                btn.style.display   = 'none';
                return;
            }
            // ── End validation ────────────────────────────────────────────

            _importTasks = tasks;
            var MAX  = 5;  // MAX_DYNAMIC_TASKS firmware limit
            var warn = tasks.length > MAX
                ? ' &nbsp;<b style="color:var(--danger)">&#9888; Exceeds firmware limit of ' + MAX + '</b>'
                : '';
            var warnHtml = warnings.length
                ? '<br><span style="color:var(--warning)">&#9888; ' + warnings.join(' &nbsp;&middot;&nbsp; ') + '</span>'
                : '';
            var rows = tasks.map(function(t) {
                return '<span style="opacity:0.65">' + t.name + ' &middot; ' + t.type + ' pin&nbsp;' + t.pin + ' @' + t.interval + 's</span>';
            }).join('<br>');
            preview.className   = 'import-preview ok';
            preview.style.display = 'block';
            preview.innerHTML   = '<i class="fas fa-check-circle"></i> Found <b>' + tasks.length + ' task(s)</b>' + warn + warnHtml + '<br>' + rows;
            counter.innerText   = tasks.length;
            btn.style.display   = 'flex';
        }

        async function importTasks() {
            if (!_importTasks.length) return;
            var ok = 0, fail = 0;
            for (var i = 0; i < _importTasks.length; i++) {
                var t = _importTasks[i];
                try {
                    var res = await fetch('/api/schedule/add', { method: 'POST', body: new URLSearchParams(t) });
                    if (res.ok) ok++; else fail++;
                } catch(e) { fail++; }
            }
            var msg = ok + ' task(s) imported';
            if (fail) msg += ', ' + fail + ' failed';
            showToast(msg, fail ? 'exclamation-triangle' : 'file-import', fail ? 'var(--danger)' : '');
            _importTasks = [];
            document.getElementById('importPreview').style.display = 'none';
            document.getElementById('importBtn').style.display     = 'none';
            document.getElementById('importBox').value             = '';
            loadTasks();
        }
        // ── End Bulk Import ──────────────────────────────────────────

        // Auto-refresh live indicators every 2 seconds
        setInterval(loadTasks, 2000);
        loadTasks();
    </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void WiFiManager::serveApiSchedule() {
  JsonDocument doc;
  ScheduleManager::getInstance().getTaskJson(doc);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void WiFiManager::serveApiScheduleAdd() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "Missing name");
    return;
  }

  String name = server.arg("name");
  String type = server.hasArg("type") ? server.arg("type") : "TOGGLE";
  String pin = server.hasArg("pin") ? server.arg("pin") : "RELAY1";
  unsigned long interval =
      server.hasArg("interval") ? server.arg("interval").toInt() : 60;
  unsigned long duration =
      server.hasArg("duration") ? server.arg("duration").toInt() : 0;
  int value = server.hasArg("value") ? server.arg("value").toInt() : 0;
  bool enabled = server.hasArg("enabled") ? (server.arg("enabled") == "true" ||
                                             server.arg("enabled") == "1")
                                          : true;

  int triggerPin = server.hasArg("triggerPin")
                       ? CommandManager::getInstance().getPinFromName(
                             server.arg("triggerPin"))
                       : -1;
  int triggerMode =
      server.hasArg("triggerMode") ? server.arg("triggerMode").toInt() : 0;
  int threshold =
      server.hasArg("threshold") ? server.arg("threshold").toInt() : 0;
  bool thresholdGreater = server.hasArg("thresholdGreater")
                              ? (server.arg("thresholdGreater") == "true" ||
                                 server.arg("thresholdGreater") == "1")
                              : true;

  bool ok = ScheduleManager::getInstance().addDynamicTask(
      name, type, pin, interval, duration, "WEB", enabled, value, triggerPin,
      triggerMode, threshold, thresholdGreater);

  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Error");
}

void WiFiManager::serveApiScheduleRemove() {
  if (!server.hasArg("name")) {
    server.send(400, "text/plain", "Missing name");
    return;
  }
  bool ok =
      ScheduleManager::getInstance().removeDynamicTask(server.arg("name"));
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Error");
}

void WiFiManager::serveApiScheduleClear() {
  ScheduleManager::getInstance().clearDynamicTasks();
  server.send(200, "text/plain", "OK");
}

void WiFiManager::serveApiScheduleSave() {
  ScheduleManager::getInstance().saveDynamicTasks();
  server.send(200, "text/plain", "OK");
}

void WiFiManager::serveIntegrationSave() {
  DataManager &data = DataManager::getInstance();

  if (server.hasArg("mqtt_en")) {
    bool en = server.arg("mqtt_en") == "1";
    String srv = server.hasArg("mqtt_srv") ? server.arg("mqtt_srv") : "";
    int port =
        server.hasArg("mqtt_port") ? server.arg("mqtt_port").toInt() : 1883;
    String user = server.hasArg("mqtt_user") ? server.arg("mqtt_user") : "";
    String pass = server.hasArg("mqtt_pass") ? server.arg("mqtt_pass") : "";

    data.SetMqtt(en, srv, port, user, pass);
  }

  data.SaveSettings();

  server.sendHeader("Location", "/integration?saved=1");
  server.send(303);

  // Reboot to apply changes cleanly like in serveConfigSave
  delay(1000);
  ESP.restart();
}

void WiFiManager::serveApiPinName() {
  if (server.hasArg("pin") && server.hasArg("name")) {
    String pin = server.arg("pin");
    String name = server.arg("name");
    DataManager::getInstance().SetPinName(pin, name);
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing args");
  }
}

void WiFiManager::serveApiConfig() {
  String json = DataManager::getInstance().ExportConfig();
  server.send(200, "application/json", json);
}

void WiFiManager::serveApiConfigApply() {
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    if (DataManager::getInstance().ImportConfig(body)) {
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain",
                  "ERR: Invalid config or hardware mismatch");
    }
  } else {
    server.send(400, "text/plain", "ERR: Missing body");
  }
}

void WiFiManager::serveApiFileList() {
  String json = "{\"files\":[";
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  bool first = true;
  while (file) {
    if (!first)
      json += ",";
    json += "{\"name\":\"" + String(file.name()) +
            "\",\"size\":" + String(file.size()) + "}";
    first = false;
    file = root.openNextFile();
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void WiFiManager::serveApiFileRead() {
  if (server.hasArg("path")) {
    String path = server.arg("path");
    if (!path.startsWith("/"))
      path = "/" + path;
    if (LittleFS.exists(path)) {
      File file = LittleFS.open(path, "r");
      server.streamFile(file, "text/plain");
      file.close();
    } else {
      server.send(404, "text/plain", "Not found");
    }
  } else {
    server.send(400, "text/plain", "Missing path");
  }
}
void WiFiManager::serveHardware() {
  String html = R"rawhtml(<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Hardware Visualizer</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',sans-serif;background:#050510;color:#e0e0e0;overflow-x:hidden}
.nav{background:#111122;padding:12px;border-bottom:1px solid #2a2a4a;display:flex;justify-content:center;gap:15px;position:sticky;top:0;z-index:100}
.nav a{color:#888;text-decoration:none;font-size:0.85em;padding:4px 8px;border-radius:4px}
.nav a:hover, .nav a.active{color:#00d4ff;background:#1a1a3a}
.container{padding:20px;display:flex;flex-direction:column;align-items:center;max-width:800px;margin:0 auto}
.board-container{position:relative;background:#1a1a2e;border:1px solid #2a2a4a;border-radius:15px;padding:40px;margin-top:20px;box-shadow:0 10px 30px rgba(0,0,0,0.5)}
.pin-label{position:absolute;font-size:10px;color:#aaa;pointer-events:none;white-space:nowrap}
.pin-circle{cursor:help;transition:all 0.2s}
.pin-circle:hover{filter:brightness(1.5);r:6}
.info-panel{margin-top:20px;width:100%;background:#111122;border:1px solid #2a2a4a;border-radius:10px;padding:15px;position:relative}
.dbg-id{position:absolute;right:8px;top:8px;font-size:9px;color:#555;opacity:0.6;pointer-events:none;font-family:monospace;letter-spacing:0.5px;z-index:10}
.info-panel h2{font-size:1.1em;color:#00d4ff;margin-bottom:10px}
.test-panel{margin-top:20px;width:100%;background:#111122;border:1px solid #2a2a4a;border-radius:10px;padding:15px;display:grid;grid-template-columns:1fr 1fr;gap:10px}
.test-panel h2{grid-column:1/-1;font-size:1.1em;color:#00ff88;margin-bottom:5px}
.test-btn{padding:10px;background:#1a1a3a;border:1px solid #336;border-radius:8px;color:#fff;cursor:pointer;font-size:0.85em;display:flex;align-items:center;justify-content:center;gap:8px;transition:0.2s}
.test-btn:hover{background:#00d4ff22;border-color:#00d4ff}
.test-btn i{font-size:1.1em;color:#00d4ff}
.selector{margin-bottom:20px}
.selector select{background:#111122;color:#fff;border:1px solid #2a2a4a;padding:8px 12px;border-radius:6px;outline:none}
</style>
<link rel='stylesheet' href='https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.0.0/css/all.min.css'>
</head><body>
<div class='nav' style='position:relative'>
  <span class='dbg-id' style='top:4px;right:20px'>HW-NAV</span>
  <a href='/'>&#x1F4E1; Dash</a>
  <a href='/config'>&#x2699; Config</a>
  <a href='/hardware' class='active'>&#x1F527; Hardware</a>
</div>
<div class='container'>
  <div class='selector' style='position:relative'>
    <span class='dbg-id' style='right:-20px'>HW-SEL</span>
    <select id='boardSelect' onchange='loadBoard(this.value)'>
      <option value='heltec_v3'>Heltec WiFi LoRa 32 V3 (Local)</option>
      <option value='xiao_samd21'>Seeeduino Xiao (Satellite)</option>
    </select>
  </div>
  <div id='boardArea' class='board-container'><span class='dbg-id'>HW-VIEW</span></div>
  <div class='info-panel' id='infoPanel' style='position:relative'>
    <span class='dbg-id'>HW-INFO</span>
    <h2>Board Info</h2>
    <div id='pinInfo'>Select a pin to see details</div>
  </div>

  <div class='test-panel' style='position:relative'>
    <span class='dbg-id'>HW-TEST-SUITE</span>
    <h2>RF Diagnostics & Field Testing</h2>
    <button class='test-btn' onclick='runCmd("RTEST")'><i class='fas fa-wave-square'></i> Range Comparison</button>
    <button class='test-btn' onclick='runCmd("ALL BINCMD 6 1")'><i class='fas fa-satellite-dish'></i> Reliable Ping (ALL)</button>
    <button class='test-btn' onclick='runCmd("ALL PMISER AUTO")'><i class='fas fa-bolt'></i> Power Cycle (ALL)</button>
    <button class='test-btn' onclick='location.href="/"'><i class='fas fa-chart-line'></i> View Metrics</button>
  </div>
</div>
<script>
var currentBoard = null;
var liveData = {};

function runCmd(c){
  if(confirm('Run command: '+c+'?')){
    fetch('/api/cmd',{method:'POST',body:new URLSearchParams({'cmd':c})});
  }
}

function up(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    liveData = d;
    updatePins();
  });
}

function loadBoard(id){
  fetch('/api/files/read?path=/boards/'+id+'.json').then(r=>r.json()).then(data=>{
    currentBoard = data;
    renderBoard();
  });
}

function renderBoard(){
  if(!currentBoard) return;
  var b = currentBoard;
  var html = '<svg width="'+b.width+'" height="'+b.height+'" viewBox="0 0 '+b.width+' '+b.height+'">';
  // Board outline
  html += '<rect x="30" y="20" width="'+(b.width-60)+'" height="'+(b.height-40)+'" rx="10" fill="#222" stroke="#444" stroke-width="2"/>';
  // Component markers (OLED, Buttons, etc)
  if(b.id=='heltec_v3'){
    html += '<rect x="45" y="50" width="90" height="40" fill="#000" rx="2"/>'; // OLED
    html += '<rect x="45" y="100" width="10" height="10" fill="#333"/>'; // Boot btn
  }

  // Pins
  b.pins.forEach(function(p){
    var color = '#444';
    if(p.t=='GND') color='#555';
    if(p.t=='PWR') color='#ff4444';
    if(p.t=='ADC') color='#ffcc00';
    if(p.t=='SDA'||p.t=='SCL') color='#00d4ff';

    html += '<circle class="pin-circle" id="p'+p.p+'" cx="'+p.x+'" cy="'+p.y+'" r="4" fill="'+color+'" onmouseover="showPin(\''+p.n+'\','+p.p+',\''+p.t+'\')"/>';
    // Label
    var lx = p.x < b.width/2 ? p.x + 10 : p.x - 10;
    var ta = p.x < b.width/2 ? 'start' : 'end';
    html += '<text x="'+lx+'" y="'+(p.y+3)+'" font-size="8" fill="#888" text-anchor="'+ta+'">'+p.n+'</text>';
  });

  html += '</svg>';
  document.getElementById('boardArea').innerHTML = html;
  updatePins();
}

function showPin(name, num, type){
  var h = '<strong>Pin:</strong> '+name+'<br>';
  h += '<strong>GPIO:</strong> '+(num>=0?num:'N/A')+'<br>';
  h += '<strong>Type:</strong> '+type+'<br>';
  if(num >= 0 && liveData.pins && liveData.pins[num]){
    var ps = liveData.pins[num];
    h += '<strong>State:</strong> <span style="color:'+(ps.val?'#00ff88':'#ff4444')+'">'+(ps.val?'HIGH':'LOW')+'</span><br>';
    if(ps.name) h += '<strong>User Name:</strong> '+ps.name+'<br>';
  }
  document.getElementById('pinInfo').innerHTML = h;
}

function updatePins(){
  if(!currentBoard || !liveData.pins) return;
  currentBoard.pins.forEach(function(p){
    if(p.p < 0) return;
    var el = document.getElementById('p'+p.p);
    if(el && liveData.pins[p.p]){
      var val = liveData.pins[p.p].val;
      el.setAttribute('fill', val ? '#00ff88' : '#ff4444');
      el.setAttribute('stroke', val ? '#00ff8844' : '#ff444444');
      el.setAttribute('stroke-width', '4');
    }
  });
}

loadBoard('heltec_v3');
setInterval(up, 1000);
up();
</script></body></html>)rawhtml";
  server.send(200, "text/html", html);
}

// ── Transport: negotiate()
// ──────────────────────────────────────────────────── Called once from
// main.cpp setup() after WiFiManager::init(). Spends up to windowMs probing the
// preferred transport and locks currentLink.
LinkPreference WiFiManager::negotiate(uint32_t windowMs) {
  DataManager &data = DataManager::getInstance();
  LinkPreference pref = data.preferredLink;

  // Fixed preference that doesn't require WiFi — skip probe entirely
  if (pref == LinkPreference::LINK_BLE || pref == LinkPreference::LINK_LORA) {
    data.currentLink = pref;
    Serial.printf("TRANS: Negotiate skip — fixed pref: %s\n",
                  DataManager::linkName(pref));
    return pref;
  }

  // No SSID configured — nothing to probe
  if (data.wifiSsid.length() == 0) {
    data.currentLink = LinkPreference::LINK_LORA;
    Serial.println("TRANS: Negotiate → LORA (no SSID configured)");
    return LinkPreference::LINK_LORA;
  }

  Serial.printf("BOOT: Transport negotiation window %lums (pref=%s)...\n",
                windowMs, DataManager::linkName(pref));
  ProbeResult r = probeWifi(windowMs);

  LinkPreference locked;
  switch (r) {
  case ProbeResult::PROBE_OK_MQTT:
    locked = LinkPreference::LINK_WIFI_MQTT;
    break;
  case ProbeResult::PROBE_OK_HTTP:
    locked = LinkPreference::LINK_WIFI_HTTP;
    break;
  default:
    locked = LinkPreference::LINK_LORA;
    break;
  }

  // AUTO: use whatever we found
  // Specific pref but missed it → fall to LORA, probe backoff will recover
  if (pref != LinkPreference::LINK_AUTO &&
      locked == LinkPreference::LINK_LORA && r != ProbeResult::PROBE_OK_MQTT &&
      r != ProbeResult::PROBE_OK_HTTP) {
    Serial.printf("TRANS: Preferred %s unreachable — holding LORA\n",
                  DataManager::linkName(pref));
  }

  data.currentLink = locked;
  Serial.printf("BOOT: Transport locked → %s\n", DataManager::linkName(locked));
  return locked;
}

// ── Transport: probeWifi()
// ──────────────────────────────────────────────────── Lightweight WiFi
// reachability test. Safe to call on battery (<=5s, ~130mA). Hook: replace
// OK_HTTP return with PROBE_OK_MQTT when MQTTManager exposes probe().
ProbeResult WiFiManager::probeWifi(uint32_t timeoutMs) {
  DataManager &data = DataManager::getInstance();
  data.lastProbeAtMs = millis();

  if (data.wifiSsid.length() == 0) {
    data.lastProbeResult = "NO_SSID";
    return ProbeResult::PROBE_NO_AP;
  }

  bool alreadyConnected = (WiFi.status() == WL_CONNECTED);
  if (!alreadyConnected) {
    WiFi.mode(WIFI_STA);
    // Apply static IP if configured — mirrors handle()'s WiFi setup so the
    // probe lands on the correct address rather than a random DHCP lease.
    if (data.staticIp.length() > 0) {
      IPAddress ip, gw, sn;
      if (ip.fromString(data.staticIp)) {
        if (!gw.fromString(data.gateway)) {
          gw = ip;
          gw[3] = 1;
        }
        if (!sn.fromString(data.subnet)) {
          sn = IPAddress(255, 255, 255, 0);
        }
        WiFi.config(ip, gw, sn);
        Serial.printf("TRANS: Probe using static IP %s\n",
                      data.staticIp.c_str());
      }
    }
    WiFi.begin(data.wifiSsid.c_str(), data.wifiPass.c_str());
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t) < timeoutMs) {
      delay(50);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (!alreadyConnected)
      WiFi.disconnect(true);
    Serial.println("TRANS: Probe → NO_AP");
    data.lastProbeResult = "NO_AP";
    return ProbeResult::PROBE_NO_AP;
  }

  // WiFi OK — check MQTT if enabled
  // TODO: replace with MQTTManager::getInstance().probe() when available
  if (data.mqttEnabled && data.mqttServer.length() > 0) {
    Serial.printf("TRANS: Probe → OK_MQTT candidate (IP: %s)\n",
                  WiFi.localIP().toString().c_str());
    data.lastProbeResult = "OK_MQTT";
    return ProbeResult::PROBE_OK_MQTT;
  }

  Serial.printf("TRANS: Probe → OK_HTTP (IP: %s)\n",
                WiFi.localIP().toString().c_str());
  data.lastProbeResult = "OK_HTTP";
  return ProbeResult::PROBE_OK_HTTP;
}

// ── Transport: checkProbeBackoff() ───────────────────────────────────────────
// Called every loop iteration from handle(). Fires a probe when the backoff
// interval has elapsed and device is holding LORA.
void WiFiManager::checkProbeBackoff() {
  DataManager &data = DataManager::getInstance();
  if (data.currentLink != LinkPreference::LINK_LORA)
    return;
  if (data.wifiSsid.length() == 0)
    return;
  if (data.lastProbeAtMs == 0) {
    data.lastProbeAtMs = millis(); // Seed timer on first call
    return;
  }
  if ((millis() - data.lastProbeAtMs) < data.probeBackoffMs)
    return;

  Serial.printf("TRANS: Probe backoff fired (interval=%lus, fails=%d)\n",
                data.probeBackoffMs / 1000, data.probeFailCount);
  ProbeResult r = probeWifi(PROBE_TIMEOUT_MS);

  if (r == ProbeResult::PROBE_OK_MQTT || r == ProbeResult::PROBE_OK_HTTP) {
    onLinkUpgrade(LinkPreference::LINK_LORA,
                  (r == ProbeResult::PROBE_OK_MQTT)
                      ? LinkPreference::LINK_WIFI_MQTT
                      : LinkPreference::LINK_WIFI_HTTP);
    data.ResetProbeState();
  } else {
    // Exponential backoff — double interval, cap at max
    uint32_t next =
        min(data.probeBackoffMs * 2U, (uint32_t)PROBE_BACKOFF_MAX_MS);
    uint8_t fails = data.probeFailCount + 1;
    data.SetProbeState(next, fails);
    Serial.printf("TRANS: Probe failed — next in %lus (fail #%d)\n",
                  next / 1000, fails);
  }
}

// ── Transport: onLinkDowngrade() / onLinkUpgrade() ───────────────────────────
void WiFiManager::onPowerStateChange(PowerMode mode) {
  if (mode == PowerMode::CRITICAL) {
    // Force immediate downgrade to LORA mesh mode to save power
    /*
    onLinkDowngrade(DataManager::getInstance().currentLink,
                    LinkPreference::LINK_LORA);
    */

    /*
    // WiFi is handled proactively in handle() next tick, but we can nudge it
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    isConnected = false;
    serverStarted = false;
    */
  } else if (mode == PowerMode::CONSERVE) {
    /*
    // Enable WiFi Modem Sleep to reduce current draw by ~50%
    WiFi.setSleep(true);
    modemSleepEnabled = true;
    Serial.println("POWER: Miser enabling WiFi Modem-Sleep (CONSERVE)");
    */
  } else {
    /*
    // NORMAL: Full performance
    WiFi.setSleep(false);
    modemSleepEnabled = false;
    */
  }
}

void WiFiManager::onLinkDowngrade(LinkPreference from, LinkPreference to) {
  DataManager &data = DataManager::getInstance();
  data.currentLink = to;
  data.probeBackoffMs = PROBE_BACKOFF_MIN_MS;
  data.probeFailCount = 0;

  // Proactive Fallback: boost BLE advertising to make the device easier to find
  // when WiFi is lost.
  BLEManager::getInstance().boostAdvertising(true);

  Serial.printf("TRANS: Downgrade %s → %s\n", DataManager::linkName(from),
                DataManager::linkName(to));
  data.LogMessage("TRANS", 0,
                  "Link downgrade: " + String(DataManager::linkName(from)) +
                      " → " + String(DataManager::linkName(to)));
}

void WiFiManager::onLinkUpgrade(LinkPreference from, LinkPreference to) {
  DataManager &data = DataManager::getInstance();
  data.currentLink = to;
  Serial.printf("TRANS: Upgrade %s → %s\n", DataManager::linkName(from),
                DataManager::linkName(to));
  data.LogMessage("TRANS", 0,
                  "Link upgrade: " + String(DataManager::linkName(from)) +
                      " → " + String(DataManager::linkName(to)));
  // Re-start HTTP server if not already running
  if (!serverStarted && WiFi.status() == WL_CONNECTED) {
    startServer();
    ArduinoOTA.begin();
    serverStarted = true;
    lastApiHit = millis();
  }
}
