/**
 * @file status_led.c
 * @brief Onboard WS2812 status LED: dim state colour, blinks off on input traffic.
 */
#include "bridge.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#ifndef BRIDGE_LED_GPIO
  /// ESP32-S3-DevKitC-1 v1.0 and most clones; v1.1 uses GPIO38. -1 disables the LED.
  #define BRIDGE_LED_GPIO 48
#endif

#define LED_BLINK_MS 40U
#define LED_LEVEL 12U  ///< Brightness out of 255; WS2812s are very bright up close.

static const char *TAG = "lvhb_led";

static led_strip_handle_t s_strip;
static volatile bool s_activity;
static bool s_blink_off;
static TickType_t s_blink_since;
static uint32_t s_shown = UINT32_MAX;

void bridge_led_init(void) {
  if (BRIDGE_LED_GPIO < 0) {
    return;
  }
  const led_strip_config_t strip_config = {
    .strip_gpio_num = BRIDGE_LED_GPIO,
    .max_leds = 1,
    .led_pixel_format = LED_PIXEL_FORMAT_GRB,
    .led_model = LED_MODEL_WS2812,
  };
  const led_strip_rmt_config_t rmt_config = {
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = 10U * 1000U * 1000U,
  };
  if (led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip) != ESP_OK) {
    ESP_LOGW(TAG, "status LED unavailable on GPIO%d", BRIDGE_LED_GPIO);
    s_strip = NULL;
    return;
  }
  led_strip_clear(s_strip);
}

void bridge_led_activity(void) {
  s_activity = true;
}

void bridge_led_update(bridge_led_state_t state) {
  if (s_strip == NULL) {
    return;
  }
  const TickType_t now = xTaskGetTickCount();
  if (now - s_blink_since >= pdMS_TO_TICKS(LED_BLINK_MS)) {
    // Each blink is one off phase followed by at least one on phase, so steady traffic flickers.
    s_blink_off = !s_blink_off && s_activity;
    s_activity = false;
    s_blink_since = now;
  }

  uint32_t rgb;
  switch (state) {
    case BRIDGE_LED_ACTIVE:
      rgb = LED_LEVEL << 8;  // green
      break;
    case BRIDGE_LED_HOST_SILENT:
      rgb = (LED_LEVEL << 16) | ((LED_LEVEL / 2U) << 8);  // amber
      break;
    case BRIDGE_LED_HOST_OPEN:
      rgb = LED_LEVEL;  // blue
      break;
    default:
      rgb = 0;
      break;
  }
  if (s_blink_off) {
    rgb = 0;
  }
  if (rgb == s_shown) {
    return;
  }
  s_shown = rgb;
  led_strip_set_pixel(s_strip, 0, (rgb >> 16) & 0xFFU, (rgb >> 8) & 0xFFU, rgb & 0xFFU);
  led_strip_refresh(s_strip);
}
