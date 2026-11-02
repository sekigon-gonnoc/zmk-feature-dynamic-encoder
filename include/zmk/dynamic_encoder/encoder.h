/* SPDX-License-Identifier: MIT */

#pragma once

#include <drivers/behavior.h>
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/iterable_sections.h>

/* Feature modules can provide runtime sensor bindings for their own sources. */
typedef struct zmk_behavior_binding *(*dynamic_encoder_binding_provider_fn)(
    uint8_t sensor_index, uint8_t layer_id, struct zmk_behavior_binding *static_binding);

struct dynamic_encoder_binding_provider {
    dynamic_encoder_binding_provider_fn lookup;
};

#define DYNAMIC_ENCODER_BINDING_PROVIDER_DEFINE(name, lookup_fn)                                   \
    STRUCT_SECTION_ITERABLE(dynamic_encoder_binding_provider, name) = {.lookup = lookup_fn}

/* Called by keymap.c after reading the Devicetree sensor binding. */
struct zmk_behavior_binding *
dynamic_sensor_binding_lookup(uint8_t sensor_index, uint8_t layer_id,
                              struct zmk_behavior_binding *static_binding);
