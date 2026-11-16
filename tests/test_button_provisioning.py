"""Unit tests for the Matter button provisioning and hex patching module."""

from pathlib import Path
import struct
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from src.button_provisioning import (
    DEFAULT_PASSCODE,
    DEFAULT_VERIFIER_B64,
    compute_manual_pairing_code,
    compute_qr_payload,
    compute_spake2p_verifier_b64,
    encode_thumb2_movw_r3,
    generate_batch,
    make_button_config,
)


def test_default_matter_credentials_match_reference() -> None:
    """Verify default passcode and discriminator match the stock Matter vectors."""
    assert compute_manual_pairing_code(3840, DEFAULT_PASSCODE) == "34970112332"
    assert compute_qr_payload(3840, DEFAULT_PASSCODE) == "MT:4CT9142C00KA0648G00"
    assert compute_spake2p_verifier_b64(DEFAULT_PASSCODE).encode("ascii") == DEFAULT_VERIFIER_B64


def test_20_button_configs_are_all_unique() -> None:
    """Ensure all 20 generated button configurations have distinct names and pairing codes."""
    configs = [make_button_config(i) for i in range(1, 21)]
    assert len({c.device_name for c in configs}) == 20
    assert len({c.bt_name for c in configs}) == 20
    assert len({c.serial_number for c in configs}) == 20
    assert len({c.manual_pairing_code for c in configs}) == 20
    assert len({c.qr_payload for c in configs}) == 20
    assert len({c.verifier_b64 for c in configs}) == 20
    assert configs[0].device_name == "Egen_superknapp_01"
    assert configs[-1].device_name == "Egen_superknapp_20"


def test_encode_thumb2_movw_r3() -> None:
    """Verify Thumb-2 MOVW encoding for 12-bit and 16-bit immediates."""
    encoded = encode_thumb2_movw_r3(0x0ABC)
    hw1, hw2 = struct.unpack("<HH", encoded)
    assert hw1 == 0xF640
    assert hw2 == 0x23BC
    with pytest.raises(ValueError):
        encode_thumb2_movw_r3(0x10000)


def test_patch_and_generate_batch_on_real_hex(tmp_path: Path) -> None:
    """Verify patching against the actual HolyIoT 25015 hex image."""
    src_hex = Path(__file__).resolve().parent.parent / "build_hex" / "micro_matter_button_h25015_om1.hex"
    batch = generate_batch(src_hex, tmp_path, count=2)
    assert len(batch) == 2
    for cfg in batch:
        assert cfg.hex_path is not None and cfg.hex_path.exists()
        raw_text = cfg.hex_path.read_text(encoding="ascii")
        assert raw_text.startswith(":")
        # Verify Intel HEX checksums on every record
        for line in raw_text.splitlines():
            if not line.startswith(":"):
                continue
            rec = bytes.fromhex(line[1:])
            assert (sum(rec) & 0xFF) == 0
