#pragma once

#include "kun/core/types.hpp"
#include "kun/engine/multi_account_router.hpp"
#include "kun/engine/position_manager.hpp"
#include <string>
#include <vector>

namespace kun {

struct ServerConfig {
    int port{8900};
    std::string static_dir{"tools/kunboard"};
    std::string db_path{"data/kun_quant.db"};
};

struct QuantAppConfig {
    ServerConfig server;
    std::vector<AccountProfile> accounts;
    std::vector<SymbolInfo> symbols;
};

class ConfigLoader {
public:
    static bool load_from_file(const std::string& path, QuantAppConfig& out_cfg);
    static QuantAppConfig default_config();
};

} // namespace kun
