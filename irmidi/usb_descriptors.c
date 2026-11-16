// USB MIDI device: shows up as "Syrup IR MIDI" on computers, phones (USB-C / camera adapter) and DAWs
#include "tusb.h"
#include "pico/unique_id.h"
#include <string.h>
static tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE, .bcdUSB = 0x0200,
    .bDeviceClass = 0x00, .bDeviceSubClass = 0x00, .bDeviceProtocol = 0x00, .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x1209, .idProduct = 0x5359, .bcdDevice = 0x0100,   // pid.codes test range
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1};
uint8_t const* tud_descriptor_device_cb(void) { return (uint8_t const*)&desc_device; }
enum { ITF_NUM_MIDI = 0, ITF_NUM_MIDI_STREAMING, ITF_NUM_TOTAL };
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN)
#define EPNUM_MIDI_OUT 0x01
#define EPNUM_MIDI_IN 0x81
static uint8_t const desc_cfg[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 0, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64)};
uint8_t const* tud_descriptor_configuration_cb(uint8_t index) { (void)index; return desc_cfg; }
static char const* strs[] = {"", "Syrup", "Syrup IR MIDI", NULL};
static uint16_t str_buf[32];
uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid; uint8_t n;
    if (index == 0) { str_buf[1] = 0x0409; n = 1; }
    else {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        const char* s;
        if (index == 3) { pico_get_unique_board_id_string(serial, sizeof(serial)); s = serial; }
        else if (index < 3) s = strs[index];
        else return NULL;
        n = (uint8_t)strlen(s); if (n > 31) n = 31;
        for (uint8_t i = 0; i < n; i++) str_buf[1 + i] = s[i];
    }
    str_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return str_buf;
}
