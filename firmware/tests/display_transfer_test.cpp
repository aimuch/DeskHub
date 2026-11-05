// DMA transfer failure modes, before implementation:
// 460800-byte bus allowance permits 48000-byte bounce allocations that fail;
// a smaller limit is raised; the caller's config or pin/flag fields change;
// non-LCD buses or non-DMA operation change; null input crashes; errors disappear.
#include "driver/spi_common.h"
#include <cassert>
#include <initializer_list>
extern "C" esp_err_t __wrap_spi_bus_initialize(spi_host_device_t, const spi_bus_config_t *, spi_dma_chan_t);
static const spi_bus_config_t *original;
static int expected;
static spi_host_device_t expected_host;
static spi_dma_chan_t expected_dma;
static esp_err_t driver_result;
extern "C" esp_err_t __real_spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *cfg, spi_dma_chan_t dma) {
    assert(host == expected_host && dma == expected_dma);
    if (!original) { assert(!cfg); return driver_result; }
    assert(cfg && cfg->max_transfer_sz == expected);
    assert(cfg->data0_io_num == original->data0_io_num && cfg->data1_io_num == original->data1_io_num);
    assert(cfg->sclk_io_num == original->sclk_io_num && cfg->data2_io_num == original->data2_io_num);
    assert(cfg->data3_io_num == original->data3_io_num && cfg->flags == original->flags);
    return driver_result;
}
int main() {
    for (auto host : {SPI2_HOST, SPI3_HOST}) for (auto dma : {SPI_DMA_DISABLED, SPI_DMA_CH_AUTO}) {
        expected_host = host; expected_dma = dma;
        for (int size : {460800, 48000, 4092, 2048, 0, -1}) {
            spi_bus_config_t cfg{11, 12, 13, 14, 15, size, 0x55};
            original = &cfg;
            expected = host == SPI2_HOST && dma != SPI_DMA_DISABLED && size > 4092 ? 4092 : size;
            for (auto result : {ESP_OK, ESP_FAIL}) {
                driver_result = result;
                assert(__wrap_spi_bus_initialize(host, &cfg, dma) == result);
                assert(cfg.max_transfer_sz == size);
            }
        }
    }
    original = nullptr;
    assert(__wrap_spi_bus_initialize(expected_host, nullptr, expected_dma) == driver_result);
}
