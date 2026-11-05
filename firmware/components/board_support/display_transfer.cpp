// The LCD draw buffers live in PSRAM. IDF copies each SPI chunk to internal
// DMA memory; a 48 KB chunk can fail after Wi-Fi/UI startup. Bound each bounce
// allocation to 4092 bytes. The LCD driver already streams chunks under CS.
#include "driver/spi_common.h"
extern "C" esp_err_t __real_spi_bus_initialize(spi_host_device_t host,
    const spi_bus_config_t *config, spi_dma_chan_t dma);
extern "C" esp_err_t __wrap_spi_bus_initialize(spi_host_device_t host,
    const spi_bus_config_t *config, spi_dma_chan_t dma) {
    if (host != SPI2_HOST || !config || dma == SPI_DMA_DISABLED)
        return __real_spi_bus_initialize(host, config, dma);
    auto bounded = *config;
    if (bounded.max_transfer_sz > 4092) bounded.max_transfer_sz = 4092;
    return __real_spi_bus_initialize(host, &bounded, dma);
}
