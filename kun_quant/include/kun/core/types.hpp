#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>
#include <iostream>

namespace kun {

// ======================= 枚举定义 =======================

enum class Direction : uint8_t {
    LONG = 0,     // 多
    SHORT = 1,    // 空
    NET = 2       // 净持仓 (股票)
};

inline std::string to_string(Direction d) {
    switch (d) {
        case Direction::LONG:  return "LONG";
        case Direction::SHORT: return "SHORT";
        case Direction::NET:   return "NET";
        default: return "UNKNOWN";
    }
}

enum class Offset : uint8_t {
    OPEN = 0,            // 开仓
    CLOSE = 1,           // 平仓
    CLOSE_TODAY = 2,     // 平今 (国内期货上期所等特有)
    CLOSE_YESTERDAY = 3  // 平昨
};

inline std::string to_string(Offset o) {
    switch (o) {
        case Offset::OPEN:            return "OPEN";
        case Offset::CLOSE:           return "CLOSE";
        case Offset::CLOSE_TODAY:     return "CLOSE_TODAY";
        case Offset::CLOSE_YESTERDAY: return "CLOSE_YESTERDAY";
        default: return "UNKNOWN";
    }
}

enum class OrderStatus : uint8_t {
    PENDING_SUBMIT = 0,   // 待提交
    SUBMITTED = 1,        // 已提交至柜台
    ACCEPTED = 2,         // 交易所已接受排队
    PARTIALLY_FILLED = 3, // 部分成交
    FILLED = 4,           // 全部成交
    PENDING_CANCEL = 5,   // 待撤单
    CANCELED = 6,         // 已撤销
    REJECTED = 7          // 拒单
};

inline std::string to_string(OrderStatus s) {
    switch (s) {
        case OrderStatus::PENDING_SUBMIT:   return "PENDING_SUBMIT";
        case OrderStatus::SUBMITTED:        return "SUBMITTED";
        case OrderStatus::ACCEPTED:         return "ACCEPTED";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED:           return "FILLED";
        case OrderStatus::PENDING_CANCEL:   return "PENDING_CANCEL";
        case OrderStatus::CANCELED:         return "CANCELED";
        case OrderStatus::REJECTED:         return "REJECTED";
        default: return "UNKNOWN";
    }
}

enum class OrderType : uint8_t {
    LIMIT = 0,    // 限价单
    MARKET = 1,   // 市价单
    FAK = 2,      // Fill and Kill
    FOK = 3       // Fill or Kill
};

enum class ProductType : uint8_t {
    FUTURES = 0,  // 期货
    STOCK = 1,    // 股票
    OPTION = 2,   // 期权
    CRYPTO = 3    // 加密货币
};

enum class EventType : uint32_t {
    EVENT_TIMER = 1,
    EVENT_LOG = 2,
    EVENT_TICK = 10,
    EVENT_BAR = 11,
    EVENT_ORDER_REQ = 20,
    EVENT_ORDER = 21,
    EVENT_TRADE = 22,
    EVENT_POSITION = 30,
    EVENT_ACCOUNT = 31
};

// ======================= POD 通信载荷 (用于 MessageBus 零拷贝与安全传输) =======================

#pragma pack(push, 1)

struct QuantTickMsg {
    char     symbol[16];
    char     exchange[8];
    uint64_t timestamp_us;
    double   last_price;
    double   volume;
    double   open_interest;
    double   bid_price1;
    double   bid_volume1;
    double   ask_price1;
    double   ask_volume1;
};

struct QuantBarMsg {
    char     symbol[16];
    char     exchange[8];
    char     interval[8]; // "1m", "5m", "1d"
    uint64_t timestamp_us;
    double   open_price;
    double   high_price;
    double   low_price;
    double   close_price;
    double   volume;
};

struct QuantOrderReqMsg {
    char     symbol[16];
    char     exchange[8];
    char     strategy_name[32];
    uint64_t order_req_id;
    uint8_t  direction; // 0=LONG, 1=SHORT
    uint8_t  offset;    // 0=OPEN, 1=CLOSE, 2=CLOSE_TODAY, 3=CLOSE_YESTERDAY
    uint8_t  order_type;// 0=LIMIT, 1=MARKET, 2=FAK, 3=FOK
    double   price;
    double   volume;
};

struct QuantOrderRtnMsg {
    char     symbol[16];
    char     exchange[8];
    char     strategy_name[32];
    char     order_ref[32];
    uint64_t order_id;
    uint8_t  direction;
    uint8_t  offset;
    uint8_t  status;     // OrderStatus
    double   price;
    double   total_volume;
    double   traded_volume;
    uint64_t update_time_us;
};

struct QuantTradeMsg {
    char     symbol[16];
    char     exchange[8];
    char     strategy_name[32];
    char     order_ref[32];
    uint64_t trade_id;
    uint64_t order_id;
    uint8_t  direction;
    uint8_t  offset;
    double   price;
    double   volume;
    double   commission;
    uint64_t trade_time_us;
};

#pragma pack(pop)

// ======================= 核心数据结构 =======================

// Tick行情切片数据 (L1/L2)
struct TickData {
    std::string symbol;          // 合约代码 (如 rb2405)
    std::string exchange;        // 交易所代码 (如 SHFE, DCE, CZCE)
    int64_t timestamp_us{0};     // 微秒时间戳
    std::string datetime_str;    // 格式化时间 "2026-08-29 09:30:00.500"

    double last_price{0.0};      // 最新成交价
    double volume{0.0};          // 当日累计成交量
    double open_interest{0.0};   // 持仓量
    double turnover{0.0};        // 当日成交金额

    // 买卖五档行情
    double bid_price[5]{0.0};
    double ask_price[5]{0.0};
    double bid_volume[5]{0.0};
    double ask_volume[5]{0.0};
};

// K线数据 (Bar)
struct BarData {
    std::string symbol;          // 合约代码
    std::string exchange;        // 交易所
    int64_t timestamp_us{0};     // 柱线结束时间戳
    std::string datetime_str;    // "2026-08-29 09:31:00"
    std::string interval{"1m"};  // 周期 (1m, 5m, 1d)

    double open_price{0.0};
    double high_price{0.0};
    double low_price{0.0};
    double close_price{0.0};
    double volume{0.0};
    double open_interest{0.0};
    double turnover{0.0};
};

// 报单请求
struct OrderRequest {
    std::string symbol;
    std::string exchange;
    Direction direction{Direction::LONG};
    Offset offset{Offset::OPEN};
    OrderType order_type{OrderType::LIMIT};
    double price{0.0};
    double volume{0.0};
    std::string strategy_name;
    std::string order_ref;
};

// 订单回报数据
struct OrderData {
    uint64_t order_id{0};        // 引擎分配全局唯一 ID
    std::string order_ref;       // 客户端引用
    std::string symbol;
    std::string exchange;
    Direction direction{Direction::LONG};
    Offset offset{Offset::OPEN};
    OrderType order_type{OrderType::LIMIT};
    double price{0.0};
    double total_volume{0.0};
    double traded_volume{0.0};
    OrderStatus status{OrderStatus::PENDING_SUBMIT};
    int64_t submit_time_us{0};
    int64_t update_time_us{0};
    std::string error_msg;
    std::string strategy_name;
};

// 成交回报数据
struct TradeData {
    uint64_t trade_id{0};
    uint64_t order_id{0};
    std::string order_ref;
    std::string symbol;
    std::string exchange;
    Direction direction{Direction::LONG};
    Offset offset{Offset::OPEN};
    double price{0.0};
    double volume{0.0};
    double commission{0.0};      // 手续费
    int64_t trade_time_us{0};
    std::string datetime_str;
    std::string strategy_name;
};

// 单标的方向持仓数据
struct PositionData {
    std::string symbol;
    std::string exchange;
    Direction direction{Direction::LONG};
    double volume{0.0};          // 总持仓手数
    double frozen{0.0};          // 冻结手数 (挂单平仓中)
    double yd_volume{0.0};       // 昨仓手数
    double today_volume{0.0};    // 今仓手数
    double avg_price{0.0};       // 持仓均价
    double open_cost{0.0};       // 开仓成本
    double margin{0.0};          // 占用保证金
    double realized_pnl{0.0};    // 平仓已实现盈亏
    double floating_pnl{0.0};    // 盯市浮动盈亏
};

// 资金账户数据
struct AccountData {
    std::string account_id;
    double balance{1000000.0};   // 静态权益
    double available{1000000.0}; // 可用资金
    double margin{0.0};          // 占用保证金
    double frozen_margin{0.0};   // 挂单冻结保证金
    double frozen_commission{0.0};
    double realized_pnl{0.0};    // 当日平仓盈亏
    double floating_pnl{0.0};    // 当日持仓浮动盈亏
    double commission{0.0};      // 当日累计手续费

    // 计算动态权益
    double dynamic_equity() const {
        return balance + floating_pnl;
    }
};

} // namespace kun
