#!/usr/bin/env bash
# tools/build_lanelet.sh — 为指定 maps/<name>/ 生成 lanelet.osm（D2-02 转换器调用）
#
# 按 docs/REQ_L3_DIR2_HDMAP.md §3 user story：
#   "在 maps/osm_lujiazui_v2 加一条新路（OSM 拉个 bbox 重跑转换器）→
#    bash tools/build_lanelet.sh osm_lujiazui_v2 一条命令，CI 自动跑 consistency gate"
#
# 用法:
#   bash tools/build_lanelet.sh                  # 生成所有缺 lanelet.osm 的 maps/*/
#   bash tools/build_lanelet.sh osm_lujiazui_v2  # 只生成指定地图
#   bash tools/build_lanelet.sh --all            # 强制重新生成所有（含已存在的）
#   bash tools/build_lanelet.sh --check          # 跑 ci/gates/lanelet_consistency_check.py
#                                              # 检查所有已存在的 lanelet.osm
#
# 选项:
#   --all       强制重新生成全部（含已存在 lanelet.osm）
#   --check     跳过生成，直接跑 consistency gate（CI 友好）
#   --quiet     减少输出（CI 默认）
#   --help      显示本帮助
#
# 与 docs/M1_OSM_INTERFACE_CONTRACT.md §4.7 / §7 的关系:
#   转换器字节级确定性，所以再次运行只要 map.json 不变 → 输出完全相同
#   （test_05_byte_level_determinism / test_11_byte_stability_with_regulatory_D2_08 单测守）。
#   本脚本不修改任何 map.json，只读 → 写。
#
# 大地图（osm_zhengdong 63MB+）默认不 commit，依赖本脚本现生成。CI 跑前必须先调本脚本。
#
# Exit codes:
#   0 = 全部成功（生成+检查都过）
#   1 = 转换器或 consistency gate 报错
#   2 = 参数错误（缺 maps 子目录）

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONVERTER="${ROOT}/tools/json_to_lanelet.py"
GATE="${ROOT}/ci/gates/lanelet_consistency_check.py"

# 默认 --quiet，CI 友好
QUIET=1
ALL=0
CHECK_ONLY=0
TARGETS=()

usage() {
    sed -n '2,/^set /p' "${BASH_SOURCE[0]}" | sed -e 's/^# \{0,1\}//' -e '/^#!$/d'
    exit 0
}

log() {
    if [[ "${QUIET}" -eq 0 ]]; then
        echo "$@"
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --all)       ALL=1; shift ;;
        --check)     CHECK_ONLY=1; shift ;;
        --quiet)     QUIET=1; shift ;;
        --no-quiet)  QUIET=0; shift ;;
        --help|-h)   usage ;;
        -*)          echo "::error::unknown option: $1" >&2; exit 2 ;;
        *)           TARGETS+=("$1"); shift ;;
    esac
done

# 校验转换器存在
if [[ ! -f "${CONVERTER}" ]]; then
    echo "::error::converter not found: ${CONVERTER}" >&2
    exit 2
fi

# 决定要处理的 maps/<name> 子目录
if [[ ${CHECK_ONLY} -eq 1 ]]; then
    # --check 模式：跑 consistency gate 即可
    exec python3 "${GATE}"
fi

if [[ ${#TARGETS[@]} -eq 0 ]]; then
    # 默认模式：扫 ROOT/maps/*/，挑出 (有 map.json 且缺 lanelet.osm) 或 (--all 时所有)
    for d in "${ROOT}"/maps/*/; do
        [[ -d "${d}" ]] || continue
        name="$(basename "${d%/}")"
        mp="${d}/map.json"
        op="${d}/lanelet.osm"
        [[ -f "${mp}" ]] || continue
        if [[ ${ALL} -eq 1 || ! -f "${op}" ]]; then
            TARGETS+=("${name}")
        fi
    done
fi

if [[ ${#TARGETS[@]} -eq 0 ]]; then
    log "no missing lanelet.osm; nothing to do. Use --all to force regenerate."
    exit 0
fi

failed=0
for name in "${TARGETS[@]}"; do
    mp="${ROOT}/maps/${name}/map.json"
    op="${ROOT}/maps/${name}/lanelet.osm"
    if [[ ! -f "${mp}" ]]; then
        echo "::error::missing map.json: ${mp}" >&2
        failed=1
        continue
    fi
    log "[${name}] regenerating lanelet.osm from map.json..."
    if ! python3 "${CONVERTER}" -o "${op}" "${mp}" >/dev/null; then
        echo "::error::conversion failed: ${name}" >&2
        failed=1
        continue
    fi
    log "[${name}] done ($(wc -c < "${op}") bytes)"
done

if [[ ${failed} -ne 0 ]]; then
    exit 1
fi

log ""
log "all done; running consistency gate for sanity check..."
exec python3 "${GATE}"
