# =============================================================================
# FlowEngine IDL v2 — Common Types (Header, TimeSync, Geometry)
# =============================================================================

enum TimeSyncStatus {
    TIME_SYNC_UNSYNCED = 0
    TIME_SYNC_GPTP     = 1
    TIME_SYNC_GNSS     = 2
    TIME_SYNC_SIM      = 3
}

struct Header {
    uint32         seq
    uint64         stamp_us        # 主时间戳（微秒；优先 sync_time_us，无同步时回退 host_time_us）
    uint64         sensor_time_us  # 传感器曝光/采样时刻（微秒）
    uint64         host_time_us    # 板端接收时刻（微秒，CLOCK_MONOTONIC）
    uint64         sync_time_us    # gPTP/GNSS 对齐后时刻（微秒，0=未同步）
    TimeSyncStatus sync_status     # 时钟同步状态
    string         frame_id        # 坐标系 ID（如 "vehicle", "lidar_top", "world", "utm"）
}

struct Point3f {
    float x  # 米 (m)
    float y  # 米 (m)
    float z  # 米 (m)
}
