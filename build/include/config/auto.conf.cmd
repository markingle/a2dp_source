deps_config := \
	/Users/Mark/esp/esp-idf/components/app_trace/Kconfig \
	/Users/Mark/esp/esp-idf/components/aws_iot/Kconfig \
	/Users/Mark/esp/esp-idf/components/bt/Kconfig \
	/Users/Mark/esp/esp-idf/components/driver/Kconfig \
	/Users/Mark/esp/esp-idf/components/esp32/Kconfig \
	/Users/Mark/esp/esp-idf/components/esp_adc_cal/Kconfig \
	/Users/Mark/esp/esp-idf/components/esp_http_client/Kconfig \
	/Users/Mark/esp/esp-idf/components/ethernet/Kconfig \
	/Users/Mark/esp/esp-idf/components/fatfs/Kconfig \
	/Users/Mark/esp/esp-idf/components/freemodbus/Kconfig \
	/Users/Mark/esp/esp-idf/components/freertos/Kconfig \
	/Users/Mark/esp/esp-idf/components/heap/Kconfig \
	/Users/Mark/esp/esp-idf/components/http_server/Kconfig \
	/Users/Mark/esp/esp-idf/components/libsodium/Kconfig \
	/Users/Mark/esp/esp-idf/components/log/Kconfig \
	/Users/Mark/esp/esp-idf/components/lwip/Kconfig \
	/Users/Mark/esp/esp-idf/components/mbedtls/Kconfig \
	/Users/Mark/esp/esp-idf/components/mdns/Kconfig \
	/Users/Mark/esp/esp-idf/components/mqtt/Kconfig \
	/Users/Mark/esp/esp-idf/components/nvs_flash/Kconfig \
	/Users/Mark/esp/esp-idf/components/openssl/Kconfig \
	/Users/Mark/esp/esp-idf/components/pthread/Kconfig \
	/Users/Mark/esp/esp-idf/components/spi_flash/Kconfig \
	/Users/Mark/esp/esp-idf/components/spiffs/Kconfig \
	/Users/Mark/esp/esp-idf/components/tcpip_adapter/Kconfig \
	/Users/Mark/esp/esp-idf/components/vfs/Kconfig \
	/Users/Mark/esp/esp-idf/components/wear_levelling/Kconfig \
	/Users/Mark/esp/esp-idf/components/bootloader/Kconfig.projbuild \
	/Users/Mark/esp/esp-idf/components/esptool_py/Kconfig.projbuild \
	/Users/Mark/esp/esp-idf/components/partition_table/Kconfig.projbuild \
	/Users/Mark/esp/esp-idf/Kconfig

include/config/auto.conf: \
	$(deps_config)

ifneq "$(IDF_CMAKE)" "n"
include/config/auto.conf: FORCE
endif

$(deps_config): ;
