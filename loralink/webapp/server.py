#!/usr/bin/env python3
"""
server.py — LoRaLink PC Control Webapp Backend

Serves the 4-panel browser dashboard at http://localhost:8000.
Talks to the ESP32 over HTTP (primary) and BLE (fallback).

Usage:
    # HTTP + BLE hybrid  (device on WiFi)
    python tools/webapp/server.py --device HT-LoRa --ip 192.168.1.50

    # BLE only  (no WiFi configured on device)
    python tools/webapp/server.py --device HT-LoRa

    # Then open: http://localhost:8000
"""

from __future__ import annotations

import argparse
import asyncio
from datetime import datetime
import re
import json
import os
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Set

import uuid
import aiofiles

# pyserial — optional, degrades gracefully
try:
    import serial
    import serial.tools.list_ports

    PYSERIAL = True
except ImportError:
    PYSERIAL = False
    print("WARNING: pyserial not installed — Serial transport disabled")

# ── FastAPI / WebSocket ──────────────────────────────────────────────────────
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request, HTTPException
from fastapi.responses import (
    JSONResponse,
    PlainTextResponse,
    HTMLResponse,
)
from fastapi.staticfiles import StaticFiles
import uvicorn

# ── aiohttp for device HTTP ──────────────────────────────────────────────────
try:
    import aiohttp

    AIOHTTP = True
except ImportError:
    AIOHTTP = False

# ── BLE stack — import from sibling ble_instrument.py ────────────────────────
_tools_dir = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_tools_dir))
try:
    from ble_instrument import (
        BLELink,
        Config as BLEConfig,
        ResponseBuffer,
    )
    from bleak import BleakScanner

    BLEAK = True
except ImportError:
    BLEAK = False
    print("WARNING: bleak not available — BLE transport disabled")

STATIC_DIR = Path(__file__).parent / "static"
SETTINGS_FILE = Path(__file__).parent / ".settings.json"

NODES_FILE = Path(__file__).parent / ".nodes.json"
SEQUENCES_FILE = Path(__file__).parent / ".sequences.json"
SUITES_FILE = Path(__file__).parent / ".suites.json"
CONFIGS_DIR = Path(__file__).parent / "configs"
LOGS_DIR = Path(__file__).parent / "logs"
LOGS_DIR.mkdir(exist_ok=True)
BOARDS_DIR = Path(__file__).parent / "boards"
BOARDS_DIR.mkdir(exist_ok=True)

_TRANSPORT_STRATEGIES = [
    "http_first",  # HTTP when reachable; BLE after 3 consecutive failures
    "ble_only",  # Always BLE; ignores HTTP entirely
    "readwrite_split",  # STATUS/MESH → HTTP; GPIO/RELAY/SCHED → BLE
    "immediate_fallback",  # HTTP only when zero failures on record; else BLE
    "roundrobin",  # Alternate HTTP/BLE on every command
    "serial_only",  # Always Serial; ignores HTTP and BLE entirely
]

# ════════════════════════════════════════════════════════════════════════════
# 0b. Node registry — persisted to .nodes.json
# ════════════════════════════════════════════════════════════════════════════


@dataclass
class NodeConfig:
    id: str
    name: str
    type: str  # "wifi" | "serial" | "ble" | "lora"
    address: str  # IP, COM port, BLE prefix, or gateway node name
    active: bool = False

    def to_dict(self) -> dict:
        return {
            "id": self.id,
            "name": self.name,
            "type": self.type,
            "address": self.address,
            "active": self.active,
        }

    @staticmethod
    def from_dict(d: dict) -> "NodeConfig":
        return NodeConfig(
            id=d["id"],
            name=d["name"],
            type=d["type"],
            address=d["address"],
            active=d.get("active", False),
        )


class NodeRegistry:
    def __init__(self) -> None:
        self._nodes: list[NodeConfig] = []
        self._load()

    def _load(self) -> None:
        if NODES_FILE.exists():
            try:
                data = json.loads(NODES_FILE.read_text())
                self._nodes = [NodeConfig.from_dict(n) for n in data]
            except Exception:
                pass

    def _save(self) -> None:
        try:
            NODES_FILE.write_text(
                json.dumps([n.to_dict() for n in self._nodes], indent=2)
            )
        except Exception as e:
            print(f"[nodes] Save failed: {e}")

    def list(self) -> list[NodeConfig]:
        return list(self._nodes)

    def add(self, name: str, type_: str, address: str) -> NodeConfig:
        node = NodeConfig(id=str(uuid.uuid4()), name=name, type=type_, address=address)
        self._nodes.append(node)
        self._save()
        return node

    def remove(self, node_id: str) -> bool:
        before = len(self._nodes)
        self._nodes = [n for n in self._nodes if n.id != node_id]
        if len(self._nodes) < before:
            self._save()
            return True
        return False

    def clear(self) -> None:
        self._nodes = []
        self._save()

    def set_active(self, node_id: str) -> Optional[NodeConfig]:
        target = next((n for n in self._nodes if n.id == node_id), None)
        if not target:
            return None
        for n in self._nodes:
            n.active = n.id == node_id
        self._save()
        return target

    def get(self, node_id: str) -> Optional[NodeConfig]:
        """Get a node by ID without changing active state."""
        return next((n for n in self._nodes if n.id == node_id), None)

    def active_node(self) -> Optional[NodeConfig]:
        return next((n for n in self._nodes if n.active), None)


# ════════════════════════════════════════════════════════════════════════════
# 0c. Sequence registry — persisted to .sequences.json
# ════════════════════════════════════════════════════════════════════════════


@dataclass
class TaskSpec:
    name: str
    type: str
    pin: int
    interval: int
    duration: int = 0

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "type": self.type,
            "pin": self.pin,
            "interval": self.interval,
            "duration": self.duration,
        }

    @staticmethod
    def from_dict(d: dict) -> "TaskSpec":
        return TaskSpec(
            name=d["name"],
            type=d["type"],
            pin=int(d["pin"]),
            interval=int(d["interval"]),
            duration=int(d.get("duration", 0)),
        )


@dataclass
class Sequence:
    name: str
    tasks: list[TaskSpec] = field(default_factory=list)

    def to_dict(self) -> dict:
        return {"name": self.name, "tasks": [t.to_dict() for t in self.tasks]}

    @staticmethod
    def from_dict(d: dict) -> "Sequence":
        return Sequence(
            name=d["name"], tasks=[TaskSpec.from_dict(t) for t in d.get("tasks", [])]
        )


class SequenceRegistry:
    def __init__(self) -> None:
        self._seqs: dict[str, Sequence] = {}
        self._load()

    def _load(self) -> None:
        if SEQUENCES_FILE.exists():
            try:
                data = json.loads(SEQUENCES_FILE.read_text())
                self._seqs = {s["name"]: Sequence.from_dict(s) for s in data}
            except Exception:
                pass

    def _save(self) -> None:
        try:
            SEQUENCES_FILE.write_text(
                json.dumps([s.to_dict() for s in self._seqs.values()], indent=2)
            )
        except Exception as e:
            print(f"[sequences] Save failed: {e}")

    def list(self) -> list[Sequence]:
        return list(self._seqs.values())

    def save(self, seq: Sequence) -> None:
        self._seqs[seq.name] = seq
        self._save()

    def delete(self, name: str) -> bool:
        if name in self._seqs:
            del self._seqs[name]
            self._save()
            return True
        return False

    def get(self, name: str) -> Optional[Sequence]:
        return self._seqs.get(name)


# ════════════════════════════════════════════════════════════════════════════
# 1. DeviceState — shared live state
# ════════════════════════════════════════════════════════════════════════════


@dataclass
class DeviceState:
    status: dict = field(default_factory=dict)
    transport: str = "disconnected"  # "http" | "ble" | "disconnected"
    http_ok: bool = False
    ble_ok: bool = False  # True if any peer connected
    ble1_ok: bool = False  # peer A
    ble2_ok: bool = False  # peer B
    peer_names: list = field(default_factory=list)
    last_update: float = 0.0
    http_failures: int = 0  # consecutive HTTP failures
    settings: dict = field(default_factory=lambda: {"transport_strategy": "http_first"})
    serial_ok: bool = False
    serial_port: Optional[str] = None
    active_node: Optional[str] = None  # display name of active node
    active_ip: Optional[str] = None  # routable IP for active WiFi/LoRa node
    discovered_devices: list = field(default_factory=list)  # from network scan
    discovery_time: float = 0.0  # timestamp of last discovery scan
    discovered_ble: list = field(default_factory=list)  # all BLE devices seen
    serial_peripherals: dict = field(
        default_factory=dict
    )  # {port: {id, hwType, readings, lastSeen}}


# ════════════════════════════════════════════════════════════════════════════
# 2. WebSocketManager — browser connection pool
# ════════════════════════════════════════════════════════════════════════════


class WebSocketManager:
    def __init__(self) -> None:
        self._connections: Set[WebSocket] = set()

    async def connect(self, ws: WebSocket) -> None:
        await ws.accept()
        self._connections.add(ws)

    def disconnect(self, ws: WebSocket) -> None:
        self._connections.discard(ws)

    async def broadcast(self, data: dict) -> None:
        dead: Set[WebSocket] = set()
        for ws in self._connections:
            try:
                await ws.send_json(data)
            except Exception:
                dead.add(ws)
        self._connections -= dead


# ════════════════════════════════════════════════════════════════════════════
# 2b. SerialLink — sync serial wrapped for asyncio
# ════════════════════════════════════════════════════════════════════════════


class SerialLink:
    """Wraps a pyserial connection for use alongside BLE and HTTP transports."""

    def __init__(self, port: str, baud: int = 115200) -> None:
        self._port = port
        self._baud = baud
        self._ser: Optional[serial.Serial] = None if PYSERIAL else None
        self._loop: Optional[asyncio.AbstractEventLoop] = None

    async def connect(self) -> None:
        if not PYSERIAL:
            raise RuntimeError("pyserial not installed")
        self._loop = asyncio.get_event_loop()
        await self._loop.run_in_executor(None, self._open)
        print(f"[Serial] Connected to {self._port}")

    def _open(self) -> None:
        self._ser = serial.Serial(self._port, self._baud, timeout=1)

    async def disconnect(self) -> None:
        if self._ser and self._ser.is_open:
            self._ser.close()
        print(f"[Serial] Disconnected from {self._port}")

    async def send_command(self, cmd: str) -> bool:
        if not self.is_connected or not self._loop:
            return False
        try:
            data = (cmd.strip() + "\n").encode()
            await self._loop.run_in_executor(None, self._ser.write, data)
            return True
        except Exception as e:
            print(f"[Serial] Write error: {e}")
            return False

    @property
    def is_connected(self) -> bool:
        return bool(self._ser and self._ser.is_open)

    @property
    def device_name(self) -> str:
        return self._port


class SerialHub:
    """Rigorous Multi-Port Serial Manager with Auto-Discovery & Reconnect."""

    def __init__(self, state: DeviceState, ws_mgr: WebSocketManager) -> None:
        self.state = state
        self.ws_mgr = ws_mgr
        self._links: dict[str, serial.Serial] = {}
        self._tasks: dict[str, asyncio.Task] = {}
        self._discovery_task: Optional[asyncio.Task] = None
        self._running = False

    async def start(self) -> None:
        if not PYSERIAL:
            return
        self._running = True
        self._discovery_task = asyncio.create_task(self._discovery_loop())
        print("[SerialHub] Started discovery & monitor")

    async def stop(self) -> None:
        self._running = False
        if self._discovery_task:
            self._discovery_task.cancel()
        for t in self._tasks.values():
            t.cancel()
        for s in self._links.values():
            s.close()
        self._links.clear()

    async def _discovery_loop(self) -> None:
        """Scan for new serial ports every 3 seconds."""
        while self._running:
            try:
                available = [p.device for p in serial.tools.list_ports.comports()]
                # Add new ports (avoiding the primary serial device if set on CLI)
                for port in available:
                    if port not in self._links and port != self.state.serial_port:
                        print(f"[SerialHub] New port detected: {port}")
                        asyncio.create_task(self._connect_port(port))
            except Exception as e:
                print(f"[SerialHub] Discovery error: {e}")
            await asyncio.sleep(3.0)

    async def _connect_port(self, port: str) -> None:
        """Establish connection and start the monitor task."""
        try:
            ser = await asyncio.get_event_loop().run_in_executor(
                None, lambda: serial.Serial(port, 115200, timeout=0.1)
            )
            self._links[port] = ser
            self._tasks[port] = asyncio.create_task(self._monitor_port(port, ser))
        except Exception as e:
            # Suppress common Windows permission errors on busy ports
            if "PermissionError" in str(e) or "(13," in str(e):
                pass
            else:
                print(f"[SerialHub] Error opening {port}: {e}")

    async def _monitor_port(self, port: str, ser: serial.Serial) -> None:
        """Rigorous read loop with line-buffering and error recovery."""
        print(f"[SerialHub] Monitoring {port}...")
        buffer = ""
        try:
            while self._running and ser.is_open:
                # Read chunks to avoid blocking
                data = await asyncio.get_event_loop().run_in_executor(
                    None, ser.read, 128
                )
                if data:
                    buffer += data.decode(errors="ignore")
                    if "\n" in buffer:
                        lines = buffer.split("\n")
                        buffer = lines.pop()  # Keep partial line
                        for line in lines:
                            await self._handle_line(port, line.strip())
                else:
                    await asyncio.sleep(0.05)
        except Exception as e:
            print(f"[SerialHub] Connection lost on {port}: {e}")
        finally:
            if port in self._links:
                del self._links[port]
            if port in self._tasks:
                del self._tasks[port]
            if port in self.state.serial_peripherals:
                del self.state.serial_peripherals[port]
            ser.close()

    async def _handle_line(self, port: str, line: str) -> None:
        """Parse HELLO/SENSOR messages and update state/broadcast."""
        if not line:
            return

        # HELLO device=XIAO hw=SAMD21 fw=1.2 caps=adc0,adc1
        if line.startswith("HELLO"):
            parts = re.findall(r"(\w+)=([\w\.,]+)", line)
            info = {k: v for k, v in parts}
            dev_id = info.get("device", "Unknown")
            self.state.serial_peripherals[port] = {
                "id": dev_id,
                "hwType": info.get("hw", "generic"),
                "fw": info.get("fw", "0.0"),
                "caps": info.get("caps", "").split(","),
                "lastSeen": time.time(),
                "readings": {},
            }
            print(f"[SerialHub] Handshake from {port}: {dev_id}")
            await self.ws_mgr.broadcast({"type": "serial_hello", "port": port, **info})

        # SENSOR adc0=1.234 adc1=2.112
        elif line.startswith("SENSOR"):
            if port not in self.state.serial_peripherals:
                return  # Ignore until HELLO
            parts = re.findall(r"([\w\.]+)=([\w\.-]+)", line)
            readings = {k: v for k, v in parts}
            self.state.serial_peripherals[port]["readings"].update(readings)
            self.state.serial_peripherals[port]["lastSeen"] = time.time()
            await self.ws_mgr.broadcast(
                {
                    "type": "serial_sensor",
                    "port": port,
                    "id": self.state.serial_peripherals[port]["id"],
                    "data": readings,
                }
            )

    async def send_command(self, port: str, cmd: str) -> bool:
        """Send a configuration or control string to a specific port."""
        ser = self._links.get(port)
        if not ser or not ser.is_open:
            return False
        try:
            data = (cmd.strip() + "\n").encode()
            await asyncio.get_event_loop().run_in_executor(None, ser.write, data)
            return True
        except Exception:
            return False


# ════════════════════════════════════════════════════════════════════════════
# 3. TransportManager — hybrid HTTP + BLE command routing
# ════════════════════════════════════════════════════════════════════════════


class TransportManager:
    def __init__(
        self,
        state: DeviceState,
        device_ip: Optional[str],
        peers: list,  # list[BLELink] — 0, 1, or 2 peers
        serial_link: Optional["SerialLink"] = None,
    ) -> None:
        self.state = state
        self.device_ip = device_ip
        self._peers = peers
        self._session: Optional[aiohttp.ClientSession] = None
        self._round_counter: int = 0
        self._serial: Optional[SerialLink] = serial_link
        self._rediscover_running: bool = False  # guard: only one scan at a time

    # ── Public send ──────────────────────────────────────────────────────────

    async def send_command(self, cmd: str) -> bool:
        transport = await self.pick_transport(cmd)
        if transport == "serial":
            ok = await self._send_serial(cmd)
            self.state.serial_ok = ok
            self.state.transport = "serial" if ok else "disconnected"
            return ok
        if transport == "http":
            ok = await self._send_http(cmd)
            if ok:
                self.state.http_failures = 0
                self.state.http_ok = True
                self.state.transport = "http"
                return True
            self.state.http_failures += 1
            self.state.http_ok = False
            # After 5 consecutive HTTP failures the device IP has likely changed
            # (e.g. DHCP reissue after reboot). Kick off a background subnet scan.
            if self.state.http_failures == 5 and not self._rediscover_running:
                asyncio.create_task(self._try_refresh_ip())
        # BLE path (fallback or primary) — broadcasts to all connected peers
        ok = await self._send_ble(cmd)
        self.state.ble_ok = ok
        self.state.ble1_ok = len(self._peers) > 0 and self._peers[0].is_connected
        self.state.ble2_ok = len(self._peers) > 1 and self._peers[1].is_connected
        self.state.transport = "ble" if ok else "disconnected"
        return ok

    async def pick_transport(self, cmd: str) -> str:
        """Dispatch to the saved transport strategy (persisted in .settings.json)."""
        strategy = self.state.settings.get("transport_strategy", "http_first")
        _ip = self.state.active_ip or self.device_ip
        http_ok = bool(_ip and AIOHTTP)

        if strategy == "ble_only":
            # Always BLE — ignores HTTP entirely
            return "ble"

        elif strategy == "readwrite_split":
            # STATUS / MESH queries → HTTP; mutating commands → BLE
            read_prefixes = ("STATUS", "SCHED LIST", "MESH", "LOG")
            is_read = any(cmd.upper().startswith(p) for p in read_prefixes)
            return ("http" if http_ok else "ble") if is_read else "ble"

        elif strategy == "immediate_fallback":
            # HTTP only when there is a perfect track record (zero failures)
            return "http" if (http_ok and self.state.http_failures == 0) else "ble"

        elif strategy == "roundrobin":
            # Alternate between HTTP and BLE on every command
            self._round_counter += 1
            return "http" if (self._round_counter % 2 == 0 and http_ok) else "ble"

        elif strategy == "serial_only":
            return "serial"

        else:  # http_first (default)
            # HTTP while reachable; fall back to BLE after 3 consecutive failures
            return "http" if (http_ok and self.state.http_failures < 3) else "ble"

    # ── Private transports ───────────────────────────────────────────────────

    async def _send_http(self, cmd: str) -> bool:
        ip = self.state.active_ip or self.device_ip
        if not AIOHTTP or not ip:
            return False
        try:
            session = await self._get_session()
            async with session.post(
                f"http://{ip}/api/cmd",
                data={"cmd": cmd},
                timeout=aiohttp.ClientTimeout(total=12.0),
            ) as r:
                return r.status == 200
        except Exception:
            return False

    async def _send_ble(self, cmd: str) -> bool:
        """Broadcast command to all connected BLE peers; returns True if any succeeded."""
        if not BLEAK or not self._peers:
            return False
        results = await asyncio.gather(
            *[p.send_command(cmd) for p in self._peers if p.is_connected],
            return_exceptions=True,
        )
        return any(r is True for r in results)

    async def _send_serial(self, cmd: str) -> bool:
        if not self._serial or not self._serial.is_connected:
            return False
        return await self._serial.send_command(cmd)

    async def _get_session(self) -> aiohttp.ClientSession:
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession()
        return self._session

    async def _try_refresh_ip(self) -> None:
        """Scan the /24 subnet of the last known device IP to find it after a
        DHCP-assigned address change (e.g. after an unexpected reboot).
        Runs as a background asyncio task; updates state.active_ip on success."""
        if not AIOHTTP or not self.state.active_ip:
            return
        self._rediscover_running = True
        old_ip = self.state.active_ip
        parts = old_ip.rsplit(".", 1)
        if len(parts) != 2:
            self._rediscover_running = False
            return
        base = parts[0]
        print(f"[transport] HTTP failed 5x -- scanning {base}.0/24 (was {old_ip})")

        sem = asyncio.Semaphore(20)  # cap concurrent probes

        async def _probe(ip: str) -> Optional[str]:
            async with sem:
                if ip == old_ip:
                    return None
                try:
                    s = await self._get_session()
                    async with s.get(
                        f"http://{ip}/api/status",
                        timeout=aiohttp.ClientTimeout(total=0.8),
                    ) as r:
                        if r.status == 200:
                            return ip
                except Exception:
                    pass
            return None

        candidates = [f"{base}.{i}" for i in range(1, 255)]
        results = await asyncio.gather(
            *[_probe(ip) for ip in candidates], return_exceptions=True
        )
        found = next((r for r in results if isinstance(r, str)), None)
        if found:
            print(f"[transport] Device found at new IP {found} (was {old_ip})")
            self.state.active_ip = found
            self.state.http_failures = 0
            self.state.http_ok = True
        else:
            print(f"[transport] No device found on {base}.0/24 -- staying on BLE")
        self._rediscover_running = False

    async def close(self) -> None:
        if self._session and not self._session.closed:
            await self._session.close()


# ════════════════════════════════════════════════════════════════════════════
# 4. StatusPoller — background task pushing device state to WS clients
# ════════════════════════════════════════════════════════════════════════════


class StatusPoller:
    INTERVAL = 0.25  # seconds between polls

    def __init__(
        self,
        state: DeviceState,
        ws_manager: WebSocketManager,
        device_ip: Optional[str],
        peers: list,  # list[BLELink] — 0, 1, or 2 peers
        node_reg: "NodeRegistry",
    ) -> None:
        self.state = state
        self.ws_manager = ws_manager
        self.device_ip = device_ip
        self._peers = peers
        self.node_reg = node_reg
        self._session_start = datetime.now().strftime("%Y%m%d_%H%M%S")
        self._session: Optional[aiohttp.ClientSession] = None
        self._running = False

    async def run(self) -> None:
        self._running = True
        while self._running:
            # HTTP poll (primary — returns full status JSON)
            status = await self._poll_http()
            if status:
                self.state.status = status
                self.state.last_update = time.time()
                await self.ws_manager.broadcast(
                    {
                        "type": "status",
                        "peer": "A",
                        "transport": self.state.transport,
                        "http_ok": self.state.http_ok,
                        "ble_ok": self.state.ble_ok,
                        "serial_peripherals": self.state.serial_peripherals,
                        **status,
                    }
                )
                # Harvest trace unconditionally — fall back to IP as log filename
                # if the device hasn't reported its ID yet (e.g. first boot).
                ip = self.state.active_ip or self.device_ip
                if ip:
                    device_id = status.get("id") or ip.replace(".", "_")
                    await self._harvest_trace(ip, device_id)

            # --- Multi-Peer Harvesting ---
            # Also harvest from any other active nodes in the registry that aren't the primary
            # This ensures "Master" and "Slave" both get logs even if one isn't the current "Active" transport target.
            for node in self.node_reg.list():
                if node.active and node.type == "wifi" and node.address:
                    # Avoid double-harvesting if this is already the primary active_ip
                    if node.address != (self.state.active_ip or self.device_ip):
                        await self._harvest_trace(node.address, node.id)

            # BLE poll — all peers in parallel; each broadcast tagged with peer label
            if self._peers:
                ble_results = await asyncio.gather(
                    *[self._poll_ble_peer(ble, i) for i, ble in enumerate(self._peers)],
                    return_exceptions=True,
                )
                for i, result in enumerate(ble_results):
                    if isinstance(result, dict) and result:
                        peer_label = chr(ord("A") + i)
                        if not status:  # only update primary status if HTTP missed
                            self.state.status = result
                            self.state.last_update = time.time()
                        await self.ws_manager.broadcast(
                            {
                                "type": "status",
                                "peer": peer_label,
                                "transport": "ble",
                                "http_ok": self.state.http_ok,
                                "ble_ok": self.state.ble_ok,
                                "serial_peripherals": self.state.serial_peripherals,
                                **result,
                            }
                        )

            await asyncio.sleep(self.INTERVAL)

    async def _harvest_trace(self, ip: str, device_id: str) -> None:
        """Fetch /trace.log from device and append to local storage."""
        if not ip:
            return

        # Try to resolve registry ID for this device to keep filenames consistent
        registry_id = device_id
        for node in self.node_reg.list():
            if node.address == ip:
                registry_id = node.id
                break

        local_log = LOGS_DIR / f"{registry_id}_{self._session_start}_trace.log"
        try:
            session = await self._get_session()
            async with session.get(
                f"http://{ip}/api/files/read?path=/trace.log",
                timeout=aiohttp.ClientTimeout(total=5.0),
            ) as r:
                if r.status == 200:
                    content = await r.text()
                    if content.strip():
                        async with aiofiles.open(
                            local_log, mode="a", encoding="utf-8"
                        ) as f:
                            await f.write(content)
                        # Clear the trace on device to avoid duplicates
                        await session.post(f"http://{ip}/api/trace/clear", timeout=2.0)
        except Exception:
            pass

    def stop(self) -> None:
        self._running = False

    async def _poll_http(self) -> Optional[dict]:
        ip = self.state.active_ip or self.device_ip
        if not AIOHTTP or not ip:
            return None
        try:
            session = await self._get_session()
            async with session.get(
                f"http://{ip}/api/status",
                timeout=aiohttp.ClientTimeout(total=12.0),
            ) as r:
                if r.status == 200:
                    data = await r.json(content_type=None)
                    self.state.http_ok = True
                    self.state.http_failures = 0
                    self.state.transport = "http"
                    return data
        except Exception:
            pass
        self.state.http_ok = False
        self.state.http_failures += 1
        return None

    async def _poll_ble_peer(self, ble: "BLELink", idx: int) -> Optional[dict]:
        """Poll a single BLE peer, update per-peer connection state."""
        if not BLEAK or not ble.is_connected:
            if idx == 0:
                self.state.ble1_ok = False
            elif idx == 1:
                self.state.ble2_ok = False
            self.state.ble_ok = self.state.ble1_ok or self.state.ble2_ok
            if not self.state.ble_ok:
                self.state.transport = "disconnected"
            return None
        await ble.send_command("STATUS")
        if idx == 0:
            self.state.ble1_ok = True
        elif idx == 1:
            self.state.ble2_ok = True
        self.state.ble_ok = True
        self.state.transport = "ble"
        existing = self.state.status or {}
        return {**existing, "_via": "ble", "_peer": ble.device_name}

    async def _get_session(self) -> aiohttp.ClientSession:
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession()
        return self._session

    async def close(self) -> None:
        if self._session and not self._session.closed:
            await self._session.close()


# ════════════════════════════════════════════════════════════════════════════
# 5. HTTP proxy helpers (device API → browser)
# ════════════════════════════════════════════════════════════════════════════


async def _proxy_get(device_ip: Optional[str], path: str) -> Optional[dict]:
    if not AIOHTTP or not device_ip:
        return None
    try:
        async with aiohttp.ClientSession() as s:
            async with s.get(
                f"http://{device_ip}{path}",
                timeout=aiohttp.ClientTimeout(total=12.0),
            ) as r:
                return await r.json(content_type=None)
    except Exception:
        return None


async def _proxy_post(device_ip: Optional[str], path: str, data: dict) -> bool:
    if not AIOHTTP or not device_ip:
        return False
    try:
        async with aiohttp.ClientSession() as s:
            async with s.post(
                f"http://{device_ip}{path}",
                data=data,
                timeout=aiohttp.ClientTimeout(total=12.0),
            ) as r:
                return r.status == 200
    except Exception:
        return False


async def _send_cmd_to_ip(ip: str, cmd: str) -> bool:
    """Send a single command directly to a specific device IP, bypassing TransportManager."""
    if not AIOHTTP or not ip:
        return False
    try:
        async with aiohttp.ClientSession() as s:
            async with s.post(
                f"http://{ip}/api/cmd",
                data={"cmd": cmd},
                timeout=aiohttp.ClientTimeout(total=12.0),
            ) as r:
                return r.status == 200
    except Exception:
        return False


# ════════════════════════════════════════════════════════════════════════════
# 6. App factory
# ════════════════════════════════════════════════════════════════════════════


def build_app(
    device_name: str,
    device_ip: Optional[str],
    device_name2: Optional[str] = None,
    no_ble: bool = False,
    debug: bool = False,
) -> FastAPI:
    app = FastAPI(title="LoRaLink PC Control")
    state = DeviceState()
    ws_mgr = WebSocketManager()

    node_reg = NodeRegistry()
    seq_reg = SequenceRegistry()
    CONFIGS_DIR.mkdir(parents=True, exist_ok=True)
    _serial_link: Optional[SerialLink] = None

    # Singletons populated at startup
    _ble_peers: list = []  # up to 2 BLELink instances
    _serial_hub: Optional[SerialHub] = None
    _transport: Optional[TransportManager] = None
    _poller: Optional[StatusPoller] = None
    _poll_task: Optional[asyncio.Task] = None

    def _effective_ip() -> Optional[str]:
        """Resolve the effective IP: active_ip (if set) overrides startup device_ip."""
        return state.active_ip or device_ip

    # ── Startup / Shutdown ────────────────────────────────────────────────

    async def _ble_scan() -> None:
        """Background BLE scan — runs after HTTP is already serving."""
        try:
            # Look up current prefixes from state (allows runtime updates via header)
            pa = state.settings.get("ble_prefix_a", device_name)
            pb = state.settings.get("ble_prefix_b", device_name2)
            prefixes = [p for p in [pa, pb] if p]

            print(f"[BLE] Scanning for {prefixes} (10s)…")
            all_devs = await BleakScanner.discover(timeout=10.0)

            # Store ALL discovered devices for the UI to list
            state.discovered_ble = [
                {"name": d.name or "Unnamed", "address": d.address, "rssi": d.rssi}
                for d in all_devs
            ]

            matched_addrs: set = set()
            matches: list = []
            for prefix in prefixes:
                for d in all_devs:
                    if (
                        d.name
                        and d.name.startswith(prefix)
                        and d.address not in matched_addrs
                    ):
                        matches.append(d)
                        matched_addrs.add(d.address)
                        break
            print(
                f"[BLE] Found {len(matches)} matching device(s) from {len(all_devs)} total"
            )
            for i, dev in enumerate(matches[:2]):
                buf = ResponseBuffer()
                # Use the prefix that matched this specific device
                p_match = next((p for p in prefixes if dev.name.startswith(p)), pa)
                cfg = BLEConfig(device_name_prefix=p_match)
                ble = BLELink(cfg, buf)
                try:
                    await ble.connect(dev)
                    _ble_peers.append(ble)
                    state.peer_names = state.peer_names + [dev.name]
                    print(f"[BLE] Peer {chr(65 + i)}: {dev.name} ✓")
                except Exception as e:
                    print(f"[BLE] Peer {i + 1} ({dev.name}) failed: {e}")
            state.ble1_ok = len(_ble_peers) >= 1 and _ble_peers[0].is_connected
            state.ble2_ok = len(_ble_peers) >= 2 and _ble_peers[1].is_connected
            state.ble_ok = bool(_ble_peers)
            if _poller:
                _poller._peers = list(_ble_peers)
            if _transport:
                _transport._peers = list(_ble_peers)
        except Exception as e:
            print(f"[BLE] Scan failed: {e}")

    @app.on_event("startup")
    async def _startup() -> None:
        nonlocal _ble_peers, _transport, _poller, _poll_task, _serial_hub

        # Initialize active IP to the startup device_ip
        state.active_ip = device_ip

        # Load persisted transport strategy
        if SETTINGS_FILE.exists():
            try:
                saved = json.loads(SETTINGS_FILE.read_text())
                if saved.get("transport_strategy") in _TRANSPORT_STRATEGIES:
                    state.settings["transport_strategy"] = saved["transport_strategy"]
                    print(
                        f"[settings] Transport strategy: {state.settings['transport_strategy']}"
                    )
            except Exception:
                pass

        # HTTP transport + status poller start immediately — no waiting for BLE
        _serial_hub = SerialHub(state, ws_mgr)
        _transport = TransportManager(state, device_ip, _ble_peers, _serial_link)
        _poller = StatusPoller(state, ws_mgr, device_ip, _ble_peers, node_reg)
        _poll_task = asyncio.create_task(_poller.run())
        asyncio.create_task(_serial_hub.start())
        port = int(os.environ.get("PORT", 8000))
        print(f"[server] Listening at http://localhost:{port}")

        # BLE scan deferred to background task — won't block first HTTP poll
        if BLEAK and not no_ble:
            asyncio.create_task(_ble_scan())

    @app.on_event("shutdown")
    async def _shutdown() -> None:
        if _poller:
            _poller.stop()
        if _poll_task:
            _poll_task.cancel()
        if _transport:
            await _transport.close()
        for ble in _ble_peers:
            try:
                await ble.disconnect()
            except Exception:
                pass
        if _serial_link:
            try:
                await _serial_link.disconnect()
            except Exception:
                pass
        if _serial_hub:
            await _serial_hub.stop()

    # ── Static files ──────────────────────────────────────────────────────

    app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")

    @app.get("/")
    async def _root():
        # Serve index.html dynamically to strip debug IDs if not in --debug mode
        try:
            with open(STATIC_DIR / "index.html", "r", encoding="utf-8") as f:
                content = f.read()
            if not debug:
                # Strip all <span class="dbg-id">...</span> tags
                content = re.sub(r'<span class="dbg-id">[^<]*</span>', "", content)
            return HTMLResponse(content)
        except Exception as e:
            return PlainTextResponse(f"Error loading index.html: {e}", status_code=500)

    @app.get("/devqa")
    async def _devqa():
        try:
            with open(STATIC_DIR / "devqa.html", "r", encoding="utf-8") as f:
                return HTMLResponse(f.read())
        except Exception as e:
            return PlainTextResponse(f"Error loading devqa.html: {e}", status_code=500)

    @app.get("/product-builder")
    async def _product_builder():
        try:
            with open(STATIC_DIR / "product_builder.html", "r", encoding="utf-8") as f:
                return HTMLResponse(f.read())
        except Exception as e:
            return PlainTextResponse(f"Error loading product_builder.html: {e}", status_code=500)

    # ── WebSocket ─────────────────────────────────────────────────────────

    @app.websocket("/ws")
    async def _ws_endpoint(ws: WebSocket) -> None:
        await ws_mgr.connect(ws)
        # Push current state immediately on connect
        if state.status:
            await ws.send_json(
                {
                    "type": "status",
                    "peer": "A",
                    "transport": state.transport,
                    "http_ok": state.http_ok,
                    "ble_ok": state.ble_ok,
                    "ble1_ok": state.ble1_ok,
                    "ble2_ok": state.ble2_ok,
                    "peer_names": state.peer_names,
                    **state.status,
                }
            )
        try:
            while True:
                await ws.receive_text()  # keep connection alive; ignore client pings
        except WebSocketDisconnect:
            ws_mgr.disconnect(ws)

    # ── Command endpoint ──────────────────────────────────────────────────

    @app.post("/api/cmd")
    async def _cmd(body: dict) -> PlainTextResponse:
        cmd = (body.get("cmd") or "").strip()
        if not cmd:
            return PlainTextResponse("Missing cmd", status_code=400)
        if _transport is None:
            return PlainTextResponse("No transport available", status_code=503)
        ok = await _transport.send_command(cmd)
        return PlainTextResponse("OK" if ok else "ERR", status_code=200 if ok else 502)

    # ── AI command recommendation ─────────────────────────────────────────

    @app.post("/api/recommend")
    async def _recommend(body: dict) -> JSONResponse:
        """Call Claude API to suggest the next test command based on history."""
        import os
        import json as _json

        try:
            import anthropic as _anthropic
        except ImportError:
            return JSONResponse({"error": "anthropic package not installed"}, status_code=503)

        api_key = os.environ.get("ANTHROPIC_API_KEY")
        if not api_key:
            return JSONResponse({"error": "ANTHROPIC_API_KEY not set"}, status_code=503)

        # Sanitise inputs — never trust client-provided strings past a length cap
        cmd  = str(body.get("cmd",      "")).strip()[:200]
        resp = str(body.get("response", "")).strip()[:500]
        raw_hist = body.get("history", [])
        if not isinstance(raw_hist, list):
            raw_hist = []

        # Build a compact history block (last 6 exchanges)
        history_lines: list[str] = []
        for h in raw_hist[-6:]:
            if not isinstance(h, dict):
                continue
            hcmd = str(h.get("cmd", "")).strip()[:200]
            hresp = str(h.get("response", "")).strip()[:150]
            if hcmd:
                history_lines.append(f"→ {hcmd}")
            if hresp:
                history_lines.append(f"← {hresp}")

        user_content = ""
        if history_lines:
            user_content += "Recent exchanges:\n" + "\n".join(history_lines) + "\n\n"
        user_content += f"Last command sent: {cmd}\nDevice response:   {resp}\n\nSuggest the single most useful next test command."

        try:
            client = _anthropic.AsyncAnthropic(api_key=api_key)
            message = await client.messages.create(
                model="claude-opus-4-6",
                max_tokens=200,
                system=(
                    "You are a concise test engineer assistant for embedded IoT devices. "
                    "Based on the command history and last device response, suggest the single "
                    "most useful next test command. The command must be a raw device command string. "
                    "Respond with valid JSON only — no extra text."
                ),
                messages=[{"role": "user", "content": user_content}],
                output_config={
                    "format": {
                        "type": "json_schema",
                        "schema": {
                            "type": "object",
                            "properties": {
                                "command": {"type": "string"},
                                "reason":  {"type": "string"},
                            },
                            "required": ["command", "reason"],
                            "additionalProperties": False,
                        },
                    }
                },
            )
            # Extract the text block (structured output is always the last text block)
            text = next((b.text for b in message.content if b.type == "text"), "{}")
            data = _json.loads(text)
            return JSONResponse(data)
        except Exception as exc:
            return JSONResponse({"error": str(exc)}, status_code=500)

    # ── Transport status ──────────────────────────────────────────────────

    @app.get("/api/transport")
    async def _transport_status() -> JSONResponse:
        return JSONResponse(
            {
                "transport": state.transport,
                "http_ok": state.http_ok,
                "ble_ok": state.ble_ok,
                "ble1_ok": state.ble1_ok,
                "ble2_ok": state.ble2_ok,
                "peer_names": state.peer_names,
                "http_failures": state.http_failures,
                "last_update": round(time.time() - state.last_update, 1)
                if state.last_update
                else -1,
                "serial_ok": state.serial_ok,
                "serial_port": state.serial_port,
                "active_node": state.active_node,
                "active_ip": state.active_ip,
                "serial_peripherals": state.serial_peripherals,
            }
        )

    # ── Device API proxy — status ─────────────────────────────────────────

    def _inject_debug_version(d: dict) -> dict:
        if debug and "version" in d and not d["version"].endswith("d"):
            d["version"] += "d"
        return d

    @app.get("/api/status")
    async def _status() -> JSONResponse:
        if state.status:
            return JSONResponse(_inject_debug_version(state.status.copy()))
        data = await _proxy_get(_effective_ip(), "/api/status")
        return JSONResponse(_inject_debug_version(data or {}))

    # ── Device API proxy — schedule ───────────────────────────────────────

    @app.get("/api/schedule")
    async def _sched_get() -> JSONResponse:
        data = await _proxy_get(_effective_ip(), "/api/schedule")
        return JSONResponse(data or {"schedules": []})

    @app.post("/api/schedule/add")
    async def _sched_add(body: dict) -> PlainTextResponse:
        ok = await _proxy_post(_effective_ip(), "/api/schedule/add", body)
        return PlainTextResponse("OK" if ok else "ERR", status_code=200 if ok else 502)

    @app.post("/api/schedule/remove")
    async def _sched_remove(body: dict) -> PlainTextResponse:
        ok = await _proxy_post(_effective_ip(), "/api/schedule/remove", body)
        return PlainTextResponse("OK" if ok else "ERR", status_code=200 if ok else 502)

    @app.post("/api/schedule/save")
    async def _sched_save() -> PlainTextResponse:
        await _proxy_post(_effective_ip(), "/api/schedule/save", {})
        # Also send via BLE in case HTTP is down
        if _transport:
            await _transport.send_command("SCHED SAVE")
        return PlainTextResponse("OK")

    @app.post("/api/schedule/clear")
    async def _sched_clear() -> PlainTextResponse:
        await _proxy_post(_effective_ip(), "/api/schedule/clear", {})
        if _transport:
            await _transport.send_command("SCHED SAVE")
        return PlainTextResponse("OK")

    # ── Transport settings ────────────────────────────────────────────────

    @app.get("/api/settings")
    async def _settings_get() -> JSONResponse:
        saved: dict = {}
        if SETTINGS_FILE.exists():
            try:
                saved = json.loads(SETTINGS_FILE.read_text())
            except Exception:
                pass
        return JSONResponse(
            {
                "transport_strategy": state.settings.get(
                    "transport_strategy", "http_first"
                ),
                "options": _TRANSPORT_STRATEGIES,
                "ble_prefix_a": saved.get("ble_prefix_a", device_name),
                "ble_prefix_b": saved.get("ble_prefix_b", device_name2 or ""),
            }
        )

    @app.post("/api/settings")
    async def _settings_post(body: dict) -> JSONResponse:
        # Read existing settings to preserve fields we aren't updating
        persisted: dict = {}
        if SETTINGS_FILE.exists():
            try:
                persisted = json.loads(SETTINGS_FILE.read_text())
            except Exception:
                pass

        strategy = body.get("transport_strategy", "")
        if strategy and strategy not in _TRANSPORT_STRATEGIES:
            return JSONResponse(
                {"ok": False, "error": f"Unknown strategy '{strategy}'"},
                status_code=400,
            )
        if strategy:
            state.settings["transport_strategy"] = strategy
            persisted["transport_strategy"] = strategy
            # Reset round-robin counter on strategy change
            if _transport:
                _transport._round_counter = 0
            print(f"[settings] Transport strategy → {strategy}")

        # BLE prefix update (takes effect on next /api/rescan)
        if "ble_prefix_a" in body:
            persisted["ble_prefix_a"] = body["ble_prefix_a"].strip()
            print(f"[settings] BLE prefix A → {persisted['ble_prefix_a']}")
        if "ble_prefix_b" in body:
            persisted["ble_prefix_b"] = body["ble_prefix_b"].strip()
            print(f"[settings] BLE prefix B → {persisted['ble_prefix_b']}")

        try:
            SETTINGS_FILE.write_text(json.dumps(persisted, indent=2))
        except Exception as e:
            print(f"[settings] Failed to persist: {e}")

        return JSONResponse({"ok": True, **persisted})

    # ── BLE rescan ────────────────────────────────────────────────────────

    @app.post("/api/rescan")
    async def _rescan() -> JSONResponse:
        """Disconnect existing BLE peers, re-scan, reconnect. Runs in background."""
        if not BLEAK:
            return JSONResponse(
                {"ok": False, "error": "BLE not available"}, status_code=503
            )

        async def _do_rescan() -> None:
            for ble in list(_ble_peers):
                try:
                    await ble.disconnect()
                except Exception:
                    pass
            _ble_peers.clear()
            state.ble_ok = state.ble1_ok = state.ble2_ok = False
            state.peer_names = []
            await _ble_scan()  # reuse shared scan helper

        asyncio.create_task(_do_rescan())
        return JSONResponse(
            {"ok": True, "scanning": True, "msg": "Scan started (≈10s)…"}
        )

    @app.get("/api/ble/discovered")
    async def _ble_discovered() -> JSONResponse:
        """Get results of the last BLE scan."""
        return JSONResponse({"ok": True, "devices": state.discovered_ble})

    @app.get("/api/discover")
    async def _discover() -> JSONResponse:
        """Network discovery: scan subnet for LoRaLink devices. Runs in background."""
        if not AIOHTTP:
            return JSONResponse(
                {"ok": False, "error": "aiohttp not available"}, status_code=503
            )

        async def _scan_subnet() -> None:
            """Scan 172.16.0.1-254 for devices responding to /api/status."""
            found = []
            base_ip = "172.16.0"

            async with aiohttp.ClientSession() as session:
                tasks = []
                for i in range(1, 255):
                    ip = f"{base_ip}.{i}"
                    tasks.append(_probe_device(session, ip))

                results = await asyncio.gather(*tasks, return_exceptions=True)
                found = [r for r in results if r is not None]

            # Save discovery results
            state.discovered_devices = found
            state.discovery_time = time.time()

        async def _probe_device(
            session: aiohttp.ClientSession, ip: str
        ) -> Optional[dict]:
            """Probe a single IP and return device info if it's a LoRaLink device."""
            try:
                async with session.get(
                    f"http://{ip}/api/status",
                    timeout=aiohttp.ClientTimeout(total=4.0),
                ) as r:
                    if r.status == 200:
                        data = await r.json()
                        print(f"[scan] Found Antigravity device at {ip}")
                        if "myId" in data or "id" in data:  # LoRaLink device
                            return {
                                "ip": ip,
                                "name": data.get("myId") or data.get("id", "Unknown"),
                                "rssi": data.get("rssi"),
                                "bat": data.get("bat"),
                                "uptime": data.get("uptime"),
                            }
            except Exception:
                pass
            return None

        # Start scan in background
        asyncio.create_task(_scan_subnet())
        return JSONResponse(
            {"ok": True, "scanning": True, "msg": "Network scan started (≈15s)…"}
        )

    @app.get("/api/discover/results")
    async def _discover_results() -> JSONResponse:
        """Get results from the last network discovery scan."""
        if not hasattr(state, "discovered_devices"):
            return JSONResponse(
                {
                    "ok": False,
                    "devices": [],
                    "msg": "No scan performed yet. Start with /api/discover",
                }
            )
        return JSONResponse(
            {
                "ok": True,
                "devices": state.discovered_devices or [],
                "timestamp": state.discovery_time
                if hasattr(state, "discovery_time")
                else None,
            }
        )

    # ── Node registry ─────────────────────────────────────────────────────

    @app.get("/api/nodes")
    async def _nodes_list() -> JSONResponse:
        return JSONResponse([n.to_dict() for n in node_reg.list()])

    @app.post("/api/nodes")
    async def _nodes_add(body: dict) -> JSONResponse:
        name = body.get("name", "").strip()
        type_ = body.get("type", "wifi")
        address = body.get("address", "").strip()
        if not name or not address:
            return JSONResponse(
                {"ok": False, "error": "name and address required"}, status_code=400
            )
        if type_ not in ("wifi", "serial", "ble", "lora"):
            return JSONResponse(
                {"ok": False, "error": f"Unknown type '{type_}'"}, status_code=400
            )
        node = node_reg.add(name, type_, address)
        return JSONResponse({"ok": True, "node": node.to_dict()})

    @app.delete("/api/nodes/{node_id}")
    async def _nodes_delete(node_id: str) -> JSONResponse:
        ok = node_reg.remove(node_id)
        return JSONResponse({"ok": ok})

    @app.post("/api/nodes/clear")
    async def _nodes_clear() -> JSONResponse:
        node_reg.clear()
        return JSONResponse({"ok": True})

    @app.post("/api/nodes/{node_id}/connect")
    async def _nodes_connect(node_id: str) -> JSONResponse:
        nonlocal _serial_link
        node = node_reg.set_active(node_id)
        if not node:
            return JSONResponse(
                {"ok": False, "error": "Node not found"}, status_code=404
            )
        state.active_node = node.name
        print(f"[nodes] Active: {node.name} ({node.type}:{node.address})")
        # Route HTTP transport to this node's IP if it has one
        if node.type in ("wifi", "lora") and node.address:
            state.active_ip = node.address
        else:
            state.active_ip = None  # BLE/serial nodes have no HTTP address
        # Wire up the appropriate transport for this node type
        if node.type == "serial" and PYSERIAL:
            if _serial_link:
                try:
                    await _serial_link.disconnect()
                except Exception:
                    pass
            _serial_link = SerialLink(node.address)
            try:
                await _serial_link.connect()
                state.serial_ok = _serial_link.is_connected
                state.serial_port = node.address
            except Exception as e:
                print(f"[serial] Connect failed: {e}")
                state.serial_ok = False
            if _transport:
                _transport._serial = _serial_link
        return JSONResponse({"ok": True, "node": node.to_dict()})

    # ── ESP-NOW Peer Management ───────────────────────────────────────────

    @app.get("/api/espnow/peers")
    async def _espnow_list() -> JSONResponse:
        """Fetch list of ESP-NOW peers from the device."""
        if not _transport:
            return JSONResponse({"ok": False, "error": "No transport"}, status_code=503)
        await _transport.send_command("LISTPEERS")
        await asyncio.sleep(0.5)
        peers = state.status.get("esp_now_peers", [])
        return JSONResponse({"ok": True, "peers": peers})

    @app.post("/api/espnow/peers/add")
    async def _espnow_add(body: dict) -> JSONResponse:
        mac = body.get("mac", "").strip()
        name = body.get("name", "").strip()
        if not mac:
            return JSONResponse({"ok": False, "error": "MAC address required"}, status_code=400)
        cmd = f"ADDPEER {mac} {name}" if name else f"ADDPEER {mac}"
        ok = await _transport.send_command(cmd)
        return JSONResponse({"ok": ok})

    @app.post("/api/espnow/peers/remove")
    async def _espnow_remove(body: dict) -> JSONResponse:
        mac = body.get("mac", "").strip()
        if not mac:
            return JSONResponse({"ok": False, "error": "MAC address required"}, status_code=400)
        ok = await _transport.send_command(f"RMPEER {mac}")
        return JSONResponse({"ok": ok})


    @app.get("/api/nodes/{node_id}/snapshot")
    async def _nodes_snapshot(node_id: str) -> JSONResponse:
        """Fetch status + schedule from a specific node (WiFi/LoRa only)."""
        node = node_reg.get(node_id)
        if not node:
            return JSONResponse(
                {"ok": False, "error": "Node not found"}, status_code=404
            )
        if node.type not in ("wifi", "lora") or not node.address:
            return JSONResponse(
                {
                    "ok": False,
                    "error": f"Node '{node.name}' has no HTTP address (type={node.type})",
                },
                status_code=400,
            )
        status_data, schedule_data = await asyncio.gather(
            _proxy_get(node.address, "/api/status"),
            _proxy_get(node.address, "/api/schedule"),
        )
        return JSONResponse(
            {
                "ok": True,
                "node_id": node_id,
                "node_name": node.name,
                "status": status_data or {},
                "schedule": schedule_data or {"schedules": []},
            }
        )

    # ── Serial ports ──────────────────────────────────────────────────────

    @app.get("/api/serial/ports")
    async def _serial_ports() -> JSONResponse:
        if not PYSERIAL:
            return JSONResponse({"ports": [], "error": "pyserial not installed"})
        ports = [p.device for p in serial.tools.list_ports.comports()]
        return JSONResponse({"ports": ports, "peripherals": state.serial_peripherals})

    @app.post("/api/serial/config")
    async def _serial_config(body: dict) -> JSONResponse:
        port = body.get("port")
        cmd = body.get("cmd")
        if not port or not cmd:
            return JSONResponse({"ok": False, "error": "port and cmd required"})
        if not _serial_hub:
            return JSONResponse({"ok": False, "error": "SerialHub not active"})

        ok = await _serial_hub.send_command(port, cmd)
        return JSONResponse({"ok": ok})

    # ── Board Definitions ─────────────────────────────────────────────────

    @app.get("/api/boards")
    async def _boards_list() -> JSONResponse:
        boards = []
        if BOARDS_DIR.exists():
            for p in BOARDS_DIR.glob("*.json"):
                try:
                    data = json.loads(p.read_text())
                    boards.append({"id": data.get("id"), "name": data.get("name")})
                except Exception:
                    pass
        return JSONResponse(boards)

    @app.get("/api/boards/{hw_id}")
    async def _board_get(hw_id: str) -> JSONResponse:
        p = BOARDS_DIR / f"{hw_id}.json"
        if not p.exists():
            return JSONResponse({"error": "Board not found"}, status_code=404)
        try:
            return JSONResponse(json.loads(p.read_text()))
        except Exception as e:
            return JSONResponse({"error": str(e)}, status_code=500)

    # ── Test sequences ────────────────────────────────────────────────────

    @app.get("/api/sequences")
    async def _seq_list() -> JSONResponse:
        return JSONResponse([s.to_dict() for s in seq_reg.list()])

    @app.post("/api/sequences")
    async def _seq_save(body: dict) -> JSONResponse:
        name = body.get("name", "").strip()
        if not name:
            return JSONResponse(
                {"ok": False, "error": "name required"}, status_code=400
            )
        tasks = [TaskSpec.from_dict(t) for t in body.get("tasks", [])]
        seq_reg.save(Sequence(name=name, tasks=tasks))
        return JSONResponse({"ok": True, "name": name, "count": len(tasks)})

    @app.delete("/api/sequences/{name}")
    async def _seq_delete(name: str) -> JSONResponse:
        ok = seq_reg.delete(name)
        return JSONResponse({"ok": ok})

    @app.post("/api/sequences/{name}/apply")
    async def _seq_apply(name: str) -> JSONResponse:
        seq = seq_reg.get(name)
        if not seq:
            return JSONResponse(
                {"ok": False, "error": "Sequence not found"}, status_code=404
            )
        if not _transport:
            return JSONResponse({"ok": False, "error": "No transport"}, status_code=503)
        results = []
        for t in seq.tasks:
            dur = f" {t.duration}" if t.duration else ""
            cmd = f"SCHED ADD {t.name} {t.type} {t.pin} {t.interval}{dur}"
            ok = await _transport.send_command(cmd)
            results.append({"task": t.name, "ok": ok})
            await asyncio.sleep(0.2)
        all_ok = all(r["ok"] for r in results)
        failed = [r["task"] for r in results if not r["ok"]]
        # Only persist to flash when every task applied — partial saves leave
        # the device in an inconsistent state that's hard to reason about.
        saved = False
        if all_ok:
            saved = await _transport.send_command("SCHED SAVE")
        return JSONResponse(
            {
                "ok": all_ok,
                "partial": not all_ok,
                "failed": failed,
                "saved": saved,
                "results": results,
            }
        )

    @app.post("/api/sequences/{name}/apply-multi")
    async def _seq_apply_multi(name: str, body: dict) -> JSONResponse:
        """Apply a sequence to multiple WiFi/LoRa nodes in parallel."""
        seq = seq_reg.get(name)
        if not seq:
            return JSONResponse(
                {"ok": False, "error": "Sequence not found"}, status_code=404
            )
        node_ids: list = body.get("node_ids", [])
        if not node_ids:
            return JSONResponse(
                {"ok": False, "error": "node_ids required"}, status_code=400
            )

        per_node_results = []
        for nid in node_ids:
            node = node_reg.get(nid)
            if not node:
                per_node_results.append(
                    {"node_id": nid, "ok": False, "error": "not found"}
                )
                continue
            if node.type not in ("wifi", "lora") or not node.address:
                per_node_results.append(
                    {
                        "node_id": nid,
                        "node_name": node.name,
                        "ok": False,
                        "error": "skipped — no HTTP address",
                    }
                )
                continue
            task_results = []
            for t in seq.tasks:
                dur = f" {t.duration}" if t.duration else ""
                cmd = f"SCHED ADD {t.name} {t.type} {t.pin} {t.interval}{dur}"
                ok = await _send_cmd_to_ip(node.address, cmd)
                task_results.append({"task": t.name, "ok": ok})
                await asyncio.sleep(0.15)
            await _send_cmd_to_ip(node.address, "SCHED SAVE")
            ok_count = sum(1 for x in task_results if x["ok"])
            per_node_results.append(
                {
                    "node_id": nid,
                    "node_name": node.name,
                    "address": node.address,
                    "tasks_ok": ok_count,
                    "tasks_total": len(task_results),
                    "ok": ok_count == len(task_results),
                    "results": task_results,
                }
            )
        return JSONResponse({"ok": True, "sequence": name, "nodes": per_node_results})

    # ── Product Profile Deploy ─────────────────────────────────────────────

    @app.post("/api/products/{name}/deploy")
    async def _product_deploy(name: str, body: dict) -> JSONResponse:
        """Deploy a product profile (config + schedule) to N nodes in parallel.

        Body: { node_ids: [...], transport: "http"|"ble"|"lora"|"mqtt", format: "J"|"C"|"K"|"B" }
        Returns: { results: [{node_id, ok, error?}], ok: N, fail: M }
        """
        # Sanitise name — strip path traversal attempts
        safe_name = name.replace("/", "_").replace("..", "_")
        product_file = CONFIGS_DIR / f"product_{safe_name}.json"
        if not product_file.exists():
            # Also try without 'product_' prefix (in case caller already included it)
            product_file = CONFIGS_DIR / f"{safe_name}.json"
        if not product_file.exists():
            return JSONResponse(
                {"ok": False, "error": f"Product '{name}' not found"}, status_code=404
            )

        try:
            product = json.loads(product_file.read_text())
        except Exception as e:
            return JSONResponse(
                {"ok": False, "error": f"Product file invalid: {e}"}, status_code=500
            )

        node_ids: list = body.get("node_ids", [])
        transport: str = body.get("transport", "http").lower()
        fmt: str = body.get("format", "J").upper()

        if not node_ids:
            return JSONResponse(
                {"ok": False, "error": "node_ids required"}, status_code=400
            )

        # Stub non-HTTP/BLE transports — planned but require firmware/server additions
        if transport not in ("http", "ble"):
            stub_msg = {
                "mqtt": "MQTT deploy: planned — requires server.py paho-mqtt client (MQTTManager)",
                "lora": "LoRa relay deploy: planned — requires node-targeted routing in firmware",
            }.get(transport, f"Transport '{transport}' not recognised")
            return JSONResponse(
                {
                    "ok": False,
                    "error": stub_msg,
                    "results": [
                        {"node_id": nid, "ok": False, "error": stub_msg}
                        for nid in node_ids
                    ],
                    "ok_count": 0,
                    "fail": len(node_ids),
                }
            )

        # Stub non-JSON formats — firmware tp_mode output switching not yet active
        if fmt != "J":
            fmt_msg = f"Format '{fmt}' requires firmware TRANS support — only J (JSON) active today"
            return JSONResponse(
                {
                    "ok": False,
                    "error": fmt_msg,
                    "results": [
                        {"node_id": nid, "ok": False, "error": fmt_msg}
                        for nid in node_ids
                    ],
                    "ok_count": 0,
                    "fail": len(node_ids),
                }
            )

        async def _deploy_to_node(node_id: str) -> dict:
            """Push config + schedule from product profile to a single node."""
            node = node_reg.get(node_id)
            if not node:
                return {"node_id": node_id, "ok": False, "error": "node not found"}
            if not node.address:
                return {
                    "node_id": node_id,
                    "node_name": node.name,
                    "ok": False,
                    "error": f"no HTTP address (type={node.type})",
                }

            errors: list[str] = []

            # ── Step 1: Apply config file ──────────────────────────────────
            config_file: str = product.get("config_file", "")
            if config_file:
                cfg_path = CONFIGS_DIR / config_file
                if cfg_path.exists():
                    try:
                        cfg_data = json.loads(cfg_path.read_text())
                        ok = await _proxy_post(
                            node.address, "/api/config/apply", cfg_data
                        )
                        if not ok:
                            errors.append(f"config apply failed ({config_file})")
                    except Exception as e:
                        errors.append(f"config error: {e}")
                else:
                    errors.append(f"config file '{config_file}' not found in configs/")

            # ── Step 2: Apply schedule file ────────────────────────────────
            schedule_file: str = product.get("schedule_file", "")
            if schedule_file:
                sched_path = CONFIGS_DIR / schedule_file
                if sched_path.exists():
                    try:
                        sched_text = sched_path.read_text()
                        sched_data = json.loads(sched_text)
                        # Accept either a bare list or {"tasks": [...]}
                        tasks = (
                            sched_data
                            if isinstance(sched_data, list)
                            else sched_data.get("tasks", [])
                        )
                        # Clear existing tasks first
                        await _send_cmd_to_ip(node.address, "SCHED CLEAR")
                        await asyncio.sleep(0.05)
                        for t in tasks:
                            t_name = t.get("name", "task")
                            t_type = t.get("type", "TOGGLE")
                            pin = t.get("pin", 35)
                            interval = t.get("interval", 10)
                            duration = t.get("duration", 0)
                            dur_part = f" {duration}" if duration else ""
                            cmd = f"SCHED ADD {t_name} {t_type} {pin} {interval}{dur_part}"
                            ok = await _send_cmd_to_ip(node.address, cmd)
                            if not ok:
                                errors.append(f"sched task '{t_name}' failed")
                            await asyncio.sleep(0.1)
                        await _send_cmd_to_ip(node.address, "SCHED SAVE")
                    except Exception as e:
                        errors.append(f"schedule error: {e}")
                else:
                    errors.append(
                        f"schedule file '{schedule_file}' not found in configs/"
                    )

            node_ok = len(errors) == 0
            result: dict = {"node_id": node_id, "node_name": node.name, "ok": node_ok}
            if errors:
                result["error"] = "; ".join(errors)
            return result

        # ── Dispatch ALL nodes simultaneously — no sequential blocking ─────
        raw_results = await asyncio.gather(
            *[_deploy_to_node(nid) for nid in node_ids],
            return_exceptions=True,
        )

        # Normalise any unexpected exceptions bubbled from gather
        results: list[dict] = []
        for r in raw_results:
            if isinstance(r, Exception):
                results.append({"ok": False, "error": str(r)})
            else:
                results.append(r)

        ok_count = sum(1 for r in results if r.get("ok"))
        fail_count = len(results) - ok_count
        return JSONResponse({"results": results, "ok": ok_count, "fail": fail_count})

    # ── Config files ──────────────────────────────────────────────────────

    @app.get("/api/files")
    async def _files_list() -> JSONResponse:
        files = []
        for p in sorted(CONFIGS_DIR.iterdir()):
            if p.suffix.lower() in (".json", ".csv") and p.is_file():
                files.append({"name": p.name, "size": p.stat().st_size})
        return JSONResponse({"files": files})

    @app.get("/api/files/{filename}")
    async def _files_get(filename: str) -> PlainTextResponse:
        p = CONFIGS_DIR / filename
        if not p.exists() or p.suffix.lower() not in (".json", ".csv"):
            return PlainTextResponse("Not found", status_code=404)
        async with aiofiles.open(p, "r") as f:
            content = await f.read()
        return PlainTextResponse(content)

    def _assert_local_origin(request: Request) -> None:
        """Reject cross-origin mutation requests (CSRF guard).
        Allows: no Origin (same-origin fetch/form), localhost, and RFC-1918 ranges.
        Blocks: requests with an Origin that doesn't match a trusted local prefix."""
        origin = request.headers.get("origin", "")
        if not origin:
            return  # same-origin or non-browser client — allow
        trusted = ("localhost", "127.", "172.16.", "192.168.", "10.")
        if not any(t in origin for t in trusted):
            raise HTTPException(status_code=403, detail="Cross-origin request blocked")

    @app.post("/api/files/{filename}")
    async def _files_save(filename: str, body: dict, request: Request) -> JSONResponse:
        _assert_local_origin(request)
        if not filename.endswith((".json", ".csv")):
            return JSONResponse(
                {"ok": False, "error": "Only .json and .csv allowed"}, status_code=400
            )
        content = body.get("content", "")
        p = CONFIGS_DIR / filename
        async with aiofiles.open(p, "w") as f:
            await f.write(content)
        return JSONResponse({"ok": True, "name": filename, "size": len(content)})

    @app.delete("/api/files/{filename}")
    async def _files_delete(filename: str, request: Request) -> JSONResponse:
        _assert_local_origin(request)
        p = CONFIGS_DIR / filename
        if not p.exists():
            return JSONResponse({"ok": False, "error": "Not found"}, status_code=404)
        p.unlink()
        return JSONResponse({"ok": True})

    # ── Device Side Config/Files ──────────────────────────────────────────

    @app.get("/api/device/config")
    async def _device_config_get() -> JSONResponse:
        content = await _proxy_get(_effective_ip(), "/api/config")
        return JSONResponse(content or {"error": "Device unreachable"})

    @app.post("/api/device/config/apply")
    async def _device_config_apply(body: dict) -> PlainTextResponse:
        # body is the full JSON config
        ip = _effective_ip()

        # Try HTTP first (device proxy)
        ok = await _proxy_post(ip, "/api/config/apply", body)
        if ok:
            return PlainTextResponse("OK", status_code=200)

        # HTTP failed — provide helpful feedback about available transports
        transports_available = []
        if _transport:
            if state.ble_ok or (len(_transport._peers) > 0):
                transports_available.append("BLE")
            if state.serial_ok:
                transports_available.append("Serial")
            if state.espnow_ok:
                transports_available.append("ESP-NOW")

        msg = f"Device not reachable via HTTP at {ip}."
        if transports_available:
            msg += f" Available transports: {', '.join(transports_available)}. Use command console to apply config."
        else:
            msg += " No alternative transports available. Make sure device is powered and in range."

        return PlainTextResponse(msg, status_code=502)

    @app.get("/api/device/files")
    async def _device_files_list() -> JSONResponse:
        content = await _proxy_get(_effective_ip(), "/api/files/list")
        return JSONResponse(content or {"files": []})

    @app.post("/api/pins/enable")
    async def _pin_enable(body: dict) -> PlainTextResponse:
        ok = await _proxy_post(_effective_ip(), "/api/pins/enable", body)
        return PlainTextResponse("OK" if ok else "ERR", status_code=200 if ok else 502)

    @app.post("/api/pins/name")
    async def _pin_name(body: dict) -> PlainTextResponse:
        ok = await _proxy_post(_effective_ip(), "/api/pins/name", body)
        return PlainTextResponse("OK" if ok else "ERR", status_code=200 if ok else 502)

    @app.post("/api/transport/mode")
    async def _transport_mode(body: dict) -> PlainTextResponse:
        ok = await _proxy_post(_effective_ip(), "/api/transport/mode", body)
        return PlainTextResponse("OK" if ok else "ERR", status_code=200 if ok else 502)

    @app.get("/api/registry")
    async def _registry() -> JSONResponse:
        data = await _proxy_get(_effective_ip(), "/api/registry")
        return JSONResponse(data or {})

    @app.get("/api/device/files/read")
    async def _device_file_read(path: str) -> PlainTextResponse:
        if not device_ip:
            return PlainTextResponse("No device IP", status_code=503)
        try:
            async with aiohttp.ClientSession() as s:
                async with s.get(f"http://{device_ip}/api/files/read?path={path}") as r:
                    content = await r.text()
                    return PlainTextResponse(content, status_code=r.status)
        except Exception as e:
            return PlainTextResponse(str(e), status_code=500)

    def find_log_path(dev_id):
        # 1. Look for the most recent session log for this ID
        logs = list(LOGS_DIR.glob(f"{dev_id}_*_trace.log"))
        if logs:
            return max(logs, key=lambda p: p.stat().st_mtime)

        # 2. Fallback to generic name (backwards compatibility or manual renames)
        p = LOGS_DIR / f"{dev_id}_trace.log"
        if p.exists():
            return p

        # 3. Match by node name from registry
        node = node_reg.get(dev_id)
        if node:
            logs = list(LOGS_DIR.glob(f"{node.name}_*_trace.log"))
            if logs:
                return max(logs, key=lambda p: p.stat().st_mtime)
            p = LOGS_DIR / f"{node.name}_trace.log"
            if p.exists():
                return p

        # 4. Final grep-style match
        all_logs = list(LOGS_DIR.glob("*_trace.log"))
        matches = [f for f in all_logs if dev_id in f.name]
        if matches:
            return max(matches, key=lambda p: p.stat().st_mtime)

        return None

    @app.get("/api/analysis/trace")
    async def get_test_trace(device_id: str):
        log_file = find_log_path(device_id)
        if not log_file or not log_file.exists():
            return PlainTextResponse("")
        async with aiofiles.open(log_file, mode="r", encoding="utf-8") as f:
            content = await f.read()
            return PlainTextResponse(content)

    @app.get("/api/analysis/suites")
    async def get_test_suites():
        if not SUITES_FILE.exists():
            return JSONResponse([])
        async with aiofiles.open(SUITES_FILE, mode="r", encoding="utf-8") as f:
            content = await f.read()
            return JSONResponse(json.loads(content))

    @app.post("/api/analysis/suites")
    async def save_test_suite(suite: dict):
        suites = []
        if SUITES_FILE.exists():
            async with aiofiles.open(SUITES_FILE, mode="r", encoding="utf-8") as f:
                content = await f.read()
                suites = json.loads(content)

        # Update if exists, else append
        idx = next((i for i, s in enumerate(suites) if s["name"] == suite["name"]), -1)
        if idx >= 0:
            suites[idx] = suite
        else:
            suites.append(suite)

        async with aiofiles.open(SUITES_FILE, mode="w", encoding="utf-8") as f:
            await f.write(json.dumps(suites, indent=2))
        return JSONResponse({"ok": True})

    @app.post("/api/analysis/verify")
    async def verify_test_suite(body: dict):
        """Run automated regex validation on local traces, non-blocking."""
        try:
            suite_name = body.get("suite")
            dev_a_id = body.get("device_a")
            dev_b_id = body.get("device_b")

            if not suite_name or not dev_a_id or not dev_b_id:
                return JSONResponse(
                    {"ok": False, "error": "Missing suite, device_a, or device_b"},
                    status_code=400,
                )

            # Get suite expectations (handle chaining)
            suites = []
            if SUITES_FILE.exists():
                async with aiofiles.open(SUITES_FILE, "r", encoding="utf-8") as f:
                    content = await f.read()
                    if content.strip():
                        suites = json.loads(content)

            def get_all_expectations(name, seen=None):
                if seen is None:
                    seen = set()
                if name in seen:
                    return []  # Prevent recursion
                seen.add(name)

                s = next((suite for suite in suites if suite["name"] == name), None)
                if not s:
                    return []

                all_exps = []
                # If this is a MetaSuite (has chaining)
                if "chain" in s:
                    for chained_name in s["chain"]:
                        all_exps.extend(get_all_expectations(chained_name, seen))

                # Add local expectations
                if "expectations" in s:
                    all_exps.extend(s["expectations"])
                return all_exps

            expectations = get_all_expectations(suite_name)
            if not expectations:
                return JSONResponse(
                    {
                        "ok": False,
                        "error": f"Suite '{suite_name}' not found or has no expectations",
                    },
                    status_code=404,
                )

            # Read traces
            log_a_path = find_log_path(dev_a_id)
            log_b_path = find_log_path(dev_b_id)

            if not log_a_path or not log_b_path:
                msg = f"Trace logs not found for {dev_a_id} and/or {dev_b_id}. "
                if not log_a_path:
                    msg += "Missing Log A. "
                if not log_b_path:
                    msg += "Missing Log B. "
                return JSONResponse({"ok": False, "error": msg}, status_code=400)

            async with aiofiles.open(log_a_path, "r", encoding="utf-8") as f:
                log_a = await f.read()
            async with aiofiles.open(log_b_path, "r", encoding="utf-8") as f:
                log_b = await f.read()

            if not log_a.strip() or not log_b.strip():
                return JSONResponse(
                    {"ok": False, "error": "One or both trace logs are empty."},
                    status_code=400,
                )

            # Run validation
            results = []
            errors = 0

            for exp in expectations:
                device_log = log_a if exp["device"] == "A" else log_b
                pattern = re.compile(rf"{exp['type']}:\s*.*{re.escape(exp['data'])}")
                match = pattern.search(device_log)

                step_result = {
                    "device": exp["device"],
                    "type": exp["type"],
                    "data": exp["data"],
                    "pass": bool(match),
                }
                results.append(step_result)
                if not match:
                    errors += 1

            ts = datetime.now()
            report = {
                "timestamp": ts.isoformat(),
                "date": ts.strftime("%Y-%m-%d"),
                "time": ts.strftime("%H:%M:%S"),
                "version": "v1.5.0-2026.03.03.23",
                "suite": suite_name,
                "device_a": dev_a_id,
                "device_b": dev_b_id,
                "total_steps": len(expectations),
                "errors": errors,
                "passed": errors == 0,
                "details": results,
            }

            safe_name = suite_name.replace(" ", "_").replace("/", "_")
            report_filename = (
                f"verification_{safe_name}_{ts.strftime('%Y%m%d_%H%M%S')}.json"
            )

            async with aiofiles.open(
                LOGS_DIR / report_filename, "w", encoding="utf-8"
            ) as f:
                await f.write(json.dumps(report, indent=2))

            return JSONResponse(
                {"ok": True, "report": report, "filename": report_filename}
            )
        except Exception as e:
            return JSONResponse(
                {"ok": False, "error": f"Internal Server Error: {str(e)}"},
                status_code=500,
            )

    return app


# ════════════════════════════════════════════════════════════════════════════
# 7. CLI entry point
# ════════════════════════════════════════════════════════════════════════════


def main() -> None:
    parser = argparse.ArgumentParser(
        description="LoRaLink PC Control Webapp",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--device",
        default=None,
        metavar="PREFIX",
        help="BLE name prefix for peer A  (default: read from .settings.json, then 'LoRaLink')",
    )
    parser.add_argument(
        "--device2",
        default=None,
        metavar="PREFIX2",
        help="BLE name prefix for peer B  (optional; omit for single-device mode)",
    )
    parser.add_argument(
        "--ip",
        default=None,
        metavar="ADDRESS",
        help="Device IP for HTTP transport  (optional)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=None,
        metavar="PORT",
        help="Local port to serve on  (default: $PORT env var, then 8000)",
    )
    parser.add_argument(
        "--no-ble",
        action="store_true",
        help="Disable BLE scanning entirely (HTTP-only mode)",
    )
    parser.add_argument(
        "--host",
        default="0.0.0.0",
        metavar="HOST",
        help="Host address to listen on (default: 0.0.0.0 for external/LAN access)",
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Enable uvicorn debug/reload mode",
    )
    args = parser.parse_args()

    # Honour PORT env var injected by preview tools / CI; CLI flag overrides it.
    port = args.port or int(os.environ.get("PORT", 8000))
    host = args.host

    # Resolve DNS hostname to IP if provided
    device_ip = args.ip
    if device_ip and "." not in device_ip:  # Likely a hostname (no dots)
        try:
            import socket

            resolved_ip = socket.gethostbyname(device_ip)
            print(f"[DNS] Resolved '{device_ip}' → {resolved_ip}")
            device_ip = resolved_ip
        except Exception as e:
            print(f"[DNS] Failed to resolve '{device_ip}': {e}")
            print("[DNS] Will retry during operation...")

    # Load BLE prefixes from .settings.json if not provided on CLI
    _saved_settings: dict = {}
    if SETTINGS_FILE.exists():
        try:
            _saved_settings = json.loads(SETTINGS_FILE.read_text())
        except Exception:
            pass
    ble_a = args.device or _saved_settings.get("ble_prefix_a", "LoRaLink")
    ble_b = args.device2 or _saved_settings.get("ble_prefix_b") or None

    print("LoRaLink PC Control Webapp")
    print(f"  Peer A     : {ble_a}")
    print(f"  Peer B     : {ble_b or '(single-device mode)'}")
    print(f"  Device IP  : {device_ip or 'not set (BLE only)'}")
    print(
        f"  BLE        : {'disabled (--no-ble)' if args.no_ble else 'enabled (background scan)'}"
    )
    print(f"  Serving at : http://{host}:{port}")
    print()

    app = build_app(
        device_name=ble_a,
        device_ip=device_ip,
        device_name2=ble_b,
        no_ble=args.no_ble,
        debug=args.debug,
    )
    uvicorn.run(app, host=host, port=port, log_level="warning")


if __name__ == "__main__":
    main()
