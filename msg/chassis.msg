# =============================================================================
# FlowEngine IDL v2 — Chassis Feedback & HMI Decision State
# =============================================================================

import "common.msg"
import "control.msg"
import "planning.msg"

enum EpbStatus {
    EPB_RELEASED = 0
    EPB_APPLIED  = 1
    EPB_MOVING   = 2
    EPB_FAULT    = 3
}

enum DriveMode {
    DRIVE_MODE_MANUAL    = 0
    DRIVE_MODE_AUTO      = 1
    DRIVE_MODE_OVERRIDE  = 2
    DRIVE_MODE_EMERGENCY = 3
    DRIVE_MODE_FAULT     = 4
}

# 底盘实时反馈（CAN FD / SOME-IP 对齐，车体坐标系）
struct ChassisState {
    Header    header
    float     speed_mps            # 车速 (m/s)
    float     wheel_speed_fl_mps   # 左前轮速 (m/s)
    float     wheel_speed_fr_mps   # 右前轮速 (m/s)
    float     wheel_speed_rl_mps   # 左后轮速 (m/s)
    float     wheel_speed_rr_mps   # 右后轮速 (m/s)
    float     steering_angle_rad   # 实际前轮/方向盘转角 (rad，+右/-左)
    float     steering_rate_rps    # 转向角速度 (rad/s)
    float     steering_torque_nm   # 转向手力矩 (N·m)
    float     throttle_pct         # 油门踏板开度 [0.0, 1.0]
    float     brake_pct            # 制动踏板开度 [0.0, 1.0]
    float     brake_pressure_mpa   # 主缸制动压力 (MPa)
    Gear      gear                 # 当前档位
    EpbStatus epb                  # 电子驻车状态
    DriveMode drive_mode           # 线控底盘模式
    uint8     turn_signal          # 转向灯状态: 0=off, 1=left, 2=right
    bool      hazard               # 双闪状态
    bool      driver_override      # 驾驶员接管标志
    uint32    dtc_code             # 故障码 (0=正常)
}

# HMI / 决策输出状态
struct HmiDecisionState {
    Header          header
    DriveMode       active_mode        # 当前智驾模式
    BehaviorCommand behavior           # 当前行为决策
    bool            takeover_requested # 是否请求接管 (TOR)
    uint8           tor_level          # 接管告警等级 (0=无, 1=提示, 2=紧急)
    string          reason             # 人类可读降级/接管原因
}
