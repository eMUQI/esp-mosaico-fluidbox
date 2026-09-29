#pragma once

#include "sim.h"

// Scripted acceleration sequence for tools/preview.py: upright, tilt, flat,
// circular tilt, upright, shake and settle. Angular velocity and acceleration
// remain zero; the firmware uses live IMU input instead of this sequence.

#define SCENARIO_SECONDS 24.0f

// Fills the forces for time t (seconds, wrapped to the scenario length).
void scenario_forces(float t, sim_forces_t *out);
