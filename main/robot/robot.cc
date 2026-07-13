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

#include "robot/stanford_gait.h"
#include "robot/stanford_kinematics.h"
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

// ── Web joystick state (written by joy_input, read by gait task) ─
// Values -1..1 (already clamped).  s_js_active means the Stanford
// walk was started BY the joystick: when the input goes stale the
// robot then steps in place instead of walking forward.
volatile float    s_js_f = 0.0f, s_js_s = 0.0f, s_js_t = 0.0f;
volatile uint32_t s_js_last_ms = 0;
volatile bool     s_js_active  = false;

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

// ── Neutral-angle calibration (like NEUTRAL_ANGLE_DEGREES in the BSP,
//    identical to ik_neutral_init in the reference minipupperesp) ─────
// On this robot the physical standing pose is ALL SERVOS CENTRED (the
// Init pose).  The plain IK, however, returns big absolute angles
// (~ +52° on the shoulders) for the standing pose, so starting any
// motion used to reposition the legs first — and the boot-up stand was
// wrong.  Fix: command IK angles RELATIVE to the IK angles of the
// neutral stand (x=0, z=NEUTRAL_Z).  Then front_right_ik(0,0,NEUTRAL_Z)
// == centred servos == the power-on pose, and every gait starts right
// from the start-up stance — the same stand phase as the reference repo.
float s_th1_neutral_deg = 0.0f;
float s_th2_neutral_deg = 0.0f;

static void ik_neutral_init()
{
    const float ld  = NEUTRAL_Z;   // x=0 -> phi=0, ld=z
    const float th1 = -std::acos((L1 * L1 + ld * ld - L2 * L2) / (2.0f * L1 * ld));
    const float th2 =  std::asin((ld * ld - L1 * L1 - L2 * L2) / (2.0f * L1 * L2)) - th1;
    s_th1_neutral_deg = th1 * 180.0f / PI;
    s_th2_neutral_deg = th2 * 180.0f / PI;
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
 
// ═══════════════════════════════════════════════════════════════
//  Stanford exact-IK helpers (imported from StanfordQuadruped via
//  the reference minipupperesp firmware).
//
//  Everything below drives the 12 servos through the EXACT Stanford
//  Pupper / Mini Pupper BSP inverse kinematics (stanford_kinematics.cc)
//  instead of the approximate planar per-leg IK.  Foot targets are
//  sg_foot_t: x fwd / y left / z = body height above foot (downward-
//  positive), one per hip, leg order FR, FL, RR, RL.
// ═══════════════════════════════════════════════════════════════

sg_foot_t s_sg_feet[4] = {};   // last written Stanford foot targets
bool      s_sg_valid   = false;

// Buffer all 12 servo goals from four Stanford foot targets (does NOT flush).
static void sg_write(const sg_foot_t feet[4])
{
    float sdeg[13];
    stanford_kinematics_servo_deg(feet, sdeg);
    for (int i = 1; i <= 12; ++i) set_servo_angle(i, sdeg[i] + s_offset[i]);
    for (int l = 0; l < 4; ++l) s_sg_feet[l] = feet[l];
    s_sg_valid = true;
}

// Rest stance: all feet straight under the hips at height h.
static void sg_rest(float h, sg_foot_t feet[4])
{
    for (int l = 0; l < 4; ++l) {
        feet[l].x = 0.0f;
        feet[l].y = 0.0f;
        feet[l].z = h;
    }
}

// ── Body-attitude pose (StanfordQuadruped "head aiming") ─────
// StanfordQuadruped aims the "head" by holding the four feet planted
// while rotating the whole body (roll/pitch/yaw).  This computes each
// foot's position in the rotated body frame and returns the per-hip
// Stanford foot targets (fed to the exact IK by sg_write()).
//
//   roll  > 0  ->  lean right       (ROLL_SIGN)
//   pitch > 0  ->  nose up          (PITCH_SIGN)
//   yaw   > 0  ->  head turns right (YAW_SIGN)
//
// sg_attitude_feet() is the general form: it applies the body rotation
// on top of ARBITRARY per-hip foot targets (`base`), exactly like the
// MangDang FPC pipeline combines explicit foot locations with an
// attitude.  sg_attitude() is the common case with feet at rest.
static void sg_attitude_feet(float roll_deg, float pitch_deg, float yaw_deg,
                             const sg_foot_t base[4], sg_foot_t feet[4])
{
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

    // Hip origins in the body frame (+x fwd, +y left) — true Stanford
    // geometry (LEG_FB, LEG_LR + ABDUCTION_OFFSET), order FR, FL, RR, RL.
    const float ox[4] = { +SG_ORIGIN_X, +SG_ORIGIN_X, -SG_ORIGIN_X, -SG_ORIGIN_X };
    const float oy[4] = { -SG_ORIGIN_Y, +SG_ORIGIN_Y, -SG_ORIGIN_Y, +SG_ORIGIN_Y };

    for (int l = 0; l < 4; ++l) {
        // Foot offset from body centre = hip origin + per-hip target.
        const float px = ox[l] + base[l].x;
        const float py = oy[l] + base[l].y;
        const float pz = -base[l].z;

        // Foot position in the rotated body frame = R^T * offset.
        const float fx = R00 * px + R10 * py + R20 * pz;
        const float fy = R01 * px + R11 * py + R21 * pz;
        const float fz = R02 * px + R12 * py + R22 * pz;

        feet[l].x = fx - ox[l];       // per-hip forward offset
        feet[l].y = fy - oy[l];       // per-hip lateral offset (+ left)
        feet[l].z = -fz;              // downward-positive body height
    }
}

// Attitude with the feet at the rest stance (the common case).
static void sg_attitude(float roll_deg, float pitch_deg, float yaw_deg,
                        float h, sg_foot_t feet[4])
{
    sg_foot_t base[4];
    sg_rest(h, base);
    sg_attitude_feet(roll_deg, pitch_deg, yaw_deg, base, feet);
}

// Pose-transition duration: scales with the web "Period" slider so the
// MOVEMENT ITSELF slows down together with the tempo (not just the time
// between moves).  Period 80 -> 250 ms ramps (snappy), 160 -> 480 ms,
// 300 -> 900 ms (slow and deliberate).
static int sg_ramp_ms()
{
    int ms = s_cfg.period * 3;
    if (ms < 250)  ms = 250;
    if (ms > 1500) ms = 1500;
    return ms;
}

// Smoothly move from the last written Stanford pose to `to` (sin-eased).
// Used both to ENTER a pose without snapping and to RETURN to the stand
// after a move ends — the StanfordQuadruped-style "goes back" behaviour.
static void sg_ramp_to(const sg_foot_t to[4], int ms)
{
    if (ms < 1) ms = 1;
    sg_foot_t from[4];
    if (s_sg_valid) {
        for (int l = 0; l < 4; ++l) from[l] = s_sg_feet[l];
    } else {
        sg_rest(static_cast<float>(s_cfg.height), from);
    }

    const uint32_t t0 = millis();
    for (;;) {
        uint32_t el = millis() - t0;
        if (el > static_cast<uint32_t>(ms)) el = ms;
        const float f = std::sin(static_cast<float>(el) * PI / 2.0f
                                 / static_cast<float>(ms));
        sg_foot_t cur[4];
        for (int l = 0; l < 4; ++l) {
            cur[l].x = from[l].x + (to[l].x - from[l].x) * f;
            cur[l].y = from[l].y + (to[l].y - from[l].y) * f;
            cur[l].z = from[l].z + (to[l].z - from[l].z) * f;
        }
        sg_write(cur);
        flush();                       // self-paced (≥5 ms per bus write)
        if (el >= static_cast<uint32_t>(ms)) break;
    }
}

// Park back in the neutral stand (Stanford "return to rest").
static void sg_park(float h)
{
    sg_foot_t rest[4];
    sg_rest(h, rest);
    sg_ramp_to(rest, (sg_ramp_ms() * 2) / 3);
    s_sg_valid = false;
}

}  // anonymous namespace

// Forward declaration of gait task (defined at end of file)
void gait_task();

// ═══════════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════════

bool init()
{
    // ── Neutral-angle calibration (Ini pose == IK neutral stand) ─
    ik_neutral_init();

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
        ESP_LOGW(TAG, "Check: power, baud rate (configured %d), wiring (TX→RX cross?)",
                 SERVO_BAUD_RATE);
    }

    // ── Restore persisted config ────────────────────────────────
    int32_t v;
    if (s_nvs) {
        if (nvs_get_i32(s_nvs, "period", &v) == ESP_OK)   s_cfg.period    = static_cast<int>(v);
        if (nvs_get_i32(s_nvs, "height", &v) == ESP_OK)   s_cfg.height    = static_cast<int>(v);
        if (nvs_get_i32(s_nvs, "upHeight", &v) == ESP_OK) s_cfg.up_height = static_cast<int>(v);
        if (nvs_get_i32(s_nvs, "stride", &v) == ESP_OK)   s_cfg.stride    = static_cast<int>(v);
        if (nvs_get_i32(s_nvs, "tilt", &v) == ESP_OK)     s_cfg.tilt      = static_cast<int>(v);
        if (nvs_get_i32(s_nvs, "sgspeed", &v) == ESP_OK)  s_cfg.sg_speed  = static_cast<int>(v);

        for (int i = 1; i <= 12; ++i) {
            char key[16];
            std::snprintf(key, sizeof(key), "offset%d", i);
            s_offset[i] = nvs_get_float(key, 0.0f);
        }
    }

    ESP_LOGI(TAG, "Config restored: period=%d height=%d upHeight=%d stride=%d tilt=%d sgspeed=%d",
             s_cfg.period, s_cfg.height, s_cfg.up_height, s_cfg.stride, s_cfg.tilt,
             s_cfg.sg_speed);

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

// ── Web joystick ─────────────────────────────────────────────
// Same behaviour as the reference minipupperesp /js handler:
// touching a pad auto-starts the Stanford trot; the gait task
// consumes the values each 15 ms tick, scaled by s_cfg.sg_speed.
void joy_input(float f, float s, float t)
{
    if (f >  1.0f) f =  1.0f;
    if (f < -1.0f) f = -1.0f;
    if (s >  1.0f) s =  1.0f;
    if (s < -1.0f) s = -1.0f;
    if (t >  1.0f) t =  1.0f;
    if (t < -1.0f) t = -1.0f;

    const bool moving = (std::fabs(f) > 0.08f ||
                         std::fabs(s) > 0.08f ||
                         std::fabs(t) > 0.08f);

    if (moving && s_gait_cmd != GaitCmd::StanfordWalk) {
        s_js_active = true;             // joystick-started walk
        s_gait_cmd  = GaitCmd::StanfordWalk;
    }
    if (moving) s_js_active = true;

    s_js_f = f;
    s_js_s = s;
    s_js_t = t;
    s_js_last_ms = millis();
}

// ── Configuration ────────────────────────────────────────────

Config get_config()
{
    return s_cfg;
}

void set_config(const Config &cfg)
{
    s_cfg = cfg;
    if (s_cfg.sg_speed < 10)  s_cfg.sg_speed = 10;
    if (s_cfg.sg_speed > static_cast<int>(SG_SPEED_MAX_MM_S))
        s_cfg.sg_speed = static_cast<int>(SG_SPEED_MAX_MM_S);
    if (s_nvs) {
        nvs_put_i32("period",   cfg.period);
        nvs_put_i32("height",   cfg.height);
        nvs_put_i32("upHeight", cfg.up_height);
        nvs_put_i32("stride",   cfg.stride);
        nvs_put_i32("tilt",     cfg.tilt);
        nvs_put_i32("sgspeed",  s_cfg.sg_speed);
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
    // Convert degrees to raw pulse for this 0–180° servo:
    // 0° → 0, 180° → 1023, centre (90°) → 511.
    // `deg` is RELATIVE to centre, so 1° = 1023/180 ≈ 5.683 raw steps.
    // (The reference minipupperesp robot uses 0–270° servos and the
    //  scale deg/0.263 there; this is the same formula for 180°.)
    int sig = 511 + static_cast<int>(deg * SERVO_DEG_TO_RAW);
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

// All shoulder/knee angles are commanded RELATIVE to the neutral stand
// (s_th1_neutral_deg / s_th2_neutral_deg), exactly like the reference
// minipupperesp fRIK/fLIK/rRIK/rLIK: front_right_ik(0,0,NEUTRAL_Z) ==
// centred servos == the calibrated stand pose.

void front_right_ik(float x, float th0, float z)
{
    float th1, th2;
    calculate_ik(x, th0, z, th1, th2);
    set_servo_angle(1,  th0                          + s_offset[1]);
    set_servo_angle(2, -(th1 - s_th1_neutral_deg)    + s_offset[2]);
    set_servo_angle(3,  (th2 - s_th2_neutral_deg)    + s_offset[3]);
}

void front_left_ik(float x, float th0, float z)
{
    float th1, th2;
    calculate_ik(x, th0, z, th1, th2);
    set_servo_angle(4,  th0                          + s_offset[4]);
    set_servo_angle(5,  (th1 - s_th1_neutral_deg)    + s_offset[5]);
    set_servo_angle(6, -(th2 - s_th2_neutral_deg)    + s_offset[6]);
}

void rear_right_ik(float x, float th0, float z)
{
    float th1, th2;
    calculate_ik(x, th0, z, th1, th2);
    set_servo_angle(7,  th0                          + s_offset[7]);
    set_servo_angle(8, -(th1 - s_th1_neutral_deg)    + s_offset[8]);
    set_servo_angle(9,  (th2 - s_th2_neutral_deg)    + s_offset[9]);
}

void rear_left_ik(float x, float th0, float z)
{
    float th1, th2;
    calculate_ik(x, th0, z, th1, th2);
    set_servo_angle(10,  th0                         + s_offset[10]);
    set_servo_angle(11,  (th1 - s_th1_neutral_deg)   + s_offset[11]);
    set_servo_angle(12, -(th2 - s_th2_neutral_deg)   + s_offset[12]);
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
                case GaitCmd::StanfordWalk:   name = "stanford";      break;
                case GaitCmd::FrontKick:      name = "frontkick";     break;
                case GaitCmd::Wiggle:         name = "wiggle";        break;
                case GaitCmd::ButtShrug:      name = "buttshrug";     break;
                case GaitCmd::WiggleLeft:     name = "wiggleL";       break;
                case GaitCmd::WiggleRight:    name = "wiggleR";       break;
                case GaitCmd::ButtShrugLeft:  name = "buttshrugL";    break;
                case GaitCmd::ButtShrugRight: name = "buttshrugR";    break;
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

            // Exact StanfordQuadruped attitude via the Stanford leg IK:
            // ease into the pose, hold it, then ease back to the stand.
            const float h = static_cast<float>(s_cfg.height);
            set_all_servo_speed(0);

            sg_foot_t pose[4];
            sg_attitude(0.0f, pitch, yaw, h, pose);
            sg_ramp_to(pose, sg_ramp_ms());                  // smooth head move in

            while (s_gait_cmd == cmd) {             // hold the pose
                sg_write(pose);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            sg_park(h);                             // Stanford-style return
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

            // Weight-shift attitude + raised paw, all through the exact
            // Stanford IK: ease in, hold, ease back to the stand.
            set_all_servo_speed(0);

            sg_foot_t pose[4];
            sg_attitude(roll, pitch, 0.0f, h, pose);
            pose[leg].z -= lift;                    // raise the chosen paw

            sg_ramp_to(pose, sg_ramp_ms());
            while (s_gait_cmd == cmd) {
                sg_write(pose);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            sg_park(h);
            continue;
        }

        // ── Static body height up / down (Stanford IK) ───────────
        if (cmd == GaitCmd::HeightUp || cmd == GaitCmd::HeightDown) {
            const float h = static_cast<float>(s_cfg.height);
            const float d = 20.0f;
            const float z = (cmd == GaitCmd::HeightUp) ? (h + d) : (h - d);

            set_all_servo_speed(0);
            sg_foot_t pose[4];
            sg_rest(z, pose);
            sg_ramp_to(pose, sg_ramp_ms());
            while (s_gait_cmd == cmd) {
                sg_write(pose);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            sg_park(h);
            continue;
        }

        // ── Balance: hold a roll + pitch attitude (Stanford IK) ──
        if (cmd == GaitCmd::Balance) {
            const float h = static_cast<float>(s_cfg.height);
            const float t = static_cast<float>(s_cfg.tilt);

            set_all_servo_speed(0);
            sg_foot_t pose[4];
            sg_attitude(t, t, 0.0f, h, pose);
            sg_ramp_to(pose, sg_ramp_ms());
            while (s_gait_cmd == cmd) {
                sg_write(pose);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            sg_park(h);
            continue;
        }

        // ── Bow + shuffle back, then rise (auto-return, Stanford IK) ─
        if (cmd == GaitCmd::BowBack) {
            const float h         = static_cast<float>(s_cfg.height);
            const float p         = static_cast<float>(s_cfg.period);
            const float bow       = 25.0f;   // front crouch (mm)
            const float backshift = 15.0f;   // body shifts back (mm)

            set_all_servo_speed(0);

            // Bow pose: feet forward relative to the body (body shifts
            // back), front lowered.
            sg_foot_t pose[4];
            sg_rest(h, pose);
            for (int l = 0; l < 4; ++l) pose[l].x = backshift;
            pose[0].z = h - bow;   // FR
            pose[1].z = h - bow;   // FL

            // Lower the front and shift weight backward (sin-eased)
            sg_ramp_to(pose, static_cast<int>(p * 4.0f));
            if (s_gait_cmd != cmd) { sg_park(h); continue; }

            // Hold the bow briefly
            time_mSt = millis(); tim = 0;
            while (tim < 600.0f) { tim = millis() - time_mSt;
                sg_write(pose);
                flush();
                if (s_gait_cmd != cmd) break;
                vTaskDelay(pdMS_TO_TICKS(20));
            }

            // Rise back to neutral — the Stanford-style "goes back"
            sg_foot_t rest[4];
            sg_rest(h, rest);
            sg_ramp_to(rest, static_cast<int>(p * 4.0f));
            s_sg_valid = false;
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Body cycle: body centre draws a circle (auto-return,
        //    exact Stanford IK — lateral moves use the true abduction
        //    solve instead of the old hip-angle approximation) ──────
        if (cmd == GaitCmd::BodyCycle) {
            const float h   = static_cast<float>(s_cfg.height);
            const float p   = static_cast<float>(s_cfg.period) * 8.0f;
            const float Rad = 15.0f;   // circle radius (mm)
            set_all_servo_speed(0);

            // Ease to the circle start (tt = 0: feet at x = -Rad).
            sg_foot_t pose[4];
            sg_rest(h, pose);
            for (int l = 0; l < 4; ++l) pose[l].x = -Rad;
            sg_ramp_to(pose, sg_ramp_ms());

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                // Feet move opposite to the body centre (+y = left).
                for (int l = 0; l < 4; ++l) {
                    pose[l].x = -Rad * std::cos(tt);
                    pose[l].y = -Rad * std::sin(tt);
                    pose[l].z = h;
                }
                sg_write(pose);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            sg_park(h);                 // Stanford-style return to stand
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Head ellipse: head sweeps an ellipse (auto-return) ───
        // Exact StanfordQuadruped behaviour: ease into the ellipse,
        // sweep it, then GO BACK smoothly to the neutral stand.
        if (cmd == GaitCmd::HeadEllipse) {
            const float h = static_cast<float>(s_cfg.height);
            const float p = static_cast<float>(s_cfg.period) * 8.0f;
            set_all_servo_speed(0);

            // Ease to the ellipse start (tt = 0: pitch 0, yaw = max).
            sg_foot_t pose[4];
            sg_attitude(0.0f, 0.0f, LOOK_YAW_DEG, h, pose);
            sg_ramp_to(pose, sg_ramp_ms());

            time_mSt = millis(); tim = 0;
            while (tim < p) { tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                float pitch = LOOK_PITCH_DEG * std::sin(tt);
                float yaw   = LOOK_YAW_DEG   * std::cos(tt);
                sg_attitude(0.0f, pitch, yaw, h, pose);
                sg_write(pose);
                flush();
                if (s_gait_cmd != cmd) break;
            }
            sg_park(h);                 // after the ellipse it goes back
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Diagonal trot walks (Stanford gait, vx + vy) ─────────
        // The Stanford trot natively supports lateral velocity, so the
        // diagonals are simply the walk with a combined vx/vy command —
        // exactly what StanfordQuadruped does with a diagonal stick.
        if (cmd == GaitCmd::MoveLeftFront  || cmd == GaitCmd::MoveRightFront ||
            cmd == GaitCmd::MoveLeftBack   || cmd == GaitCmd::MoveRightBack) {

            const float h  = static_cast<float>(s_cfg.height);

            // xf: +1 forward, -1 backward.  yf: +1 left, -1 right.
            const float xf = (cmd == GaitCmd::MoveLeftFront || cmd == GaitCmd::MoveRightFront) ? 1.0f : -1.0f;
            const float yf = (cmd == GaitCmd::MoveLeftFront || cmd == GaitCmd::MoveLeftBack)   ? 1.0f : -1.0f;

            // Diagonal speed comes from the SAME adjustable walk speed
            // (web "Walk speed" slider, Config::sg_speed, mm/s).
            const float v  = static_cast<float>(s_cfg.sg_speed) * 0.7071f;
            const float vx = xf * v;
            float       vy = yf * v;
            if (SG_MIRROR_LR) vy = -vy;    // reference L/R mirror (see robot.h)

            set_all_servo_speed(0);
            stanford_gait_reset(h);

            int64_t next_us = esp_timer_get_time();
            while (s_gait_cmd == cmd) {
                sg_foot_t feet[4];
                stanford_gait_step(vx, vy, 0.0f, h, SG_NATIVE_CLEARANCE_MM, feet);
                sg_write(feet);
                flush();

                // Pace to the next 15 ms gait tick; resync if far behind.
                next_us += static_cast<int64_t>(SG_DT * 1e6f);
                int64_t now = esp_timer_get_time();
                if (now > next_us + 100000) next_us = now;
                while (esp_timer_get_time() < next_us && s_gait_cmd == cmd) {
                    vTaskDelay(1);
                }
            }
            sg_park(h);    // park mid-stride feet back in the stand
            continue;
        }

        // ── Stanford Pupper trot walk (exact StanfordQuadruped port) ─
        // Phase scheduler + stance/swing controllers from Gaits.py /
        // StanceController.py / SwingLegController.py, feet solved by the
        // exact mini_pupper BSP IK.  Runs at the native 15 ms tick with
        // the native clearance.
        //
        // Speed comes from the web "Walk speed" slider (Config::sg_speed).
        // Velocity command each tick (same scheme as the reference /js):
        //   - fresh joystick input  -> full 3-axis command from the pads
        //   - joystick released     -> step in place (zeros stay fresh /
        //                              joystick-started walk went stale)
        //   - started by the button -> steady forward walk at sg_speed
        if (cmd == GaitCmd::StanfordWalk) {
            const float h = static_cast<float>(s_cfg.height);

            set_all_servo_speed(0);

            // From the stand this is already the walk stance (the gait
            // rests at feet (0,0,h)), so the walk starts directly from
            // the pose the robot is in — same as the reference repo.
            stanford_gait_reset(h);

            int64_t next_us = esp_timer_get_time();
            while (s_gait_cmd == cmd) {
                const float sg = static_cast<float>(s_cfg.sg_speed);
                float vx, vy, wz;
                if (millis() - s_js_last_ms < JOY_TIMEOUT_MS) {
                    // Fresh joystick: full stick == sg_speed; strafe and
                    // yaw scaled proportionally so turning matches.
                    const float k = sg / SG_SPEED_MAX_MM_S;
                    vx = s_js_f * sg;
                    // Invert lateral (strafe) joystick for Stanford walk —
                    // user reported left/right reversed when using the
                    // Stanford gait via the joystick pads.
                    vy = -s_js_s * JOY_VY_MAX * k;
                    wz = s_js_t * JOY_WZ_MAX * k;
                } else if (s_js_active) {
                    // Joystick-started walk, input gone stale: step in
                    // place instead of surprising the user by walking.
                    vx = vy = wz = 0.0f;
                } else {
                    // Button-started walk: steady forward trot.
                    vx = sg; vy = 0.0f; wz = 0.0f;
                }
                if (SG_MIRROR_LR) { vy = -vy; wz = -wz; }   // see robot.h

                sg_foot_t feet[4];
                stanford_gait_step(vx, vy, wz, h, SG_NATIVE_CLEARANCE_MM, feet);
                sg_write(feet);
                flush();

                // Pace to the next 15 ms gait tick; resync if far behind.
                next_us += static_cast<int64_t>(SG_DT * 1e6f);
                int64_t now = esp_timer_get_time();
                if (now > next_us + 100000) next_us = now;
                while (esp_timer_get_time() < next_us && s_gait_cmd == cmd) {
                    vTaskDelay(1);
                }
            }
            s_js_active = false;
            sg_park(h);    // park mid-stride feet back in the stand
            continue;
        }

        // ═══════════════════════════════════════════════════════════
        //  FPC choreography (MangDang MovementGroups.py), driven by the
        //  exact Stanford IK.  FPC coordinates are body-frame metres;
        //  converted here to per-hip mm (body = hip origin + local).
        // ═══════════════════════════════════════════════════════════

        // ── Front kick: rear up like a horse (auto-return) ───────
        // FPC front_kick(ht=0.04, pitch=15): front feet reach forward
        // ([0.12, ±0.06] body frame) and lift 40 mm, rear legs squat
        // 20 mm, body pitches 15° nose-up.  Hold ~1 s, then return to
        // the stand (front_kick_to_stand).
        if (cmd == GaitCmd::FrontKick) {
            const float h = static_cast<float>(s_cfg.height);
            set_all_servo_speed(0);

            sg_foot_t base[4];
            sg_rest(h, base);
            base[0].x = 61.0f;  base[0].y = -10.5f; base[0].z = h - 40.0f; // FR paw fwd+up
            base[1].x = 61.0f;  base[1].y = +10.5f; base[1].z = h - 40.0f; // FL paw fwd+up
            base[2].x = -1.0f;  base[2].y = -0.5f;  base[2].z = h + 20.0f; // RR squat
            base[3].x = -1.0f;  base[3].y = +0.5f;  base[3].z = h + 20.0f; // RL squat

            sg_foot_t pose[4];
            sg_attitude_feet(0.0f, 15.0f, 0.0f, base, pose);   // nose up 15°
            sg_ramp_to(pose, sg_ramp_ms());                             // rear up

            // Hold the kick pose ~1 s (interruptible)
            time_mSt = millis(); tim = 0;
            while (tim < 1000.0f) { tim = millis() - time_mSt;
                sg_write(pose);
                flush();
                if (s_gait_cmd != cmd) break;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            sg_park(h);                    // front_kick_to_stand
            s_gait_cmd = GaitCmd::None;
            continue;
        }

        // ── Wiggle / butt shrug: pitched body + yaw tail-wag ─────
        // FPC wiggle_trajectory: butt up (pitch -25) with the yaw
        // sweeping side to side; butt_shrug_trajectory: nose up
        // (pitch +25) with the same sweep.  Angles softened slightly
        // (-22/+20, yaw ±15) to stay well inside this servo's range.
        // Runs continuously while held, then returns to the stand.
        if (cmd == GaitCmd::Wiggle || cmd == GaitCmd::ButtShrug) {
            const float h     = static_cast<float>(s_cfg.height);
            const float pitch = (cmd == GaitCmd::Wiggle) ? -22.0f : 20.0f;
            const float yawA  = 15.0f;                          // wag amplitude
            const float p     = static_cast<float>(s_cfg.period) * 8.0f;
            set_all_servo_speed(0);

            sg_foot_t pose[4];
            sg_attitude(0.0f, pitch, 0.0f, h, pose);
            sg_ramp_to(pose, sg_ramp_ms());          // ease into the pitched pose

            time_mSt = millis();
            while (s_gait_cmd == cmd) {     // wag until released
                tim = millis() - time_mSt; tt = tim * 2.0f * PI / p;
                const float yaw = yawA * std::sin(tt);
                sg_attitude(0.0f, pitch, yaw, h, pose);
                sg_write(pose);
                flush();
            }
            sg_park(h);
            continue;
        }

        // ── One-sided wiggle / butt shrug (FPC wiggle_left/right,
        //    butt_shrug_left/right): pitched body with the yaw swept
        //    to ONE side and held there.  The FPC versions step the
        //    yaw 0→±20 through intermediate poses; the entry ramp here
        //    sweeps through the same path.  Hold while active, then
        //    return to the stand.
        if (cmd == GaitCmd::WiggleLeft    || cmd == GaitCmd::WiggleRight ||
            cmd == GaitCmd::ButtShrugLeft || cmd == GaitCmd::ButtShrugRight) {

            const float h = static_cast<float>(s_cfg.height);
            const bool wig  = (cmd == GaitCmd::WiggleLeft ||
                               cmd == GaitCmd::WiggleRight);
            const bool left = (cmd == GaitCmd::WiggleLeft ||
                               cmd == GaitCmd::ButtShrugLeft);
            // FPC: wiggle pitch -25, shrug +25, yaw to ±20; softened to
            // the verified-safe -22/+20 and ±15 for this 0–180° servo.
            const float pitch = wig  ? -22.0f : 20.0f;
            const float yaw   = left ? +15.0f : -15.0f;   // FPC sign convention

            set_all_servo_speed(0);
            sg_foot_t pose[4];
            sg_attitude(0.0f, pitch, yaw, h, pose);
            sg_ramp_to(pose, sg_ramp_ms());          // sweep pitch+yaw in like FPC
            while (s_gait_cmd == cmd) {     // hold the side pose
                sg_write(pose);
                flush();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            sg_park(h);
            continue;
        }

        // ── Unknown / fallback ───────────────────────────────────
        s_gait_cmd = GaitCmd::None;
    }
}

}  // namespace robot
