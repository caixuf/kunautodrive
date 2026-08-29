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
            "请严格根据提供的真实量化交易执行数据、持仓快照、行情真实记录与风控日志，生成一份专业、客观、不夸大、深具洞察力的收盘复盘研报。\n"
            "【绝对纪律】：严禁虚构或脑补未经提供的收益率、成交单或持仓。若当日交易数为 0 或持仓为空（如系统刚启动或处于行情接入信号监听期），必须实事求是反映真实状态（说明当前处于高频行情接入与信号监听阶段），绝对不能捏造假盈利或假交易。\n"
            "报告必须包含：\n"
            "1. 核心资产与权益变动（净值、收益额、收益率、最大回撤，如实反映）\n"
            "2. 交易执行归因（成交量、手续费、各标的行情覆盖度）\n"
            "3. 异常刺针与风控拦截记录（Sensor Fusion 多源行情接入与过滤表现）\n"
            "4. 次阶段操作建议与风控参数调优指南\n"
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
