// admin-config.js -- Device Config Settings, 3-state tracking, Golden Image rollback

var CONFIG_SETTINGS = [
  { key: 'DEV_NAME',  label: 'Device Name',   type: 'text',    fw: 'dev_name',  max: 15,  hint: 'Max 15 chars' },
  { key: 'WIFI_EN',   label: 'WiFi Enabled',  type: 'bool',    fw: 'wifi_en' },
  { key: 'WIFI_SSID', label: 'WiFi SSID',     type: 'text',    fw: 'wifi_ssid' },
  { key: 'WIFI_PASS', label: 'WiFi Password', type: 'password',fw: 'wifi_pass', hint: 'Blank = keep current' },
  { key: 'STATIC_IP', label: 'Static IP',     type: 'text',    fw: 'static_ip', hint: '192.168.x.x or blank for DHCP' },
  { key: 'REPEATER',  label: 'Repeater Mode', type: 'bool',    fw: 'repeater' },
  { key: 'ESPNOW',    label: 'ESP-NOW',       type: 'bool',    fw: 'espnow_en' },
  { key: 'BLE_EN',    label: 'BLE Enabled',   type: 'bool',    fw: 'ble_en' },
  { key: 'MQTT_EN',   label: 'MQTT Enabled',  type: 'bool',    fw: 'mqtt_en' },
  { key: 'MQTT_SRV',  label: 'MQTT Server',   type: 'text',    fw: 'mqtt_srv' },
  { key: 'MQTT_PRT',  label: 'MQTT Port',     type: 'number',  fw: 'mqtt_prt', min: 1, max: 65535 },
];

var _pendingConfig        = {};
var _reportedConfig       = {};
var _goldenFile           = null;
var _reconcileTimer       = null;
var CFG_PENDING_TIMEOUT_MS = 10000; // 10 s before marking a pending item as Unconfirmed

function _cfgVal(v) {
  if (v === null || v === undefined) return '--';
  if (typeof v === 'boolean') return v ? 'ON' : 'OFF';
  return String(v);
}

function _cfgStateHtml(key) {
  if (_pendingConfig[key]) {
    return '<span class="cfg-dot cfg-dot-pending"></span><span class="cfg-state-pending">Pending</span>';
  }
  var desired  = _cfgDesiredValue(key);
  var reported = _reportedConfig[key];
  if (reported === undefined) return '<span style="color:#555">--</span>';
  if (desired === null || String(desired) === String(reported !== null ? reported : '')) {
    return '<span class="cfg-dot cfg-dot-ok"></span><span class="cfg-state-ok">OK</span>';
  }
  return '<span class="cfg-dot cfg-dot-mismatch"></span><span class="cfg-state-mismatch">!</span>';
}

function _cfgDesiredValue(key) {
  var inp = document.getElementById('cfg-inp-' + key);
  if (!inp) return null;
  var s = CONFIG_SETTINGS.find(function(s) { return s.key === key; });
  if (!s) return null;
  return s.type === 'bool' ? (inp.checked ? 'true' : 'false') : inp.value;
}

function _esc(s) {
  return typeof escHtml === 'function' ? escHtml(s)
    : String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function _log(level, msg) {
  if (typeof logConsole === 'function') logConsole(level, msg);
}

async function refreshConfigSettings() {
  var tbody = document.getElementById('cfg-settings-body');
  if (!tbody) return;
  try {
    var r   = await fetch('/api/device/config');
    var cfg = await r.json();
    if (cfg.error) throw new Error(cfg.error);
    var settings = cfg.settings || {};
    CONFIG_SETTINGS.forEach(function(s) {
      _reportedConfig[s.key] = settings[s.fw] !== undefined ? settings[s.fw] : null;
    });
    renderConfigSettings();
  } catch (e) {
    _log('err', 'Config load: ' + e.message);
    tbody.innerHTML = '<tr><td colspan="4" style="color:var(--err);text-align:center;padding:12px">Device unreachable -- connect first</td></tr>';
  }
}

function renderConfigSettings() {
  var tbody = document.getElementById('cfg-settings-body');
  if (!tbody) return;
  var rows = CONFIG_SETTINGS.map(function(s) {
    var rep      = _reportedConfig[s.key];
    var repTxt   = _cfgVal(rep);
    var inputHtml;
    if (s.type === 'bool') {
      var chk = (rep === true || rep === 'true' || rep === 1) ? ' checked' : '';
      var lbl = chk ? 'Enabled' : 'Disabled';
      inputHtml = '<label style="display:flex;align-items:center;gap:6px;cursor:pointer">'
        + '<input type="checkbox" id="cfg-inp-' + s.key + '"' + chk
        + ' onchange="onCfgInputChange(\'' + s.key + '\')"'
        + ' style="accent-color:var(--accent);width:16px;height:16px">'
        + '<span style="font-size:0.8em;color:#888">' + lbl + '</span></label>';
    } else {
      var val  = (rep !== null && rep !== undefined) ? _esc(String(rep)) : '';
      var t    = s.type === 'password' ? 'password' : (s.type === 'number' ? 'number' : 'text');
      var ml   = (s.max && s.type !== 'number') ? ' maxlength="' + s.max + '"' : '';
      var mn   = s.min !== undefined ? ' min="' + s.min + '"' : '';
      var mx   = (s.max !== undefined && s.type === 'number') ? ' max="' + s.max + '"' : '';
      var ph   = s.hint ? _esc(s.hint) : '';
      inputHtml = '<input type="' + t + '" id="cfg-inp-' + s.key + '" value="' + val + '"'
        + ' class="cfg-input" placeholder="' + ph + '"' + ml + mn + mx
        + ' oninput="onCfgInputChange(\'' + s.key + '\')">';
    }
    return '<tr id="cfg-row-' + s.key + '">'
      + '<td style="color:#aaa;font-weight:500">' + s.label + '</td>'
      + '<td class="cfg-reported">' + repTxt + '</td>'
      + '<td>' + inputHtml + '</td>'
      + '<td style="text-align:center;white-space:nowrap" id="cfg-state-' + s.key + '">'
      + _cfgStateHtml(s.key) + '</td></tr>';
  });
  tbody.innerHTML = rows.join('');
}

function onCfgInputChange(key) {
  var s   = CONFIG_SETTINGS.find(function(s) { return s.key === key; });
  var inp = document.getElementById('cfg-inp-' + key);
  if (!inp || !s) return;
  if (s.type === 'bool') {
    var lbl = inp.nextElementSibling;
    if (lbl) lbl.textContent = inp.checked ? 'Enabled' : 'Disabled';
  } else {
    var rep = _reportedConfig[key];
    inp.classList.toggle('dirty', inp.value !== String(rep !== null && rep !== undefined ? rep : ''));
  }
  var el = document.getElementById('cfg-state-' + key);
  if (el) el.innerHTML = _cfgStateHtml(key);
}

async function applyConfigChanges() {
  var errEl  = document.getElementById('cfg-validation-err');
  var errors = [];
  CONFIG_SETTINGS.forEach(function(s) {
    var val = _cfgDesiredValue(s.key);
    if (val === null) return;
    if (s.type === 'number') {
      var n = parseInt(val);
      if (isNaN(n)) errors.push(s.label + ': must be a number');
      else if (s.min !== undefined && n < s.min) errors.push(s.label + ': min ' + s.min);
      else if (s.max !== undefined && n > s.max) errors.push(s.label + ': max ' + s.max);
    }
    if (s.type === 'text' && s.max && val.length > s.max) {
      errors.push(s.label + ': max ' + s.max + ' chars');
    }
  });
  if (errors.length) {
    if (errEl) { errEl.textContent = errors.join(' / '); errEl.style.display = 'block'; }
    return;
  }
  if (errEl) errEl.style.display = 'none';

  var dirty = CONFIG_SETTINGS.filter(function(s) {
    var d = _cfgDesiredValue(s.key);
    if (d === null) return false;
    var rep = _reportedConfig[s.key];
    return String(d) !== String(rep !== null && rep !== undefined ? rep : '');
  });
  if (!dirty.length) { _log('rx', 'No config changes to apply'); return; }

  _log('tx', 'Applying ' + dirty.length + ' change(s)...');
  for (var i = 0; i < dirty.length; i++) {
    var s   = dirty[i];
    var val = _cfgDesiredValue(s.key);
    _pendingConfig[s.key] = { desired: val, sentAt: Date.now() };
    var el = document.getElementById('cfg-state-' + s.key);
    if (el) el.innerHTML = _cfgStateHtml(s.key);
    if (typeof sendRaw === 'function') await sendRaw('CONFIG SET ' + s.key + ' ' + val);
    await new Promise(function(r) { setTimeout(r, 200); });
  }
  _log('rx', 'Commands sent — polling for confirmation...');
  // Poll every 2s so we catch the device response quickly
  if (_reconcileTimer) clearInterval(_reconcileTimer);
  _reconcileTimer = setInterval(async function() {
    await refreshConfigSettings();
    reconcileConfigStates();
  }, 2000);
  // Hard stop after timeout + grace period so nothing stays Pending forever
  setTimeout(function() {
    if (_reconcileTimer) { clearInterval(_reconcileTimer); _reconcileTimer = null; }
    reconcileConfigStates(); // final pass — times out any still-pending items
  }, CFG_PENDING_TIMEOUT_MS + 2000);
}

function _cfgStateTimeout() {
  return '<span class="cfg-dot cfg-dot-mismatch"></span><span class="cfg-state-mismatch">? Unconfirmed</span>';
}

function reconcileConfigStates() {
  var now = Date.now();
  Object.keys(_pendingConfig).forEach(function(key) {
    var entry   = _pendingConfig[key];
    var rep     = String(_reportedConfig[key] !== undefined ? _reportedConfig[key] : '');
    var desired = String(entry.desired);
    var el      = document.getElementById('cfg-state-' + key);
    if (rep === desired) {
      delete _pendingConfig[key];
      if (el) el.innerHTML = '<span class="cfg-dot cfg-dot-ok"></span><span class="cfg-state-ok">OK</span>';
    } else if (now - entry.sentAt > CFG_PENDING_TIMEOUT_MS) {
      // Device never confirmed — stop waiting, surface the failure clearly
      delete _pendingConfig[key];
      if (el) el.innerHTML = _cfgStateTimeout();
      _log('err', 'Timeout: ' + key + ' — no confirmation after ' + (CFG_PENDING_TIMEOUT_MS / 1000) + 's');
    } else {
      if (el) el.innerHTML = _cfgStateHtml(key); // still pending, keep spinner
    }
  });
  // Stop polling interval once all items have resolved or timed out
  if (Object.keys(_pendingConfig).length === 0 && _reconcileTimer) {
    clearInterval(_reconcileTimer);
    _reconcileTimer = null;
  }
}

async function loadGoldenImage() {
  try {
    var r    = await fetch('/api/files');
    var d    = await r.json();
    var bk   = (d.files || []).filter(function(f) {
      return f.name.startsWith('backup_') && f.name.endsWith('.json');
    });
    var nameEl = document.getElementById('golden-name');
    var btnEl  = document.getElementById('golden-restore-btn');
    if (bk.length) {
      _goldenFile = bk[bk.length - 1].name;
      if (nameEl) nameEl.textContent = _goldenFile;
      if (btnEl)  btnEl.disabled = false;
    } else {
      _goldenFile = null;
      if (nameEl) nameEl.textContent = 'No backup on file';
      if (btnEl)  btnEl.disabled = true;
    }
  } catch (_) {}
}

async function restoreGoldenImage() {
  if (!_goldenFile) return;
  if (!confirm('Restore ' + _goldenFile + '?\nThis overwrites current NVS config.')) return;
  try {
    _log('tx', 'Restoring: ' + _goldenFile);
    var fr  = await fetch('/api/files/' + encodeURIComponent(_goldenFile));
    var cfg = JSON.parse(await fr.text());
    var r   = await fetch('/api/device/config/apply', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(cfg)
    });
    if (r.ok) { _log('rx', 'Golden image applied -- rebooting...'); setTimeout(refreshConfigSettings, 5000); }
    else       { _log('err', 'Restore failed'); }
  } catch (e) { _log('err', 'Restore error: ' + e.message); }
}

function onBoardVariantChange() {
  var el = document.getElementById('board-variant');
  _log('rx', 'Board variant: ' + (el ? el.value : 'unknown'));
}
