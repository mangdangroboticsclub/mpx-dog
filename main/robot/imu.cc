#include "robot/imu.h"

#include <cstring>
#include <cmath>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "IMU";

// ── SPI pin assignments (Mini Pupper 2 board) ────────────────
#define SPI_HOST       SPI2_HOST
#define SPI_MOSI_GPIO  11
#define SPI_MISO_GPIO  13
#define SPI_CLK_GPIO   12
#define SPI_CS_GPIO    38
#define INT2_GPIO      39

// ── QMI8658C register map ────────────────────────────────────
#define REG_WHO_AM_I           0x00
#define REG_REVISION           0x01
#define REG_CTRL1              0x02  // SPI / sensor control
#define REG_CTRL2_ACC          0x03  // Accelerometer config
#define REG_CTRL3_GYRO         0x04  // Gyroscope config
#define REG_CTRL7              0x08  // Enable acc + gyro
#define REG_OUT_XL_XL          0x35  // Accel X low byte
#define REG_OUT_XH_XL          0x36
#define REG_OUT_YL_XL          0x37
#define REG_OUT_YH_XL          0x38
#define REG_OUT_ZL_XL          0x39
#define REG_OUT_ZH_XL          0x3A
#define REG_OUT_XL_G           0x3B  // Gyro X low byte
#define REG_OUT_XH_G           0x3C
#define REG_OUT_YL_G           0x3D
#define REG_OUT_YH_G           0x3E
#define REG_OUT_ZL_G           0x3F
#define REG_OUT_ZH_G           0x40

#define SPI_READ_FLAG  0x80
#define SPI_WRITE_MASK 0x7F

// ── Local state ──────────────────────────────────────────────
static spi_device_handle_t s_spi = nullptr;
static TaskHandle_t s_task = nullptr;
static robot::ImuData s_data = {};
static SemaphoreHandle_t s_mutex = nullptr;

// ── SPI helpers ──────────────────────────────────────────────

static esp_err_t spi_write_reg(uint8_t reg, uint8_t val)
{
    spi_transaction_t t{};
    t.addr       = reg & SPI_WRITE_MASK;
    t.length     = 8;
    t.tx_buffer  = &val;
    t.rx_buffer  = nullptr;
    return spi_device_transmit(s_spi, &t);
}

static esp_err_t spi_read_reg(uint8_t reg, uint8_t &val)
{
    spi_transaction_t t{};
    t.addr       = reg | SPI_READ_FLAG;
    t.length     = 8;
    t.rxlength   = 8;
    t.tx_buffer  = nullptr;
    t.rx_buffer  = &val;
    return spi_device_transmit(s_spi, &t);
}

static esp_err_t spi_read_burst(uint8_t reg, uint8_t *buf, size_t len)
{
    spi_transaction_t t{};
    t.addr       = reg | SPI_READ_FLAG;
    t.length     = len * 8;
    t.rxlength   = len * 8;
    t.tx_buffer  = nullptr;
    t.rx_buffer  = buf;
    return spi_device_transmit(s_spi, &t);
}

// ── Initialisation ──────────────────────────────────────────

static bool configure_imu()
{
    struct RegVal { uint8_t reg; uint8_t val; };

    const RegVal cfg[] = {
        { REG_CTRL1,      0b01100000 },  // addr auto-inc + little-endian + sensor enable
        { REG_CTRL7,      0b00000011 },  // enable gyro + accel
        { REG_CTRL2_ACC,  0b00000101 },  // ±2g, ODR = 235 Hz
        { REG_CTRL3_GYRO, 0b01110101 },  // ±2048 dps, ODR = 235 Hz
    };

    for (auto &c : cfg) {
        if (spi_write_reg(c.reg, c.val) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write reg 0x%02x", c.reg);
            return false;
        }
        uint8_t verify = 0;
        if (spi_read_reg(c.reg, verify) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read-back reg 0x%02x", c.reg);
            return false;
        }
        if (verify != c.val) {
            ESP_LOGE(TAG, "Reg 0x%02x: wrote 0x%02x, read 0x%02x", c.reg, c.val, verify);
            return false;
        }
        ESP_LOGI(TAG, "Reg 0x%02x = 0x%02x  OK", c.reg, c.val);
    }
    return true;
}

// ── Read 6-DOF ──────────────────────────────────────────────

static bool read_6dof(robot::ImuData &out)
{
    uint8_t raw[12] = {};
    if (spi_read_burst(REG_OUT_XL_XL, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }

    // Accel: ±2g → 16384 LSB/g
    constexpr float ACC_SCALE = 1.0f / 16384.0f;
    int16_t ax_raw = static_cast<int16_t>((raw[1] << 8) | raw[0]);
    int16_t ay_raw = static_cast<int16_t>((raw[3] << 8) | raw[2]);
    int16_t az_raw = static_cast<int16_t>((raw[5] << 8) | raw[4]);

    // Gyro: ±2048 dps → 16 LSB/dps
    constexpr float GYRO_SCALE = 1.0f / 16.0f;
    int16_t gx_raw = static_cast<int16_t>((raw[7] << 8) | raw[6]);
    int16_t gy_raw = static_cast<int16_t>((raw[9] << 8) | raw[8]);
    int16_t gz_raw = static_cast<int16_t>((raw[11] << 8) | raw[10]);

    out.ax = static_cast<float>(ax_raw) * ACC_SCALE;
    out.ay = static_cast<float>(ay_raw) * ACC_SCALE;
    out.az = static_cast<float>(az_raw) * ACC_SCALE;
    out.gx = static_cast<float>(gx_raw) * GYRO_SCALE;
    out.gy = static_cast<float>(gy_raw) * GYRO_SCALE;
    out.gz = static_cast<float>(gz_raw) * GYRO_SCALE;

    return true;
}

// ── Log-level helper ────────────────────────────────────────
// Map the Kconfig choice to the right ESP_LOG macro at compile time.
#if defined(CONFIG_APP_IMU_LOG_LEVEL_VERBOSE)
#define IMU_LOG(...) ESP_LOGV(__VA_ARGS__)
#elif defined(CONFIG_APP_IMU_LOG_LEVEL_DEBUG)
#define IMU_LOG(...) ESP_LOGD(__VA_ARGS__)
#else
#define IMU_LOG(...) ESP_LOGI(__VA_ARGS__)
#endif

// ── Background IMU task ─────────────────────────────────────

static void imu_task_fn(void *)
{
    ESP_LOGI(TAG, "IMU task started");
#if CONFIG_APP_IMU_LOG_ENABLE
    ESP_LOGI(TAG, "Logging enabled, interval=%d reads", CONFIG_APP_IMU_LOG_INTERVAL);
    uint32_t count = 0;
#else
    ESP_LOGI(TAG, "Logging disabled (enable via menuconfig)");
#endif

    while (true) {
        robot::ImuData sample;
        if (read_6dof(sample)) {
            // Atomically update shared data
            if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_data = sample;
            if (s_mutex) xSemaphoreGive(s_mutex);

            // Periodic print (gated by Kconfig)
#if CONFIG_APP_IMU_LOG_ENABLE
            if (++count % CONFIG_APP_IMU_LOG_INTERVAL == 0) {
                IMU_LOG(TAG, "Accel: x=%6.3f  y=%6.3f  z=%6.3f  |  Gyro: x=%7.1f  y=%7.1f  z=%7.1f",
                         sample.ax, sample.ay, sample.az,
                         sample.gx, sample.gy, sample.gz);
            }
#endif
        } else {
            ESP_LOGW(TAG, "SPI read failed");
        }

        // Poll at ~20 Hz (IMU ODR is 235 Hz, so this is plenty)
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ── Public API ──────────────────────────────────────────────

namespace robot {

bool imu_init()
{
    // ── Log SPI config ─────────────────────────────────────────
    ESP_LOGI(TAG, "Initialising SPI bus (SPI2_HOST, MOSI=%d, MISO=%d, CLK=%d, CS=%d)...",
             SPI_MOSI_GPIO, SPI_MISO_GPIO, SPI_CLK_GPIO, SPI_CS_GPIO);

    // ── Initialise SPI bus ─────────────────────────────────────
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num     = SPI_MOSI_GPIO;
    bus_cfg.miso_io_num     = SPI_MISO_GPIO;
    bus_cfg.sclk_io_num     = SPI_CLK_GPIO;
    bus_cfg.quadwp_io_num   = -1;
    bus_cfg.quadhd_io_num   = -1;
    bus_cfg.max_transfer_sz = 128;

    // This bus is SHARED with the four AT32 servo driver boards (same host,
    // same three pins, their own CS lines — see robot/driver_board.c). Whoever
    // initialises it first wins; the second caller gets ESP_ERR_INVALID_STATE,
    // which means "already up", not a failure. robot::init() currently brings
    // the driver boards up before the IMU, so in practice this is the second
    // caller — but neither side may assume an order.
    esp_err_t err = spi_bus_initialize(SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SPI bus already initialised (shared with servo boards)");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    } else {
        ESP_LOGI(TAG, "SPI bus initialised");
    }

    // ── Add SPI device (IMU) ───────────────────────────────────
    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.command_bits    = 0;
    dev_cfg.address_bits    = 8;
    dev_cfg.dummy_bits      = 0;
    dev_cfg.mode            = 0;
    dev_cfg.duty_cycle_pos  = 128;
    dev_cfg.cs_ena_pretrans = 0;
    dev_cfg.cs_ena_posttrans= 0;
    dev_cfg.clock_speed_hz  = 12 * 1000 * 1000;  // 12 MHz
    dev_cfg.input_delay_ns  = 0;
    dev_cfg.spics_io_num    = SPI_CS_GPIO;
    dev_cfg.flags           = 0;
    dev_cfg.queue_size      = 2;
    dev_cfg.pre_cb          = nullptr;
    dev_cfg.post_cb         = nullptr;

    err = spi_bus_add_device(SPI_HOST, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(err));
        spi_bus_free(SPI_HOST);
        return false;
    }
    ESP_LOGI(TAG, "SPI device added");

    // ── Configure INT2 pin as input ────────────────────────────
    gpio_config_t io_conf = {};
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << INT2_GPIO);
    gpio_config(&io_conf);

    // ── Read WHO_AM_I ──────────────────────────────────────────
    {
        uint8_t id = 0, rev = 0;
        spi_read_reg(REG_WHO_AM_I, id);
        spi_read_reg(REG_REVISION, rev);
        ESP_LOGI(TAG, "QMI8658C: WHO_AM_I = 0x%02x, REVISION = 0x%02x", id, rev);
        if (id != 0x05) {
            ESP_LOGW(TAG, "Unexpected WHO_AM_I (expected 0x05) — continuing anyway");
        }
    }

    // ── Configure the IMU registers ─────────────────────────────
    if (!configure_imu()) {
        ESP_LOGE(TAG, "IMU register configuration failed");
        return false;
    }

    // ── Create mutex for thread-safe data access ───────────────
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return false;
    }

    // ── Spawn background IMU task (core 0, priority 5) ────────
    BaseType_t rv = xTaskCreatePinnedToCore(
        imu_task_fn,
        "imu",
        4096,
        nullptr,
        5,
        &s_task,
        0);

    if (rv != pdPASS) {
        ESP_LOGE(TAG, "Failed to create IMU task");
        return false;
    }

    ESP_LOGI(TAG, "IMU initialised successfully");
    return true;
}

ImuData imu_read()
{
    ImuData copy = {};
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        copy = s_data;
        xSemaphoreGive(s_mutex);
    }
    return copy;
}

void imu_print()
{
    ImuData d = imu_read();
#if CONFIG_APP_IMU_LOG_ENABLE
    IMU_LOG(TAG, "Accel: ax=%6.3f  ay=%6.3f  az=%6.3f  |  Gyro: gx=%7.1f  gy=%7.1f  gz=%7.1f",
             d.ax, d.ay, d.az, d.gx, d.gy, d.gz);
#else
    ESP_LOGI(TAG, "IMU data available via robot::imu_read() (logging disabled in Kconfig)");
#endif
}

}  // namespace robot
