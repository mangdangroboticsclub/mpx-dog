#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "lua.h"
#ifdef __cplusplus
}
#endif

/**
 * @brief Register all robot bindings into the given Lua state.
 *        Creates the `robot` global table with all sub-modules.
 *
 * Called automatically by lua_init(). Safe to call multiple times
 * on different states (idempotent per state).
 *
 * Registered functions:
 *
 *   robot.gait(name)           — Start gait by string name
 *   robot.get_mode()           → Current gait as string
 *   robot.get_config()         → {period, height, up_height, stride, tilt}
 *   robot.set_config(p, h, u, s, t) — Set gait parameters
 *
 *   robot.set_servo_angle(id, deg) — Set servo angle (degrees)
 *   robot.set_servo_speed(id, speed) — Set servo speed (0=max)
 *   robot.set_all_servo_speed(speed) — Set all servos speed
 *   robot.flush()              — Commit servo positions
 *
 *   robot.read_position(id)    → raw 0-1023
 *   robot.read_speed(id)       → signed speed
 *   robot.read_load(id)        → signed load
 *   robot.read_voltage(id)     → 0.1V units
 *   robot.read_temperature(id) → °C
 *   robot.read_moving(id)      → 0/1
 *   robot.read_current(id)     → mA
 *   robot.ping(id)             → model number or 0
 *
 *   robot.set_offset(id, deg)  — Calibration offset (degrees)
 *   robot.get_offset(id)       → offset in degrees
 *   robot.reset_offsets()      — Zero all calibration offsets
 *
 *   robot.delay_ms(ms)         — Blocking delay with cancellation
 *
 *   robot.ik_fr(x, th0, z)     — Front-right leg IK (no flush)
 *   robot.ik_fl(x, th0, z)     — Front-left  leg IK
 *   robot.ik_rr(x, th0, z)     — Rear-right  leg IK
 *   robot.ik_rl(x, th0, z)     — Rear-left   leg IK
 *
 *   robot.imu_read()           → {ax, ay, az, gx, gy, gz}
 *   robot.imu_print()          — Log IMU to console
 *
 * @param L  Lua state to register into
 */
void lua_register_robot_bindings(lua_State *L);

/**
 * @brief Register the `wasm` module into the given Lua state.
 *
 * Creates global `wasm` table with:
 *   wasm.run(path, func_name?) → bool  — Run .wasm from LittleFS
 *   wasm.run_bytes(data, func_name?) → bool — Run from a string buffer
 *
 * @param L  Lua state to register into
 */
void lua_register_wasm_bindings(lua_State *L);

/**
 * @brief Register the `fs` module into the given Lua state.
 *
 * Creates global `fs` table with:
 *   fs.read(path)          → string or nil
 *   fs.write(path, content) → bool (requests user permission)
 *   fs.delete(path)        → bool (requests user permission)
 *   fs.exists(path)        → bool
 *   fs.list(dir)           → table of {name, size, is_dir}
 *   fs.info()              → {total, used} bytes
 *
 * Write and delete operations broadcast an action telemetry to
 * the PWA and block waiting for explicit user approval.
 *
 * @param L  Lua state to register into
 */
void lua_register_fs_bindings(lua_State *L);
