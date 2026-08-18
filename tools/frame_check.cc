/* frame_check — prove the angle path still means what it meant.
 *
 *     c++ -std=c++17 -O1 -o frame_check tools/frame_check.cc && ./frame_check
 *
 * This firmware has two raw position frames running in opposite directions:
 * the GAIT frame that s_goal_pos[] and set_servo_angle() speak, and the AT32
 * frame the driver boards, the feedback cache and Servo Studio speak, with
 * at32 == 1024 - gait. See the frame note in robot/robot.h.
 *
 * Confusing them is not a compile error and not visible in a code review. It
 * has already happened once: read_moving() compared a measured AT32 value
 * against a commanded gait value, so every pose away from dead centre read as
 * "still moving" forever and any skill polling it hung.
 *
 * This file models the whole path -- command, overlay, wire, feedback, read
 * back -- and asserts the properties that must hold. It compiles natively, so
 * it runs on a laptop in a second with no robot and no ESP-IDF. Run it after
 * touching set_servo_angle(), flush(), driver_board_sync_write(), fb_store(),
 * read_angle_cdeg(), or the overlay.
 *
 * Keep the constants below in step with robot.h and driver_board.c. They are
 * duplicated deliberately: the point is to notice when one of them changes.
 */
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

/* ── mirrors of the firmware ──────────────────────────────────────────────*/
constexpr float SERVO_RANGE_DEG  = 270.0f;                      /* robot.h   */
constexpr float SERVO_DEG_TO_RAW = 1023.0f / SERVO_RANGE_DEG;
constexpr float OVERLAY_MAX_DEG  = 20.0f;                       /* robot.h   */
constexpr int at32_from_gait(int g) { return 1024 - g; }        /* robot.h   */

static int set_servo_angle(float deg)                           /* robot.cc  */
{
	int sig = 511 + static_cast<int>(deg * SERVO_DEG_TO_RAW);
	return std::clamp(sig, 0, 1023);
}

static int flush_pos(int goal_gait, float overlay_deg)          /* robot.cc  */
{
	int raw = goal_gait;
	if (overlay_deg != 0.0f) {
		raw += static_cast<int>(overlay_deg * SERVO_DEG_TO_RAW);
		raw = std::clamp(raw, 0, 1023);
	}
	return raw;
}

/* driver_board.c — the ONLY place the direction flips */
static uint16_t wire_dd(int gait_raw)
{
	return static_cast<uint16_t>(2700 - static_cast<uint32_t>(gait_raw) * 2700u / 1024u);
}
/* driver_board.c — scales back, deliberately does NOT flip */
static uint16_t fb_from_wire(uint16_t dd)
{
	return static_cast<uint16_t>(static_cast<uint32_t>(dd) * 1024u / 2700u);
}
static int read_angle_cdeg(uint16_t fb_at32)                    /* robot.cc  */
{
	const int   gait = at32_from_gait(static_cast<int>(fb_at32));
	const float deg  = (gait - 511) / SERVO_DEG_TO_RAW;
	return static_cast<int>(deg * 100.0f);
}

/* ── the properties ───────────────────────────────────────────────────────*/
static int g_failures = 0;
static void check(bool ok, const char *what, const char *detail = "")
{
	std::printf("  %-4s %s%s%s\n", ok ? "ok" : "FAIL", what,
	            *detail ? "  " : "", detail);
	if (!ok) ++g_failures;
}

static int measure(float deg, float overlay = 0.0f)
{
	return read_angle_cdeg(fb_from_wire(wire_dd(flush_pos(set_servo_angle(deg), overlay))));
}

int main()
{
	std::printf("frame_check — the angle path, end to end\n\n");

	/* 1. A command must come back as itself. Quantisation only. */
	double worst = 0.0;
	for (float d = -135.0f; d <= 135.0f; d += 0.25f)
		worst = std::max(worst, std::fabs(measure(d) / 100.0 - d));
	char buf[64];
	std::snprintf(buf, sizeof buf, "(worst %.3f deg)", worst);
	check(worst < 0.5, "command survives the round trip", buf);

	/* 2. No mirror. This is the one that destroys a robot. */
	int inversions = 0;
	for (float d = -130.0f; d <= 129.0f; d += 1.0f)
		if (measure(d + 1.0f) <= measure(d)) ++inversions;
	check(inversions == 0, "a positive command raises the measured angle");

	/* 3. An overlay must push a joint the same way a command does. */
	int wrong = 0;
	for (float d = -100.0f; d <= 100.0f; d += 5.0f)
		if (!(measure(d, +5.0f) > measure(d) && measure(d, -5.0f) < measure(d))) ++wrong;
	check(wrong == 0, "overlay agrees in sign with commands");

	/* 4. An overlay at its limit must not be able to swing a joint wildly. */
	double biggest = 0.0;
	for (float d = -120.0f; d <= 120.0f; d += 5.0f)
		biggest = std::max(biggest,
		                   std::fabs(measure(d, OVERLAY_MAX_DEG) - measure(d)) / 100.0);
	std::snprintf(buf, sizeof buf, "(max %.1f deg, clamp is %.0f)", biggest, OVERLAY_MAX_DEG);
	check(biggest <= OVERLAY_MAX_DEG + 1.0, "overlay stays inside its clamp", buf);

	/* 5. The frames really are opposed — so anything comparing a measured
	 *    value against a commanded one must convert. read_moving() forgot
	 *    once; this is the property that made that a bug. */
	const int gait_centre = set_servo_angle(0.0f);
	const int gait_up     = set_servo_angle(30.0f);
	check(at32_from_gait(gait_up) < at32_from_gait(gait_centre),
	      "AT32 runs opposite to GAIT (so conversion is mandatory)");

	/* 6. Centre is centre in both frames, which is why a bug here hides. */
	check(std::abs(measure(0.0f)) < 50, "zero commands zero");

	std::printf("\n%s\n", g_failures ? "FAILED — do not flash this."
	                                 : "All properties hold.");
	return g_failures ? 1 : 0;
}
