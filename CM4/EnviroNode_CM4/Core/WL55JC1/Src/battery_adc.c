#include "battery_adc.h"
#include "pins_config.h"

#include "main.h"
#include "adc.h"    /* ADC_ReadChannelAvg + ENVNODE_ADC_CH_BATT */

/* EnviroNode reads four ADC channels, so a battery read must select its own
   channel (PB14/A4 = ADC_IN1) instead of assuming whatever rank 1 held last.
   ADC_ReadChannelAvg() does the select-convert-stop sequence. */
float Battery_ReadVoltage(uint16_t samples) {
    if (samples == 0) samples = 1;

    uint16_t raw = 0;
    if (ADC_ReadChannelAvg(ENVNODE_ADC_CH_BATT, samples, &raw) != HAL_OK) {
        return 0.0f;                       /* caller treats 0 V as "no read" */
    }

    /* Convert raw ADC -> Vadc -> Vbattery */
    float v_adc = ((float)raw / ADC_FULL_SCALE) * ADC_VREF_VOLT;
    float v_bat = v_adc * VBAT_DIVIDER_GAIN;  // undo the divider

    return v_bat;
}
