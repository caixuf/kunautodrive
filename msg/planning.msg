# =============================================================================
# FlowEngine IDL v2 — Planning & Reference Line Messages
# =============================================================================

import "common.msg"

enum BehaviorCommand {
    BEH_CRUISE       = 0     # 巡航（自由行驶）
    BEH_FOLLOW       = 1     # 跟车（保持安全距离）
    BEH_LEFT_CHANGE  = 2     # 向左变道
    BEH_RIGHT_CHANGE = 3     # 向右变道
    BEH_STOP         = 4     # 停车
    BEH_YIELD        = 5     # 让行（减速等待）
    BEH_EMERGENCY    = 6     # 紧急停车
    BEH_U_TURN       = 7     # 掉头（三把方向：左打死前进→右打倒退→左打前进入车道）
}

struct Behavior {
    uint32          seq            # 指令序号
    uint64          stamp_us       # 发布时刻（微秒，CLOCK_MONOTONIC）
    BehaviorCommand command        # 行为指令
    int8            target_lane_idx # 目标车道索引：-1=保持当前, 0..N-1=具体车道
    float           target_speed   # 目标速度（m/s）
    uint32          follow_obs_id  # 跟车目标障碍物 ID（0=无前车）
}

struct TrajectoryPoint {
    uint32   t_rel_us       # 相对轨迹起点的时间偏移（微秒）
    float    x              # X 坐标（米，全局坐标系）
    float    y              # Y 坐标（米，全局坐标系）
    float    s              # Frenet 纵坐标（米）
    float    l              # Frenet 横坐标（米）
    float    heading        # 期望航向角（弧度）
    float    kappa          # 曲率（1/m）
    float    v              # 期望速度（m/s）
    float    a              # 期望加速度（m/s²）
    float    jerk           # 加加速度（m/s³）
    float    z              # 期望高程 Z 坐标（米）
    float    grade          # 期望道路坡度（rad）
}

struct Trajectory {
    uint32          seq            # 轨迹序号
    uint64          stamp_us       # 发布时刻（微秒，CLOCK_MONOTONIC）
    uint32          ref_line_id    # 参考线 ID（变了就禁用拼接）
    uint16          point_count    # 有效轨迹点数
    TrajectoryPoint points[64]     # 最多 64 个轨迹点
    uint8           planner_state  # 规划器状态：0=unknown, 1=sampling, 2=qp, 3=failsafe
    uint8           is_stitched    # 是否拼接轨迹（与上帧连续）
    uint8           valid          # 轨迹是否通过可行性检查
}

struct ReferenceLinePoint {
    float    s              # Frenet 纵坐标（米）
    float    x              # 全局 X 坐标（米）
    float    y              # 全局 Y 坐标（米）
    float    theta          # 切线航向角（弧度）
    float    kappa          # 曲率（1/m）
    float    dkappa         # 曲率对 s 的导数（1/m²）
    float    left_bound     # 左侧可通行边界（米，相对中心线）
    float    right_bound    # 右侧可通行边界（米，相对中心线）
    float    speed_limit    # 速度限制（m/s）
}

struct ReferenceLine {
    Header   header
    uint32   id
    float    total_length
    ReferenceLinePoint points[]
}

struct NavPath {
    Header   header
    uint32   route_id
    float    total_length
    string   route_name
    Point3f  waypoints[]
}
