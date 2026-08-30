/**
 * 鲲小助手 (KunQuant AI Assistant) — 悬浮球 + 弹窗
 * 统一走 8900 同源端口 (/api/ai/*、/api/focus*、/api/tasks* 由 C++ Server 内部代理至 8901)
 * 不依赖任何外部库
 */
(function () {
    'use strict';

    const API = {
        chat:   (msg) => req('POST', '/api/ai/chat', { message: msg }),
        focus:  () => req('GET', '/api/focus'),
        focusAdd: (symbol, note) => req('POST', '/api/focus/add', { symbol, note, category: 'futures' }),
        focusRemove: (symbol) => req('POST', '/api/focus/remove', { symbol }),
        tasks:  () => req('GET', '/api/tasks'),
        taskRun: (id) => req('POST', '/api/tasks/run', { id }),
    };

    async function req(method, path, body) {
        const opt = { method, headers: { 'Content-Type': 'application/json' } };
        if (body) opt.body = JSON.stringify(body);
        const r = await fetch(path, opt);
        return r.json();
    }

    let panelOpen = false;

    function el(tag, cls, html) {
        const e = document.createElement(tag);
        if (cls) e.className = cls;
        if (html !== undefined) e.innerHTML = html;
        return e;
    }
    const esc = (s) => String(s ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

    // ── 消息区 ──
    function addMsg(container, cls, html) {
        const m = el('div', 'kun-ai-msg ' + cls, html);
        container.appendChild(m);
        container.scrollTop = container.scrollHeight;
        return m;
    }

    // ── 专项跟踪渲染 ──
    async function renderFocus(container) {
        container.innerHTML = '';
        container.appendChild(el('div', 'kun-ai-section-title', '专项跟踪'));
        try {
            const res = await API.focus();
            if (!res.focus || !res.focus.length) {
                container.appendChild(el('div', 'kun-ai-focus-item',
                    '<span class="stale">暂无跟踪产品 — 在下方输入"跟踪 au2406"即可添加</span>'));
            }
            for (const f of (res.focus || [])) {
                const s = f.snapshot || {};
                let stat;
                if (s.available) {
                    const cls = s.change_pct >= 0 ? 'up' : 'down';
                    const sign = s.change_pct >= 0 ? '+' : '';
                    stat = `<span class="stat">${esc(s.last_price)} <span class="${cls}">${sign}${esc(s.change_pct)}%</span> <span class="stale">${esc(s.tick_count)}tick/${esc(s.stale_minutes)}min</span></span>`;
                } else {
                    stat = `<span class="stale">${esc(s.reason || '无数据')}</span>`;
                }
                const row = el('div', 'kun-ai-focus-item',
                    `<span class="sym">${esc(f.symbol)}</span>${stat}<button title="移除">✕</button>`);
                row.querySelector('button').onclick = async () => { await API.focusRemove(f.symbol); renderFocus(container); };
                container.appendChild(row);
            }
        } catch (e) {
            container.appendChild(el('div', 'kun-ai-focus-item', '<span class="stale">AI 投研服务未连接 (后端已就绪)</span>'));
        }
    }

    // ── 任务列表渲染 ──
    async function renderTasks(container) {
        container.innerHTML = '';
        container.appendChild(el('div', 'kun-ai-section-title', 'AI 分析任务'));
        try {
            const res = await API.tasks();
            const tasks = (res.tasks || []).slice(-8).reverse();
            if (!tasks.length) {
                container.appendChild(el('div', 'kun-ai-task-item',
                    '<span class="t-desc">暂无任务 — 对话中让 AI 建任务，或等聊天自动生成</span>'));
            }
            for (const t of tasks) {
                const row = el('div', 'kun-ai-task-item',
                    `<span class="t-status ${esc(t.status)}">${esc(t.status)}</span>` +
                    `<span class="t-desc" title="${esc(t.description)}">#${t.id} ${esc(t.symbol)} · ${esc(t.type)}${t.description ? ' · ' + esc(t.description) : ''}</span>` +
                    (t.status === 'pending' ? '<button>执行</button>' : ''));
                const btn = row.querySelector('button');
                if (btn) btn.onclick = async () => {
                    btn.disabled = true;
                    const r = await API.taskRun(t.id);
                    if (r.ok && r.task.result) {
                        addMsg(msgBox, 'ai', '#' + t.id + ' 结果:\n' + esc(JSON.stringify(r.task.result, null, 2).slice(0, 600)));
                    }
                    renderTasks(container);
                };
                container.appendChild(row);
            }
        } catch (e) { /* AI 离线时静默 */ }
    }

    // ── 发送消息 ──
    async function send(input, msgBox) {
        const text = input.value.trim();
        if (!text) return;

        // 本地指令: 跟踪 <合约>
        const m = text.match(/^跟踪\s+([A-Za-z0-9]+)\s*(.*)$/);
        if (m) {
            addMsg(msgBox, 'user', esc(text));
            const r = await API.focusAdd(m[1].toUpperCase() === m[1] ? m[1] : m[1], m[2] || '对话添加');
            addMsg(msgBox, 'ai', r.ok ? `已开始专项跟踪 <b>${esc(r.focus.symbol)}</b>${r.snapshot && r.snapshot.available ? '，最新价 ' + esc(r.snapshot.last_price) : ''}`
                                      : '失败: ' + esc(r.error || ''));
            focusBox && renderFocus(focusBox);
            input.value = '';
            return;
        }

        input.value = '';
        sendBtn.disabled = true;
        addMsg(msgBox, 'user', esc(text));
        const thinking = addMsg(msgBox, 'ai', '思考中…');
        try {
            const res = await API.chat(text);
            thinking.innerHTML = esc(res.reply || '(空回复)').replace(/\n/g, '<br>');
            if (res.created_tasks && res.created_tasks.length) {
                const chips = res.created_tasks.map(t => `<span class="kun-task-chip">已建任务 #${t.id} ${esc(t.type)}</span>`).join('');
                thinking.innerHTML += '<br>' + chips;
                tasksBox && renderTasks(tasksBox);
            }
        } catch (e) {
            thinking.textContent = 'AI 服务请求失败 — 确认已启动: python3 ai_service/server.py';
        }
        sendBtn.disabled = false;
        input.focus();
    }

    // ── 初始化 ──
    let msgBox, focusBox, tasksBox, sendBtn;

    function init() {
        const root = el('div'); root.id = 'kun-assistant';

        const ball = el('button'); ball.id = 'kun-ai-ball'; ball.textContent = '鲲'; ball.title = '鲲小助手';
        const panel = el('div', 'hidden'); panel.id = 'kun-ai-panel';

        const header = el('div', 'kun-ai-header');
        header.appendChild(el('span', 'kun-ai-title', '鲲小助手 · AI 投研 (统一端口 8900)'));
        const closeBtn = el('button'); closeBtn.id = 'kun-ai-close'; closeBtn.textContent = '×';
        header.appendChild(closeBtn);

        const body = el('div', 'kun-ai-body');
        msgBox = el('div'); msgBox.style.cssText = 'display:flex;flex-direction:column;gap:6px;';
        focusBox = el('div');
        tasksBox = el('div');
        body.appendChild(msgBox); body.appendChild(focusBox); body.appendChild(tasksBox);

        const inputRow = el('div', 'kun-ai-input-row');
        const input = el('input'); input.id = 'kun-ai-input';
        input.placeholder = '问行情 / 说"跟踪 au2406" / 让 AI 建分析任务…';
        sendBtn = el('button'); sendBtn.id = 'kun-ai-send'; sendBtn.textContent = '发送';
        inputRow.appendChild(input); inputRow.appendChild(sendBtn);

        panel.appendChild(header); panel.appendChild(body); panel.appendChild(inputRow);
        root.appendChild(ball); root.appendChild(panel);
        document.body.appendChild(root);

        ball.addEventListener('click', async () => {
            panelOpen = !panelOpen;
            panel.classList.toggle('hidden', !panelOpen);
            if (panelOpen) {
                if (!msgBox.children.length) addMsg(msgBox, 'ai', '你好，我是<b>鲲小助手</b>。\n可以问我行情与持仓，说"跟踪 au2406"添加专项跟踪，或让我创建分析任务。');
                renderFocus(focusBox);
                renderTasks(tasksBox);
                input.focus();
            }
        });
        closeBtn.addEventListener('click', () => { panelOpen = false; panel.classList.add('hidden'); });
        sendBtn.addEventListener('click', () => send(input, msgBox));
        input.addEventListener('keydown', (e) => { if (e.key === 'Enter') send(input, msgBox); });
    }

    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
    else init();
})();
