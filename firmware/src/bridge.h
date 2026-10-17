/**
 * @file bridge.h
 * @brief Shared state for the ESP32-S3 libvirtualhid USB bridge firmware.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp32_bridge_protocol.h"

#define BRIDGE_FIRMWARE_VERSION "0.2.0"

/// Interface numbers 0 and 1 always belong to CDC so the host COM port stays put.
#define BRIDGE_CDC_INTERFACES 2U
#define BRIDGE_EP_CDC_NOTIF 0x81U
#define BRIDGE_EP_CDC_OUT 0x02U
#define BRIDGE_EP_CDC_IN 0x82U
/// Vendor request code for the MS OS 2.0 descriptor set.
#define BRIDGE_MS_OS_20_VENDOR_CODE 0x20U

/**
 * @brief Cached feature report answered from GET_REPORT(Feature).
 */
typedef struct {
  uint8_t size;  ///< Report size including the report id, 0 when unused.
  uint8_t data[LVHB_MAX_REPORT];  ///< Report bytes; data[0] is the report id.
} bridge_feature_report_t;

/**
 * @brief Host-supplied configuration of one slot.
 */
typedef struct {
  uint8_t kind;  ///< #lvhb_slot_kind.
  uint8_t interval_ms;  ///< Interrupt IN polling interval.
  uint16_t descriptor_size;  ///< HID report descriptor size.
  uint8_t descriptor[LVHB_MAX_REPORT_DESCRIPTOR];  ///< HID report descriptor.
  uint8_t neutral_size;  ///< Neutral input report size.
  uint8_t neutral[LVHB_MAX_REPORT];  ///< Input report sent when the host goes quiet.
  bridge_feature_report_t features[LVHB_MAX_FEATURE_REPORTS];  ///< Cached feature reports.
} bridge_slot_config_t;

/**
 * @brief Complete enumerated personality.
 */
typedef struct {
  uint16_t vendor_id;  ///< USB idVendor.
  uint16_t product_id;  ///< USB idProduct.
  uint16_t bcd_device;  ///< USB bcdDevice.
  char manufacturer[LVHB_MAX_STRING + 1U];  ///< Manufacturer string.
  char product[LVHB_MAX_STRING + 1U];  ///< Product string.
  uint32_t generation;  ///< Host-chosen layout generation, 0 for the boot layout.
  bridge_slot_config_t slots[LVHB_MAX_SLOTS];  ///< Slot configuration.
} bridge_layout_t;

/**
 * @brief Runtime endpoint and interface assignment for one enumerated slot.
 */
typedef struct {
  uint8_t interface_number;  ///< USB interface number.
  uint8_t ep_in;  ///< Interrupt IN endpoint address.
  uint8_t ep_out;  ///< Interrupt OUT endpoint address, 0 when absent.
  int8_t hid_instance;  ///< TinyUSB HID instance, -1 for non-HID slots.
} bridge_slot_binding_t;

/// Active layout; only modified while the bus is disconnected.
extern bridge_layout_t g_layout;
/// Interface and endpoint assignment for #g_layout.
extern bridge_slot_binding_t g_bindings[LVHB_MAX_SLOTS];

/**
 * @brief Rebuild device, configuration, string, and BOS descriptors from #g_layout.
 */
void bridge_descriptors_build(void);

/**
 * @brief Get the descriptor configuration passed to esp_tinyusb at install time.
 *
 * @param device Receives the device descriptor.
 * @param configuration Receives the configuration descriptor.
 * @param strings Receives the string table.
 * @param string_count Receives the string table size.
 */
void bridge_descriptors_get(const void **device, const uint8_t **configuration, const char ***strings, int *string_count);

/**
 * @brief Set the USB serial number string.
 *
 * @param serial Serial number, at most #LVHB_MAX_STRING characters.
 */
void bridge_descriptors_set_serial(const char *serial);

/**
 * @brief Find the slot that owns a USB interface.
 *
 * @param interface_number USB interface number.
 * @return Slot index, or -1.
 */
int bridge_slot_for_interface(uint8_t interface_number);

/**
 * @brief Queue the latest input report for a slot and start a transfer when idle.
 *
 * @param slot Slot index.
 * @param report Report bytes.
 * @param size Report size.
 */
void bridge_slot_submit(uint8_t slot, const uint8_t *report, uint16_t size);

/**
 * @brief Forward a USB-host output or feature report to the bridge host.
 *
 * @param slot Slot index.
 * @param kind #lvhb_report_kind.
 * @param report_id Report id, prepended when non-zero (TinyUSB strips it from SET_REPORT data).
 * @param data Report bytes.
 * @param size Report size.
 */
void bridge_forward_output(uint8_t slot, uint8_t kind, uint8_t report_id, const uint8_t *data, uint16_t size);

/**
 * @brief Start an XInput IN transfer with the latest report for a slot.
 *
 * @param slot Slot index.
 * @param report Report bytes.
 * @param size Report size.
 * @return `true` when the transfer started.
 */
bool xinput_send(uint8_t slot, const uint8_t *report, uint16_t size);

/**
 * @brief Clear XInput endpoint state after a bus reset.
 */
void xinput_reset_all(void);

/**
 * @brief Called from the XInput driver when an IN transfer completes.
 *
 * @param slot Slot index.
 */
void bridge_slot_in_complete(uint8_t slot);

/**
 * @brief Status LED colour.
 */
typedef enum {
  BRIDGE_LED_OFF,  ///< USB not configured.
  BRIDGE_LED_IDLE,  ///< Configured, no host program has the port open (off).
  BRIDGE_LED_HOST_OPEN,  ///< Host program connected, no slots yet (blue).
  BRIDGE_LED_ACTIVE,  ///< Slots enumerated and host alive (green).
  BRIDGE_LED_HOST_SILENT,  ///< Host went quiet, neutral reports sent (amber).
  BRIDGE_LED_CONFIRM,  ///< A request waits for the BOOT button (purple).
  BRIDGE_LED_BUTTON_HELD,  ///< BOOT is held past a short press; release now or keep holding to reset (white).
} bridge_led_state_t;

/**
 * @brief Set up the onboard status LED; a no-op when the board has none.
 */
void bridge_led_init(void);

/**
 * @brief Record host traffic; the next update blinks the LED.
 */
void bridge_led_activity(void);

/**
 * @brief Refresh the LED, called periodically from the control task.
 *
 * @param state Current bridge state.
 */
void bridge_led_update(bridge_led_state_t state);

/**
 * @brief Outcome of one pairing poll.
 */
typedef struct {
  size_t result_size;  ///< CONFIRM_RESULT payload size, 0 when there is nothing to send.
  uint8_t result[2U + LVHB_KEY_ID_SIZE + LVHB_SECRET_SIZE];  ///< CONFIRM_RESULT payload.
  bool authorize;  ///< A pairing was confirmed; authorize the session with #key_id.
  uint8_t key_id[LVHB_KEY_ID_SIZE];  ///< Key id of the new pairing.
  bool config_changed;  ///< Pairings or the pairing requirement changed; re-check the session.
} bridge_pairing_event_t;

/**
 * @brief Load stored pairings and set up the BOOT button.
 *
 * @param device_id Bridge identity bound into every authentication, #LVHB_DEVICE_ID_SIZE bytes.
 */
void bridge_pairing_init(const uint8_t *device_id);

/**
 * @brief Check whether layout and input frames need an authenticated session.
 *
 * @return `true` unless the user turned pairing off.
 */
bool bridge_pairing_required(void);

/**
 * @brief Draw a fresh authentication challenge.
 *
 * @param nonce Receives #LVHB_NONCE_SIZE bytes.
 */
void bridge_pairing_new_nonce(uint8_t *nonce);

/**
 * @brief Check an AUTH answer against the current challenge.
 *
 * @param key_id Pairing key id.
 * @param mac HMAC-SHA256 from the host.
 * @return `true` when the answer is valid.
 */
bool bridge_pairing_verify(const uint8_t *key_id, const uint8_t *mac);

/**
 * @brief Check whether a session is still allowed after a configuration change.
 *
 * @param has_key Whether the session authenticated with a key.
 * @param key_id Key id of the session.
 * @return `true` when the session may keep sending layout and input frames.
 */
bool bridge_pairing_session_valid(bool has_key, const uint8_t *key_id);

/**
 * @brief Arm a PAIR or CONFIG_SET request that waits for the BOOT button.
 *
 * @param type Message type.
 * @param payload Request payload.
 * @param size Payload size.
 * @return #lvhb_status for the immediate ACK.
 */
uint8_t bridge_pairing_request(uint8_t type, const uint8_t *payload, size_t size);

/**
 * @brief Drop a request whose host went away.
 */
void bridge_pairing_cancel(void);

/**
 * @brief Check whether a request waits for the BOOT button.
 *
 * @return `true` while a request is armed.
 */
bool bridge_pairing_pending(void);

/**
 * @brief Check whether BOOT is held longer than a confirming press.
 *
 * @return `true` while held.
 */
bool bridge_pairing_button_held(void);

/**
 * @brief Build a CONFIG payload.
 *
 * @param out Destination.
 * @param capacity Destination size.
 * @return Payload size.
 */
size_t bridge_pairing_config(uint8_t *out, size_t capacity);

/**
 * @brief Handle the BOOT button and confirmation timeouts; call every control tick.
 *
 * @param event Receives what the control task must do.
 */
void bridge_pairing_poll(bridge_pairing_event_t *event);
