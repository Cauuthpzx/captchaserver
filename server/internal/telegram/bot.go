package telegram

import (
	"fmt"
	"log"
	"net/http"
	"net/url"
	"strings"
	"time"
)

type Bot struct {
	client *http.Client
}

func NewBot() *Bot {
	return &Bot{
		client: &http.Client{Timeout: 10 * time.Second},
	}
}

func (b *Bot) SendMessage(botToken, chatID, text string) error {
	if botToken == "" || chatID == "" {
		return nil
	}
	apiURL := fmt.Sprintf("https://api.telegram.org/bot%s/sendMessage", botToken)
	resp, err := b.client.PostForm(apiURL, url.Values{
		"chat_id":    {chatID},
		"text":       {text},
		"parse_mode": {"HTML"},
	})
	if err != nil {
		log.Printf("telegram send error: %v", err)
		return err
	}
	resp.Body.Close()
	if resp.StatusCode != 200 {
		log.Printf("telegram API returned status %d", resp.StatusCode)
	}
	return nil
}

// formatMsg replaces {name} and {time} placeholders and wraps name in bold
func formatMsg(template, agentName string, responseMs int64) string {
	msg := strings.ReplaceAll(template, "{name}", "<b>"+agentName+"</b>")
	msg = strings.ReplaceAll(msg, "{time}", fmt.Sprintf("%d", responseMs))
	return msg
}

// NotifySuccess sends success notification
func (b *Bot) NotifySuccess(botToken, chatID, agentName string, responseMs int64, msgTemplate string) {
	msg := "✅ " + formatMsg(msgTemplate, agentName, responseMs)
	b.SendMessage(botToken, chatID, msg)
}

// NotifyFail sends fail notification
func (b *Bot) NotifyFail(botToken, chatID, agentName string, msgTemplate string) {
	msg := "❌ " + formatMsg(msgTemplate, agentName, 0)
	b.SendMessage(botToken, chatID, msg)
}

// NotifyMiss sends miss/timeout notification
func (b *Bot) NotifyMiss(botToken, chatID, agentName string, msgTemplate string) {
	msg := "⏰ " + formatMsg(msgTemplate, agentName, 0)
	b.SendMessage(botToken, chatID, msg)
}

// NotifyClosePopup sends close popup warning
func (b *Bot) NotifyClosePopup(botToken, chatID, agentName string, msgTemplate string) {
	msg := "⚠️ " + formatMsg(msgTemplate, agentName, 0)
	b.SendMessage(botToken, chatID, msg)
}

// NotifyUninstall sends uninstall attempt warning
func (b *Bot) NotifyUninstall(botToken, chatID, agentName string, msgTemplate string) {
	msg := "🚨 " + formatMsg(msgTemplate, agentName, 0)
	b.SendMessage(botToken, chatID, msg)
}

// NotifyTamper sends tamper detection warning
func (b *Bot) NotifyTamper(botToken, chatID, agentName, detail string) {
	msg := fmt.Sprintf("🔴 Nhân viên <b>%s</b> - phát hiện can thiệp: %s", agentName, detail)
	b.SendMessage(botToken, chatID, msg)
}
