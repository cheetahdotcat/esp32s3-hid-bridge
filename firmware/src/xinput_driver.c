/**
 * @file xinput_driver.c
 * @brief TinyUSB application class driver for Xbox 360 wired (XUSB) interfaces.
 */
#include <string.h>

#include "bridge.h"
#include "device/usbd_pvt.h"
#include "tusb.h"

#define XINPUT_EP_SIZE 32U

typedef struct {
  uint8_t ep_in;
  uint8_t ep_out;
  CFG_TUSB_MEM_ALIGN uint8_t in_buffer[XINPUT_EP_SIZE];
  CFG_TUSB_MEM_ALIGN uint8_t out_buffer[XINPUT_EP_SIZE];
} xinput_interface_t;

CFG_TUSB_MEM_SECTION static xinput_interface_t s_interfaces[LVHB_MAX_SLOTS];

static int slot_for_endpoint(uint8_t ep_addr) {
  for (unsigned index = 0; index < LVHB_MAX_SLOTS; ++index) {
    if (s_interfaces[index].ep_in == ep_addr || s_interfaces[index].ep_out == ep_addr) {
      return (int) index;
    }
  }
  return -1;
}

static void xinput_init(void) {
  xinput_reset_all();
}

static bool xinput_deinit(void) {
  return true;
}

static void xinput_reset(uint8_t rhport) {
  (void) rhport;
  xinput_reset_all();
}

static uint16_t xinput_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len) {
  TU_VERIFY(itf->bInterfaceClass == 0xFF && itf->bInterfaceSubClass == 0x5D && itf->bInterfaceProtocol == 0x01, 0);
  const int slot = bridge_slot_for_interface(itf->bInterfaceNumber);
  TU_VERIFY(slot >= 0, 0);

  xinput_interface_t *state = &s_interfaces[slot];
  const uint8_t *cursor = (const uint8_t *) itf;
  const uint8_t *end = cursor + max_len;
  uint16_t length = (uint16_t) tu_desc_len(cursor);
  cursor = tu_desc_next(cursor);
  uint8_t endpoints = 0;
  while (cursor < end && endpoints < itf->bNumEndpoints) {
    if (tu_desc_type(cursor) == TUSB_DESC_INTERFACE) {
      break;
    }
    if (tu_desc_type(cursor) == TUSB_DESC_ENDPOINT) {
      const tusb_desc_endpoint_t *endpoint = (const tusb_desc_endpoint_t *) cursor;
      TU_ASSERT(usbd_edpt_open(rhport, endpoint), 0);
      if (tu_edpt_dir(endpoint->bEndpointAddress) == TUSB_DIR_IN) {
        state->ep_in = endpoint->bEndpointAddress;
      } else {
        state->ep_out = endpoint->bEndpointAddress;
      }
      ++endpoints;
    }
    length = (uint16_t) (length + tu_desc_len(cursor));
    cursor = tu_desc_next(cursor);
  }

  if (state->ep_out != 0U) {
    TU_ASSERT(usbd_edpt_xfer(rhport, state->ep_out, state->out_buffer, XINPUT_EP_SIZE, false), 0);
  }
  return length;
}

static bool xinput_control_xfer(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
  (void) rhport;
  (void) stage;
  (void) request;
  // Vendor requests are answered by tud_vendor_control_xfer_cb().
  return false;
}

static bool xinput_xfer(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
  const int slot = slot_for_endpoint(ep_addr);
  TU_VERIFY(slot >= 0);
  xinput_interface_t *state = &s_interfaces[slot];

  if (ep_addr == state->ep_out) {
    if (result == XFER_RESULT_SUCCESS && xferred_bytes > 0U) {
      bridge_forward_output((uint8_t) slot, LVHB_REPORT_OUTPUT, 0, state->out_buffer, (uint16_t) xferred_bytes);
    }
    TU_ASSERT(usbd_edpt_xfer(rhport, state->ep_out, state->out_buffer, XINPUT_EP_SIZE, false));
    return true;
  }

  bridge_slot_in_complete((uint8_t) slot);
  return true;
}

static const usbd_class_driver_t s_driver = {
  .name = "XINPUT",
  .init = xinput_init,
  .deinit = xinput_deinit,
  .reset = xinput_reset,
  .open = xinput_open,
  .control_xfer_cb = xinput_control_xfer,
  .xfer_cb = xinput_xfer,
  .sof = NULL,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
  *driver_count = 1;
  return &s_driver;
}

void xinput_reset_all(void) {
  memset(s_interfaces, 0, sizeof(s_interfaces));
}

bool xinput_send(uint8_t slot, const uint8_t *report, uint16_t size) {
  xinput_interface_t *state = &s_interfaces[slot];
  if (state->ep_in == 0U || !tud_ready()) {
    return false;
  }
  if (!usbd_edpt_claim(0, state->ep_in)) {
    return false;
  }
  const uint16_t length = tu_min16(size, XINPUT_EP_SIZE);
  memcpy(state->in_buffer, report, length);
  if (!usbd_edpt_xfer(0, state->ep_in, state->in_buffer, length, false)) {
    usbd_edpt_release(0, state->ep_in);
    return false;
  }
  return true;
}
