/* SPDX-License-Identifier: MIT */

#include <zmk/dynamic_encoder/encoder.h>

struct zmk_behavior_binding *
dynamic_sensor_binding_lookup(uint8_t sensor_index, uint8_t layer_id,
                              struct zmk_behavior_binding *static_binding) {
    STRUCT_SECTION_FOREACH(dynamic_encoder_binding_provider, provider) {
        if (provider->lookup) {
            struct zmk_behavior_binding *binding =
                provider->lookup(sensor_index, layer_id, static_binding);
            if (binding) {
                return binding;
            }
        }
    }
    return static_binding;
}
