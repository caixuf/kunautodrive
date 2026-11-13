#ifndef FLOWSIM_XODR_CACHE_H
#define FLOWSIM_XODR_CACHE_H

#include "logger.h"
#include "platform_paths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

/* JSON → xodr 转换（调 json_to_xodr.py 子进程，带 mtime 持久化缓存）。
 * 失败返回空字符串，调用方按 roads=nullptr 降级（NPC AI 用横向距离判车道）。 */
static inline std::string convert_scenario_to_xodr(const std::string& scenario_path) {
    const char* slash = strrchr(scenario_path.c_str(), '/');
    const char* backslash = strrchr(scenario_path.c_str(), '\\');
    const char* separator = slash;
    if (!separator || (backslash && backslash > separator)) separator = backslash;
    std::string base = flow_path_basename(scenario_path.c_str());
    if (base.size() > 5 && base.compare(base.size() - 5, 5, ".json") == 0) {
        base.resize(base.size() - 5);
    }
    char xodr_buf[1024];
    std::string xodr_name = "flowsim_" + base + ".xodr";
    if (flow_temp_path(xodr_buf, sizeof(xodr_buf), xodr_name.c_str()) != 0) return "";
    std::string xodr_path = xodr_buf;

    struct stat scen_st, xodr_st;
    if (stat(scenario_path.c_str(), &scen_st) == 0 &&
        stat(xodr_path.c_str(), &xodr_st) == 0 && xodr_st.st_size > 0 &&
        xodr_st.st_mtime >= scen_st.st_mtime) {
        LOG_INFO("flowsim", "json_to_xodr: cache hit for %s → %s",
                 scenario_path.c_str(), xodr_path.c_str());
        return xodr_path;
    }

    std::string scenario_dir;
    if (separator) scenario_dir.assign(scenario_path.c_str(), separator);
    std::vector<std::string> candidates = {
        scenario_dir + "/../tools/json_to_xodr.py",
        "/workspace/tools/json_to_xodr.py",
        "tools/json_to_xodr.py",
    };

    for (const auto& tool : candidates) {
        struct stat st;
        if (stat(tool.c_str(), &st) != 0) continue;

        char err_log_buf[1024];
        if (flow_temp_path(err_log_buf, sizeof(err_log_buf), "flowsim_xodr_err.log") != 0)
            continue;
        const char* err_log = err_log_buf;
        const char* python = getenv("FLOWENGINE_PYTHON");
        if (!python || !python[0]) {
#if defined(_WIN32)
            python = "python";
#else
            python = "python3";
#endif
        }
#if defined(_WIN32)
        std::string cmd = "\"\"";
#else
        std::string cmd = "\"";
#endif
        cmd += python;
        cmd += "\" \"";
        cmd += tool;
        cmd += "\" \"";
        cmd += scenario_path;
        cmd += "\" -o \"";
        cmd += xodr_path;
        cmd += "\" >";
#if defined(_WIN32)
        cmd += "NUL";
#else
        cmd += "/dev/null";
#endif
        cmd += " 2>\"";
        cmd += err_log;
        cmd += "\"";
#if defined(_WIN32)
        cmd += "\"";
#endif
        int rc = system(cmd.c_str());
        if (rc == 0 && stat(xodr_path.c_str(), &st) == 0 && st.st_size > 0) {
            LOG_INFO("flowsim", "json_to_xodr: %s → %s (via %s)",
                     scenario_path.c_str(), xodr_path.c_str(), tool.c_str());
            return xodr_path;
        }
        LOG_WARN("flowsim", "json_to_xodr failed (rc=%d) via %s", rc, tool.c_str());
        if (rc != 0) {
            FILE* ef = std::fopen(err_log, "r");
            if (ef) {
                char line[256];
                while (std::fgets(line, sizeof line, ef)) {
                    LOG_WARN("flowsim", "json_to_xodr: %s", line);
                }
                std::fclose(ef);
            }
        }
    }

    LOG_WARN("flowsim", "json_to_xodr: no working tool found, esmini road network disabled");
    return "";
}

#endif  // FLOWSIM_XODR_CACHE_H
