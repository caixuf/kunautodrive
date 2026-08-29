import json
import os
from datetime import datetime
from typing import Dict, Any, List, Optional
from ai_service.provider import LLMProvider

class DailyReporter:
    def __init__(self, provider: Optional[LLMProvider] = None):
        self.provider = provider or LLMProvider()

    def generate_report(self, summary_data: Dict[str, Any], date_str: Optional[str] = None) -> str:
        date_str = date_str or datetime.now().strftime("%Y-%m-%d")
        
        system_prompt = (
            "你是一个严谨的量化对冲基金高级投研总监与风控主管。"
            "请根据提供的每日量化交易执行数据、持仓快照、收益率曲线与风控日志，生成一份专业、客观、不夸大、深具洞察力的收盘复盘研报。"
            "报告必须包含：\n"
            "1. 核心资产与权益变动（净值、收益额、收益率、最大回撤）\n"
            "2. 交易执行归因（胜率、盈亏比、手续费磨损、多空表现）\n"
            "3. 异常刺针与风控拦截记录（Sensor Fusion 与风控表现）\n"
            "4. 次日操作建议与风控参数调优指南\n"
            "语言风格专业严谨，采用 Markdown 格式输出。"
        )

        user_content = f"以下是账户 [{summary_data.get('account_id', 'ALL')}] 在 {date_str} 的全量统计数据：\n"
        user_content += json.dumps(summary_data, ensure_ascii=False, indent=2)

        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_content}
        ]

        report_md = self.provider.chat_completion(messages)

        # 自动落盘
        try:
            os.makedirs("runs/reports", exist_ok=True)
            report_file = f"runs/reports/daily_report_{date_str}_{summary_data.get('account_id', 'master')}.md"
            with open(report_file, "w", encoding="utf-8") as f:
                f.write(report_md)
        except Exception:
            pass

        return report_md
