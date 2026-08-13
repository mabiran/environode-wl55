/**
  ******************************************************************************
  * @file    analog_sensors.h
  * @brief   ADC-based sensors: soil moisture, leaf wetness, battery, wind dir.
  *
  *          Four single-ended channels, read one at a time through
  *          ADC_ReadChannelAvg() (adc.c) with a 160.5-cycle sampling time — the
  *          soil and leaf probes are high-impedance sources.
  *
  *          Raw counts are returned for soil/leaf (the calibration curve is
  *          applied off-node), battery is scaled by the measured divider ratio
  *          from pins_config.h, and wind direction maps the vane potentiometer
  *          0..Vref onto 0..360° plus the configurable north offset.
  ******************************************************************************
  */
#ifndef ANALOG_SENSORS_H
#define ANALOG_SENSORS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "sensors/envnode_sensors.h"
#include "pins_config.h"      /* ADC_VREF_VOLT, ADC_FULL_SCALE, VBAT_DIVIDER_GAIN */

/* Front-end constants — the divider ratio is the one measured on the board. */
#define ANALOG_OVERSAMPLES   (8u)                 /*!< reads averaged per channel */

/** Settling time after the excitation rail (VSENS) is switched on, before any
 *  conversion. The Decagon LWS specifies a **10 ms minimum excitation time**;
 *  15 ms carries margin for the rail's RC and the vane pot. Cost per cycle is
 *  15 ms of ~4 mA — about 0.017 uAh, versus 91 mAh/day left permanently on. */
#define ANALOG_RAIL_SETTLE_MS (15u)

/** @name Davis 7911 wind-speed burst sampling
 *
 * The anemometer's output is a contact closure to ground — a switch, with no
 * voltage that encodes speed. It is read by sampling the pin fast for a short
 * window and counting transitions, rather than by an EXTI interrupt: interrupt
 * counting would pin the core awake for the whole interval, which costs far more
 * energy than everything else on this sensor put together.
 *
 * The trade is temporal coverage — a burst sees ANEMO_BURST_MS of wind per
 * cycle, not the whole interval. The window is 3 s so it matches the WMO gust
 * definition, which is why speed and gust come out equal here (both are the same
 * 3-second mean). True interval averaging needs EXTI wake-from-STOP2 instead.
 * @{ */
#define ANEMO_BURST_MS        (3000u)  /*!< sampling window = the WMO gust window */
#define ANEMO_SAMPLE_US       (1000u)  /*!< ~1 kHz: ~11 samples per period at the
                                            sensor's 88 Hz (89 m/s) ceiling      */
#define ANEMO_HI_COUNTS       (2600u)  /*!< Schmitt-style hysteresis, ~2.1 V      */
#define ANEMO_LO_COUNTS       (1400u)  /*!< ~1.1 V                                */
/** @} */
#define BATT_DIVIDER_RATIO   (VBAT_DIVIDER_GAIN)  /*!< ~4.748 (56.06k / 14.711k)  */
#define WINDDIR_DEG_MAX      (360.0f)

/** @name PT1000 soil temperature — resistor divider on A2 (PA10 / ADC_IN6)
 *
 * Wiring: 3V3 —[series R]— A2 —[PT1000]— GND. Replaced the MAX31865 (dropped
 * from this node, LOGBOOK r18). Because the divider's top rail *is* the ADC
 * reference, the measurement is ratiometric: R_rtd = Rs·counts/(4095−counts),
 * with no dependence on the actual rail voltage. Then the same Callendar–Van
 * Dusen conversion the MAX31865 driver used (pt1000_ohms_to_celsius).
 *
 * ⚠️ A2 is also I²C1 SDA (BME280 #2). While the divider is fitted, `T2` is
 * electrically unusable — the divider holds SDA at ~1.8 V. The pin is muxed to
 * analog only for the conversion and handed back to I²C1 afterwards.
 *
 * Set ANALOG_RTD_SERIES_OHMS to the series resistor's *measured* value: each
 * ohm of error is ~0.26 °C, so a 5 % tolerance part left uncorrected can be
 * off by ~12 °C. Fine-trim with `set_cal` sensor_id 7 (docs/PAYLOAD.md).
 * @{ */
#define ANALOG_RTD_SERIES_OHMS  (900.0f)  /*!< measure yours with a DMM        */
/** @} */

/**
 * @brief  Prepare the analog block. The ADC itself is configured and calibrated
 *         by MX_ADC_Init(); this only resets the driver state.
 */
env_status_t analog_init(void);

/**
 * @brief  One pass over all four channels.
 * @param[out] soil_raw     soil-moisture ADC counts (0..4095)
 * @param[out] leaf_raw     leaf-wetness ADC counts (0..4095)
 * @param[out] batt_v       battery volts (= Vadc * BATT_DIVIDER_RATIO)
 * @param[out] winddir_deg  wind direction 0..359.9° (vane pot + north offset)
 * @retval ENV_OK if every channel converted, ENV_ERR otherwise (the channels
 *         that did succeed are still written).
 */
env_status_t analog_read_all(uint16_t *soil_raw, uint16_t *leaf_raw, float *batt_v, float *winddir_deg);

/**
 * @brief  Measure wind speed by bursting the ADC on the contact input.
 *
 * Blocks for ANEMO_BURST_MS while sampling, so only call it when `WS` is
 * actually selected. The excitation rail is not involved — the contact is a
 * passive switch to ground with a pull-up at the connector.
 *
 * @param[out] speed_ms  mean over the burst window
 * @param[out] gust_ms   equal to @p speed_ms by construction: the burst *is* the
 *                       3-second gust window. Reported separately so the on-air
 *                       frame keeps its shape and so a future EXTI-based
 *                       implementation can fill it independently.
 * @retval ENV_OK, or ENV_ERR if a conversion failed or an argument was NULL.
 */
env_status_t analog_wind_burst(float *speed_ms, float *gust_ms);

/**
 * @brief  Set/get the wind-vane north offset in degrees (downlink cmd 0x05).
 *         Added to the raw vane angle, then wrapped into 0..360.
 */
void  analog_set_winddir_offset(float deg);
float analog_get_winddir_offset(void);

/**
 * @brief  Read the PT1000 through the A2 resistor divider (see the block
 *         comment above). Muxes PA10 to analog, converts, restores I²C1 AF.
 * @param[out] t_c  soil temperature °C
 * @retval ENV_OK, or ENV_ERR when the probe is open (counts pinned at the
 *         rail), shorted (counts near 0), or the result is outside −60…120 °C.
 */
env_status_t analog_rtd_a2_celsius(float *t_c);

#ifdef __cplusplus
}
#endif
#endif /* ANALOG_SENSORS_H */
