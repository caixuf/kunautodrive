/**
 * pwm_map.c — ControlCmd → PWM 脉宽纯映射实现（详见 pwm_map.h）
 */

#include "pwm_map.h"

static int clamp_pulse(int us) {
    if (us < PWM_MIN_US) return PWM_MIN_US;
    if (us > PWM_MAX_US) return PWM_MAX_US;
    return us;
}

void pwm_map_control_cmd(double throttle, double brake, double steering_rad,
                         int e_stop,
                         double throttle_scale, double steering_scale,
                         int* esc_us, int* steer_us) {
    int esc;
    if (e_stop) {
        esc = PWM_CENTER_US;                       /* 紧急停转：中位 */
    } else if (brake > 0.01) {
        esc = (int)(PWM_CENTER_US - brake * throttle_scale);
    } else {
        esc = (int)(PWM_CENTER_US + throttle * throttle_scale);
    }

    double steer_norm = steering_rad / PWM_MAX_STEER_RAD;
    if (steer_norm > 1.0) steer_norm = 1.0;
    if (steer_norm < -1.0) steer_norm = -1.0;
    int steer = (int)(PWM_CENTER_US + steer_norm * steering_scale);

    if (esc_us)   *esc_us   = clamp_pulse(esc);
    if (steer_us) *steer_us = clamp_pulse(steer);
}
