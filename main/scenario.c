#include "scenario.h"

#include <math.h>

#include "config.h"

static float ease(float t) { return t <= 0.0f ? 0.0f : t >= 1.0f ? 1.0f : t * t * (3.0f - 2.0f * t); }

void scenario_forces(float t, sim_forces_t *out)
{
    t = fmodf(t, SCENARIO_SECONDS);
    // Gravity direction in simulation axes: +y is down the upright screen, +z is into the case.
    float x = 0.0f, y = 1.0f, z = 0.0f, shake[3] = {0};
    if (t < 3.0f) {
        // upright
    } else if (t < 8.0f) {
        // tilt: 35 degrees to one side, then the other
        const float a = 0.61f * sinf((t - 3.0f) * 2.0f * (float)M_PI / 5.0f);
        x = sinf(a); y = cosf(a);
    } else if (t < 11.0f) {
        // lay flat
        const float a = ease((t - 8.0f) / 1.5f) * (float)M_PI * 0.5f;
        y = cosf(a); z = sinf(a);
    } else if (t < 16.0f) {
        // flat circling: rotate an approximately 12-degree tilt around the screen normal.
        const float w = (t - 11.0f) * 2.0f * (float)M_PI / 2.5f, r = 0.21f * ease((t - 11.0f) / 0.5f);
        x = r * cosf(w); y = r * sinf(w); z = 1.0f;
    } else if (t < 18.0f) {
        // stand up
        const float a = (1.0f - ease((t - 16.0f) / 1.5f)) * (float)M_PI * 0.5f;
        y = cosf(a); z = sinf(a);
    } else if (t < 21.0f) {
        // shake: apply 3 Hz lateral acceleration, pause, then apply positive-y pulses.
        const float u = t - 18.0f, side = sinf(u * 2.0f * (float)M_PI * 3.0f), flick = sinf((u - 1.6f) * 2.0f * (float)M_PI * 2.5f);
        if (u < 1.2f) shake[0] = 25.0f * side;
        else if (u >= 1.6f) shake[1] = flick > 0.0f ? 45.0f * flick * flick * flick * flick : 0.0f;
    }  // then settle upright
    const float len = sqrtf(x * x + y * y + z * z), g = GRAVITY_GAIN * GRAVITY_MPS2 * PX_PER_METER;
    // Apply device acceleration as opposing inertial acceleration, as in imu_read().
    out->gravity[0] = x / len * g - shake[0] * SHAKE_GAIN * PX_PER_METER;
    out->gravity[1] = y / len * g - shake[1] * SHAKE_GAIN * PX_PER_METER;
    out->gravity[2] = z / len * g - shake[2] * SHAKE_GAIN * PX_PER_METER;
    for (int i = 0; i < 3; ++i) out->omega[i] = out->alpha[i] = 0.0f;
}
