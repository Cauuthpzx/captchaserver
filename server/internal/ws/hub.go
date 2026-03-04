package ws

import (
	"captcha-server/internal/alert"
	"captcha-server/internal/captcha"
	"captcha-server/internal/db"
	"captcha-server/internal/model"
	"captcha-server/internal/telegram"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"log"
	"math/rand"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/google/uuid"
	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	CheckOrigin:     func(r *http.Request) bool { return true },
	ReadBufferSize:  4096,
	WriteBufferSize: 4096,
}

type pendingCaptcha struct {
	ID         string
	RecordID   int64
	Text       string
	SentAt     time.Time
	TimeoutSec int
}

type agentConn struct {
	conn    *websocket.Conn
	agentID string
	mu      sync.Mutex
	pending map[string]*pendingCaptcha
}

func (ac *agentConn) writeJSON(v interface{}) error {
	ac.mu.Lock()
	defer ac.mu.Unlock()
	ac.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
	return ac.conn.WriteJSON(v)
}

type Hub struct {
	store    *db.Store
	gen      *captcha.Generator
	notifier *alert.Notifier
	tgBot    *telegram.Bot
	mu       sync.RWMutex
	agents   map[string]*agentConn
	stopCh   map[string]chan struct{}
	rng      *rand.Rand
}

func NewHub(store *db.Store, gen *captcha.Generator, notifier *alert.Notifier) *Hub {
	return &Hub{
		store:    store,
		gen:      gen,
		notifier: notifier,
		tgBot:    telegram.NewBot(),
		agents:   make(map[string]*agentConn),
		stopCh:   make(map[string]chan struct{}),
		rng:      rand.New(rand.NewSource(time.Now().UnixNano())),
	}
}

func (h *Hub) HandleWS(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("ws upgrade error: %v", err)
		return
	}

	conn.SetReadDeadline(time.Now().Add(15 * time.Second))
	_, msg, err := conn.ReadMessage()
	if err != nil {
		conn.Close()
		return
	}

	var authMsg model.AuthMessage
	if err := json.Unmarshal(msg, &authMsg); err != nil || authMsg.Type != model.MsgAuth {
		conn.WriteJSON(model.AuthResultMessage{Type: model.MsgAuthResult, Success: false, Message: "invalid auth message"})
		conn.Close()
		return
	}

	agent, err := h.store.AuthenticateAgent(authMsg.AgentID, authMsg.Token)
	if err != nil || agent == nil {
		conn.WriteJSON(model.AuthResultMessage{Type: model.MsgAuthResult, Success: false, Message: "invalid credentials"})
		conn.Close()
		return
	}

	conn.WriteJSON(model.AuthResultMessage{Type: model.MsgAuthResult, Success: true})
	conn.SetReadDeadline(time.Time{})

	ac := &agentConn{
		conn:    conn,
		agentID: agent.ID,
		pending: make(map[string]*pendingCaptcha),
	}

	h.registerAgent(agent.ID, ac)
	defer h.unregisterAgent(agent.ID)

	h.readLoop(ac)
}

func (h *Hub) registerAgent(agentID string, ac *agentConn) {
	h.mu.Lock()
	defer h.mu.Unlock()

	if old, ok := h.agents[agentID]; ok {
		old.conn.Close()
		if stopCh, ok := h.stopCh[agentID]; ok {
			close(stopCh)
		}
	}

	h.agents[agentID] = ac
	h.store.UpdateAgentStatus(agentID, model.StatusOnline)

	stopCh := make(chan struct{})
	h.stopCh[agentID] = stopCh
	go h.schedulerLoop(agentID, stopCh)

	log.Printf("agent %s connected", agentID)
}

func (h *Hub) unregisterAgent(agentID string) {
	h.mu.Lock()
	defer h.mu.Unlock()

	if ac, ok := h.agents[agentID]; ok {
		ac.conn.Close()
		delete(h.agents, agentID)
	}
	if stopCh, ok := h.stopCh[agentID]; ok {
		close(stopCh)
		delete(h.stopCh, agentID)
	}

	h.store.UpdateAgentStatus(agentID, model.StatusOffline)
	log.Printf("agent %s disconnected", agentID)
}

func (h *Hub) readLoop(ac *agentConn) {
	ac.conn.SetPongHandler(func(string) error {
		ac.conn.SetReadDeadline(time.Now().Add(90 * time.Second))
		return nil
	})

	go func() {
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()
		for {
			<-ticker.C
			ac.mu.Lock()
			err := ac.conn.WriteMessage(websocket.PingMessage, nil)
			ac.mu.Unlock()
			if err != nil {
				return
			}
		}
	}()

	for {
		ac.conn.SetReadDeadline(time.Now().Add(90 * time.Second))
		_, msg, err := ac.conn.ReadMessage()
		if err != nil {
			return
		}

		var base model.WSMessage
		if err := json.Unmarshal(msg, &base); err != nil {
			continue
		}

		switch base.Type {
		case model.MsgCaptchaResponse:
			var resp model.CaptchaResponseMessage
			if err := json.Unmarshal(msg, &resp); err != nil {
				continue
			}
			h.handleCaptchaResponse(ac, &resp)

		case model.MsgHeartbeat:
			h.store.UpdateAgentStatus(ac.agentID, model.StatusOnline)

		case model.MsgAlert:
			var alertMsg model.AgentAlertMessage
			if err := json.Unmarshal(msg, &alertMsg); err != nil {
				continue
			}
			h.handleAgentAlert(ac, &alertMsg)
		}
	}
}

func (h *Hub) handleAgentAlert(ac *agentConn, alertMsg *model.AgentAlertMessage) {
	agent, _ := h.store.GetAgent(ac.agentID)
	agentName := ac.agentID
	if agent != nil {
		agentName = agent.Name
	}

	var message string
	switch alertMsg.AlertType {
	case "uninstall_attempt":
		message = fmt.Sprintf("Agent %s - uninstall attempt detected!", agentName)
	case "close_popup":
		message = fmt.Sprintf("Agent %s closed CAPTCHA popup", agentName)
	case "tamper":
		message = fmt.Sprintf("Agent %s - tamper detected: %s", agentName, alertMsg.Message)
	default:
		message = fmt.Sprintf("Agent %s - alert: %s", agentName, alertMsg.Message)
	}

	log.Printf("AGENT ALERT: %s", message)

	// Send SSE notification
	h.notifier.Send(model.Alert{
		Type:      alertMsg.AlertType,
		AgentID:   ac.agentID,
		AgentName: agentName,
		Message:   message,
		Timestamp: time.Now(),
	})

	// Send Telegram notification
	settings, err := h.store.GetSettings()
	if err == nil && settings.TelegramEnabled {
		switch alertMsg.AlertType {
		case "uninstall_attempt":
			go h.tgBot.NotifyUninstall(settings.TelegramBotToken, settings.TelegramChatID, agentName, settings.MsgUninstall)
		case "close_popup":
			go h.tgBot.NotifyClosePopup(settings.TelegramBotToken, settings.TelegramChatID, agentName, settings.MsgClose)
		case "tamper":
			go h.tgBot.NotifyTamper(settings.TelegramBotToken, settings.TelegramChatID, agentName, alertMsg.Message)
		}
	}
}

func (h *Hub) handleCaptchaResponse(ac *agentConn, resp *model.CaptchaResponseMessage) {
	ac.mu.Lock()
	pending, ok := ac.pending[resp.ID]
	if ok {
		delete(ac.pending, resp.ID)
	}
	ac.mu.Unlock()

	if !ok {
		return
	}

	agent, _ := h.store.GetAgent(ac.agentID)
	agentName := ac.agentID
	if agent != nil {
		agentName = agent.Name
	}

	var result model.CaptchaResult
	var answer *string
	var responseMs *int64

	if resp.Answer == nil {
		result = model.ResultMiss
		h.notifier.Send(model.Alert{
			Type:      "miss",
			AgentID:   ac.agentID,
			AgentName: agentName,
			Message:   fmt.Sprintf("Agent %s missed CAPTCHA", agentName),
			Timestamp: time.Now(),
		})
		// Telegram
		settings, err := h.store.GetSettings()
		if err == nil && settings.TelegramEnabled {
			go h.tgBot.NotifyMiss(settings.TelegramBotToken, settings.TelegramChatID, agentName, settings.MsgMiss)
		}
	} else {
		answer = resp.Answer
		ms := resp.ResponseMs
		responseMs = &ms
		if strings.EqualFold(*resp.Answer, pending.Text) {
			result = model.ResultSuccess
			// Telegram success
			settings, err := h.store.GetSettings()
			if err == nil && settings.TelegramEnabled {
				go h.tgBot.NotifySuccess(settings.TelegramBotToken, settings.TelegramChatID, agentName, ms, settings.MsgSuccess)
			}
		} else {
			result = model.ResultFail
			// Telegram fail
			settings, err := h.store.GetSettings()
			if err == nil && settings.TelegramEnabled {
				go h.tgBot.NotifyFail(settings.TelegramBotToken, settings.TelegramChatID, agentName, settings.MsgFail)
			}
		}
	}

	h.store.UpdateCaptchaResult(pending.RecordID, answer, result, responseMs)

	ac.writeJSON(model.CaptchaResultMessage{
		Type:    model.MsgCaptchaResult,
		ID:      resp.ID,
		Correct: result == model.ResultSuccess,
	})
}

func (h *Hub) schedulerLoop(agentID string, stopCh chan struct{}) {
	for {
		settings, err := h.store.GetSettings()
		if err != nil {
			log.Printf("get settings error: %v", err)
			settings = model.DefaultSettings()
		}

		// If auto send disabled, wait and re-check
		if !settings.AutoSendEnabled {
			timer := time.NewTimer(30 * time.Second)
			select {
			case <-stopCh:
				timer.Stop()
				return
			case <-timer.C:
			}
			continue
		}

		// Check skip time ranges
		if settings.IsInSkipTime(time.Now()) {
			timer := time.NewTimer(60 * time.Second)
			select {
			case <-stopCh:
				timer.Stop()
				return
			case <-timer.C:
			}
			continue
		}

		intervalRange := settings.MaxIntervalSec - settings.MinIntervalSec
		if intervalRange <= 0 {
			intervalRange = 1
		}
		interval := settings.MinIntervalSec + h.rng.Intn(intervalRange)
		timer := time.NewTimer(time.Duration(interval) * time.Second)

		select {
		case <-stopCh:
			timer.Stop()
			return
		case <-timer.C:
		}

		// Re-check
		settings, err = h.store.GetSettings()
		if err == nil && (!settings.AutoSendEnabled || settings.IsInSkipTime(time.Now())) {
			continue
		}

		// Don't send if pending
		h.mu.RLock()
		ac, online := h.agents[agentID]
		h.mu.RUnlock()
		if online {
			ac.mu.Lock()
			hasPending := len(ac.pending) > 0
			ac.mu.Unlock()
			if hasPending {
				log.Printf("agent %s has pending captcha, skipping", agentID)
				continue
			}
		}

		h.sendCaptchaToAgent(agentID, settings)
	}
}

func (h *Hub) sendCaptchaToAgent(agentID string, settings model.Settings) {
	h.mu.RLock()
	ac, ok := h.agents[agentID]
	h.mu.RUnlock()
	if !ok {
		return
	}

	text, imgBytes, err := h.gen.GenerateEx(settings.CaptchaLength, settings.NoiseLevel, settings.CaptchaCharset, settings.ImageWidth, settings.ImageHeight)
	if err != nil {
		log.Printf("captcha gen error: %v", err)
		return
	}

	captchaID := uuid.New().String()
	sentAt := time.Now().UTC()

	recordID, err := h.store.RecordCaptcha(agentID, text, sentAt)
	if err != nil {
		log.Printf("record captcha error: %v", err)
		return
	}

	pending := &pendingCaptcha{
		ID:         captchaID,
		RecordID:   recordID,
		Text:       text,
		SentAt:     sentAt,
		TimeoutSec: settings.TimeoutSec,
	}

	ac.mu.Lock()
	ac.pending[captchaID] = pending
	ac.mu.Unlock()

	challenge := model.CaptchaChallengeMessage{
		Type:         model.MsgCaptchaChallenge,
		ID:           captchaID,
		Image:        base64.StdEncoding.EncodeToString(imgBytes),
		TimeoutSec:   settings.TimeoutSec,
		PopupMessage: settings.PopupMessage,
	}

	if err := ac.writeJSON(challenge); err != nil {
		log.Printf("send captcha error: %v", err)
		return
	}

	// Timeout goroutine
	go func() {
		time.Sleep(time.Duration(settings.TimeoutSec+5) * time.Second)
		ac.mu.Lock()
		if _, still := ac.pending[captchaID]; still {
			delete(ac.pending, captchaID)
			ac.mu.Unlock()

			h.store.UpdateCaptchaResult(recordID, nil, model.ResultMiss, nil)

			agent, _ := h.store.GetAgent(agentID)
			agentName := agentID
			if agent != nil {
				agentName = agent.Name
			}
			h.notifier.Send(model.Alert{
				Type:      "miss",
				AgentID:   agentID,
				AgentName: agentName,
				Message:   fmt.Sprintf("Agent %s timed out on CAPTCHA", agentName),
				Timestamp: time.Now(),
			})
			// Telegram
			s, err := h.store.GetSettings()
			if err == nil && s.TelegramEnabled {
				go h.tgBot.NotifyMiss(s.TelegramBotToken, s.TelegramChatID, agentName, s.MsgMiss)
			}
		} else {
			ac.mu.Unlock()
		}
	}()

	log.Printf("sent captcha to agent %s (id=%s, text=%s)", agentID, captchaID, text)
}

func (h *Hub) IsAgentOnline(agentID string) bool {
	h.mu.RLock()
	defer h.mu.RUnlock()
	_, ok := h.agents[agentID]
	return ok
}

func (h *Hub) OnlineCount() int {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return len(h.agents)
}

func (h *Hub) ManualSendCaptcha(agentIDs []string) map[string]string {
	settings, err := h.store.GetSettings()
	if err != nil {
		settings = model.DefaultSettings()
	}
	results := make(map[string]string)
	for _, id := range agentIDs {
		if !h.IsAgentOnline(id) {
			results[id] = "offline"
			continue
		}
		h.mu.RLock()
		ac, ok := h.agents[id]
		h.mu.RUnlock()
		if !ok {
			results[id] = "not_found"
			continue
		}
		ac.mu.Lock()
		hasPending := len(ac.pending) > 0
		ac.mu.Unlock()
		if hasPending {
			results[id] = "pending"
			continue
		}
		h.sendCaptchaToAgent(id, settings)
		results[id] = "sent"
	}
	return results
}

func (h *Hub) SendConfigUpdate(settings model.Settings) {
	h.mu.RLock()
	defer h.mu.RUnlock()
	msg := model.ConfigUpdateMessage{
		Type:       model.MsgConfigUpdate,
		TimeoutSec: settings.TimeoutSec,
	}
	for _, ac := range h.agents {
		ac.writeJSON(msg)
	}
}

func (h *Hub) SendRemoteCommand(agentIDs []string, command string) map[string]string {
	results := make(map[string]string)
	h.mu.RLock()
	defer h.mu.RUnlock()
	for _, id := range agentIDs {
		ac, ok := h.agents[id]
		if !ok {
			results[id] = "offline"
			continue
		}
		msg := model.RemoteCommandMessage{
			Type:    model.MsgRemoteCommand,
			Command: command,
		}
		if err := ac.writeJSON(msg); err != nil {
			results[id] = "error"
		} else {
			results[id] = "sent"
		}
	}
	return results
}
