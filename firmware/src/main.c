/**
 * @file main.c
 * @brief ESP32-S3 libvirtualhid USB bridge: CDC protocol, slot state, and HID callbacks.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bridge.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb.h"
#include "tusb_cdc_acm.h"

#define BOOT_VENDOR_ID 0x303AU  ///< Espressif VID. Request a PID from Espressif before distributing hardware.
#define BOOT_PRODUCT_ID 0x40F0U
#define BOOT_BCD_DEVICE 0x0100U
#define REENUMERATE_DETACH_MS 300U
#define CONTROL_TASK_PERIOD_MS 10U

static const char *TAG = "lvhb";

/**
 * @brief Latest input report and transfer bookkeeping for one slot.
 */
typedef struct {
  uint8_t report[LVHB_MAX_REPORT];
  uint16_t size;
  bool pending;
} slot_runtime_t;

#define KM_QUEUE_DEPTH 32U
#define KM_MAX_REPORT 16U

/**
 * @brief One queued keyboard/mouse report.
 */
typedef struct {
  uint8_t data[KM_MAX_REPORT];
  uint8_t size;
} km_report_t;

/**
 * @brief In-order report queue for a keyboard/mouse slot.
 *
 * Unlike gamepads these reports are events: each goes out exactly once.
 */
typedef struct {
  km_report_t items[KM_QUEUE_DEPTH];
  uint8_t head;
  uint8_t count;
  bool front_sending;  ///< The front report is being handed to TinyUSB and must not be merged into.
  bool repump;  ///< A completion arrived while the front was being sent; pump again.
  bool dropped;  ///< A report was dropped because the queue was full.
} km_queue_t;

static slot_runtime_t s_runtime[LVHB_MAX_SLOTS];
static km_queue_t s_km_queues[LVHB_MAX_SLOTS];
static portMUX_TYPE s_runtime_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_tx_mutex;
static TaskHandle_t s_control_task;
static bridge_layout_t s_staged;
static bool s_staging;
static uint8_t s_tx_sequence;
static TickType_t s_last_host_frame;
static bool s_neutralized;
static volatile bool s_host_connected;

static void send_frame(uint8_t type, const uint8_t *payload, size_t size) {
  static uint8_t encoded[LVHB_MAX_ENCODED_FRAME];
  if (!tud_cdc_n_connected(0)) {
    return;
  }
  xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
  const size_t encoded_size = lvhb_encode_frame(type, s_tx_sequence++, payload, size, encoded);
  // Drop rather than split a frame; the host resynchronizes on the next delimiter anyway.
  if (encoded_size != 0U && tud_cdc_n_write_available(0) >= encoded_size) {
    tud_cdc_n_write(0, encoded, encoded_size);
    tud_cdc_n_write_flush(0);
  }
  xSemaphoreGive(s_tx_mutex);
}

static void send_ack(uint8_t type, uint8_t sequence, uint8_t status) {
  const uint8_t payload[] = {type, sequence, status};
  send_frame(LVHB_MSG_ACK, payload, sizeof(payload));
}

static void send_log(const char *text) {
  send_frame(LVHB_MSG_LOG, (const uint8_t *) text, strlen(text));
}

static void send_status(void) {
  const uint32_t generation = g_layout.generation;
  const uint8_t payload[] = {
    tud_mounted() ? 1U : 0U,
    tud_suspended() ? 1U : 0U,
    (uint8_t) generation,
    (uint8_t) (generation >> 8U),
    (uint8_t) (generation >> 16U),
    (uint8_t) (generation >> 24U),
  };
  send_frame(LVHB_MSG_STATUS, payload, sizeof(payload));
}

static bool start_transfer(uint8_t slot, const uint8_t *report, uint16_t size) {
  const uint8_t kind = g_layout.slots[slot].kind;
  if (kind == LVHB_SLOT_XINPUT) {
    return xinput_send(slot, report, size);
  }
  if (kind == LVHB_SLOT_HID || kind == LVHB_SLOT_KEYBOARD_MOUSE) {
    const int8_t instance = g_bindings[slot].hid_instance;
    // Report bytes already carry their report id, so pass id 0.
    return instance >= 0 && tud_hid_n_ready((uint8_t) instance) && tud_hid_n_report((uint8_t) instance, 0, report, size);
  }
  return false;
}

/**
 * @brief Send the slot's pending report, copying it out of the shared state first.
 *
 * @param slot Slot index.
 * @param stream Resend the latest report even when nothing new arrived.
 */
static void pump_slot(uint8_t slot, bool stream) {
  uint8_t report[LVHB_MAX_REPORT];
  uint16_t size;
  taskENTER_CRITICAL(&s_runtime_lock);
  const bool send = s_runtime[slot].size != 0U && (s_runtime[slot].pending || stream);
  size = s_runtime[slot].size;
  memcpy(report, s_runtime[slot].report, size);
  s_runtime[slot].pending = false;
  taskEXIT_CRITICAL(&s_runtime_lock);

  if (send && !start_transfer(slot, report, size)) {
    // Endpoint busy: the completion callback picks the report up.
    taskENTER_CRITICAL(&s_runtime_lock);
    s_runtime[slot].pending = true;
    taskEXIT_CRITICAL(&s_runtime_lock);
  }
}

static int16_t clamp_i16(int32_t value) {
  return (int16_t) (value > INT16_MAX ? INT16_MAX : (value < INT16_MIN ? INT16_MIN : value));
}

static int8_t clamp_i8(int32_t value) {
  return (int8_t) (value > INT8_MAX ? INT8_MAX : (value < INT8_MIN ? INT8_MIN : value));
}

/**
 * @brief Merge a pointer report into the queued tail report when both share an id and button state.
 *
 * Merging keeps a fast host from overflowing the 1 kHz endpoint without
 * reordering clicks relative to motion.
 *
 * @return `true` when merged.
 */
static bool km_merge(km_report_t *tail, const uint8_t *report, uint16_t size) {
  if (tail->size != size || size < 2U || tail->data[0] != report[0] || tail->data[1] != report[1]) {
    return false;
  }
  if (report[0] == LVHB_KM_RELATIVE_REPORT_ID && size == LVHB_KM_RELATIVE_REPORT_SIZE) {
    for (unsigned offset = 2U; offset <= 4U; offset += 2U) {
      const int16_t queued = (int16_t) (tail->data[offset] | (tail->data[offset + 1U] << 8U));
      const int16_t added = (int16_t) (report[offset] | (report[offset + 1U] << 8U));
      const uint16_t sum = (uint16_t) clamp_i16((int32_t) queued + added);
      tail->data[offset] = (uint8_t) sum;
      tail->data[offset + 1U] = (uint8_t) (sum >> 8U);
    }
    for (unsigned offset = 6U; offset <= 7U; ++offset) {
      tail->data[offset] = (uint8_t) clamp_i8((int32_t) (int8_t) tail->data[offset] + (int8_t) report[offset]);
    }
    return true;
  }
  if (report[0] == LVHB_KM_ABSOLUTE_REPORT_ID && size == LVHB_KM_ABSOLUTE_REPORT_SIZE) {
    memcpy(tail->data, report, size);
    return true;
  }
  return false;
}

static void km_pump(uint8_t slot) {
  km_queue_t *queue = &s_km_queues[slot];
  km_report_t front;
  taskENTER_CRITICAL(&s_runtime_lock);
  if (queue->front_sending) {
    queue->repump = true;
    taskEXIT_CRITICAL(&s_runtime_lock);
    return;
  }
  if (queue->count == 0U) {
    taskEXIT_CRITICAL(&s_runtime_lock);
    return;
  }
  queue->front_sending = true;
  front = queue->items[queue->head];
  taskEXIT_CRITICAL(&s_runtime_lock);

  const bool started = start_transfer(slot, front.data, front.size);
  taskENTER_CRITICAL(&s_runtime_lock);
  if (started) {
    queue->head = (uint8_t) ((queue->head + 1U) % KM_QUEUE_DEPTH);
    --queue->count;
  }
  queue->front_sending = false;
  const bool again = queue->repump && queue->count != 0U;
  queue->repump = false;
  taskEXIT_CRITICAL(&s_runtime_lock);
  if (again) {
    km_pump(slot);
  }
}

static void km_enqueue(uint8_t slot, const uint8_t *report, uint16_t size) {
  if (size == 0U || size > KM_MAX_REPORT) {
    return;
  }
  km_queue_t *queue = &s_km_queues[slot];
  taskENTER_CRITICAL(&s_runtime_lock);
  bool queued = false;
  if (queue->count != 0U && !(queue->count == 1U && queue->front_sending)) {
    queued = km_merge(&queue->items[(queue->head + queue->count - 1U) % KM_QUEUE_DEPTH], report, size);
  }
  if (!queued && queue->count < KM_QUEUE_DEPTH) {
    km_report_t *item = &queue->items[(queue->head + queue->count) % KM_QUEUE_DEPTH];
    memcpy(item->data, report, size);
    item->size = (uint8_t) size;
    ++queue->count;
    queued = true;
  }
  if (!queued) {
    queue->dropped = true;
  }
  taskEXIT_CRITICAL(&s_runtime_lock);
  km_pump(slot);
}

/**
 * @brief Queue a keyboard/mouse slot's neutral reports (u8-length-prefixed list).
 */
static void km_enqueue_neutral(uint8_t slot) {
  const bridge_slot_config_t *config = &g_layout.slots[slot];
  size_t offset = 0;
  while (offset < config->neutral_size) {
    const uint8_t length = config->neutral[offset++];
    if (length == 0U || offset + length > config->neutral_size) {
      break;
    }
    km_enqueue(slot, config->neutral + offset, length);
    offset += length;
  }
}

void bridge_slot_submit(uint8_t slot, const uint8_t *report, uint16_t size) {
  if (slot >= LVHB_MAX_SLOTS || size == 0U || size > LVHB_MAX_REPORT) {
    return;
  }
  if (g_layout.slots[slot].kind == LVHB_SLOT_KEYBOARD_MOUSE) {
    km_enqueue(slot, report, size);
    return;
  }
  taskENTER_CRITICAL(&s_runtime_lock);
  memcpy(s_runtime[slot].report, report, size);
  s_runtime[slot].size = size;
  s_runtime[slot].pending = true;
  taskEXIT_CRITICAL(&s_runtime_lock);
  pump_slot(slot, false);
}

void bridge_slot_in_complete(uint8_t slot) {
  pump_slot(slot, false);
}

static void neutralize_slot(uint8_t slot) {
  const bridge_slot_config_t *config = &g_layout.slots[slot];
  if (config->kind == LVHB_SLOT_KEYBOARD_MOUSE) {
    km_enqueue_neutral(slot);
  } else if (config->kind != LVHB_SLOT_NONE && config->neutral_size != 0U) {
    bridge_slot_submit(slot, config->neutral, config->neutral_size);
  }
}

void bridge_forward_output(uint8_t slot, uint8_t kind, uint8_t report_id, const uint8_t *data, uint16_t size) {
  uint8_t payload[3U + LVHB_MAX_REPORT];
  size_t length = 0;
  payload[length++] = slot;
  payload[length++] = kind;
  if (report_id != 0U) {
    payload[length++] = report_id;
  }
  const size_t copy = size < (sizeof(payload) - length) ? size : (sizeof(payload) - length);
  memcpy(payload + length, data, copy);
  send_frame(LVHB_MSG_OUTPUT_REPORT, payload, length + copy);
}

static int slot_for_hid_instance(uint8_t instance) {
  for (unsigned index = 0; index < LVHB_MAX_SLOTS; ++index) {
    if (g_bindings[index].hid_instance == (int8_t) instance) {
      return (int) index;
    }
  }
  return -1;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
  const int slot = slot_for_hid_instance(instance);
  if (slot < 0) {
    return 0;
  }

  const uint8_t *source = NULL;
  uint16_t size = 0;
  uint8_t input[LVHB_MAX_REPORT];
  if (report_type == HID_REPORT_TYPE_FEATURE) {
    for (unsigned index = 0; index < LVHB_MAX_FEATURE_REPORTS; ++index) {
      const bridge_feature_report_t *feature = &g_layout.slots[slot].features[index];
      if (feature->size != 0U && feature->data[0] == report_id) {
        source = feature->data;
        size = feature->size;
        break;
      }
    }
  } else if (report_type == HID_REPORT_TYPE_INPUT) {
    taskENTER_CRITICAL(&s_runtime_lock);
    size = s_runtime[slot].size;
    memcpy(input, s_runtime[slot].report, size);
    taskEXIT_CRITICAL(&s_runtime_lock);
    source = input;
  }

  // TinyUSB already wrote the report id into the reply when one was requested.
  if (source != NULL && report_id != 0U && size != 0U && source[0] == report_id) {
    ++source;
    --size;
  }
  if (source == NULL || size == 0U) {
    return 0;
  }
  const uint16_t length = size < reqlen ? size : reqlen;
  memcpy(buffer, source, length);
  return length;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
  const int slot = slot_for_hid_instance(instance);
  if (slot < 0) {
    return;
  }
  const uint8_t kind = report_type == HID_REPORT_TYPE_FEATURE ? LVHB_REPORT_FEATURE : LVHB_REPORT_OUTPUT;
  bridge_forward_output((uint8_t) slot, kind, report_id, buffer, bufsize);
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
  (void) report;
  (void) len;
  const int slot = slot_for_hid_instance(instance);
  if (slot < 0) {
    return;
  }
  if (g_layout.slots[slot].kind == LVHB_SLOT_KEYBOARD_MOUSE) {
    km_pump((uint8_t) slot);
    return;
  }
  // HID gamepads stream continuously like real controllers; PlayStation
  // drivers treat a silent pad as disconnected.
  pump_slot((uint8_t) slot, true);
}

static void set_boot_layout(bridge_layout_t *layout) {
  memset(layout, 0, sizeof(*layout));
  layout->vendor_id = BOOT_VENDOR_ID;
  layout->product_id = BOOT_PRODUCT_ID;
  layout->bcd_device = BOOT_BCD_DEVICE;
  strcpy(layout->manufacturer, "libvirtualhid");
  strcpy(layout->product, "ESP32-S3 HID Bridge");
}

static void apply_layout(const bridge_layout_t *layout) {
  ESP_LOGI(TAG, "re-enumerating with layout generation %lu", (unsigned long) layout->generation);
  // Give the ACK time to leave the CDC FIFO before the port vanishes.
  vTaskDelay(pdMS_TO_TICKS(20));
  tud_disconnect();
  vTaskDelay(pdMS_TO_TICKS(REENUMERATE_DETACH_MS));

  taskENTER_CRITICAL(&s_runtime_lock);
  memset(s_runtime, 0, sizeof(s_runtime));
  memset(s_km_queues, 0, sizeof(s_km_queues));
  taskEXIT_CRITICAL(&s_runtime_lock);
  g_layout = *layout;
  for (unsigned index = 0; index < LVHB_MAX_SLOTS; ++index) {
    const bridge_slot_config_t *slot = &g_layout.slots[index];
    if (slot->kind == LVHB_SLOT_KEYBOARD_MOUSE) {
      // Enumerate with every key and button released; sent once the host polls.
      km_enqueue_neutral((uint8_t) index);
    } else if (slot->kind != LVHB_SLOT_NONE && slot->neutral_size != 0U) {
      memcpy(s_runtime[index].report, slot->neutral, slot->neutral_size);
      s_runtime[index].size = slot->neutral_size;
      s_runtime[index].pending = true;
    }
  }
  bridge_descriptors_build();
  s_neutralized = false;
  s_last_host_frame = xTaskGetTickCount();
  tud_connect();
}

static bool layout_has_slots(void) {
  for (unsigned index = 0; index < LVHB_MAX_SLOTS; ++index) {
    if (g_layout.slots[index].kind != LVHB_SLOT_NONE) {
      return true;
    }
  }
  return false;
}

static uint16_t read_u16(const uint8_t *data) {
  return (uint16_t) (data[0] | (data[1] << 8U));
}

static uint32_t read_u32(const uint8_t *data) {
  return (uint32_t) data[0] | ((uint32_t) data[1] << 8U) | ((uint32_t) data[2] << 16U) | ((uint32_t) data[3] << 24U);
}

static uint8_t handle_layout_begin(const uint8_t *payload, size_t size) {
  if (size < 8U) {
    return LVHB_STATUS_BAD_REQUEST;
  }
  memset(&s_staged, 0, sizeof(s_staged));
  s_staged.vendor_id = read_u16(payload);
  s_staged.product_id = read_u16(payload + 2);
  s_staged.bcd_device = read_u16(payload + 4);
  size_t offset = 6U;
  char *strings[] = {s_staged.manufacturer, s_staged.product};
  for (unsigned index = 0; index < 2U; ++index) {
    if (offset >= size) {
      return LVHB_STATUS_BAD_REQUEST;
    }
    const uint8_t length = payload[offset++];
    if (length > LVHB_MAX_STRING) {
      return LVHB_STATUS_TOO_LARGE;
    }
    if (offset + length > size) {
      return LVHB_STATUS_BAD_REQUEST;
    }
    memcpy(strings[index], payload + offset, length);
    strings[index][length] = '\0';
    offset += length;
  }
  s_staging = true;
  return LVHB_STATUS_OK;
}

static uint8_t handle_layout_slot(const uint8_t *payload, size_t size) {
  if (!s_staging) {
    return LVHB_STATUS_BAD_STATE;
  }
  if (size < 5U || payload[0] >= LVHB_MAX_SLOTS || payload[1] > LVHB_SLOT_KEYBOARD_MOUSE) {
    return LVHB_STATUS_BAD_REQUEST;
  }
  bridge_slot_config_t *slot = &s_staged.slots[payload[0]];
  const uint16_t descriptor_size = read_u16(payload + 3);
  if (descriptor_size > LVHB_MAX_REPORT_DESCRIPTOR) {
    return LVHB_STATUS_TOO_LARGE;
  }
  if (5U + descriptor_size > size) {
    return LVHB_STATUS_BAD_REQUEST;
  }
  const size_t neutral_size = size - 5U - descriptor_size;
  if (neutral_size > LVHB_MAX_REPORT) {
    return LVHB_STATUS_TOO_LARGE;
  }
  if ((payload[1] == LVHB_SLOT_HID || payload[1] == LVHB_SLOT_KEYBOARD_MOUSE) && descriptor_size == 0U) {
    return LVHB_STATUS_INVALID_LAYOUT;
  }
  memset(slot, 0, sizeof(*slot));
  slot->kind = payload[1];
  slot->interval_ms = payload[2] == 0U ? 1U : payload[2];
  slot->descriptor_size = descriptor_size;
  memcpy(slot->descriptor, payload + 5, descriptor_size);
  slot->neutral_size = (uint8_t) neutral_size;
  memcpy(slot->neutral, payload + 5 + descriptor_size, neutral_size);
  return LVHB_STATUS_OK;
}

static uint8_t handle_layout_feature(const uint8_t *payload, size_t size) {
  if (!s_staging) {
    return LVHB_STATUS_BAD_STATE;
  }
  if (size < 2U || payload[0] >= LVHB_MAX_SLOTS) {
    return LVHB_STATUS_BAD_REQUEST;
  }
  const size_t report_size = size - 1U;
  if (report_size > LVHB_MAX_REPORT) {
    return LVHB_STATUS_TOO_LARGE;
  }
  bridge_slot_config_t *slot = &s_staged.slots[payload[0]];
  for (unsigned index = 0; index < LVHB_MAX_FEATURE_REPORTS; ++index) {
    bridge_feature_report_t *feature = &slot->features[index];
    if (feature->size == 0U || feature->data[0] == payload[1]) {
      feature->size = (uint8_t) report_size;
      memcpy(feature->data, payload + 1, report_size);
      return LVHB_STATUS_OK;
    }
  }
  return LVHB_STATUS_TOO_LARGE;
}

static uint8_t validate_staged(uint32_t generation) {
  uint8_t kinds[LVHB_MAX_SLOTS];
  for (unsigned index = 0; index < LVHB_MAX_SLOTS; ++index) {
    kinds[index] = s_staged.slots[index].kind;
  }
  if (!s_staging || generation == 0U) {
    return LVHB_STATUS_BAD_STATE;
  }
  return lvhb_layout_fits(kinds) ? LVHB_STATUS_OK : LVHB_STATUS_INVALID_LAYOUT;
}

static void handle_frame(const uint8_t *frame, size_t payload_size) {
  const uint8_t type = frame[0];
  const uint8_t sequence = frame[1];
  const uint8_t *payload = frame + 2;
  s_last_host_frame = xTaskGetTickCount();
  s_neutralized = false;

  switch (type) {
    case LVHB_MSG_HELLO:
      {
        if (payload_size < 2U || read_u16(payload) != LVHB_PROTOCOL_VERSION) {
          send_ack(type, sequence, LVHB_STATUS_VERSION_MISMATCH);
          break;
        }
        uint8_t reply[10U + sizeof(BRIDGE_FIRMWARE_VERSION)];
        const uint32_t generation = g_layout.generation;
        reply[0] = (uint8_t) LVHB_PROTOCOL_VERSION;
        reply[1] = (uint8_t) (LVHB_PROTOCOL_VERSION >> 8U);
        reply[2] = LVHB_MAX_SLOTS;
        reply[3] = tud_mounted() ? 1U : 0U;
        reply[4] = (uint8_t) generation;
        reply[5] = (uint8_t) (generation >> 8U);
        reply[6] = (uint8_t) (generation >> 16U);
        reply[7] = (uint8_t) (generation >> 24U);
        reply[8] = (uint8_t) LVHB_MAX_REPORT_DESCRIPTOR;
        reply[9] = (uint8_t) (LVHB_MAX_REPORT_DESCRIPTOR >> 8U);
        memcpy(reply + 10, BRIDGE_FIRMWARE_VERSION, sizeof(BRIDGE_FIRMWARE_VERSION) - 1U);
        send_frame(LVHB_MSG_HELLO_ACK, reply, sizeof(reply) - 1U);
        break;
      }
    case LVHB_MSG_LAYOUT_BEGIN:
      send_ack(type, sequence, handle_layout_begin(payload, payload_size));
      break;
    case LVHB_MSG_LAYOUT_SLOT:
      send_ack(type, sequence, handle_layout_slot(payload, payload_size));
      break;
    case LVHB_MSG_LAYOUT_FEATURE:
      send_ack(type, sequence, handle_layout_feature(payload, payload_size));
      break;
    case LVHB_MSG_LAYOUT_COMMIT:
      {
        const uint32_t generation = payload_size >= 4U ? read_u32(payload) : 0U;
        const uint8_t status = validate_staged(generation);
        send_ack(type, sequence, status);
        if (status == LVHB_STATUS_OK) {
          s_staged.generation = generation;
          s_staging = false;
          apply_layout(&s_staged);
        }
        break;
      }
    case LVHB_MSG_INPUT_REPORT:
      if (payload_size >= 2U && payload[0] < LVHB_MAX_SLOTS && g_layout.slots[payload[0]].kind != LVHB_SLOT_NONE) {
        bridge_slot_submit(payload[0], payload + 1, (uint16_t) (payload_size - 1U));
        bridge_led_activity();
      }
      break;
    case LVHB_MSG_PING:
      send_frame(LVHB_MSG_PONG, payload, payload_size >= 4U ? 4U : payload_size);
      break;
    default:
      send_log("unknown message type");
      break;
  }
}

static void check_host_watchdog(void) {
  if (!layout_has_slots()) {
    return;
  }
  const TickType_t idle = xTaskGetTickCount() - s_last_host_frame;
  if (idle >= pdMS_TO_TICKS(LVHB_HOST_IDLE_RELEASE_MS)) {
    ESP_LOGW(TAG, "host silent, dropping all slots");
    bridge_layout_t boot;
    set_boot_layout(&boot);
    apply_layout(&boot);
  } else if (!s_neutralized && idle >= pdMS_TO_TICKS(LVHB_HOST_IDLE_NEUTRAL_MS)) {
    ESP_LOGW(TAG, "host silent, sending neutral reports");
    s_neutralized = true;
    for (uint8_t index = 0; index < LVHB_MAX_SLOTS; ++index) {
      neutralize_slot(index);
    }
  }
}

static void notify_control_task(void) {
  if (s_control_task != NULL) {
    xTaskNotifyGive(s_control_task);
  }
}

static void on_cdc_rx(int itf, cdcacm_event_t *event) {
  (void) itf;
  (void) event;
  notify_control_task();
}

static void on_cdc_line_state(int itf, cdcacm_event_t *event) {
  (void) itf;
  const bool connected = event->line_state_changed_data.dtr;
  if (connected && !s_host_connected) {
    s_last_host_frame = xTaskGetTickCount();
  }
  s_host_connected = connected;
  notify_control_task();
}

static void control_task(void *argument) {
  (void) argument;
  static uint8_t encoded[LVHB_MAX_ENCODED_FRAME];
  static uint8_t frame[LVHB_MAX_FRAME];
  size_t encoded_size = 0;
  bool overflow = false;
  bool was_mounted = false;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONTROL_TASK_PERIOD_MS));

    uint8_t chunk[64];
    uint32_t count;
    while ((count = tud_cdc_n_read(0, chunk, sizeof(chunk))) > 0U) {
      for (uint32_t index = 0; index < count; ++index) {
        const uint8_t byte = chunk[index];
        if (byte != 0U) {
          if (encoded_size < sizeof(encoded)) {
            encoded[encoded_size++] = byte;
          } else {
            overflow = true;
          }
          continue;
        }
        if (!overflow && encoded_size != 0U) {
          const int payload_size = lvhb_decode_frame(encoded, encoded_size, frame);
          if (payload_size >= 0) {
            handle_frame(frame, (size_t) payload_size);
          } else {
            send_log("dropped corrupt frame");
          }
        }
        encoded_size = 0;
        overflow = false;
      }
    }

    // HID slots stream; kick the first transfer once the host configures us.
    const bool mounted = tud_mounted();
    if (mounted && !was_mounted) {
      for (uint8_t index = 0; index < LVHB_MAX_SLOTS; ++index) {
        pump_slot(index, true);
      }
      send_status();
    }
    was_mounted = mounted;

    for (uint8_t index = 0; index < LVHB_MAX_SLOTS; ++index) {
      if (g_layout.slots[index].kind != LVHB_SLOT_KEYBOARD_MOUSE) {
        continue;
      }
      // Retry reports that found the endpoint busy or the bus not yet configured.
      km_pump(index);
      bool dropped;
      taskENTER_CRITICAL(&s_runtime_lock);
      dropped = s_km_queues[index].dropped;
      s_km_queues[index].dropped = false;
      taskEXIT_CRITICAL(&s_runtime_lock);
      if (dropped) {
        send_log("keyboard/mouse queue full, report dropped");
      }
    }

    check_host_watchdog();

    bridge_led_state_t led = BRIDGE_LED_OFF;
    if (mounted) {
      led = !s_host_connected ? BRIDGE_LED_IDLE
            : !layout_has_slots() ? BRIDGE_LED_HOST_OPEN
            : s_neutralized ? BRIDGE_LED_HOST_SILENT
                            : BRIDGE_LED_ACTIVE;
    }
    bridge_led_update(led);
  }
}

void app_main(void) {
  uint8_t mac[6];
  char serial[LVHB_MAX_STRING + 1U];
  ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
  snprintf(serial, sizeof(serial), LVHB_SERIAL_PREFIX "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  bridge_descriptors_set_serial(serial);
  set_boot_layout(&g_layout);
  bridge_descriptors_build();

  s_tx_mutex = xSemaphoreCreateMutex();
  bridge_led_init();

  const void *device;
  const uint8_t *configuration;
  const char **strings;
  int string_count;
  bridge_descriptors_get(&device, &configuration, &strings, &string_count);
  const tinyusb_config_t usb_config = {
    .device_descriptor = device,
    .string_descriptor = strings,
    .string_descriptor_count = string_count,
    .external_phy = false,
    .configuration_descriptor = configuration,
  };
  ESP_ERROR_CHECK(tinyusb_driver_install(&usb_config));

  const tinyusb_config_cdcacm_t cdc_config = {
    .usb_dev = TINYUSB_USBDEV_0,
    .cdc_port = TINYUSB_CDC_ACM_0,
    .callback_rx = on_cdc_rx,
    .callback_rx_wanted_char = NULL,
    .callback_line_state_changed = on_cdc_line_state,
    .callback_line_coding_changed = NULL,
  };
  ESP_ERROR_CHECK(tusb_cdc_acm_init(&cdc_config));
  xTaskCreatePinnedToCore(control_task, "lvhb_control", 6144, NULL, 5, &s_control_task, 1);
  ESP_LOGI(TAG, "bridge ready, serial %s", serial);
}
