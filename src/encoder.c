/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT sekigon_dynamic_behavior_runtime_encoder

#include <errno.h>
#include <limits.h>
#include <string.h>

#include <drivers/behavior.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/dynamic_settings/store.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/sensors.h>
#include <zmk/virtual_key_position.h>

#include "settings_schema_generated.h"
#include <zmk/dynamic_encoder/encoder.h>

#include "sensor_value.h"

LOG_MODULE_REGISTER(dynamic_encoder, CONFIG_ZMK_LOG_LEVEL);

#define ENCODER_RECORD_SIZE 24
#define ENCODER_RECORD_VERSION 1
#define ENCODER_ACTION_TRANSPARENT 0
#define ENCODER_ACTION_NONE 1
#define ENCODER_ACTION_BEHAVIOR 2
#define ENCODER_STEP_LIMIT 32
#define ENCODER_PENDING_TASKS 8
#define ENCODER_QUEUE_RETRY_MS 5

struct encoder_slot_config {
    uint8_t sensor_index;
    uint8_t layer_id;
};

struct encoder_source_config {
    uint8_t sensor_index;
    bool tick_input;
    const struct device *device;
};

struct encoder_direction {
    uint8_t action;
    struct zmk_behavior_binding binding;
};

struct encoder_slot_data {
    struct encoder_slot_config config;
    uint8_t record[ENCODER_RECORD_SIZE];
    struct encoder_direction cw;
    struct encoder_direction ccw;
    int64_t accumulator;
    int32_t pending_triggers;
    int32_t triggers;
    bool initialized;
    bool record_resolved;
};

struct encoder_task {
    struct encoder_slot_data *slot;
    struct zmk_behavior_binding_event event;
    struct zmk_behavior_binding binding;
    uint8_t remaining;
    bool press_queued;
};

static struct encoder_task pending_tasks[ENCODER_PENDING_TASKS];
static uint8_t pending_head;
static uint8_t pending_count;
static struct k_work_delayable encoder_runner;
K_MUTEX_DEFINE(encoder_mutex);

static void encoder_runner_work(struct k_work *work);

static int encoder_accept_data(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event,
                               const struct zmk_sensor_config *sensor_config,
                               size_t channel_data_size,
                               const struct zmk_sensor_channel_data *channel_data);
static int encoder_process(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event,
                           enum behavior_sensor_binding_process_mode mode);

static const struct behavior_driver_api encoder_behavior_api = {
    .sensor_binding_accept_data = encoder_accept_data,
    .sensor_binding_process = encoder_process,
};

static int encoder_adapter_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

BEHAVIOR_DT_INST_DEFINE(0, encoder_adapter_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &encoder_behavior_api);

#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT sekigon_dynamic_encoder_source

#define ENCODER_SOURCE_CONFIG(node_id)                                                             \
    {                                                                                              \
        .sensor_index = DT_PROP(node_id, sensor_index),                                            \
        .tick_input = (DT_ENUM_IDX(node_id, input_mode) == 0),                                     \
        .device = DEVICE_DT_GET(node_id),                                                          \
    },
#define ENCODER_SOURCE_ASSERT(node_id)                                                             \
    BUILD_ASSERT(DT_PROP(node_id, sensor_index) < ZMK_KEYMAP_SENSORS_LEN,                          \
                 "encoder sensor-index is outside zmk,keymap-sensors");                            \
    BUILD_ASSERT(DT_SAME_NODE(DT_PHANDLE(node_id, sensor),                                         \
                              ZMK_KEYMAP_SENSORS_BY_IDX(DT_PROP(node_id, sensor_index))),          \
                 "encoder sensor phandle and sensor-index disagree");

static int encoder_source_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}
#define ENCODER_SOURCE_DEVICE(node_id)                                                             \
    DEVICE_DT_DEFINE(node_id, encoder_source_init, NULL, NULL, NULL, POST_KERNEL, 99, NULL);

#define ENCODER_SOURCE_COUNT DT_NUM_INST_STATUS_OKAY(sekigon_dynamic_encoder_source)
#define ENCODER_SLOT_COUNT (ENCODER_SOURCE_COUNT * ZMK_KEYMAP_LAYERS_LEN)
#define ENCODER_STORAGE_SLOT_COUNT (ENCODER_SLOT_COUNT > 0 ? ENCODER_SLOT_COUNT : 1)
#if ENCODER_SOURCE_COUNT > 0
static const struct encoder_source_config encoder_sources[] = {
    DT_FOREACH_STATUS_OKAY(sekigon_dynamic_encoder_source, ENCODER_SOURCE_CONFIG)};
#else
static const struct encoder_source_config encoder_sources[1] = {{0}};
#endif
DT_FOREACH_STATUS_OKAY(sekigon_dynamic_encoder_source, ENCODER_SOURCE_ASSERT)
DT_FOREACH_STATUS_OKAY(sekigon_dynamic_encoder_source, ENCODER_SOURCE_DEVICE)

static struct encoder_slot_data slot_data[ENCODER_STORAGE_SLOT_COUNT];
static uint16_t source_triggers_per_rotation[ENCODER_STORAGE_SLOT_COUNT];

static struct encoder_slot_data *slot_at(size_t source_index, uint8_t layer_id) {
    if (source_index >= ENCODER_SOURCE_COUNT || layer_id >= ZMK_KEYMAP_LAYERS_LEN) {
        return NULL;
    }
    struct encoder_slot_data *slot = &slot_data[source_index * ZMK_KEYMAP_LAYERS_LEN + layer_id];
    if (!slot->initialized) {
        slot->config = (struct encoder_slot_config){
            .sensor_index = encoder_sources[source_index].sensor_index,
            .layer_id = layer_id,
        };
        slot->record[0] = ENCODER_RECORD_VERSION;
        slot->record[1] = 0;
        slot->initialized = true;
    }
    return slot;
}

static int source_index_for_sensor(uint8_t sensor_index) {
    for (size_t i = 0; i < ENCODER_SOURCE_COUNT; i++) {
        if (encoder_sources[i].sensor_index == sensor_index) {
            return i;
        }
    }
    return -1;
}

static int source_index_for_device(const struct device *device) {
    for (size_t i = 0; i < ENCODER_SOURCE_COUNT; i++) {
        if (encoder_sources[i].device == device) {
            return i;
        }
    }
    return -1;
}

static struct zmk_behavior_binding adapter_binding = {
    .behavior_dev = DEVICE_DT_NAME(DT_INST(0, sekigon_dynamic_behavior_runtime_encoder)),
};

static uint16_t behavior_identity(const char *name) {
    uint32_t hash = 2166136261u;
    for (const uint8_t *p = (const uint8_t *)name; *p; p++) {
        hash = (hash ^ *p) * 16777619u;
    }
    hash ^= hash >> 16;
    return (uint16_t)hash;
}

static const struct device *resolve_behavior(uint16_t identity) {
    const struct device *match = NULL;
    STRUCT_SECTION_FOREACH(zmk_behavior_ref, ref) {
        if (!ref->device) {
            continue;
        }
        bool identity_matches = behavior_identity(ref->device->name) == identity;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
        if (ref->metadata.display_name &&
            behavior_identity(ref->metadata.display_name) == identity) {
            identity_matches = true;
        }
#endif
        if (!identity_matches) {
            continue;
        }
        if (match && match != ref->device) {
            return NULL;
        }
        match = ref->device;
    }
    if (!match || !device_is_ready(match)) {
        return NULL;
    }
    return match;
}

static int validate_record(const uint8_t *record, size_t length, struct encoder_slot_data *slot,
                           bool resolve_bindings) {
    if (!record || length != ENCODER_RECORD_SIZE || record[0] != ENCODER_RECORD_VERSION ||
        record[1] > 1 || record[2] > ENCODER_ACTION_BEHAVIOR ||
        record[3] > ENCODER_ACTION_BEHAVIOR) {
        return -EINVAL;
    }
    if (record[1] == 0) {
        for (size_t i = 2; i < length; i++) {
            if (record[i]) {
                return -EINVAL;
            }
        }
        return 0;
    }
    for (int direction = 0; direction < 2; direction++) {
        const size_t offset = direction == 0 ? 4 : 14;
        const uint8_t action = record[2 + direction];
        const uint16_t identity = sys_get_le16(record + offset);
        const uint32_t param1 = sys_get_le32(record + offset + 2);
        const uint32_t param2 = sys_get_le32(record + offset + 6);
        if (action != ENCODER_ACTION_BEHAVIOR) {
            if (identity || param1 || param2) {
                return -EINVAL;
            }
            continue;
        }
        struct encoder_direction *target = direction == 0 ? &slot->cw : &slot->ccw;
        target->action = action;
        if (resolve_bindings) {
            const struct device *behavior = resolve_behavior(identity);
            if (!behavior) {
                return -ENOENT;
            }
            target->binding = (struct zmk_behavior_binding){
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS)
                .local_id = zmk_behavior_get_local_id(behavior->name),
#endif
                .behavior_dev = behavior->name,
                .param1 = param1,
                .param2 = param2,
            };
            if (zmk_behavior_validate_binding(&target->binding)) {
                return -EINVAL;
            }
        }
    }
    return 0;
}

dynamic_setting_record_on_change_fn dynamic_encoder_record_changed;

static bool slot_indices_for_group(const struct dynamic_setting_group_descriptor *group,
                                   const void *context,
                                   dynamic_setting_record_on_change_fn *callback,
                                   size_t *source_index, uint8_t *layer_id) {
    const struct device *sensor = context ? context : group->context;
    if (!sensor) {
        return false;
    }
    size_t source = ENCODER_SOURCE_COUNT;
    for (size_t i = 0; i < ENCODER_SOURCE_COUNT; i++) {
        if (encoder_sources[i].device == sensor) {
            source = i;
            break;
        }
    }
    if (source == ENCODER_SOURCE_COUNT) {
        return false;
    }

    uint8_t layer = 0;
    for (size_t i = 0; i < dynamic_setting_group_count; i++) {
        const struct dynamic_setting_group_descriptor *candidate = &dynamic_setting_groups[i];
        if (candidate == group) {
            *source_index = source;
            *layer_id = layer;
            return layer < ZMK_KEYMAP_LAYERS_LEN;
        }
        if (candidate->context == sensor && candidate->record_on_change == callback) {
            layer++;
        }
    }
    return false;
}

int dynamic_encoder_record_changed(const struct dynamic_setting_group_descriptor *group,
                                   const uint8_t *record, size_t length,
                                   enum dynamic_setting_change_reason reason, const void *context) {
    size_t source_index;
    uint8_t layer_id;
    if (!slot_indices_for_group(group, context, dynamic_encoder_record_changed, &source_index,
                                &layer_id)) {
        return -EINVAL;
    }
    k_mutex_lock(&encoder_mutex, K_FOREVER);
    struct encoder_slot_data *slot = slot_at(source_index, layer_id);
    for (uint8_t i = 0; i < pending_count; i++) {
        if (pending_tasks[(pending_head + i) % ENCODER_PENDING_TASKS].slot == slot) {
            k_mutex_unlock(&encoder_mutex);
            return -EBUSY;
        }
    }
    struct encoder_slot_data staged = *slot;
    memset(&staged.cw, 0, sizeof(staged.cw));
    memset(&staged.ccw, 0, sizeof(staged.ccw));
    const bool resolve_bindings = reason != DYNAMIC_SETTING_CHANGE_BOOT_LOAD;
    int err = validate_record(record, length, &staged, resolve_bindings);
    if (err) {
        k_mutex_unlock(&encoder_mutex);
        return err;
    }
    memcpy(slot->record, record, length);
    slot->cw = staged.cw;
    slot->ccw = staged.ccw;
    slot->record_resolved = resolve_bindings || record[1] == 0;
    slot->accumulator = 0;
    slot->triggers = 0;
    slot->pending_triggers = 0;
    k_mutex_unlock(&encoder_mutex);
    return 0;
}

dynamic_setting_on_change_fn dynamic_encoder_triggers_per_rotation_changed;

int dynamic_encoder_triggers_per_rotation_changed(
    const struct dynamic_setting_descriptor *descriptor, const struct dynamic_setting_value *value,
    enum dynamic_setting_change_reason reason, const void *context) {
    ARG_UNUSED(reason);
    if (!descriptor || !value || descriptor->type != DYNAMIC_SETTING_TYPE_U16 ||
        value->type != DYNAMIC_SETTING_TYPE_U16 || value->length != sizeof(uint16_t) || !context) {
        return -EINVAL;
    }

    const int source_index = source_index_for_device(context);
    const uint16_t triggers_per_rotation = sys_get_le16(value->data);
    if (source_index < 0 || !triggers_per_rotation) {
        return -EINVAL;
    }

    k_mutex_lock(&encoder_mutex, K_FOREVER);
    for (uint8_t i = 0; i < pending_count; i++) {
        const struct encoder_task *task =
            &pending_tasks[(pending_head + i) % ENCODER_PENDING_TASKS];
        if (task->slot->config.sensor_index == encoder_sources[source_index].sensor_index) {
            k_mutex_unlock(&encoder_mutex);
            return -EBUSY;
        }
    }
    source_triggers_per_rotation[source_index] = triggers_per_rotation;
    for (uint8_t layer_id = 0; layer_id < ZMK_KEYMAP_LAYERS_LEN; layer_id++) {
        struct encoder_slot_data *slot = slot_at(source_index, layer_id);
        if (slot) {
            slot->accumulator = 0;
            slot->triggers = 0;
            slot->pending_triggers = 0;
        }
    }
    k_mutex_unlock(&encoder_mutex);
    return 0;
}

struct zmk_behavior_binding *dynamic_encoder_lookup(uint8_t sensor_index, uint8_t layer_id,
                                                    struct zmk_behavior_binding *static_binding) {
    k_mutex_lock(&encoder_mutex, K_FOREVER);
    int source_index = source_index_for_sensor(sensor_index);
    struct encoder_slot_data *slot = source_index < 0 ? NULL : slot_at(source_index, layer_id);
    if (slot && slot->record[1] == 1) {
        if (!slot->record_resolved) {
            struct encoder_slot_data staged = *slot;
            memset(&staged.cw, 0, sizeof(staged.cw));
            memset(&staged.ccw, 0, sizeof(staged.ccw));
            if (validate_record(slot->record, sizeof(slot->record), &staged, true) != 0) {
                k_mutex_unlock(&encoder_mutex);
                return static_binding;
            }
            slot->cw = staged.cw;
            slot->ccw = staged.ccw;
            slot->record_resolved = true;
        }
        k_mutex_unlock(&encoder_mutex);
        return &adapter_binding;
    }
    k_mutex_unlock(&encoder_mutex);
    return static_binding;
}

static struct zmk_behavior_binding *
runtime_encoder_binding_provider_lookup(uint8_t sensor_index, uint8_t layer_id,
                                        struct zmk_behavior_binding *static_binding) {
    const int source_index = source_index_for_sensor(sensor_index);
    if (source_index < 0 || layer_id >= ZMK_KEYMAP_LAYERS_LEN) {
        return NULL;
    }
    k_mutex_lock(&encoder_mutex, K_FOREVER);
    struct encoder_slot_data *slot = slot_at(source_index, layer_id);
    const bool configured = slot && slot->record[1] == 1;
    k_mutex_unlock(&encoder_mutex);
    return configured ? dynamic_encoder_lookup(sensor_index, layer_id, static_binding) : NULL;
}

DYNAMIC_ENCODER_BINDING_PROVIDER_DEFINE(runtime_encoder_binding_provider,
                                        runtime_encoder_binding_provider_lookup);

static struct encoder_slot_data *slot_for_event(struct zmk_behavior_binding_event event) {
    uint8_t sensor_index = ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(event.position);
    int source_index = source_index_for_sensor(sensor_index);
    if (source_index < 0) {
        return NULL;
    }
    k_mutex_lock(&encoder_mutex, K_FOREVER);
    struct encoder_slot_data *slot = slot_at(source_index, event.layer);
    k_mutex_unlock(&encoder_mutex);
    return slot;
}

static int encoder_accept_data(struct zmk_behavior_binding *binding,
                               struct zmk_behavior_binding_event event,
                               const struct zmk_sensor_config *sensor_config,
                               size_t channel_data_size,
                               const struct zmk_sensor_channel_data *channel_data) {
    ARG_UNUSED(binding);
    if (channel_data_size != 1 || channel_data[0].channel != SENSOR_CHAN_ROTATION) {
        return -ENOTSUP;
    }
    struct encoder_slot_data *slot = slot_for_event(event);
    if (!slot) {
        return -ENOENT;
    }
    k_mutex_lock(&encoder_mutex, K_FOREVER);
    if (slot->record[1] != 1) {
        k_mutex_unlock(&encoder_mutex);
        return -ENOENT;
    }
    const int source_index = source_index_for_sensor(slot->config.sensor_index);
    if (source_index < 0) {
        k_mutex_unlock(&encoder_mutex);
        return -ENOENT;
    }
    uint16_t triggers_per_rotation = source_triggers_per_rotation[source_index];
    if (!triggers_per_rotation) {
        triggers_per_rotation = sensor_config->triggers_per_rotation;
        if (!triggers_per_rotation) {
            triggers_per_rotation = 1;
        }
        source_triggers_per_rotation[source_index] = triggers_per_rotation;
    }
    int err = dynamic_encoder_sensor_value_to_triggers(
        encoder_sources[source_index].tick_input, channel_data[0].value.val1,
        channel_data[0].value.val2, triggers_per_rotation, &slot->accumulator, &slot->triggers);
    if (err) {
        k_mutex_unlock(&encoder_mutex);
        return err;
    }
    k_mutex_unlock(&encoder_mutex);
    return 0;
}

static int encoder_process(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event,
                           enum behavior_sensor_binding_process_mode mode) {
    ARG_UNUSED(binding);
    struct encoder_slot_data *slot = slot_for_event(event);
    if (!slot) {
        return ZMK_BEHAVIOR_TRANSPARENT;
    }
    k_mutex_lock(&encoder_mutex, K_FOREVER);
    if (slot->record[1] != 1) {
        k_mutex_unlock(&encoder_mutex);
        return ZMK_BEHAVIOR_TRANSPARENT;
    }
    if (mode != BEHAVIOR_SENSOR_BINDING_PROCESS_MODE_TRIGGER) {
        slot->triggers = 0;
        k_mutex_unlock(&encoder_mutex);
        return ZMK_BEHAVIOR_TRANSPARENT;
    }
    int32_t steps = slot->triggers;
    slot->triggers = 0;
    if (!steps) {
        k_mutex_unlock(&encoder_mutex);
        return ZMK_BEHAVIOR_TRANSPARENT;
    }
    if (steps > ENCODER_STEP_LIMIT || steps < -ENCODER_STEP_LIMIT) {
        k_mutex_unlock(&encoder_mutex);
        return -EOVERFLOW;
    }
    const struct encoder_direction *direction = steps > 0 ? &slot->cw : &slot->ccw;
    uint32_t count = (uint32_t)(steps > 0 ? steps : -steps);
    if (direction->action == ENCODER_ACTION_TRANSPARENT) {
        k_mutex_unlock(&encoder_mutex);
        return ZMK_BEHAVIOR_TRANSPARENT;
    }
    if (direction->action == ENCODER_ACTION_NONE) {
        k_mutex_unlock(&encoder_mutex);
        return ZMK_BEHAVIOR_OPAQUE;
    }
    event.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL;
    if (pending_count) {
        uint8_t last_index = (pending_head + pending_count - 1) % ENCODER_PENDING_TASKS;
        struct encoder_task *last = &pending_tasks[last_index];
        if (last->slot == slot && last->binding.behavior_dev == direction->binding.behavior_dev &&
            last->binding.param1 == direction->binding.param1 &&
            last->binding.param2 == direction->binding.param2 &&
            (uint32_t)last->remaining + count <= UINT8_MAX) {
            last->remaining += count;
            k_mutex_unlock(&encoder_mutex);
            return ZMK_BEHAVIOR_OPAQUE;
        }
    }
    if (pending_count == ENCODER_PENDING_TASKS) {
        k_mutex_unlock(&encoder_mutex);
        return -ENOSPC;
    }
    uint8_t tail = (pending_head + pending_count) % ENCODER_PENDING_TASKS;
    pending_tasks[tail] = (struct encoder_task){
        .slot = slot,
        .event = event,
        .binding = direction->binding,
        .remaining = count,
    };
    pending_count++;
    k_mutex_unlock(&encoder_mutex);
    k_work_schedule(&encoder_runner, K_NO_WAIT);
    return ZMK_BEHAVIOR_OPAQUE;
}

static void encoder_runner_work(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&encoder_mutex, K_FOREVER);
    if (!pending_count) {
        k_mutex_unlock(&encoder_mutex);
        return;
    }
    struct encoder_task *task = &pending_tasks[pending_head];
    int err;
    if (!task->press_queued) {
        err = zmk_behavior_queue_add(&task->event, task->binding, true, 5);
        if (err) {
            k_mutex_unlock(&encoder_mutex);
            k_work_schedule(&encoder_runner, K_MSEC(ENCODER_QUEUE_RETRY_MS));
            return;
        }
        task->press_queued = true;
    }
    err = zmk_behavior_queue_add(&task->event, task->binding, false, 0);
    if (err) {
        k_mutex_unlock(&encoder_mutex);
        k_work_schedule(&encoder_runner, K_MSEC(ENCODER_QUEUE_RETRY_MS));
        return;
    }
    task->press_queued = false;
    task->remaining--;
    if (!task->remaining) {
        memset(task, 0, sizeof(*task));
        pending_head = (pending_head + 1) % ENCODER_PENDING_TASKS;
        pending_count--;
    }
    bool more = pending_count > 0;
    k_mutex_unlock(&encoder_mutex);
    if (more) {
        k_work_schedule(&encoder_runner, K_MSEC(1));
    }
}

static int encoder_runner_init(void) {
    k_work_init_delayable(&encoder_runner, encoder_runner_work);
    return 0;
}

SYS_INIT(encoder_runner_init, APPLICATION, 0);
