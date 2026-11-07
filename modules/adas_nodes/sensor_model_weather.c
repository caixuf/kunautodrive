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

/* 大小写不敏感子串匹配（weather/lighting 串来自场景 JSON，写法不统一）。 */
static int contains_ci(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return 0;
    const size_t nlen = strlen(needle);
    for (const char* p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               (p[i] | 0x20) == (needle[i] | 0x20)) {
            i++;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

SensorCameraDegradation sensor_model_camera_degradation(
    double visibility_m, const char* weather, const char* lighting) {
    SensorCameraDegradation d;
    d.visibility = sensor_model_camera_visibility(visibility_m);

    if (contains_ci(weather, "fog"))        d.contrast = 0.45;
    else if (contains_ci(weather, "snow"))  d.contrast = 0.65;
    else if (contains_ci(weather, "rain"))  d.contrast = 0.75;
    else                                    d.contrast = 1.0;

    if (contains_ci(lighting, "night"))      d.lighting = 0.30;
    else if (contains_ci(lighting, "dusk"))  d.lighting = 0.65;
    else                                     d.lighting = 1.0;

    /* dusk 的逆光/眩光：只在黄昏给额外损失，夜间不叠（避免双重惩罚）。 */
    d.glare = contains_ci(lighting, "dusk") ? 0.15 : 0.0;

    double eff = d.visibility * d.contrast * d.lighting - d.glare;
    if (!(eff >= 0.05)) eff = 0.05;   /* 兼收 NaN / 负值 */
    if (eff > 1.0) eff = 1.0;
    d.effective = eff;
    return d;
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
