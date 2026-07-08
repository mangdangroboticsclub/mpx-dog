#include "robot/robot.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

extern "C" {
#include "SCServo.h"
}

#include "wasm/wasm_sandbox.h"

static const char *TAG = "robot";

namespace robot {
namespace {

// ── NVS handle ───────────────────────────────────────────────
nvs_handle_t s_nvs = 0;

// ── Current gait command (written by send_gait_cmd, read by gait task) ──
GaitCmd s_gait_cmd = GaitCmd::None;

// ── Robot configuration (cached + persisted) ─────────────────
Config s_cfg;

// ── Servo offsets in degrees (index 1‑12, index 0 unused) ────
float s_offset[13] = {};

// ── Goal buffers (1‑based, index 0 unused) ───────────────────
uint16_t s_goal_pos[13]   = {};
uint16_t s_goal_speed[13] = {};

// ── Sync‑write ID list for all 12 servos ─────────────────────
const uint8_t s_sync_ids[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

// ── Gait task handle ─────────────────────────────────────────
TaskHandle_t s_gait_task_handle = nullptr;

// ── Helper: millis since boot ────────────────────────────────
static inline uint32_t millis()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

// ── NVS persistence helpers ──────────────────────────────────
static void nvs_put_float(const char *key, float v)
{
    if (s_nvs) {
        nvs_set_blob(s_nvs, key, &v, sizeof(v));
        nvs_commit(s_nvs);
    }
}

static float nvs_get_float(const char *key, float def)
{
    float v = def;
    size_t sz = sizeof(v);
    if (!s_nvs || nvs_get_blob(s_nvs, key, &v, &sz) != ESP_OK) {
        v = def;
    }
    return v;
}

static void nvs_put_i32(const char *key, int32_t v)
{
    if (s_nvs) {
        nvs_set_i32(s_nvs, key, v);
        nvs_commit(s_nvs);
    }
}

// ── Inverse kinematics (shared math) ─────────────────────────
// Given x (forward), th0 (hip angle deg), z (height),
// compute shoulder (th1) and knee (th2) angles and write all 3 servos.
static void calculate_ik(float x, float th0_deg, float z,
                          float &th1_deg, float &th2_deg)
{
    const float th0_rad = th0_deg * PI / 180.0f;
    const float zd = z / std::cos(th0_rad);
    const float ld = std::sqrt(x * x + zd * zd);
    const float phi = std::atan2(x, zd);
    const float cos_arg = (L1 * L1 + ld * ld - L2 * L2) / (2.0f * L1 * ld);
    // Clamp to [-1, 1] to avoid domain errors
    const float clamped = (cos_arg > 1.0f) ? 1.0f : (cos_arg < -1.0f) ? -1.0f : cos_arg;
    const float th1 = phi - std::acos(clamped);
    const float th2 = std::asin((ld * ld - L1 * L1 - L2 * L2) / (2.0f * L1 * L2)) - th1;

    th1_deg = th1 * 180.0f / PI;
    th2_deg = th2 * 180.0f / PI;
}
 
// ── Body-attitude pose (imported from StanfordQuadruped) ─────
// StanfordQuadruped aims the "head" by holding the four feet planted
// while rotating the whole body (roll/pitch/yaw).  This helper does
// the same: it computes each foot's position in the rotated body
// frame, then converts that to this robot's per-leg IK inputs
// (x = forward, th0 = hip yaw/abduction, z = depth) and buffers all
// 12 servo goals via the existing *_ik() helpers.  It does NOT flush.
//
//   roll  > 0  ->  lean right      (ROLL_SIGN)
//   pitch > 0  ->  nose up         (PITCH_SIGN)
//   yaw   > 0  ->  head turns right (YAW_SIGN)
static void pose_attitude(float roll_deg, float pitch_deg, float yaw_deg)
{
    const float H = static_cast<float>(s_cfg.height);

    const float r = roll_deg  * ROLL_SIGN  * PI / 180.0f;
    const float p = pitch_deg * PITCH_SIGN * PI / 180.0f;
    const float y = yaw_deg   * YAW_SIGN   * PI / 180.0f;

    const float cr = std::cos(r), sr = std::sin(r);
    const float cp = std::cos(p), sp = std::sin(p);
    const float cy = std::cos(y), sy = std::sin(y);

    // Body->world rotation R = Rz(yaw) * Ry(pitch) * Rx(roll).
    const float R00 = cy * cp;
    const float R01 = cy * sp * sr - sy * cr;
    const float R02 = cy * sp * cr + sy * sr;
    const float R10 = sy * cp;
    const float R11 = sy * sp * sr + cy * cr;
    const float R12 = sy * sp * cr - cy * sr;
    const float R20 = -sp;
    const float R21 = cp * sr;
    const float R22 = cp * cr;

    // Per leg: hip origin (x0,y0) in body frame (+x fwd, +y left),
    // whether it is a right-side leg, and which IK helper to call.
    struct LegDef {
        float x0, y0;
        bool  right;
        void (*ik)(float, float, float);
    };
    const LegDef legs[4] = {
        { +BODY_LX, -BODY_LY, true,  &front_right_ik }, // FR (1-3)
        { +BODY_LX, +BODY_LY, false, &front_left_ik  }, // FL (4-6)
        { -BODY_LX, -BODY_LY, true,  &rear_right_ik  }, // RR (7-9)
        { -BODY_LX, +BODY_LY, false, &rear_left_ik   }, // RL (10-12)
    };

    for (const LegDef &leg : legs) {
        // Default foot offset from body centre (foot on the ground).
        const float ox = leg.x0;
        const float oy = leg.y0;
        const float oz = -H;

        // Foot position in the rotated body frame = R^T * offset.
        const float fb_x = R00 * ox + R10 * oy + R20 * oz;
        const float fb_y = R01 * ox + R11 * oy + R21 * oz;
        const float fb_z = R02 * ox + R12 * oy + R22 * oz;

        // Position relative to this leg's hip.
        const float dx = fb_x - leg.x0;
        const float dy = fb_y - leg.y0;
        const float dz = fb_z;                       // negative (below hip)

        const float z_param = -dz;                   // positive depth
        const float outward = leg.right ? -dy : dy;  // +ve = splay outward
        const float th0_deg = std::atan2(outward, z_param) * 180.0f / PI;

        leg.ik(dx, th0_deg, z_param);
    }
}

}  // anonymous namespace

// Forward declaration of gait task (defined at end of file)
void gait_task();

// ═══════════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════════

bool init()
{
    // ── Open NVS ────────────────────────────────────────────────
    esp_err_t err = nvs_open("robot", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        s_nvs = 0;
    }

    // ── Enable servo power ──────────────────────────────────────
    gpio_set_direction(static_cast<gpio_num_t>(SERVO_POWER_PIN), GPIO_MODE_OUTPUT);
    gpio_set_level(static_cast<gpio_num_t>(SERVO_POWER_PIN), 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Servo power enabled (GPIO%d)", SERVO_POWER_PIN);

    // ── Initialise UART for SCSCL servos ────────────────────────
    // UART_NUM_1 (UART1), TX=GPIO4, RX=GPIO5, no RTS, 1 Mbaud
    ESP_LOGI(TAG, "Initialising SCSCL bus on UART1 (TX=%d, RX=%d, %d baud)...",
             SERVO_TX_PIN, SERVO_RX_PIN, SERVO_BAUD_RATE);
    ftServo_InitWithType(SERVO_SCSCL, SERVO_TX_PIN, SERVO_RX_PIN, -1, SERVO_BAUD_RATE);
    ESP_LOGI(TAG, "UART init done");

    // ── Diagnostic: Ping servo ID 1 to verify bus communication ─
    vTaskDelay(pdMS_TO_TICKS(100));
    int ping_result = Ping(1);
    if (ping_result > 0) {
        ESP_LOGI(TAG, "Servo ping OK — ID=1 responded with model=%d", ping_result);
    } else {
        ESP_LOGW(TAG, "Servo ping FAILED — ID=1 did not respond (err=%d)", ping_result);
        ESP_LOGW(TAG, "Check: power, baud rate (is it 1Mbaud?), wiring (TX→RX cross?)");
    }

    // ── Restore persisted config ────────────────────────────────
    int32_t v;
    if (s_nvs) {
        if (nvs_get_i32(s_nvs, "period", &v) == ESP_OK)   s_cfg.period    = static_cast<int>(v);
        if (nvs_get_i32(s_nvs, "height", &v) == ESP_OK)   s_cfg.height    = static_cast<int>(v);
        if (nvs_get_i32(s_nvs, "upHeight", &v) == ESP_OK) s_cfg.up_height = static_cast<int>(v);
        if (nvs_get_i32(s_nvs, "stride", &v) == ESP_OK)   s_cfg.stride    = static_cast<int>(v);
        if (nvs_get_i32(s_nvs, "tilt", &v) == ESP_OK)     s_cfg.tilt      = static_cast<int>(v);

        for (int i = 1; i <= 12; ++i) {
            char key[16];
            std::snprintf(key, sizeof(key), "offset%d", i);
            s_offset[i] = nvs_get_float(key, 0.0f);
        }
    }

    ESP_LOGI(TAG, "Config restored: period=%d height=%d upHeight=%d stride=%d tilt=%d",
             s_cfg.period, s_cfg.height, s_cfg.up_height, s_cfg.stride, s_cfg.tilt);

    // ── Initialise goal buffers to centre ───────────────────────
    for (int i = 1; i <= 12; ++i) {
        s_goal_pos[i] = 511;
        s_goal_speed[i] = 0;
    }

    // ── Initialise IMU (QMI8658C on SPI2_HOST) ──────────────────
    if (!imu_init()) {
        ESP_LOGW(TAG, "IMU init failed — continuing without IMU");
    } else {
        ESP_LOGI(TAG, "IMU initialised");
    }

    // ── Spawn gait task on core 1 ───────────────────────────────
    BaseType_t rv = xTaskCreatePinnedToCore(
        [](void *) { gait_task(); },
        "gait", 8192, nullptr, 22, &s_gait_task_handle, 1);

    if (rv != pdPASS) {
        ESP_LOGE(TAG, "Failed to create gait task");
        return false;
    }

    ESP_LOGI(TAG, "Robot HAL initialised");
    return true;
}

// ── Gait command ─────────────────────────────────────────────

void send_gait_cmd(GaitCmd cmd)
{
    s_gait_cmd = cmd;
}

GaitCmd current_gait_cmd()
{
    return s_gait_cmd;
}

// ── Configuration ────────────────────────────────────────────

Config get_config()
{
    return s_cfg;
}

void set_config(const Config &cfg)
{
    s_cfg = cfg;
    if (s_nvs) {
        nvs_put_i32("period",   cfg.period);
        nvs_put_i32("height",   cfg.height);
        nvs_put_i32("upHeight", cfg.up_height);
        nvs_put_i32("stride",   cfg.stride);
        nvs_put_i32("tilt",     cfg.tilt);
    }
}

// ── Servo offsets ────────────────────────────────────────────

float get_offset(int servo_id)
{
    if (servo_id < 1 || servo_id > 12) return 0.0f;
    return s_offset[servo_id];
}

void set_offset(int servo_id, float deg)
{
    if (servo_id < 1 || servo_id > 12) return;
    s_offset[servo_id] = deg;
    char key[16];
    std::snprintf(key, sizeof(key), "offset%d", servo_id);
    nvs_put_float(key, deg);
}

void reset_offsets()
{
    for (int i = 1; i <= 12; ++i) {
        s_offset[i] = 0.0f;
        char key[16];
        std::snprintf(key, sizeof(key), "offset%d", i);
        nvs_put_float(key, 0.0f);
    }
}

// ── Servo feedback ───────────────────────────────────────────

int read_position(int servo_id)
{
    if (servo_id < 1 || servo_id > 12) return -1;
    return ReadPos(servo_id);
}

int read_speed(int servo_id)
{
    if (servo_id < 1 || servo_id > 12) return -1;
    return ReadSpeed(servo_id);
}

int read_load(int servo_id)
{
    if (servo_id < 1 || servo_id > 12) return -1;
    return ReadLoad(servo_id);
}

int read_voltage(int servo_id)
{
    if (servo_id < 1 || servo_id > 12) return -1;
    return ReadVoltage(servo_id);
}

int read_temperature(int servo_id)
{
    if (servo_id < 1 || servo_id > 12) return -1;
    return ReadTemper(servo_id);
}

int read_moving(int servo_id)
{
    if (servo_id < 1 || servo_id > 12) return -1;
    return ReadMove(servo_id);
}

int read_current(int servo_id)
{
    if (servo_id < 1 || servo_id > 12) return -1;
    return ReadCurrent(servo_id);
}

int ping_servo(int servo_id)
{
    return Ping(static_cast<uint8_t>(servo_id));
}

// ── Low-level servo control ──────────────────────────────────

void set_servo_angle(int servo_id, float deg)
{
    // Convert degrees to raw pulse: 0° → 0, 300° → 1023
    // Centre (150°) → 511
    int sig = 511 + static_cast<int>(deg / 0.263f);
    if (sig < 0)   sig = 0;
    if (sig > 1023) sig = 1023;
    s_goal_pos[servo_id] = static_cast<uint16_t>(sig);
    ESP_LOGD(TAG, "set_servo_angle: id=%d deg=%.1f raw=%u", servo_id, deg, s_goal_pos[servo_id]);
}

void set_servo_speed(int servo_id, uint16_t speed)
{
    s_goal_speed[servo_id] = speed;
}

void set_all_servo_speed(uint16_t speed)
{
    for (int i = 1; i <= 12; ++i) s_goal_speed[i] = speed;
}

void flush()
{
    // Ensure at least 5 ms between bus accesses
    static int64_t last_us = 0;
    while (esp_timer_get_time() - last_us < 5000) {
        vTaskDelay(1);
    }
    last_us = esp_timer_get_time();

    uint16_t pos[12], spd[12], tim[12];
    for (int i = 0; i < 12; ++i) {
        pos[i] = s_goal_pos[i + 1];
        spd[i] = s_goal_speed[i + 1];
        tim[i] = 0;
    }
    SyncWritePos(const_cast<uint8_t *>(s_sync_ids), 12, pos, tim, spd);
    ESP_LOGD(TAG, "flush: SyncWritePos sent");
}

// ── Per‑leg IK ───────────────────────────────────────────────

void front_right_ik(float x, float th0, float z)
{
    float th1, th2;
    calculate_ik(x, th0, z, th1, th2);
    set_servo_angle(1,  th0            + s_offset[1]);
    set_servo_angle(2, -(th1)          + s_offset[2]);
    set_servo_angle(3,  th2            + s_offset[3]);
}

void front_left_ik(float x, float th0, float z)
{
    float th1, th2;
    calculate_ik(x, th0, z, th1, th2);
    set_servo_angle(4,  th0            + s_offset[4]);
    set_servo_angle(5,  th1            + s_offset[5]);
    set_servo_angle(6, -(th2)          + s_offset[6]);
}

void rear_right_ik(float x, float th0, float z)
{
    float th1, th2;
    calculate_ik(x, th0, z, th1, th2);
    set_servo_angle(7,  th0            + s_offset[7]);
    set_servo_angle(8, -(th1)          + s_offset[8]);
    set_servo_angle(9,  th2            + s_offset[9]);
}

void rear_left_ik(float x, float th0, float z)
{
    float th1, th2;
    calculate_ik(x, th0, z, th1, th2);
    set_servo_angle(10, th0            + s_offset[10]);
    set_servo_angle(11, th1            + s_offset[11]);
    set_servo_angle(12, -(th2)         + s_offset[12]);
}

// ═══════════════════════════════════════════════════════════════
//  Gait task (runs on core 1)
// ═══════════════════════════════════════════════════════════════

void gait_task()
{
    uint32_t time_mSt;
    float tim, tt;
    const float PI = robot::PI;

    // Start centred
    for (int i = 1; i <= 12; ++i) s_goal_pos[i] = 511;
    set_all_servo_speed(0);

    GaitCmd last_logged = GaitCmd::None;

    for (;;) {
        const GaitCmd cmd = s_gait_cmd;

        // Log gait transitions (avoid spam on continuous gaits)
        if (cmd != last_logged) {
            last_logged = cmd;
            const char *name = "none";
            switch (cmd) {
                case GaitCmd::None:      name = "none";      break;
                case GaitCmd::Init:      name = "init";      break;
                case GaitCmd::Step:      name = "step";      break;
                case GaitCmd::Roll:      name = "roll";      break;
                case GaitCmd::Pitch:     name = "pitch";     break;
                case GaitCmd::Stretch:   name = "stretch";   break;
                case GaitCmd::Advance:   name = "advance";   break;
                case GaitCmd::Back:      name = "back";      break;
                case GaitCmd::Left:      name = "left";      break;
                case GaitCmd::Right:     name = "right";     break;
                case GaitCmd::TurnL:     name = "turnL";     break;
                case GaitCmd::TurnR:     name = "turnR";     break;
                case GaitCmd::Twerk:     name = "twerk";     break;
                case GaitCmd::Jump:      name = "jump";      break;
                case GaitCmd::JumpFwd:   name = "jumpfwd";   break;
                case GaitCmd::TestSpeed: name = "testspeed"; break;
                case GaitCmd::LookUp:         name = "lookup";        break;
                case GaitCmd::LookDown:       name = "lookdown";      break;
                case GaitCmd::LookLeft:       name = "lookleft";      break;
                case GaitCmd::LookRight:      name = "lookright";     break;
                case GaitCmd::LookUpperLeft:  name = "lookul";        break;
                case GaitCmd::LookUpperRight: name = "lookur";        break;
                case GaitCmd::LookLowerLeft:  name = "lookll";        break;
                case GaitCmd::LookLowerRight: name = "looklr";        break;
                case GaitCmd::ForelegLiftL:   name = "flegL";         break;
                case GaitCmd::ForelegLiftR:   name = "flegR";         break;
                case GaitCmd::BacklegLiftL:   name = "blegL";         break;
                case GaitCmd::BacklegLiftR:   name = "blegR";         break;
                case GaitCmd::HeightUp:       name = "heightup";      break;
                case GaitCmd::HeightDown:     name = "heightdown";    break;
                case GaitCmd::Balance:        name = "balance";       break;
                case GaitCmd::BowBack:        name = "bowback";       break;
                case GaitCmd::BodyCycle:      name = "bodycycle";     break;
                case GaitCmd::HeadEllipse:    name = "headellipse";   break;
                case GaitCmd::MoveLeftFront:  name = "moveLF";        break;
                case GaitCmd::MoveRightFront: name = "moveRF";        break;
                case GaitCmd::MoveLeftBack:   name = "moveLB";        break;
                case GaitCmd::MoveRightBack:  name = "moveRB";        break;
            }
            ESP_LOGI(TAG, "Gait: %s", name);
        }

        // ── Idle (no command) ────────────────────────────────────
        if (cmd == GaitCmd::None) {
            front_right_ik(0, 0, static_cast<float>(s_cfg.height));
            rear_right_ik(0, 0, static_cast<float>(s_cfg.height));
            front_left_ik(0, 0, static_cast<float>(s_cfg.height));
            rear_left_ik(0, 0, static_cast<float>(s_cfg.height));
            // When a WASM skill is running, skip the flush so the
            // skill's own servo commands (sent via robot_flush from
            // host functions) are not immediately overwritten with
            // neutral positions.  The goal buffer is still updated
            // so positions are correct when WASM ends.
            if (!wasm::is_running()) {
                flush();
            }
            vTaskDelay(1);
            continue;
        }

        // ── Initial pose ─────────────────────────────────────────
        if (cmd == GaitCmd::Init) {
            set_all_servo_speed(0);
            for (int i = 1; i <= 12; ++i) set_servo_angle(i, s_offset[i]);
            flush();
            vTaskDelay(1);
            s_gait_cmd = GaitCmd::None;   // auto‑reset
            continue;
        }

        // ── Step (in‑place trot) ─────────────────────────────────
        if (cmd == GaitCmd::Step) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float p  = static_cast<float>(s_cfg.period);

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, 0, h - uh * std::sin(tt));
                rear_left_ik(0, 0, h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, 0, h - uh * std::cos(tt));
                rear_left_ik(0, 0, h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                rear_right_ik(0, 0, h - uh * std::sin(tt));
                front_left_ik(0, 0, h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                rear_right_ik(0, 0, h - uh * std::cos(tt));
                front_left_ik(0, 0, h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Roll ─────────────────────────────────────────────────
        if (cmd == GaitCmd::Roll) {
            const float h = static_cast<float>(s_cfg.height);
            const float t = static_cast<float>(s_cfg.tilt);
            const float p = static_cast<float>(s_cfg.period) * 8.0f;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                front_right_ik(0, -t * std::sin(tt), h);
                rear_left_ik(0,  t * std::sin(tt), h);
                rear_right_ik(0,  t * std::sin(tt), h);
                front_left_ik(0, -t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Pitch ────────────────────────────────────────────────
        if (cmd == GaitCmd::Pitch) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float p  = static_cast<float>(s_cfg.period) * 8.0f;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                front_right_ik(0, 0, h - uh * std::sin(tt));
                rear_left_ik(0, 0, h + uh * std::sin(tt));
                rear_right_ik(0, 0, h + uh * std::sin(tt));
                front_left_ik(0, 0, h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Stretch ──────────────────────────────────────────────
        if (cmd == GaitCmd::Stretch) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float p  = static_cast<float>(s_cfg.period) * 8.0f;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                front_right_ik(0, 0, h + uh * std::sin(tt));
                rear_left_ik(0, 0, h + uh * std::sin(tt));
                rear_right_ik(0, 0, h + uh * std::sin(tt));
                front_left_ik(0, 0, h + uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Advance (forward trot) ───────────────────────────────
        if (cmd == GaitCmd::Advance) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float s  = static_cast<float>(s_cfg.stride);
            const float p  = static_cast<float>(s_cfg.period);

            // Phase 1
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(-s * std::cos(tt), 0, h - uh * std::sin(tt));
                rear_left_ik(  -s * std::cos(tt), 0, h - uh * std::sin(tt));
                rear_right_ik(  s * std::cos(tt), 0, h);
                front_left_ik(  s * std::cos(tt), 0, h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 2
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik( s * std::sin(tt), 0, h - uh * std::cos(tt));
                rear_left_ik(   s * std::sin(tt), 0, h - uh * std::cos(tt));
                rear_right_ik(-s * std::sin(tt), 0, h);
                front_left_ik(-s * std::sin(tt), 0, h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 3
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik( s * std::cos(tt), 0, h);
                rear_left_ik(   s * std::cos(tt), 0, h);
                rear_right_ik(-s * std::cos(tt), 0, h - uh * std::sin(tt));
                front_left_ik(-s * std::cos(tt), 0, h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 4
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(-s * std::sin(tt), 0, h);
                rear_left_ik(  -s * std::sin(tt), 0, h);
                rear_right_ik( s * std::sin(tt), 0, h - uh * std::cos(tt));
                front_left_ik( s * std::sin(tt), 0, h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Back (backward trot) ─────────────────────────────────
        if (cmd == GaitCmd::Back) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float s  = static_cast<float>(s_cfg.stride);
            const float p  = static_cast<float>(s_cfg.period);

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik( s * std::cos(tt), 0, h - uh * std::sin(tt));
                rear_left_ik(   s * std::cos(tt) + 15, 0, h - uh * std::sin(tt));
                rear_right_ik(-s * std::cos(tt) + 15, 0, h);
                front_left_ik(-s * std::cos(tt), 0, h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(-s * std::sin(tt), 0, h - uh * std::cos(tt));
                rear_left_ik(  -s * std::sin(tt) + 15, 0, h - uh * std::cos(tt));
                rear_right_ik( s * std::sin(tt) + 15, 0, h);
                front_left_ik( s * std::sin(tt), 0, h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(-s * std::cos(tt), 0, h);
                rear_left_ik(  -s * std::cos(tt) + 15, 0, h);
                rear_right_ik( s * std::cos(tt) + 15, 0, h - uh * std::sin(tt));
                front_left_ik( s * std::cos(tt), 0, h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik( s * std::sin(tt), 0, h);
                rear_left_ik(   s * std::sin(tt) + 15, 0, h);
                rear_right_ik(-s * std::sin(tt) + 15, 0, h - uh * std::cos(tt));
                front_left_ik(-s * std::sin(tt), 0, h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Left (strafe) ────────────────────────────────────────
        if (cmd == GaitCmd::Left) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float t  = static_cast<float>(s_cfg.tilt);
            const float p  = static_cast<float>(s_cfg.period);

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0,  t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_left_ik(0,  -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_right_ik(0,  t * std::cos(tt), h - uh * std::cos(tt));
                front_left_ik(0, -t * std::cos(tt), h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                rear_right_ik(0, -t * std::sin(tt), h);
                front_left_ik(0,  t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, -t * std::cos(tt), h - uh * std::cos(tt));
                rear_left_ik(0,   t * std::cos(tt), h - uh * std::cos(tt));
                rear_right_ik(0, -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                front_left_ik(0,  t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0,  t * std::sin(tt), h);
                rear_left_ik(0,  -t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Right (strafe) ───────────────────────────────────────
        if (cmd == GaitCmd::Right) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float t  = static_cast<float>(s_cfg.tilt);
            const float p  = static_cast<float>(s_cfg.period);

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_left_ik(0,   t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_right_ik(0, -t * std::cos(tt), h - uh * std::cos(tt));
                front_left_ik(0,  t * std::cos(tt), h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                rear_right_ik(0,  t * std::sin(tt), h);
                front_left_ik(0, -t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0,  t * std::cos(tt), h - uh * std::cos(tt));
                rear_left_ik(0,  -t * std::cos(tt), h - uh * std::cos(tt));
                rear_right_ik(0,  t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                front_left_ik(0, -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, -t * std::sin(tt), h);
                rear_left_ik(0,  t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Turn Left ────────────────────────────────────────────
        if (cmd == GaitCmd::TurnL) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float t  = static_cast<float>(s_cfg.tilt);
            const float p  = static_cast<float>(s_cfg.period);

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0,  t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_left_ik(0,    t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_right_ik(0, -t * std::cos(tt), h - uh * std::cos(tt));
                front_left_ik(0, -t * std::cos(tt), h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                rear_right_ik(0,  t * std::sin(tt), h);
                front_left_ik(0,  t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, -t * std::cos(tt), h - uh * std::cos(tt));
                rear_left_ik(0,   -t * std::cos(tt), h - uh * std::cos(tt));
                rear_right_ik(0,  t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                front_left_ik(0,  t - 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0,  t * std::sin(tt), h);
                rear_left_ik(0,    t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Turn Right ───────────────────────────────────────────
        if (cmd == GaitCmd::TurnR) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float t  = static_cast<float>(s_cfg.tilt);
            const float p  = static_cast<float>(s_cfg.period);

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_left_ik(0,   -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                rear_right_ik(0,  t * std::cos(tt), h - uh * std::cos(tt));
                front_left_ik(0,  t * std::cos(tt), h - uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                rear_right_ik(0, -t * std::sin(tt), h);
                front_left_ik(0, -t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0,  t * std::cos(tt), h - uh * std::cos(tt));
                rear_left_ik(0,    t * std::cos(tt), h - uh * std::cos(tt));
                rear_right_ik(0, -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                front_left_ik(0, -t + 2.0f * t * std::sin(tt), h - uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                front_right_ik(0, -t * std::sin(tt), h);
                rear_left_ik(0,   -t * std::sin(tt), h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Twerk ────────────────────────────────────────────────
        if (cmd == GaitCmd::Twerk) {
            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float p  = static_cast<float>(s_cfg.period);

            // Phase 1: crouch front
            time_mSt = millis(); tim = 0;
            while (tim < p * 4.0f) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / (p * 4.0f);
                front_right_ik(0, 0, h - uh * std::sin(tt));
                front_left_ik(0,  0, h - uh * std::sin(tt));
                rear_right_ik(0, 0, h + uh * std::sin(tt));
                rear_left_ik(0,  0, h + uh * std::sin(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 2: wiggle
            time_mSt = millis(); tim = 0;
            while (tim < p * 6.0f) { tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                front_right_ik(0, 0, h - uh);
                front_left_ik(0,  0, h - uh);
                rear_right_ik(0, 0, h + uh * (1.0f + 0.5f * std::sin(tt)));
                rear_left_ik(0,  0, h + uh * (1.0f + 0.5f * std::sin(tt)));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 3: return
            time_mSt = millis(); tim = 0;
            while (tim < p * 4.0f) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / (p * 4.0f);
                front_right_ik(0, 0, h - uh * std::cos(tt));
                front_left_ik(0,  0, h - uh * std::cos(tt));
                rear_right_ik(0, 0, h + uh * std::cos(tt));
                rear_left_ik(0,  0, h + uh * std::cos(tt));
                flush();
                if (s_gait_cmd != cmd) break;
            }
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Jump (vertical) ──────────────────────────────────────
        if (cmd == GaitCmd::Jump) {
            const float h       = static_cast<float>(s_cfg.height);
            const float p       = static_cast<float>(s_cfg.period);
            const float crouchZ = 40.0f;
            const float pushZ   = 105.0f;
            const float tuckZ   = 45.0f;

            // Crouch
            time_mSt = millis(); tim = 0;
            while (tim < p * 2.0f) { tim = millis() - time_mSt;
                tt = tim * PI / 2.0f / (p * 2.0f);
                float z = h - (h - crouchZ) * std::sin(tt);
                front_right_ik(0, 0, z); front_left_ik(0, 0, z);
                rear_right_ik(0, 0, z);  rear_left_ik(0, 0, z);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Hold crouch
            time_mSt = millis(); tim = 0;
            while (tim < 20.0f) { tim = millis() - time_mSt;
                front_right_ik(0, 0, crouchZ); front_left_ik(0, 0, crouchZ);
                rear_right_ik(0, 0, crouchZ);  rear_left_ik(0, 0, crouchZ);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Explosive extension
            set_all_servo_speed(0);
            front_right_ik(0, 0, pushZ); front_left_ik(0, 0, pushZ);
            rear_right_ik(0, 0, pushZ);  rear_left_ik(0, 0, pushZ);
            flush();
            flush();

            int air_ms = static_cast<int>(160.0f + (70.0f - crouchZ) * 1.0f);
            vTaskDelay(pdMS_TO_TICKS(air_ms));

            // Tuck in air
            time_mSt = millis(); tim = 0;
            const float tuck_ms = 50.0f;
            while (tim < tuck_ms) { tim = millis() - time_mSt;
                float frac = std::sin(tim * PI / 2.0f / tuck_ms);
                float z = pushZ - (pushZ - tuckZ) * frac;
                front_right_ik(0, 0, z); front_left_ik(0, 0, z);
                rear_right_ik(0, 0, z);  rear_left_ik(0, 0, z);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Soft landing
            time_mSt = millis(); tim = 0;
            while (tim < p * 3.0f) { tim = millis() - time_mSt;
                tt = tim * PI / 2.0f / (p * 3.0f);
                float z = tuckZ + (h - tuckZ) * std::sin(tt);
                front_right_ik(0, 0, z); front_left_ik(0, 0, z);
                rear_right_ik(0, 0, z);  rear_left_ik(0, 0, z);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Jump Forward ─────────────────────────────────────────
        if (cmd == GaitCmd::JumpFwd) {
            const float h             = static_cast<float>(s_cfg.height);
            const float p             = static_cast<float>(s_cfg.period);
            const float crouchZrear   = 60.0f;
            const float crouchZfront  = 40.0f;
            const float pushZ         = 105.0f;
            const float tuckZ         = 45.0f;

            // Phase 1: controlled crouch
            set_all_servo_speed(100);
            front_right_ik(0, 0, crouchZfront); front_left_ik(0, 0, crouchZfront);
            rear_right_ik(0, 0, crouchZrear);   rear_left_ik(0, 0, crouchZrear);
            flush();
            vTaskDelay(pdMS_TO_TICKS(900));

            time_mSt = millis(); tim = 0;
            while (tim < 25.0f) { tim = millis() - time_mSt;
                front_right_ik(0, 0, crouchZfront); front_left_ik(0, 0, crouchZfront);
                rear_right_ik(0, 0, crouchZrear);   rear_left_ik(0, 0, crouchZrear);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 2: explosive extension
            set_all_servo_speed(0);
            front_right_ik(0, 0, pushZ); front_left_ik(0, 0, pushZ);
            rear_right_ik(0, 0, pushZ);  rear_left_ik(0, 0, pushZ);
            flush();
            flush();

            int air_ms = static_cast<int>(160.0f + (70.0f - crouchZrear) * 1.0f);
            vTaskDelay(pdMS_TO_TICKS(air_ms));

            // Phase 3: pounce tuck (rear first, then front)
            const float rear_tuck_ms = 45.0f;
            time_mSt = millis(); tim = 0;
            while (tim < rear_tuck_ms) { tim = millis() - time_mSt;
                float frac = std::sin(tim * PI / 2.0f / rear_tuck_ms);
                float zr = pushZ - (pushZ - tuckZ) * frac;
                rear_right_ik(0, 0, zr);  rear_left_ik(0, 0, zr);
                front_right_ik(0, 0, pushZ); front_left_ik(0, 0, pushZ);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            const float front_tuck_ms = 45.0f;
            time_mSt = millis(); tim = 0;
            while (tim < front_tuck_ms) { tim = millis() - time_mSt;
                float frac = std::sin(tim * PI / 2.0f / front_tuck_ms);
                float zf = pushZ - (pushZ - tuckZ) * frac;
                front_right_ik(0, 0, zf); front_left_ik(0, 0, zf);
                rear_right_ik(0, 0, tuckZ);  rear_left_ik(0, 0, tuckZ);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 4: soft landing
            set_all_servo_speed(0);
            front_right_ik(0, 0, h); front_left_ik(0, 0, h);
            rear_right_ik(0, 0, h);  rear_left_ik(0, 0, h);
            flush();
            vTaskDelay(pdMS_TO_TICKS(p * 4.0f));
            set_all_servo_speed(0);
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Speed Test ───────────────────────────────────────────
        if (cmd == GaitCmd::TestSpeed) {
            ESP_LOGI(TAG, "--- Speed Test START ---");
            set_all_servo_speed(2047);
            front_right_ik(0, 0, 70); front_left_ik(0, 0, 70);
            rear_right_ik(0, 0, 70);  rear_left_ik(0, 0, 70);
            flush();
            vTaskDelay(pdMS_TO_TICKS(1500));

            ESP_LOGI(TAG, "Moving SLOW (speed=30)");
            set_all_servo_speed(30);
            front_right_ik(0, 0, 100); front_left_ik(0, 0, 100);
            rear_right_ik(0, 0, 100);  rear_left_ik(0, 0, 100);
            flush();
            vTaskDelay(pdMS_TO_TICKS(3000));

            ESP_LOGI(TAG, "Moving FAST (speed=2047)");
            set_all_servo_speed(2047);
            front_right_ik(0, 0, 70); front_left_ik(0, 0, 70);
            rear_right_ik(0, 0, 70);  rear_left_ik(0, 0, 70);
            flush();
            vTaskDelay(pdMS_TO_TICKS(2000));

            ESP_LOGI(TAG, "--- Speed Test DONE ---");
            set_all_servo_speed(0);
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ═══════════════════════════════════════════════════════════
        //  Imported from StanfordQuadruped
        // ═══════════════════════════════════════════════════════════

        // ── Look / head poses (static body-attitude holds) ───────
        if (cmd == GaitCmd::LookUp     || cmd == GaitCmd::LookDown  ||
            cmd == GaitCmd::LookLeft   || cmd == GaitCmd::LookRight ||
            cmd == GaitCmd::LookUpperLeft  || cmd == GaitCmd::LookUpperRight ||
            cmd == GaitCmd::LookLowerLeft  || cmd == GaitCmd::LookLowerRight) {

            float pitch = 0.0f, yaw = 0.0f;
            switch (cmd) {
                case GaitCmd::LookUp:         pitch =  LOOK_PITCH_DEG; break;
                case GaitCmd::LookDown:       pitch = -LOOK_PITCH_DEG; break;
                case GaitCmd::LookLeft:       yaw   = -LOOK_YAW_DEG;   break;
                case GaitCmd::LookRight:      yaw   =  LOOK_YAW_DEG;   break;
                case GaitCmd::LookUpperLeft:  pitch =  LOOK_PITCH_DEG; yaw = -LOOK_YAW_DEG; break;
                case GaitCmd::LookUpperRight: pitch =  LOOK_PITCH_DEG; yaw =  LOOK_YAW_DEG; break;
                case GaitCmd::LookLowerLeft:  pitch = -LOOK_PITCH_DEG; yaw = -LOOK_YAW_DEG; break;
                case GaitCmd::LookLowerRight: pitch = -LOOK_PITCH_DEG; yaw =  LOOK_YAW_DEG; break;
                default: break;
            }
            set_all_servo_speed(150);   // smooth, controlled head move
            while (s_gait_cmd == cmd) {
                pose_attitude(0.0f, pitch, yaw);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            set_all_servo_speed(0);
            continue;
        }

        // ── Single-leg lifts (static hold) ───────────────────────
        if (cmd == GaitCmd::ForelegLiftL || cmd == GaitCmd::ForelegLiftR ||
            cmd == GaitCmd::BacklegLiftL || cmd == GaitCmd::BacklegLiftR) {

            const float h    = static_cast<float>(s_cfg.height);
            const float lift = 30.0f;   // how far the paw rises (mm)

            // 0 = FR, 1 = FL, 2 = RR, 3 = RL
            int leg = 0;
            switch (cmd) {
                case GaitCmd::ForelegLiftR: leg = 0; break;
                case GaitCmd::ForelegLiftL: leg = 1; break;
                case GaitCmd::BacklegLiftR: leg = 2; break;
                case GaitCmd::BacklegLiftL: leg = 3; break;
                default: break;
            }
            const bool front = (leg == 0 || leg == 1);
            const bool left  = (leg == 1 || leg == 3);

            // Shift weight away from the lifted leg for balance.
            const float pitch = front ? -6.0f :  6.0f;  // lifting front -> lean back
            const float roll  = left  ?  6.0f : -6.0f;  // lifting left  -> lean right

            set_all_servo_speed(120);
            while (s_gait_cmd == cmd) {
                pose_attitude(roll, pitch, 0.0f);       // weight shift (all 4 legs)
                switch (leg) {                          // override the lifted paw
                    case 0: front_right_ik(0.0f, 0.0f, h - lift); break;
                    case 1: front_left_ik (0.0f, 0.0f, h - lift); break;
                    case 2: rear_right_ik (0.0f, 0.0f, h - lift); break;
                    case 3: rear_left_ik  (0.0f, 0.0f, h - lift); break;
                }
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            set_all_servo_speed(0);
            continue;
        }

        // ── Static body height up / down ─────────────────────────
        if (cmd == GaitCmd::HeightUp || cmd == GaitCmd::HeightDown) {
            const float h = static_cast<float>(s_cfg.height);
            const float d = 20.0f;
            const float z = (cmd == GaitCmd::HeightUp) ? (h + d) : (h - d);
            set_all_servo_speed(120);
            while (s_gait_cmd == cmd) {
                front_right_ik(0, 0, z); front_left_ik(0, 0, z);
                rear_right_ik(0, 0, z);  rear_left_ik(0, 0, z);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            set_all_servo_speed(0);
            continue;
        }

        // ── Balance: hold a roll + pitch attitude (static) ───────
        if (cmd == GaitCmd::Balance) {
            const float t = static_cast<float>(s_cfg.tilt);
            set_all_servo_speed(120);
            while (s_gait_cmd == cmd) {
                pose_attitude(t, t, 0.0f);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            set_all_servo_speed(0);
            continue;
        }

        // ── Bow + shuffle back, then rise (auto-return) ──────────
        if (cmd == GaitCmd::BowBack) {
            const float h         = static_cast<float>(s_cfg.height);
            const float p         = static_cast<float>(s_cfg.period);
            const float bow       = 25.0f;   // front crouch (mm)
            const float backshift = 15.0f;   // body shifts back (mm)

            set_all_servo_speed(100);

            // Lower the front and shift weight backward
            time_mSt = millis(); tim = 0;
            while (tim < p * 4.0f) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / (p * 4.0f);
                float f = std::sin(tt);
                front_right_ik(backshift * f, 0, h - bow * f);
                front_left_ik (backshift * f, 0, h - bow * f);
                rear_right_ik (backshift * f, 0, h);
                rear_left_ik  (backshift * f, 0, h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Hold the bow briefly
            time_mSt = millis(); tim = 0;
            while (tim < 600.0f) { tim = millis() - time_mSt;
                flush();
                if (s_gait_cmd != cmd) break;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (s_gait_cmd != cmd) continue;

            // Rise back to neutral
            time_mSt = millis(); tim = 0;
            while (tim < p * 4.0f) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / (p * 4.0f);
                float f = std::cos(tt);
                front_right_ik(backshift * f, 0, h - bow * f);
                front_left_ik (backshift * f, 0, h - bow * f);
                rear_right_ik (backshift * f, 0, h);
                rear_left_ik  (backshift * f, 0, h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Body cycle: body centre draws a circle (auto-return) ─
        if (cmd == GaitCmd::BodyCycle) {
            const float h   = static_cast<float>(s_cfg.height);
            const float p   = static_cast<float>(s_cfg.period) * 8.0f;
            const float Rad = 15.0f;   // circle radius (mm)
            set_all_servo_speed(0);

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                const float cx = Rad * std::cos(tt);
                const float cy = Rad * std::sin(tt);
                // Feet move opposite to the body centre.
                const float dx = -cx;
                const float dy = -cy;                       // body-frame +y = left
                const float th0_r = std::atan2(-dy, h) * 180.0f / PI; // right: outward = -dy
                const float th0_l = std::atan2( dy, h) * 180.0f / PI; // left:  outward = +dy
                front_right_ik(dx, th0_r, h);
                rear_right_ik (dx, th0_r, h);
                front_left_ik (dx, th0_l, h);
                rear_left_ik  (dx, th0_l, h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Head ellipse: head sweeps an ellipse (auto-return) ───
        if (cmd == GaitCmd::HeadEllipse) {
            const float p = static_cast<float>(s_cfg.period) * 8.0f;
            set_all_servo_speed(0);

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                float pitch = LOOK_PITCH_DEG * std::sin(tt);
                float yaw   = LOOK_YAW_DEG   * std::cos(tt);
                pose_attitude(0.0f, pitch, yaw);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Diagonal trot walks ──────────────────────────────────
        if (cmd == GaitCmd::MoveLeftFront  || cmd == GaitCmd::MoveRightFront ||
            cmd == GaitCmd::MoveLeftBack   || cmd == GaitCmd::MoveRightBack) {

            const float h  = static_cast<float>(s_cfg.height);
            const float uh = static_cast<float>(s_cfg.up_height);
            const float s  = static_cast<float>(s_cfg.stride);
            const float p  = static_cast<float>(s_cfg.period);

            // xf: +1 forward, -1 backward.  yf: +1 left, -1 right.
            const float xf = (cmd == GaitCmd::MoveLeftFront || cmd == GaitCmd::MoveRightFront) ? 1.0f : -1.0f;
            const float yf = (cmd == GaitCmd::MoveLeftFront || cmd == GaitCmd::MoveLeftBack)   ? 1.0f : -1.0f;

            // Drive each foot along the (xf,yf) diagonal: the forward
            // stride value also produces a matching lateral offset
            // (converted to a hip-yaw angle), so the same Advance trot
            // timing yields a true diagonal translation.
            auto emit = [&](float aS, float aZ, float bS, float bZ,
                            float cS, float cZ, float dS, float dZ) {
                auto th = [&](float sx, bool right, float z) {
                    float lat = yf * sx;
                    float out = right ? -lat : lat;
                    return std::atan2(out, z) * 180.0f / PI;
                };
                front_right_ik(xf * aS, th(aS, true,  aZ), aZ);  // FR
                rear_left_ik  (xf * bS, th(bS, false, bZ), bZ);  // RL
                rear_right_ik (xf * cS, th(cS, true,  cZ), cZ);  // RR
                front_left_ik (xf * dS, th(dS, false, dZ), dZ);  // FL
            };

            // Phase 1
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                float c = std::cos(tt), si = std::sin(tt);
                emit(-s * c, h - uh * si, -s * c, h - uh * si,  s * c, h,  s * c, h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 2
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                float c = std::cos(tt), si = std::sin(tt);
                emit( s * si, h - uh * c,  s * si, h - uh * c, -s * si, h, -s * si, h);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 3
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                float c = std::cos(tt), si = std::sin(tt);
                emit( s * c, h,  s * c, h, -s * c, h - uh * si, -s * c, h - uh * si);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            if (s_gait_cmd != cmd) continue;

            // Phase 4
            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * PI / 2.0f / p;
                float c = std::cos(tt), si = std::sin(tt);
                emit(-s * si, h, -s * si, h,  s * si, h - uh * c,  s * si, h - uh * c);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            continue;
        }

        // ── Unknown / fallback ───────────────────────────────────
        s_gait_cmd = GaitCmd::None;
    }
}

}  // namespace robot
