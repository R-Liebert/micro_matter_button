#!/usr/bin/env python3
"""Fast continuous SWD detector and instant in-process flasher for HolyIoT nRF54L15 buttons."""

from __future__ import annotations

from pathlib import Path
import sys
import time

from pyocd.core.helpers import ConnectHelper
from pyocd.flash.file_programmer import FileProgrammer
from pyocd.probe.pydapaccess import DAPAccess


def main() -> int:
    """Poll the SWD probe at 50 Hz and flash the target hex in-process on contact."""
    btn_num = int(sys.argv[1]) if len(sys.argv) > 1 else 2
    repo_dir = Path(__file__).resolve().parent
    hex_path = repo_dir / "build_hex" / "variants" / f"Egen_superknapp_{btn_num:02d}.hex"
    if not hex_path.exists():
        print(f"Error: {hex_path} not found.")
        return 1

    print(f"=== Fast 50 Hz Auto-Flasher armed for Button {btn_num:02d} ({hex_path.name}) ===")
    print("Hold the pogo clip on the pads. Probing 50 times/sec...")

    dap = None
    last_status = ""

    while True:
        try:
            if dap is None:
                devs = DAPAccess.get_connected_devices()
                if not devs:
                    status = "[USB] Waiting for Nano RP2040 Connect probe..."
                    if status != last_status:
                        print(status)
                        last_status = status
                    time.sleep(0.2)
                    continue
                dap = devs[0]
                dap.open()
                dap.connect(DAPAccess.PORT.SWD)
                dap.set_clock(400000)

            # Send official ADIv6 Dormant-to-SWD + Line Reset sequence
            dap.swj_sequence(51, 0xFFFFFFFFFFFFFF)
            dap.swj_sequence(31, 0x33BBFFFF)
            dap.swj_sequence(8, 0xFF)
            dap.swj_sequence(128, 0x19BC0EA2E3DDAFE986852D956209F392)
            dap.swj_sequence(12, 0x01A0)
            dap.swj_sequence(51, 0xFFFFFFFFFFFFFF)
            dap.swj_sequence(8, 0x00)

            dpidr = dap.read_reg(DAPAccess.REG.DP_0x0, now=True)
            print(f"\a\n>>> TARGET LOCKED! DPIDR = 0x{dpidr:08X} — HOLD STEADY, FLASHING NOW... <<<")
            dap.close()
            dap = None

            with ConnectHelper.session_with_chosen_probe(
                target_override="nrf54l",
                frequency=1000000,
                blocking=False,
            ) as session:
                FileProgrammer(session).program(str(hex_path))
                session.target.reset()

            print(f"\a\n=== SUCCESS! {hex_path.name} FLASHED AND RESET! ===")
            return 0

        except KeyboardInterrupt:
            print("\nStopped by user.")
            if dap is not None:
                try:
                    dap.close()
                except Exception:
                    pass
            return 130
        except Exception as exc:
            msg = str(exc)
            if "Unexpected ACK (0)" in msg:
                status = "State: [ACK=0] SWDIO pulled LOW (check D10/D11 order or VCC power)"
            elif "No ACK" in msg:
                status = "State: [No ACK] Waiting for all 4 pogo pins to touch pads..."
            else:
                status = f"State: [{type(exc).__name__}] {msg}"
                if dap is not None:
                    try:
                        dap.close()
                    except Exception:
                        pass
                    dap = None
            if status != last_status:
                print(status)
                last_status = status
            time.sleep(0.015)


if __name__ == "__main__":
    raise SystemExit(main())
