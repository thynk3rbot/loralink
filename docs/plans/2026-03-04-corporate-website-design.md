# LoRaLink Corporate Website — Design Doc
**Date:** 2026-03-04
**Status:** Approved

## Summary

A corporate website and customer-facing MQTT dashboard for LoRaLink, self-hosted on a VPS alongside an EMQX Community broker. Placeholder content throughout — priority is ecosystem integration, not content completeness. Firmware bugs and failing features are still being resolved.

---

## Infrastructure

```
VPS (Ubuntu 22.04)
├── EMQX Community  → port 1883 (MQTT), 8083 (WebSocket), 18083 (admin)
├── Nginx           → port 80/443, reverse-proxy to FastAPI + static files
├── FastAPI app     → port 8000 (internal only)
└── Certbot         → TLS cert for domain
```

- EMQX handles broker auth: username/password per client, ACL per `loralink/<client_id>/#`
- Nginx terminates SSL, routes `/api/*` and `/mqtt` to FastAPI/EMQX, serves static marketing files
- FastAPI serves pages and handles contact form storage (SQLite)
- Separate from existing cPanel/WordPress site

---

## Marketing Site (Placeholder Phase)

All pages are static HTML/CSS/JS. Reuses `docs/media/logo.png` and color palette from `docs/css/docs.css`.

| Route | Content |
|---|---|
| `/` | Hero, product pitch, key features, CTA |
| `/features` | Feature breakdown (LoRa mesh, MQTT, scheduling, ESP-NOW) |
| `/docs` | Links to existing `docs/` HTML pages |
| `/contact` | Lead capture form → stored in SQLite via FastAPI |
| `/dashboard` | Login gate → MQTT dashboard |

All content is placeholder text at this stage.

---

## MQTT Dashboard

Accessed at `/dashboard`, protected by a single shared password (no per-client accounts in this phase).

### Multi-Client View
Card grid, one card per connected EMQX client:
- Client ID, connection status, last seen timestamp
- Last telemetry message received on `loralink/<client_id>/telemetry`
- "Send Command" input → publishes to `loralink/<client_id>/cmd`

### Broadcast Panel
Top of dashboard — type a message, publish to `loralink/+/cmd` (all clients).

### Contact Entries Panel
Admin table of contact form submissions from `/contact`.

### Data Flow
```
Browser → wss://domain/mqtt → Nginx → EMQX :8083 (MQTT over WebSocket)
```
Browser uses `mqtt.js` to connect directly to EMQX. FastAPI handles page serving and contact storage only — no MQTT polling on the backend.

---

## Tech Stack

| Component | Technology |
|---|---|
| Broker | EMQX Community Edition |
| Web backend | Python FastAPI (extends `tools/webapp/server.py` patterns) |
| Frontend | Static HTML/CSS/JS (consistent with existing webapp/docs style) |
| MQTT client (browser) | mqtt.js over WebSocket |
| DB | SQLite (contact form entries) |
| Reverse proxy | Nginx |
| TLS | Certbot (Let's Encrypt) |
| Auth | Single shared password for dashboard (Phase 1) |

---

## Out of Scope (This Phase)

- Per-client customer accounts / registration
- Billing or licensing
- Real marketing copy (all placeholder)
- Mobile responsiveness beyond basic layout
