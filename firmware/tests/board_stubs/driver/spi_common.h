#pragma once
#include "esp_err.h"
#include <cstdint>
enum spi_host_device_t { SPI2_HOST = 1, SPI3_HOST = 2 };
using spi_dma_chan_t = int;
constexpr spi_dma_chan_t SPI_DMA_DISABLED = 0, SPI_DMA_CH_AUTO = 3;
struct spi_bus_config_t {
    int data0_io_num, data1_io_num, sclk_io_num, data2_io_num, data3_io_num;
    int max_transfer_sz;
    uint32_t flags;
};
