package model

import (
	"fmt"
	"strings"
	"time"
)

type AgentStatus string

const (
	StatusOnline  AgentStatus = "online"
	StatusOffline AgentStatus = "offline"
)

type CaptchaResult string

const (
	ResultSuccess CaptchaResult = "success"
	ResultFail    CaptchaResult = "fail"
	ResultMiss    CaptchaResult = "miss"
)

type Agent struct {
	ID        string      `json:"id"`
	Name      string      `json:"name"`
	Token     string      `json:"token,omitempty"`
	Status    AgentStatus `json:"status"`
	LastSeen  *time.Time  `json:"last_seen,omitempty"`
	CreatedAt time.Time   `json:"created_at"`
}

type CaptchaRecord struct {
	ID          int64         `json:"id"`
	AgentID     string        `json:"agent_id"`
	AgentName   string        `json:"agent_name,omitempty"`
	CaptchaText string        `json:"captcha_text"`
	Answer      *string       `json:"answer,omitempty"`
	Result      CaptchaResult `json:"result"`
	ResponseMs  *int64        `json:"response_ms,omitempty"`
	SentAt      time.Time     `json:"sent_at"`
	AnsweredAt  *time.Time    `json:"answered_at,omitempty"`
}

type Settings struct {
	MinIntervalSec  int    `json:"min_interval_sec"`
	MaxIntervalSec  int    `json:"max_interval_sec"`
	CaptchaLength   int    `json:"captcha_length"`
	TimeoutSec      int    `json:"timeout_sec"`
	NoiseLevel      int    `json:"noise_level"`
	AutoSendEnabled bool   `json:"auto_send_enabled"`
	CaptchaCharset  string `json:"captcha_charset"`
	ImageWidth      int    `json:"image_width"`
	ImageHeight     int    `json:"image_height"`
	// Skip time ranges (e.g. "12:00-13:00,18:00-08:00")
	SkipTimeRanges string `json:"skip_time_ranges"`
	// Telegram
	TelegramEnabled  bool   `json:"telegram_enabled"`
	TelegramBotToken string `json:"telegram_bot_token"`
	TelegramChatID   string `json:"telegram_chat_id"`
	// Notification message templates ({name} = agent name, {time} = response time ms)
	MsgSuccess   string `json:"msg_success"`
	MsgFail      string `json:"msg_fail"`
	MsgMiss      string `json:"msg_miss"`
	MsgClose     string `json:"msg_close"`
	MsgUninstall string `json:"msg_uninstall"`
	// Popup notification message (shown on captcha popup)
	PopupMessage string `json:"popup_message"`
}

func DefaultSettings() Settings {
	return Settings{
		MinIntervalSec:  300,
		MaxIntervalSec:  900,
		CaptchaLength:   5,
		TimeoutSec:      60,
		NoiseLevel:      3,
		AutoSendEnabled: true,
		CaptchaCharset:  "alphanumeric",
		ImageWidth:      200,
		ImageHeight:     80,
		SkipTimeRanges:  "",
		TelegramEnabled: false,
		TelegramBotToken: "",
		TelegramChatID:  "",
		MsgSuccess:   "Nhân viên {name} đã hoàn thành trả lời câu hỏi trong {time}ms",
		MsgFail:      "Nhân viên {name} đã không hoàn thành trả lời câu hỏi (sai đáp án)",
		MsgMiss:      "Nhân viên {name} đã không hoàn thành trả lời câu hỏi (hết thời gian)",
		MsgClose:     "Nhân viên {name} đã tắt bảng xác nhận CAPTCHA",
		MsgUninstall: "Nhân viên {name} đã cố gỡ phần mềm giám sát!",
		PopupMessage: "Vui lòng nhập mã xác nhận bên dưới",
	}
}

// FormatMessage replaces {name} and {time} placeholders in a message template
func FormatMessage(template, name string, timeMs int64) string {
	msg := strings.ReplaceAll(template, "{name}", name)
	msg = strings.ReplaceAll(msg, "{time}", fmt.Sprintf("%d", timeMs))
	return msg
}

// IsInSkipTime checks if current time falls in any skip time range
func (s Settings) IsInSkipTime(now time.Time) bool {
	if s.SkipTimeRanges == "" {
		return false
	}
	currentMinutes := now.Hour()*60 + now.Minute()
	ranges := parseTimeRanges(s.SkipTimeRanges)
	for _, r := range ranges {
		if r[0] <= r[1] {
			// Same day range: 12:00-13:00
			if currentMinutes >= r[0] && currentMinutes < r[1] {
				return true
			}
		} else {
			// Overnight range: 18:00-08:00
			if currentMinutes >= r[0] || currentMinutes < r[1] {
				return true
			}
		}
	}
	return false
}

func parseTimeRanges(s string) [][2]int {
	var ranges [][2]int
	for _, part := range splitTrim(s, ",") {
		halves := splitTrim(part, "-")
		if len(halves) != 2 {
			continue
		}
		start := parseHHMM(halves[0])
		end := parseHHMM(halves[1])
		if start >= 0 && end >= 0 {
			ranges = append(ranges, [2]int{start, end})
		}
	}
	return ranges
}

func parseHHMM(s string) int {
	parts := splitTrim(s, ":")
	if len(parts) != 2 {
		return -1
	}
	h := atoi(parts[0])
	m := atoi(parts[1])
	if h < 0 || h > 23 || m < 0 || m > 59 {
		return -1
	}
	return h*60 + m
}

func splitTrim(s, sep string) []string {
	var result []string
	for _, part := range strings.Split(s, sep) {
		trimmed := strings.TrimSpace(part)
		if trimmed != "" {
			result = append(result, trimmed)
		}
	}
	return result
}

func atoi(s string) int {
	n := 0
	for _, ch := range s {
		if ch < '0' || ch > '9' {
			return -1
		}
		n = n*10 + int(ch-'0')
	}
	return n
}

// WebSocket message types
type WSMessageType string

const (
	MsgCaptchaChallenge WSMessageType = "captcha_challenge"
	MsgCaptchaResponse  WSMessageType = "captcha_response"
	MsgCaptchaResult    WSMessageType = "captcha_result"
	MsgHeartbeat        WSMessageType = "heartbeat"
	MsgConfigUpdate     WSMessageType = "config_update"
	MsgAuth             WSMessageType = "auth"
	MsgAuthResult       WSMessageType = "auth_result"
	MsgAlert            WSMessageType = "alert"          // agent sends alerts (uninstall attempt, close popup, etc.)
	MsgRemoteCommand    WSMessageType = "remote_command" // server sends commands to agent
)

type WSMessage struct {
	Type WSMessageType `json:"type"`
}

type AuthMessage struct {
	Type    WSMessageType `json:"type"`
	AgentID string        `json:"agent_id"`
	Token   string        `json:"token"`
}

type AuthResultMessage struct {
	Type    WSMessageType `json:"type"`
	Success bool          `json:"success"`
	Message string        `json:"message,omitempty"`
}

type CaptchaChallengeMessage struct {
	Type         WSMessageType `json:"type"`
	ID           string        `json:"id"`
	Image        string        `json:"image"`
	TimeoutSec   int           `json:"timeout_sec"`
	PopupMessage string        `json:"popup_message,omitempty"`
}

type CaptchaResponseMessage struct {
	Type       WSMessageType `json:"type"`
	ID         string        `json:"id"`
	Answer     *string       `json:"answer"`
	ResponseMs int64         `json:"response_ms"`
}

type CaptchaResultMessage struct {
	Type    WSMessageType `json:"type"`
	ID      string        `json:"id"`
	Correct bool          `json:"correct"`
}

type ConfigUpdateMessage struct {
	Type       WSMessageType `json:"type"`
	TimeoutSec int           `json:"timeout_sec"`
}

// AgentAlertMessage is sent by agent for uninstall/close events
type AgentAlertMessage struct {
	Type       WSMessageType `json:"type"`
	AlertType  string        `json:"alert_type"` // "uninstall_attempt", "close_popup", "tamper"
	Message    string        `json:"message"`
}

// RemoteCommandMessage is sent from server to agent
type RemoteCommandMessage struct {
	Type    WSMessageType `json:"type"`
	Command string        `json:"command"` // "uninstall", "restart", "update_config"
}

// Alert for realtime notification
type Alert struct {
	Type      string    `json:"type"`
	AgentID   string    `json:"agent_id"`
	AgentName string    `json:"agent_name"`
	Message   string    `json:"message"`
	Timestamp time.Time `json:"timestamp"`
}
