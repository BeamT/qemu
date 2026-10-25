/*
 * Interface between a camera sensor model and the camera interface (DVP
 * receiver) of a SoC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "qom/object.h"

#define TYPE_DVP_SENSOR "dvp-sensor"

typedef struct DvpSensorClass DvpSensorClass;
DECLARE_CLASS_CHECKERS(DvpSensorClass, DVP_SENSOR, TYPE_DVP_SENSOR)

struct DvpSensorClass {
    InterfaceClass parent_class;

    /*
     * The bytes of the next frame, as the sensor puts them on its data
     * lines between two VSYNCs, or NULL while it is not streaming. The
     * caller owns a reference to the array.
     */
    GByteArray *(*next_frame)(Object *sensor);
};

/* The sensor of the machine, or NULL if there is none or more than one */
Object *dvp_sensor_find(void);
