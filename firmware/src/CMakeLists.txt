idf_component_register(
  SRCS "main.c" "usb_descriptors.c" "xinput_driver.c" "status_led.c"
  INCLUDE_DIRS "." "../../libvirtualhid/src/platform/esp32")
