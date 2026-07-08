#pragma once

#include <cstdint>

namespace robot {

/**
 * @brief IMU data snapshot: accelerometer (g) and gyroscope (dps).
 */
struct ImuData {
    float ax = 0.0f;  ///< Accel X (g)
    float ay = 0.0f;  ///< Accel Y (g)
    float az = 0.0f;  ///< Accel Z (g)
    float gx = 0.0f;  ///< Gyro X (dps)
    float gy = 0.0f;  ///< Gyro Y (dps)
    float gz = 0.0f;  ///< Gyro Z (dps)
};

/**
 * @brief Initialise the IMU (QMI8658C) over SPI.
 *
 * - Configures SPI2_HOST (MOSI=11, MISO=13, CLK=12, CS=38)
 * - Sets up the IMU INT2 pin (GPIO39) as an input
 * - Writes the configuration registers
 * - Starts a background FreeRTOS task that reads 6-DOF data
 *   at the IMU's data-ready rate and stores the latest values
 *
 * @return true on success, false on failure.
 */
bool imu_init();

/**
 * @brief Return a copy of the latest IMU sample (thread-safe).
 */
ImuData imu_read();

/**
 * @brief Print the latest IMU data via ESP_LOGI.
 */
void imu_print();

}  // namespace robot
