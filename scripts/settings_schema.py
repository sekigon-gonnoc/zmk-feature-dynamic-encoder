# SPDX-License-Identifier: MIT
"""Devicetree-to-settings-schema adapter for runtime Encoder records."""

import re
from types import SimpleNamespace


def _write_binding(record, offset, binding):
    if binding is None:
        return
    identity, param1, param2 = binding
    record[offset:offset + 2] = [identity & 0xFF, identity >> 8]
    for index, param in enumerate((param1, param2)):
        record[offset + 2 + index * 4:offset + 6 + index * 4] = [
            (param >> shift) & 0xFF for shift in (0, 8, 16, 24)
        ]


def _default_record(context, node, source_name):
    field = f"encoder source {source_name}"
    cw = context.default_behavior_binding(node, "default-cw-binding", field)
    ccw = context.default_behavior_binding(node, "default-ccw-binding", field)
    if cw is None and ccw is None:
        return [1] + [0] * 23
    record = [1, 1, 0 if cw is None else 2, 0 if ccw is None else 2] + [0] * 20
    _write_binding(record, 4, cw)
    _write_binding(record, 14, ccw)
    return record


def _triggers_per_rotation_default(context, sensor_index):
    value = context.node_value
    configs = [
        node for node in context.nodes
        if node.status == "okay" and "zmk,keymap-sensors" in set(node.compats)
    ]
    if len(configs) > 1:
        raise ValueError("runtime encoder sources need exactly one zmk,keymap-sensors")

    config_values = getattr(context, "config_values", {})
    fallback = config_values.get(
        "CONFIG_ZMK_KEYMAP_SENSORS_DEFAULT_TRIGGERS_PER_ROTATION", 1
    )
    config = configs[0] if configs else None
    result = value(config, "triggers-per-rotation", fallback) if config else fallback
    children = list(config.children.values()) if config else []
    if children and sensor_index < len(children):
        result = value(children[sensor_index], "triggers-per-rotation", result)
    if type(result) is not int or not 1 <= result <= 0xFFFF:
        raise ValueError(
            f"sensor index {sensor_index}: triggers-per-rotation default must be in 1..65535"
        )
    return result


def _triggers_per_rotation_setting(default):
    props = {
        "setting-name": "triggers-per-rotation",
        "label": "Triggers per rotation",
        "help": (
            "Number of triggers per full rotation for angle-valued sensor input. "
            "Legacy tick input triggers once per tick and ignores this setting."
        ),
        "value-type": "u16",
        "default-int": default,
        "minimum": 1,
        "maximum": 0xFFFF,
        "step": 1,
        "widget": "number",
        "unit": "triggers/rotation",
        "persistent": True,
        "on-change": "dynamic_encoder_triggers_per_rotation_changed",
    }
    return SimpleNamespace(
        status="okay",
        compats={"sekigon,dynamic-setting"},
        props={key: SimpleNamespace(val=value) for key, value in props.items()},
        children={},
    )


def contribute(context):
    value = context.node_value
    sources = [n for n in context.nodes
               if n.status == "okay" and "sekigon,dynamic-encoder-source" in set(n.compats)]
    if not sources:
        return []
    keymaps = [n for n in context.nodes if n.status == "okay" and "zmk,keymap" in set(n.compats)]
    if len(keymaps) != 1:
        raise ValueError("runtime encoder sources need exactly one zmk,keymap")
    layers = list(keymaps[0].children.values())
    if not context.layer_reordering:
        layers = [layer for layer in layers if layer.status == "okay"]
    if not layers:
        raise ValueError("runtime encoder sources need at least one enabled keymap layer")

    used_indices = set()
    groups = []
    for source in sources:
        name = context.check_name(value(source, "source-name"), "source-name")
        sensor_index = value(source, "sensor-index")
        input_mode = value(source, "input-mode")
        sensor = value(source, "sensor")
        target_role = value(source, "target-role")
        label = value(source, "label")
        if type(sensor_index) is not int or sensor_index < 0:
            raise ValueError(f"encoder source {name}: sensor-index must be nonnegative")
        if input_mode not in ("tick", "angle"):
            raise ValueError(f"encoder source {name}: input-mode must be 'tick' or 'angle'")
        if sensor_index in used_indices:
            raise ValueError(f"encoder source sensor-index {sensor_index} is declared more than once")
        used_indices.add(sensor_index)
        if sensor is None or not target_role or not label:
            raise ValueError(f"encoder source {name}: sensor, target-role, and label are required")
        labels = getattr(source, "labels", [])
        if not labels or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", labels[0]):
            raise ValueError(f"encoder source {name}: source needs a devicetree label")
        record = _default_record(context, source, name)
        triggers_per_rotation = (
            _triggers_per_rotation_default(context, sensor_index) if input_mode == "angle" else None
        )
        if len(record) != 24:
            raise ValueError(f"encoder source {name}: record-default must contain 24 bytes")
        for layer_id, _layer in enumerate(layers):
            instance = f"{name}-i{sensor_index}-l{layer_id}"
            groups.append(context.make_group({
                "module-name": "encoder", "group-name": "assignment", "instance-name": instance,
                "label": f"{label} index {sensor_index} - layer {layer_id}",
                "scope": "shared", "format-version": 1, "target-role": target_role,
                "actual-device": source, "record-type": "encoder-v1", "record-max-length": 24,
                "record-default": record, "record-on-change": "dynamic_encoder_record_changed",
            }))
        if triggers_per_rotation is not None:
            tpr_group = context.make_group({
                "module-name": "encoder", "group-name": "triggers",
                "instance-name": f"{name}-i{sensor_index}",
                "label": f"{label} index {sensor_index} rotation",
                "scope": "shared", "format-version": 1, "target-role": target_role,
                "actual-device": source,
            })
            tpr_group.children = {
                "triggers_per_rotation": _triggers_per_rotation_setting(triggers_per_rotation),
            }
            groups.append(tpr_group)
    return groups
