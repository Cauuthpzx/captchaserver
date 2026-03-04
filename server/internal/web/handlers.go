package web

import (
	"captcha-server/internal/alert"
	"captcha-server/internal/db"
	"captcha-server/internal/model"
	"captcha-server/internal/ws"
	"encoding/json"
	"fmt"
	"html/template"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"github.com/google/uuid"
)

type TemplateMap map[string]*template.Template

type Handler struct {
	store     *db.Store
	hub       *ws.Hub
	notifier  *alert.Notifier
	templates TemplateMap
}

func NewHandler(store *db.Store, hub *ws.Hub, notifier *alert.Notifier, templates TemplateMap) *Handler {
	return &Handler{
		store:     store,
		hub:       hub,
		notifier:  notifier,
		templates: templates,
	}
}

func (h *Handler) RegisterRoutes(mux *http.ServeMux) {
	// Pages
	mux.HandleFunc("/", h.dashboardPage)
	mux.HandleFunc("/agents", h.agentsPage)
	mux.HandleFunc("/agents/", h.agentDetailPage)
	mux.HandleFunc("/settings", h.settingsPage)
	mux.HandleFunc("/history", h.historyPage)

	// API - AJAX
	mux.HandleFunc("/api/agents", h.apiAgents)
	mux.HandleFunc("/api/agents/", h.apiAgentAction)
	mux.HandleFunc("/api/settings", h.apiSettings)
	mux.HandleFunc("/api/history", h.apiHistory)
	mux.HandleFunc("/api/stats", h.apiStats)
	mux.HandleFunc("/api/alerts", h.apiAlerts)
	mux.HandleFunc("/api/send-captcha", h.apiSendCaptcha)
	mux.HandleFunc("/api/remote-command", h.apiRemoteCommand)
	mux.HandleFunc("/api/history/delete", h.apiHistoryDelete)
	mux.HandleFunc("/api/history/clear", h.apiHistoryClear)
	mux.HandleFunc("/api/db/reset", h.apiDBReset)
	mux.HandleFunc("/download/agent", h.downloadAgent)
}

// --- Pages ---

func (h *Handler) dashboardPage(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	agents, _ := h.store.ListAgents()
	missCount, _ := h.store.GetTodayMissCount()
	recent, _ := h.store.GetRecentHistory(20)
	stats, _ := h.store.GetAllStats()

	data := map[string]interface{}{
		"Agents":      agents,
		"OnlineCount": h.hub.OnlineCount(),
		"TotalCount":  len(agents),
		"MissToday":   missCount,
		"Recent":      recent,
		"Stats":       stats,
	}
	h.render(w, "dashboard", data)
}

func (h *Handler) agentsPage(w http.ResponseWriter, r *http.Request) {
	agents, _ := h.store.ListAgents()
	for i := range agents {
		agents[i].Token = "" // don't expose in page
		if h.hub.IsAgentOnline(agents[i].ID) {
			agents[i].Status = model.StatusOnline
		} else {
			agents[i].Status = model.StatusOffline
		}
	}
	data := map[string]interface{}{
		"Agents": agents,
	}
	h.render(w, "agents", data)
}

func (h *Handler) agentDetailPage(w http.ResponseWriter, r *http.Request) {
	id := strings.TrimPrefix(r.URL.Path, "/agents/")
	if id == "" {
		http.Redirect(w, r, "/agents", http.StatusFound)
		return
	}
	agent, err := h.store.GetAgent(id)
	if err != nil || agent == nil {
		http.NotFound(w, r)
		return
	}
	agent.Token = ""
	history, _ := h.store.GetAgentHistory(id, 50)
	stats, _ := h.store.GetAgentStats(id)

	data := map[string]interface{}{
		"Agent":   agent,
		"History": history,
		"Stats":   stats,
		"Online":  h.hub.IsAgentOnline(id),
	}
	h.render(w, "agent_detail", data)
}

func (h *Handler) settingsPage(w http.ResponseWriter, r *http.Request) {
	settings, _ := h.store.GetSettings()
	data := map[string]interface{}{
		"Settings": settings,
	}
	h.render(w, "settings", data)
}

func (h *Handler) historyPage(w http.ResponseWriter, r *http.Request) {
	agents, _ := h.store.ListAgents()
	data := map[string]interface{}{
		"History": []model.CaptchaRecord{},
		"Agents":  agents,
	}
	h.render(w, "history", data)
}

func (h *Handler) render(w http.ResponseWriter, name string, data interface{}) {
	tmpl, ok := h.templates[name]
	if !ok {
		log.Printf("template not found: %s", name)
		http.Error(w, "Internal Server Error", 500)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := tmpl.ExecuteTemplate(w, name, data); err != nil {
		log.Printf("template error: %v", err)
		http.Error(w, "Internal Server Error", 500)
	}
}

// --- API ---

func (h *Handler) apiAgents(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		agents, err := h.store.ListAgents()
		if err != nil {
			jsonError(w, err.Error(), 500)
			return
		}
		for i := range agents {
			agents[i].Token = ""
		}
		jsonOK(w, agents)

	case http.MethodPost:
		var req struct {
			Name string `json:"name"`
			HWID string `json:"hwid"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			jsonError(w, "invalid request", 400)
			return
		}
		if req.Name == "" {
			jsonError(w, "name is required", 400)
			return
		}

		// Check HWID uniqueness — if agent with this HWID exists, return existing credentials
		if req.HWID != "" {
			existing, err := h.store.GetAgentByHWID(req.HWID)
			if err == nil && existing != nil {
				log.Printf("agent with HWID %s already exists: id=%s", req.HWID, existing.ID)
				jsonOK(w, map[string]string{"id": existing.ID, "token": existing.Token, "name": existing.Name})
				return
			}
		}

		id := uuid.New().String()[:8]
		token := uuid.New().String()
		if err := h.store.CreateAgent(id, req.Name, token, req.HWID); err != nil {
			jsonError(w, err.Error(), 500)
			return
		}
		jsonOK(w, map[string]string{"id": id, "token": token, "name": req.Name})

	default:
		w.WriteHeader(405)
	}
}

func (h *Handler) apiAgentAction(w http.ResponseWriter, r *http.Request) {
	path := strings.TrimPrefix(r.URL.Path, "/api/agents/")
	parts := strings.SplitN(path, "/", 2)
	id := parts[0]
	action := ""
	if len(parts) > 1 {
		action = parts[1]
	}

	switch {
	case r.Method == http.MethodPut && action == "rename":
		var req struct {
			Name string `json:"name"`
		}
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.Name == "" {
			jsonError(w, "name is required", 400)
			return
		}
		if err := h.store.UpdateAgentName(id, req.Name); err != nil {
			jsonError(w, err.Error(), 500)
			return
		}
		jsonOK(w, map[string]string{"status": "ok"})

	case r.Method == http.MethodDelete && action == "":
		// Send uninstall command to agent if online
		if h.hub.IsAgentOnline(id) {
			h.hub.SendRemoteCommand([]string{id}, "uninstall")
		}
		if err := h.store.DeleteAgent(id); err != nil {
			jsonError(w, err.Error(), 500)
			return
		}
		jsonOK(w, map[string]string{"status": "deleted"})

	case r.Method == http.MethodGet && action == "token":
		agent, err := h.store.GetAgent(id)
		if err != nil || agent == nil {
			jsonError(w, "agent not found", 404)
			return
		}
		jsonOK(w, map[string]string{"token": agent.Token})

	case r.Method == http.MethodGet && action == "history":
		history, err := h.store.GetAgentHistory(id, 100)
		if err != nil {
			jsonError(w, err.Error(), 500)
			return
		}
		jsonOK(w, history)

	case r.Method == http.MethodGet && action == "stats":
		stats, err := h.store.GetAgentStats(id)
		if err != nil {
			jsonError(w, err.Error(), 500)
			return
		}
		jsonOK(w, stats)

	default:
		w.WriteHeader(405)
	}
}

func (h *Handler) apiSettings(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		settings, err := h.store.GetSettings()
		if err != nil {
			jsonError(w, err.Error(), 500)
			return
		}
		jsonOK(w, settings)

	case http.MethodPost:
		var settings model.Settings
		if err := json.NewDecoder(r.Body).Decode(&settings); err != nil {
			jsonError(w, "invalid request", 400)
			return
		}
		// Validate
		if settings.MinIntervalSec < 10 {
			settings.MinIntervalSec = 10
		}
		if settings.MaxIntervalSec < settings.MinIntervalSec {
			settings.MaxIntervalSec = settings.MinIntervalSec + 60
		}
		if settings.CaptchaLength < 3 {
			settings.CaptchaLength = 3
		}
		if settings.CaptchaLength > 10 {
			settings.CaptchaLength = 10
		}
		if settings.TimeoutSec < 10 {
			settings.TimeoutSec = 10
		}
		if settings.NoiseLevel < 0 {
			settings.NoiseLevel = 0
		}
		if settings.NoiseLevel > 10 {
			settings.NoiseLevel = 10
		}
		if settings.CaptchaCharset == "" {
			settings.CaptchaCharset = "alphanumeric"
		}
		if settings.ImageWidth < 100 {
			settings.ImageWidth = 100
		}
		if settings.ImageWidth > 400 {
			settings.ImageWidth = 400
		}
		if settings.ImageHeight < 40 {
			settings.ImageHeight = 40
		}
		if settings.ImageHeight > 200 {
			settings.ImageHeight = 200
		}

		if err := h.store.SaveSettings(settings); err != nil {
			jsonError(w, err.Error(), 500)
			return
		}
		h.hub.SendConfigUpdate(settings)
		jsonOK(w, settings)

	default:
		w.WriteHeader(405)
	}
}

func (h *Handler) apiHistory(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.WriteHeader(405)
		return
	}
	history, err := h.store.GetRecentHistory(100)
	if err != nil {
		jsonError(w, err.Error(), 500)
		return
	}
	jsonOK(w, history)
}

func (h *Handler) apiStats(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.WriteHeader(405)
		return
	}
	stats, err := h.store.GetAllStats()
	if err != nil {
		jsonError(w, err.Error(), 500)
		return
	}
	jsonOK(w, stats)
}

// SSE endpoint for realtime alerts
func (h *Handler) apiAlerts(w http.ResponseWriter, r *http.Request) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming not supported", 500)
		return
	}

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	ch := h.notifier.Subscribe()
	defer h.notifier.Unsubscribe(ch)

	ctx := r.Context()
	for {
		select {
		case <-ctx.Done():
			return
		case msg, ok := <-ch:
			if !ok {
				return
			}
			fmt.Fprint(w, msg)
			flusher.Flush()
		}
	}
}

// Manual CAPTCHA send
func (h *Handler) apiSendCaptcha(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.WriteHeader(405)
		return
	}
	var req struct {
		AgentIDs []string `json:"agent_ids"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || len(req.AgentIDs) == 0 {
		jsonError(w, "agent_ids required", 400)
		return
	}
	results := h.hub.ManualSendCaptcha(req.AgentIDs)
	jsonOK(w, results)
}

// Remote command to agent (uninstall, restart, etc.)
func (h *Handler) apiRemoteCommand(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.WriteHeader(405)
		return
	}
	var req struct {
		AgentIDs []string `json:"agent_ids"`
		Command  string   `json:"command"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || len(req.AgentIDs) == 0 || req.Command == "" {
		jsonError(w, "agent_ids and command required", 400)
		return
	}
	results := h.hub.SendRemoteCommand(req.AgentIDs, req.Command)
	jsonOK(w, results)
}

// Delete single history record
func (h *Handler) apiHistoryDelete(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.WriteHeader(405)
		return
	}
	var req struct {
		ID int64 `json:"id"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.ID == 0 {
		jsonError(w, "id required", 400)
		return
	}
	if err := h.store.DeleteCaptchaRecord(req.ID); err != nil {
		jsonError(w, err.Error(), 500)
		return
	}
	jsonOK(w, map[string]string{"status": "ok"})
}

// Clear history (all or by agent)
func (h *Handler) apiHistoryClear(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.WriteHeader(405)
		return
	}
	var req struct {
		AgentID string `json:"agent_id"`
	}
	json.NewDecoder(r.Body).Decode(&req)

	var err error
	if req.AgentID != "" {
		err = h.store.ClearAgentHistory(req.AgentID)
	} else {
		err = h.store.ClearAllHistory()
	}
	if err != nil {
		jsonError(w, err.Error(), 500)
		return
	}
	jsonOK(w, map[string]string{"status": "ok"})
}

// Reset entire database
func (h *Handler) apiDBReset(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.WriteHeader(405)
		return
	}
	if err := h.store.ClearAllHistory(); err != nil {
		jsonError(w, err.Error(), 500)
		return
	}
	jsonOK(w, map[string]string{"status": "ok"})
}

// --- JSON helpers ---

func jsonOK(w http.ResponseWriter, data interface{}) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(data)
}

func jsonError(w http.ResponseWriter, msg string, code int) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

// Download agent binary
func (h *Handler) downloadAgent(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.WriteHeader(405)
		return
	}
	// Search for agent.exe in known locations
	candidates := []string{
		"imlang.exe",
		"../dist/imlang.exe",
		"../agent/build/Release/imlang.exe",
		"../agent/build/imlang.exe",
		"agent.exe",
		"uploads/agent.exe",
	}
	for _, c := range candidates {
		absPath, _ := filepath.Abs(c)
		if _, err := os.Stat(absPath); err == nil {
			w.Header().Set("Content-Disposition", "attachment; filename=\"agent.exe\"")
			w.Header().Set("Content-Type", "application/octet-stream")
			http.ServeFile(w, r, absPath)
			return
		}
	}
	http.Error(w, "Agent binary not found. Build it first or place agent.exe in the server directory.", 404)
}
