/*
 * Interface between a camera sensor model and the camera interface (DVP
 * receiver) of a SoC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/misc/dvp_sensor.h"

Object *dvp_sensor_find(void)
{
    return object_resolve_path_type("", TYPE_DVP_SENSOR, NULL);
}

static const TypeInfo dvp_sensor_info = {
    .name = TYPE_DVP_SENSOR,
    .parent = TYPE_INTERFACE,
    .class_size = sizeof(DvpSensorClass),
};

static void dvp_sensor_register_types(void)
{
    type_register_static(&dvp_sensor_info);
}

type_init(dvp_sensor_register_types)
