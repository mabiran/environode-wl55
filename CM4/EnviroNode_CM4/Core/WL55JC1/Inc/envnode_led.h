/**
  ******************************************************************************
  * @file    envnode_led.h
  * @brief   Status LED on D6 (PB10) — the "power button" LED.
  *
  *          One LED tells the whole story, and DARK is the normal state:
  *
  *            solid ON          booting (init in progress)
  *            1 flash / 2 s     awake, LoRaWAN joined, healthy
  *            2 flashes / 2 s   awake, NOT joined (no gateway / keys)
  *            3 flashes / 2 s   awake, last measurement raised SENS_FAULT
  *            single pulse      an uplink was just handed to the radio
  *            dark              STOP2 sleep between cycles — or no power
  *
  *          The pattern generator runs from the main loop (envnode_led_tick()),
  *          so it costs nothing while the core sleeps — which is exactly why
  *          "dark = asleep" needs no code: STOP2 stops the ticker itself.
  *          envnode_led_off() is called on the way into STOP2 so a nap can
  *          never freeze the LED mid-flash (a lit LED all night would out-draw
  *          the sleeping MCU a hundredfold).
  *
  *          Wiring: LED anode -> ~470 Ω series resistor -> D6 (PB10, CN9
  *          pin 7); cathode -> GND. Flip ENV_PWRLED_ACTIVE_HIGH for the
  *          opposite wiring.
  ******************************************************************************
  */
#ifndef ENVNODE_LED_H
#define ENVNODE_LED_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ENVLED_BOOT = 0,  /*!< solid ON — init in progress                     */
  ENVLED_OK,        /*!< 1 flash per beat — awake, joined, healthy       */
  ENVLED_NOJOIN,    /*!< 2 flashes — awake, not joined                   */
  ENVLED_FAULT,     /*!< 3 flashes — last sample raised SENS_FAULT       */
} envled_mode_t;

void envnode_led_init(void);                 /* PB10 as output, LED off      */
void envnode_led_set_mode(envled_mode_t m);  /* cheap; safe to call per pass */
void envnode_led_tick(void);                 /* main loop — drives the pins  */
void envnode_led_pulse(void);                /* one 120 ms flash now (TX)    */
void envnode_led_off(void);                  /* force dark (STOP2 entry)     */

#ifdef __cplusplus
}
#endif
#endif /* ENVNODE_LED_H */
