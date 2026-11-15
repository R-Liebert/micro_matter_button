"""Unit tests for the C++ RotaryDimmer engine and firmware configuration."""

import ctypes
import math
from pathlib import Path
import subprocess
import pytest


REPO_ROOT = Path(__file__).resolve().parent.parent


class RotaryUpdateC(ctypes.Structure):
    """C-compatible representation of the C++ RotaryUpdate struct."""

    _fields_ = [
        ("hasStep", ctypes.c_bool),
        ("stopDimming", ctypes.c_bool),
        ("movementDetected", ctypes.c_bool),
        ("mode", ctypes.c_uint8),
        ("stepPercent", ctypes.c_int8),
        ("encodedPosition", ctypes.c_uint8),
        ("stepLevel", ctypes.c_int16),
        ("targetLevel", ctypes.c_uint8),
    ]


class RotaryDimmerWrapper:
    """Python wrapper around the compiled C++ RotaryDimmer class."""

    def __init__(self, lib: ctypes.CDLL) -> None:
        """Initialize a new native RotaryDimmer instance."""
        self._lib = lib
        self._handle = self._lib.rotary_dimmer_create()

    def close(self) -> None:
        """Release the native RotaryDimmer instance."""
        if self._handle:
            self._lib.rotary_dimmer_destroy(self._handle)
            self._handle = None

    def reset(self) -> None:
        """Reset dimmer baseline state for a new wake-on-turn session."""
        self._lib.rotary_dimmer_reset(self._handle)

    def set_mode(self, mode: int) -> None:
        """Set control mode: 0 for Brightness, 1 for ColorTemp."""
        self._lib.rotary_dimmer_set_mode(self._handle, ctypes.c_uint8(mode))

    def process(self, x: float, y: float, z: float, now_ms: int) -> RotaryUpdateC:
        """Process a 3-axis accelerometer sample (m/s^2) at timestamp now_ms."""
        return self._lib.rotary_dimmer_process(
            self._handle,
            ctypes.c_double(x),
            ctypes.c_double(y),
            ctypes.c_double(z),
            ctypes.c_uint32(now_ms),
        )

    def process_angle_deg(
        self, angle_deg: float, now_ms: int, gravity: float = 9.81, z: float = 0.0
    ) -> RotaryUpdateC:
        """Simulate a vertical wall-mounted knob rotated to angle_deg at timestamp now_ms."""
        rad = math.radians(angle_deg)
        x = gravity * math.cos(rad)
        y = gravity * math.sin(rad)
        return self.process(x, y, z, now_ms)


@pytest.fixture(scope="session")
def compiled_dimmer_lib(tmp_path_factory: pytest.TempPathFactory) -> ctypes.CDLL:
    """Compile src/rotary_dimmer.cpp into a shared library and configure ctypes signatures."""
    build_dir = tmp_path_factory.mktemp("native_build")
    so_path = build_dir / "librotary_dimmer.so"
    cpp_path = REPO_ROOT / "src" / "rotary_dimmer.cpp"

    subprocess.run(
        [
            "g++",
            "-std=c++17",
            "-O2",
            "-shared",
            "-fPIC",
            str(cpp_path),
            "-o",
            str(so_path),
        ],
        check=True,
    )

    lib = ctypes.CDLL(str(so_path))
    lib.rotary_dimmer_create.restype = ctypes.c_void_p
    lib.rotary_dimmer_destroy.argtypes = [ctypes.c_void_p]
    lib.rotary_dimmer_destroy.restype = None
    lib.rotary_dimmer_reset.argtypes = [ctypes.c_void_p]
    lib.rotary_dimmer_reset.restype = None
    lib.rotary_dimmer_set_mode.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
    lib.rotary_dimmer_set_mode.restype = None
    lib.rotary_dimmer_process.argtypes = [
        ctypes.c_void_p,
        ctypes.c_double,
        ctypes.c_double,
        ctypes.c_double,
        ctypes.c_uint32,
    ]
    lib.rotary_dimmer_process.restype = RotaryUpdateC
    return lib


@pytest.fixture()
def dimmer(compiled_dimmer_lib: ctypes.CDLL) -> RotaryDimmerWrapper:
    """Provide a fresh RotaryDimmerWrapper instance for each test."""
    instance = RotaryDimmerWrapper(compiled_dimmer_lib)
    yield instance
    instance.close()


def test_wake_baseline_does_not_emit_step(dimmer: RotaryDimmerWrapper) -> None:
    """Verify the first sample after wake captures the baseline angle without emitting a step."""
    res = dimmer.process_angle_deg(45.0, now_ms=100)
    assert not res.hasStep
    assert not res.stopDimming
    assert not res.movementDetected


def test_live_updates_emitted_while_turning_clockwise(dimmer: RotaryDimmerWrapper) -> None:
    """Verify continuous clockwise rotation emits live positive steps in Brightness mode (1..100)."""
    dimmer.process_angle_deg(0.0, now_ms=0)

    live_updates = []
    for tick in range(1, 13):
        res = dimmer.process_angle_deg(tick * 5.0, now_ms=tick * 50)
        assert res.movementDetected
        if res.hasStep:
            live_updates.append((res.stepPercent, res.encodedPosition, res.mode))

    assert len(live_updates) >= 3
    for step_pct, encoded_pos, mode in live_updates:
        assert mode == 0
        assert step_pct > 0
        assert encoded_pos == step_pct
        assert 1 <= encoded_pos <= 100


def test_live_updates_emitted_while_turning_counter_clockwise(dimmer: RotaryDimmerWrapper) -> None:
    """Verify continuous counter-clockwise rotation emits live negative steps while turning."""
    dimmer.process_angle_deg(90.0, now_ms=0)

    live_updates = []
    for tick in range(1, 13):
        res = dimmer.process_angle_deg(90.0 - tick * 5.0, now_ms=tick * 50)
        assert res.movementDetected
        if res.hasStep:
            live_updates.append((res.stepPercent, res.encodedPosition))

    assert len(live_updates) >= 3
    for step_pct, encoded_pos in live_updates:
        assert step_pct < 0
        assert encoded_pos == abs(step_pct)


def test_color_temperature_mode_encodes_positions_101_to_200(dimmer: RotaryDimmerWrapper) -> None:
    """Verify double-click ColorTemp mode encodes positions in 101..200 and preserves brightness."""
    dimmer.set_mode(1)
    dimmer.reset()
    initial = dimmer.process_angle_deg(0.0, now_ms=0)
    initial_brightness = initial.targetLevel

    res_cw = None
    for tick in range(1, 5):
        res = dimmer.process_angle_deg(tick * 6.0, now_ms=tick * 50)
        if res.hasStep:
            res_cw = res
            break

    assert res_cw is not None
    assert res_cw.mode == 1
    assert res_cw.stepPercent > 0
    assert res_cw.encodedPosition == 100 + res_cw.stepPercent
    assert 101 <= res_cw.encodedPosition <= 200
    assert res_cw.targetLevel == initial_brightness


def test_settle_flushes_residual_rotation(dimmer: RotaryDimmerWrapper) -> None:
    """Verify a small sub-interval rotation is flushed once movement settles for 150ms."""
    dimmer.process_angle_deg(0.0, now_ms=0)
    res_move = dimmer.process_angle_deg(6.0, now_ms=50)
    assert res_move.movementDetected
    assert not res_move.hasStep

    res_wait = dimmer.process_angle_deg(6.0, now_ms=150)
    assert not res_wait.hasStep

    res_settled = dimmer.process_angle_deg(6.0, now_ms=210)
    assert res_settled.hasStep
    assert res_settled.stepPercent > 0


def test_angle_wrap_around_pi_boundary(dimmer: RotaryDimmerWrapper) -> None:
    """Verify crossing the +180/-180 degree boundary does not invert rotation direction."""
    dimmer.process_angle_deg(170.0, now_ms=0)
    dimmer.process_angle_deg(178.0, now_ms=50)
    dimmer.process_angle_deg(-174.0, now_ms=100)
    res = dimmer.process_angle_deg(-165.0, now_ms=160)

    assert res.hasStep
    assert res.stepPercent > 0
    assert res.stepLevel > 0


def test_noise_deadband_rejection(dimmer: RotaryDimmerWrapper) -> None:
    """Verify sensor jitter smaller than ~2 degrees is ignored as stationary noise."""
    dimmer.process_angle_deg(30.0, now_ms=0)
    for tick in range(1, 10):
        jitter = 0.8 if (tick % 2 == 0) else -0.8
        res = dimmer.process_angle_deg(30.0 + jitter, now_ms=tick * 50)
        assert not res.movementDetected
        assert not res.hasStep


def test_horizontal_tilt_stops_dimming_and_flushes(dimmer: RotaryDimmerWrapper) -> None:
    """Verify tilting the board flat (|z| > 7.5 m/s^2) flushes pending delta and stops dimming."""
    dimmer.process_angle_deg(0.0, now_ms=0)
    dimmer.process_angle_deg(7.0, now_ms=50)

    res = dimmer.process(x=1.0, y=1.0, z=9.2, now_ms=100)
    assert res.stopDimming
    assert res.hasStep
    assert res.stepPercent > 0


def test_low_xy_gravity_magnitude_ignored(dimmer: RotaryDimmerWrapper) -> None:
    """Verify weak X-Y gravity magnitude (< 3 m/s^2) does not emit false steps."""
    dimmer.process_angle_deg(0.0, now_ms=0)
    res = dimmer.process(x=1.0, y=1.0, z=2.0, now_ms=200)
    assert not res.hasStep
    assert not res.movementDetected


def test_firmware_and_ci_configuration() -> None:
    """Verify prj.conf, CMakeLists.txt, and GitHub Actions workflow are properly configured."""
    prj_conf = (REPO_ROOT / "prj.conf").read_text()
    assert "CONFIG_LIS2DH_TRIGGER_NONE=y" in prj_conf

    cmake_txt = (REPO_ROOT / "CMakeLists.txt").read_text()
    assert "src/rotary_dimmer.cpp" in cmake_txt
    assert "list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})" in cmake_txt

    app_task = (REPO_ROOT / "src" / "app_task.cpp").read_text()
    assert "EnterAccelIdleMode" in app_task
    assert "StartAccelDimming" in app_task
    assert "EmitRotationStep" in app_task
    assert "RotaryControlMode::ColorTemp" in app_task

    workflow = (REPO_ROOT / ".github" / "workflows" / "build.yml").read_text()
    assert "micro_matter_button_h25015_om1.hex" in workflow
    assert "pytest" in workflow
