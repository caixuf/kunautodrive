# =============================================================================
# FlowEngine IDL v2 — Control Messages
# =============================================================================

enum Gear {
    GEAR_REVERSE = -1
    GEAR_NEUTRAL = 0
    GEAR_DRIVE   = 1
}

struct ControlRaw {
    uint32   seq            # 指令序号
    float    throttle       # 油门 [0.0, 1.0]
    float    brake          # 制动 [0.0, 1.0]
    float    steering       # 转向角（rad）：正值右转，负值左转
    float    speed          # 当前速度 (m/s)
    float    target         # 目标速度 (m/s)
    float    error          # 速度误差 (m/s)
    float    cte            # 横向跟踪误差 (m)，正=偏左，负=偏右
    char     mode[24]       # 模式字符串: ACCEL/BRAKE/HOLD/ROAD_GUARD等
    float    confidence     # 命令自评置信度 [0,1]
    uint8    turn_signal    # 转向灯指令：0=off, 1=left, 2=right
    bool     hazard         # 双闪灯指令
    Gear     gear           # 档位
}

struct ControlCmd {
    uint32   seq            # 指令序号
    float    throttle       # 油门 [0.0, 1.0]
    float    brake          # 制动 [0.0, 1.0]
    float    steering       # 转向角（rad）：正值右转，负值左转
    float    confidence     # 命令自评置信度 [0,1]
    Gear     gear           # 档位
    bool     emergency_stop # 紧急制动标志
    uint8    turn_signal    # 转向灯指令：0=off, 1=left, 2=right
    bool     hazard         # 双闪灯指令
}
