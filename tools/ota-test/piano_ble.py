# ============================================================================
#  Player Piano - ESP32-S3 self-playing acoustic piano
#  Copyright (c) 2026 Steven Jin <stevenjin20090101@gmail.com>
#  Original author & creator: Steven Jin.
#  Licensed under the MIT License (see LICENSE). This copyright and attribution
#  notice MUST be preserved in all copies or substantial portions of the work.
#  Authorship provenance (Ed25519 fingerprint): eab16a502f679465  - see PROVENANCE.md
# ============================================================================
# Shared, importable helpers for the Bluetooth bench tools (docs/BLE_OTA.md,
# docs/BLE_SETTINGS.md). No side effects on import: the UUIDs, the pure frame
# builders/parsers, BLE-MIDI framing, console line assembly, and a couple of
# bleak helpers for finding and connecting to the piano. Nothing here touches
# the radio until you call find_piano()/connect_piano().
#
# "Steven Piano" is an ESP32-S3 BLE peripheral. It does NOT advertise while
# another device is connected to it, so a scan can time out for that reason
# alone (see find_piano's message).

from __future__ import annotations

import asyncio
import base64
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

# ---- UUIDs (docs/BLE_OTA.md §3, docs/BLE_SETTINGS.md §1) --------------------
# 128-bit forms are written out in full; the 16-bit ones expand to the Bluetooth
# base UUID, which bleak does for us when we look them up.

MIDI_SERVICE_UUID = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
MIDI_CHAR_UUID = "7772e5db-3868-4112-a1a9-f2669d106bf3"

NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # app -> piano (write)
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # piano -> app (notify)

UPDATE_SERVICE_UUID = "7d0a0001-3f5e-4b8c-9a2d-6e1f0c4b8a17"
UPDATE_CONTROL_UUID = "7d0a0002-3f5e-4b8c-9a2d-6e1f0c4b8a17"  # write+notify
UPDATE_DATA_UUID = "7d0a0003-3f5e-4b8c-9a2d-6e1f0c4b8a17"  # write-no-response

DIS_SERVICE_UUID = "0000180a-0000-1000-8000-00805f9b34fb"  # 0x180A
FW_REVISION_UUID = "00002a26-0000-1000-8000-00805f9b34fb"  # 0x2A26
MANUFACTURER_UUID = "00002a29-0000-1000-8000-00805f9b34fb"  # 0x2A29
MODEL_NUMBER_UUID = "00002a24-0000-1000-8000-00805f9b34fb"  # 0x2A24

PIANO_NAME = "Steven Piano"

# ---- OTA opcodes and error names (docs/BLE_OTA.md §5) -----------------------

OP_BEGIN = 0x01
OP_END = 0x02
OP_ABORT = 0x03
OP_READY = 0x81
OP_VERIFYING = 0x82
OP_OK = 0x83
OP_ABORTED = 0x84
OP_ACK = 0x85
OP_ERR = 0xE1

BEGIN_BYTES = 118
VERSION_BYTES = 16
MAX_WINDOW = 32
MAX_CHUNK = 250  # Data's max length is 252 = seq u16 + 250
ATT_HEADER = 3
SEQ_HEADER = 2
SLOT_BYTES = 6_553_600  # app0 / app1 in default_16MB.csv (0x640000)
RESTART_MS = 1500

ERROR_NAMES = {
    1: "not quiet",
    2: "too large",
    3: "already updating",
    4: "bad header or sequence gap",
    5: "hash mismatch",
    6: "bad signature",
    7: "flash write failed",
    8: "image invalid",
    9: "timeout",
    10: "refused by safety",
}

# version[16]: 1..16 of these, zero-padded (docs/BLE_OTA.md §5)
_VERSION_CHARS = set(
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.+-"
)


def version_ok(version: str) -> bool:
    """True when `version` is 1..16 bytes of the allowed set (BLE_OTA.md §5)."""
    raw = version.encode("utf-8")
    return 1 <= len(raw) <= VERSION_BYTES and all(chr(b) in _VERSION_CHARS for b in raw)


def max_chunk(mtu: int) -> int:
    """The Data payload limit at this MTU: min(MTU - 5, 250) (docs/BLE_OTA.md §3)."""
    return max(0, min(mtu - ATT_HEADER - SEQ_HEADER, MAX_CHUNK))


# ---- Frame builders (pure) -------------------------------------------------


def begin_frame(size: int, sha256: bytes, sig: bytes, version: str, window: int) -> bytes:
    """
    BEGIN, 118 bytes (docs/BLE_OTA.md §5):
    01 · size u32 LE · sha256[32] · sig[64] · version[16] zero-padded · window u8.
    Builds the frame from the values given; no validation of ranges here so the
    fault tools can build a deliberately bad BEGIN (e.g. a claimed size).
    """
    if len(sha256) != 32:
        raise ValueError("sha256 must be 32 bytes")
    if len(sig) != 64:
        raise ValueError("sig must be 64 bytes")
    ver = version.encode("utf-8")
    if len(ver) > VERSION_BYTES:
        raise ValueError("version is more than 16 bytes")
    out = bytearray(BEGIN_BYTES)
    out[0] = OP_BEGIN
    out[1:5] = (size & 0xFFFFFFFF).to_bytes(4, "little")
    out[5:37] = sha256
    out[37:101] = sig
    out[101 : 101 + len(ver)] = ver  # the rest stays zero (zero-padded)
    out[117] = window & 0xFF
    return bytes(out)


def end_frame() -> bytes:
    return bytes([OP_END])


def abort_frame() -> bytes:
    return bytes([OP_ABORT])


def data_frame(seq: int, payload: bytes) -> bytes:
    """Data (WRITE_NR): seq u16 LE + payload (docs/BLE_OTA.md §5)."""
    return (seq & 0xFFFF).to_bytes(2, "little") + bytes(payload)


# ---- Control notification parsing ------------------------------------------


@dataclass
class OtaEvent:
    """A parsed Control notification from the piano (docs/BLE_OTA.md §5)."""

    op: int
    name: str
    raw: bytes
    max_chunk: Optional[int] = None
    window: Optional[int] = None
    bytes_received: Optional[int] = None
    restart_ms: Optional[int] = None
    code: Optional[int] = None
    malformed: bool = False

    def decoded(self) -> str:
        """A short human-readable meaning, for the hex log."""
        if self.malformed:
            return f"{self.name} (MALFORMED, {len(self.raw)} bytes)"
        if self.op == OP_READY:
            return f"READY maxChunk={self.max_chunk} window={self.window}"
        if self.op == OP_ACK:
            return f"ACK bytesReceived={self.bytes_received}"
        if self.op == OP_VERIFYING:
            return "VERIFYING"
        if self.op == OP_OK:
            return f"OK restartInMs={self.restart_ms}"
        if self.op == OP_ABORTED:
            return "ABORTED"
        if self.op == OP_ERR:
            return f"ERR {self.code} ({ERROR_NAMES.get(self.code, 'unknown')})"
        return f"unknown opcode 0x{self.op:02X}"

    @property
    def ends(self) -> bool:
        """Whether the session is over with this event."""
        return self.op in (OP_OK, OP_ABORTED, OP_ERR)


def parse_control(data: bytes) -> OtaEvent:
    """Parse one Control notification. A known opcode of the wrong length is
    flagged `malformed`; an unknown opcode is returned with name 'unknown'."""
    data = bytes(data)
    if not data:
        return OtaEvent(op=-1, name="empty", raw=data, malformed=True)
    op = data[0]
    if op == OP_READY:
        if len(data) == 4:
            return OtaEvent(OP_READY, "READY", data,
                            max_chunk=int.from_bytes(data[1:3], "little"), window=data[3])
        return OtaEvent(OP_READY, "READY", data, malformed=True)
    if op == OP_ACK:
        if len(data) == 5:
            return OtaEvent(OP_ACK, "ACK", data,
                            bytes_received=int.from_bytes(data[1:5], "little"))
        return OtaEvent(OP_ACK, "ACK", data, malformed=True)
    if op == OP_VERIFYING:
        return OtaEvent(OP_VERIFYING, "VERIFYING", data, malformed=len(data) != 1)
    if op == OP_OK:
        if len(data) == 3:
            return OtaEvent(OP_OK, "OK", data,
                            restart_ms=int.from_bytes(data[1:3], "little"))
        return OtaEvent(OP_OK, "OK", data, malformed=True)
    if op == OP_ABORTED:
        return OtaEvent(OP_ABORTED, "ABORTED", data, malformed=len(data) != 1)
    if op == OP_ERR:
        if len(data) == 2:
            return OtaEvent(OP_ERR, "ERR", data, code=data[1])
        return OtaEvent(OP_ERR, "ERR", data, malformed=True)
    return OtaEvent(op=op, name="unknown", raw=data)


def hexs(data: bytes) -> str:
    """Spaced uppercase hex, as the specs print frames."""
    return " ".join(f"{b:02X}" for b in bytes(data))


# ---- BLE-MIDI framing (docs/BLE_OTA.md §13's "notes then BEGIN") ------------
# The piano's parser wants a header byte, then a timestamp byte before EVERY
# status byte, and no running status (android/.../ble/BleMidiFramer.kt). One
# message per packet is all the bench tools need.


def _midi_packet(status: int, data1: int, data2: int, timestamp_ms: int = 0) -> bytes:
    time13 = timestamp_ms & 0x1FFF
    header = 0x80 | (time13 >> 7)
    tstamp = 0x80 | (time13 & 0x7F)
    return bytes([header, tstamp, status & 0xFF, data1 & 0x7F, data2 & 0x7F])


def midi_note_on(note: int, velocity: int, channel: int = 0, timestamp_ms: int = 0) -> bytes:
    """A one-message BLE-MIDI NoteOn packet. THIS SOUNDS A KEY on the piano."""
    return _midi_packet(0x90 | (channel & 0x0F), note, velocity, timestamp_ms)


def midi_note_off(note: int, velocity: int = 0, channel: int = 0, timestamp_ms: int = 0) -> bytes:
    """A one-message BLE-MIDI NoteOff packet."""
    return _midi_packet(0x80 | (channel & 0x0F), note, velocity, timestamp_ms)


def midi_cc(controller: int, value: int, channel: int = 0, timestamp_ms: int = 0) -> bytes:
    """A one-message BLE-MIDI Control Change packet (e.g. CC123 all-notes-off)."""
    return _midi_packet(0xB0 | (channel & 0x0F), controller, value, timestamp_ms)


# ---- Console line assembly (docs/BLE_SETTINGS.md §3) ------------------------


class ConsoleLineAssembler:
    """Turns TX notifications back into lines: bytes gather until '\\n', then
    decode as UTF-8 (a notification may split a multi-byte character); a
    trailing '\\r' is dropped, and the firmware's '> command' echo is dropped.
    Mirrors android/.../ble/ConsoleChannel.kt's ConsoleLineAssembler."""

    ECHO = "> "

    def __init__(self, max_bytes: int = 1024) -> None:
        self._pending = bytearray()
        self._max = max_bytes

    def feed(self, chunk: bytes, emit: Callable[[str], None]) -> None:
        for b in bytes(chunk):
            if b == 0x0A:  # '\n'
                line = self._pending.decode("utf-8", "replace")
                if line.endswith("\r"):
                    line = line[:-1]
                self._pending.clear()
                if not line.startswith(self.ECHO):
                    emit(line)
            elif len(self._pending) < self._max:
                self._pending.append(b)

    def clear(self) -> None:
        self._pending.clear()


# ---- Timestamps ------------------------------------------------------------


class Stamp:
    """A millisecond stopwatch for the notification log. `()` returns a short
    '[   1234 ms]' tag measured from when the Stamp was made."""

    def __init__(self) -> None:
        self._t0 = time.monotonic()

    def ms(self) -> float:
        return (time.monotonic() - self._t0) * 1000.0

    def tag(self) -> str:
        return f"[{self.ms():9.1f} ms]"


# ---- Finding and connecting (the only functions that touch the radio) ------


class PianoNotFound(Exception):
    pass


class BluetoothPermission(Exception):
    pass


def _bleak():
    """Import bleak lazily so importing this module needs no radio stack."""
    import bleak  # noqa: F401
    from bleak import BleakClient, BleakScanner
    from bleak.exc import BleakError

    return BleakClient, BleakScanner, BleakError


async def find_piano(timeout: float = 12.0, name: str = PIANO_NAME):
    """Scan for the piano by advertised name. Raises PianoNotFound with a clear
    message (it does not advertise while connected to another device), or
    BluetoothPermission when macOS has not granted Bluetooth to this terminal."""
    _, BleakScanner, _ = _bleak()
    try:
        from bleak.exc import BleakBluetoothNotAvailableError
    except Exception:  # older bleak; treat as not present
        BleakBluetoothNotAvailableError = None
    try:
        device = await BleakScanner.find_device_by_name(name, timeout=timeout)
    except Exception as exc:  # pragma: no cover - needs a real radio
        if BleakBluetoothNotAvailableError and isinstance(exc, BleakBluetoothNotAvailableError):
            raise BluetoothPermission(
                "macOS would not let this program use Bluetooth: "
                f"{exc}. Allow the app that launched Python (Terminal) under "
                "System Settings > Privacy & Security > Bluetooth, then retry."
            ) from exc
        raise
    if device is None:
        raise PianoNotFound(
            f'No BLE device named "{name}" advertised within {timeout:.0f} s. '
            "The piano does not advertise while another device is connected to it "
            "(phone, iPad, another Mac) — disconnect that first. After a USB "
            "flash, macOS can show a stale service list: toggle Bluetooth off and on."
        )
    return device


@dataclass
class Features:
    """Which of the piano's services this connection discovered."""

    midi: bool = False
    console: bool = False
    update: bool = False
    device_info: bool = False
    control: bool = False
    data: bool = False
    fw_revision: bool = False

    def summary(self) -> str:
        def mark(name: str, present: bool) -> str:
            return f"{name}={'yes' if present else 'no'}"

        return "  ".join(
            [
                mark("MIDI", self.midi),
                mark("console", self.console),
                mark("update", self.update),
                mark("device-info", self.device_info),
            ]
        )


def discover_features(client) -> Features:
    """Report which services/characteristics are present on a connected client."""
    services = client.services
    f = Features()
    if services.get_service(MIDI_SERVICE_UUID):
        f.midi = services.get_characteristic(MIDI_CHAR_UUID) is not None
    if services.get_service(NUS_SERVICE_UUID):
        f.console = services.get_characteristic(NUS_TX_UUID) is not None
    if services.get_service(UPDATE_SERVICE_UUID):
        f.update = True
        f.control = services.get_characteristic(UPDATE_CONTROL_UUID) is not None
        f.data = services.get_characteristic(UPDATE_DATA_UUID) is not None
    if services.get_service(DIS_SERVICE_UUID):
        f.device_info = True
        f.fw_revision = services.get_characteristic(FW_REVISION_UUID) is not None
    return f


async def connect_piano(timeout: float = 12.0, name: str = PIANO_NAME):
    """Find, connect, discover services. Returns (client, device, features).
    The caller is responsible for client.disconnect()."""
    BleakClient, _, BleakError = _bleak()
    device = await find_piano(timeout=timeout, name=name)
    client = BleakClient(device)
    try:
        await client.connect()
    except Exception as exc:  # pragma: no cover - needs a real radio
        try:
            from bleak.exc import BleakBluetoothNotAvailableError

            if isinstance(exc, BleakBluetoothNotAvailableError):
                raise BluetoothPermission(str(exc)) from exc
        except ImportError:
            pass
        raise
    features = discover_features(client)
    return client, device, features


async def read_firmware_revision(client) -> Optional[str]:
    """Read Firmware Revision String 0x2A26, or None if absent/unreadable."""
    try:
        raw = await client.read_gatt_char(FW_REVISION_UUID)
    except Exception:
        return None
    text = bytes(raw).split(b"\x00", 1)[0].decode("utf-8", "replace").strip()
    return text or None


# Re-export base64 so tools can decode an --sig argument without importing it.
b64decode = base64.b64decode
b64encode = base64.b64encode
