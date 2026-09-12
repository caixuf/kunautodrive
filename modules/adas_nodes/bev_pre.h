/**
 * bev_pre.h — BEV 前处理：世界系障碍物真值 → NCHW 特征图（纯函数，header-only）
 *
 * 供 BEV 检测节点把「ge7 raw 感知输入」栅格化成模型能吃的中 BEV 特征。
 * 与执行模型无关、无状态，便于单测（单测见 bev_pre 的兄弟测试）。
 *
 * 输入契约（与 perception_node.cpp 读 vehicle/state 的字段一致）：
 *   - 障碍物 xe/ye 为世界系，速度 vx/vy 为世界系
 *   - ego 位置 (ego_x,ego_y) + 航向 ego_heading（rad）
 *   - 类型 num(typeC)：0=车,1=行人,2=自行车,3=施工/静态（作通道编码）
 *
 * 输出 NCHW 张量（row-major，通道优先）：
 *   shape = [1, C, H, W]，其中
 *     - H = 前后网格数（正 x = 车头方向），W = 左右网格数（正 y = 车左）
 *     - 通道 C（默认 4）：
 *         ch0 = 占据（该格存在目标 → 1.0）
 *         ch1 = 速度幅值（0..v_scale 归一化到 0..1）
 *         ch2 = 类型编码（car/ego 类目标高亮，作为弱先验）
 *         ch3 = 高度/存在度渐变（中心往周围衰减，帮助 heatmap 学习）
 *
 * 坐标系：x 向前、y 向左。网格原点位于 ego 中心前（-range_x）、左（+range_x/2）处，
 * 即 grid[iy][ix] 覆盖世界车体系坐标 (x=-range_x + ix*res, y=+range_y - iy*res)。
 * 每个障碍物按 bbox 落入的格子叠加占据值与速度/类型，保证光栅化后的目标可检测。
 */
#ifndef FLOWSIM_BEV_PRE_H
#define FLOWSIM_BEV_PRE_H

#include <stdint.h>
#include <stddef.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 类型编码（与 perception OBJ_TYPE 对齐的 int 契约） */
#define BEV_OBJ_VEHICLE       0
#define BEV_OBJ_PEDESTRIAN    1
#define BEV_OBJ_CYCLIST       2
#define BEV_OBJ_CONSTRUCTION  3

/* 默认 BEV 栅格参数（可配，见 bev_pre_config） */
#define BEV_PRE_DEFAULT_W       88   /* 宽（左右方向网格数） */
#define BEV_PRE_DEFAULT_H       88   /* 高（前后方向网格数） */
#define BEV_PRE_DEFAULT_RANGE_X 60.0 /* 前后各覆盖 60m（共 120m） */
#define BEV_PRE_DEFAULT_RANGE_Y_HALF 20.0 /* 左右各 20m（共 40m）+ 半宽偏移 */
#define BEV_PRE_DEFAULT_V_SCALE 30.0  /* 速度归一化参考值(m/s) */

typedef struct BevPreConfig {
    int    w;              /* 左右网格数 */
    int    h;              /* 前后网格数 */
    double range_x;        /* 前向覆盖距离 m（网格从 -range_x 到 +range_x 前向） */
    double range_y_half;   /* 单侧横向覆盖 m（y ∈ [-range_y_half, +range_y_half]） */
    double v_scale;        /* 速度归一化参考值 */
    int    channels;       /* 输出通道数（>=3，越界自动取 3/4/…编码） */
} BevPreConfig;

/** 世界系障碍物输入（与 perception_node 读 vehicle/state 字段一致）。 */
typedef struct BevPreObs {
    double x, y;        /* 世界系位置 */
    double vx, vy;      /* 世界系速度 */
    int    type;        /* 见 BEV_OBJ_* */
} BevPreObs;

/** 初始化默认配置，并填充公式化参数。 */
void bev_pre_config_default(BevPreConfig* cfg);

/**
 * 栅格化：把障碍物真值渲染成 NCHW 浮点张量（row-major，channels×H×W）。
 *
 * @param cfg     栅格参数（决定 NCHW 的 H/W/C）
 * @param ego_x   ego 世界 x
 * @param ego_y   ego 世界 y
 * @param ego_h   ego 航向 rad（车头 +x enu）
 * @param obs     世界系障碍物数组
 * @param n_obs   障碍物个数
 * @param feat    输出缓冲，至少 wells cfg->channels*cfg->h*cfg->w 个 float
 * @return 实际写入的 float 个数（channels*h*w）；cfg/feat 非法返回 0
 */
size_t bev_pre_rasterize(const BevPreConfig* cfg,
                         double ego_x, double ego_y, double ego_heading,
                         const BevPreObs* obs, int n_obs,
                         float* feat);

#ifdef __cplusplus
}
#endif

#endif /* FLOWSIM_BEV_PRE_H */