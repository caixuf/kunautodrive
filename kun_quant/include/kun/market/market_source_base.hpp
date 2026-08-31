#pragma once

#include "kun/core/types.hpp"
#include <string>
#include <vector>

namespace kun {

/**
 * @brief 抽象行情数据源接口 (IMarketFetcher)
 * 遵循 DRY 与开闭原则，为 Sina、Eastmoney、CTP 等多源行情提供统一标准接口
 */
class IMarketFetcher {
public:
    virtual ~IMarketFetcher() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool fetch_once() = 0;
    virtual std::string get_source_name() const = 0;
};

} // namespace kun
