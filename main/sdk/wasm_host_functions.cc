#include "sdk/wasm_host_functions.h"

#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wasm_export.h"

#include "robot/robot.h"
#include "wasm/wasm_sandbox.h"

static const char *TAG = "wasm_sdk";

namespace sdk {

int32_t host_print(wasm_exec_env_t exec_env,
				   int32_t text_ptr, int32_t len)
{
	// Check for watchdog cancellation — if the sandbox timeout has
	// fired, bail out so the WASM thread can terminate promptly.
	if (wasm::was_cancelled()) {
		return -1;
	}

	if (len <= 0 || text_ptr == 0) {
		ESP_LOGW(TAG, "print called with invalid args (ptr=%" PRId32 ", len=%" PRId32 ")",
				 text_ptr, len);
		return -1;
	}

	// Clamp length to a sane maximum to prevent runaway reads
	if (len > 4096) {
		ESP_LOGW(TAG, "print text too long (%" PRId32 "), truncating to 4096", len);
		len = 4096;
	}

	// With the "$" signature in our NativeSymbol, WAMR already converts
	// the wasm linear-memory offset to a native pointer before calling
	// this function.  So text_ptr IS a native pointer — do NOT call
	// wasm_runtime_addr_app_to_native again.
	const char *wasm_ptr = reinterpret_cast<const char *>(
		static_cast<uintptr_t>(text_ptr));

	if (!wasm_ptr) {
		ESP_LOGE(TAG, "print: invalid pointer %p", wasm_ptr);
		return -1;
	}

	// Copy into a stack buffer + null-terminate
	char buf[4096];
	const int copy_len = (len < 4096) ? len : 4095;
	std::memcpy(buf, wasm_ptr, static_cast<std::size_t>(copy_len));
	buf[copy_len] = '\0';

	ESP_LOGI(TAG, "print: \"%s\"", buf);

	return 0;
}

// ═══════════════════════════════════════════════════════════════
//  Robot host functions
// ═══════════════════════════════════════════════════════════════

int32_t host_robot_gait(wasm_exec_env_t exec_env,
						int32_t name_ptr)
{
	// Check for watchdog cancellation
	if (wasm::was_cancelled()) {
		return -1;
	}

	// With "$" signature, name_ptr is already a native pointer
	const char *name = reinterpret_cast<const char *>(
		static_cast<uintptr_t>(name_ptr));

	if (!name || !*name) {
		ESP_LOGW(TAG, "robot_gait: empty name");
		return -1;
	}

	ESP_LOGI(TAG, "robot_gait: \"%s\"", name);

	// Map string to GaitCmd
	robot::GaitCmd cmd = robot::GaitCmd::None;

	if      (std::strcmp(name, "none")     == 0) cmd = robot::GaitCmd::None;
	else if (std::strcmp(name, "init")     == 0) cmd = robot::GaitCmd::Init;
	else if (std::strcmp(name, "step")     == 0) cmd = robot::GaitCmd::Step;
	else if (std::strcmp(name, "roll")     == 0) cmd = robot::GaitCmd::Roll;
	else if (std::strcmp(name, "pitch")    == 0) cmd = robot::GaitCmd::Pitch;
	else if (std::strcmp(name, "stretch")  == 0) cmd = robot::GaitCmd::Stretch;
	else if (std::strcmp(name, "advance")  == 0) cmd = robot::GaitCmd::Advance;
	else if (std::strcmp(name, "back")     == 0) cmd = robot::GaitCmd::Back;
	else if (std::strcmp(name, "left")     == 0) cmd = robot::GaitCmd::Left;
	else if (std::strcmp(name, "right")    == 0) cmd = robot::GaitCmd::Right;
	else if (std::strcmp(name, "turnL")    == 0) cmd = robot::GaitCmd::TurnL;
	else if (std::strcmp(name, "turnR")    == 0) cmd = robot::GaitCmd::TurnR;
	else if (std::strcmp(name, "twerk")    == 0) cmd = robot::GaitCmd::Twerk;
	else if (std::strcmp(name, "jump")     == 0) cmd = robot::GaitCmd::Jump;
	else if (std::strcmp(name, "jumpfwd")  == 0) cmd = robot::GaitCmd::JumpFwd;
	else if (std::strcmp(name, "testspeed")== 0) cmd = robot::GaitCmd::TestSpeed;
	else if (std::strcmp(name, "lookup")   == 0) cmd = robot::GaitCmd::LookUp;
	else if (std::strcmp(name, "lookdown") == 0) cmd = robot::GaitCmd::LookDown;
	else if (std::strcmp(name, "lookleft") == 0) cmd = robot::GaitCmd::LookLeft;
	else if (std::strcmp(name, "lookright")== 0) cmd = robot::GaitCmd::LookRight;
	else if (std::strcmp(name, "lookul")   == 0) cmd = robot::GaitCmd::LookUpperLeft;
	else if (std::strcmp(name, "lookur")   == 0) cmd = robot::GaitCmd::LookUpperRight;
	else if (std::strcmp(name, "lookll")   == 0) cmd = robot::GaitCmd::LookLowerLeft;
	else if (std::strcmp(name, "looklr")   == 0) cmd = robot::GaitCmd::LookLowerRight;
	else if (std::strcmp(name, "flegL")    == 0) cmd = robot::GaitCmd::ForelegLiftL;
	else if (std::strcmp(name, "flegR")    == 0) cmd = robot::GaitCmd::ForelegLiftR;
	else if (std::strcmp(name, "blegL")    == 0) cmd = robot::GaitCmd::BacklegLiftL;
	else if (std::strcmp(name, "blegR")    == 0) cmd = robot::GaitCmd::BacklegLiftR;
	else if (std::strcmp(name, "heightup") == 0) cmd = robot::GaitCmd::HeightUp;
	else if (std::strcmp(name, "heightdown")==0) cmd = robot::GaitCmd::HeightDown;
	else if (std::strcmp(name, "balance")  == 0) cmd = robot::GaitCmd::Balance;
	else if (std::strcmp(name, "bowback")  == 0) cmd = robot::GaitCmd::BowBack;
	else if (std::strcmp(name, "bodycycle")== 0) cmd = robot::GaitCmd::BodyCycle;
	else if (std::strcmp(name, "headellipse")==0) cmd = robot::GaitCmd::HeadEllipse;
	else if (std::strcmp(name, "moveLF")   == 0) cmd = robot::GaitCmd::MoveLeftFront;
	else if (std::strcmp(name, "moveRF")   == 0) cmd = robot::GaitCmd::MoveRightFront;
	else if (std::strcmp(name, "moveLB")   == 0) cmd = robot::GaitCmd::MoveLeftBack;
	else if (std::strcmp(name, "moveRB")   == 0) cmd = robot::GaitCmd::MoveRightBack;
	else if (std::strcmp(name, "stanford") == 0) cmd = robot::GaitCmd::StanfordWalk;
	else if (std::strcmp(name, "frontkick")== 0) cmd = robot::GaitCmd::FrontKick;
	else if (std::strcmp(name, "wiggle")   == 0) cmd = robot::GaitCmd::Wiggle;
	else if (std::strcmp(name, "buttshrug")== 0) cmd = robot::GaitCmd::ButtShrug;
	else if (std::strcmp(name, "wiggleL")  == 0) cmd = robot::GaitCmd::WiggleLeft;
	else if (std::strcmp(name, "wiggleR")  == 0) cmd = robot::GaitCmd::WiggleRight;
	else if (std::strcmp(name, "buttshrugL")==0) cmd = robot::GaitCmd::ButtShrugLeft;
	else if (std::strcmp(name, "buttshrugR")==0) cmd = robot::GaitCmd::ButtShrugRight;
	else {
		ESP_LOGW(TAG, "robot_gait: unknown gait \"%s\"", name);
		return -1;
	}

	robot::send_gait_cmd(cmd);
	return 0;
}

int32_t host_robot_get_mode(wasm_exec_env_t exec_env)
{
	int32_t mode = static_cast<int32_t>(robot::current_gait_cmd());
	ESP_LOGI(TAG, "robot_get_mode: %" PRId32, mode);
	return mode;
}

int32_t host_robot_set_config(wasm_exec_env_t exec_env,
							  int32_t period, int32_t height,
							  int32_t up_height, int32_t stride,
							  int32_t tilt)
{
	// Start from the CURRENT config so fields not exposed to WASM
	// (e.g. sg_speed, the Stanford walk speed) keep their values.
	robot::Config cfg = robot::get_config();
	cfg.period    = static_cast<int>(period);
	cfg.height    = static_cast<int>(height);
	cfg.up_height = static_cast<int>(up_height);
	cfg.stride    = static_cast<int>(stride);
	cfg.tilt      = static_cast<int>(tilt);
	robot::set_config(cfg);

	ESP_LOGI(TAG, "robot_set_config: p=%d h=%d uh=%d s=%d t=%d",
			 cfg.period, cfg.height, cfg.up_height, cfg.stride, cfg.tilt);
	return 0;
}

int32_t host_robot_get_period(wasm_exec_env_t exec_env)
{
	int32_t val = static_cast<int32_t>(robot::get_config().period);
	ESP_LOGD(TAG, "robot_get_period: %" PRId32, val);
	return val;
}

int32_t host_robot_get_height(wasm_exec_env_t exec_env)
{
	int32_t val = static_cast<int32_t>(robot::get_config().height);
	ESP_LOGD(TAG, "robot_get_height: %" PRId32, val);
	return val;
}

int32_t host_robot_get_up_height(wasm_exec_env_t exec_env)
{
	int32_t val = static_cast<int32_t>(robot::get_config().up_height);
	ESP_LOGD(TAG, "robot_get_up_height: %" PRId32, val);
	return val;
}

int32_t host_robot_get_stride(wasm_exec_env_t exec_env)
{
	int32_t val = static_cast<int32_t>(robot::get_config().stride);
	ESP_LOGD(TAG, "robot_get_stride: %" PRId32, val);
	return val;
}

int32_t host_robot_get_tilt(wasm_exec_env_t exec_env)
{
	int32_t val = static_cast<int32_t>(robot::get_config().tilt);
	ESP_LOGD(TAG, "robot_get_tilt: %" PRId32, val);
	return val;
}

int32_t host_robot_set_servo_angle(wasm_exec_env_t exec_env,
								   int32_t id, int32_t centideg)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "set_servo_angle: invalid id %" PRId32, id);
		return -1;
	}
	robot::set_servo_angle(static_cast<int>(id),
						   static_cast<float>(centideg) / 100.0f);
	ESP_LOGV(TAG, "robot_set_servo_angle: id=%" PRId32 " deg=%.2f",
			 id, static_cast<float>(centideg) / 100.0f);
	return 0;
}

int32_t host_robot_flush(wasm_exec_env_t exec_env)
{
	robot::flush();
	ESP_LOGV(TAG, "robot_flush");
	return 0;
}

int32_t host_robot_set_servo_speed(wasm_exec_env_t exec_env,
								   int32_t id, int32_t speed)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "set_servo_speed: invalid id %" PRId32, id);
		return -1;
	}
	robot::set_servo_speed(static_cast<int>(id),
						   static_cast<uint16_t>(speed));
	ESP_LOGV(TAG, "robot_set_servo_speed: id=%" PRId32 " speed=%" PRId32, id, speed);
	return 0;
}

int32_t host_robot_read_position(wasm_exec_env_t exec_env,
								 int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_position: invalid id %" PRId32, id);
		return -1;
	}
	int32_t val = static_cast<int32_t>(robot::read_position(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_position: id=%" PRId32 " pos=%" PRId32, id, val);
	return val;
}

int32_t host_robot_read_speed(wasm_exec_env_t exec_env,
							  int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_speed: invalid id %" PRId32, id);
		return -1;
	}
	int32_t val = static_cast<int32_t>(robot::read_speed(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_speed: id=%" PRId32 " speed=%" PRId32, id, val);
	return val;
}

int32_t host_robot_read_load(wasm_exec_env_t exec_env,
							 int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_load: invalid id %" PRId32, id);
		return -1;
	}
	int32_t val = static_cast<int32_t>(robot::read_load(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_load: id=%" PRId32 " load=%" PRId32, id, val);
	return val;
}

int32_t host_robot_read_voltage(wasm_exec_env_t exec_env,
								int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_voltage: invalid id %" PRId32, id);
		return -1;
	}
	int32_t val = static_cast<int32_t>(robot::read_voltage(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_voltage: id=%" PRId32 " mV=%" PRId32, id, val);
	return val;
}

int32_t host_robot_read_temperature(wasm_exec_env_t exec_env,
									int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_temperature: invalid id %" PRId32, id);
		return -1;
	}
	int32_t val = static_cast<int32_t>(robot::read_temperature(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_temperature: id=%" PRId32 " temp=%" PRId32, id, val);
	return val;
}

int32_t host_robot_read_moving(wasm_exec_env_t exec_env,
							   int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_moving: invalid id %" PRId32, id);
		return -1;
	}
	int32_t val = static_cast<int32_t>(robot::read_moving(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_moving: id=%" PRId32 " moving=%" PRId32, id, val);
	return val;
}

int32_t host_robot_read_current(wasm_exec_env_t exec_env,
								int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "read_current: invalid id %" PRId32, id);
		return -1;
	}
	int32_t val = static_cast<int32_t>(robot::read_current(static_cast<int>(id)));
	ESP_LOGD(TAG, "read_current: id=%" PRId32 " mA=%" PRId32, id, val);
	return val;
}

int32_t host_robot_set_offset(wasm_exec_env_t exec_env,
							  int32_t id, int32_t centideg)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "set_offset: invalid id %" PRId32, id);
		return -1;
	}
	robot::set_offset(static_cast<int>(id),
					  static_cast<float>(centideg) / 100.0f);
	ESP_LOGV(TAG, "set_offset: id=%" PRId32 " deg=%.2f",
			 id, static_cast<float>(centideg) / 100.0f);
	return 0;
}

int32_t host_robot_get_offset(wasm_exec_env_t exec_env,
							  int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "get_offset: invalid id %" PRId32, id);
		return 0;
	}
	float deg = robot::get_offset(static_cast<int>(id));
	ESP_LOGV(TAG, "get_offset: id=%" PRId32 " deg=%.2f", id, deg);
	return static_cast<int32_t>(deg * 100.0f);
}

int32_t host_robot_ping_servo(wasm_exec_env_t exec_env,
							  int32_t id)
{
	if (id < 1 || id > 12) {
		ESP_LOGW(TAG, "ping_servo: invalid id %" PRId32, id);
		return -1;
	}
	int32_t result = static_cast<int32_t>(
		robot::ping_servo(static_cast<int>(id)));
	ESP_LOGV(TAG, "ping_servo: id=%" PRId32 " result=%" PRId32, id, result);
	return result;
}

int32_t host_robot_delay_ms(wasm_exec_env_t exec_env,
							int32_t ms)
{
	if (ms <= 0) return 0;

	// Break the delay into small chunks so we can check for
	// watchdog cancellation.  This lets the WASM thread terminate
	// promptly when the sandbox timeout fires.
	constexpr TickType_t CHUNK_MS = 50;
	int32_t remaining = ms;

	ESP_LOGV(TAG, "robot_delay_ms: %" PRId32 " ms", ms);

	while (remaining > 0) {
		if (wasm::was_cancelled()) {
			ESP_LOGW(TAG, "robot_delay_ms: cancelled after %" PRId32 " ms",
					 ms - remaining);
			return -1;
		}

		TickType_t delay = pdMS_TO_TICKS(
			(remaining > static_cast<int32_t>(CHUNK_MS))
				? CHUNK_MS
				: static_cast<TickType_t>(remaining));
		vTaskDelay(delay);
		remaining -= static_cast<int32_t>(CHUNK_MS);
	}

	return 0;
}

// ═══════════════════════════════════════════════════════════════
//  Inverse Kinematics host functions
// ═══════════════════════════════════════════════════════════════

int32_t host_robot_ik_fr(wasm_exec_env_t exec_env,
						 float x, float th0, float z)
{
	if (wasm::was_cancelled()) return -1;
	ESP_LOGD(TAG, "ik_fr: x=%.1f th0=%.1f z=%.1f", x, th0, z);
	robot::front_right_ik(x, th0, z);
	return 0;
}

int32_t host_robot_ik_fl(wasm_exec_env_t exec_env,
						 float x, float th0, float z)
{
	if (wasm::was_cancelled()) return -1;
	ESP_LOGD(TAG, "ik_fl: x=%.1f th0=%.1f z=%.1f", x, th0, z);
	robot::front_left_ik(x, th0, z);
	return 0;
}

int32_t host_robot_ik_rr(wasm_exec_env_t exec_env,
						 float x, float th0, float z)
{
	if (wasm::was_cancelled()) return -1;
	ESP_LOGD(TAG, "ik_rr: x=%.1f th0=%.1f z=%.1f", x, th0, z);
	robot::rear_right_ik(x, th0, z);
	return 0;
}

int32_t host_robot_ik_rl(wasm_exec_env_t exec_env,
						 float x, float th0, float z)
{
	if (wasm::was_cancelled()) return -1;
	ESP_LOGD(TAG, "ik_rl: x=%.1f th0=%.1f z=%.1f", x, th0, z);
	robot::rear_left_ik(x, th0, z);
	return 0;
}

// ═══════════════════════════════════════════════════════════════
//  IMU host functions
// ═══════════════════════════════════════════════════════════════

int32_t host_robot_imu_read(wasm_exec_env_t exec_env,
							int32_t buffer_ptr)
{
	if (wasm::was_cancelled()) return -1;
	if (buffer_ptr == 0) {
		ESP_LOGW(TAG, "robot_imu_read: null buffer pointer");
		return -1;
	}

	// Convert WASM linear-memory offset to a native pointer
	wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
	if (!module_inst) {
		ESP_LOGE(TAG, "robot_imu_read: failed to get module instance");
		return -1;
	}

	void *native_ptr = wasm_runtime_addr_app_to_native(
		module_inst, static_cast<uint32_t>(buffer_ptr));
	if (!native_ptr) {
		ESP_LOGE(TAG, "robot_imu_read: invalid WASM pointer %" PRId32, buffer_ptr);
		return -1;
	}

	// Read the latest IMU data
	robot::ImuData data = robot::imu_read();

	// Write 6 floats into the WASM buffer (ax, ay, az, gx, gy, gz)
	float *buf = static_cast<float *>(native_ptr);
	buf[0] = data.ax;
	buf[1] = data.ay;
	buf[2] = data.az;
	buf[3] = data.gx;
	buf[4] = data.gy;
	buf[5] = data.gz;

	ESP_LOGD(TAG, "robot_imu_read: ax=%.2f ay=%.2f az=%.2f gx=%.2f gy=%.2f gz=%.2f",
			 data.ax, data.ay, data.az, data.gx, data.gy, data.gz);
	return 0;
}

int32_t host_robot_imu_print(wasm_exec_env_t exec_env)
{
	if (wasm::was_cancelled()) return -1;
	ESP_LOGV(TAG, "robot_imu_print");
	robot::imu_print();
	return 0;
}

}  // namespace sdk

// ============================================================================
//  Registration function called by wasm_sandbox.cc after runtime init
// ============================================================================
extern "C" void wasm_host_functions_register()
{
	wasm::register_natives(
		"env",
		const_cast<NativeSymbol *>(sdk::NATIVE_SYMBOLS),
		sdk::NUM_NATIVE_SYMBOLS);

	ESP_LOGI(TAG, "SDK + Robot host functions registered (%" PRIu32 " symbols)",
			 sdk::NUM_NATIVE_SYMBOLS);
}
