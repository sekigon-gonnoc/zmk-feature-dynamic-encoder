/* SPDX-License-Identifier: MIT */

#ifndef ZMK_DYNAMIC_ENCODER_SENSOR_VALUE_H
#define ZMK_DYNAMIC_ENCODER_SENSOR_VALUE_H

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#define DYNAMIC_ENCODER_MICRODEGREES_PER_ROTATION 360000000LL

static inline int dynamic_encoder_sensor_value_to_triggers(bool tick_input, int32_t val1,
                                                           int32_t val2,
                                                           uint32_t triggers_per_rotation,
                                                           int64_t *accumulator,
                                                           int32_t *triggers) {
    if (!accumulator || !triggers) {
        return -EINVAL;
    }

    if (tick_input) {
        *triggers = val2;
        return 0;
    }

    if (triggers_per_rotation == 0) {
        return -EINVAL;
    }

    int64_t delta = (int64_t)val1 * 1000000LL + val2;
    *accumulator += delta * triggers_per_rotation;
    int64_t steps = *accumulator / DYNAMIC_ENCODER_MICRODEGREES_PER_ROTATION;
    *accumulator %= DYNAMIC_ENCODER_MICRODEGREES_PER_ROTATION;
    if (steps > INT32_MAX || steps < INT32_MIN) {
        return -ERANGE;
    }
    *triggers = (int32_t)steps;
    return 0;
}

#endif /* ZMK_DYNAMIC_ENCODER_SENSOR_VALUE_H */
