#!/usr/bin/env python3
"""
Simulador do Gateway (grupo B) e do Servidor (grupo C) do Projeto Final.

Sobe dois servidores HTTP no mesmo processo:
  - Gateway : porta 5500  -> GET /sensors, GET /health, POST /reset
  - Servidor: porta 5501  -> GET /health, GET /data, GET /log?node=X&n=N, POST /reset

O Gateway simula N nos sensores transmitindo por radio (luz, temperatura,
aceleracao, seq, uptime, flags). O Servidor consulta o Gateway periodicamente
via HTTP e grava CSV em /data (o "cartao SD").

Extras fora do contrato, para testar o Supervisor (todos sob /_sim/):
  GET  /_sim/state
  POST /_sim/fault?mode=hang|error|close|none&seconds=20
  POST /_sim/node?id=2&state=offline|online

Sem dependencias externas: so biblioteca padrao do Python 3.
"""

import json
import math
import os
import random
import threading
import time
import urllib.error
import urllib.request
from collections import deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# ----------------------------------------------------------------------------
# Configuracao (tudo sobrescrivel por variavel de ambiente)
# ----------------------------------------------------------------------------

GATEWAY_PORT = int(os.environ.get("GATEWAY_PORT", "5500"))
SERVER_PORT = int(os.environ.get("SERVER_PORT", "5501"))
NODE_IDS = [int(x) for x in os.environ.get("NODE_IDS", "1,2,3").split(",")]
RADIO_PERIOD_S = float(os.environ.get("RADIO_PERIOD_S", "0.5"))
SERVER_POLL_S = float(os.environ.get("SERVER_POLL_S", "1.0"))
RESET_DOWNTIME_S = float(os.environ.get("RESET_DOWNTIME_S", "3.0"))
NODE_ONLINE_MS = int(os.environ.get("NODE_ONLINE_MS", "5000"))
CSV_PATH = os.environ.get("CSV_PATH", "/data/log.csv")
HISTORY_LEN = int(os.environ.get("HISTORY_LEN", "500"))


def now_ms() -> int:
    return int(time.monotonic() * 1000)


# ----------------------------------------------------------------------------
# Estado de "saude" de um subsistema: uptime, reset, injecao de falhas
# ----------------------------------------------------------------------------


class Subsystem:
    """Estado comum ao gateway e ao servidor: uptime, reset e falhas."""

    def __init__(self, name):
        self.name = name
        self.lock = threading.Lock()
        self.boot = time.monotonic()
        self.reset_count = 0
        self.down_until = 0.0
        self.fault_mode = None
        self.fault_until = 0.0

    def uptime_ms(self):
        return int((time.monotonic() - self.boot) * 1000)

    def reset(self):
        """Simula o reboot: fica inacessivel por RESET_DOWNTIME_S e zera o uptime."""
        with self.lock:
            self.reset_count += 1
            self.down_until = time.monotonic() + RESET_DOWNTIME_S
            self.fault_mode = None
            self.fault_until = 0.0

        def _come_back():
            time.sleep(RESET_DOWNTIME_S)
            self.boot = time.monotonic()
            log(self.name, "reboot concluido, uptime zerado")

        threading.Thread(target=_come_back, daemon=True).start()
        log(self.name, f"RESET pedido -> fora do ar por {RESET_DOWNTIME_S:.0f}s")

    def inject(self, mode, seconds):
        with self.lock:
            if mode in (None, "none"):
                self.fault_mode = None
                self.fault_until = 0.0
                log(self.name, "falha injetada cancelada")
            else:
                self.fault_mode = mode
                self.fault_until = time.monotonic() + seconds
                log(self.name, f"falha injetada: {mode} por {seconds:.0f}s")

    def active_fault(self):
        """Retorna 'close', 'hang', 'error' ou None."""
        now = time.monotonic()
        with self.lock:
            if now < self.down_until:
                return "close"  # rebootando: recusa conexao
            if self.fault_mode and now < self.fault_until:
                return self.fault_mode
            if self.fault_mode:
                self.fault_mode = None
        return None

    def sim_state(self):
        fault = self.active_fault()
        return {
            "name": self.name,
            "uptime_ms": self.uptime_ms(),
            "reset_count": self.reset_count,
            "fault": fault,
            "rebooting": time.monotonic() < self.down_until,
        }


def log(who, msg):
    ts = datetime.now().strftime("%H:%M:%S")
    print(f"[{ts}] [{who}] {msg}", flush=True)


# ----------------------------------------------------------------------------
# Nos sensores simulados (grupo A) - vivem "dentro" do gateway
# ----------------------------------------------------------------------------


class SensorNode:
    """Gera leituras plausiveis de luz, temperatura e aceleracao."""

    FLAG_LOW_BATTERY = 0x01
    FLAG_LOCAL_LOG = 0x02

    def __init__(self, node_id):
        self.node_id = node_id
        self.seq = 0
        self.online = True
        self.boot = time.monotonic()
        self.phase = random.random() * math.tau
        self.last_seen = time.monotonic()
        self.light = 300
        self.temp_c = 24.5
        self.accel = [0.0, 0.0, 9.80]
        self.flags = self.FLAG_LOCAL_LOG if node_id == 1 else 0

    def tick(self):
        """Um "pacote de radio" recebido pelo gateway."""
        if not self.online:
            return
        t = time.monotonic()
        self.seq = (self.seq + 1) & 0xFFFF
        # luz: ciclo lento + ruido, saturado em 0..1023 (ADC de 10 bits)
        self.light = int(max(0, min(1023, 420 + 260 * math.sin(t / 9 + self.phase) + random.gauss(0, 10))))
        # temperatura: deriva lenta em torno de 24 C
        self.temp_c = round(24.0 + 2.0 * math.sin(t / 37 + self.phase) + random.gauss(0, 0.05), 2)
        # aceleracao: parado, gravidade em Z, com ruido
        self.accel = [
            round(random.gauss(0.0, 0.03), 3),
            round(random.gauss(0.0, 0.03), 3),
            round(9.80 + random.gauss(0.0, 0.03), 3),
        ]
        # no 3 entra em bateria baixa de vez em quando
        if self.node_id == 3 and int(t) % 60 > 40:
            self.flags |= self.FLAG_LOW_BATTERY
        else:
            self.flags &= ~self.FLAG_LOW_BATTERY
        self.last_seen = t

    def uptime_ms(self):
        return int((time.monotonic() - self.boot) * 1000)

    def last_seen_ms(self):
        return int((time.monotonic() - self.last_seen) * 1000)

    def reading(self):
        """Formato do contrato: mesmo JSON de /sensors e /data."""
        return {
            "node_id": self.node_id,
            "seq": self.seq,
            "light": self.light,
            "temp_c": self.temp_c,
            "accel": self.accel,
            "uptime_ms": self.uptime_ms(),
            "flags": self.flags,
            "last_seen_ms": self.last_seen_ms(),
        }


class GatewayState(Subsystem):
    def __init__(self):
        super().__init__("gateway")
        self.nodes = {nid: SensorNode(nid) for nid in NODE_IDS}
        threading.Thread(target=self._radio_loop, daemon=True).start()

    def _radio_loop(self):
        while True:
            for node in self.nodes.values():
                node.tick()
            time.sleep(RADIO_PERIOD_S)

    def sensors(self):
        return [n.reading() for n in self.nodes.values()]

    def nodes_online(self):
        return sum(1 for n in self.nodes.values() if n.last_seen_ms() < NODE_ONLINE_MS)

    def set_node(self, node_id, online):
        node = self.nodes.get(node_id)
        if node is None:
            return False
        node.online = online
        log("gateway", f"no {node_id} -> {'online' if online else 'offline'}")
        return True


# ----------------------------------------------------------------------------
# Servidor / datalogger (grupo C)
# ----------------------------------------------------------------------------


class ServerState(Subsystem):
    CSV_HEADER = "ts_iso,ts_ms,node_id,seq,light,temp_c,accel_x,accel_y,accel_z,flags\n"

    def __init__(self, gateway_url):
        super().__init__("servidor")
        self.gateway_url = gateway_url
        self.latest = {}
        self.history = {}
        self.records = 0
        self.last_poll_ok = False
        self.last_poll_ms = None
        self.poll_errors = 0
        self._init_csv()
        threading.Thread(target=self._poll_loop, daemon=True).start()

    def _init_csv(self):
        try:
            os.makedirs(os.path.dirname(CSV_PATH) or ".", exist_ok=True)
            if not os.path.exists(CSV_PATH):
                with open(CSV_PATH, "w") as f:
                    f.write(self.CSV_HEADER)
        except OSError as exc:
            log("servidor", f"aviso: nao consegui abrir o CSV ({exc})")

    def _poll_loop(self):
        while True:
            time.sleep(SERVER_POLL_S)
            if time.monotonic() < self.down_until:
                continue  # rebootando
            try:
                req = urllib.request.Request(self.gateway_url + "/sensors")
                with urllib.request.urlopen(req, timeout=2.0) as resp:
                    payload = json.loads(resp.read().decode())
                self._store(payload)
                if not self.last_poll_ok:
                    log("servidor", "gateway respondendo de novo")
                self.last_poll_ok = True
                self.last_poll_ms = now_ms()
            except (urllib.error.URLError, OSError, ValueError, TimeoutError) as exc:
                self.poll_errors += 1
                if self.last_poll_ok:
                    log("servidor", f"falha ao consultar o gateway: {exc}")
                self.last_poll_ok = False

    def _store(self, readings):
        ts_ms = now_ms()
        ts_iso = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
        lines = []
        for r in readings:
            nid = r["node_id"]
            self.latest[nid] = r
            entry = {
                "ts_ms": ts_ms,
                "ts_iso": ts_iso,
                "seq": r.get("seq"),
                "light": r.get("light"),
                "temp_c": r.get("temp_c"),
                "accel": r.get("accel"),
                "flags": r.get("flags"),
            }
            self.history.setdefault(nid, deque(maxlen=HISTORY_LEN)).append(entry)
            ax, ay, az = r.get("accel", [0, 0, 0])
            lines.append(
                f"{ts_iso},{ts_ms},{nid},{r.get('seq')},{r.get('light')},"
                f"{r.get('temp_c')},{ax},{ay},{az},{r.get('flags')}\n"
            )
            self.records += 1
        try:
            with open(CSV_PATH, "a") as f:
                f.writelines(lines)
        except OSError:
            pass

    def data(self):
        return list(self.latest.values())

    def log_for(self, node_id, n):
        entries = list(self.history.get(node_id, []))[-n:]
        return {"node_id": node_id, "n": len(entries), "entries": entries}


# ----------------------------------------------------------------------------
# HTTP
# ----------------------------------------------------------------------------


class BaseHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    state = None  # preenchido nas subclasses

    # --- utilidades ---------------------------------------------------------

    def log_message(self, fmt, *args):
        log(self.state.name, f"{self.address_string()} {fmt % args}")

    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def qs_int(self, q, key, default):
        try:
            return int(q.get(key, [default])[0])
        except (TypeError, ValueError):
            return default

    # --- despacho -----------------------------------------------------------

    def do_GET(self):
        self.dispatch("GET")

    def do_POST(self):
        self.dispatch("POST")

    def dispatch(self, method):
        parsed = urlparse(self.path)
        path = parsed.path.rstrip("/") or "/"
        query = parse_qs(parsed.query)

        if path.startswith("/_sim"):
            return self.handle_sim(method, path, query)

        fault = self.state.active_fault()
        if fault == "close":
            # placa rebootando / cabo caido: derruba a conexao sem responder
            self.close_connection = True
            try:
                self.connection.close()
            except OSError:
                pass
            return
        if fault == "hang":
            # trava: o cliente vai estourar o timeout
            time.sleep(120)
            return
        if fault == "error":
            return self.reply(500, {"status": "error", "detail": "falha simulada"})

        try:
            self.route(method, path, query)
        except Exception as exc:  # noqa: BLE001
            self.reply(500, {"status": "error", "detail": str(exc)})

    def route(self, method, path, query):
        self.reply(404, {"status": "error", "detail": "rota inexistente", "path": path})

    # --- controle do simulador (fora do contrato) ---------------------------

    def handle_sim(self, method, path, query):
        if path == "/_sim/state":
            return self.reply(200, self.sim_state())
        if path == "/_sim/fault" and method == "POST":
            mode = query.get("mode", ["none"])[0]
            if mode not in ("hang", "error", "close", "none"):
                return self.reply(400, {"status": "error", "detail": "mode invalido"})
            try:
                seconds = float(query.get("seconds", ["20"])[0])
            except ValueError:
                return self.reply(400, {"status": "error", "detail": "seconds invalido"})
            self.state.inject(mode, seconds)
            return self.reply(200, {"status": "ok", "mode": mode, "seconds": seconds})
        return self.reply(404, {"status": "error", "detail": "rota de simulacao inexistente"})

    def sim_state(self):
        return self.state.sim_state()


class GatewayHandler(BaseHandler):
    def route(self, method, path, query):
        st = self.state
        if method == "GET" and path == "/sensors":
            return self.reply(200, st.sensors())
        if method == "GET" and path == "/health":
            return self.reply(
                200,
                {
                    "status": "ok",
                    "uptime_ms": st.uptime_ms(),
                    "nodes_online": st.nodes_online(),
                    "nodes_known": len(st.nodes),
                    "reset_count": st.reset_count,
                },
            )
        if method == "POST" and path == "/reset":
            self.reply(200, {"status": "resetting"})
            st.reset()
            return
        return super().route(method, path, query)

    def handle_sim(self, method, path, query):
        if path == "/_sim/node" and method == "POST":
            node_id = self.qs_int(query, "id", -1)
            state = query.get("state", ["online"])[0]
            if state not in ("online", "offline"):
                return self.reply(400, {"status": "error", "detail": "state invalido"})
            ok = self.state.set_node(node_id, state == "online")
            if not ok:
                return self.reply(404, {"status": "error", "detail": "no inexistente"})
            return self.reply(200, {"status": "ok", "node_id": node_id, "state": state})
        return super().handle_sim(method, path, query)

    def sim_state(self):
        base = super().sim_state()
        base["nodes"] = {n.node_id: ("online" if n.online else "offline") for n in self.state.nodes.values()}
        return base


class ServerHandler(BaseHandler):
    def route(self, method, path, query):
        st = self.state
        if method == "GET" and path == "/health":
            return self.reply(
                200,
                {
                    "status": "ok" if st.last_poll_ok else "degraded",
                    "uptime_ms": st.uptime_ms(),
                    "records": st.records,
                    "nodes_known": len(st.latest),
                    "gateway_ok": st.last_poll_ok,
                    "last_poll_ms": st.last_poll_ms,
                    "poll_errors": st.poll_errors,
                    "reset_count": st.reset_count,
                },
            )
        if method == "GET" and path == "/data":
            return self.reply(200, st.data())
        if method == "GET" and path == "/log":
            node_id = self.qs_int(query, "node", NODE_IDS[0])
            n = max(1, min(HISTORY_LEN, self.qs_int(query, "n", 10)))
            return self.reply(200, st.log_for(node_id, n))
        if method == "POST" and path == "/reset":
            self.reply(200, {"status": "resetting"})
            st.reset()
            return
        return super().route(method, path, query)


def serve(port, handler_cls, state):
    cls = type(handler_cls.__name__ + "Bound", (handler_cls,), {"state": state})
    httpd = ThreadingHTTPServer(("0.0.0.0", port), cls)
    httpd.daemon_threads = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    log(state.name, f"escutando em 0.0.0.0:{port}")


def main():
    random.seed()
    gateway = GatewayState()
    server = ServerState(f"http://127.0.0.1:{GATEWAY_PORT}")

    serve(GATEWAY_PORT, GatewayHandler, gateway)
    serve(SERVER_PORT, ServerHandler, server)

    log("sim", f"nos simulados: {NODE_IDS} | CSV em {CSV_PATH}")
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        log("sim", "encerrando")


if __name__ == "__main__":
    main()
