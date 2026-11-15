/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "rotary_dimmer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

void RotaryDimmer::ResetOnWake()
{
	mFirstSampleAfterWake = true;
	mPendingAngleDelta = 0.0;
	mSwipeArmed = true;
}

double RotaryDimmer::NormalizeAngle(double angleRad)
{
	while (angleRad > kPi) {
		angleRad -= 2.0 * kPi;
	}
	while (angleRad <= -kPi) {
		angleRad += 2.0 * kPi;
	}
	return angleRad;
}

void RotaryDimmer::FlushPendingDelta(uint32_t nowMs, RotaryUpdate &outUpdate)
{
	double rawLevelDelta = mPendingAngleDelta * kRotationToLevelMultiplier;
	int16_t stepLevel = static_cast<int16_t>(std::round(rawLevelDelta));
	if (stepLevel == 0) {
		stepLevel = (mPendingAngleDelta > 0.0) ? 1 : -1;
	}
	stepLevel = std::clamp<int16_t>(stepLevel, -254, 254);

	double rawPercentDelta = (static_cast<double>(stepLevel) * 100.0) / 254.0;
	int8_t stepPercent = static_cast<int8_t>(std::round(rawPercentDelta));
	if (stepPercent == 0) {
		stepPercent = (stepLevel > 0) ? 1 : -1;
	}
	stepPercent = std::clamp<int8_t>(stepPercent, -100, 100);

	uint8_t magnitudePct = static_cast<uint8_t>(std::clamp<int>(std::abs(stepPercent), 1, 100));
	uint8_t offset = (mControlMode == RotaryControlMode::ColorTemp) ? kColorTempPositionOffset : 0U;

	outUpdate.hasStep = true;
	outUpdate.mode = mControlMode;
	outUpdate.stepLevel = stepLevel;
	outUpdate.stepPercent = stepPercent;
	outUpdate.encodedPosition = static_cast<uint8_t>(magnitudePct + offset);
	outUpdate.targetLevel = static_cast<uint8_t>(std::round(mAccumulatedBrightness));

	mPendingAngleDelta = 0.0;
	mLastSendTimeMs = nowMs;
}

RotaryUpdate RotaryDimmer::ProcessSample(double x, double y, double z, uint32_t nowMs)
{
	RotaryUpdate update;
	update.mode = mControlMode;
	update.targetLevel = static_cast<uint8_t>(std::round(mAccumulatedBrightness));

	double totalNorm = std::sqrt((x * x) + (y * y) + (z * z));
	double currentAngle = std::atan2(y, x);

	if (mFirstSampleAfterWake) {
		mLastAngle = currentAngle;
		mFirstSampleAfterWake = false;
		mPendingAngleDelta = 0.0;
		mLastMovementTimeMs = nowMs;
		mLastSendTimeMs = nowMs;
		if (totalNorm >= 7.0 && totalNorm <= 12.5) {
			mGravityX = x;
			mGravityY = y;
			mGravityZ = z;
			return update;
		}
	}

	/* Decompose 3D linear acceleration relative to the tracked upward gravity vector */
	double gNorm = std::sqrt((mGravityX * mGravityX) + (mGravityY * mGravityY) + (mGravityZ * mGravityZ));
	double linX = x - mGravityX;
	double linY = y - mGravityY;
	double linZ = z - mGravityZ;
	double aVert = 0.0;
	double aHoriz = 0.0;

	if (gNorm >= 3.0) {
		double ux = mGravityX / gNorm;
		double uy = mGravityY / gNorm;
		double uz = mGravityZ / gNorm;
		aVert = (linX * ux) + (linY * uy) + (linZ * uz);
		double hx = linX - (aVert * ux);
		double hy = linY - (aVert * uy);
		double hz = linZ - (aVert * uz);
		aHoriz = std::sqrt((hx * hx) + (hy * hy) + (hz * hz));
	}

	bool hasLinearImpulse = (totalNorm >= 2.8) && (std::abs(totalNorm - kStandardGravity) >= 0.8);

	/* Check for 3D linear swipe gesture (Up, Down, or Horizontal) when linear impulse is present */
	if (hasLinearImpulse && nowMs >= mSwipeCooldownUntilMs && mSwipeArmed) {
		if (std::abs(aVert) >= kSwipeAccelThreshold && std::abs(aVert) >= aHoriz) {
			update.swipe = (aVert > 0.0) ? SwipeGesture::Up : SwipeGesture::Down;
			update.encodedPosition =
				(aVert > 0.0) ? kShutterUpPosition : kShutterDownPosition;
		} else if (aHoriz >= kSwipeAccelThreshold && aHoriz > std::abs(aVert)) {
			update.swipe = SwipeGesture::Horizontal;
			update.encodedPosition = kShutterStopPosition;
		}

		if (update.swipe != SwipeGesture::None) {
			update.movementDetected = true;
			mSwipeCooldownUntilMs = nowMs + kSwipeCooldownMs;
			mSwipeArmed = false;
			mPendingAngleDelta = 0.0;
			mLastAngle = currentAngle;
			mLastMovementTimeMs = nowMs;
			return update;
		}
	}

	/* Update gravity vector when in 1g condition (no linear acceleration) */
	if (!hasLinearImpulse) {
		mGravityX = x;
		mGravityY = y;
		mGravityZ = z;
		if (nowMs >= mSwipeCooldownUntilMs) {
			mSwipeArmed = true;
		}
	} else if (std::abs(totalNorm - kStandardGravity) < kLinearRestThreshold) {
		constexpr double kAlpha = 0.25;
		mGravityX = ((1.0 - kAlpha) * mGravityX) + (kAlpha * x);
		mGravityY = ((1.0 - kAlpha) * mGravityY) + (kAlpha * y);
		mGravityZ = ((1.0 - kAlpha) * mGravityZ) + (kAlpha * z);
		if (nowMs >= mSwipeCooldownUntilMs) {
			mSwipeArmed = true;
		}
	}

	/* Suppress rotary dimming while in swipe cooldown or experiencing linear acceleration */
	if (nowMs < mSwipeCooldownUntilMs || std::abs(totalNorm - kStandardGravity) >= 2.5) {
		mLastAngle = currentAngle;
		mPendingAngleDelta = 0.0;
		return update;
	}

	if (std::abs(z) > kHorizontalZLimit) {
		if (std::abs(mPendingAngleDelta) >= kSettleStepAngleRad) {
			FlushPendingDelta(nowMs, update);
		}
		update.stopDimming = true;
		return update;
	}

	double magnitudeSq = (x * x) + (y * y);
	if (magnitudeSq < kMinXYMagnitudeSq) {
		mLastAngle = currentAngle;
		mLastMovementTimeMs = nowMs;
		return update;
	}

	double deltaAngle = NormalizeAngle(currentAngle - mLastAngle);

	if (std::abs(deltaAngle) >= kNoiseDeadbandRad) {
		mLastAngle = currentAngle;
		mLastMovementTimeMs = nowMs;
		mPendingAngleDelta += deltaAngle;
		if (mControlMode == RotaryControlMode::Brightness) {
			mAccumulatedBrightness = std::clamp(
				mAccumulatedBrightness + (deltaAngle * kRotationToLevelMultiplier), 1.0, 254.0);
		}
		update.targetLevel = static_cast<uint8_t>(std::round(mAccumulatedBrightness));
		update.movementDetected = true;

		if ((nowMs - mLastSendTimeMs) >= kLiveSendIntervalMs &&
		    std::abs(mPendingAngleDelta) >= kLiveStepAngleRad) {
			FlushPendingDelta(nowMs, update);
		}
	} else {
		if ((nowMs - mLastMovementTimeMs) >= kSettlingTimeMs &&
		    std::abs(mPendingAngleDelta) >= kSettleStepAngleRad) {
			FlushPendingDelta(nowMs, update);
		}
	}

	return update;
}

extern "C" {

/**
 * @brief C-ABI wrapper to allocate a RotaryDimmer instance for unit testing.
 */
RotaryDimmer *rotary_dimmer_create()
{
	return new RotaryDimmer();
}

/**
 * @brief C-ABI wrapper to destroy a RotaryDimmer instance.
 */
void rotary_dimmer_destroy(RotaryDimmer *dimmer)
{
	delete dimmer;
}

/**
 * @brief C-ABI wrapper to reset a RotaryDimmer instance on wake.
 */
void rotary_dimmer_reset(RotaryDimmer *dimmer)
{
	if (dimmer) {
		dimmer->ResetOnWake();
	}
}

/**
 * @brief C-ABI wrapper to set the control mode (0 = Brightness, 1 = ColorTemp).
 */
void rotary_dimmer_set_mode(RotaryDimmer *dimmer, uint8_t mode)
{
	if (dimmer) {
		dimmer->SetControlMode(
			mode == 1 ? RotaryControlMode::ColorTemp : RotaryControlMode::Brightness);
	}
}

/**
 * @brief C-ABI wrapper to process a sample and populate a RotaryUpdate struct.
 */
RotaryUpdate rotary_dimmer_process(RotaryDimmer *dimmer, double x, double y, double z, uint32_t nowMs)
{
	if (!dimmer) {
		return RotaryUpdate{};
	}
	return dimmer->ProcessSample(x, y, z, nowMs);
}

} /* extern "C" */
