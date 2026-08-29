import json
import os
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler
from ai_service.config import AIConfig
from ai_service.provider import LLMProvider
from ai_service.daily_reporter import DailyReporter
from ai_service.memory import StrategyMemoryStore
from ai_service.workflow_pipeline import EvolutionWorkflowPipeline
from ai_service.background_agent_client import BackgroundAgentClient
from ai_service.proactive_daemon import ProactiveAutonomousScheduler
from ai_service.focus_tracker import FocusProductTracker

class AIServiceHandler(BaseHTTPRequestHandler):
    memory = StrategyMemoryStore()
    provider = LLMProvider()
    reporter = DailyReporter(provider)
    pipeline = EvolutionWorkflowPipeline(provider.config, memory)
    bg_agent_client = BackgroundAgentClient(provider.config)
    scheduler = ProactiveAutonomousScheduler(provider.config, memory, reporter)
    focus = FocusProductTracker(provider.config, provider)

    def do_GET(self):
        if self.path == "/api/ai/health":
            self._send_json(200, {
                "status": "UP",
                "service": "KunQuant AI Proactive Autonomous Service",
                "model": self.provider.config.model,
                "has_api_key": bool(self.provider.config.api_key),
                "bg_agent_base_url": self.bg_agent_client.config.bg_agent_base_url
            })
        elif self.path == "/api/ai/memory/stats":
            stats = self.memory.get_memory_stats()
            self._send_json(200, {"status": "OK", "data": stats})
        elif self.path == "/api/focus":
            self._send_json(200, {"status": "OK", "focus": self.focus.list_focus()})
        elif self.path.startswith("/api/tasks"):
            status = None
            if "status=" in self.path:
                status = self.path.split("status=")[1].split("&")[0]
            self._send_json(200, {"status": "OK", "tasks": self.focus.list_tasks(status)})
        else:
            self._send_json(404, {"error": "Not Found"})

    def do_POST(self):
        content_length = int(self.headers.get('Content-Length', 0))
        post_data = self.rfile.read(content_length).decode('utf-8')
        
        try:
            body = json.loads(post_data) if post_data else {}
        except Exception:
            self._send_json(400, {"error": "Invalid JSON"})
            return

        if self.path == "/api/ai/report":
            report_md = self.reporter.generate_report(body)
            self._send_json(200, {"status": "OK", "report": report_md})

        el        if self.path == "/api/ai/chat":
            prompt = body.get("message", "")
            context = body.get("context", "")
            focus_ctx = self.focus.focus_context_for_chat()
            messages = [
                {"role": "system", "content":
                    "你是 KunQuant 专属量化投研助手。回答专业、严谨、不讲空话。\n"
                    + focus_ctx},
                {"role": "user", "content": f"上下文: {context}\n问题: {prompt}" if context else prompt}
            ]
            reply = self.provider.chat_completion(messages)
            created_tasks = self.focus.extract_and_create_tasks(reply)
            resp = {"status": "OK", "reply": reply}
            if created_tasks:
                resp["created_tasks"] = created_tasks
            self._send_json(200, resp)

        # ── 专项产品跟踪 (Focus Tracker) ──
        elif self.path == "/api/focus/add":
            res = self.focus.add_focus(
                body.get("symbol", ""),
                body.get("note", ""),
                body.get("category", "futures"),
            )
            self._send_json(200 if res.get("ok") else 400, res)

        elif self.path == "/api/focus/remove":
            res = self.focus.remove_focus(body.get("symbol", ""))
            self._send_json(200, res)

        elif self.path == "/api/focus/snapshot":
            self._send_json(200, {"status": "OK", "snapshot": self.focus.snapshot(body.get("symbol", ""))})

        # ── AI 分析任务 ──
        elif self.path == "/api/tasks/create":
            res = self.focus.create_task(
                body.get("symbol", ""),
                body.get("type", ""),
                body.get("description", ""),
            )
            self._send_json(200 if res.get("ok") else 400, res)

        elif self.path == "/api/tasks/run":
            res = self.focus.run_task(int(body.get("id", 0)))
            self._send_json(200, res)

        elif self.path == "/api/tasks/run_pending":
            res = self.focus.run_pending()
            self._send_json(200, {"status": "OK", "executed": len(res), "results": res})

        # 主动定时触发接口
        elif self.path == "/api/ai/proactive/daily_review":
            report = self.scheduler.trigger_daily_review_workflow(body.get("date"))
            self._send_json(200, {"status": "OK", "report": report})

        elif self.path == "/api/ai/proactive/weekly_evolution":
            res = self.scheduler.trigger_weekly_distillation_and_evolution()
            self._send_json(200, {"status": "OK", "result": res})

        # 实时事件驱动触发接口
        elif self.path == "/api/ai/event/drawdown_alert":
            acc_id = body.get("account_id", "acc_master")
            mdd = float(body.get("max_drawdown_pct", 2.0))
            advice = self.scheduler.on_drawdown_breach_event(acc_id, mdd, body.get("context"))
            self._send_json(200, {"status": "TRIGGERED", "advice": advice})

        elif self.path == "/api/ai/event/sensor_alert":
            symbol = body.get("symbol", "rb2405")
            count = int(body.get("outlier_count", 5))
            dev = float(body.get("max_deviation_pct", 4.5))
            msg = self.scheduler.on_sensor_outlier_spike_event(symbol, count, dev)
            self._send_json(200, {"status": "TRIGGERED", "alert_message": msg})

        # 云端 Background Agent 接口
        elif self.path == "/api/ai/agent/create":
            prompt = body.get("prompt", "")
            session_id = body.get("sessionId")
            agent_title = body.get("agentTitle", "量化助手")
            sandbox_type = body.get("sandboxType", "cvm")
            repo_config = body.get("repoConfig")
            res = self.bg_agent_client.create_agent(
                prompt=prompt,
                session_id=session_id,
                agent_title=agent_title,
                sandbox_type=sandbox_type,
                repo_config=repo_config
            )
            self._send_json(200, res)

        else:
            self._send_json(404, {"error": "Not Found"})

    def _send_json(self, code: int, data: dict):
        resp = json.dumps(data, ensure_ascii=False).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(resp)))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, format, *args):
        return

def run_server(port: int = 8901):
    server = HTTPServer(('0.0.0.0', port), AIServiceHandler)
    print(f"[KunQuant AI Proactive Service] Listening on http://0.0.0.0:{port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.server_close()

if __name__ == "__main__":
    port = 8901
    if len(sys.argv) > 1:
        port = int(sys.argv[1])
    run_server(port)
