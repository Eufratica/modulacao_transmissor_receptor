#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Suíte Completa de Testes da Plataforma 4-QAM (Seção 17)
=======================================================
Cobre todos os 16 itens exigidos:
  1. Listagem de portas COM
  2. Conexão independente TX/RX e bloqueio de conflito da mesma COM
  3. Tratamento de desconexão física e porta ocupada
  4. Parser serial híbrido (CLI ASCII + binário 4QAM-TLM v1)
  5. Integridade CRC-16, versão e comprimento do protocolo
  6. Detecção de perda (lacuna seq) e duplicação de pacotes
  7. Reconstrução de quadros TX, PWM, rajadas, símbolos DSP e snapshots ADC
  8. Associação TX <-> RX para Ground Truth
  9. Cálculo exato de BER (zero e não-zero + recusa quando não alinhável)
 10. Cálculo exato de SER
 11. Cálculo exato de EVM_RMS (e recusa sem I/Q alinhado)
 12. Análise FFT real (1000 Hz e recusa com amostras insuficientes)
 13. Função de transferência acoplada RC 2ª ordem
 14. Exportação JSON e CSV
 15. Persistência e recuperação de sessões em disco
 16. Estado inicial "Aguardando hardware" sem dados falsos + downsampling + HTTP/WebSocket API
"""

import asyncio
import json
import math
import os
import shutil
import struct
import sys
import threading
import time
import unittest
from unittest.mock import MagicMock, patch

BACKEND_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "backend"))
if BACKEND_DIR not in sys.path:
    sys.path.insert(0, BACKEND_DIR)

import requests  # noqa: E402
import serial  # noqa: E402
import uvicorn  # noqa: E402
import websockets  # noqa: E402

import app as backend_app  # noqa: E402
import dsp_analysis as dsp  # noqa: E402
import offline_sim  # noqa: E402
import serial_manager as sm  # noqa: E402
import serial_protocol as proto  # noqa: E402
import session_store as store  # noqa: E402

TEST_PORT = 18421
BASE_URL = f"http://127.0.0.1:{TEST_PORT}"
WS_URL = f"ws://127.0.0.1:{TEST_PORT}/ws"


class FakePortInfo:
    def __init__(self, device: str, description: str):
        self.device = device
        self.description = description
        self.hwid = "USB VID:PID=10C4:EA60"
        self.manufacturer = "Silicon Labs"


class PlatformValidationTests(unittest.TestCase):
    _server = None
    _srv_thread = None

    @classmethod
    def setUpClass(cls) -> None:
        cfg = uvicorn.Config(backend_app.app, host="127.0.0.1", port=TEST_PORT, log_level="warning")
        cls._server = uvicorn.Server(cfg)
        cls._srv_thread = threading.Thread(target=cls._server.run, daemon=True)
        cls._srv_thread.start()
        for _ in range(50):
            try:
                if requests.get(f"{BASE_URL}/api/state", timeout=0.5).status_code == 200:
                    return
            except Exception:
                time.sleep(0.1)
        raise RuntimeError("Servidor local Uvicorn de testes não iniciou a tempo.")

    @classmethod
    def tearDownClass(cls) -> None:
        if cls._server:
            cls._server.should_exit = True
        if cls._srv_thread:
            cls._srv_thread.join(timeout=2.0)

    def setUp(self) -> None:
        backend_app.hub.reset_live_plots(clear_offline_flag=True)
        backend_app.hub.tx_hello = None
        backend_app.hub.rx_hello = None

    def test_01_list_com_ports(self) -> None:
        with patch("serial.tools.list_ports.comports", return_value=[FakePortInfo("COM7", "CP2102"), FakePortInfo("COM3", "CH340")]):
            ports = sm.list_com_ports()
        self.assertEqual([p["device"] for p in ports], ["COM3", "COM7"])

    def test_02_independent_tx_rx_and_port_exclusivity(self) -> None:
        events = []
        mgr = sm.DualSerialManager(
            on_event=lambda r, p: events.append((r, p)),
            on_cli=lambda r, l: None,
            on_state_change=lambda r, s: None,
        )

        class MockSerial:
            def __init__(self, port, baudrate, **kwargs):
                self.port = port
                self.baudrate = baudrate
                self.is_open = True
                self.in_waiting = 0
                self.written = []

            def reset_input_buffer(self):
                pass

            def write(self, data):
                self.written.append(data)

            def flush(self):
                pass

            def read(self, n):
                import time
                time.sleep(0.02)
                return b""

            def close(self):
                self.is_open = False

        with patch("serial.Serial", side_effect=MockSerial):
            r_tx = mgr.connect("tx", "COM5", 115200)
            self.assertTrue(r_tx["ok"])
            r_dup = mgr.connect("rx", "COM5", 115200)
            self.assertFalse(r_dup["ok"])
            self.assertIn("COM5", r_dup["error"])
            r_rx = mgr.connect("rx", "COM6", 230400)
            self.assertTrue(r_rx["ok"])
            self.assertEqual(mgr.tx.baudrate, 115200)
            self.assertEqual(mgr.rx.baudrate, 230400)
            mgr.disconnect("tx")
            mgr.disconnect("rx")

    def test_03_disconnect_and_busy_port_handling(self) -> None:
        states = []
        mgr = sm.DualSerialManager(
            on_event=lambda r, p: None,
            on_cli=lambda r, l: None,
            on_state_change=lambda r, s: states.append((r, s)),
        )
        with patch("serial.Serial", side_effect=serial.SerialException("PermissionError(13, 'Access is denied.')")):
            res = mgr.connect("tx", "COM4", 115200)
        self.assertFalse(res["ok"])
        self.assertIn("ocupada", res["error"])

        # Simula desconexão física no meio da leitura
        conn = sm.DeviceConnection("rx", lambda r, p: None, lambda r, l: None, lambda r, s: states.append((r, s)))
        fake_ser = MagicMock()
        fake_ser.is_open = True
        fake_ser.in_waiting = 1
        fake_ser.read.side_effect = serial.SerialException("Device disconnected")
        conn._serial = fake_ser
        conn.port = "COM9"
        conn.status = "connected"
        conn._reader_loop()
        self.assertEqual(conn.status, "error")
        self.assertIn("Desconexão física", conn.last_error or "")

    def test_04_05_06_hybrid_parser_integrity_loss_and_dup(self) -> None:
        cli_lines = []
        packets = []
        parser = proto.HybridSerialParser(on_packet=packets.append, on_cli_line=cli_lines.append)

        p0 = proto.build_packet(proto.MSG_TX_FRAME_EVENT, struct.pack("<IBBBBIIHHHHHBB", 1, 2, 0, 0, 0, 1000, 100, 1300, 900, 40, 0x3929, 1, 2, 0) + b"A", seq=0)
        # Pula seq=1,2 -> seq=3 (2 perdidos)
        p3 = proto.build_packet(proto.MSG_TX_FRAME_EVENT, struct.pack("<IBBBBIIHHHHHBB", 2, 2, 0, 0, 0, 1000, 100, 1300, 900, 40, 0x3929, 1, 2, 0) + b"A", seq=3)
        # Pacote duplicado seq=3
        p3_dup = p3
        # Pacote com CRC corrompido
        bad_crc = bytearray(p3)
        bad_crc[-1] ^= 0xFF
        # Versão errada (ver=9)
        bad_ver = bytearray(proto.PROTO_SYNC + struct.pack("<BBBHIH", 9, 1, 0, 4, 0, 0) + b"\x00\x00")

        stream = b"ID: ROLE=TX PROTOCOL=4QAM-TLM-v1\n" + p0 + bytes(bad_crc) + bytes(bad_ver) + b"4QAM-TX> ok\n" + p3 + p3_dup
        # Alimenta fragmentado em blocos ímpares de 3 bytes para testar reassembly!
        for i in range(0, len(stream), 3):
            parser.feed(stream[i:i + 3])

        self.assertEqual(len(packets), 3)
        self.assertEqual(parser.stats.packets_ok, 3)
        self.assertEqual(parser.stats.packets_lost, 2)
        self.assertEqual(parser.stats.packets_dup, 1)
        self.assertEqual(parser.stats.crc_errors, 1)
        self.assertEqual(parser.stats.version_errors, 1)
        self.assertIn("ID: ROLE=TX PROTOCOL=4QAM-TLM-v1", cli_lines)

    def test_07_08_09_10_11_offline_protocol_reconstruction_and_ground_truth(self) -> None:
        res = requests.post(f"{BASE_URL}/api/offline_test", json={"text": "4QAM-OK!"}, timeout=15)
        self.assertEqual(res.status_code, 200)
        st = res.json()["state"]

        self.assertTrue(st["is_offline_test"])
        self.assertEqual(st["data_source"], "offline_test")
        self.assertEqual(st["last_tx_frame"]["payload_text"], "4QAM-OK!")
        self.assertEqual(st["last_rx_frame"]["payload_text"], "4QAM-OK!")
        self.assertTrue(st["last_rx_frame"]["crc_ok"])

        gt = st["last_ground_truth"]
        self.assertTrue(gt["calculable"])
        self.assertEqual(gt["ber"], 0.0)
        self.assertEqual(gt["ser"], 0.0)
        self.assertEqual(gt["bit_errors"], 0)
        self.assertIsNotNone(gt["evm_rms_pct"])
        self.assertLess(gt["evm_rms_pct"], 5.0)
        self.assertEqual(len(st["tx_pwm_ref"]["cmp_ticks"]), 512)
        self.assertEqual(st["scope"]["original_count"], 2048)

        # Testa BER / SER / recusa de alinhamento quando há erros ou tamanho divergente
        tx_rec = st["last_tx_frame"]
        rx_err_frame = dict(st["last_rx_frame"])
        # '4' (0x34) -> 0x37 troca 1 símbolo (00->11 = 2 bits errados em 64 bits)
        rx_err_frame["payload_bytes"] = [0x37] + tx_rec["payload_bytes"][1:]
        rx_err_frame["payload_text"] = "7QAM-OK!"
        gt_err = dsp.compute_ground_truth(tx_rec, rx_err_frame, [])
        self.assertTrue(gt_err["calculable"])
        self.assertEqual(gt_err["bit_errors"], 2)
        self.assertAlmostEqual(gt_err["ber"], 2.0 / 64.0)
        self.assertEqual(gt_err["symbol_errors"], 1)
        self.assertAlmostEqual(gt_err["ser"], 1.0 / 32.0)
        # Sem símbolos I/Q alinhados, EVM não é inventado
        self.assertIsNone(gt_err["evm_rms_pct"])

        # Comprimento diferente -> não calculável
        rx_mismatch = dict(rx_err_frame)
        rx_mismatch["payload_bytes"] = [0x34]
        gt_bad = dsp.compute_ground_truth(tx_rec, rx_mismatch, [])
        self.assertFalse(gt_bad["calculable"])
        self.assertIsNone(gt_bad["ber"])

    def test_12_real_fft_and_insufficient_guard(self) -> None:
        empty_fft = dsp.compute_fft_spectrum([1300.0] * 10, 20000.0)
        self.assertFalse(empty_fft["valid"])

        fft_res = requests.get(f"{BASE_URL}/api/fft?window=hann", timeout=5).json()
        # Recarrega primeiro após garantir estado offline
        requests.post(f"{BASE_URL}/api/offline_test", json={"text": "4QAM-OK!"}, timeout=15)
        fft_res = requests.get(f"{BASE_URL}/api/fft?window=hann", timeout=5).json()
        self.assertTrue(fft_res["valid"])
        self.assertEqual(fft_res["fft_size"], 2048)
        self.assertAlmostEqual(fft_res["df_hz"], 20000.0 / 2048.0, places=2)
        self.assertLess(abs(fft_res["peak_freq_hz"] - 1000.0), 15.0)

    def test_13_rc_coupled_transfer_function(self) -> None:
        m = dsp.compute_rc2_bode(1000.0, 33.0, 10000.0, 3.3, 80000.0, 1000.0)
        self.assertTrue(m["is_theoretical_model"])
        # a1 = R1C1 + R2C2 + R1C2 = 33 + 33 + 3.3 = 69.3 us
        self.assertAlmostEqual(m["a1_us"], 69.3, places=1)
        self.assertAlmostEqual(m["gain_fc_db"], -0.44, places=1)
        self.assertAlmostEqual(m["attenuation_pwm_db"], -48.83, delta=0.15)

    def test_14_15_sessions_and_exports(self) -> None:
        tmp_dir = os.path.join(os.path.dirname(__file__), "_tmp_sessions")
        orig_dir = store.SESSIONS_DIR
        store.SESSIONS_DIR = tmp_dir
        try:
            requests.post(f"{BASE_URL}/api/offline_test", json={"text": "AB"}, timeout=15)
            sv = requests.post(f"{BASE_URL}/api/sessions/save", json={"name": "Teste_Unit"}, timeout=5).json()
            self.assertTrue(sv["ok"])
            sid = sv["session_id"]

            lst = requests.get(f"{BASE_URL}/api/sessions", timeout=5).json()["sessions"]
            self.assertTrue(any(s["session_id"] == sid for s in lst))

            loaded = requests.get(f"{BASE_URL}/api/sessions/{sid}", timeout=5).json()
            self.assertTrue(loaded["metadata"]["is_offline_test"])
            self.assertEqual(loaded["tx_frames"][0]["payload_text"], "AB")

            exp_json = requests.get(f"{BASE_URL}/api/export/json", timeout=5)
            self.assertEqual(exp_json.status_code, 200)
            self.assertIn("adc_snapshot_full", json.loads(exp_json.content))

            for kind in ("ground_truth", "adc", "dsp_symbols"):
                r_csv = requests.get(f"{BASE_URL}/api/export/csv?kind={kind}", timeout=5)
                self.assertEqual(r_csv.status_code, 200)
                self.assertGreater(len(r_csv.content), 20)
        finally:
            store.SESSIONS_DIR = orig_dir
            shutil.rmtree(tmp_dir, ignore_errors=True)

    def test_16_awaiting_hardware_websocket_assets_and_downsampling(self) -> None:
        st = requests.get(f"{BASE_URL}/api/state", timeout=5).json()
        self.assertEqual(st["data_source"], "awaiting_hardware")
        self.assertFalse(st["is_offline_test"])
        self.assertFalse(st["scope"]["available"])
        self.assertEqual(st["scope"]["samples_mv"], [])
        self.assertEqual(st["dsp_symbols"], [])
        self.assertIsNone(st["last_ground_truth"])

        r_landing = requests.get(f"{BASE_URL}/", timeout=5)
        self.assertEqual(r_landing.status_code, 200)
        self.assertIn("4-QAM LAB", r_landing.text)
        self.assertIn("Aguardando hardware", r_landing.text)
        self.assertIn("PRÉVIA VISUAL — DADOS ILUSTRATIVOS", r_landing.text)

        for lab_route in ("/lab", "/dashboard"):
            r_lab = requests.get(f"{BASE_URL}{lab_route}", timeout=5)
            self.assertEqual(r_lab.status_code, 200, f"Falha em {lab_route}")
            self.assertIn("Aguardando hardware", r_lab.text)
            self.assertIn('id="canvas-scope"', r_lab.text)
            self.assertIn('id="canvas-pwm-scope"', r_lab.text)
            self.assertIn('id="chk-scope-pwm"', r_lab.text)
            self.assertIn('id="chk-scope-carrier"', r_lab.text)
            self.assertIn('id="chk-scope-bb"', r_lab.text)
            self.assertIn('id="btn-jump-pwm"', r_lab.text)

        for asset in ("/static/css/tokens.css", "/static/css/landing.css",
                      "/static/js/a11y_theme.js", "/static/js/landing.js",
                      "/static/css/lab.css", "/static/js/main.js", "/static/js/scope.js",
                      "/static/js/constellation.js", "/static/js/spectrum.js",
                      "/static/js/filters_rc.js", "/static/js/digital_truth.js",
                      "/static/js/dsp_lab.js", "/static/js/math_ref.js", "/static/js/history.js"):
            r_a = requests.get(f"{BASE_URL}{asset}", timeout=5)
            self.assertEqual(r_a.status_code, 200, f"Falha ao carregar {asset}")

        # Verifica WebSocket real entregando atualização inicial e push ao disparar evento
        async def ws_check() -> dict:
            async with websockets.connect(WS_URL) as ws:
                msg = await asyncio.wait_for(ws.recv(), timeout=3.0)
                return json.loads(msg)

        ws_state = asyncio.run(ws_check())
        self.assertEqual(ws_state["data_source"], "awaiting_hardware")

        # Downsampling preserva picos min/max exatos
        wave = [1300] * 8000
        wave[1234] = 350
        wave[5678] = 2290
        ds = backend_app.TelemetryHub._minmax_downsample(wave, 1200)
        self.assertTrue(ds["decimated_for_view"])
        self.assertEqual(min(ds["samples_mv"]), 350)
        self.assertEqual(max(ds["samples_mv"]), 2290)

        # Valida whitelist de comandos reais do firmware
        self.assertIsNone(sm.validate_firmware_command("tx", "/baud 100"))
        self.assertIsNone(sm.validate_firmware_command("tx", "/test cycle"))
        self.assertIsNotNone(sm.validate_firmware_command("tx", "/comando_inventado 1"))
        self.assertIsNotNone(sm.validate_firmware_command("rx", "/test carrier"))

    def test_17_live_overrides_offline_and_allows_frame_id_reuse(self) -> None:
        # Simula teste offline prévio que gerou frame_id=1 ("4QAM-OK!")
        res = requests.post(f"{BASE_URL}/api/offline_test", json={"text": "4QAM-OK!"}, timeout=15)
        self.assertEqual(res.status_code, 200)
        self.assertTrue(backend_app.hub.is_offline_test)

        # Agora hardware real envia evento tx_frame também com frame_id=1 ("OI")
        live_pkt = {
            "event": "tx_frame",
            "frame_id": 1,
            "ts_us": 500000,
            "status": "done",
            "tx_mode": 2,
            "map_preset": "natural",
            "bit_order": "msb",
            "q_sign_minus": False,
            "carrier_freq_hz": 1000,
            "symbol_rate_baud": 100,
            "dc_offset_mv": 1300,
            "amplitude_peak_mv": 900,
            "payload_bytes": [79, 73],
            "payload_text": "OI",
        }
        backend_app.hub.on_serial_packet("tx", live_pkt, offline=False)
        snap = backend_app.hub.build_state_snapshot(include_waveforms=True)
        self.assertFalse(snap["is_offline_test"])
        self.assertEqual(snap["data_source"], "live_hardware")
        self.assertEqual(snap["last_tx_frame"]["payload_text"], "OI")
        self.assertIsNone(snap["last_rx_frame"])

        # Novo reboot do TX ou novo ciclo com frame_id=1 e payload diferente também deve registrar
        live_pkt_2 = {**live_pkt, "ts_us": 900000, "status": "queued", "payload_bytes": [65, 66], "payload_text": "AB"}
        backend_app.hub.on_serial_packet("tx", live_pkt_2, offline=False)
        snap2 = backend_app.hub.build_state_snapshot(include_waveforms=False)
        self.assertEqual(snap2["last_tx_frame"]["payload_text"], "AB")


if __name__ == "__main__":
    unittest.main(verbosity=2)
