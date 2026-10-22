// Compile-only check that the firmware's C protocol header matches
// libvirtualhid's C++ one:
//   c++ -std=c++23 -Ifirmware/src -Ilibvirtualhid/src tools/protocol_check.cpp -o protocol_check && ./protocol_check

#include "esp32_bridge_protocol.h"
#include "platform/esp32/esp32_bridge_protocol.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace protocol = lvh::detail::esp32::protocol;

static_assert(LVHB_PROTOCOL_VERSION == protocol::version);
static_assert(LVHB_MAX_SLOTS == protocol::max_slots);
static_assert(LVHB_MAX_PAYLOAD == protocol::max_payload);
static_assert(LVHB_MAX_FRAME == protocol::max_frame);
static_assert(LVHB_MAX_ENCODED_FRAME == protocol::max_encoded_frame);
static_assert(LVHB_MAX_REPORT_DESCRIPTOR == protocol::max_report_descriptor);
static_assert(LVHB_MAX_REPORT == protocol::max_report);
static_assert(LVHB_MAX_FEATURE_REPORTS == protocol::max_feature_reports);
static_assert(LVHB_MAX_STRING == protocol::max_string);
static_assert(std::string_view {LVHB_SERIAL_PREFIX} == protocol::serial_prefix);
static_assert(LVHB_XINPUT_REPORT_SIZE == protocol::xinput_report_size);
static_assert(LVHB_KEEPALIVE_MS == protocol::keepalive_interval.count());
static_assert(LVHB_NONCE_SIZE == protocol::nonce_size);
static_assert(LVHB_DEVICE_ID_SIZE == protocol::device_id_size);
static_assert(LVHB_KEY_ID_SIZE == protocol::key_id_size);
static_assert(LVHB_SECRET_SIZE == protocol::secret_size);
static_assert(LVHB_MAC_SIZE == protocol::mac_size);
static_assert(LVHB_MAX_PAIRINGS == protocol::max_pairings);
static_assert(LVHB_MAX_PAIRING_NAME == protocol::max_pairing_name);
static_assert(LVHB_CONFIRM_WINDOW_MS == protocol::confirm_window.count());
static_assert(std::string_view {LVHB_AUTH_CONTEXT} == protocol::auth_context);

static_assert(LVHB_MSG_HELLO == protocol::message::hello);
static_assert(LVHB_MSG_LAYOUT_BEGIN == protocol::message::layout_begin);
static_assert(LVHB_MSG_LAYOUT_SLOT == protocol::message::layout_slot);
static_assert(LVHB_MSG_LAYOUT_FEATURE == protocol::message::layout_feature);
static_assert(LVHB_MSG_LAYOUT_COMMIT == protocol::message::layout_commit);
static_assert(LVHB_MSG_AUTH == protocol::message::auth);
static_assert(LVHB_MSG_PAIR == protocol::message::pair);
static_assert(LVHB_MSG_CONFIG_GET == protocol::message::config_get);
static_assert(LVHB_MSG_CONFIG_SET == protocol::message::config_set);
static_assert(LVHB_MSG_INPUT_REPORT == protocol::message::input_report);
static_assert(LVHB_MSG_PING == protocol::message::ping);
static_assert(LVHB_MSG_HELLO_ACK == protocol::message::hello_ack);
static_assert(LVHB_MSG_ACK == protocol::message::ack);
static_assert(LVHB_MSG_OUTPUT_REPORT == protocol::message::output_report);
static_assert(LVHB_MSG_STATUS == protocol::message::status);
static_assert(LVHB_MSG_PONG == protocol::message::pong);
static_assert(LVHB_MSG_LOG == protocol::message::log);
static_assert(LVHB_MSG_CONFIRM_RESULT == protocol::message::confirm_result);
static_assert(LVHB_MSG_CONFIG == protocol::message::config);

static_assert(LVHB_AUTH_PAIRING_REQUIRED == protocol::auth_flag::pairing_required);
static_assert(LVHB_AUTH_AUTHORIZED == protocol::auth_flag::authorized);
static_assert(LVHB_CONFIG_REQUIRE_PAIRING == protocol::config_op::require_pairing);
static_assert(LVHB_CONFIG_REVOKE == protocol::config_op::revoke);
static_assert(LVHB_CONFIG_REVOKE_ALL == protocol::config_op::revoke_all);

static_assert(LVHB_SLOT_NONE == protocol::slot::none);
static_assert(LVHB_SLOT_XINPUT == protocol::slot::xinput);
static_assert(LVHB_SLOT_HID == protocol::slot::hid);
static_assert(LVHB_SLOT_KEYBOARD_MOUSE == protocol::slot::keyboard_mouse);

static_assert(LVHB_KM_KEYBOARD_REPORT_ID == protocol::km::keyboard_report_id);
static_assert(LVHB_KM_KEYBOARD_REPORT_SIZE == protocol::km::keyboard_report_size);
static_assert(LVHB_KM_RELATIVE_REPORT_ID == protocol::km::relative_report_id);
static_assert(LVHB_KM_RELATIVE_REPORT_SIZE == protocol::km::relative_report_size);
static_assert(LVHB_KM_ABSOLUTE_REPORT_ID == protocol::km::absolute_report_id);
static_assert(LVHB_KM_ABSOLUTE_REPORT_SIZE == protocol::km::absolute_report_size);

static_assert(LVHB_REPORT_OUTPUT == protocol::report_kind::output);
static_assert(LVHB_REPORT_FEATURE == protocol::report_kind::feature);

static_assert(LVHB_STATUS_OK == protocol::status::ok);
static_assert(LVHB_STATUS_BAD_REQUEST == protocol::status::bad_request);
static_assert(LVHB_STATUS_BAD_STATE == protocol::status::bad_state);
static_assert(LVHB_STATUS_INVALID_LAYOUT == protocol::status::invalid_layout);
static_assert(LVHB_STATUS_TOO_LARGE == protocol::status::too_large);
static_assert(LVHB_STATUS_VERSION_MISMATCH == protocol::status::version_mismatch);
static_assert(LVHB_STATUS_UNAUTHORIZED == protocol::status::unauthorized);
static_assert(LVHB_STATUS_BUSY == protocol::status::busy);
static_assert(LVHB_STATUS_FULL == protocol::status::full);
static_assert(LVHB_STATUS_TIMEOUT == protocol::status::timeout);
static_assert(LVHB_STATUS_NOT_FOUND == protocol::status::not_found);

// Both sides must produce identical CRCs and endpoint budgets.
constexpr std::array<std::uint8_t, 9> crc_sample {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
static_assert(protocol::crc16(crc_sample) == 0x29B1);  // CRC-16/CCITT-FALSE check value.

int main() {
  if (lvhb_crc16(crc_sample.data(), crc_sample.size()) != protocol::crc16(crc_sample)) {
    return 1;
  }
  for (std::uint8_t first = 0; first <= LVHB_SLOT_KEYBOARD_MOUSE; ++first) {
    for (std::uint8_t second = 0; second <= LVHB_SLOT_KEYBOARD_MOUSE; ++second) {
      const std::array<std::uint8_t, LVHB_MAX_SLOTS> kinds {first, second};
      if ((lvhb_layout_fits(kinds.data()) != 0) != protocol::layout_fits(kinds)) {
        return 1;
      }
    }
  }
  return 0;
}
