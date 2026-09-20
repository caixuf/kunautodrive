#pragma once
/* flowsim 时间步唯一事实源。flowsim_node.cpp 与 scene_events.cpp 必须都包含本文件，
 * 禁止各自 #define（曾因两份定义不一致导致红绿灯编排时长差 3 倍）。 */
#define FLOWSIM_FREQUENCY_HZ   60.0
#define FLOWSIM_DT_SEC         (1.0 / FLOWSIM_FREQUENCY_HZ)   /* ~0.0167s */
#define FLOWSIM_DT_US          ((uint64_t)(FLOWSIM_DT_SEC * 1e6))  /* 16666 */
