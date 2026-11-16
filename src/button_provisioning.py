"""Provisioning utility to generate unique Matter button firmware hex files and pairing codes."""

from __future__ import annotations

import base64
from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct

P256_P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
P256_A = P256_P - 3
P256_GX = 0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296
P256_GY = 0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5
P256_ORDER = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
DEFAULT_SALT = b"SPAKE2P Key Salt"
DEFAULT_ITERATIONS = 1000
DEFAULT_PASSCODE = 20202021
DEFAULT_VERIFIER_B64 = (
    b"uWFwqugDNGiEck/po7KHwwMwwqZgN10XuyBajPGuyzUEV/iree4lOrao5GuwnlQ65CJzbeUB"
    b"49s31EH+NEkg0JVI5MGCQGMMT/SRPFNRODm3wH/MBiehuFc6FJ/NH6Rmzw=="
)
DEFAULT_PRODUCT_NAME = b"Matter Light Switch\x00"
DEFAULT_SERIAL_NUMBER = b"11223344556677889900\x00"
DEFAULT_BT_NAME = b"MatterSwitch\x00"
DISCRIMINATOR_INSTR_SIG = b"\x4f\xf4\x70\x63\x01\x93"
PASSCODE_GETTER_SIG = b"\x03\x4b\x00\x20\x23\x60\x10\xbd\x00\xbf"

_VERHOEFF_D = (
    (0, 1, 2, 3, 4, 5, 6, 7, 8, 9),
    (1, 2, 3, 4, 0, 6, 7, 8, 9, 5),
    (2, 3, 4, 0, 1, 7, 8, 9, 5, 6),
    (3, 4, 0, 1, 2, 8, 9, 5, 6, 7),
    (4, 0, 1, 2, 3, 9, 5, 6, 7, 8),
    (5, 9, 8, 7, 6, 0, 4, 3, 2, 1),
    (6, 5, 9, 8, 7, 1, 0, 4, 3, 2),
    (7, 6, 5, 9, 8, 2, 1, 0, 4, 3),
    (8, 7, 6, 5, 9, 3, 2, 1, 0, 4),
    (9, 8, 7, 6, 5, 4, 3, 2, 1, 0),
)
_VERHOEFF_P = (
    (0, 1, 2, 3, 4, 5, 6, 7, 8, 9),
    (1, 5, 7, 6, 2, 8, 3, 0, 9, 4),
    (5, 8, 0, 3, 7, 9, 6, 1, 4, 2),
    (8, 9, 1, 6, 0, 4, 3, 5, 2, 7),
    (9, 4, 5, 3, 1, 2, 6, 8, 7, 0),
    (4, 2, 8, 6, 5, 7, 3, 9, 0, 1),
    (2, 7, 9, 3, 8, 0, 6, 4, 1, 5),
    (7, 0, 4, 6, 9, 1, 3, 2, 5, 8),
)
_VERHOEFF_INV = (0, 4, 3, 2, 1, 5, 6, 7, 8, 9)
_B38_CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-."


@dataclass(frozen=True)
class ButtonProvisioningConfig:
    """Represents the unique Matter provisioning identity for a single button."""

    index: int
    device_name: str
    bt_name: str
    serial_number: str
    discriminator: int
    passcode: int
    manual_pairing_code: str
    qr_payload: str
    verifier_b64: str
    hex_path: Path | None = None


def _p256_point_add(
    p1: tuple[int, int] | None,
    p2: tuple[int, int] | None,
) -> tuple[int, int] | None:
    """Add two points on the NIST P-256 elliptic curve."""
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2:
        if (y1 + y2) % P256_P == 0:
            return None
        lam = (3 * x1 * x1 + P256_A) * pow(2 * y1, -1, P256_P) % P256_P
    else:
        lam = (y2 - y1) * pow(x2 - x1, -1, P256_P) % P256_P
    x3 = (lam * lam - x1 - x2) % P256_P
    y3 = (lam * (x1 - x3) - y1) % P256_P
    return x3, y3


def _p256_base_mult(scalar: int) -> tuple[int, int]:
    """Multiply the NIST P-256 base point G by a positive scalar."""
    result: tuple[int, int] | None = None
    addend: tuple[int, int] | None = (P256_GX, P256_GY)
    k = scalar
    while k > 0:
        if k & 1:
            result = _p256_point_add(result, addend)
        addend = _p256_point_add(addend, addend)
        k >>= 1
    if result is None:
        raise ValueError("Scalar multiplication resulted in point at infinity")
    return result


def compute_spake2p_verifier_b64(
    passcode: int,
    salt: bytes = DEFAULT_SALT,
    iterations: int = DEFAULT_ITERATIONS,
) -> str:
    """Compute the Matter SPAKE2+ verifier (w0 || L) encoded in Base64."""
    ws_len = 40
    ws = hashlib.pbkdf2_hmac(
        "sha256",
        struct.pack("<I", passcode),
        salt,
        iterations,
        dklen=ws_len * 2,
    )
    w0 = int.from_bytes(ws[:ws_len], "big") % P256_ORDER
    w1 = int.from_bytes(ws[ws_len:], "big") % P256_ORDER
    lx, ly = _p256_base_mult(w1)
    point_l = b"\x04" + lx.to_bytes(32, "big") + ly.to_bytes(32, "big")
    return base64.b64encode(w0.to_bytes(32, "big") + point_l).decode("ascii")


def compute_verhoeff_check_digit(digits: str) -> str:
    """Compute the Verhoeff check digit for a numeric string."""
    c = 0
    for i, ch in enumerate(reversed(digits)):
        c = _VERHOEFF_D[c][_VERHOEFF_P[(i + 1) % 8][int(ch)]]
    return str(_VERHOEFF_INV[c])


def compute_manual_pairing_code(discriminator: int, passcode: int) -> str:
    """Compute the 11-digit Matter manual pairing code for a discriminator and passcode."""
    short_disc = (discriminator >> 8) & 0xF
    chunk1 = (short_disc >> 2) & 0x3
    chunk2 = ((short_disc & 0x3) << 14) | (passcode & 0x3FFF)
    chunk3 = (passcode >> 14) & 0x1FFF
    body = f"{chunk1:01d}{chunk2:05d}{chunk3:04d}"
    return body + compute_verhoeff_check_digit(body)


def encode_base38(data: bytes) -> str:
    """Encode raw bytes into Matter Base38 representation."""
    out: list[str] = []
    for i in range(0, len(data), 3):
        chunk = data[i : i + 3]
        val = int.from_bytes(chunk, "little")
        n_chars = {1: 2, 2: 4, 3: 5}[len(chunk)]
        for _ in range(n_chars):
            out.append(_B38_CHARS[val % 38])
            val //= 38
    return "".join(out)


def compute_qr_payload(
    discriminator: int,
    passcode: int,
    vid: int = 0xFFF1,
    pid: int = 0x8004,
    discovery_caps: int = 0x02,
) -> str:
    """Compute the Matter QR code payload string ('MT:...')."""
    bits = (
        (0 << 0)
        | (vid << 3)
        | (pid << 19)
        | (0 << 35)
        | (discovery_caps << 37)
        | ((discriminator & 0xFFF) << 45)
        | ((passcode & 0x7FFFFFF) << 57)
    )
    return "MT:" + encode_base38(bits.to_bytes(11, "little"))


def encode_thumb2_movw_r3(imm16: int) -> bytes:
    """Encode a 32-bit Thumb-2 'movw r3, #imm16' instruction."""
    if not 0 <= imm16 <= 0xFFFF:
        raise ValueError("imm16 must be between 0 and 65535")
    imm4 = (imm16 >> 12) & 0xF
    i_bit = (imm16 >> 11) & 0x1
    imm3 = (imm16 >> 8) & 0x7
    imm8 = imm16 & 0xFF
    hw1 = 0xF240 | (i_bit << 10) | imm4
    hw2 = (imm3 << 12) | (3 << 8) | imm8
    return struct.pack("<HH", hw1, hw2)


def make_button_config(index: int, prefix: str = "Egen_superknapp") -> ButtonProvisioningConfig:
    """Create a deterministic, unique ButtonProvisioningConfig for button index (1..99)."""
    if not 1 <= index <= 99:
        raise ValueError("Button index must be in 1..99")
    device_name = f"{prefix}_{index:02d}"
    if len(device_name) > 19:
        raise ValueError("device_name must be <= 19 ASCII characters")
    bt_name = f"Superknapp{index:02d}"
    serial_number = f"EGEN-SUPERKNAPP-{index:04d}"
    discriminator = (((index - 1) % 15 + 1) << 8) | index
    passcode = 20202000 + index
    return ButtonProvisioningConfig(
        index=index,
        device_name=device_name,
        bt_name=bt_name,
        serial_number=serial_number,
        discriminator=discriminator,
        passcode=passcode,
        manual_pairing_code=compute_manual_pairing_code(discriminator, passcode),
        qr_payload=compute_qr_payload(discriminator, passcode),
        verifier_b64=compute_spake2p_verifier_b64(passcode),
    )


def patch_firmware_bytes(raw_rom: bytes, cfg: ButtonProvisioningConfig) -> bytearray:
    """Patch a flat firmware binary image with a button's unique Matter credentials."""
    buf = bytearray(raw_rom)

    def replace_exact(target: bytes, replacement: bytes, label: str) -> None:
        if len(replacement) != len(target):
            raise ValueError(f"{label} length mismatch: {len(replacement)} != {len(target)}")
        pos = buf.find(target)
        if pos == -1:
            raise ValueError(f"{label} pattern not found in firmware image")
        buf[pos : pos + len(target)] = replacement

    padded_name = cfg.device_name.encode("ascii") + b"\x00"
    padded_name += b"\x00" * (len(DEFAULT_PRODUCT_NAME) - len(padded_name))
    replace_exact(DEFAULT_PRODUCT_NAME, padded_name, "ProductName")

    padded_bt = cfg.bt_name.encode("ascii") + b"\x00"
    padded_bt += b"\x00" * (len(DEFAULT_BT_NAME) - len(padded_bt))
    replace_exact(DEFAULT_BT_NAME, padded_bt, "BtDeviceName")

    padded_sn = cfg.serial_number.encode("ascii") + b"\x00"
    replace_exact(DEFAULT_SERIAL_NUMBER, padded_sn, "SerialNumber")

    replace_exact(DEFAULT_VERIFIER_B64, cfg.verifier_b64.encode("ascii"), "Spake2pVerifier")

    disc_pos = buf.find(DISCRIMINATOR_INSTR_SIG)
    if disc_pos == -1:
        raise ValueError("GetSetupDiscriminator signature not found")
    buf[disc_pos : disc_pos + 4] = encode_thumb2_movw_r3(cfg.discriminator)

    pc_sig_pos = buf.find(PASSCODE_GETTER_SIG)
    if pc_sig_pos == -1:
        raise ValueError("GetSetupPasscode signature not found")
    pc_lit_pos = pc_sig_pos + len(PASSCODE_GETTER_SIG) + 4
    if buf[pc_lit_pos : pc_lit_pos + 4] != struct.pack("<I", DEFAULT_PASSCODE):
        raise ValueError("Unexpected literal at GetSetupPasscode")
    buf[pc_lit_pos : pc_lit_pos + 4] = struct.pack("<I", cfg.passcode)

    return buf


def patch_intel_hex(src_hex: Path, dst_hex: Path, cfg: ButtonProvisioningConfig) -> None:
    """Load an Intel HEX file, apply button provisioning patches, and write a new Intel HEX."""
    lines = src_hex.read_text(encoding="ascii").splitlines()
    mem: dict[int, int] = {}
    upper = 0
    for line in lines:
        if not line.startswith(":"):
            continue
        raw = bytes.fromhex(line[1:].strip())
        cnt, addr, rectype = raw[0], (raw[1] << 8) | raw[2], raw[3]
        data = raw[4 : 4 + cnt]
        if rectype == 0:
            for i, b_val in enumerate(data):
                mem[upper + addr + i] = b_val
        elif rectype == 4:
            upper = ((data[0] << 8) | data[1]) << 16
        elif rectype == 2:
            upper = ((data[0] << 8) | data[1]) << 4

    base = min(mem.keys())
    end = max(mem.keys()) + 1
    rom = bytearray(end - base)
    for a, b_val in mem.items():
        rom[a - base] = b_val

    patched = patch_firmware_bytes(rom, cfg)

    out_lines: list[str] = []
    upper = 0
    for line in lines:
        if not line.startswith(":"):
            out_lines.append(line)
            continue
        raw = bytearray.fromhex(line[1:].strip())
        cnt, addr, rectype = raw[0], (raw[1] << 8) | raw[2], raw[3]
        if rectype == 0:
            abs_addr = upper + addr - base
            new_data = patched[abs_addr : abs_addr + cnt]
            if new_data != raw[4 : 4 + cnt]:
                raw[4 : 4 + cnt] = new_data
                chk = (-(sum(raw[:-1]))) & 0xFF
                raw[-1] = chk
                line = ":" + raw.hex().upper()
        elif rectype == 4:
            upper = ((raw[4] << 8) | raw[5]) << 16
        elif rectype == 2:
            upper = ((raw[4] << 8) | raw[5]) << 4
        out_lines.append(line)

    dst_hex.parent.mkdir(parents=True, exist_ok=True)
    dst_hex.write_text("\n".join(out_lines) + "\n", encoding="ascii")


def generate_batch(
    src_hex: Path,
    out_dir: Path,
    count: int = 20,
    prefix: str = "Egen_superknapp",
) -> list[ButtonProvisioningConfig]:
    """Generate a batch of unique provisioned Intel HEX files."""
    results: list[ButtonProvisioningConfig] = []
    for idx in range(1, count + 1):
        cfg = make_button_config(idx, prefix=prefix)
        hex_out = out_dir / f"{cfg.device_name}.hex"
        patch_intel_hex(src_hex, hex_out, cfg)
        results.append(
            ButtonProvisioningConfig(
                index=cfg.index,
                device_name=cfg.device_name,
                bt_name=cfg.bt_name,
                serial_number=cfg.serial_number,
                discriminator=cfg.discriminator,
                passcode=cfg.passcode,
                manual_pairing_code=cfg.manual_pairing_code,
                qr_payload=cfg.qr_payload,
                verifier_b64=cfg.verifier_b64,
                hex_path=hex_out,
            )
        )
    return results
