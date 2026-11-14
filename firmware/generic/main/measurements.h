#pragma once

#include <cstdint>
#include "driver/i2c_master.h"

namespace measurements {

void register_types();
i2c_master_dev_handle_t i2c_device(int sda, int scl, uint8_t address);
uint8_t sensirion_crc(const uint8_t* word);

}  // namespace measurements
