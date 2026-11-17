# =============================================================================
# FlowEngine IDL v2 — Perception & Localization Messages
# =============================================================================

import "common.msg"

enum ObstacleType {
    OBJ_TYPE_UNKNOWN      = 0
    OBJ_TYPE_VEHICLE      = 1
    OBJ_TYPE_PEDESTRIAN   = 2
    OBJ_TYPE_CYCLIST      = 3
    OBJ_TYPE_CONSTRUCTION = 4
    OBJ_TYPE_SIGN         = 5
}

struct Obstacle {
    uint32       id             # 目标 ID（跨帧唯一）
    float        x              # 纵向距离（米，车体坐标系）
    float        y              # 横向距离（米，车体坐标系）
    float        vx             # 纵向相对速度（m/s）
    float        vy             # 横向相对速度（m/s）
    float        width          # 目标宽度（米）
    float        length         # 目标长度（米）
    int8         lane_id        # 感知计算的车道归属：0=最左, N-1=最右, -1=未分配
    ObstacleType type           # 目标类型（引用枚举）
    float        confidence     # 置信度 [0, 1]
    int8         obs_lane_match_hint   # 0 = no usable lane_match; 1 = lane_match.valid
    float        z              # 目标高程/Z 坐标（米）
    float        height         # 目标高度（米）
    float        heading        # 目标航向角（弧度）
}

struct ObstacleList {
    uint32   frame_id
    uint64   timestamp_us   # CLOCK_MONOTONIC 时间戳（微秒）
    uint32   count          # 有效障碍物数量
    Obstacle obstacles[128] # 固定 128 个障碍物
}

struct EgoState {
    float64  latitude       # 纬度（度）
    float64  longitude      # 经度（度）
    float    heading_deg    # 航向角（度）
    float    speed_mps      # 车速（m/s）
    float    yaw_rate_dps   # 偏航角速度（度/秒）
    float    acceleration_mss # 纵向加速度（m/s²）
}

struct Localization {
    uint32   frame_id       # 定位帧号（与 sensor 帧同步）
    uint64   timestamp_us   # CLOCK_MONOTONIC 时间戳（微秒）
    float    x              # 融合后 X 坐标（米）
    float    y              # 融合后 Y 坐标（米）
    float    v              # 融合速度（m/s）
    float    heading        # 航向角（弧度）
    float    yaw_rate       # 偏航角速度（弧度/秒）
    float    cov_xx         # 协方差对角元素 [0]
    float    cov_yy         # 协方差对角元素 [1]
    float    cov_vv         # 协方差对角元素 [2]
    float    cov_hh         # 协方差对角元素 [3]
    float    cov_yyaw       # 协方差对角元素 [4]
    float    innovation     # EKF 新息
    bool     diverged       # EKF 发散标志
    float    raw_pos_x      # 原始 GPS 位置 X
    float    raw_pos_y      # 原始 GPS 位置 Y
    float    raw_speed      # 原始 GPS 速度
    float    z              # 车辆高程 Z 坐标（米）
    float    grade          # 当前道路坡度（dz/ds）
}

struct LaneMatchInfo {
    uint32   seq
    uint64   stamp_us
    int32    road_id
    int32    lane_id
    float    s
    float    l
    float    heading_err
    uint8    valid
    string   road_name
}
