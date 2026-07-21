// clang-format off
/********************************************************************************
 * Name:    blink_led.c
 * Purpose: Blinks the green LED (DS3) on the ADICUP3029 baseboard
 * Author:  Mohamed Elazab (mohamed.elazab@case.edu)
 * Date:    07/21/2026
 ********************************************************************************/
// clang-format on

/* P2.00 -- DS3 (green), net "3029_LED1" on the ADICUP3029 baseboard
 * schematic (sheet 2, "ADUCM3029 CONNECTIONS"): pin 36 of the ADuCM3029 is
 * GPIO32, ADI's flat GPIO numbering maps GPIO32 to Port2 Pin0. Read
 * straight off the schematic PDF, not inferred from another board's mbed
 * target -- that guess (P2.02/P2.10) was wrong, which is why nothing
 * seemed to happen before. The other LED, DS4 (blue, "3029_LED2"), is on
 * GPIO31 = P1.15. */
#define LED_PORT GPIO2
#define LED_PIN 0u

/* P1.15 -- DS4 (blue), net "3029_LED2". The LED's anode (not cathode) is
 * on this node, pulled up to DVDD through R25 -- so driving the pin HIGH
 * pushes the anode to ~DVDD directly, forward-biasing the LED *harder*
 * (and bypassing R25 as a current limiter). Driving it LOW is what
 * actually turns it off, by sinking R25's pull-up current into the pin
 * instead of through the LED. Nothing to blink it for here, so just turn
 * it off at startup. */
#define AUX_LED_PORT GPIO1
#define AUX_LED_PIN 15u

static void delay(volatile uint32_t count) {
  while (count--) {
  }
}

int main(void) {
  /* Harmless whether or not it was actually needed: stop the watchdog
   * before it resets the board mid-blink. */
  WDT0->CTL &= (uint16_t)~WDT_CTL_EN;

  /* Ditto: make sure the GPIO block's clock isn't gated off. */
  CLKG0_CLK->CTL5 &= ~CLKG_CLK_CTL5_GPIOCLKOFF;

  /* Mux the pin to plain GPIO (mux field = 00) and drive it as an output. */
  LED_PORT->CFG &= ~(0x3u << (LED_PIN * 2));
  LED_PORT->OEN |= (uint16_t)(1u << LED_PIN);

  AUX_LED_PORT->CFG &= ~(0x3u << (AUX_LED_PIN * 2));
  AUX_LED_PORT->OEN |= (uint16_t)(1u << AUX_LED_PIN);
  AUX_LED_PORT->CLR = (uint16_t)(1u << AUX_LED_PIN);

  for (;;) {
    LED_PORT->TGL = (uint16_t)(1u << LED_PIN);
    delay(1000000);
  }
}
