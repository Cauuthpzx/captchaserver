// SSE Alerts
const alertBar = document.getElementById('alert-bar');
if (alertBar) {
    const evtSource = new EventSource('/api/alerts');
    evtSource.onmessage = function(event) {
        const alert = JSON.parse(event.data);
        showAlert(alert);
    };
}

function showAlert(alert) {
    const div = document.createElement('div');
    div.className = 'alert-toast';
    div.textContent = alert.message + ' - ' + new Date(alert.timestamp).toLocaleTimeString();
    alertBar.appendChild(div);
    setTimeout(() => div.remove(), 10000);

    const missEl = document.getElementById('miss-today');
    if (missEl) {
        missEl.textContent = parseInt(missEl.textContent) + 1;
    }
}

// --- Toast notification ---
function showToast(msg, type) {
    const div = document.createElement('div');
    div.className = 'alert-toast';
    if (type === 'success') div.style.borderColor = '#34d399';
    if (type === 'error') div.style.borderColor = '#f87171';
    div.textContent = msg;
    alertBar.appendChild(div);
    setTimeout(() => div.remove(), 5000);
}

// --- Agents Page ---

function showAddAgent() {
    document.getElementById('add-agent-modal').style.display = 'flex';
    document.getElementById('agent-result').style.display = 'none';
    document.getElementById('agent-name').value = '';
    document.getElementById('agent-name').focus();
}

function hideAddAgent() {
    document.getElementById('add-agent-modal').style.display = 'none';
}

function addAgent() {
    const name = document.getElementById('agent-name').value.trim();
    if (!name) return;

    fetch('/api/agents', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: name })
    })
    .then(r => r.json())
    .then(data => {
        if (data.error) { alert(data.error); return; }
        document.getElementById('new-agent-id').textContent = data.id;
        document.getElementById('new-agent-token').textContent = data.token;
        document.getElementById('agent-result').style.display = 'block';
        setTimeout(() => location.reload(), 3000);
    });
}

function showRename(id, currentName) {
    document.getElementById('rename-modal').style.display = 'flex';
    document.getElementById('rename-id').value = id;
    document.getElementById('rename-name').value = currentName;
    document.getElementById('rename-name').focus();
}

function hideRename() {
    document.getElementById('rename-modal').style.display = 'none';
}

function renameAgent() {
    const id = document.getElementById('rename-id').value;
    const name = document.getElementById('rename-name').value.trim();
    if (!name) return;

    fetch('/api/agents/' + id + '/rename', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: name })
    })
    .then(r => r.json())
    .then(data => {
        if (data.error) { alert(data.error); return; }
        hideRename();
        location.reload();
    });
}

function showToken(id) {
    fetch('/api/agents/' + id + '/token')
    .then(r => r.json())
    .then(data => {
        if (data.error) { alert(data.error); return; }
        prompt('Agent Token:', data.token);
    });
}

function deleteAgent(id, name) {
    const msg = (typeof t === 'function' ? t('confirm_delete') : 'Delete agent "{name}"?').replace('{name}', name);
    if (!confirm(msg)) return;

    fetch('/api/agents/' + id, { method: 'DELETE' })
    .then(r => r.json())
    .then(data => {
        if (data.error) { alert(data.error); return; }
        const row = document.getElementById('agent-row-' + id);
        if (row) row.remove();
    });
}

// --- Checkbox selection ---

function toggleSelectAll(el) {
    document.querySelectorAll('.agent-check').forEach(cb => cb.checked = el.checked);
}

function getSelectedAgentIDs() {
    return Array.from(document.querySelectorAll('.agent-check:checked')).map(cb => cb.value);
}

// --- Manual CAPTCHA send ---

function sendCaptchaTo(agentId) {
    sendCaptchaToAgents([agentId]);
}

function sendSelectedCaptcha() {
    const ids = getSelectedAgentIDs();
    if (ids.length === 0) {
        showToast(typeof t === 'function' ? t('select_agents') : 'Chọn ít nhất 1 agent', 'error');
        return;
    }
    sendCaptchaToAgents(ids);
}

function sendCaptchaToAgents(ids) {
    fetch('/api/send-captcha', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ agent_ids: ids })
    })
    .then(r => r.json())
    .then(data => {
        if (data.error) { showToast(data.error, 'error'); return; }
        let sentCount = 0;
        for (const [id, status] of Object.entries(data)) {
            if (status === 'sent') sentCount++;
            else showToast(id + ': ' + (typeof t === 'function' ? t(status) : status), 'error');
        }
        if (sentCount > 0) {
            showToast((typeof t === 'function' ? t('sent') : 'Đã gửi') + ': ' + sentCount + ' agent(s)', 'success');
        }
    });
}

// --- Settings Page ---

const settingsForm = document.getElementById('settings-form');
if (settingsForm) {
    settingsForm.addEventListener('submit', function(e) {
        e.preventDefault();
        syncSkipTime();
        const formData = new FormData(settingsForm);
        const autoSendEl = document.getElementById('auto-send-enabled');
        const telegramEl = document.getElementById('telegram-enabled');
        const settings = {
            min_interval_sec: parseInt(formData.get('min_interval_sec')) || 300,
            max_interval_sec: parseInt(formData.get('max_interval_sec')) || 900,
            captcha_length: parseInt(formData.get('captcha_length')) || 5,
            timeout_sec: parseInt(formData.get('timeout_sec')) || 60,
            noise_level: parseInt(formData.get('noise_level')) || 3,
            auto_send_enabled: autoSendEl ? autoSendEl.checked : true,
            captcha_charset: formData.get('captcha_charset') || 'alphanumeric',
            image_width: parseInt(formData.get('image_width')) || 200,
            image_height: parseInt(formData.get('image_height')) || 80,
            skip_time_ranges: formData.get('skip_time_ranges') || '',
            telegram_enabled: telegramEl ? telegramEl.checked : false,
            telegram_bot_token: formData.get('telegram_bot_token') || '',
            telegram_chat_id: formData.get('telegram_chat_id') || '',
            popup_message: formData.get('popup_message') || '',
            msg_success: formData.get('msg_success') || '',
            msg_fail: formData.get('msg_fail') || '',
            msg_miss: formData.get('msg_miss') || '',
            msg_close: formData.get('msg_close') || '',
            msg_uninstall: formData.get('msg_uninstall') || ''
        };

        const status = document.getElementById('settings-status');
        fetch('/api/settings', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(settings)
        })
        .then(r => r.json())
        .then(data => {
            if (data.error) {
                status.textContent = 'Error: ' + data.error;
                status.style.color = '#f87171';
                return;
            }
            status.textContent = typeof t === 'function' ? t('saved') : 'Đã lưu!';
            status.style.color = '#34d399';
            setTimeout(() => status.textContent = '', 3000);
        });
    });
}

// --- Skip Time Picker ---

function addSkipTimeRow(startVal, endVal) {
    const container = document.getElementById('skip-time-rows');
    if (!container) return;
    const row = document.createElement('div');
    row.className = 'form-row skip-time-row';
    row.style.marginBottom = '8px';
    row.style.alignItems = 'center';
    row.innerHTML =
        '<input type="time" class="input skip-start" value="' + (startVal || '') + '" style="flex:1">' +
        '<span style="margin:0 8px;color:var(--text-muted)">→</span>' +
        '<input type="time" class="input skip-end" value="' + (endVal || '') + '" style="flex:1">' +
        '<button type="button" class="btn btn-sm btn-danger" onclick="this.parentElement.remove();syncSkipTime()" style="margin-left:8px">X</button>';
    container.appendChild(row);
    row.querySelectorAll('input').forEach(inp => inp.addEventListener('change', syncSkipTime));
}

function syncSkipTime() {
    const rows = document.querySelectorAll('.skip-time-row');
    const ranges = [];
    rows.forEach(row => {
        const start = row.querySelector('.skip-start').value;
        const end = row.querySelector('.skip-end').value;
        if (start && end) ranges.push(start + '-' + end);
    });
    const hidden = document.getElementById('skip-time-hidden');
    if (hidden) hidden.value = ranges.join(', ');
}

function initSkipTimePicker() {
    const hidden = document.getElementById('skip-time-hidden');
    if (!hidden || !hidden.value.trim()) return;
    const parts = hidden.value.split(',');
    parts.forEach(part => {
        const trimmed = part.trim();
        if (!trimmed) return;
        const halves = trimmed.split('-');
        if (halves.length === 2) {
            addSkipTimeRow(halves[0].trim(), halves[1].trim());
        }
    });
}

// Init skip time picker on page load
if (document.getElementById('skip-time-rows')) {
    initSkipTimePicker();
}

// --- History Page Filter ---

function filterHistory() {
    const agentFilter = document.getElementById('filter-agent');
    const resultFilter = document.getElementById('filter-result');
    if (!agentFilter || !resultFilter) return;

    const agentVal = agentFilter.value;
    const resultVal = resultFilter.value;

    document.querySelectorAll('.history-row').forEach(row => {
        const matchAgent = !agentVal || row.dataset.agent === agentVal;
        const matchResult = !resultVal || row.dataset.result === resultVal;
        row.style.display = (matchAgent && matchResult) ? '' : 'none';
    });
}
