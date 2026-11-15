# =============================================================================
# FlowEngine IDL v2 — Prediction Messages
# =============================================================================

import "common.msg"
import "planning.msg"

struct PredictionHypothesis {
    float           probability    # 该模态概率
    float           sigma_x        # 纵向不确定度增长因子
    float           sigma_y        # 横向不确定度增长因子
    uint8           intent         # 意图：0=LANE_KEEP, 1=LEFT_CHANGE, 2=RIGHT_CHANGE, 3=TURN_LEFT, 4=TURN_RIGHT, 5=YIELD
    uint32          point_count    # 有效轨迹点数
    TrajectoryPoint points[25]     # 最多 25 个预测点（5s @ 0.2s）
}

struct PredictionSet {
    uint32               seq            # 预测序号
    uint64               stamp_us       # 发布时刻（微秒）
    uint32               track_id       # 被预测的障碍物 track_id
    uint8                n_hypotheses   # 有效假设数
    PredictionHypothesis hypotheses[3]  # 最多 3 模态
}

struct ObjectPrediction {
    uint32   track_id
    float    probability
    uint8    intent
    float    sigma_x
    float    sigma_y
    uint32   point_start
    uint32   point_count
}

struct PredictionBatch {
    Header           header
    ObjectPrediction objects[]
    TrajectoryPoint  points[]
}
