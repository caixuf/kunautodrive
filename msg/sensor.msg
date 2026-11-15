# =============================================================================
# FlowEngine IDL v2 — Sensor Messages
# =============================================================================

import "common.msg"

struct LidarFrame {
    float    x
    float    y
    float    z
    float    intensity
    uint32   point_count
    uint32   frame_id
}

# LiDAR 点云批次（FAST-LIO2/LIO-SAM 的输入契约）
# 与单点 LidarFrame 分离：count <= 2048，时间偏移用于 deskew。
struct LidarPoint {
    float    x
    float    y
    float    z
    float    intensity
    uint32   time_offset_us
}

struct LidarPointCloud {
    uint32   frame_id
    uint64   timestamp_us
    uint32   count
    uint32   sensor_id
    LidarPoint points[2048]
}

# 毫米波雷达（L3-P2 冗余）
struct RadarTarget {
    float    range_m        # 径向距离（米）
    float    azimuth_deg    # 方位角（度，正=左）
    float    range_rate_mps # 径向速度（m/s，正=远离）
    float    rcs_dbsm       # 雷达截面积（dBsm，幅度代理）
}

struct RadarFrame {
    uint64   timestamp_us   # 采样时间戳（微秒，CLOCK_MONOTONIC）
    uint32   frame_id
    uint32   count          # 有效目标数
    uint32   sensor_id      # 传感器编号（多雷达时区分）
    uint32   reserved       # 保留/扩展位
    RadarTarget targets[64]
}

struct GpsData {
    float64  latitude       # 纬度（度）
    float64  longitude      # 经度（度）
    float    speed_mps      # 速度（m/s）
    float    heading_deg    # 航向角（度，北偏东）
    float    accuracy_m     # 定位精度（米，1-sigma）
    uint64   timestamp_us   # 采样时间戳（微秒）
}

# IMU 原始数据（6 轴：加速度 + 陀螺仪）
struct ImuData {
    float    accel_x        # 加速度 X 轴（m/s²）
    float    accel_y        # 加速度 Y 轴（m/s²）
    float    accel_z        # 加速度 Z 轴（m/s²，含重力）
    float    gyro_x         # 陀螺仪 X 轴（rad/s）
    float    gyro_y         # 陀螺仪 Y 轴（rad/s）
    float    gyro_z         # 陀螺仪 Z 轴（rad/s，即 yaw_rate）
    uint64   timestamp_us   # 采样时间戳（微秒）
    float    temperature    # 温度（℃）
}

# 2D 位姿（SLAM 或里程计输出）
struct Pose2D {
    float    x              # X 坐标（米）
    float    y              # Y 坐标（米）
    float    heading        # 航向角（弧度）
    float    cov_xx         # 位置 X 协方差
    float    cov_yy         # 位置 Y 协方差
    float    cov_hh         # 航向协方差
    bool     converged      # SLAM 是否收敛
    uint32   source         # 来源标志：0=UNKNOWN, 1=GPS, 2=SLAM, 3=ODOMETRY
}

# 双目视觉帧（固定大小兼容版）
struct StereoFrame {
    uint32   width          # 图像宽度（像素）
    uint32   height         # 图像高度（像素）
    uint64   timestamp_us   # 采样时间戳（微秒）
    uint32   left_jpeg_size # 左图 JPEG 数据有效字节数
    uint32   depth_count    # 深度数据有效浮点数个数
    float    baseline_m     # 双目基线长度（米）
    float    fov_deg        # 水平视场角（度）
    uint8    left_jpeg[25600]   # 左图 JPEG 压缩数据
    float    depth_data[4800]   # 深度图降采样数据
}

# 零拷贝大图像帧（v2 变长）
struct ImageFrame {
    Header   header
    uint32   width
    uint32   height
    uint32   format         # 0=RGB8, 1=BGR8, 2=JPEG, 3=MONO8, 4=FLOAT32_DEPTH
    uint32   stride
    uint8    data[]
}
