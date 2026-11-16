// Syrup IR MIDI adapter (RP2040)
// USB MIDI in (from a computer, phone or DAW) -> IrDA SIR light pulses at 115200 baud -> the 2DS's IR port.
// Optional 5-pin DIN MIDI in on GP5 (UART1 RX, through the usual 6N138 opto circuit) is merged in too.
// In Syrup: HARDWARE page -> LINK -> IR MIDI IN, then point the LED at the 2DS's IR window (top edge, back).
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/uart.h"
#include "hardware/clocks.h"
#include "tusb.h"
#include "irda_tx.pio.h"

#define IR_PIN 15        // GP15 -> 1k -> NPN base; LED + resistor from VBUS to the collector
#define DIN_RX_PIN 5     // GP5: optional MIDI DIN input (UART1 RX, 31250 baud)
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

static PIO pio = pio0; static uint sm;
static uint32_t ledOff = 0;
static void sendByte(uint8_t b) { irda_tx_byte(pio, sm, b); gpio_put(PICO_DEFAULT_LED_PIN, 1); ledOff = time_us_32() + 30000; }

int main(void) {
    gpio_init(PICO_DEFAULT_LED_PIN); gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    uint off = pio_add_program(pio, &irda_tx_program);
    sm = pio_claim_unused_sm(pio, true);
    irda_tx_init(pio, sm, off, IR_PIN, 115200);
    uart_init(uart1, 31250); gpio_set_function(DIN_RX_PIN, GPIO_FUNC_UART); gpio_pull_up(DIN_RX_PIN);
    tud_init(0);
    uint32_t hb = 0;
    while (1) {
        tud_task();
        uint8_t buf[64];
        while (tud_midi_available()) {
            uint32_t n = tud_midi_stream_read(buf, sizeof(buf));
            for (uint32_t i = 0; i < n; i++) if (buf[i] != 0xFE) sendByte(buf[i]);   // drop active sensing
        }
        while (uart_is_readable(uart1)) { uint8_t b = uart_getc(uart1); if (b != 0xFE) sendByte(b); }
        uint32_t now = time_us_32();
        if ((int32_t)(now - ledOff) > 0) gpio_put(PICO_DEFAULT_LED_PIN, (now - hb) < 50000);   // short blink every 2 s = alive
        if (now - hb > 2000000) hb = now;
    }
}
