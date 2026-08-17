#pragma once

#include <cstddef>
#include <cstdint>

#include "wasm_export.h"

namespace sdk {

// ═══════════════════════════════════════════════════════════════
//  ABI version and the one error convention
// ═══════════════════════════════════════════════════════════════

/* MPX_ABI_VERSION 2 — a BREAKING change from version 1.
 *
 * Seventeen host functions used to be registered with signatures that declare
 * no result ("()", "(ii)", "($)" ...) even though every one of them computed
 * an error code. WAMR discards a result the signature does not mention, so
 * those codes were physically unreachable from a skill: a misspelled gait
 * name, an out-of-range servo id, a null IMU pointer and a watchdog
 * cancellation all looked identical from inside a skill — like nothing
 * happening — while the real reason went to a serial console nobody was
 * watching.
 *
 * They now return int32_t. This is not backwards compatible and cannot be
 * made so: WAMR's check_symbol_signature() requires the signature string to
 * be fully consumed after ')' when the calling module declares no result, so
 * a v1 module trying to call one of these fails to link and traps on first
 * call. Skills built against SDK v1 must be rebuilt — one `mpx-cli deploy`.
 * Better to spend that now than after the marketplace fills with binaries
 * nobody can recompile.
 */
constexpr int32_t MPX_ABI_VERSION = 2;

/* Every host function returns one of these, or a value >= 0 where it is
 * documented to return data. Before v2 there were four incompatible
 * conventions in one 48-symbol table; this is the only one now.
 */
enum : int32_t {
	MPX_OK             =  0,   /**< Success.                                */
	MPX_ERR_ARG        = -1,   /**< Bad argument: id, index or pointer.     */
	MPX_ERR_NOT_LOCKED = -2,   /**< YOU do not hold the servo bus.          */
	MPX_ERR_NO_REPLY   = -3,   /**< The driver board did not answer.        */
	MPX_ERR_READONLY   = -4,   /**< Calibration parameter; read-only.       */
	MPX_ERR_CANCELLED  = -5,   /**< The skill was stopped mid-call.         */
	MPX_ERR_STATE      = -6,   /**< Right call, wrong time.                 */
};

// ═══════════════════════════════════════════════════════════════
//  print — SDK host function
// ═══════════════════════════════════════════════════════════════

/**
 * @brief Wasm-side: extern int mpx_abi_version(void);
 * Signature "()i". Returns MPX_ABI_VERSION. Lets a skill — or the CLI —
 * check what it is talking to instead of discovering a mismatch as a trap.
 */
int32_t host_mpx_abi_version(wasm_exec_env_t exec_env);

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
 * @brief Wasm-side: extern void robot_set_body_pose(float roll_deg,
 *                       float pitch_deg, float yaw_deg);
 * Signature "(fff)". Holds a Stanford-IK body attitude in degrees.
 */
int32_t host_robot_set_body_pose(wasm_exec_env_t exec_env,
                                 float roll_deg, float pitch_deg,
                                 float yaw_deg);

/**
 * @brief Wasm-side: extern void robot_set_attitude_speed(int dps);
 * Signature "(i)". Sets the roll/pitch/yaw slew speed in degrees/second
 * (0 = instant snap; >0 eases toward the target at that speed).
 */
int32_t host_robot_set_attitude_speed(wasm_exec_env_t exec_env,
                                      int32_t dps);

/**
 * @brief Wasm-side: extern void robot_set_attitude_speed_xyz(int roll_dps,
 *                       int pitch_dps, int yaw_dps);
 * Signature "(iii)". Per-axis roll/pitch/yaw slew speed in degrees/second
 * (0 on an axis = instant snap; >0 = eases at that speed).
 */
int32_t host_robot_set_attitude_speed_xyz(wasm_exec_env_t exec_env,
                                          int32_t roll_dps, int32_t pitch_dps,
                                          int32_t yaw_dps);

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
 * Signature "(i)i". Returns raw position (0-1023) in the AT32 FRAME, or -1
 * on error. This is NOT the frame robot_set_servo_angle() accepts — see
 * robot_read_angle_cdeg() below.
 */
int32_t host_robot_read_position(wasm_exec_env_t exec_env,
								 int32_t id);

/**
 * @brief Wasm-side: extern int robot_read_angle_cdeg(int id);
 * Signature "(i)i". Returns the measured angle in signed centidegrees from
 * centre — the same frame robot_set_servo_angle() takes — or INT32_MIN on a
 * bad id. Use this one to close a control loop.
 */
int32_t host_robot_read_angle_cdeg(wasm_exec_env_t exec_env,
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

// NOTE: intentionally NOT const. WAMR's wasm_runtime_register_natives()
// sorts this table in place with qsort() so it can binary-search symbol
// names later. If the array is const it is placed in flash (.rodata) and
// the in-place sort triggers a "Dbus write to cache rejected" cache-error
// panic on the ESP32-S3. Keeping it non-const puts it in writable RAM.

// ═══════════════════════════════════════════════════════════════
//  Low-level servo (Unitree-style) — see wasm_host_functions.cc
// ═══════════════════════════════════════════════════════════════

int32_t host_servo_lock(wasm_exec_env_t exec_env);
int32_t host_servo_unlock(wasm_exec_env_t exec_env);
int32_t host_servo_is_locked(wasm_exec_env_t exec_env);

int32_t host_servo_set_gain(wasm_exec_env_t exec_env, int32_t id, int32_t param, float value);
int32_t host_servo_get_gain(wasm_exec_env_t exec_env, int32_t id, int32_t param, int32_t out_ptr);
int32_t host_servo_save_config(wasm_exec_env_t exec_env, int32_t id);
int32_t host_servo_restore_config(wasm_exec_env_t exec_env, int32_t id);

int32_t host_servo_stage(wasm_exec_env_t exec_env, int32_t id, float q_deg, float tau_ma,
						 float kp, float kd);
int32_t host_servo_commit(wasm_exec_env_t exec_env);
int32_t host_servo_write_all(wasm_exec_env_t exec_env, int32_t cmd_ptr, int32_t count);
int32_t host_servo_read(wasm_exec_env_t exec_env, int32_t id, int32_t out_ptr);
int32_t host_servo_read_all(wasm_exec_env_t exec_env, int32_t out_ptr);
int32_t host_servo_poll(wasm_exec_env_t exec_env);
int32_t host_servo_direct(wasm_exec_env_t exec_env, int32_t id, int32_t mode,
						  float q_deg, float tau_ma);
int32_t host_servo_scan(wasm_exec_env_t exec_env);

static NativeSymbol NATIVE_SYMBOLS[] = {
	// ABI
	{ "mpx_abi_version",        (void *)host_mpx_abi_version,        "()i",  nullptr },
	// SDK
	{ "print", (void *)host_print, "($i)i", nullptr },

	// Robot — high-level gait
	{ "robot_gait",         (void *)host_robot_gait,         "($)i",   nullptr },
	{ "robot_get_mode",     (void *)host_robot_get_mode,     "()i",   nullptr },
	{ "robot_set_body_pose",(void *)host_robot_set_body_pose,"(fff)i", nullptr },
	{ "robot_set_attitude_speed",(void *)host_robot_set_attitude_speed,"(i)i", nullptr },
	{ "robot_set_attitude_speed_xyz",(void *)host_robot_set_attitude_speed_xyz,"(iii)i", nullptr },

	// Robot — configuration
	{ "robot_set_config",   (void *)host_robot_set_config,   "(iiiii)i", nullptr },
	{ "robot_get_period",   (void *)host_robot_get_period,   "()i",   nullptr },
	{ "robot_get_height",   (void *)host_robot_get_height,   "()i",   nullptr },
	{ "robot_get_up_height",(void *)host_robot_get_up_height,"()i",   nullptr },
	{ "robot_get_stride",   (void *)host_robot_get_stride,   "()i",   nullptr },
	{ "robot_get_tilt",     (void *)host_robot_get_tilt,     "()i",   nullptr },

	// Robot — low-level servo
	{ "robot_set_servo_angle",    (void *)host_robot_set_servo_angle,    "(ii)i", nullptr },
	{ "robot_flush",              (void *)host_robot_flush,              "()i",   nullptr },
	{ "robot_set_servo_speed",    (void *)host_robot_set_servo_speed,    "(ii)i", nullptr },
	{ "robot_read_position",      (void *)host_robot_read_position,      "(i)i", nullptr },
	{ "robot_read_angle_cdeg",    (void *)host_robot_read_angle_cdeg,    "(i)i", nullptr },
	{ "robot_read_speed",         (void *)host_robot_read_speed,         "(i)i", nullptr },
	{ "robot_read_load",          (void *)host_robot_read_load,          "(i)i", nullptr },
	{ "robot_read_voltage",       (void *)host_robot_read_voltage,       "(i)i", nullptr },
	{ "robot_read_temperature",   (void *)host_robot_read_temperature,   "(i)i", nullptr },
	{ "robot_read_moving",        (void *)host_robot_read_moving,        "(i)i", nullptr },
	{ "robot_read_current",       (void *)host_robot_read_current,       "(i)i", nullptr },

	// Robot — calibration
	{ "robot_set_offset",   (void *)host_robot_set_offset,   "(ii)i", nullptr },
	{ "robot_get_offset",   (void *)host_robot_get_offset,   "(i)i", nullptr },
	{ "robot_ping_servo",   (void *)host_robot_ping_servo,   "(i)i", nullptr },

	// Robot — utility
	{ "robot_delay_ms",     (void *)host_robot_delay_ms,     "(i)i",  nullptr },

	// Robot — IK (per-leg)
	{ "robot_ik_fr",        (void *)host_robot_ik_fr,        "(fff)i", nullptr },
	{ "robot_ik_fl",        (void *)host_robot_ik_fl,        "(fff)i", nullptr },
	{ "robot_ik_rr",        (void *)host_robot_ik_rr,        "(fff)i", nullptr },
	{ "robot_ik_rl",        (void *)host_robot_ik_rl,        "(fff)i", nullptr },

	// Robot — IMU
	{ "robot_imu_read",     (void *)host_robot_imu_read,     "(i)i",  nullptr },
	{ "robot_imu_print",    (void *)host_robot_imu_print,    "()i",   nullptr },
	// Robot — low-level servo (Unitree-style, SDK v2)
	{ "servo_lock",           (void *)host_servo_lock,           "()i",     nullptr },
	{ "servo_unlock",         (void *)host_servo_unlock,         "()i",     nullptr },
	{ "servo_is_locked",      (void *)host_servo_is_locked,      "()i",     nullptr },
	{ "servo_set_gain",       (void *)host_servo_set_gain,       "(iif)i",  nullptr },
	{ "servo_get_gain",       (void *)host_servo_get_gain,       "(iii)i",  nullptr },
	{ "servo_save_config",    (void *)host_servo_save_config,    "(i)i",    nullptr },
	{ "servo_restore_config", (void *)host_servo_restore_config, "(i)i",    nullptr },
	{ "servo_stage",          (void *)host_servo_stage,          "(iffff)i",nullptr },
	{ "servo_commit",         (void *)host_servo_commit,         "()i",     nullptr },
	{ "servo_write_all",      (void *)host_servo_write_all,      "(ii)i",   nullptr },
	{ "servo_read",           (void *)host_servo_read,           "(ii)i",   nullptr },
	{ "servo_read_all",       (void *)host_servo_read_all,       "(i)i",    nullptr },
	{ "servo_poll",           (void *)host_servo_poll,           "()i",     nullptr },
	{ "servo_direct",         (void *)host_servo_direct,         "(iiff)i", nullptr },
	{ "servo_scan",           (void *)host_servo_scan,           "()i",     nullptr },

};

static constexpr uint32_t NUM_NATIVE_SYMBOLS =
	sizeof(NATIVE_SYMBOLS) / sizeof(NATIVE_SYMBOLS[0]);

}  // namespace sdk
