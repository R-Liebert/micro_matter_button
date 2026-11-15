/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <cstdint>

/**
 * @brief Result of processing a single 3-axis accelerometer sample during rotational dimming.
 */
struct RotaryUpdate {
	/** True if a brightness step should be transmitted immediately. */
	bool hasStep = false;
	/** True if the board was tilted flat/horizontally and dimming should stop. */
	bool stopDimming = false;
	/** True if rotational movement above the noise floor was detected on this sample. */
	bool movementDetected = false;
	/** Signed relative brightness change in percent (-100 to +100). */
	int8_t stepPercent = 0;
	/** Signed relative brightness change in Matter LevelControl units (-254 to +254). */
	int16_t stepLevel = 0;
	/** Clamped absolute brightness level (1 to 254) for absolute level bindings. */
	uint8_t targetLevel = 127;
};

/**
 * @brief Stateful rotational dimming calculator for a vertically mounted 3-axis accelerometer.
 *
 * Converts gravity vector rotation in the X-Y plane into live, rate-limited relative
 * and absolute brightness updates without waiting for user rotation to stop.
 */
class RotaryDimmer {
public:
	static constexpr double kPi = 3.14159265358979323846;
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
	/** Minimum interval (ms) between live Matter brightness transmissions during continuous rotation. */
	static constexpr uint32_t kLiveSendIntervalMs = 150;
	/** Settling time (ms) after rotation stops before flushing any remaining angle delta. */
	static constexpr uint32_t kSettlingTimeMs = 150;
	/** Maps pi radians (180-degree half turn) to full 254 Matter brightness range (~81 units/rad). */
	static constexpr double kRotationToLevelMultiplier = 81.0;

	/**
	 * @brief Reset the dimmer state when waking the accelerometer for a new rotation session.
	 */
	void ResetOnWake();

	/**
	 * @brief Process a 3-axis accelerometer reading (in m/s^2) at timestamp nowMs.
	 *
	 * @param x X-axis acceleration in m/s^2.
	 * @param y Y-axis acceleration in m/s^2.
	 * @param z Z-axis acceleration in m/s^2.
	 * @param nowMs Monotonic system uptime in milliseconds.
	 * @return RotaryUpdate describing whether to send a dimming step, extend wake, or stop.
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
	 * @brief Populate step fields in outUpdate from mPendingAngleDelta and clear pending delta.
	 */
	void FlushPendingDelta(uint32_t nowMs, RotaryUpdate &outUpdate);

	double mLastAngle = 0.0;
	double mPendingAngleDelta = 0.0;
	double mAccumulatedBrightness = 127.0;
	uint32_t mLastMovementTimeMs = 0;
	uint32_t mLastSendTimeMs = 0;
	bool mFirstSampleAfterWake = true;
};
