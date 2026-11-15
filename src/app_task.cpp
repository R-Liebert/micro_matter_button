/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "app_task.h"

#include "light_switch.h"
#include "rotary_dimmer.h"

#include "app/matter_init.h"
#include "app/task_executor.h"
#include "board/board.h"
#include "clusters/identify.h"

#include <setup_payload/OnboardingCodesUtil.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/clusters/switch-server/switch-server.h>
#include <app/server/Server.h>
#include <app/util/endpoint-config-api.h>
#include <platform/CHIPDeviceLayer.h>

#include <dk_buttons_and_leds.h>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

/* --- Raw SPI & INT1 GPIO for LIS2DH12 accelerometer ---------------------- */

static const struct spi_dt_spec sAccelBus =
	SPI_DT_SPEC_GET(DT_ALIAS(accel0), SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);

static const struct gpio_dt_spec sAccelInt1Gpio =
	GPIO_DT_SPEC_GET_BY_IDX(DT_ALIAS(accel0), irq_gpios, 0);

static struct gpio_callback sAccelInt1CbData;

static void WriteAccelRegister(uint8_t reg, uint8_t val)
{
	uint8_t tx_buf[2] = { reg, val };
	struct spi_buf tx_spi_buf = { .buf = tx_buf, .len = 2 };
	struct spi_buf_set tx_spi_buf_set = { .buffers = &tx_spi_buf, .count = 1 };
	int err = spi_write_dt(&sAccelBus, &tx_spi_buf_set);
	if (err) {
		LOG_ERR("Accel SPI write failed (reg 0x%02X): %d", reg, err);
	}
}

static uint8_t ReadAccelRegister(uint8_t reg)
{
	uint8_t tx_buf[2] = { static_cast<uint8_t>(reg | 0x80U), 0x00U };
	uint8_t rx_buf[2] = { 0x00U, 0x00U };
	struct spi_buf tx_spi_buf = { .buf = tx_buf, .len = 2 };
	struct spi_buf rx_spi_buf = { .buf = rx_buf, .len = 2 };
	struct spi_buf_set tx_set = { .buffers = &tx_spi_buf, .count = 1 };
	struct spi_buf_set rx_set = { .buffers = &rx_spi_buf, .count = 1 };
	if (spi_transceive_dt(&sAccelBus, &tx_set, &rx_set) != 0) {
		return 0;
	}
	return rx_buf[1];
}

/* --- LPS22HB power-down (25015 only) ------------------------------------- */
#ifdef CONFIG_SHT4X

static const struct spi_dt_spec sLps22hbBus = {
	.bus = DEVICE_DT_GET(DT_BUS(DT_NODELABEL(lps22hb))),
	.config = {
		.frequency = 10000000U,
		.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_LINES_SINGLE,
		.slave    = DT_REG_ADDR(DT_NODELABEL(lps22hb)),
		.cs       = SPI_CS_CONTROL_INIT(DT_NODELABEL(lps22hb), 0),
	},
};

static void WriteLps22hbRegister(uint8_t reg, uint8_t val)
{
	uint8_t tx_buf[2] = { (uint8_t)(reg & 0x3F), val };
	struct spi_buf tx_spi_buf = { .buf = tx_buf, .len = 2 };
	struct spi_buf_set tx_set = { .buffers = &tx_spi_buf, .count = 1 };
	spi_write_dt(&sLps22hbBus, &tx_set);
}

#endif /* CONFIG_SHT4X */

/* --- Constants and module-level state ------------------------------------ */
namespace
{
constexpr EndpointId kLightSwitchEndpointId = 1;
constexpr EndpointId kLightEndpointId = 1;
constexpr EndpointId kGenericSwitchEndpointId = 2;
constexpr EndpointId kTempEndpointId = 3;
constexpr EndpointId kHumEndpointId = 4;
constexpr EndpointId kPressureEndpointId = 5;

/* Accelerometer rotational dimming */
constexpr uint32_t kAccelPollIntervalMs = 50;
constexpr uint32_t kRotationWakeExtensionMs = 2500;

/* GPIO long-press dimming */
constexpr uint32_t kDimmerTriggeredTimeout = 500;
constexpr uint32_t kDimmerInterval = 300;

/* Factory reset: 12s warning, 15s trigger */
constexpr uint32_t kFactoryResetWarningMs = 12000;
constexpr uint32_t kFactoryResetTriggerMs = 15000;

/* LED blink: slow (50ms on / 950ms off) and fast (100ms on/off) */
constexpr uint32_t kSlowBlinkOnMs = 50;
constexpr uint32_t kSlowBlinkOffMs = 950;
constexpr uint32_t kFastBlinkMs = 100;
constexpr uint32_t kFactoryWarningWhiteMs = 500;

/* Sensor poll: every 5 minutes */
constexpr uint32_t kSensorPollIntervalMs = 300000;

/* --- Timers --- */
k_timer sSingleTapTimer;
k_timer sAccelPollTimer;
k_timer sAccelSleepTimer;
k_timer sDimmerTriggerTimer;
k_timer sDimmerTimer;
k_timer sLongPressTimer;
k_timer sFactoryWarningTimer;
k_timer sFactoryResetTimer;
k_timer sStatusLedBlinkTimer;
k_timer sSensorPollTimer;

Nrf::Matter::IdentifyCluster sIdentifyCluster(kLightEndpointId);

/* --- Accelerometer & dimming state --- */
const struct device *sAccelDevice = nullptr;
RotaryDimmer sRotaryDimmer;
bool sAccelPollingActive = false;
bool sAccelInt1Ready = false;
bool sDimmerTriggered = false;

/* --- Factory reset state --- */
bool sInFactoryWarning = false;
uint8_t sFactoryWarningBlinksRemaining = 0;

/* --- LED state --- */
enum class LedState { SlowBlink, FastBlink, Off };
LedState sLedState = LedState::SlowBlink;
bool sStatusLedOn = false;

/* --- Sensor devices (25015 only) --- */
#ifdef CONFIG_SHT4X
const struct device *sSht40Device = nullptr;
bool sIsSensorTimerStarted = false;
#endif

/* Button mask: on 25015 the physical button is sw0 (DK_BTN1), on 25008 it is sw1 (DK_BTN2) */
#if defined(CONFIG_BOARD_HOLYIOT_25015_NRF54L15_CPUAPP)
#define APPLICATION_BUTTON_MASK DK_BTN1_MSK
#else
#define APPLICATION_BUTTON_MASK DK_BTN2_MSK
#endif

#ifdef CONFIG_CHIP_ICD_UAT_SUPPORT
#define UAT_BUTTON_MASK DK_BTN3_MSK
#endif
} /* namespace */

/* --- Accelerometer Wake-on-Turn Interrupt -------------------------------- */

static void AccelInt1Isr(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	gpio_pin_interrupt_configure_dt(&sAccelInt1Gpio, GPIO_INT_DISABLE);
	Nrf::PostTask([] { AppTask::Instance().UpdateClusterState(); });
}

void AppTask::EnterAccelIdleMode()
{
	sAccelPollingActive = false;
	Instance().CancelTimer(Timer::AccelPoll);
	Instance().CancelTimer(Timer::AccelSleep);

	if (!sAccelDevice) {
		return;
	}

	/* Configure LIS2DH12 for 10Hz Low-Power (~3 uA) Wake-on-Turn on INT1 (P2.00):
	 * - CTRL_REG1 (0x20) = 0x2F: ODR=10Hz, LPen=1, Z/Y/X enabled
	 * - CTRL_REG2 (0x21) = 0x01: High-pass filter enabled for AOI1 (INT1), bypassed for output data
	 * - CTRL_REG3 (0x22) = 0x40: Route IA1 interrupt to INT1 pin
	 * - CTRL_REG4 (0x23) = 0x80: BDU=1, +/-2g full scale
	 * - CTRL_REG5 (0x24) = 0x00: Non-latched INT1
	 * - INT1_THS  (0x32) = 0x06: ~96 mg threshold (~6 deg rotation on vertical wall)
	 * - INT1_DUR  (0x33) = 0x00: Immediate trigger
	 * - INT1_CFG  (0x30) = 0x0A: XHIE | YHIE (wake on X or Y gravity shift)
	 */
	WriteAccelRegister(0x20, 0x2F);
	WriteAccelRegister(0x21, 0x01);
	WriteAccelRegister(0x22, 0x40);
	WriteAccelRegister(0x23, 0x80);
	WriteAccelRegister(0x24, 0x00);
	WriteAccelRegister(0x32, 0x06);
	WriteAccelRegister(0x33, 0x00);
	WriteAccelRegister(0x30, 0x0A);

	/* Dummy-read REFERENCE (0x26) to zero the high-pass filter at the current resting angle */
	(void)ReadAccelRegister(0x26);
	(void)ReadAccelRegister(0x31);

	if (sAccelInt1Ready) {
		gpio_pin_interrupt_configure_dt(&sAccelInt1Gpio, GPIO_INT_EDGE_TO_ACTIVE);
	}
}

void AppTask::StartAccelDimming()
{
	if (!sAccelDevice) {
		return;
	}

	if (sAccelInt1Ready) {
		gpio_pin_interrupt_configure_dt(&sAccelInt1Gpio, GPIO_INT_DISABLE);
	}

	if (!sAccelPollingActive) {
		LOG_INF("Wake-on-turn triggered — starting active rotation tracking.");
		/* CTRL_REG1 (0x20) = 0x57: 100Hz Normal mode, Z/Y/X enabled */
		WriteAccelRegister(0x20, 0x57);
		sRotaryDimmer.ResetOnWake();
		sAccelPollingActive = true;
		Instance().StartTimer(Timer::AccelPoll, kAccelPollIntervalMs);
	}

	Server::GetInstance().GetICDManager().OnNetworkActivity();
	Instance().StartTimer(Timer::AccelSleep, kRotationWakeExtensionMs);
}

void AppTask::EmitRotationStep(const RotaryUpdate &update)
{
	if (!update.hasStep || update.stepPercent == 0) {
		return;
	}

	Server::GetInstance().GetICDManager().OnNetworkActivity();

	uint8_t stepPct = static_cast<uint8_t>(std::clamp<int>(std::abs(update.stepPercent), 1, 100));
	chip::app::Clusters::Switch::Attributes::CurrentPosition::Set(kGenericSwitchEndpointId, stepPct);

	if (update.stepPercent > 0) {
		LOG_INF("Rotate CW: +%u%% (level +%d)", stepPct, update.stepLevel);
		chip::app::Clusters::SwitchServer::Instance().OnLongPress(kGenericSwitchEndpointId, stepPct);
	} else {
		LOG_INF("Rotate CCW: -%u%% (level %d)", stepPct, update.stepLevel);
		chip::app::Clusters::SwitchServer::Instance().OnLongRelease(kGenericSwitchEndpointId, stepPct);
	}

#ifdef CONFIG_MATTER_BUTTON_MODE_1
	LightSwitch::GetInstance().StepBrightness(update.stepLevel);
#endif
}

/* --- LED management ------------------------------------------------------ */

void AppTask::UpdateStatusLed()
{
	if (sInFactoryWarning) {
		return;
	}

	CancelTimer(Timer::StatusLedBlink);

	switch (sLedState) {
	case LedState::Off:
		dk_set_led_off(DK_LED1);
		break;
	case LedState::FastBlink:
		sStatusLedOn = false;
		StartTimer(Timer::StatusLedBlink, kFastBlinkMs);
		break;
	case LedState::SlowBlink:
		sStatusLedOn = false;
		dk_set_led_off(DK_LED1);
		StartTimer(Timer::StatusLedBlink, kSlowBlinkOffMs);
		break;
	}
}

void AppTask::MatterEventHandler(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t)
{
	static bool sIsBleConnected = false;

	switch (event->Type) {
	case DeviceEventType::kCHIPoBLEAdvertisingChange:
		sIsBleConnected = ConnectivityMgr().NumBLEConnections() != 0;
		break;
	case DeviceEventType::kCommissioningComplete:
#ifdef CONFIG_SHT4X
		if (sSht40Device && !sIsSensorTimerStarted) {
			LOG_INF("Commissioning complete — starting sensor measurements.");
			Nrf::PostTask([] { SensorUpdateEventHandler(); });
			k_timer_start(&sSensorPollTimer, K_MSEC(kSensorPollIntervalMs), K_MSEC(kSensorPollIntervalMs));
			sIsSensorTimerStarted = true;
		}
#endif
		break;
	case DeviceEventType::kThreadStateChange:
		break;
	default:
		return;
	}

	bool isCommissioned = Server::GetInstance().GetFabricTable().FabricCount() > 0;

	if (isCommissioned) {
		sLedState = LedState::Off;
	} else if (sIsBleConnected) {
		sLedState = LedState::FastBlink;
	} else {
		sLedState = LedState::SlowBlink;
	}
	Instance().UpdateStatusLed();
}

/* --- Accelerometer poll -------------------------------------------------- */

void AppTask::AccelPollEventHandler()
{
	if (!sAccelDevice) {
		return;
	}

	if (chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen()) {
		return;
	}

	struct sensor_value accel[3];
	if (sensor_sample_fetch(sAccelDevice) < 0 ||
	    sensor_channel_get(sAccelDevice, SENSOR_CHAN_ACCEL_XYZ, accel) < 0) {
		return;
	}

	double x = sensor_value_to_double(&accel[0]);
	double y = sensor_value_to_double(&accel[1]);
	double z = sensor_value_to_double(&accel[2]);

	RotaryUpdate update = sRotaryDimmer.ProcessSample(x, y, z, k_uptime_get_32());

	if (update.movementDetected) {
		Instance().StartTimer(Timer::AccelSleep, kRotationWakeExtensionMs);
	}

	if (update.hasStep) {
		EmitRotationStep(update);
	}

	if (update.stopDimming) {
		LOG_INF("Horizontal tilt — returning accelerometer to idle wake-on-turn mode.");
		EnterAccelIdleMode();
	}
}

/* --- 25015 sensor reading ------------------------------------------------ */

void AppTask::SensorUpdateEventHandler()
{
#ifdef CONFIG_SHT4X
	if (!sSht40Device) {
		return;
	}

	struct sensor_value temp_val, hum_val;

	if (sensor_sample_fetch(sSht40Device) < 0) {
		LOG_ERR("SHT40 fetch failed");
		return;
	}
	sensor_channel_get(sSht40Device, SENSOR_CHAN_AMBIENT_TEMP, &temp_val);
	sensor_channel_get(sSht40Device, SENSOR_CHAN_HUMIDITY, &hum_val);

	int16_t temp_matter = (int16_t)(sensor_value_to_double(&temp_val) * 100.0);
	uint16_t hum_matter = (uint16_t)(sensor_value_to_double(&hum_val) * 100.0);

	LOG_INF("Sensors: temp=%d (x100 C), hum=%u (x100 %%)", temp_matter, hum_matter);

	chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Set(kTempEndpointId, temp_matter);
	chip::app::Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Set(kHumEndpointId, hum_matter);
#endif /* CONFIG_SHT4X */
}

/* --- Timer event dispatcher ---------------------------------------------- */

void AppTask::TimerEventHandler(const Timer &timerType)
{
	switch (timerType) {
	case Timer::SingleTap:
		break;

	case Timer::AccelPoll:
		AccelPollEventHandler();
		break;

	case Timer::AccelSleep:
		LOG_INF("Accel inactivity timeout — returning to low-power wake-on-turn mode.");
		EnterAccelIdleMode();
		break;

	case Timer::DimmerTrigger:
		sDimmerTriggered = true;
#ifdef CONFIG_MATTER_BUTTON_MODE_1
		LOG_INF("Long press — starting bound brightness cycle.");
		LightSwitch::GetInstance().InitiateActionSwitch(LightSwitch::Action::On);
		Instance().StartTimer(Timer::Dimmer, kDimmerInterval);
#endif
		break;

	case Timer::Dimmer:
#ifdef CONFIG_MATTER_BUTTON_MODE_1
		LightSwitch::GetInstance().DimmerChangeBrightness();
#endif
		break;

	case Timer::LongPressDetect:
		break;

	case Timer::FactoryWarning:
		LOG_INF("Factory reset warning — release to cancel, or hold to reset.");
		sInFactoryWarning = true;
		sFactoryWarningBlinksRemaining = 5;
		sStatusLedOn = true;
		dk_set_led_on(DK_LED1);
		dk_set_led_on(DK_LED2);
		dk_set_led_on(DK_LED3);
		StartTimer(Timer::StatusLedBlink, kFactoryWarningWhiteMs);
		break;

	case Timer::FactoryReset:
		LOG_INF("Factory reset triggered.");
		chip::Server::GetInstance().ScheduleFactoryReset();
		break;

	case Timer::StatusLedBlink:
		if (sInFactoryWarning) {
			sStatusLedOn = !sStatusLedOn;
			if (sStatusLedOn) {
				dk_set_led_on(DK_LED1);
				dk_set_led_on(DK_LED2);
				dk_set_led_on(DK_LED3);
			} else {
				dk_set_led_off(DK_LED1);
				dk_set_led_off(DK_LED2);
				dk_set_led_off(DK_LED3);
			}
			sFactoryWarningBlinksRemaining--;
			if (sFactoryWarningBlinksRemaining > 0) {
				StartTimer(Timer::StatusLedBlink, kFactoryWarningWhiteMs);
			} else {
				sInFactoryWarning = false;
				Instance().UpdateStatusLed();
			}
		} else if (sLedState == LedState::FastBlink) {
			sStatusLedOn = !sStatusLedOn;
			sStatusLedOn ? dk_set_led_on(DK_LED1) : dk_set_led_off(DK_LED1);
			StartTimer(Timer::StatusLedBlink, kFastBlinkMs);
		} else if (sLedState == LedState::SlowBlink) {
			sStatusLedOn = !sStatusLedOn;
			sStatusLedOn ? dk_set_led_on(DK_LED1) : dk_set_led_off(DK_LED1);
			StartTimer(Timer::StatusLedBlink, sStatusLedOn ? kSlowBlinkOnMs : kSlowBlinkOffMs);
		}
		break;

	case Timer::SensorPoll:
		SensorUpdateEventHandler();
		break;

	default:
		break;
	}
}

/* --- Button handler ------------------------------------------------------ */

void AppTask::ButtonEventHandler(Nrf::ButtonState state, Nrf::ButtonMask hasChanged)
{
	if (APPLICATION_BUTTON_MASK & hasChanged) {
		if (APPLICATION_BUTTON_MASK & state) {
			/* Button pressed */
			sInFactoryWarning = false;
			Instance().CancelTimer(Timer::FactoryWarning);
			Instance().CancelTimer(Timer::FactoryReset);
			Instance().StartTimer(Timer::FactoryWarning, kFactoryResetWarningMs);
			Instance().StartTimer(Timer::FactoryReset, kFactoryResetTriggerMs);

			sDimmerTriggered = false;
			StartAccelDimming();

			chip::app::Clusters::Switch::Attributes::CurrentPosition::Set(kGenericSwitchEndpointId, 1);
			chip::app::Clusters::SwitchServer::Instance().OnInitialPress(kGenericSwitchEndpointId, 1);
			Instance().StartTimer(Timer::DimmerTrigger, kDimmerTriggeredTimeout);
		} else {
			/* Button released */
			Instance().CancelTimer(Timer::FactoryWarning);
			Instance().CancelTimer(Timer::FactoryReset);

			if (sInFactoryWarning) {
				LOG_INF("Factory reset cancelled.");
				sInFactoryWarning = false;
				sFactoryWarningBlinksRemaining = 0;
				dk_set_led_off(DK_LED2);
				dk_set_led_off(DK_LED3);
				Instance().UpdateStatusLed();
				return;
			}

			Instance().CancelTimer(Timer::DimmerTrigger);
			if (sDimmerTriggered) {
				Instance().CancelTimer(Timer::Dimmer);
				sDimmerTriggered = false;
			} else {
				/* Short press: emit ShortRelease to Home Assistant and Toggle to bound lights */
				LOG_INF("Short press — toggling light.");
				chip::app::Clusters::Switch::Attributes::CurrentPosition::Set(kGenericSwitchEndpointId, 0);
				chip::app::Clusters::SwitchServer::Instance().OnShortRelease(kGenericSwitchEndpointId, 1);
#ifdef CONFIG_MATTER_BUTTON_MODE_1
				LightSwitch::GetInstance().InitiateActionSwitch(LightSwitch::Action::Toggle);
#endif
			}
		}

#ifdef CONFIG_CHIP_ICD_UAT_SUPPORT
	} else if ((UAT_BUTTON_MASK & state & hasChanged)) {
		Server::GetInstance().GetICDManager().OnNetworkActivity();
#endif
	}
}

/* --- Timer start / cancel / callback ------------------------------------- */

void AppTask::StartTimer(Timer timer, uint32_t timeoutMs)
{
	switch (timer) {
	case Timer::SingleTap:
		k_timer_start(&sSingleTapTimer, K_MSEC(timeoutMs), K_NO_WAIT);
		break;
	case Timer::AccelPoll:
		k_timer_start(&sAccelPollTimer, K_MSEC(timeoutMs), K_MSEC(timeoutMs));
		break;
	case Timer::AccelSleep:
		k_timer_start(&sAccelSleepTimer, K_MSEC(timeoutMs), K_NO_WAIT);
		break;
	case Timer::DimmerTrigger:
		k_timer_start(&sDimmerTriggerTimer, K_MSEC(timeoutMs), K_NO_WAIT);
		break;
	case Timer::Dimmer:
		k_timer_start(&sDimmerTimer, K_MSEC(timeoutMs), K_MSEC(timeoutMs));
		break;
	case Timer::LongPressDetect:
		k_timer_start(&sLongPressTimer, K_MSEC(timeoutMs), K_NO_WAIT);
		break;
	case Timer::FactoryWarning:
		k_timer_start(&sFactoryWarningTimer, K_MSEC(timeoutMs), K_NO_WAIT);
		break;
	case Timer::FactoryReset:
		k_timer_start(&sFactoryResetTimer, K_MSEC(timeoutMs), K_NO_WAIT);
		break;
	case Timer::StatusLedBlink:
		k_timer_start(&sStatusLedBlinkTimer, K_MSEC(timeoutMs), K_NO_WAIT);
		break;
	case Timer::SensorPoll:
		k_timer_start(&sSensorPollTimer, K_MSEC(timeoutMs), K_MSEC(timeoutMs));
		break;
	default:
		break;
	}
}

void AppTask::CancelTimer(Timer timer)
{
	switch (timer) {
	case Timer::SingleTap:
		k_timer_stop(&sSingleTapTimer);
		break;
	case Timer::AccelPoll:
		k_timer_stop(&sAccelPollTimer);
		break;
	case Timer::AccelSleep:
		k_timer_stop(&sAccelSleepTimer);
		break;
	case Timer::DimmerTrigger:
		k_timer_stop(&sDimmerTriggerTimer);
		break;
	case Timer::Dimmer:
		k_timer_stop(&sDimmerTimer);
		break;
	case Timer::LongPressDetect:
		k_timer_stop(&sLongPressTimer);
		break;
	case Timer::FactoryWarning:
		k_timer_stop(&sFactoryWarningTimer);
		break;
	case Timer::FactoryReset:
		k_timer_stop(&sFactoryResetTimer);
		break;
	case Timer::StatusLedBlink:
		k_timer_stop(&sStatusLedBlinkTimer);
		break;
	case Timer::SensorPoll:
		k_timer_stop(&sSensorPollTimer);
		break;
	default:
		break;
	}
}

void AppTask::UserTimerTimeoutCallback(k_timer *timer)
{
	if (!timer) {
		return;
	}

	Timer timerType;
	if (timer == &sSingleTapTimer)          timerType = Timer::SingleTap;
	else if (timer == &sAccelPollTimer)     timerType = Timer::AccelPoll;
	else if (timer == &sAccelSleepTimer)    timerType = Timer::AccelSleep;
	else if (timer == &sDimmerTriggerTimer) timerType = Timer::DimmerTrigger;
	else if (timer == &sDimmerTimer)        timerType = Timer::Dimmer;
	else if (timer == &sLongPressTimer)     timerType = Timer::LongPressDetect;
	else if (timer == &sFactoryWarningTimer) timerType = Timer::FactoryWarning;
	else if (timer == &sFactoryResetTimer)  timerType = Timer::FactoryReset;
	else if (timer == &sStatusLedBlinkTimer) timerType = Timer::StatusLedBlink;
	else if (timer == &sSensorPollTimer)    timerType = Timer::SensorPoll;
	else return;

	Nrf::PostTask([timerType]() { TimerEventHandler(timerType); });
}

/* --- Init and StartApp --------------------------------------------------- */

CHIP_ERROR AppTask::Init()
{
	ReturnErrorOnFailure(Nrf::Matter::PrepareServer(Nrf::Matter::InitData{ .mPostServerInitClbk = [] {
		LightSwitch::GetInstance().Init(kLightSwitchEndpointId);
		return CHIP_NO_ERROR;
	} }));

	/* Init all timers before any code that might start them */
	k_timer_init(&sSingleTapTimer,       AppTask::UserTimerTimeoutCallback, nullptr);
	k_timer_init(&sAccelPollTimer,       AppTask::UserTimerTimeoutCallback, nullptr);
	k_timer_init(&sAccelSleepTimer,      AppTask::UserTimerTimeoutCallback, nullptr);
	k_timer_init(&sDimmerTriggerTimer,   AppTask::UserTimerTimeoutCallback, nullptr);
	k_timer_init(&sDimmerTimer,          AppTask::UserTimerTimeoutCallback, nullptr);
	k_timer_init(&sLongPressTimer,       AppTask::UserTimerTimeoutCallback, nullptr);
	k_timer_init(&sFactoryWarningTimer,  AppTask::UserTimerTimeoutCallback, nullptr);
	k_timer_init(&sFactoryResetTimer,    AppTask::UserTimerTimeoutCallback, nullptr);
	k_timer_init(&sStatusLedBlinkTimer,  AppTask::UserTimerTimeoutCallback, nullptr);
	k_timer_init(&sSensorPollTimer,      AppTask::UserTimerTimeoutCallback, nullptr);

	/* Accelerometer: Wake-on-Turn enabled via LIS2DH12 INT1 (P2.00) */
	sAccelDevice = DEVICE_DT_GET(DT_ALIAS(accel0));
	if (!device_is_ready(sAccelDevice)) {
		LOG_ERR("Accelerometer not ready.");
		sAccelDevice = nullptr;
	} else {
		if (gpio_is_ready_dt(&sAccelInt1Gpio) &&
		    gpio_pin_configure_dt(&sAccelInt1Gpio, GPIO_INPUT) == 0) {
			gpio_init_callback(&sAccelInt1CbData, AccelInt1Isr, BIT(sAccelInt1Gpio.pin));
			if (gpio_add_callback(sAccelInt1Gpio.port, &sAccelInt1CbData) == 0) {
				sAccelInt1Ready = true;
			}
		}
		EnterAccelIdleMode();
	}

	/* Environmental sensors (25015 only) */
#ifdef CONFIG_SHT4X
	sSht40Device = DEVICE_DT_GET(DT_ALIAS(env0));
	if (!device_is_ready(sSht40Device)) {
		LOG_ERR("SHT40 not ready.");
		sSht40Device = nullptr;
	} else {
		WriteLps22hbRegister(0x10, 0x00); /* CTRL_REG1: power-down mode */
	}
#endif

	int led_err = dk_leds_init();
	if (led_err) {
		LOG_ERR("LED init failed: %d", led_err);
	}

	if (!Nrf::GetBoard().Init(ButtonEventHandler)) {
		LOG_ERR("UI init failed.");
		return CHIP_ERROR_INCORRECT_STATE;
	}

	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(Nrf::Board::DefaultMatterEventHandler, 0));
	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(AppTask::MatterEventHandler, 0));
	ReturnErrorOnFailure(sIdentifyCluster.Init());

	CHIP_ERROR err = Nrf::Matter::StartServer();
	if (err != CHIP_NO_ERROR) {
		return err;
	}

	/* Configure Endpoint 2 Switch cluster to support 0..100% step positions for Home Assistant */
	chip::app::Clusters::Switch::Attributes::NumberOfPositions::Set(kGenericSwitchEndpointId, 100);
	chip::app::Clusters::Switch::Attributes::CurrentPosition::Set(kGenericSwitchEndpointId, 0);

#ifdef CONFIG_MATTER_BUTTON_MODE_2
	emberAfEndpointEnableDisable(kLightSwitchEndpointId, false);
#endif

	/* Disable sensor endpoints on boards without environmental sensors */
#ifndef CONFIG_SHT4X
	emberAfEndpointEnableDisable(kTempEndpointId, false);
	emberAfEndpointEnableDisable(kHumEndpointId, false);
#endif
	emberAfEndpointEnableDisable(kPressureEndpointId, false);

	bool commissioned = Server::GetInstance().GetFabricTable().FabricCount() > 0;

#ifdef CONFIG_SHT4X
	if (commissioned && sSht40Device && !sIsSensorTimerStarted) {
		LOG_INF("Device already commissioned — starting sensor measurements.");
		k_timer_start(&sSensorPollTimer, K_NO_WAIT, K_MSEC(kSensorPollIntervalMs));
		sIsSensorTimerStarted = true;
	}
#endif

	sLedState = commissioned ? LedState::Off : LedState::SlowBlink;
	UpdateStatusLed();

	return CHIP_NO_ERROR;
}

void AppTask::UpdateClusterState()
{
	StartAccelDimming();
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	while (true) {
		Nrf::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}
