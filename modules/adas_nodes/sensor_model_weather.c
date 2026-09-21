/**
 * sensor_model_weather.c — 天气/可见度衰减纯逻辑实现（详见 sensor_model_weather.h）
 */

#include "sensor_model_weather.h"

#include <string.h>

double sensor_model_camera_visibility(double visibility_m) {
    double factor = visibility_m / 200.0;
    if (!(factor >= 0.1)) factor = 0.1;   /* 兼收 NaN / 负值 */
    if (factor > 1.0) factor = 1.0;
    return factor;
}

double sensor_model_weather_attenuation(double visibility_m, const char* weather) {
    double att = 1.0 - sensor_model_camera_visibility(visibility_m);
    if (weather && (strstr(weather, "rain") || strstr(weather, "fog") ||
                    strstr(weather, "snow"))) {
        if (att < 0.3) att = 0.3;   /* 降水/雾霾散射地板 */
    }
    if (att > 1.0) att = 1.0;
    if (att < 0.0) att = 0.0;
    return att;
}
