#ifndef CLOCK_SERVICE_H
#define CLOCK_SERVICE_H

/**
 * @file clock_service.h
 * @brief 统一时钟服务（纯头文件 + 单 .c 实现）
 *
 * 正常模式：返回 CLOCK_MONOTONIC 单调时间（微秒）。
 * 仿真模式：由外部注入时间戳（bag 回放时使用录制时间）。
 *
 * 用法：
 *   // 获取当前时间（替代各处散落的 clock_gettime）
 *   uint64_t now = clock_now_us();
 *
 *   // 切换到仿真时间（bag 回放）
 *   clock_set_sim_mode(true);
 *   clock_set_sim_time(recorded_timestamp_us);
 *   // ... replay loop ...
 *   clock_set_sim_mode(false);   // 恢复真实时间
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 获取当前时间（微秒）
 * 仿真模式下返回 clock_set_sim_time() 最后设置的值。
 * 正常模式返回 CLOCK_MONOTONIC 单调时间。
 */
uint64_t clock_now_us(void);

/**
 * 获取当前绝对时间（微秒，CLOCK_REALTIME）
 * 不受仿真模式影响，始终返回真实墙钟时间。
 * 用于训练样本时间戳、仪表盘时间戳等需要绝对时间的场景。
 */
uint64_t clock_now_realtime_us(void);

/**
 * 获取墙钟单调时间（微秒，CLOCK_MONOTONIC）
 * 不受仿真模式影响，始终返回真实墙钟单调时间。
 *
 * 用途：测量真实处理延迟。仿真模式下 clock_now_us() 返回按固定步长
 * 推进的逻辑时间（如 50ms/tick），同一 tick 内发布与分发的延迟恒为 0、
 * 跨 tick 才出现 50ms 倍数，使 total_latency_us / p50 / p99 等延迟统计
 * 失去意义。本函数提供不受仿真时钟污染的单调时间源，供 message_bus
 * 等延迟/寿命计算使用。
 */
uint64_t clock_now_monotonic_wall_us(void);

/**
 * 启用/禁用仿真时间模式
 * @param enable  true = 使用仿真时间；false = 使用真实 CLOCK_MONOTONIC
 */
void clock_set_sim_mode(bool enable);

/**
 * 设置仿真时间（仅在 sim_mode=true 时生效）
 * @param timestamp_us  目标时间（微秒，与录制时的 CLOCK_MONOTONIC 对齐）
 */
void clock_set_sim_time(uint64_t timestamp_us);

/**
 * 查询当前是否处于仿真时间模式
 */
bool clock_is_sim_mode(void);

/**
 * 在仿真模式下将逻辑时钟推进 delta_us 微秒。
 * 等价于 clock_set_sim_time(clock_now_us() + delta_us)。
 * 非仿真模式下调用本函数为空操作。
 *
 * 使用方式（仿真主循环）：
 *   clock_set_sim_mode(true);
 *   clock_set_sim_time(0);
 *   while (running) {
 *       // ... 执行一步仿真 ...
 *       clock_advance_us(10000);   // 逻辑时间 +10 ms
 *   }
 */
void clock_advance_us(uint64_t delta_us);

/**
 * 获取仿真步长（微秒）— 由 sim_world_node 初始化时配置。
 * 非仿真模式下返回 0。
 */
uint64_t clock_get_step_us(void);

/**
 * 设置仿真步长（微秒），供 sim_world_node 在 init 时调用。
 * @param step_us  每个 tick 推进的逻辑时间（例如 50000 = 50 ms @20 Hz）
 */
void clock_set_step_us(uint64_t step_us);

/* ── v2: 三模式时间语义 + 跨进程共享内存虚拟时钟 ─────────────────── */

typedef enum {
    CLOCK_MODE_SOFT_RT   = 0,  /* 跟随墙钟（加速比 <= 1），可测实时性 */
    CLOCK_MODE_FAST_TIME = 1,  /* 加速模式（10~100x），响应延迟不具实时有效性 */
    CLOCK_MODE_LOCK_STEP = 2,  /* 锁步模式：最高确定性，抹平实时性 */
} ClockMode;

/** 设置/查询当前时钟模式（与 clock_set_sim_mode 保持兼容）。 */
void clock_set_mode(ClockMode mode);
ClockMode clock_get_mode(void);
const char* clock_mode_name(ClockMode mode);

/**
 * 仅在 CLOCK_MODE_SOFT_RT 下返回 true。
 * 在 FAST_TIME / LOCK_STEP 下返回 false，防止将仿真响应延迟误用于实时性论证。
 */
bool clock_rt_latency_valid(void);

/** 查询当前仿真 tick 序号（每次 clock_advance_us 递增 1）。 */
uint64_t clock_get_tick_seq(void);

/** 设置/查询统一仿真随机种子（跨进程共享）。 */
void clock_set_seed(uint64_t seed);
uint64_t clock_get_seed(void);

/**
 * 挂载/创建跨进程共享内存虚拟时钟页（POSIX shm）。
 * @param shm_name  共享内存名称（如 "/flowengine_clock_1234"）
 * @param create    true=launcher 创建并初始化；false=子节点只读/读写挂载
 * @return 0 成功，-1 失败
 */
int clock_attach_shm(const char* shm_name, bool create);

/** 若环境变量 FLOW_CLOCK_SHM 非空，则自动挂载（create=false）。 */
int clock_attach_env_shm(void);

/** 解除共享内存虚拟时钟挂载（若本进程 create，则同时 shm_unlink）。 */
void clock_detach_shm(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOCK_SERVICE_H */
