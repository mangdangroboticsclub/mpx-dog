#pragma once

#include "sdkconfig.h"
#include <cstdint>

namespace robot {

// ── Forward declarations ─────────────────────────────────────
struct ImuData;

}  // namespace robot

#include "robot/imu.h"

namespace robot {

// ── Robot constants ──────────────────────────────────────────
constexpr float L1 = 50.0f;   // Upper leg length (mm)
constexpr float L2 = 56.0f;   // Lower leg length (mm)
constexpr float PI = 3.14159265358979f;

// ── Body geometry for attitude (look) poses ──────────────────
// Half the distance between hip axes, in mm.  Used to convert a
// body roll/pitch/yaw (Stanford-style "head aiming") into per-leg
// foot offsets.  Tune to your chassis if the look angles feel off.
constexpr float BODY_LX = 50.0f;   // half fore-aft hip spacing (mm)
constexpr float BODY_LY = 35.0f;   // half left-right hip spacing (mm)

// Direction sign flips for the look poses.  If the robot looks the
// OPPOSITE way to what a command says, flip that axis sign.
// (Verified in simulation: PITCH_SIGN = -1 makes LookUp raise the nose.)
constexpr float PITCH_SIGN = -1.0f; // LookUp raises the nose
constexpr float YAW_SIGN   =  1.0f; // LookRight aims head right (flip if reversed)
constexpr float ROLL_SIGN  =  1.0f; // positive roll leans right (flip if reversed)

// Default look amplitudes (degrees), matching StanfordQuadruped.
constexpr float LOOK_PITCH_DEG = 20.0f;
constexpr float LOOK_YAW_DEG   = 30.0f;

// ── Gait command enum ────────────────────────────────────────
enum class GaitCmd : uint8_t {
    None,
    Init,        // Initial pose (zero offsets)
    Step,        // In-place stepping (trot)
    Roll,        // Body roll
    Pitch,       // Body pitch
    Stretch,     // Body stretch (up/down)
    Advance,     // Walk forward
    Back,        // Walk backward
    Left,        // Strafe left
    Right,       // Strafe right
    TurnL,       // Turn left (spin)
    TurnR,       // Turn right (spin)
    Twerk,       // Twerk / wiggle
    Jump,        // Vertical jump
    JumpFwd,     // Jump forward
    TestSpeed,   // Servo speed test

    // ── Imported from StanfordQuadruped (body-attitude poses) ────
    // These hold a static "head aiming" pose by rotating the body
    // while the feet stay planted (Stanford's roll/pitch/yaw model
    // converted to this robot's per-leg IK).  All static-hold.
    LookUp,         // Pitch nose up
    LookDown,       // Pitch nose down
    LookLeft,       // Yaw head left
    LookRight,      // Yaw head right
    LookUpperLeft,  // Up + left
    LookUpperRight, // Up + right
    LookLowerLeft,  // Down + left
    LookLowerRight, // Down + right

    // ── Single-leg lifts (raise one paw while standing) ──────────
    ForelegLiftL,   // Lift front-left paw
    ForelegLiftR,   // Lift front-right paw
    BacklegLiftL,   // Lift rear-left paw
    BacklegLiftR,   // Lift rear-right paw

    // ── Static body height ───────────────────────────────────────
    HeightUp,       // Raise body
    HeightDown,     // Lower body

    // ── Static combined roll+pitch hold ──────────────────────────
    Balance,        // Hold a roll+pitch attitude

    // ── Level-3 choreography (dynamic, auto-return) ──────────────
    BowBack,        // Bow head down and shuffle backward
    BodyCycle,      // Body centre draws a circle (orientation fixed)
    HeadEllipse,    // Head draws an ellipse via body attitude

    // ── Diagonal trot walks ──────────────────────────────────────
    MoveLeftFront,  // Forward + strafe left
    MoveRightFront, // Forward + strafe right
    MoveLeftBack,   // Backward + strafe left
    MoveRightBack,  // Backward + strafe right
};

// ── Robot configuration ──────────────────────────────────────
struct Config {
    int period   = 80;   // Gait period (ms per phase)
    int height   = 70;   // Body height (mm)
    int up_height = 10;  // Foot lift height (mm)
    int stride   = 10;   // Stride length (mm)
    int tilt     = 10;   // Body tilt angle (degrees)
};

// ── Servo pin / UART configuration ───────────────────────────
constexpr int SERVO_TX_PIN    = 4;
constexpr int SERVO_RX_PIN    = 5;
constexpr int SERVO_POWER_PIN = 8;
constexpr int SERVO_BAUD_RATE = CONFIG_APP_SERVO_BAUD_RATE;

// ── Public API ───────────────────────────────────────────────

/**
 * @brief Initialise robot HAL.
 *
 * - Opens NVS handle
 * - Enables servo power (GPIO8 high)
 * - Initialises SCSCL bus on UART1 (TX=4, RX=5, 500 kbps)
 * - Restores persisted offsets and config from NVS
 * - Spawns the gait task on core 1
 *
 * @return true on success.
 */
bool init();

/**
 * @brief Send a gait command (thread-safe).
 *
 * Overwrites any previous command.  The gait task picks it up
 * on its next iteration.
 */
void send_gait_cmd(GaitCmd cmd);

/**
 * @brief Return the currently active gait command.
 */
GaitCmd current_gait_cmd();

/**
 * @brief Get the current robot configuration.
 */
Config get_config();

/**
 * @brief Overwrite the robot configuration and persist to NVS.
 */
void set_config(const Config &cfg);

// ── IMU API ─────────────────────────────────────────────────

/**
 * @brief Initialise the QMI8658C IMU over SPI (SPI2_HOST).
 *
 * Must be called once during boot, after NVS but before the IMU
 * background task is needed.  Spawns a low-priority polling task
 * on core 0 that reads 6-DOF data at ~20 Hz.
 *
 * @return true on success.
 */
bool imu_init();

/**
 * @brief Return a thread-safe copy of the latest IMU sample.
 */
ImuData imu_read();

/**
 * @brief Print the latest IMU data to the log.
 */
void imu_print();

/**
 * @brief Get the angular offset for a single servo (1‑12).
 */
float get_offset(int servo_id);

/**
 * @brief Set the angular offset for a single servo (1‑12) and
 *        persist it to NVS.
 */
void set_offset(int servo_id, float deg);

/**
 * @brief Reset all servo offsets to 0 and persist.
 */
void reset_offsets();

/**
 * @brief Read the current position of a servo (raw 0‑1023).
 *        Returns -1 on failure.
 */
int read_position(int servo_id);

/**
 * @brief Read the current speed of a servo (signed).
 *        Returns -1 on failure.
 */
int read_speed(int servo_id);

/**
 * @brief Read the current load on a servo (signed, unit‑dependent).
 *        Returns -1 on failure.
 */
int read_load(int servo_id);

/**
 * @brief Read the servo supply voltage (0.1 V increments).
 *        Returns -1 on failure.
 *
 * @note Does NOT work on the current Mini Pupper 2 hardware
 *       (SCSCL servos don't report voltage via this register).
 */
int read_voltage(int servo_id);

/**
 * @brief Read the servo internal temperature (°C).
 *        Returns -1 on failure.
 *
 * @note Does NOT work on the current Mini Pupper 2 hardware
 *       (SCSCL servos don't report temperature via this register).
 */
int read_temperature(int servo_id);

/**
 * @brief Read the servo moving state (0 = stopped, 1 = moving).
 *        Returns -1 on failure.
 */
int read_moving(int servo_id);

/**
 * @brief Read the servo current draw (mA, signed).
 *        Returns -1 on failure.
 *
 * @note Does NOT work on the current Mini Pupper 2 hardware
 *       (SCSCL servos don't report current via this register).
 */
int read_current(int servo_id);

/**
 * @brief Ping a servo to verify communication.
 *        Returns the model number on success, or <= 0 on failure.
 */
int ping_servo(int servo_id);

// ── Low-level IK (exposed for WASM host functions) ───────────

/**
 * @brief Set one servo angle (degrees), clamping to [‑270, 270].
 */
void set_servo_angle(int servo_id, float deg);

/**
 * @brief Set movement speed for one servo (0 = max, larger = slower).
 */
void set_servo_speed(int servo_id, uint16_t speed);

/**
 * @brief Set speed for all 12 servos at once.
 */
void set_all_servo_speed(uint16_t speed);

/**
 * @brief Flush all buffered servo commands to the bus
 *        (SyncWritePos on IDs 1‑12).
 */
void flush();

// ── Per‑leg IK helpers ───────────────────────────────────────

/**
 * Each leg IK takes three parameters:
 *   x    – forward/backward displacement (mm), positive = forward
 *   th0  – hip rotation (degrees), positive = outward
 *   z    – foot height (mm), measured from hip
 *
 * Leg‑to‑servo mapping
 *   Front Right (FR): servo 1 (hip),  2 (shoulder), 3 (knee)
 *   Front Left  (FL): servo 4 (hip),  5 (shoulder), 6 (knee)
 *   Rear Right  (RR): servo 7 (hip),  8 (shoulder), 9 (knee)
 *   Rear Left   (RL): servo 10 (hip), 11 (shoulder),12 (knee)
 */
void front_right_ik(float x, float th0, float z);
void front_left_ik(float x, float th0, float z);
void rear_right_ik(float x, float th0, float z);
void rear_left_ik(float x, float th0, float z);

}  // namespace robot
