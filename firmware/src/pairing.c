/**
 * @file pairing.c
 * @brief Host pairing: stored secrets, session authentication, and BOOT-button confirmation.
 *
 * Runs entirely on the control task, so no locking is needed.
 */
#include <string.h>

#include "bootloader_random.h"
#include "bridge.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"

#define BOOT_BUTTON_GPIO 0
#define BUTTON_DEBOUNCE_MS 30U
#define BUTTON_SHORT_MAX_MS 3000U  ///< Longer presses do not confirm a request.
#define BUTTON_RESET_MS 10000U  ///< Holding this long removes every pairing.
#define STORE_NAMESPACE "lvhb"
#define STORE_KEY "pairing"
#define STORE_VERSION 1U

static const char *TAG = "lvhb_pair";

typedef struct {
  uint8_t key_id[LVHB_KEY_ID_SIZE];
  uint8_t secret[LVHB_SECRET_SIZE];
  uint8_t name_len;
  char name[LVHB_MAX_PAIRING_NAME];
} stored_pairing_t;

typedef struct {
  uint8_t version;
  uint8_t require_pairing;
  uint8_t count;
  stored_pairing_t pairings[LVHB_MAX_PAIRINGS];
} stored_config_t;

/// Request waiting for the BOOT button.
typedef struct {
  bool active;
  uint8_t type;  ///< LVHB_MSG_PAIR or LVHB_MSG_CONFIG_SET.
  TickType_t since;
  uint8_t op;
  uint8_t arg[LVHB_KEY_ID_SIZE];
  uint8_t name_len;
  char name[LVHB_MAX_PAIRING_NAME];
} pending_request_t;

static stored_config_t s_config;
static pending_request_t s_pending;
static uint8_t s_nonce[LVHB_NONCE_SIZE];
static uint8_t s_device_id[LVHB_DEVICE_ID_SIZE];
static bool s_button_down;
static bool s_button_reset_done;
static TickType_t s_button_changed;

static void config_defaults(void) {
  memset(&s_config, 0, sizeof(s_config));
  s_config.version = STORE_VERSION;
  s_config.require_pairing = 1U;
}

static void config_save(void) {
  nvs_handle_t handle;
  if (nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "cannot open NVS");
    return;
  }
  if (nvs_set_blob(handle, STORE_KEY, &s_config, sizeof(s_config)) != ESP_OK || nvs_commit(handle) != ESP_OK) {
    ESP_LOGE(TAG, "cannot store pairing configuration");
  }
  nvs_close(handle);
}

static void config_load(void) {
  config_defaults();
  nvs_handle_t handle;
  if (nvs_open(STORE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
    return;
  }
  stored_config_t stored;
  size_t size = sizeof(stored);
  if (nvs_get_blob(handle, STORE_KEY, &stored, &size) == ESP_OK && size == sizeof(stored) && stored.version == STORE_VERSION && stored.count <= LVHB_MAX_PAIRINGS) {
    s_config = stored;
  }
  nvs_close(handle);
}

/// True random bytes; the RF subsystem is off, so enable the ADC entropy source while drawing.
static void fill_random(uint8_t *data, size_t size) {
  bootloader_random_enable();
  esp_fill_random(data, size);
  bootloader_random_disable();
}

static int find_pairing(const uint8_t *key_id) {
  for (unsigned index = 0; index < s_config.count; ++index) {
    if (memcmp(s_config.pairings[index].key_id, key_id, LVHB_KEY_ID_SIZE) == 0) {
      return (int) index;
    }
  }
  return -1;
}

static void remove_pairing(unsigned index) {
  memmove(&s_config.pairings[index], &s_config.pairings[index + 1U], (s_config.count - index - 1U) * sizeof(stored_pairing_t));
  --s_config.count;
  memset(&s_config.pairings[s_config.count], 0, sizeof(stored_pairing_t));
}

void bridge_pairing_init(const uint8_t *device_id) {
  memcpy(s_device_id, device_id, LVHB_DEVICE_ID_SIZE);
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS unavailable (%s); pairings will not persist", esp_err_to_name(err));
  }
  config_load();

  const gpio_config_t button = {
    .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&button);
  ESP_LOGI(TAG, "pairing %s, %u paired host(s)", s_config.require_pairing ? "required" : "off", s_config.count);
}

bool bridge_pairing_required(void) {
  return s_config.require_pairing != 0U;
}

void bridge_pairing_new_nonce(uint8_t *nonce) {
  fill_random(s_nonce, sizeof(s_nonce));
  memcpy(nonce, s_nonce, sizeof(s_nonce));
}

bool bridge_pairing_verify(const uint8_t *key_id, const uint8_t *mac) {
  const int index = find_pairing(key_id);
  if (index < 0) {
    return false;
  }
  uint8_t message[sizeof(LVHB_AUTH_CONTEXT) - 1U + LVHB_NONCE_SIZE + LVHB_KEY_ID_SIZE + LVHB_DEVICE_ID_SIZE];
  size_t length = 0;
  memcpy(message, LVHB_AUTH_CONTEXT, sizeof(LVHB_AUTH_CONTEXT) - 1U);
  length += sizeof(LVHB_AUTH_CONTEXT) - 1U;
  memcpy(message + length, s_nonce, LVHB_NONCE_SIZE);
  length += LVHB_NONCE_SIZE;
  memcpy(message + length, key_id, LVHB_KEY_ID_SIZE);
  length += LVHB_KEY_ID_SIZE;
  memcpy(message + length, s_device_id, LVHB_DEVICE_ID_SIZE);
  length += LVHB_DEVICE_ID_SIZE;

  uint8_t expected[LVHB_MAC_SIZE];
  if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), s_config.pairings[index].secret, LVHB_SECRET_SIZE, message, length, expected) != 0) {
    return false;
  }
  uint8_t difference = 0;
  for (unsigned byte = 0; byte < LVHB_MAC_SIZE; ++byte) {
    difference |= (uint8_t) (expected[byte] ^ mac[byte]);
  }
  // A new nonce per attempt keeps a failed guess from being retried against the same challenge.
  fill_random(s_nonce, sizeof(s_nonce));
  return difference == 0U;
}

bool bridge_pairing_session_valid(bool has_key, const uint8_t *key_id) {
  return !bridge_pairing_required() || (has_key && find_pairing(key_id) >= 0);
}

uint8_t bridge_pairing_request(uint8_t type, const uint8_t *payload, size_t size) {
  if (s_pending.active) {
    return LVHB_STATUS_BUSY;
  }
  pending_request_t request = {.active = true, .type = type, .since = xTaskGetTickCount()};
  if (type == LVHB_MSG_PAIR) {
    if (size < 1U || payload[0] > LVHB_MAX_PAIRING_NAME || size < 1U + payload[0]) {
      return LVHB_STATUS_BAD_REQUEST;
    }
    if (s_config.count >= LVHB_MAX_PAIRINGS) {
      return LVHB_STATUS_FULL;
    }
    request.name_len = payload[0];
    memcpy(request.name, payload + 1, payload[0]);
  } else {
    if (size < 1U) {
      return LVHB_STATUS_BAD_REQUEST;
    }
    request.op = payload[0];
    switch (request.op) {
      case LVHB_CONFIG_REQUIRE_PAIRING:
        if (size < 2U || payload[1] > 1U) {
          return LVHB_STATUS_BAD_REQUEST;
        }
        request.arg[0] = payload[1];
        break;
      case LVHB_CONFIG_REVOKE:
        if (size < 1U + LVHB_KEY_ID_SIZE) {
          return LVHB_STATUS_BAD_REQUEST;
        }
        if (find_pairing(payload + 1) < 0) {
          return LVHB_STATUS_NOT_FOUND;
        }
        memcpy(request.arg, payload + 1, LVHB_KEY_ID_SIZE);
        break;
      case LVHB_CONFIG_REVOKE_ALL:
        break;
      default:
        return LVHB_STATUS_BAD_REQUEST;
    }
  }
  s_pending = request;
  ESP_LOGI(TAG, "waiting for BOOT to confirm request 0x%02x", type);
  return LVHB_STATUS_OK;
}

void bridge_pairing_cancel(void) {
  s_pending.active = false;
}

bool bridge_pairing_pending(void) {
  return s_pending.active;
}

size_t bridge_pairing_config(uint8_t *out, size_t capacity) {
  size_t length = 0;
  if (capacity < 2U) {
    return 0;
  }
  out[length++] = s_config.require_pairing ? LVHB_AUTH_PAIRING_REQUIRED : 0U;
  out[length++] = s_config.count;
  for (unsigned index = 0; index < s_config.count; ++index) {
    const stored_pairing_t *pairing = &s_config.pairings[index];
    if (length + LVHB_KEY_ID_SIZE + 1U + pairing->name_len > capacity) {
      break;
    }
    memcpy(out + length, pairing->key_id, LVHB_KEY_ID_SIZE);
    length += LVHB_KEY_ID_SIZE;
    out[length++] = pairing->name_len;
    memcpy(out + length, pairing->name, pairing->name_len);
    length += pairing->name_len;
  }
  return length;
}

/// Apply the confirmed request and build its CONFIRM_RESULT payload.
static void confirm_pending(bridge_pairing_event_t *event) {
  event->result[0] = s_pending.type;
  event->result[1] = LVHB_STATUS_OK;
  event->result_size = 2U;
  if (s_pending.type == LVHB_MSG_PAIR) {
    if (s_config.count >= LVHB_MAX_PAIRINGS) {
      event->result[1] = LVHB_STATUS_FULL;
    } else {
      stored_pairing_t *pairing = &s_config.pairings[s_config.count];
      memset(pairing, 0, sizeof(*pairing));
      do {
        fill_random(pairing->key_id, LVHB_KEY_ID_SIZE);
      } while (find_pairing(pairing->key_id) >= 0);
      fill_random(pairing->secret, LVHB_SECRET_SIZE);
      pairing->name_len = s_pending.name_len;
      memcpy(pairing->name, s_pending.name, s_pending.name_len);
      ++s_config.count;
      config_save();
      memcpy(event->result + 2, pairing->key_id, LVHB_KEY_ID_SIZE);
      memcpy(event->result + 2 + LVHB_KEY_ID_SIZE, pairing->secret, LVHB_SECRET_SIZE);
      event->result_size += LVHB_KEY_ID_SIZE + LVHB_SECRET_SIZE;
      event->authorize = true;
      memcpy(event->key_id, pairing->key_id, LVHB_KEY_ID_SIZE);
      ESP_LOGI(TAG, "paired host %.*s", pairing->name_len, pairing->name);
    }
  } else {
    switch (s_pending.op) {
      case LVHB_CONFIG_REQUIRE_PAIRING:
        s_config.require_pairing = s_pending.arg[0];
        break;
      case LVHB_CONFIG_REVOKE:
        {
          const int index = find_pairing(s_pending.arg);
          if (index >= 0) {
            remove_pairing((unsigned) index);
          } else {
            event->result[1] = LVHB_STATUS_NOT_FOUND;
          }
          break;
        }
      default:
        s_config.count = 0;
        memset(s_config.pairings, 0, sizeof(s_config.pairings));
        break;
    }
    config_save();
    event->config_changed = true;
  }
  s_pending.active = false;
}

void bridge_pairing_poll(bridge_pairing_event_t *event) {
  memset(event, 0, sizeof(*event));
  const TickType_t now = xTaskGetTickCount();

  if (s_pending.active && now - s_pending.since >= pdMS_TO_TICKS(LVHB_CONFIRM_WINDOW_MS)) {
    event->result[0] = s_pending.type;
    event->result[1] = LVHB_STATUS_TIMEOUT;
    event->result_size = 2U;
    s_pending.active = false;
    return;
  }

  const bool down = gpio_get_level(BOOT_BUTTON_GPIO) == 0;
  if (down != s_button_down && now - s_button_changed >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
    const TickType_t held = now - s_button_changed;
    s_button_down = down;
    s_button_changed = now;
    if (!down && !s_button_reset_done && held < pdMS_TO_TICKS(BUTTON_SHORT_MAX_MS) && s_pending.active) {
      confirm_pending(event);
      return;
    }
    s_button_reset_done = false;
  }
  if (s_button_down && !s_button_reset_done && now - s_button_changed >= pdMS_TO_TICKS(BUTTON_RESET_MS)) {
    s_button_reset_done = true;
    config_defaults();
    config_save();
    s_pending.active = false;
    event->config_changed = true;
    ESP_LOGW(TAG, "BOOT held: removed all pairings, pairing required");
  }
}

bool bridge_pairing_button_held(void) {
  return s_button_down && xTaskGetTickCount() - s_button_changed >= pdMS_TO_TICKS(BUTTON_SHORT_MAX_MS);
}
