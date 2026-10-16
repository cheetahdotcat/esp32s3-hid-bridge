/**
 * @file usb_descriptors.c
 * @brief Layout-driven USB descriptors and vendor control requests.
 *
 * esp_tinyusb keeps pointers to the buffers in this file, so a new layout is
 * applied by rewriting them while the bus is disconnected.
 */
#include <string.h>

#include "bridge.h"
#include "tusb.h"

#define CONFIG_BUFFER_SIZE 256U
#define MS_OS_20_BUFFER_SIZE 128U
#define XINPUT_DESC_LEN (9U + 17U + 7U + 7U)
#define XINPUT_EP_SIZE 32U
#define STRING_COUNT 5

enum {
  STRING_LANGID = 0,
  STRING_MANUFACTURER = 1,
  STRING_PRODUCT = 2,
  STRING_SERIAL = 3,
  STRING_CDC = 4,
};

bridge_layout_t g_layout;
bridge_slot_binding_t g_bindings[LVHB_MAX_SLOTS];

static tusb_desc_device_t s_device;
static uint8_t s_configuration[CONFIG_BUFFER_SIZE];
static uint8_t s_ms_os_20[MS_OS_20_BUFFER_SIZE];
static uint16_t s_ms_os_20_size;
static uint8_t s_bos[5U + 28U];
static char s_manufacturer[LVHB_MAX_STRING + 1U];
static char s_product[LVHB_MAX_STRING + 1U];
static char s_serial[LVHB_MAX_STRING + 1U] = LVHB_SERIAL_PREFIX "000000000000";
static const char s_langid[] = {0x09, 0x04};
static const char *s_strings[STRING_COUNT] = {
  s_langid,
  s_manufacturer,
  s_product,
  s_serial,
  "libvirtualhid bridge control",
};

/// Xbox 360 wired capability reply: every input supported.
static const uint8_t s_xinput_capabilities_inputs[20] = {
  0x00, 0x14, 0x3F, 0xF7, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
  0xC0, 0xFF, 0xC0, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
/// Xbox 360 wired capability reply: both rumble motors supported.
static const uint8_t s_xinput_capabilities_rumble[8] = {0x00, 0x08, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00};
static uint8_t s_xinput_serial[4];

static size_t append(uint8_t *buffer, size_t offset, const uint8_t *data, size_t size) {
  memcpy(buffer + offset, data, size);
  return offset + size;
}

static size_t append_xinput(uint8_t *buffer, size_t offset, const bridge_slot_binding_t *binding, uint8_t interval) {
  const uint8_t descriptor[XINPUT_DESC_LEN] = {
    9, TUSB_DESC_INTERFACE, binding->interface_number, 0, 2, 0xFF, 0x5D, 0x01, 0,
    // Undocumented XUSB descriptor that xusb22 expects after the interface.
    17, 0x21, 0x00, 0x01, 0x01, 0x25, binding->ep_in, 0x14, 0x00, 0x00, 0x00, 0x00, 0x13, binding->ep_out, 0x08, 0x00, 0x00,
    7, TUSB_DESC_ENDPOINT, binding->ep_in, TUSB_XFER_INTERRUPT, XINPUT_EP_SIZE, 0, interval,
    7, TUSB_DESC_ENDPOINT, binding->ep_out, TUSB_XFER_INTERRUPT, XINPUT_EP_SIZE, 0, 8,
  };
  return append(buffer, offset, descriptor, sizeof(descriptor));
}

static size_t append_hid(uint8_t *buffer, size_t offset, const bridge_slot_binding_t *binding, const bridge_slot_config_t *slot) {
  const uint8_t descriptor[] = {
    TUD_HID_DESCRIPTOR(binding->interface_number, 0, HID_ITF_PROTOCOL_NONE, slot->descriptor_size, binding->ep_in, CFG_TUD_HID_EP_BUFSIZE, slot->interval_ms)
  };
  return append(buffer, offset, descriptor, sizeof(descriptor));
}

static void build_ms_os_20(void) {
  uint8_t functions[LVHB_MAX_SLOTS];
  size_t function_count = 0;
  for (unsigned index = 0; index < LVHB_MAX_SLOTS; ++index) {
    if (g_layout.slots[index].kind == LVHB_SLOT_XINPUT) {
      functions[function_count++] = g_bindings[index].interface_number;
    }
  }

  if (function_count == 0U) {
    const uint8_t empty_bos[] = {5, TUSB_DESC_BOS, 5, 0, 0};
    memcpy(s_bos, empty_bos, sizeof(empty_bos));
    s_ms_os_20_size = 0;
    return;
  }

  const uint16_t function_size = 8U + 20U;
  const uint16_t configuration_size = (uint16_t) (8U + function_size * function_count);
  s_ms_os_20_size = (uint16_t) (10U + configuration_size);
  const uint8_t header[] = {
    10, 0, 0x00, 0x00, 0x00, 0x00, 0x03, 0x06, U16_TO_U8S_LE(s_ms_os_20_size),
    8, 0, 0x01, 0x00, 0, 0, U16_TO_U8S_LE(configuration_size),
  };
  size_t offset = append(s_ms_os_20, 0, header, sizeof(header));
  for (size_t index = 0; index < function_count; ++index) {
    const uint8_t function[] = {
      8, 0, 0x02, 0x00, functions[index], 0, U16_TO_U8S_LE(function_size),
      20, 0, 0x03, 0x00, 'X', 'U', 'S', 'B', '1', '0', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    offset = append(s_ms_os_20, offset, function, sizeof(function));
  }

  const uint8_t bos[] = {
    5, TUSB_DESC_BOS, U16_TO_U8S_LE(sizeof(s_bos)), 1,
    // MS OS 2.0 platform capability, Windows 8.1 and newer.
    28, TUSB_DESC_DEVICE_CAPABILITY, DEVICE_CAPABILITY_PLATFORM, 0x00,
    0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C, 0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
    0x00, 0x00, 0x03, 0x06, U16_TO_U8S_LE(s_ms_os_20_size), BRIDGE_MS_OS_20_VENDOR_CODE, 0x00,
  };
  memcpy(s_bos, bos, sizeof(s_bos));
}

void bridge_descriptors_build(void) {
  uint8_t next_interface = BRIDGE_CDC_INTERFACES;
  uint8_t next_endpoint = 3U;
  int8_t next_hid = 0;
  for (unsigned index = 0; index < LVHB_MAX_SLOTS; ++index) {
    bridge_slot_binding_t *binding = &g_bindings[index];
    memset(binding, 0, sizeof(*binding));
    binding->hid_instance = -1;
    const uint8_t kind = g_layout.slots[index].kind;
    if (kind == LVHB_SLOT_NONE) {
      continue;
    }
    binding->interface_number = next_interface++;
    binding->ep_in = (uint8_t) (0x80U | next_endpoint);
    if (kind == LVHB_SLOT_XINPUT) {
      binding->ep_out = next_endpoint;
    } else {
      binding->hid_instance = next_hid++;
    }
    ++next_endpoint;
  }

  size_t offset = 9U;
  const uint8_t cdc[] = {TUD_CDC_DESCRIPTOR(0, STRING_CDC, BRIDGE_EP_CDC_NOTIF, 8, BRIDGE_EP_CDC_OUT, BRIDGE_EP_CDC_IN, 64)};
  offset = append(s_configuration, offset, cdc, sizeof(cdc));
  for (unsigned index = 0; index < LVHB_MAX_SLOTS; ++index) {
    const bridge_slot_config_t *slot = &g_layout.slots[index];
    if (slot->kind == LVHB_SLOT_XINPUT) {
      offset = append_xinput(s_configuration, offset, &g_bindings[index], slot->interval_ms);
    } else if (slot->kind == LVHB_SLOT_HID || slot->kind == LVHB_SLOT_KEYBOARD_MOUSE) {
      offset = append_hid(s_configuration, offset, &g_bindings[index], slot);
    }
  }
  const uint8_t header[] = {TUD_CONFIG_DESCRIPTOR(1, next_interface, 0, offset, 0, 500)};
  memcpy(s_configuration, header, sizeof(header));

  s_device = (tusb_desc_device_t) {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210,  // 2.1 so Windows reads the BOS and MS OS 2.0 descriptors.
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = g_layout.vendor_id,
    .idProduct = g_layout.product_id,
    .bcdDevice = g_layout.bcd_device,
    .iManufacturer = STRING_MANUFACTURER,
    .iProduct = STRING_PRODUCT,
    .iSerialNumber = STRING_SERIAL,
    .bNumConfigurations = 1,
  };
  strncpy(s_manufacturer, g_layout.manufacturer, LVHB_MAX_STRING);
  strncpy(s_product, g_layout.product, LVHB_MAX_STRING);
  build_ms_os_20();
}

void bridge_descriptors_get(const void **device, const uint8_t **configuration, const char ***strings, int *string_count) {
  *device = &s_device;
  *configuration = s_configuration;
  *strings = s_strings;
  *string_count = STRING_COUNT;
}

void bridge_descriptors_set_serial(const char *serial) {
  strncpy(s_serial, serial, LVHB_MAX_STRING);
  // XInput hosts use a 4-byte serial to tell pads apart; derive it from the USB serial.
  const uint16_t crc = lvhb_crc16((const uint8_t *) serial, strlen(serial));
  s_xinput_serial[0] = 0x00;
  s_xinput_serial[1] = 0x12;
  s_xinput_serial[2] = (uint8_t) (crc >> 8U);
  s_xinput_serial[3] = (uint8_t) crc;
}

int bridge_slot_for_interface(uint8_t interface_number) {
  for (unsigned index = 0; index < LVHB_MAX_SLOTS; ++index) {
    if (g_layout.slots[index].kind != LVHB_SLOT_NONE && g_bindings[index].interface_number == interface_number) {
      return (int) index;
    }
  }
  return -1;
}

uint8_t const *tud_descriptor_bos_cb(void) {
  return s_bos;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  for (unsigned index = 0; index < LVHB_MAX_SLOTS; ++index) {
    if (g_bindings[index].hid_instance == (int8_t) instance) {
      return g_layout.slots[index].descriptor;
    }
  }
  return NULL;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
  if (stage != CONTROL_STAGE_SETUP) {
    return true;
  }

  if (request->bRequest == BRIDGE_MS_OS_20_VENDOR_CODE && request->wIndex == 7U && s_ms_os_20_size != 0U) {
    return tud_control_xfer(rhport, request, s_ms_os_20, s_ms_os_20_size);
  }

  // XUSB capability and serial queries. Answering them is required: Windows
  // Gaming Services can crash DWM when a pad stalls the rumble capability query.
  if (request->bRequest == 0x01U && request->bmRequestType_bit.direction == TUSB_DIR_IN) {
    if (request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE) {
      if (request->wValue == 0x0100U) {
        return tud_control_xfer(rhport, request, (void *) s_xinput_capabilities_inputs, sizeof(s_xinput_capabilities_inputs));
      }
      return tud_control_xfer(rhport, request, (void *) s_xinput_capabilities_rumble, sizeof(s_xinput_capabilities_rumble));
    }
    return tud_control_xfer(rhport, request, s_xinput_serial, sizeof(s_xinput_serial));
  }

  // Accept and ignore the remaining XUSB vendor writes.
  if (request->bmRequestType_bit.direction == TUSB_DIR_OUT && request->wLength == 0U) {
    return tud_control_status(rhport, request);
  }
  return false;
}
