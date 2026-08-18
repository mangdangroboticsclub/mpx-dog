#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
/* The real header pulls in FreeRTOS; several .cc files rely on that. */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
typedef struct spi_device_t* spi_device_handle_t;
typedef enum { SPI1_HOST=0, SPI2_HOST=1, SPI3_HOST=2 } spi_host_device_t;
#define SPI_DMA_CH_AUTO 3
typedef struct { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num, max_transfer_sz, flags; } spi_bus_config_t;
typedef struct { int command_bits, address_bits, dummy_bits, mode, duty_cycle_pos, cs_ena_pretrans, cs_ena_posttrans,
                 clock_speed_hz, input_delay_ns, spics_io_num, flags, queue_size; void *pre_cb,*post_cb; } spi_device_interface_config_t;
typedef struct { uint32_t flags; uint16_t cmd; uint64_t addr; size_t length, rxlength; void *user;
                 const void *tx_buffer; void *rx_buffer; } spi_transaction_t;
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t spi_bus_initialize(spi_host_device_t, const spi_bus_config_t*, int);
esp_err_t spi_bus_add_device(spi_host_device_t, const spi_device_interface_config_t*, spi_device_handle_t*);
esp_err_t spi_bus_remove_device(spi_device_handle_t);
esp_err_t spi_bus_free(spi_host_device_t);
esp_err_t spi_device_transmit(spi_device_handle_t, spi_transaction_t*);
esp_err_t spi_device_polling_transmit(spi_device_handle_t, spi_transaction_t*);
#ifdef __cplusplus
}
#endif
