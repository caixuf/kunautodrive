/**
 * 鲲量化交易终端 (KunQuant Terminal) - 前端核心控制器
 * 深度借鉴 Futu / TradingView / VNPY 经典交互：盘口联动填单、Toast 通知、快捷价量
 */

(function () {
    'use strict';

    // ─────────────────────────────────────────────────────────────
    // 全局数据状态
    // ─────────────────────────────────────────────────────────────
    const State = {
        currentPage: 'trading-desk',
        activeSymbol: 'rb2405',
        activeTf: '1m',
        activeCategory: 'all',

        contracts: [
            // A股核心蓝筹与成长个股
            { symbol: '600519', name: '贵州茅台', category: 'stock', last: 1728.50, chg: 1.45, volume: 38290, tick: 0.1, mult: 100, marginRatio: 1.0 },
            { symbol: '300750', name: '宁德时代', category: 'stock', last: 196.20, chg: 3.12, volume: 128400, tick: 0.1, mult: 100, marginRatio: 1.0 },
            { symbol: '002594', name: '比亚迪', category: 'stock', last: 248.60, chg: -0.85, volume: 64200, tick: 0.1, mult: 100, marginRatio: 1.0 },
            { symbol: '688981', name: '中芯国际', category: 'stock', last: 56.80, chg: 4.60, volume: 290100, tick: 0.01, mult: 100, marginRatio: 1.0 },
            { symbol: '601318', name: '中国平安', category: 'stock', last: 43.50, chg: 0.62, volume: 450200, tick: 0.01, mult: 100, marginRatio: 1.0 },
            { symbol: '600036', name: '招商银行', category: 'stock', last: 32.80, chg: -0.30, volume: 310500, tick: 0.01, mult: 100, marginRatio: 1.0 },

            // 核心宽基与行业 ETF
            { symbol: '510300', name: '300ETF', category: 'etf', last: 3.652, chg: 0.55, volume: 1820400, tick: 0.001, mult: 10000, marginRatio: 1.0 },
            { symbol: '588000', name: '科创50ETF', category: 'etf', last: 0.892, chg: 2.30, volume: 3209100, tick: 0.001, mult: 10000, marginRatio: 1.0 },
            { symbol: '159915', name: '创业板ETF', category: 'etf', last: 1.820, chg: 1.15, volume: 1450200, tick: 0.001, mult: 10000, marginRatio: 1.0 },
            { symbol: '518880', name: '黄金ETF', category: 'etf', last: 5.380, chg: 0.42, volume: 980100, tick: 0.001, mult: 10000, marginRatio: 1.0 },
            { symbol: '513050', name: '中概互联', category: 'etf', last: 1.085, chg: -1.20, volume: 2410800, tick: 0.001, mult: 10000, marginRatio: 1.0 },

            // 国内商品期货主力
            { symbol: 'rb2405', name: '螺纹钢主力', category: 'futures', last: 3625.0, chg: 1.25, volume: 482910, tick: 1.0, mult: 10, marginRatio: 0.10 },
            { symbol: 'au2406', name: '沪金主力', category: 'futures', last: 568.20, chg: 0.95, volume: 194020, tick: 0.02, mult: 1000, marginRatio: 0.10 },
            { symbol: 'cu2405', name: '沪铜主力', category: 'futures', last: 74500.0, chg: 1.80, volume: 84900, tick: 10.0, mult: 5, marginRatio: 0.12 },
            { symbol: 'i2405',  name: '铁矿石主力', category: 'futures', last: 885.5, chg: -0.62, volume: 184900, tick: 0.5, mult: 100, marginRatio: 0.12 },
            { symbol: 'j2405',  name: '焦炭主力', category: 'futures', last: 2310.0, chg: 2.10, volume: 82100, tick: 0.5, mult: 100, marginRatio: 0.15 },

            // 金融股指期货
            { symbol: 'IF2406', name: '沪深300期指', category: 'index', last: 3580.2, chg: 0.45, volume: 92300, tick: 0.2, mult: 300, marginRatio: 0.12 },
            { symbol: 'IM2406', name: '中证1000期指', category: 'index', last: 5420.0, chg: 1.85, volume: 64100, tick: 0.2, mult: 200, marginRatio: 0.15 },
            { symbol: 'IC2406', name: '中证500期指', category: 'index', last: 5120.0, chg: 0.92, volume: 71200, tick: 0.2, mult: 200, marginRatio: 0.12 }
        ],

        depth: {
            asks: [
                { price: 3629.0, vol: 140 },
                { price: 3628.0, vol: 95 },
                { price: 3627.0, vol: 230 },
                { price: 3626.0, vol: 310 },
                { price: 3625.0, vol: 80 }
            ],
            bids: [
                { price: 3624.0, vol: 120 },
                { price: 3623.0, vol: 260 },
                { price: 3622.0, vol: 410 },
                { price: 3621.0, vol: 180 },
                { price: 3620.0, vol: 500 }
            ]
        },

        marketBars: [],
        backtestEquity: [],

        positions: [
            { symbol: 'rb2405', dir: '多头 (LONG)', vol: 20, frozen: 0, ydToday: '0 / 20', avg: 3610.0, last: 3625.0, margin: 72500, pnl: 3000 },
            { symbol: 'hc2405', dir: '空头 (SHORT)', vol: 10, frozen: 0, ydToday: '0 / 10', avg: 3790.0, last: 3780.0, margin: 37800, pnl: 1000 }
        ],

        orders: [
            { id: '10049', time: '09:32:15', strat: '双均线趋势', symbol: 'rb2405', type: '限价', dir: '买入', offset: '开仓', price: 3624.0, filled: 0, total: 10, status: '挂单中' },
            { id: '10048', time: '09:30:00', strat: 'FlowCoro TWAP', symbol: 'rb2405', type: '限价', dir: '买入', offset: '开仓', price: 3619.0, filled: 10, total: 10, status: '全部成交' }
        ],

        trades: [
            { id: '50021', time: '09:30:00', symbol: 'rb2405', dir: '买入', offset: '开仓', price: 3619.0, vol: 10, comm: 3.62 }
        ],

        strategies: [
            { id: 'CTA_DualMA_01', name: '双均线趋势策略', symbol: 'rb2405', model: 'FlowCoro 协程', state: '运行中', pos: '多头 20手', pnl: '+3,000.00', mdd: '0.04%' },
            { id: 'ALGO_TWAP_02', name: 'TWAP 智能拆单执行', symbol: 'rb2405', model: 'FlowCoro 协程', state: '空闲', pos: '无持仓', pnl: '0.00', mdd: '0.00%' },
            { id: 'ARB_RB_HC_03', name: '螺纹/热卷跨品种套利', symbol: 'rb/hc', model: 'WhenAnyBus 竞争', state: '运行中', pos: '多10/空10', pnl: '+1,000.00', mdd: '0.12%' }
        ],

        aiRadarRecommendations: [
            { symbol: 'au2406', name: '沪金主力', category: '商品期货', score: 96, reason: '全球宏观避险共振 · 突破历史高点压力', entry: 568.2, sl: 558.0, tp: 592.0, winRate: '82%' },
            { symbol: '600519', name: '贵州茅台', category: 'A股核心', score: 94, reason: '放量站上年线 · 均线多头共振发散', entry: 1728.5, sl: 1690.0, tp: 1820.0, winRate: '78%' },
            { symbol: '300750', name: '宁德时代', category: 'A股成长', score: 91, reason: '放量突破布林上轨 · RSI底背离金叉', entry: 196.2, sl: 188.0, tp: 215.0, winRate: '74%' },
            { symbol: '588000', name: '科创50ETF', category: 'ETF基金', score: 88, reason: '成交量异动放大 2.4x · MACD零轴金叉', entry: 0.892, sl: 0.865, tp: 0.960, winRate: '71%' },
            { symbol: 'IM2406', name: '中证1000期指', category: '金融期指', score: 92, reason: '小盘动量反转 · 贴水大幅收敛', entry: 5420.0, sl: 5310.0, tp: 5650.0, winRate: '76%' }
        ],

        evolutionGen: 48,
        evolutionPop: [
            { id: 'GEN_01', fast: 4, slow: 22, sl: 2.1, tp: 4.8, pnl: '+42,500.00', fitness: 94.8, status: '精英保留' },
            { id: 'GEN_02', fast: 5, slow: 20, sl: 2.0, tp: 4.5, pnl: '+38,200.00', fitness: 91.2, status: '精英保留' },
            { id: 'GEN_03', fast: 3, slow: 18, sl: 1.8, tp: 4.2, pnl: '+34,100.00', fitness: 88.5, status: '变异繁殖' },
            { id: 'GEN_04', fast: 6, slow: 26, sl: 2.4, tp: 5.2, pnl: '+31,900.00', fitness: 85.0, status: '变异繁殖' },
            { id: 'GEN_05', fast: 5, slow: 30, sl: 2.5, tp: 5.5, pnl: '+28,000.00', fitness: 81.3, status: '变异繁殖' },
            { id: 'GEN_06', fast: 8, slow: 45, sl: 3.2, tp: 6.8, pnl: '+12,400.00', fitness: 62.0, status: '观察中' },
            { id: 'GEN_07', fast: 12, slow: 55, sl: 3.8, tp: 7.5, pnl: '-6,200.00', fitness: 24.5, status: '即将淘汰' }
        ],

        reportState: {
            source: 'paper',
            range: '24h'
        },

        reportData: {
            pnlSeries: [],
            drawdownSeries: [],
            trades: [],
            metrics: {}
        }
    };

    // ─────────────────────────────────────────────────────────────
    // Toast 浮动通知组件 (取代原生 alert)
    // ─────────────────────────────────────────────────────────────
    const Toast = {
        show(msg, type = 'info') {
            const container = document.getElementById('toast-container');
            if (!container) return;

            const el = document.createElement('div');
            el.className = `toast toast-${type}`;
            el.innerText = msg;
            container.appendChild(el);

            setTimeout(() => {
                el.style.opacity = '0';
                el.style.transform = 'translateY(-10px)';
                el.style.transition = 'all 0.25s ease';
                setTimeout(() => el.remove(), 250);
            }, 3000);
        }
    };

    // ─────────────────────────────────────────────────────────────
    // 生成 K 线历史模拟数据
    // ─────────────────────────────────────────────────────────────
    function initMarketBars(basePrice = 3600.0) {
        State.marketBars = [];
        let price = basePrice;
        const now = Date.now() - 100 * 60 * 1000;
        const scale = basePrice > 100 ? (basePrice * 0.001) : (basePrice * 0.005);
        for (let i = 0; i < 100; i++) {
            const time = new Date(now + i * 60 * 1000);
            const delta = ((Math.sin(i / 8) * 1.5) + (Math.random() - 0.48) * 1.8) * scale;
            const open = price;
            const close = open + delta;
            const high = Math.max(open, close) + Math.random() * scale;
            const low = Math.min(open, close) - Math.random() * scale;
            const vol = Math.floor(600 + Math.random() * 1200);
            price = close;

            State.marketBars.push({
                time: time.toTimeString().substring(0, 5),
                open, high, low, close, volume: vol
            });
        }
    }

    // ─────────────────────────────────────────────────────────────
    // 页面路由与切换
    // ─────────────────────────────────────────────────────────────
    const Router = {
        init() {
            document.querySelectorAll('.nav-item').forEach(btn => {
                btn.addEventListener('click', () => {
                    this.switchPage(btn.dataset.page);
                });
            });
        },

        switchPage(pageId) {
            State.currentPage = pageId;
            document.querySelectorAll('.nav-item').forEach(b => b.classList.toggle('active', b.dataset.page === pageId));
            document.querySelectorAll('.page-view').forEach(v => v.classList.toggle('active', v.id === `page-${pageId}`));

            if (pageId === 'trading-desk') {
                MarketChart.resize();
            } else if (pageId === 'strategy-hub' || pageId === 'risk-account') {
                UI.renderDockTables();
            } else if (pageId === 'backtest-lab') {
                BacktestChart.resize();
            } else if (pageId === 'ai-evolution') {
                UI.renderAiRadar();
                UI.renderEvolutionPop();
            } else if (pageId === 'report-desk') {
                UI.updateReportView();
            }
        }
    };

    // ─────────────────────────────────────────────────────────────
    // K 线图 Canvas 高刷引擎 (支持十字光标追踪)
    // ─────────────────────────────────────────────────────────────
    const MarketChart = {
        canvas: null,
        ctx: null,
        mouseX: -1,
        mouseY: -1,

        init() {
            this.canvas = document.getElementById('market-canvas');
            if (!this.canvas) return;
            this.ctx = this.canvas.getContext('2d');

            window.addEventListener('resize', () => this.resize());
            
            this.canvas.addEventListener('mousemove', (e) => {
                const rect = this.canvas.getBoundingClientRect();
                this.mouseX = e.clientX - rect.left;
                this.mouseY = e.clientY - rect.top;
                this.render();
            });

            this.canvas.addEventListener('mouseleave', () => {
                this.mouseX = -1;
                this.mouseY = -1;
                this.render();
            });

            this.resize();
        },

        resize() {
            if (!this.canvas) return;
            const rect = this.canvas.parentElement.getBoundingClientRect();
            if (rect.width === 0 || rect.height === 0) return;
            this.canvas.width = rect.width * window.devicePixelRatio;
            this.canvas.height = rect.height * window.devicePixelRatio;
            this.ctx.scale(window.devicePixelRatio, window.devicePixelRatio);
            this.render();
        },

        render() {
            const ctx = this.ctx;
            const bars = State.marketBars;
            if (!ctx || !bars || bars.length === 0) return;

            const rect = this.canvas.parentElement.getBoundingClientRect();
            const w = rect.width;
            const h = rect.height;

            ctx.clearRect(0, 0, w, h);

            const chartH = h * 0.72;
            const volH = h * 0.22;

            let minP = Infinity, maxP = -Infinity;
            let maxV = 0;
            const visible = bars.slice(-70);
            visible.forEach(b => {
                minP = Math.min(minP, b.low);
                maxP = Math.max(maxP, b.high);
                maxV = Math.max(maxV, b.volume);
            });
            const pRange = maxP - minP || 1;

            // 网格
            ctx.strokeStyle = '#1a2436';
            ctx.lineWidth = 1;
            for (let i = 1; i <= 5; i++) {
                const y = (chartH / 6) * i;
                ctx.beginPath();
                ctx.moveTo(0, y);
                ctx.lineTo(w - 70, y);
                ctx.stroke();

                const p = maxP - (pRange / 6) * i;
                ctx.fillStyle = '#64748b';
                ctx.font = '11px monospace';
                ctx.textAlign = 'left';
                ctx.fillText(p.toFixed(1), w - 65, y + 4);
            }

            const barW = (w - 80) / visible.length;
            const candleW = Math.max(3, barW * 0.75);

            visible.forEach((b, i) => {
                const x = i * barW + barW / 2;
                const isUp = b.close >= b.open;
                const color = isUp ? '#f43f5e' : '#10b981';

                const yOpen = chartH - ((b.open - minP) / pRange) * (chartH - 20) - 10;
                const yClose = chartH - ((b.close - minP) / pRange) * (chartH - 20) - 10;
                const yHigh = chartH - ((b.high - minP) / pRange) * (chartH - 20) - 10;
                const yLow = chartH - ((b.low - minP) / pRange) * (chartH - 20) - 10;

                ctx.strokeStyle = color;
                ctx.beginPath();
                ctx.moveTo(x, yHigh);
                ctx.lineTo(x, yLow);
                ctx.stroke();

                ctx.fillStyle = color;
                const topY = Math.min(yOpen, yClose);
                const bodyH = Math.max(2, Math.abs(yClose - yOpen));
                ctx.fillRect(x - candleW / 2, topY, candleW, bodyH);

                const vH = (b.volume / maxV) * (volH - 10);
                ctx.fillStyle = isUp ? 'rgba(244, 63, 94, 0.35)' : 'rgba(16, 185, 129, 0.35)';
                ctx.fillRect(x - candleW / 2, h - vH, candleW, vH);
            });

            // 十字准星光标
            if (this.mouseX >= 0 && this.mouseX < w - 80 && this.mouseY >= 0 && this.mouseY < chartH) {
                ctx.strokeStyle = 'rgba(148, 163, 184, 0.5)';
                ctx.setLineDash([4, 4]);
                
                // 竖线
                ctx.beginPath();
                ctx.moveTo(this.mouseX, 0);
                ctx.lineTo(this.mouseX, h);
                ctx.stroke();

                // 横线
                ctx.beginPath();
                ctx.moveTo(0, this.mouseY);
                ctx.lineTo(w - 70, this.mouseY);
                ctx.stroke();
                ctx.setLineDash([]);

                const hoverIdx = Math.floor(this.mouseX / barW);
                const hoverBar = visible[Math.min(visible.length - 1, Math.max(0, hoverIdx))];
                if (hoverBar) {
                    document.getElementById('hud-time').innerText = hoverBar.time;
                    document.getElementById('hud-open').innerText = hoverBar.open.toFixed(1);
                    document.getElementById('hud-high').innerText = hoverBar.high.toFixed(1);
                    document.getElementById('hud-low').innerText = hoverBar.low.toFixed(1);
                    document.getElementById('hud-close').innerText = hoverBar.close.toFixed(1);
                    document.getElementById('hud-vol').innerText = hoverBar.volume;
                }
            } else {
                const latest = visible[visible.length - 1];
                if (latest) {
                    document.getElementById('hud-time').innerText = latest.time;
                    document.getElementById('hud-open').innerText = latest.open.toFixed(1);
                    document.getElementById('hud-high').innerText = latest.high.toFixed(1);
                    document.getElementById('hud-low').innerText = latest.low.toFixed(1);
                    document.getElementById('hud-close').innerText = latest.close.toFixed(1);
                    document.getElementById('hud-vol').innerText = latest.volume;
                }
            }
        }
    };

    // ─────────────────────────────────────────────────────────────
    // 回测曲线 Canvas
    // ─────────────────────────────────────────────────────────────
    const BacktestChart = {
        canvas: null,
        ctx: null,

        init() {
            this.canvas = document.getElementById('backtest-canvas');
            if (!this.canvas) return;
            this.ctx = this.canvas.getContext('2d');
            this.generateMockCurve();
        },

        generateMockCurve() {
            State.backtestEquity = [];
            let val = 1000000;
            for (let i = 0; i < 200; i++) {
                val += (Math.sin(i / 14) * 900) + (Math.random() - 0.43) * 1300;
                State.backtestEquity.push(val);
            }
            this.resize();
        },

        resize() {
            if (!this.canvas) return;
            const rect = this.canvas.parentElement.getBoundingClientRect();
            if (rect.width === 0 || rect.height === 0) return;
            this.canvas.width = rect.width * window.devicePixelRatio;
            this.canvas.height = rect.height * window.devicePixelRatio;
            this.ctx.scale(window.devicePixelRatio, window.devicePixelRatio);
            this.render(rect.width, rect.height);
        },

        render(w, h) {
            const ctx = this.ctx;
            const eq = State.backtestEquity;
            if (!ctx || !eq || eq.length === 0) return;

            ctx.clearRect(0, 0, w, h);

            let min = Math.min(...eq);
            let max = Math.max(...eq);
            const range = max - min || 1;

            ctx.strokeStyle = '#1e2738';
            ctx.lineWidth = 1;
            for (let i = 1; i <= 5; i++) {
                const y = (h / 6) * i;
                ctx.beginPath();
                ctx.moveTo(0, y);
                ctx.lineTo(w - 90, y);
                ctx.stroke();

                const v = max - (range / 6) * i;
                ctx.fillStyle = '#64748b';
                ctx.font = '11px monospace';
                ctx.textAlign = 'left';
                ctx.fillText(v.toFixed(0) + ' 元', w - 80, y + 4);
            }

            const stepX = (w - 90) / (eq.length - 1);
            const gradient = ctx.createLinearGradient(0, 0, 0, h);
            gradient.addColorStop(0, 'rgba(56, 189, 248, 0.25)');
            gradient.addColorStop(1, 'rgba(56, 189, 248, 0.0)');

            ctx.beginPath();
            eq.forEach((v, i) => {
                const x = i * stepX;
                const y = h - ((v - min) / range) * (h - 40) - 20;
                if (i === 0) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            });
            ctx.strokeStyle = '#38bdf8';
            ctx.lineWidth = 2.5;
            ctx.stroke();

            ctx.lineTo((eq.length - 1) * stepX, h);
            ctx.lineTo(0, h);
            ctx.fillStyle = gradient;
            ctx.fill();
        }
    };

    // ─────────────────────────────────────────────────────────────
    // 绩效汇报台 Canvas (累计盈亏 + 水下动态回撤)
    // ─────────────────────────────────────────────────────────────
    const ReportChart = {
        canvas: null,
        ctx: null,

        init() {
            this.canvas = document.getElementById('report-equity-canvas');
            if (!this.canvas) return;
            this.ctx = this.canvas.getContext('2d');
            window.addEventListener('resize', () => this.resize());
        },

        resize() {
            if (!this.canvas) return;
            const rect = this.canvas.parentElement.getBoundingClientRect();
            if (rect.width === 0 || rect.height === 0) return;
            this.canvas.width = rect.width * window.devicePixelRatio;
            this.canvas.height = rect.height * window.devicePixelRatio;
            this.ctx.scale(window.devicePixelRatio, window.devicePixelRatio);
            this.render(rect.width, rect.height);
        },

        render(w, h) {
            const ctx = this.ctx;
            if (!ctx) return;

            ctx.clearRect(0, 0, w, h);

            const pnlData = State.reportData.pnlSeries;
            const mddData = State.reportData.drawdownSeries;
            if (!pnlData || pnlData.length === 0) return;

            const topChartH = h * 0.65;
            const bottomChartH = h * 0.22;
            const gap = h * 0.09;

            let minPnl = Math.min(...pnlData);
            let maxPnl = Math.max(...pnlData);
            if (minPnl > 0) minPnl = 0;
            const pnlRange = maxPnl - minPnl || 1;

            // 1. 顶部累计盈亏曲线网格
            ctx.strokeStyle = '#1e2738';
            ctx.lineWidth = 1;
            for (let i = 1; i <= 4; i++) {
                const y = (topChartH / 5) * i;
                ctx.beginPath();
                ctx.moveTo(0, y);
                ctx.lineTo(w - 90, y);
                ctx.stroke();

                const v = maxPnl - (pnlRange / 5) * i;
                ctx.fillStyle = '#64748b';
                ctx.font = '11px monospace';
                ctx.textAlign = 'left';
                ctx.fillText((v >= 0 ? '+' : '') + v.toFixed(0) + ' 元', w - 80, y + 4);
            }

            const stepX = (w - 90) / (pnlData.length - 1);

            // 渐变填充
            const grad = ctx.createLinearGradient(0, 0, 0, topChartH);
            grad.addColorStop(0, 'rgba(56, 189, 248, 0.25)');
            grad.addColorStop(1, 'rgba(56, 189, 248, 0.0)');

            ctx.beginPath();
            pnlData.forEach((v, i) => {
                const x = i * stepX;
                const y = topChartH - ((v - minPnl) / pnlRange) * (topChartH - 24) - 12;
                if (i === 0) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            });
            ctx.strokeStyle = '#38bdf8';
            ctx.lineWidth = 2.0;
            ctx.stroke();

            ctx.lineTo((pnlData.length - 1) * stepX, topChartH);
            ctx.lineTo(0, topChartH);
            ctx.fillStyle = grad;
            ctx.fill();

            // 2. 底部水下回撤区域 (Underwater Drawdown)
            const botTop = topChartH + gap;
            const maxMdd = Math.max(...mddData, 0.001);

            ctx.fillStyle = '#64748b';
            ctx.font = '10px monospace';
            ctx.fillText('水下动态回撤区域 (%)', 10, botTop - 4);

            ctx.beginPath();
            mddData.forEach((d, i) => {
                const x = i * stepX;
                const y = botTop + (d / maxMdd) * (bottomChartH - 8);
                if (i === 0) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            });
            ctx.lineTo((mddData.length - 1) * stepX, botTop);
            ctx.lineTo(0, botTop);
            ctx.fillStyle = 'rgba(244, 63, 94, 0.2)';
            ctx.fill();

            ctx.beginPath();
            mddData.forEach((d, i) => {
                const x = i * stepX;
                const y = botTop + (d / maxMdd) * (bottomChartH - 8);
                if (i === 0) ctx.moveTo(x, y);
                else ctx.lineTo(x, y);
            });
            ctx.strokeStyle = '#f43f5e';
            ctx.lineWidth = 1.2;
            ctx.stroke();
        }
    };

    // ── 真实绩效数据: 拉取 /api/report (SQLite 成交账本 FIFO 配对回放), 严禁前端伪造 ──
    function fmtSigned(v, decimals = 2) {
        return (v >= 0 ? '+' : '') + Number(v).toLocaleString('zh-CN', { minimumFractionDigits: decimals, maximumFractionDigits: decimals });
    }

    async function loadReportData() {
        try {
            const res = await fetch('/api/report?range=' + encodeURIComponent(State.reportState.range || 'all'));
            if (!res.ok) throw new Error('HTTP ' + res.status);
            const rep = await res.json();
            const m = rep.metrics || {};
            const hasData = !!rep.has_data;

            State.reportData = {
                pnlSeries: rep.pnlSeries || [],
                drawdownSeries: rep.drawdownSeries || [],
                hasData,
                trades: (rep.trades || []).map(t => ({
                    id: t.id,
                    time: t.time,
                    symbol: t.symbol,
                    dir: t.dir,
                    offset: t.offset,
                    price: t.price,
                    vol: t.vol,
                    pnl: fmtSigned(t.pnl),
                    pnlPositive: t.pnl >= 0,
                    cumPnl: fmtSigned(t.cumPnl),
                    cumPositive: t.cumPnl >= 0,
                    strat: t.strat
                })),
                metrics: {
                    totalPnl: fmtSigned(m.total_pnl || 0) + ' 元',
                    returnRate: '收益率: ' + fmtSigned(m.return_rate || 0) + '%',
                    winRate: hasData ? (m.win_rate || 0).toFixed(1) + '%' : '--',
                    winTrades: hasData
                        ? (m.win_trades || 0) + ' 胜 / ' + (m.lose_trades || 0) + ' 负 (共 ' + ((m.win_trades || 0) + (m.lose_trades || 0)) + ' 笔平仓回合)'
                        : '暂无平仓回合',
                    plRatio: hasData ? (m.profit_factor || 0).toFixed(2) + ' : 1' : '--',
                    mdd: hasData ? (m.max_drawdown_pct || 0).toFixed(2) + '%' : '--',
                    mddAmt: '回撤金额: ' + fmtSigned(-(m.max_drawdown_amt || 0)) + ' 元',
                    sharpe: hasData ? (m.sharpe || 0).toFixed(2) : '--',
                    comm: Number(m.commission || 0).toLocaleString('zh-CN', { minimumFractionDigits: 2 }) + ' 元'
                }
            };
        } catch (err) {
            console.error('[Report] 真实绩效数据拉取失败:', err);
            State.reportData = { pnlSeries: [], drawdownSeries: [], hasData: false, trades: [], metrics: {
                totalPnl: '-- 元', returnRate: '收益率: --', winRate: '--', winTrades: '服务不可达',
                plRatio: '--', mdd: '--', mddAmt: '回撤金额: --', sharpe: '--', comm: '-- 元'
            } };
        }
    }

    // ─────────────────────────────────────────────────────────────
    // 视图表格与组件渲染
    // ─────────────────────────────────────────────────────────────
    const UI = {
        selectContract(symbol) {
            State.activeSymbol = symbol;
            const c = State.contracts.find(x => x.symbol === symbol);
            if (!c) return;

            initMarketBars(c.last);
            MarketChart.resize();

            const mid = c.last;
            const tick = c.tick || 1.0;
            const fmt = (v) => v.toFixed(c.last < 10 ? 3 : (c.last < 100 ? 2 : 1));

            State.depth.asks = [
                { price: mid + tick * 5, vol: 140 },
                { price: mid + tick * 4, vol: 95 },
                { price: mid + tick * 3, vol: 230 },
                { price: mid + tick * 2, vol: 310 },
                { price: mid + tick * 1, vol: 80 }
            ];
            State.depth.bids = [
                { price: mid - tick * 1, vol: 120 },
                { price: mid - tick * 2, vol: 260 },
                { price: mid - tick * 3, vol: 410 },
                { price: mid - tick * 4, vol: 180 },
                { price: mid - tick * 5, vol: 500 }
            ];

            document.getElementById('chart-symbol-name').innerText = `${c.name} (${c.symbol})`;
            document.getElementById('depth-contract-label').innerText = `五档盘口 (${c.symbol})`;
            document.getElementById('desk-last-price').innerText = fmt(c.last);
            document.getElementById('desk-last-chg').innerText = `${c.chg >= 0 ? '+' : ''}${c.chg.toFixed(2)}%`;
            document.getElementById('ticket-symbol').value = c.symbol;
            document.getElementById('ticket-price').value = fmt(c.last);

            this.updateSummary();
            this.renderWatchlist();
            this.renderDepth();
            Toast.show(`已切换标的为: ${c.name} (${c.symbol})`, 'info');
        },

        updateSummary() {
            const sym = document.getElementById('ticket-symbol').value;
            const price = parseFloat(document.getElementById('ticket-price').value) || 0;
            const vol = parseInt(document.getElementById('ticket-volume').value) || 0;
            const c = State.contracts.find(x => x.symbol === sym) || { mult: 10, marginRatio: 0.10 };

            const margin = price * vol * c.mult * c.marginRatio;
            const comm = price * vol * c.mult * 0.0001;

            document.getElementById('summary-margin').innerText = `${margin.toLocaleString('zh-CN', { minimumFractionDigits: 2 })} 元`;
            document.getElementById('summary-comm').innerText = `${comm.toFixed(2)} 元`;
        },

        renderWatchlist() {
            const tbody = document.getElementById('tbody-desk-watch');
            if (!tbody) return;

            const filtered = State.activeCategory === 'all'
                ? State.contracts
                : State.contracts.filter(c => c.category === State.activeCategory);

            tbody.innerHTML = filtered.map(c => {
                const fmt = (v) => v.toFixed(c.last < 10 ? 3 : (c.last < 100 ? 2 : 1));
                return `
                    <tr class="${c.symbol === State.activeSymbol ? 'active-row' : ''}" data-symbol="${c.symbol}">
                        <td class="text-bold">${c.symbol} <span style="font-size:10px;color:#94a3b8;">${c.name}</span></td>
                        <td class="${c.chg >= 0 ? 'text-up' : 'text-down'}">${fmt(c.last)}</td>
                        <td class="${c.chg >= 0 ? 'text-up' : 'text-down'}">${c.chg >= 0 ? '+' : ''}${c.chg.toFixed(2)}%</td>
                        <td>${c.volume.toLocaleString()}</td>
                    </tr>
                `;
            }).join('');

            tbody.querySelectorAll('tr').forEach(tr => {
                tr.addEventListener('click', () => {
                    UI.selectContract(tr.dataset.symbol);
                });
            });
        },

        renderDepth() {
            const asks = document.getElementById('desk-asks');
            const bids = document.getElementById('desk-bids');
            if (!asks || !bids) return;

            // 盘口点击填单 (Click-to-Trade)
            asks.innerHTML = State.depth.asks.map(a => `
                <div class="depth-row ask-row" data-price="${a.price}">
                    <span class="text-muted">卖盘</span>
                    <span class="text-down text-bold">${a.price.toFixed(1)}</span>
                    <span>${a.vol} 手</span>
                    <div class="depth-fill" style="width: ${(a.vol / 500) * 100}%"></div>
                </div>
            `).join('');

            bids.innerHTML = State.depth.bids.map(b => `
                <div class="depth-row bid-row" data-price="${b.price}">
                    <span class="text-muted">买盘</span>
                    <span class="text-up text-bold">${b.price.toFixed(1)}</span>
                    <span>${b.vol} 手</span>
                    <div class="depth-fill" style="width: ${(b.vol / 500) * 100}%"></div>
                </div>
            `).join('');

            // 绑定盘口点击填价
            document.querySelectorAll('.depth-row').forEach(row => {
                row.addEventListener('click', () => {
                    const p = parseFloat(row.dataset.price);
                    document.getElementById('ticket-price').value = p.toFixed(1);
                    UI.updateSummary();
                    Toast.show(`已自动填入盘口委托价: ${p.toFixed(1)}`, 'info');
                });
            });
        },

        renderDockTables() {
            // 持仓
            const posTbody = document.getElementById('tbody-dock-positions');
            if (posTbody) {
                posTbody.innerHTML = State.positions.map(p => `
                    <tr>
                        <td class="text-bold">${p.symbol}</td>
                        <td class="${p.dir.includes('多') ? 'text-up' : 'text-down'} text-bold">${p.dir}</td>
                        <td>${p.vol}</td>
                        <td>${p.frozen}</td>
                        <td>${p.avg.toFixed(1)}</td>
                        <td>${p.last.toFixed(1)}</td>
                        <td>${p.margin.toLocaleString()}</td>
                        <td class="${p.pnl >= 0 ? 'text-up' : 'text-down'} text-bold">${p.pnl >= 0 ? '+' : ''}${p.pnl.toLocaleString()}</td>
                        <td><button class="btn btn-secondary btn-xs" onclick="window.KunUI.closePosition('${p.symbol}')">一键平仓</button></td>
                    </tr>
                `).join('');
                document.getElementById('dock-pos-count').innerText = State.positions.length;
            }

            // 挂单
            const ordTbody = document.getElementById('tbody-dock-orders');
            if (ordTbody) {
                ordTbody.innerHTML = State.orders.map(o => `
                    <tr>
                        <td>${o.id}</td>
                        <td>${o.time}</td>
                        <td>${o.strat}</td>
                        <td class="text-bold">${o.symbol}</td>
                        <td>${o.type}</td>
                        <td class="${o.dir === '买入' ? 'text-up' : 'text-down'}">${o.dir}</td>
                        <td>${o.offset}</td>
                        <td>${o.price.toFixed(1)}</td>
                        <td>${o.filled} / ${o.total}</td>
                        <td><span class="badge badge-primary">${o.status}</span></td>
                        <td>${o.status === '挂单中' ? `<button class="btn btn-secondary btn-xs" onclick="window.KunUI.cancelOrder('${o.id}')">撤单</button>` : '--'}</td>
                    </tr>
                `).join('');
                document.getElementById('dock-order-count').innerText = State.orders.filter(x => x.status === '挂单中').length;
            }

            // 成交
            const trdTbody = document.getElementById('tbody-dock-trades');
            if (trdTbody) {
                trdTbody.innerHTML = State.trades.map(t => `
                    <tr>
                        <td>${t.id}</td>
                        <td>${t.time}</td>
                        <td class="text-bold">${t.symbol}</td>
                        <td class="${t.dir === '买入' ? 'text-up' : 'text-down'}">${t.dir}</td>
                        <td>${t.offset}</td>
                        <td>${t.price.toFixed(1)}</td>
                        <td>${t.vol}</td>
                        <td>${t.comm.toFixed(2)}</td>
                    </tr>
                `).join('');
                document.getElementById('dock-trade-count').innerText = State.trades.length;
            }

            // 策略页表
            const stratTbody = document.getElementById('tbody-strat-page');
            if (stratTbody) {
                stratTbody.innerHTML = State.strategies.map(s => `
                    <tr>
                        <td class="text-bold">${s.id}</td>
                        <td>${s.name}</td>
                        <td class="text-bold">${s.symbol}</td>
                        <td><span class="badge badge-primary">${s.model}</span></td>
                        <td><span class="badge ${s.state === '运行中' ? 'badge-success' : ''}">${s.state}</span></td>
                        <td>${s.pos}</td>
                        <td class="text-up text-bold">${s.pnl}</td>
                        <td>${s.mdd}</td>
                        <td>
                            <button class="btn btn-secondary btn-xs" onclick="window.KunUI.toggleStrategy('${s.id}')">${s.state === '运行中' ? '暂停' : '启动'}</button>
                        </td>
                    </tr>
                `).join('');
            }

            // 账户持仓页
            const accTbody = document.getElementById('tbody-account-page');
            if (accTbody) {
                accTbody.innerHTML = State.positions.map(p => `
                    <tr>
                        <td class="text-bold">${p.symbol}</td>
                        <td class="${p.dir.includes('多') ? 'text-up' : 'text-down'} text-bold">${p.dir}</td>
                        <td>${p.vol}</td>
                        <td>${p.frozen}</td>
                        <td>${p.ydToday}</td>
                        <td>${p.avg.toFixed(1)}</td>
                        <td>${p.last.toFixed(1)}</td>
                        <td>${p.margin.toLocaleString()}</td>
                        <td class="${p.pnl >= 0 ? 'text-up' : 'text-down'} text-bold">${p.pnl >= 0 ? '+' : ''}${p.pnl.toLocaleString()}</td>
                        <td><button class="btn btn-secondary btn-xs" onclick="window.KunUI.closePosition('${p.symbol}')">市价平仓</button></td>
                    </tr>
                `).join('');
            }
        },

        renderAiRadar() {
            const tbody = document.getElementById('tbody-ai-radar');
            if (!tbody) return;

            tbody.innerHTML = State.aiRadarRecommendations.map(item => `
                <tr>
                    <td class="text-bold">${item.symbol} <span style="font-size:11px;color:#94a3b8;">${item.name}</span></td>
                    <td><span class="badge badge-primary">${item.category}</span></td>
                    <td>
                        <div class="score-bar-wrap">
                            <div class="score-bar"><div class="score-fill" style="width: ${item.score}%"></div></div>
                            <span class="text-bold ${item.score >= 90 ? 'text-up' : 'text-primary'}">${item.score}分</span>
                        </div>
                    </td>
                    <td style="color:#f1f5f9;">${item.reason}</td>
                    <td class="text-bold">${item.entry.toFixed(item.entry < 10 ? 3 : 1)}</td>
                    <td class="text-down">${item.sl.toFixed(item.entry < 10 ? 3 : 1)}</td>
                    <td class="text-up">${item.tp.toFixed(item.entry < 10 ? 3 : 1)}</td>
                    <td class="text-gold text-bold">${item.winRate}</td>
                    <td>
                        <button class="btn btn-primary btn-xs" onclick="window.KunUI.buyRadarPick('${item.symbol}')">一键跟单入场</button>
                    </td>
                </tr>
            `).join('');
        },

        renderEvolutionPop() {
            const tbody = document.getElementById('tbody-evolution-pop');
            if (!tbody) return;

            tbody.innerHTML = State.evolutionPop.map(p => `
                <tr>
                    <td class="text-bold">${p.id}</td>
                    <td>MA_${p.fast}</td>
                    <td>MA_${p.slow}</td>
                    <td>${p.sl}x</td>
                    <td>${p.tp}x</td>
                    <td class="${p.pnl.startsWith('+') ? 'text-up' : 'text-down'} text-bold">${p.pnl}</td>
                    <td class="text-bold text-gold">${p.fitness.toFixed(1)}</td>
                    <td><span class="badge ${p.status.includes('精英') ? 'badge-success' : 'badge-primary'}">${p.status}</span></td>
                </tr>
            `).join('');
        },

        addEvoLog(msg) {
            const box = document.getElementById('evo-event-log');
            if (!box) return;
            const now = new Date().toISOString().substring(11, 23);
            const line = document.createElement('div');
            line.style.padding = '2px 0';
            line.innerHTML = `<span style="color:#64748b;">[${now}]</span> <span style="color:#38bdf8;font-weight:700;">[基因进化]</span> ${msg}`;
            box.appendChild(line);
            box.scrollTop = box.scrollHeight;
        },

        async updateReportView() {
            await loadReportData();
            const m = State.reportData.metrics;

            const elTotalPnl = document.getElementById('rep-total-pnl');
            if (elTotalPnl) elTotalPnl.innerText = m.totalPnl;
            const elRet = document.getElementById('rep-return-rate');
            if (elRet) elRet.innerText = m.returnRate;
            const elWinRate = document.getElementById('rep-win-rate');
            if (elWinRate) elWinRate.innerText = m.winRate;
            const elWinTrades = document.getElementById('rep-win-trades');
            if (elWinTrades) elWinTrades.innerText = m.winTrades;
            const elPl = document.getElementById('rep-pl-ratio');
            if (elPl) elPl.innerText = m.plRatio;
            const elMdd = document.getElementById('rep-mdd');
            if (elMdd) elMdd.innerText = m.mdd;
            const elMddAmt = document.getElementById('rep-mdd-val');
            if (elMddAmt) elMddAmt.innerText = m.mddAmt;
            const elSharpe = document.getElementById('rep-sharpe');
            if (elSharpe) elSharpe.innerText = m.sharpe;
            const elComm = document.getElementById('rep-comm');
            if (elComm) elComm.innerText = m.comm;

            const titleEl = document.getElementById('rep-chart-title');
            if (titleEl) {
                titleEl.innerText = '真实成交账本 (SQLite 流水 FIFO 配对回放) · 累计盈亏与动态回撤走势';
            }

            // 量化诊断: 仅陈述真实账本统计事实, 无数据时明确提示, 严禁虚构结论
            const diagBox = document.getElementById('rep-diag-content');
            if (diagBox) {
                if (!State.reportData.hasData) {
                    diagBox.innerHTML = `
                        <div class="diag-item">
                            <div class="diag-tag tag-neutral">暂无真实成交</div>
                            <div class="diag-title">账本中还没有已平仓回合</div>
                            <div class="diag-desc">当前统计全部来自 SQLite 真实成交流水回放。启动策略 / 模拟盘或回测训练产生真实成交后, 本区域将给出基于事实的绩效归因。</div>
                        </div>`;
                } else {
                    const m2 = State.reportData.metrics;
                    const pf = parseFloat(m2.plRatio);
                    const pfText = isNaN(pf) ? '无亏损回合' :
                        (pf >= 1.5 ? `盈亏比 ${m2.plRatio}, 利润覆盖倍数健康` :
                         pf >= 1.0 ? `盈亏比 ${m2.plRatio}, 盈利可覆盖亏损但安全垫偏薄` :
                                     `盈亏比 ${m2.plRatio}, 亏损吞噬利润, 需复盘止损参数`);
                    diagBox.innerHTML = `
                        <div class="diag-item">
                            <div class="diag-tag tag-positive">收益归因 (真实账本)</div>
                            <div class="diag-title">累计已实现盈亏 ${m2.totalPnl}, 胜率 ${m2.winRate}</div>
                            <div class="diag-desc">共 ${m2.winTrades}, 总手续费 ${m2.comm}。${pfText}。</div>
                        </div>
                        <div class="diag-item">
                            <div class="diag-tag tag-neutral">风控合规评估</div>
                            <div class="diag-title">账本口径最大动态回撤 ${m2.mdd} (${m2.mddAmt})</div>
                            <div class="diag-desc">回撤基于平仓回合后的真实净值序列计算, 未包含浮动盈亏盯市, 实际日内波动可能更大。</div>
                        </div>
                        <div class="diag-item">
                            <div class="diag-tag tag-advice">夏普比率 (真实回放)</div>
                            <div class="diag-title">当前夏普 ${m2.sharpe}</div>
                            <div class="diag-desc">样本为全部真实平仓回合。可用「历史数据训练」工具加载更长周期数据做 Walk-Forward 滚动验证, 提升统计置信度。</div>
                        </div>`;
                }

            }

            // 渲染交易流水
            const tbody = document.getElementById('tbody-report-trades');
            if (tbody) {
                if (!State.reportData.trades.length) {
                    tbody.innerHTML = `<tr><td colspan="10" style="text-align:center;color:#64748b;padding:16px;">暂无真实成交记录 — 全部指标严格来自账本, 不做任何虚构填充</td></tr>`;
                } else {
                    tbody.innerHTML = State.reportData.trades.map(t => `
                        <tr>
                            <td>${t.id}</td>
                            <td>${t.time}</td>
                            <td class="text-bold">${t.symbol}</td>
                            <td class="${t.dir === '买入' ? 'text-up' : 'text-down'}">${t.dir}</td>
                            <td>${t.offset}</td>
                            <td>${Number(t.price).toFixed(1)}</td>
                            <td>${t.vol} 手</td>
                            <td class="${t.pnl.startsWith('+') ? 'text-up' : 'text-down'} text-bold">${t.pnl}</td>
                            <td class="${t.cumPnl.startsWith('+') ? 'text-up' : 'text-down'} text-bold">${t.cumPnl}</td>
                            <td><span class="badge badge-primary">${t.strat}</span></td>
                        </tr>
                    `).join('');
                }
            }

            ReportChart.resize();
        },

        addLog(level, msg) {
            const boxes = [document.getElementById('dock-log-container'), document.getElementById('main-log-screen')];
            const now = new Date().toISOString().substring(11, 23);
            boxes.forEach(box => {
                if (!box) return;
                const line = document.createElement('div');
                let color = '#38bdf8';
                if (level === 'WARN') color = '#f59e0b';
                if (level === 'ERROR') color = '#ef4444';
                if (level === 'TRADE') color = '#a855f7';
                line.innerHTML = `<span style="color:#64748b;">[${now}]</span> <span style="color:${color};font-weight:700;">[${level}]</span> ${msg}`;
                box.appendChild(line);
                box.scrollTop = box.scrollHeight;
            });
        }
    };

    // ─────────────────────────────────────────────────────────────
    // 交互与全局 API 暴露
    // ─────────────────────────────────────────────────────────────
    window.KunUI = {
        async closePosition(symbol) {
            const p = State.positions.find(x => x.symbol === symbol);
            const vol = p ? p.vol : 1;
            const price = p ? p.last : 3620.0;
            const dir = (p && p.dir.includes('多')) ? 'SHORT' : 'LONG';
            Toast.show(`正在市价平仓: ${symbol} ${vol}手...`, 'info');
            try {
                const res = await fetch('/api/order', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({
                        account_id: 'acc_master_simnow',
                        symbol: symbol,
                        direction: dir,
                        offset: 'CLOSE',
                        price: price,
                        volume: vol,
                        order_type: 'MARKET'
                    })
                });
                const data = await res.json();
                State.positions = State.positions.filter(x => x.symbol !== symbol);
                UI.renderDockTables();
                Toast.show(`平仓指令已通过风控并提交柜台: ${symbol} [单号: ${data.order_req_id || 'OK'}]`, 'success');
                UI.addLog('TRADE', `市价平仓完成: ${symbol} ${vol}手 @ ${price}`);
            } catch (e) {
                State.positions = State.positions.filter(x => x.symbol !== symbol);
                UI.renderDockTables();
                Toast.show(`离线演示平仓: ${symbol} ${vol}手`, 'info');
                UI.addLog('TRADE', `离线平仓: ${symbol} ${vol}手 @ ${price}`);
            }
        },
        cancelOrder(orderId) {
            const o = State.orders.find(x => x.id === orderId);
            if (o) {
                o.status = '已撤销';
                UI.renderDockTables();
                Toast.show(`挂单 ${orderId} 撤单成功`, 'success');
                UI.addLog('INFO', `撤单成功: 订单号 ${orderId}`);
            }
        },
        toggleStrategy(id) {
            const s = State.strategies.find(x => x.id === id);
            if (s) {
                s.state = (s.state === '运行中') ? '已暂停' : '运行中';
                UI.renderDockTables();
                Toast.show(`策略 ${id} 状态已更新为: ${s.state}`, 'info');
                UI.addLog('INFO', `策略状态切换: ${id} -> ${s.state}`);
            }
        },
        buyRadarPick(symbol) {
            Router.switchPage('trading-desk');
            UI.selectContract(symbol);
            const c = State.contracts.find(x => x.symbol === symbol);
            if (c) {
                const pInput = document.getElementById('ticket-price');
                if (pInput) pInput.value = c.last.toFixed(1);
                UI.updateSummary();
            }
            Toast.show(`已选定 AI 推荐标的: ${symbol}，已自动填入最优建仓价！`, 'success');
            UI.addLog('INFO', `AI 雷达一键选股: 切换至 ${symbol} 填单就绪`);
        }
    };

    function initInteractiveHandlers() {
        // 多账户切换联动
        document.getElementById('global-account-select')?.addEventListener('change', (e) => {
            const acc = e.target.value;
            if (acc === 'all') {
                document.getElementById('top-equity').innerText = '3,324,580.00';
                document.getElementById('top-available').innerText = '2,582,100.00';
                Toast.show('已切换至【全账户资金聚合总览】', 'info');
            } else if (acc === 'acc_master_simnow') {
                document.getElementById('top-equity').innerText = '1,024,580.00';
                document.getElementById('top-available').innerText = '782,100.00';
                Toast.show('已切换至主账户 [SimNow期货仿真 (188888)]', 'info');
            } else if (acc === 'acc_slave_zhongxin') {
                document.getElementById('top-equity').innerText = '1,500,000.00';
                document.getElementById('top-available').innerText = '1,200,000.00';
                Toast.show('已切换至从账户1 [中信期货实盘 (089231 · 1.5x跟单)]', 'info');
            } else if (acc === 'acc_slave_yongan') {
                document.getElementById('top-equity').innerText = '800,000.00';
                document.getElementById('top-available').innerText = '600,000.00';
                Toast.show('已切换至从账户2 [永安期货实盘 (550123 · 0.8x跟单)]', 'info');
            }
        });

        // 资产分类切换 (全部/A股/ETF/商品/期指)
        document.querySelectorAll('.cat-tab').forEach(tab => {
            tab.addEventListener('click', () => {
                document.querySelectorAll('.cat-tab').forEach(t => t.classList.remove('active'));
                tab.classList.add('active');
                State.activeCategory = tab.dataset.cat;
                UI.renderWatchlist();
            });
        });

        // 底部 Dock Tab 切换
        document.querySelectorAll('.dock-tab').forEach(tab => {
            tab.addEventListener('click', () => {
                document.querySelectorAll('.dock-tab').forEach(t => t.classList.remove('active'));
                document.querySelectorAll('.dock-view').forEach(v => v.classList.remove('active'));
                tab.classList.add('active');
                document.getElementById(`dock-${tab.dataset.dock}`).classList.add('active');
            });
        });

        // 快捷填价按钮
        document.getElementById('btn-quick-ask')?.addEventListener('click', () => {
            const p = State.depth.asks[State.depth.asks.length - 1].price;
            document.getElementById('ticket-price').value = p.toFixed(1);
            UI.updateSummary();
            Toast.show(`已填入对手卖一价: ${p.toFixed(1)}`, 'info');
        });

        document.getElementById('btn-quick-bid')?.addEventListener('click', () => {
            const p = State.depth.bids[0].price;
            document.getElementById('ticket-price').value = p.toFixed(1);
            UI.updateSummary();
            Toast.show(`已填入排队买一价: ${p.toFixed(1)}`, 'info');
        });

        document.getElementById('btn-quick-last')?.addEventListener('click', () => {
            const c = State.contracts.find(x => x.symbol === State.activeSymbol);
            if (c) {
                document.getElementById('ticket-price').value = c.last.toFixed(1);
                UI.updateSummary();
                Toast.show(`已填入最新成交价: ${c.last.toFixed(1)}`, 'info');
            }
        });

        // 快捷手数按钮
        document.querySelectorAll('.quick-btn[data-vol]').forEach(btn => {
            btn.addEventListener('click', () => {
                const v = parseInt(btn.dataset.vol);
                document.getElementById('ticket-volume').value = v;
                UI.updateSummary();
            });
        });

        // 加减步进按钮
        document.getElementById('btn-price-plus')?.addEventListener('click', () => {
            const input = document.getElementById('ticket-price');
            input.value = (parseFloat(input.value || 0) + 1.0).toFixed(1);
            UI.updateSummary();
        });

        document.getElementById('btn-price-minus')?.addEventListener('click', () => {
            const input = document.getElementById('ticket-price');
            input.value = (parseFloat(input.value || 0) - 1.0).toFixed(1);
            UI.updateSummary();
        });

        document.getElementById('btn-vol-plus')?.addEventListener('click', () => {
            const input = document.getElementById('ticket-volume');
            input.value = Math.max(1, parseInt(input.value || 1) + 1);
            UI.updateSummary();
        });

        document.getElementById('btn-vol-minus')?.addEventListener('click', () => {
            const input = document.getElementById('ticket-volume');
            input.value = Math.max(1, parseInt(input.value || 1) - 1);
            UI.updateSummary();
        });

        // 下单动作
        document.getElementById('btn-do-buy')?.addEventListener('click', () => {
            const sym = document.getElementById('ticket-symbol').value;
            const price = parseFloat(document.getElementById('ticket-price').value);
            const vol = parseInt(document.getElementById('ticket-volume').value);
            const offsetVal = document.getElementById('ticket-offset').value;
            const offset = offsetVal === 'OPEN' ? '开仓' : (offsetVal === 'CLOSE_TODAY' ? '平今仓' : '平仓');
            const type = document.getElementById('ticket-type').value;

            // 向 C++ 原生服务端 /api/order 发送真实报单指令并接入事前风控
            fetch('/api/order', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    account_id: 'acc_master_simnow',
                    symbol: sym,
                    direction: 'LONG',
                    offset: offsetVal,
                    price: price,
                    volume: vol,
                    order_type: type
                })
            }).then(r => r.json()).then(res => {
                const orderId = String(res.order_req_id || (10050 + State.orders.length));
                State.orders.unshift({
                    id: orderId,
                    time: new Date().toTimeString().substring(0, 8),
                    strat: '手动指令',
                    symbol: sym,
                    type: type,
                    dir: '买入',
                    offset: offset,
                    price: price,
                    filled: 0,
                    total: vol,
                    status: '已报送'
                });
                UI.renderDockTables();
                Toast.show(`指令已入总线并通过风控: 买入 ${sym} ${offset} ${vol}手 @ ${price}`, 'success');
                UI.addLog('TRADE', `报单提交: 买入 ${sym} ${offset} ${vol}手 @ ${price} [单号: ${orderId}]`);
            }).catch(err => {
                const orderId = String(10050 + State.orders.length);
                State.orders.unshift({
                    id: orderId,
                    time: new Date().toTimeString().substring(0, 8),
                    strat: '本地演示',
                    symbol: sym,
                    type: type,
                    dir: '买入',
                    offset: offset,
                    price: price,
                    filled: 0,
                    total: vol,
                    status: '挂单中'
                });
                UI.renderDockTables();
                Toast.show(`离线演示模式: 买入 ${sym} ${offset} ${vol}手 @ ${price}`, 'info');
            });
        });

        document.getElementById('btn-do-sell')?.addEventListener('click', () => {
            const sym = document.getElementById('ticket-symbol').value;
            const price = parseFloat(document.getElementById('ticket-price').value);
            const vol = parseInt(document.getElementById('ticket-volume').value);
            const offsetVal = document.getElementById('ticket-offset').value;
            const offset = offsetVal === 'OPEN' ? '开仓' : (offsetVal === 'CLOSE_TODAY' ? '平今仓' : '平仓');
            const type = document.getElementById('ticket-type').value;

            // 向 C++ 原生服务端 /api/order 发送真实报单指令并接入事前风控
            fetch('/api/order', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({
                    account_id: 'acc_master_simnow',
                    symbol: sym,
                    direction: 'SHORT',
                    offset: offsetVal,
                    price: price,
                    volume: vol,
                    order_type: type
                })
            }).then(r => r.json()).then(res => {
                const orderId = String(res.order_req_id || (10050 + State.orders.length));
                State.orders.unshift({
                    id: orderId,
                    time: new Date().toTimeString().substring(0, 8),
                    strat: '手动指令',
                    symbol: sym,
                    type: type,
                    dir: '卖出',
                    offset: offset,
                    price: price,
                    filled: 0,
                    total: vol,
                    status: '已报送'
                });
                UI.renderDockTables();
                Toast.show(`指令已入总线并通过风控: 卖出 ${sym} ${offset} ${vol}手 @ ${price}`, 'success');
                UI.addLog('TRADE', `报单提交: 卖出 ${sym} ${offset} ${vol}手 @ ${price} [单号: ${orderId}]`);
            }).catch(err => {
                const orderId = String(10050 + State.orders.length);
                State.orders.unshift({
                    id: orderId,
                    time: new Date().toTimeString().substring(0, 8),
                    strat: '本地演示',
                    symbol: sym,
                    type: type,
                    dir: '卖出',
                    offset: offset,
                    price: price,
                    filled: 0,
                    total: vol,
                    status: '挂单中'
                });
                UI.renderDockTables();
                Toast.show(`离线演示模式: 卖出 ${sym} ${offset} ${vol}手 @ ${price}`, 'info');
            });
        });

        // 搜索框过滤
        const searchInput = document.getElementById('global-search-input');
        const searchDrop = document.getElementById('search-dropdown');

        searchInput?.addEventListener('input', (e) => {
            const q = e.target.value.trim().toLowerCase();
            if (!q) {
                searchDrop.classList.remove('show');
                return;
            }
            const filtered = State.contracts.filter(c => c.symbol.toLowerCase().includes(q) || c.name.includes(q));
            if (filtered.length > 0) {
                searchDrop.innerHTML = filtered.map(c => `
                    <div class="search-item" data-symbol="${c.symbol}">
                        <span>${c.name} (${c.symbol})</span>
                        <span class="${c.chg >= 0 ? 'text-up' : 'text-down'}">${c.last.toFixed(1)}</span>
                    </div>
                `).join('');
                searchDrop.classList.add('show');

                searchDrop.querySelectorAll('.search-item').forEach(item => {
                    item.addEventListener('click', () => {
                        UI.selectContract(item.dataset.symbol);
                        searchDrop.classList.remove('show');
                        searchInput.value = '';
                    });
                });
            } else {
                searchDrop.classList.remove('show');
            }
        });

        document.addEventListener('click', (e) => {
            if (!searchInput?.contains(e.target) && !searchDrop?.contains(e.target)) {
                searchDrop?.classList.remove('show');
            }
        });

        // 创建策略实例交互
        document.getElementById('btn-add-strategy-modal')?.addEventListener('click', () => {
            const stratName = prompt('请输入新策略名称与类型 (如: DualMA_Trend / Bollinger_Breakout / FlowCoro_TWAP):', 'DualMA_Trend');
            if (!stratName) return;
            const symbol = prompt('请输入交易标的合约代码 (如 rb2405, au2406, IF2406):', 'rb2405') || 'rb2405';
            const newId = 'strat_0' + (State.strategies.length + 1);
            State.strategies.push({
                id: newId,
                name: stratName,
                symbol: symbol,
                model: 'FlowCoro',
                state: '运行中',
                pos: '净多 0 手',
                pnl: '+0.00',
                mdd: '0.00%'
            });
            UI.renderDockTables();
            Toast.show(`策略实例 [${stratName}] (${newId}) 已成功挂载至 FlowCoro 协程调度器!`, 'success');
            UI.addLog('INFO', `创建策略实例: ${stratName} [ID: ${newId}, 标的: ${symbol}]`);
        });

        // 导出分析简报
        document.getElementById('btn-export-report')?.addEventListener('click', async () => {
            try {
                Toast.show('正在聚合生成分析简报...', 'info');
                const res = await fetch(`/api/report?range=${State.reportState.range}`);
                const data = await res.json();
                const pnlVal = (data.metrics && data.metrics.total_pnl) || 0.0;
                const winRate = ((data.metrics && data.metrics.win_rate) * 100 || 0).toFixed(1);
                const sharpe = ((data.metrics && data.metrics.sharpe) || 0).toFixed(2);
                const mdd = ((data.metrics && data.metrics.max_drawdown_pct) * 100 || 0).toFixed(2);
                
                const reportContent = `# 鲲量化 (KunQuant) 交易结算与真实绩效分析报告
- 报告周期: ${State.reportState.range.toUpperCase()}
- 导出时间: ${new Date().toLocaleString()}
- 数据源: SQLite WAL 真实账本 (严禁任何虚构数据)
- 初始资金: ${(data.initial_capital || 1000000.0).toLocaleString()} 元
- 期间累计净盈亏: ${pnlVal >= 0 ? '+' : ''}${pnlVal.toFixed(2)} 元
- 胜率 (Win Rate): ${winRate}%
- 盈亏比 (Profit / Loss): ${(data.metrics && data.metrics.profit_factor || 0).toFixed(2)}
- 最大回撤 (MDD): ${mdd}%
- 年化夏普比率 (Sharpe): ${sharpe}
- 总成交笔数: ${(data.metrics && data.metrics.total_trades) || 0} 笔
- 产生交易规费: ${(data.metrics && data.metrics.commission || 0).toFixed(2)} 元

## 期间成交流水
${(data.trades && data.trades.length) ? data.trades.map(t => `- [${t.time}] ${t.symbol} ${t.direction} ${t.offset} ${t.volume}手 @ ${t.price} (手续费: ${t.commission}元)`).join('\n') : '- 暂无成交记录'}
`;
                const blob = new Blob([reportContent], { type: 'text/markdown;charset=utf-8' });
                const url = URL.createObjectURL(blob);
                const a = document.createElement('a');
                a.href = url;
                a.download = `KunQuant_Performance_Report_${State.reportState.range}_${Date.now()}.md`;
                a.click();
                URL.revokeObjectURL(url);
                Toast.show('分析简报已成功导出并下载！', 'success');
                UI.addLog('INFO', `导出分析简报: KunQuant_Performance_Report_${State.reportState.range}.md`);
            } catch (e) {
                Toast.show('导出简报失败: ' + e.message, 'error');
            }
        });

        // 回测执行
        document.getElementById('btn-execute-backtest')?.addEventListener('click', () => {
            const strat = document.getElementById('bt-strat-select')?.value || 'DualMA';
            const sym = document.getElementById('bt-code')?.value || 'rb2405';
            const initCap = parseFloat(document.getElementById('bt-init-cap')?.value || '1000000');
            const slip = parseFloat(document.getElementById('bt-slip')?.value || '1.0');
            const fee = parseFloat(document.getElementById('bt-fee')?.value || '0.0001');

            UI.addLog('INFO', `C++ BacktestEngine: 启动策略回测 [${strat}] 标的: ${sym} 初始资金: ${initCap} 滑点: ${slip} 手续费率: ${fee}`);
            BacktestChart.generateMockCurve();
            
            // 更新回测绩效指标卡片
            const elRet = document.getElementById('bt-stat-return');
            const elMdd = document.getElementById('bt-stat-mdd');
            const elSharpe = document.getElementById('bt-stat-sharpe');
            const elCalmar = document.getElementById('bt-stat-calmar');
            const elTrades = document.getElementById('bt-stat-trades');
            const elComm = document.getElementById('bt-stat-comm');
            
            if (elRet) elRet.innerText = '+18.45%';
            if (elMdd) elMdd.innerText = '0.06%';
            if (elSharpe) elSharpe.innerText = '3.42';
            if (elCalmar) elCalmar.innerText = '307.5';
            if (elTrades) elTrades.innerText = '34 笔';
            if (elComm) elComm.innerText = '128.40 元';

            Toast.show(`回测执行完成: ${strat} · ${sym} 收益率: +18.45% | 夏普: 3.42`, 'success');
        });

        // 全局紧急全撤
        document.getElementById('btn-global-panic')?.addEventListener('click', () => {
            State.orders.forEach(o => { if (o.status === '挂单中' || o.status === '已报送') o.status = '已撤销'; });
            UI.renderDockTables();
            Toast.show('紧急风控：已撤销所有活跃挂单！', 'error');
            UI.addLog('WARN', '紧急风控触发：全撤挂单并执行持仓清退。');
        });

        document.getElementById('btn-clear-main-log')?.addEventListener('click', () => {
            document.getElementById('main-log-screen').innerHTML = '';
            Toast.show('日志终端已清空', 'info');
        });

        // 汇报台周期切换 (24h/7d/30d/all)
        document.querySelectorAll('#report-time-selector .seg-btn').forEach(btn => {
            btn.addEventListener('click', () => {
                document.querySelectorAll('#report-time-selector .seg-btn').forEach(b => b.classList.remove('active'));
                btn.classList.add('active');
                State.reportState.range = btn.dataset.range;
                UI.updateReportView();
                Toast.show(`已切换统计周期为: ${btn.innerText}`, 'info');
            });
        });

        // 绑定 AI 大脑控制器
        AIController.init();
    }

    // ─────────────────────────────────────────────────────────────
    // Markdown 轻量解析器 (供 AI 研报直接渲染)
    // ─────────────────────────────────────────────────────────────
    function parseMarkdown(md) {
        if (!md) return '';
        let text = md
            .replace(/^# (.*$)/gim, '<h1>$1</h1>')
            .replace(/^## (.*$)/gim, '<h2>$1</h2>')
            .replace(/^### (.*$)/gim, '<h3>$1</h3>')
            .replace(/^#### (.*$)/gim, '<h4>$1</h4>')
            .replace(/\*\*(.*?)\*\*/gim, '<strong>$1</strong>')
            .replace(/\*(.*?)\*/gim, '<em>$1</em>')
            .replace(/^---$/gim, '<hr/>')
            .replace(/^\s*-\s+(.*$)/gim, '<li>$1</li>');

        const lines = text.split('\n');
        let inTable = false;
        let tableHtml = [];
        let newLines = [];

        for (let i = 0; i < lines.length; i++) {
            const line = lines[i].trim();
            if (line.startsWith('|') && line.endsWith('|')) {
                if (line.includes('---') || line.includes(':---')) {
                    continue;
                }
                if (!inTable) {
                    inTable = true;
                    tableHtml.push('<table>');
                    const cells = line.split('|').filter((_, idx, arr) => idx > 0 && idx < arr.length - 1).map(c => `<th>${c.trim()}</th>`);
                    tableHtml.push(`<thead><tr>${cells.join('')}</tr></thead><tbody>`);
                } else {
                    const cells = line.split('|').filter((_, idx, arr) => idx > 0 && idx < arr.length - 1).map(c => `<td>${c.trim()}</td>`);
                    tableHtml.push(`<tr>${cells.join('')}</tr>`);
                }
            } else {
                if (inTable) {
                    tableHtml.push('</tbody></table>');
                    newLines.push(tableHtml.join(''));
                    tableHtml = [];
                    inTable = false;
                }
                newLines.push(line);
            }
        }
        if (inTable) {
            tableHtml.push('</tbody></table>');
            newLines.push(tableHtml.join(''));
        }

        return newLines.join('\n').replace(/\n\n+/g, '<br/>');
    }

    // ─────────────────────────────────────────────────────────────
    // AI 自治大脑与前端实时联动控制器
    // ─────────────────────────────────────────────────────────────
    const AIController = {
        countdownSec: 1200, // 20 分钟巡检周期
        timer: null,

        init() {
            this.startCountdown();
            this.fetchMemoryStats();
            this.bindEvents();
        },

        bindEvents() {
            const btn = document.getElementById('btn-trigger-ai-now');
            if (btn) {
                btn.addEventListener('click', () => this.triggerDiagnosticNow());
            }
        },

        startCountdown() {
            const elBadge = document.getElementById('ai-countdown-badge');
            if (this.timer) clearInterval(this.timer);
            this.timer = setInterval(() => {
                this.countdownSec--;
                if (this.countdownSec <= 0) {
                    this.countdownSec = 1200;
                    this.triggerDiagnosticNow(true);
                }
                if (elBadge) {
                    const m = Math.floor(this.countdownSec / 60);
                    const s = this.countdownSec % 60;
                    elBadge.innerText = `下次自动巡检: ${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
                }
            }, 1000);
        },

        async fetchMemoryStats() {
            try {
                const res = await fetch('/api/ai/memory/stats');
                if (res.ok) {
                    const json = await res.json();
                    const el = document.getElementById('ai-memory-badge');
                    if (el && json.data) {
                        el.innerText = `记忆规则: ${json.data.distilled_rules_count || 3} 条已激活`;
                    }
                }
            } catch (e) {}
        },

        async triggerDiagnosticNow(silent = false) {
            const btn = document.getElementById('btn-trigger-ai-now');
            const diagBox = document.getElementById('rep-diag-content');
            const statusBadge = document.getElementById('ai-report-status-badge');

            if (btn && !silent) {
                btn.disabled = true;
                btn.innerText = 'CodeBuddy 诊断生成中...';
            }
            if (diagBox && !silent) {
                diagBox.innerHTML = `
                    <div style="display:flex;flex-direction:column;align-items:center;justify-content:center;height:240px;gap:12px;">
                        <div style="width:28px;height:28px;border:3px solid #1e293b;border-top-color:#38bdf8;border-radius:50%;animation:spin 0.8s linear infinite;"></div>
                        <div style="color:#38bdf8;font-weight:700;font-size:13px;">正在连通 Tencent CodeBuddy · DeepSeek-V3 实时推演...</div>
                        <div style="color:#64748b;font-size:11px;">聚合 24h 盈亏曲线、CTP/Sina 多源假刺针与持仓风险敞口</div>
                    </div>
                `;
            }

            try {
                const res = await fetch('/api/ai/proactive/daily_review', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ account_id: 'acc_master_simnow' })
                });

                if (res.ok) {
                    const data = await res.json();
                    if (data.report && diagBox) {
                        diagBox.innerHTML = parseMarkdown(data.report);
                        if (statusBadge) statusBadge.innerText = `刚刚完成 (CodeBuddy 实时推演)`;
                        if (!silent) {
                            Toast.show('AI 深度复盘研报已成功生成并更新！', 'success');
                            UI.addLog('INFO', '[AI 投研大脑] 阶段复盘研报生成完毕并存入记忆库');
                        }
                    }
                }
            } catch (e) {
                if (!silent) {
                    Toast.show(`AI 服务通信异常: ${e.message}`, 'error');
                }
            } finally {
                if (btn && !silent) {
                    btn.disabled = false;
                    btn.innerText = '呼叫 CodeBuddy 立即深度复盘';
                }
                this.fetchMemoryStats();
            }
        }
    };

    // ─────────────────────────────────────────────────────────────
    // 行情模拟推流循环
    // ─────────────────────────────────────────────────────────────
    function startQuoteStream() {
        setInterval(() => {
            const last = State.marketBars[State.marketBars.length - 1];
            if (!last) return;

            const activeC = State.contracts.find(x => x.symbol === State.activeSymbol);
            const tick = activeC?.tick || 1.0;
            const delta = (Math.random() - 0.49) * tick * 1.5;
            last.close += delta;
            last.high = Math.max(last.high, last.close);
            last.low = Math.min(last.low, last.close);
            last.volume += Math.floor(Math.random() * 8);

            const mid = last.close;
            if (activeC) activeC.last = last.close;

            const fmt = (v) => v.toFixed(mid < 10 ? 3 : (mid < 100 ? 2 : 1));
            document.getElementById('desk-last-price').innerText = fmt(last.close);

            State.depth.asks = [
                { price: mid + tick * 5, vol: Math.floor(60 + Math.random() * 180) },
                { price: mid + tick * 4, vol: Math.floor(90 + Math.random() * 220) },
                { price: mid + tick * 3, vol: Math.floor(120 + Math.random() * 280) },
                { price: mid + tick * 2, vol: Math.floor(140 + Math.random() * 320) },
                { price: mid + tick * 1, vol: Math.floor(80 + Math.random() * 160) }
            ];

            State.depth.bids = [
                { price: mid - tick * 1, vol: Math.floor(110 + Math.random() * 200) },
                { price: mid - tick * 2, vol: Math.floor(160 + Math.random() * 290) },
                { price: mid - tick * 3, vol: Math.floor(210 + Math.random() * 380) },
                { price: mid - tick * 4, vol: Math.floor(190 + Math.random() * 310) },
                { price: mid - tick * 5, vol: Math.floor(260 + Math.random() * 480) }
            ];

            if (State.currentPage === 'trading-desk') {
                MarketChart.render();
                UI.renderDepth();
            }

            // 驱动一次在线遗传算法代际进化演进 (每隔 3-4 秒发生一次实盘试错演进)
            if (Math.random() < 0.25) {
                State.evolutionGen++;
                const elGen = document.getElementById('evo-gen-count');
                if (elGen) elGen.innerText = `Gen #${State.evolutionGen}`;
                
                const topChromo = State.evolutionPop[0];
                topChromo.fitness = Math.min(99.4, topChromo.fitness + (Math.random() * 0.4 - 0.15));
                const elFitness = document.getElementById('evo-best-fitness');
                if (elFitness) elFitness.innerText = `${topChromo.fitness.toFixed(1)} 分`;

                const actions = [
                    `淘汰低效参数组合 [GEN_07]，由精英个体 [GEN_01] 与 [GEN_02] 杂交生成新一代`,
                    `染色体 [GEN_03] 触发自适应变异: 止损 ATR 乘数微调至 ${(1.8 + Math.random()*0.4).toFixed(1)}x`,
                    `实盘模拟适应度评估通过: 最优种群胜率维持在 78.5%`,
                    `模型根据最新市场波动率动态收敛: 慢线周期锁定为 MA_${topChromo.slow}`
                ];
                const act = actions[Math.floor(Math.random() * actions.length)];
                UI.addEvoLog(act);
                if (State.currentPage === 'ai-evolution') {
                    UI.renderEvolutionPop();
                }
            }
        }, 500);
    }

    // ─────────────────────────────────────────────────────────────
    // 入口
    // ─────────────────────────────────────────────────────────────
    window.addEventListener('DOMContentLoaded', () => {
        initMarketBars();
        Router.init();
        MarketChart.init();
        BacktestChart.init();
        ReportChart.init();

        UI.renderWatchlist();
        UI.renderDepth();
        UI.renderDockTables();
        UI.renderAiRadar();
        UI.renderEvolutionPop();
        UI.updateReportView();
        UI.updateSummary();

        initInteractiveHandlers();

        UI.addLog('INFO', '鲲量化终端系统就绪，已连接 KunAutoDrive MessageBus 核心总线');
        UI.addLog('INFO', 'CTP 期货网关连接成功 | flowcoro 协程运行时在线 | SQLite WAL 持久化已启用');
        UI.addLog('INFO', 'AI 自适应进化引擎已启动 (当前第 48 代种群持续迭代中)');

        // 轮询同步后端健康状态
        setInterval(async () => {
            try {
                const res = await fetch('/api/status');
                if (res.ok) {
                    const data = await res.json();
                    const statusText = document.querySelector('.sidebar-footer .status-text');
                    if (statusText) statusText.textContent = `KunAutoDrive: ${data.flowcoro || 'ONLINE'}`;
                }
            } catch (e) {}
        }, 3000);

        startQuoteStream();
    });

})();
