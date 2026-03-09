# LoRaLink Corporate Website Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build a placeholder corporate website for LoRaLink with a marketing landing page, contact form, and live MQTT dashboard for customer devices.

**Architecture:** FastAPI serves static marketing pages and a `/api/contact` endpoint (SQLite storage). The MQTT dashboard connects the browser directly to EMQX via mqtt.js over WebSocket — no backend MQTT polling. Nginx reverse-proxies the whole stack with TLS.

**Tech Stack:** Python FastAPI + uvicorn, SQLite (aiosqlite), mqtt.js (CDN), Nginx, EMQX Community, Certbot, pytest + httpx

**Design doc:** `docs/plans/2026-03-04-corporate-website-design.md`

**Brand tokens (from `docs/css/docs.css`):**
- `--bg-main: #080e1f` | `--accent: #00d4ff` | `--gold: #c9a84c`
- Logo: `docs/media/logo.png`

---

## Task 1: Project Scaffold

**Files:**
- Create: `tools/website/server.py`
- Create: `tools/website/requirements.txt`
- Create: `tools/website/static/css/site.css`
- Create: `tools/website/static/js/dashboard.js`
- Create: `tools/website/db/.gitkeep`
- Create: `tools/website/tests/__init__.py`
- Create: `tools/website/tests/test_api.py`
- Create: `tools/website/nginx/loralink.conf`

**Step 1: Create requirements.txt**

```text
fastapi>=0.110.0
uvicorn[standard]>=0.29.0
aiosqlite>=0.20.0
email-validator>=2.1.0
httpx>=0.27.0
pytest>=8.0.0
pytest-asyncio>=0.23.0
```

**Step 2: Create server.py skeleton**

```python
#!/usr/bin/env python3
"""
server.py — LoRaLink Corporate Website Backend

Usage:
    python tools/website/server.py
    # Then open: http://localhost:8001
"""
from __future__ import annotations

import os
from pathlib import Path
from typing import Optional

import aiosqlite
import uvicorn
from fastapi import FastAPI
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, EmailStr

BASE = Path(__file__).parent
DB_PATH = BASE / "db" / "contacts.db"
STATIC = BASE / "static"

app = FastAPI(title="LoRaLink Website")
app.mount("/static", StaticFiles(directory=STATIC), name="static")


@app.on_event("startup")
async def init_db():
    async with aiosqlite.connect(DB_PATH) as db:
        await db.execute("""
            CREATE TABLE IF NOT EXISTS contacts (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL,
                email TEXT NOT NULL,
                company TEXT,
                message TEXT,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )
        """)
        await db.commit()


@app.get("/health")
async def health():
    return {"status": "ok"}


@app.get("/api/config")
async def client_config():
    return {"mqttBrokerUrl": os.environ.get("MQTT_BROKER_URL", "ws://localhost:8083/mqtt")}


class ContactForm(BaseModel):
    name: str
    email: EmailStr
    company: Optional[str] = None
    message: Optional[str] = None


@app.post("/api/contact")
async def submit_contact(form: ContactForm):
    async with aiosqlite.connect(DB_PATH) as db:
        await db.execute(
            "INSERT INTO contacts (name, email, company, message) VALUES (?, ?, ?, ?)",
            (form.name, form.email, form.company, form.message),
        )
        await db.commit()
    return {"ok": True}


@app.get("/api/contacts")
async def list_contacts():
    async with aiosqlite.connect(DB_PATH) as db:
        db.row_factory = aiosqlite.Row
        async with db.execute(
            "SELECT id, name, email, company, message, created_at FROM contacts ORDER BY created_at DESC"
        ) as cur:
            rows = await cur.fetchall()
    return [dict(r) for r in rows]


@app.get("/")
async def index():
    return FileResponse(STATIC / "index.html")


@app.get("/features")
async def features():
    return FileResponse(STATIC / "features.html")


@app.get("/contact")
async def contact_page():
    return FileResponse(STATIC / "contact.html")


@app.get("/dashboard")
async def dashboard():
    return FileResponse(STATIC / "dashboard.html")


@app.get("/docs")
async def docs_redirect():
    return FileResponse(STATIC / "docs-redirect.html")


if __name__ == "__main__":
    uvicorn.run("server:app", host="0.0.0.0", port=8001, reload=True)
```

**Step 3: Write tests in `tests/test_api.py`**

```python
import pytest
import sys
from pathlib import Path
from httpx import AsyncClient, ASGITransport

sys.path.insert(0, str(Path(__file__).parent.parent))
from server import app


@pytest.mark.asyncio
async def test_health():
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
        r = await c.get("/health")
    assert r.status_code == 200
    assert r.json() == {"status": "ok"}


@pytest.mark.asyncio
async def test_config_endpoint():
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
        r = await c.get("/api/config")
    assert r.status_code == 200
    assert "mqttBrokerUrl" in r.json()


@pytest.mark.asyncio
async def test_contact_submit():
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
        r = await c.post("/api/contact", json={
            "name": "Ada Lovelace",
            "email": "ada@example.com",
            "company": "Acme",
            "message": "Interested in LoRaLink",
        })
    assert r.status_code == 200
    assert r.json()["ok"] is True


@pytest.mark.asyncio
async def test_contact_missing_email():
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
        r = await c.post("/api/contact", json={"name": "No email"})
    assert r.status_code == 422


@pytest.mark.asyncio
async def test_contacts_list():
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:
        r = await c.get("/api/contacts")
    assert r.status_code == 200
    assert isinstance(r.json(), list)
```

**Step 4: Install deps and run tests**

```bash
cd tools/website
pip install -r requirements.txt
pytest tests/ -v
```
Expected: all PASS

**Step 5: Commit**

```bash
git add tools/website/
git commit -m "feat(website): scaffold FastAPI app, API endpoints, tests"
```

---

## Task 2: Shared CSS + Static Pages

**Files:**
- Create: `tools/website/static/css/site.css`
- Create: `tools/website/static/index.html`
- Create: `tools/website/static/features.html`
- Create: `tools/website/static/contact.html`
- Create: `tools/website/static/docs-redirect.html`

**Step 1: Copy logo asset**

```bash
mkdir -p tools/website/static/media
cp docs/media/logo.png tools/website/static/media/logo.png
```

**Step 2: Create `static/css/site.css`**

Copy the `:root` token block from `docs/css/docs.css` (lines 8-40), then append:

```css
body { background:var(--bg-main);color:var(--text);font-family:'Inter',system-ui,sans-serif;margin:0; }

nav { background:var(--bg-hdr);border-bottom:1px solid var(--border);padding:0 2rem;height:var(--header-h);display:flex;align-items:center;gap:2rem; }
nav a { color:var(--text-dim);text-decoration:none;font-size:.9rem; }
nav a:hover { color:var(--accent); }

.hero { text-align:center;padding:6rem 2rem; }
.hero h1 { font-size:3rem;color:var(--text-bright);margin-bottom:1rem; }
.hero p { color:var(--text-dim);font-size:1.2rem;max-width:600px;margin:0 auto 2rem; }

.btn { background:var(--accent);color:#000;padding:.75rem 2rem;border-radius:var(--radius);text-decoration:none;font-weight:600; }

.card { background:var(--bg-card);border:1px solid var(--border);border-radius:var(--radius);padding:1.5rem; }

.feature-grid { display:grid;grid-template-columns:repeat(auto-fill,minmax(240px,1fr));gap:1.5rem;padding:2rem; }

input,textarea { background:var(--bg-main);border:1px solid var(--border);color:var(--text);padding:.6rem .9rem;border-radius:6px;width:100%;box-sizing:border-box; }
label { color:var(--text-dim);font-size:.85rem;display:block;margin-bottom:.3rem; }
.form-group { margin-bottom:1rem; }
```

**Step 3: Create `static/index.html`**

Hero section + 3 feature cards. All content is placeholder text. Nav links to all 5 routes. See design doc for card topics.

**Step 4: Create `static/features.html`**

6-card feature grid (LoRa Mesh, ESP-NOW, MQTT, Scheduler, BLE, OTA). All placeholder text.

**Step 5: Create `static/contact.html`**

Form with fields: name, email, company (optional), message. Submit via `fetch('/api/contact', ...)`.
Show success/error message in a `<p id="msg">` element using `textContent` (not innerHTML):

```js
document.getElementById('msg').textContent = r.ok ? 'Submitted! We will be in touch.' : 'Error — please try again.';
```

**Step 6: Create `static/docs-redirect.html`**

Simple page with links to `../../docs/index.html` and the other docs pages.

**Step 7: Commit**

```bash
git add tools/website/static/
git commit -m "feat(website): placeholder marketing pages and shared CSS"
```

---

## Task 3: MQTT Dashboard

**Files:**
- Create: `tools/website/static/dashboard.html`
- Create: `tools/website/static/js/dashboard.js`

**Step 1: Create `static/dashboard.html`**

Three layout sections: broadcast bar, client card grid (`id="client-grid"`), contact entries table. Load config then inject `dashboard.js` script tag dynamically:

```html
<script src="https://unpkg.com/mqtt@5/dist/mqtt.min.js"></script>
<script>
  fetch('/api/config')
    .then(r => r.json())
    .then(cfg => { window.MQTT_BROKER_URL = cfg.mqttBrokerUrl; })
    .catch(() => {})
    .finally(() => {
      const s = document.createElement('script');
      s.src = '/static/js/dashboard.js';
      document.body.appendChild(s);
    });
</script>
```

**Step 2: Create `static/js/dashboard.js`**

Key security rule: MQTT client IDs come from an external broker and must be treated as untrusted. Use a `sanitize()` helper for all untrusted strings inserted into the DOM, and always set values via `textContent` (not innerHTML) where possible. Build card elements via `document.createElement` to avoid injection.

```javascript
// dashboard.js — LoRaLink MQTT Dashboard

const BROKER_URL = window.MQTT_BROKER_URL || 'ws://localhost:8083/mqtt';
const TOPIC_PREFIX = 'loralink';
const clients = {};

// Escapes untrusted strings for safe use as text node content
function sanitize(str) {
  const d = document.createElement('div');
  d.textContent = str;
  return d.textContent;  // returns the safe text (no HTML)
}

// Builds a client card using DOM methods (no innerHTML with untrusted data)
function buildClientCard(clientId) {
  const safeId = sanitize(clientId);

  const card = document.createElement('div');
  card.className = 'card client-card';
  card.id = 'card-' + safeId;

  const header = document.createElement('div');
  header.style.cssText = 'display:flex;align-items:center;margin-bottom:.75rem';

  const dot = document.createElement('span');
  dot.className = 'status-dot';
  dot.id = 'dot-' + safeId;

  const name = document.createElement('strong');
  name.style.color = 'var(--text-bright)';
  name.textContent = safeId;

  header.appendChild(dot);
  header.appendChild(name);

  const seenLine = document.createElement('div');
  seenLine.style.cssText = 'color:var(--text-dim);font-size:.8rem;margin-bottom:.5rem';
  seenLine.textContent = 'Last seen: ';
  const seenSpan = document.createElement('span');
  seenSpan.id = 'seen-' + safeId;
  seenSpan.textContent = 'now';
  seenLine.appendChild(seenSpan);

  const msgBox = document.createElement('div');
  msgBox.id = 'msg-' + safeId;
  msgBox.style.cssText = 'background:var(--bg-main);border-radius:6px;padding:.5rem;font-family:monospace;font-size:.8rem;color:var(--accent);min-height:2rem';
  msgBox.textContent = '—';

  const cmdRow = document.createElement('div');
  cmdRow.className = 'cmd-input';

  const cmdInput = document.createElement('input');
  cmdInput.id = 'cmd-' + safeId;
  cmdInput.placeholder = 'Command…';

  const cmdBtn = document.createElement('button');
  cmdBtn.textContent = 'Send';
  cmdBtn.addEventListener('click', () => sendCmd(clientId));

  cmdRow.appendChild(cmdInput);
  cmdRow.appendChild(cmdBtn);

  card.appendChild(header);
  card.appendChild(seenLine);
  card.appendChild(msgBox);
  card.appendChild(cmdRow);
  return card;
}

// MQTT connection
const mqttClient = mqtt.connect(BROKER_URL, {
  clientId: 'dashboard-' + Math.random().toString(16).slice(2, 8),
  clean: true,
  reconnectPeriod: 3000,
});

mqttClient.on('connect', () => {
  const el = document.getElementById('connection-status');
  el.textContent = '● Broker connected';
  el.style.color = 'var(--ok)';
  mqttClient.subscribe(TOPIC_PREFIX + '/+/telemetry');
  mqttClient.subscribe(TOPIC_PREFIX + '/+/status');
});

mqttClient.on('error', (err) => {
  const el = document.getElementById('connection-status');
  el.textContent = '● ' + sanitize(err.message);
  el.style.color = 'var(--err)';
});

mqttClient.on('message', (topic, payload) => {
  const parts = topic.split('/');
  if (parts.length < 3) return;
  const clientId = parts[1];
  const msg = payload.toString();

  const grid = document.getElementById('client-grid');
  if (!clients[clientId]) {
    if (Object.keys(clients).length === 0) grid.textContent = '';
    const card = buildClientCard(clientId);
    grid.appendChild(card);
    clients[clientId] = { lastSeen: 'now', element: card };
  }

  clients[clientId].lastSeen = new Date().toLocaleTimeString();
  const msgEl = document.getElementById('msg-' + sanitize(clientId));
  const seenEl = document.getElementById('seen-' + sanitize(clientId));
  if (msgEl) msgEl.textContent = msg;
  if (seenEl) seenEl.textContent = clients[clientId].lastSeen;
});

function sendCmd(clientId) {
  const input = document.getElementById('cmd-' + sanitize(clientId));
  if (!input) return;
  const msg = input.value.trim();
  if (!msg) return;
  mqttClient.publish(TOPIC_PREFIX + '/' + clientId + '/cmd', msg);
  input.value = '';
}

function broadcast() {
  const input = document.getElementById('broadcast-msg');
  const msg = input.value.trim();
  if (!msg) return;
  Object.keys(clients).forEach(id => {
    mqttClient.publish(TOPIC_PREFIX + '/' + id + '/cmd', msg);
  });
  input.value = '';
}

// Load contact entries
async function loadContacts() {
  try {
    const r = await fetch('/api/contacts');
    const rows = await r.json();
    const tbody = document.getElementById('contacts-body');
    tbody.textContent = '';  // clear safely

    if (!rows.length) {
      const tr = document.createElement('tr');
      const td = document.createElement('td');
      td.colSpan = 5;
      td.style.color = 'var(--text-dim)';
      td.textContent = 'No submissions yet.';
      tr.appendChild(td);
      tbody.appendChild(tr);
      return;
    }

    rows.forEach(c => {
      const tr = document.createElement('tr');
      [c.name, c.email, c.company || '—', c.message || '—', c.created_at].forEach(val => {
        const td = document.createElement('td');
        td.textContent = val;
        tr.appendChild(td);
      });
      tbody.appendChild(tr);
    });
  } catch (e) {
    console.error('Failed to load contacts:', e);
  }
}

loadContacts();
```

**Step 3: Commit**

```bash
git add tools/website/static/
git commit -m "feat(website): MQTT dashboard with safe DOM construction, broadcast, contacts table"
```

---

## Task 4: Nginx Config + Deploy Guide

**Files:**
- Create: `tools/website/nginx/loralink.conf`
- Create: `tools/website/DEPLOY.md`

**Step 1: Create `nginx/loralink.conf`**

```nginx
# Deploy to: /etc/nginx/sites-available/loralink
# Enable: sudo ln -s /etc/nginx/sites-available/loralink /etc/nginx/sites-enabled/
# Replace YOUR_DOMAIN with your actual domain.

server {
    listen 80;
    server_name YOUR_DOMAIN;
    return 301 https://$host$request_uri;
}

server {
    listen 443 ssl;
    server_name YOUR_DOMAIN;

    ssl_certificate     /etc/letsencrypt/live/YOUR_DOMAIN/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/YOUR_DOMAIN/privkey.pem;

    # FastAPI (marketing site + API)
    location / {
        proxy_pass http://127.0.0.1:8001;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }

    # EMQX WebSocket — mqtt.js connects here
    location /mqtt {
        proxy_pass http://127.0.0.1:8083;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_read_timeout 86400;
    }
}
```

**Step 2: Create `DEPLOY.md`**

Document the full VPS setup sequence:
1. `apt install nginx certbot python3-certbot-nginx`
2. Install EMQX Community (from emqx.io, follow their Ubuntu guide)
3. `pip install -r requirements.txt`
4. Configure EMQX: add device credentials via EMQX dashboard at `http://YOUR_VPS:18083`
5. Copy Nginx config, replace `YOUR_DOMAIN`, enable site, test with `nginx -t`
6. `certbot --nginx -d YOUR_DOMAIN`
7. Set env and start FastAPI: `MQTT_BROKER_URL=wss://YOUR_DOMAIN/mqtt uvicorn server:app --host 0.0.0.0 --port 8001`
8. (Optional) Create systemd service for uvicorn

**Step 3: Commit**

```bash
git add tools/website/nginx/ tools/website/DEPLOY.md
git commit -m "feat(website): Nginx config + VPS deployment guide"
```

---

## Final Test Run

```bash
cd tools/website
pytest tests/ -v
```
Expected: all 5 tests PASS

**Local dev with public EMQX test broker:**
```bash
MQTT_BROKER_URL=wss://broker.emqx.io:8084/mqtt python server.py
# open http://localhost:8001
```

**Local dev with Docker EMQX:**
```bash
docker run -d --name emqx -p 1883:1883 -p 8083:8083 -p 18083:18083 emqx/emqx
MQTT_BROKER_URL=ws://localhost:8083/mqtt python server.py
```

---

## Summary

| Task | Output | Commit |
|---|---|---|
| 1 | FastAPI skeleton, all endpoints, tests | feat(website): scaffold |
| 2 | 5 placeholder HTML pages, shared CSS, logo | feat(website): marketing pages |
| 3 | MQTT dashboard (safe DOM, broadcast, contacts) | feat(website): dashboard |
| 4 | Nginx config + deploy guide | feat(website): deploy |
