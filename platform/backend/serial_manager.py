#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Gerenciador Exclusivo de Portas Seriais Windows (TX + RX Simultâneos)
======================================================================
- Lista portas COM reais via pyserial.tools.list_ports.
- Bloqueia estritamente abertura simultânea da mesma COM para TX e RX.
- Identifica cada placa via /id e gerencia handshake /telem 1 / /telem 0.
- Valida comandos enviados contra a lista exata suportada pelos firmwares C.
- Registra desconexões físicas e erros de acesso (porta ocupada por outro app).
"""

import re
import threading
import time
from typing import Any, Callable, Dict, List, Optional, Set
import serial
import serial.tools.list_ports

from serial_protocol import HybridSerialParser

# Lista exata de comandos confirmados em transmitter/main/uart_cli.c
TX_ALLOWED_COMMANDS: Set[str] = {
    "/help", "/status", "/vectors", "/selftest", "/id", "/telem", "/cap",
    "/reset", "/mode", "/fc", "/baud", "/fs", "/pwm", "/amp", "/offset",
    "/order", "/map", "/qsign", "/pol", "/pulse", "/idle", "/pre1",
    "/gpio", "/verbose", "/test", "/send",
}

# Lista exata de comandos confirmados em receiver/main/rx_cli.c
RX_ALLOWED_COMMANDS: Set[str] = {
    "/help", "/status", "/selftest", "/id", "/telem", "/cap", "/adcstream",
    "/reset", "/mode", "/fc", "/baud", "/fs", "/ch", "/map", "/qsign",
    "/order", "/pol", "/rot", "/sync1", "/squelch", "/monitor",
}


def list_com_ports() -> List[Dict[str, str]]:
    ports = []
    for p in serial.tools.list_ports.comports():
        ports.append({
            "device": p.device,
            "description": p.description or p.device,
            "hwid": p.hwid or "",
            "manufacturer": p.manufacturer or "",
        })
    ports.sort(key=lambda x: x["device"])
    return ports


def validate_firmware_command(role: str, raw_line: str) -> Optional[str]:
    """
    Garante que apenas comandos reais existentes no firmware sejam enviados.
    Retorna None se válido, ou mensagem de erro explicativa.
    """
    cmd_line = raw_line.strip()
    if not cmd_line:
        return "Comando vazio."
    if role == "tx" and not cmd_line.startswith("/"):
        return None  # Texto ASCII direto é transmitido pelo TX

    head = cmd_line.split()[0]
    allowed = TX_ALLOWED_COMMANDS if role == "tx" else RX_ALLOWED_COMMANDS
    if head not in allowed:
        return f"Comando '{head}' não existe no firmware {role.upper()}."
    return None


class DeviceConnection:
    """Conexão individual (não bloqueante) para TX ou RX com parser híbrido."""

    def __init__(self, role: str,
                 on_event: Callable[[str, Dict[str, Any]], None],
                 on_cli: Callable[[str, str], None],
                 on_state_change: Callable[[str, Dict[str, Any]], None]):
        self.role = role
        self.on_event = on_event
        self.on_cli = on_cli
        self.on_state_change = on_state_change

        self._lock = threading.Lock()
        self._serial: Optional[serial.Serial] = None
        self._thread: Optional[threading.Thread] = None
        self._stop_ev = threading.Event()

        self.port: Optional[str] = None
        self.baudrate: int = 115200
        self.status: str = "disconnected"  # disconnected | connecting | connected | error
        self.identified: bool = False
        self.detected_role: Optional[str] = None
        self.last_error: Optional[str] = None
        self.last_rx_ts: float = 0.0
        self.command_log: List[Dict[str, Any]] = []

        self.parser = HybridSerialParser(
            on_packet=self._handle_packet,
            on_cli_line=self._handle_cli,
        )

    def _handle_packet(self, pkt: Dict[str, Any]) -> None:
        self.last_rx_ts = time.time()
        ev = pkt.get("event", "")
        if ev in ("tx_hello", "rx_hello"):
            self.identified = True
            self.detected_role = pkt.get("role")
        self.on_event(self.role, pkt)

    def _handle_cli(self, line: str) -> None:
        self.last_rx_ts = time.time()
        m = re.search(r"ID:\s*ROLE=(TX|RX)", line)
        if m:
            self.identified = True
            self.detected_role = m.group(1).lower()
        self.on_cli(self.role, line)

    def snapshot_status(self) -> Dict[str, Any]:
        return {
            "role": self.role,
            "port": self.port,
            "baudrate": self.baudrate,
            "status": self.status,
            "identified": self.identified,
            "detected_role": self.detected_role,
            "last_error": self.last_error,
            "last_rx_ts": self.last_rx_ts,
            "stats": self.parser.stats.to_dict(),
            "recent_commands": self.command_log[-15:],
        }

    def connect(self, port: str, baudrate: int = 115200) -> Dict[str, Any]:
        self.disconnect()
        self.port = port
        self.baudrate = baudrate
        self.status = "connecting"
        self.identified = False
        self.detected_role = None
        self.last_error = None
        self.parser = HybridSerialParser(
            on_packet=self._handle_packet,
            on_cli_line=self._handle_cli,
        )

        try:
            ser = serial.Serial(
                port=port,
                baudrate=baudrate,
                timeout=0.05,
                write_timeout=0.5,
            )
            # Evita reset indesejado quando possível no Windows USB-UART; lê backlog breve
            ser.reset_input_buffer()
        except serial.SerialException as exc:
            msg = str(exc)
            if "PermissionError" in msg or "Access is denied" in msg or "Acesso negado" in msg:
                self.last_error = f"Porta {port} ocupada por outro monitor serial ou programa."
            else:
                self.last_error = f"Falha ao abrir {port}: {msg}"
            self.status = "error"
            self.on_state_change(self.role, self.snapshot_status())
            return {"ok": False, "error": self.last_error}

        with self._lock:
            self._serial = ser
            self.status = "connected"
            self._stop_ev.clear()
            self._thread = threading.Thread(target=self._reader_loop, name=f"serial_{self.role}", daemon=True)
            self._thread.start()

        self.on_state_change(self.role, self.snapshot_status())
        # Solicita identificação e inicia telemetria binária v1
        time.sleep(0.12)
        self.send_line("/id", source="auto_handshake")
        self.send_line("/telem 1", source="auto_handshake")
        return {"ok": True, "status": self.snapshot_status()}

    def disconnect(self) -> Dict[str, Any]:
        with self._lock:
            ser = self._serial
        if ser and ser.is_open:
            try:
                ser.write(b"/telem 0\r")
                ser.flush()
            except Exception:
                pass

        self._stop_ev.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=0.6)

        with self._lock:
            if self._serial:
                try:
                    self._serial.close()
                except Exception:
                    pass
                self._serial = None
            self._thread = None
            self.status = "disconnected"
            self.identified = False
            self.detected_role = None

        self.on_state_change(self.role, self.snapshot_status())
        return {"ok": True}

    def send_line(self, line: str, source: str = "user") -> Dict[str, Any]:
        err = validate_firmware_command(self.role, line)
        if err:
            return {"ok": False, "error": err}

        with self._lock:
            ser = self._serial
            if not ser or not ser.is_open:
                return {"ok": False, "error": f"{self.role.upper()} não está conectado."}
            try:
                raw = (line.strip() + "\r").encode("utf-8")
                ser.write(raw)
                ser.flush()
                entry = {"ts": time.time(), "cmd": line.strip(), "source": source, "sent": True}
                self.command_log.append(entry)
                if len(self.command_log) > 100:
                    self.command_log.pop(0)
                return {"ok": True, "command": line.strip()}
            except Exception as exc:
                self.last_error = f"Erro de escrita serial: {exc}"
                return {"ok": False, "error": self.last_error}

    def _reader_loop(self) -> None:
        while not self._stop_ev.is_set():
            with self._lock:
                ser = self._serial
            if not ser or not ser.is_open:
                break
            try:
                waiting = ser.in_waiting
                chunk = ser.read(waiting if waiting > 0 else 1)
                if chunk:
                    self.parser.feed(chunk)
            except (serial.SerialException, OSError) as exc:
                self.last_error = f"Desconexão física detectada em {self.port}: {exc}"
                self.status = "error"
                with self._lock:
                    try:
                        if self._serial:
                            self._serial.close()
                    except Exception:
                        pass
                    self._serial = None
                self.on_state_change(self.role, self.snapshot_status())
                break


class DualSerialManager:
    """Coordena TX e RX garantindo exclusividade estrita de portas COM."""

    def __init__(self,
                 on_event: Callable[[str, Dict[str, Any]], None],
                 on_cli: Callable[[str, str], None],
                 on_state_change: Callable[[str, Dict[str, Any]], None]):
        self._guard = threading.Lock()
        self.tx = DeviceConnection("tx", on_event, on_cli, on_state_change)
        self.rx = DeviceConnection("rx", on_event, on_cli, on_state_change)

    def connect(self, role: str, port: str, baudrate: int = 115200) -> Dict[str, Any]:
        with self._guard:
            if role not in ("tx", "rx"):
                return {"ok": False, "error": "Papel inválido (use 'tx' ou 'rx')."}
            other = self.rx if role == "tx" else self.tx
            if other.status == "connected" and other.port == port:
                return {
                    "ok": False,
                    "error": f"Conflito de porta: {port} já está em uso exclusivo pelo {other.role.upper()}.",
                }
            target = self.tx if role == "tx" else self.rx
            return target.connect(port, baudrate)

    def disconnect(self, role: str) -> Dict[str, Any]:
        if role == "tx":
            return self.tx.disconnect()
        if role == "rx":
            return self.rx.disconnect()
        return {"ok": False, "error": "Papel inválido."}

    def send_command(self, role: str, command: str) -> Dict[str, Any]:
        target = self.tx if role == "tx" else self.rx
        return target.send_line(command, source="user")

    def status(self) -> Dict[str, Any]:
        return {
            "tx": self.tx.snapshot_status(),
            "rx": self.rx.snapshot_status(),
        }
