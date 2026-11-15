/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstdint>

/**
 * @brief Operating mode for rotational control.
 */
enum class RotaryControlMode : uint8_t {
	/** Default mode: turning adjusts light brightness (positions 1..100). */
	Brightness = 0,
	/** Double-click mode: turning adjusts light color temperature (positions 101..200). */
	ColorTemp = 1,
};

/**
 * @brief Linear 3D swipe gesture for roller shutter control when removed from the wall.
 */
enum class SwipeGesture : uint8_t {
	None = 0,
	/** Vertical upward swipe -> Open / raise roller shutters (position 201). */
	Up = 1,
	/** Vertical downward swipe -> Close / lower roller shutters (position 202). */
	Down = 2,
	/** Horizontal swipe in any direction -> Stop roller shutters (position 203). */
	Horizontal = 3,
};

/**
 * @brief Result of processing a single 3-axis accelerometer sample.
 */
struct RotaryUpdate {
	/** True if a rotary step should be transmitted immediately. */
	bool hasStep = false;
	/** True if the board was tilted flat/horizontally and rotary dimming is paused. */
	bool stopDimming = false;
	/** True if rotational or linear movement above the noise floor was detected. */
	bool movementDetected = false;
	/** Active rotary control mode (Brightness or ColorTemp). */
	RotaryControlMode mode = RotaryControlMode::Brightness;
	/** Detected linear swipe gesture (None, Up, Down, or Horizontal). */
	SwipeGesture swipe = SwipeGesture::None;
	/** Signed relative rotary change in percent (-100 to +100). */
	int8_t stepPercent = 0;
	/** Encoded Switch cluster position: 1..100 Brightness, 101..200 ColorTemp, 201..203 Shutter Swipe. */
	uint8_t encodedPosition = 0;
	/** Signed relative change in Matter LevelControl units (-254 to +254). */
	int16_t stepLevel = 0;
	/** Clamped absolute brightness level (1 to 254) for absolute level bindings. */
	uint8_t targetLevel = 127;
};

/**
 * @brief Stateful rotational dimmer and 3D linear swipe detector for a 3-axis accelerometer.
 *
 * Converts gravity vector rotation in the X-Y plane into live, rate-limited relative
 * updates for brightness/color-temperature, and decomposes 3D linear acceleration
 * relative to the tracked gravity vector to detect orientation-independent Up, Down,
 * and Horizontal swipe gestures for roller shutters.
 */
class RotaryDimmer {
public:
	static constexpr double kPi = 3.14159265358979323846;
	static constexpr double kStandardGravity = 9.81;
	/** Z-axis threshold (m/s^2) above which the button is considered horizontal/flat. */
	static constexpr double kHorizontalZLimit = 7.5;
	/** Minimum squared magnitude (m^2/s^4) in the X-Y plane required for reliable angle calculation. */
	static constexpr double kMinXYMagnitudeSq = 9.0;
	/** Per-sample angular deadband (radians, ~2.0 deg) to reject accelerometer noise at rest. */
	static constexpr double kNoiseDeadbandRad = 0.035;
	/** Minimum accumulated angle (radians, ~8.0 deg) required to emit a live step while turning. */
	static constexpr double kLiveStepAngleRad = 0.14;
	/** Minimum residual angle (radians, ~2.9 deg) flushed after movement settles. */
	static constexpr double kSettleStepAngleRad = 0.05;
	/** Minimum interval (ms) between live Matter transmissions during continuous rotation. */
	static constexpr uint32_t kLiveSendIntervalMs = 150;
	/** Settling time (ms) after rotation stops before flushing any remaining angle delta. */
	static constexpr uint32_t kSettlingTimeMs = 150;
	/** Maps pi radians (180-degree half turn) to full 254 Matter range (~81 units/rad). */
	static constexpr double kRotationToLevelMultiplier = 81.0;
	/** Offset added to Switch cluster position when in ColorTemp mode (101..200). */
	static constexpr uint8_t kColorTempPositionOffset = 100;

	/** Linear acceleration threshold (m/s^2) to trigger a vertical or horizontal swipe. */
	static constexpr double kSwipeAccelThreshold = 4.5;
	/** Linear acceleration threshold (m/s^2) below which gravity low-pass filter updates and swipe re-arms. */
	static constexpr double kLinearRestThreshold = 2.0;
	/** Cooldown window (ms) after a swipe to reject braking/deceleration rebound spikes. */
	static constexpr uint32_t kSwipeCooldownMs = 600;

	/** Switch cluster position emitted on a vertical upward swipe (Shutters Up). */
	static constexpr uint8_t kShutterUpPosition = 201;
	/** Switch cluster position emitted on a vertical downward swipe (Shutters Down). */
	static constexpr uint8_t kShutterDownPosition = 202;
	/** Switch cluster position emitted on a horizontal swipe (Shutters Stop). */
	static constexpr uint8_t kShutterStopPosition = 203;

	/**
	 * @brief Reset the angle baseline when waking the accelerometer for a new session.
	 */
	void ResetOnWake();

	/**
	 * @brief Set the active rotary control mode (Brightness or ColorTemp).
	 */
	void SetControlMode(RotaryControlMode mode) { mControlMode = mode; }

	/**
	 * @brief Get the active rotary control mode (Brightness or ColorTemp).
	 */
	RotaryControlMode GetControlMode() const { return mControlMode; }

	/**
	 * @brief Process a 3-axis accelerometer reading (in m/s^2) at timestamp nowMs.
	 *
	 * @param x X-axis acceleration in m/s^2.
	 * @param y Y-axis acceleration in m/s^2.
	 * @param z Z-axis acceleration in m/s^2.
	 * @param nowMs Monotonic system uptime in milliseconds.
	 * @return RotaryUpdate describing any rotary step or linear swipe gesture.
	 */
	RotaryUpdate ProcessSample(double x, double y, double z, uint32_t nowMs);

	/**
	 * @brief Return the current tracked absolute brightness level (1..254).
	 */
	uint8_t GetTargetLevel() const { return static_cast<uint8_t>(mAccumulatedBrightness); }

private:
	/**
	 * @brief Normalize an angle difference in radians into (-pi, pi].
	 */
	static double NormalizeAngle(double angleRad);

	/**
	 * @brief Populate rotary step fields in outUpdate from mPendingAngleDelta and clear pending delta.
	 */
	void FlushPendingDelta(uint32_t nowMs, RotaryUpdate &outUpdate);

	RotaryControlMode mControlMode = RotaryControlMode::Brightness;
	double mLastAngle = 0.0;
	double mPendingAngleDelta = 0.0;
	double mAccumulatedBrightness = 127.0;
	double mGravityX = kStandardGravity;
	double mGravityY = 0.0;
	double mGravityZ = 0.0;
	uint32_t mLastMovementTimeMs = 0;
	uint32_t mLastSendTimeMs = 0;
	uint32_t mSwipeCooldownUntilMs = 0;
	bool mFirstSampleAfterWake = true;
	bool mSwipeArmed = true;
};
