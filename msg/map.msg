# =============================================================================
# FlowEngine IDL v2 — Map & Traffic Signal Messages
# =============================================================================

import "common.msg"

enum TrafficLightColor {
    TL_COLOR_UNKNOWN = 0
    TL_COLOR_RED     = 1
    TL_COLOR_YELLOW  = 2
    TL_COLOR_GREEN   = 3
}

struct TrafficLightBulb {
    uint32            id
    TrafficLightColor color
    float             x
    float             y
    float             z
    float             remaining_s
    float             distance_m
}

struct TrafficLightList {
    Header           header
    TrafficLightBulb bulbs[]
}
