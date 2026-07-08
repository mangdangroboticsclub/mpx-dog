#pragma once

#include <cstddef>
#include <cstdint>

#include "wasm_export.h"

namespace sdk {

// ═══════════════════════════════════════════════════════════════
//  print — SDK host function
// ═══════════════════════════════════════════════════════════════

/**
 * @brief Wasm-side: extern void print(const char *text, int len);
 *
 * Signature "($i)": auto-converted string pointer + length.
 */
int32_t host_print(wasm_exec_env_t exec_env,
				   int32_t text_offset, int32_t len);

// ═══════════════════════════════════════════════════════════════
//  Robot host functions
// ═══════════════════════════════════════════════════════════════

/**
 * @brief Wasm-side: extern void robot_gait(const char *name, int len);
 *
 * Signature "($)": auto-converted string pointer (name_len deduced by WAMR).
 * Sends a gait command by name (e.g. "advance", "jump", "none").
 */
int32_t host_robot_gait(wasm_exec_env_t exec_env,
						int32_t name_ptr);

/**
 * @brief Wasm-side: extern int robot_get_mode(void);
 * Signature "()i". Returns current gait mode as int (see GaitCmd enum).
 */
int32_t host_robot_get_mode(wasm_exec_env_t exec_env);

/**
 * @brief Wasm-side: extern void robot_set_config(int period, int height,
 *                       int up_height, int stride, int tilt);
 * Signature "(iiiii)".
 */
int32_t host_robot_set_config(wasm_exec_env_t exec_env,
							  int32_t period, int32_t height,
							  int32_t up_height, int32_t stride,
							  int32_t tilt);

// ── Individual config getters ────────────────────────────────
int32_t host_robot_get_period(wasm_exec_env_t exec_env);
int32_t host_robot_get_height(wasm_exec_env_t exec_env);
int32_t host_robot_get_up_height(wasm_exec_env_t exec_env);
int32_t host_robot_get_stride(wasm_exec_env_t exec_env);
int32_t host_robot_get_tilt(wasm_exec_env_t exec_env);

// ── Low-level servo control ──────────────────────────────────

/**
 * @brief Wasm-side: extern void robot_set_servo_angle(int id, int centideg);
 * Signature "(ii)". Angle in centidegrees (e.g. 4500 = 45.00°).
 */
int32_t host_robot_set_servo_angle(wasm_exec_env_t exec_env,
								   int32_t id, int32_t centideg);

/**
 * @brief Wasm-side: extern void robot_flush(void);
 * Signature "()". Sends all buffered servo commands.
 */
int32_t host_robot_flush(wasm_exec_env_t exec_env);

/**
 * @brief Wasm-side: extern void robot_set_servo_speed(int id, int speed);
 * Signature "(ii)". 0 = max speed, higher = slower.
 */
int32_t host_robot_set_servo_speed(wasm_exec_env_t exec_env,
								   int32_t id, int32_t speed);

/**
 * @brief Wasm-side: extern int robot_read_position(int id);
 * Signature "(i)i". Returns raw position (0-1023) or -1 on error.
 */
int32_t host_robot_read_position(wasm_exec_env_t exec_env,
								 int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_speed(int id);
 * Signature "(i)i". Returns signed speed or -1 on error.
 */
int32_t host_robot_read_speed(wasm_exec_env_t exec_env,
							  int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_load(int id);
 * Signature "(i)i". Returns signed load or -1 on error.
 */
int32_t host_robot_read_load(wasm_exec_env_t exec_env,
							 int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_voltage(int id);
 * Signature "(i)i". Returns voltage (0.1V) or -1 on error.
 */
int32_t host_robot_read_voltage(wasm_exec_env_t exec_env,
								int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_temperature(int id);
 * Signature "(i)i". Returns temperature (°C) or -1 on error.
 */
int32_t host_robot_read_temperature(wasm_exec_env_t exec_env,
									int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_moving(int id);
 * Signature "(i)i". Returns 0=stopped, 1=moving or -1 on error.
 */
int32_t host_robot_read_moving(wasm_exec_env_t exec_env,
							   int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_current(int id);
 * Signature "(i)i". Returns signed current (mA) or -1 on error.
 */
int32_t host_robot_read_current(wasm_exec_env_t exec_env,
								int32_t id);

// ── Calibration ──────────────────────────────────────────────

/**
 * @brief Wasm-side: extern void robot_set_offset(int id, int centideg);
 * Signature "(ii)". Offset in centidegrees (e.g. 150 = 1.50°).
 */
int32_t host_robot_set_offset(wasm_exec_env_t exec_env,
							  int32_t id, int32_t centideg);

/**
 * @brief Wasm-side: extern int robot_get_offset(int id);
 * Signature "(i)i". Returns offset in centidegrees.
 */
int32_t host_robot_get_offset(wasm_exec_env_t exec_env,
							  int32_t id);

/**
 * @brief Wasm-side: extern int robot_ping_servo(int id);
 * Signature "(i)i". Returns model number or <= 0 on failure.
 */
int32_t host_robot_ping_servo(wasm_exec_env_t exec_env,
							  int32_t id);

/**
 * @brief Wasm-side: extern void robot_delay_ms(int ms);
 * Signature "(i)". Blocks the WASM calling thread for ms milliseconds.
 *
 * This is the ONLY reliable way to pause between gait commands from
 * a WASM skill -- pure-WASM busy-loops run at near-zero wall time
 * inside the interpreter.
 */
int32_t host_robot_delay_ms(wasm_exec_env_t exec_env,
							int32_t ms);

// ═══════════════════════════════════════════════════════════════
//  Inverse Kinematics (per-leg)
// ═══════════════════════════════════════════════════════════════

/**
 * @brief Wasm-side: extern void robot_ik_fr(float x, float th0, float z);
 * Signature "(fff)".
 * Front-right leg IK: x = fwd/back (mm), th0 = hip rotation (deg), z = height (mm).
 */
int32_t host_robot_ik_fr(wasm_exec_env_t exec_env,
						 float x, float th0, float z);

/**
 * @brief Wasm-side: extern void robot_ik_fl(float x, float th0, float z);
 * Signature "(fff)".
 * Front-left leg IK.
 */
int32_t host_robot_ik_fl(wasm_exec_env_t exec_env,
						 float x, float th0, float z);

/**
 * @brief Wasm-side: extern void robot_ik_rr(float x, float th0, float z);
 * Signature "(fff)".
 * Rear-right leg IK.
 */
int32_t host_robot_ik_rr(wasm_exec_env_t exec_env,
						 float x, float th0, float z);

/**
 * @brief Wasm-side: extern void robot_ik_rl(float x, float th0, float z);
 * Signature "(fff)".
 * Rear-left leg IK.
 */
int32_t host_robot_ik_rl(wasm_exec_env_t exec_env,
						 float x, float th0, float z);

// ═══════════════════════════════════════════════════════════════
//  IMU
// ═══════════════════════════════════════════════════════════════

/**
 * @brief Wasm-side: extern void robot_imu_read(int buffer_ptr);
 * Signature "(i)".
 *
 * Reads the latest IMU 6-DOF sample into a WASM buffer. The buffer must
 * be at least 6 × 4 = 24 bytes. The layout written is:
 *   float[0] = ax (accel X, g)
 *   float[1] = ay (accel Y, g)
 *   float[2] = az (accel Z, g)
 *   float[3] = gx (gyro X, dps)
 *   float[4] = gy (gyro Y, dps)
 *   float[5] = gz (gyro Z, dps)
 */
int32_t host_robot_imu_read(wasm_exec_env_t exec_env,
							int32_t buffer_ptr);

/**
 * @brief Wasm-side: extern void robot_imu_print(void);
 * Signature "()". Prints the latest IMU data to the ESP log.
 */
int32_t host_robot_imu_print(wasm_exec_env_t exec_env);

// ═══════════════════════════════════════════════════════════════
//  NativeSymbol table
// ═══════════════════════════════════════════════════════════════

static const NativeSymbol NATIVE_SYMBOLS[] = {
	// SDK
	{ "print", (void *)host_print, "($i)", nullptr },

	// Robot — high-level gait
	{ "robot_gait",         (void *)host_robot_gait,         "($)",   nullptr },
	{ "robot_get_mode",     (void *)host_robot_get_mode,     "()i",   nullptr },

	// Robot — configuration
	{ "robot_set_config",   (void *)host_robot_set_config,   "(iiiii)", nullptr },
	{ "robot_get_period",   (void *)host_robot_get_period,   "()i",   nullptr },
	{ "robot_get_height",   (void *)host_robot_get_height,   "()i",   nullptr },
	{ "robot_get_up_height",(void *)host_robot_get_up_height,"()i",   nullptr },
	{ "robot_get_stride",   (void *)host_robot_get_stride,   "()i",   nullptr },
	{ "robot_get_tilt",     (void *)host_robot_get_tilt,     "()i",   nullptr },

	// Robot — low-level servo
	{ "robot_set_servo_angle",    (void *)host_robot_set_servo_angle,    "(ii)", nullptr },
	{ "robot_flush",              (void *)host_robot_flush,              "()",   nullptr },
	{ "robot_set_servo_speed",    (void *)host_robot_set_servo_speed,    "(ii)", nullptr },
	{ "robot_read_position",      (void *)host_robot_read_position,      "(i)i", nullptr },
	{ "robot_read_speed",         (void *)host_robot_read_speed,         "(i)i", nullptr },
	{ "robot_read_load",          (void *)host_robot_read_load,          "(i)i", nullptr },
	{ "robot_read_voltage",       (void *)host_robot_read_voltage,       "(i)i", nullptr },
	{ "robot_read_temperature",   (void *)host_robot_read_temperature,   "(i)i", nullptr },
	{ "robot_read_moving",        (void *)host_robot_read_moving,        "(i)i", nullptr },
	{ "robot_read_current",       (void *)host_robot_read_current,       "(i)i", nullptr },

	// Robot — calibration
	{ "robot_set_offset",   (void *)host_robot_set_offset,   "(ii)", nullptr },
	{ "robot_get_offset",   (void *)host_robot_get_offset,   "(i)i", nullptr },
	{ "robot_ping_servo",   (void *)host_robot_ping_servo,   "(i)i", nullptr },

	// Robot — utility
	{ "robot_delay_ms",     (void *)host_robot_delay_ms,     "(i)",  nullptr },

	// Robot — IK (per-leg)
	{ "robot_ik_fr",        (void *)host_robot_ik_fr,        "(fff)", nullptr },
	{ "robot_ik_fl",        (void *)host_robot_ik_fl,        "(fff)", nullptr },
	{ "robot_ik_rr",        (void *)host_robot_ik_rr,        "(fff)", nullptr },
	{ "robot_ik_rl",        (void *)host_robot_ik_rl,        "(fff)", nullptr },

	// Robot — IMU
	{ "robot_imu_read",     (void *)host_robot_imu_read,     "(i)",  nullptr },
	{ "robot_imu_print",    (void *)host_robot_imu_print,    "()",   nullptr },
};

static constexpr uint32_t NUM_NATIVE_SYMBOLS =
	sizeof(NATIVE_SYMBOLS) / sizeof(NATIVE_SYMBOLS[0]);

}  // namespace sdk
