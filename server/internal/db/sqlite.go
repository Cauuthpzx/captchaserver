package db

import (
	"captcha-server/internal/model"
	"database/sql"
	"fmt"
	"strconv"
	"sync"
	"time"

	_ "github.com/mattn/go-sqlite3"
)

type Store struct {
	db *sql.DB
	mu sync.RWMutex
}

func New(path string) (*Store, error) {
	db, err := sql.Open("sqlite3", path+"?_journal_mode=WAL&_busy_timeout=5000")
	if err != nil {
		return nil, fmt.Errorf("open db: %w", err)
	}
	db.SetMaxOpenConns(1)
	s := &Store{db: db}
	if err := s.migrate(); err != nil {
		db.Close()
		return nil, fmt.Errorf("migrate: %w", err)
	}
	return s, nil
}

func (s *Store) Close() error {
	return s.db.Close()
}

func (s *Store) migrate() error {
	// Core tables — hwid NOT in initial schema so old DBs don't fail on index
	schema := `
	CREATE TABLE IF NOT EXISTS agents (
		id         TEXT PRIMARY KEY,
		name       TEXT NOT NULL,
		token      TEXT NOT NULL,
		status     TEXT DEFAULT 'offline',
		last_seen  DATETIME,
		created_at DATETIME DEFAULT CURRENT_TIMESTAMP
	);
	CREATE TABLE IF NOT EXISTS captcha_history (
		id           INTEGER PRIMARY KEY AUTOINCREMENT,
		agent_id     TEXT REFERENCES agents(id),
		captcha_text TEXT NOT NULL,
		answer       TEXT,
		result       TEXT NOT NULL,
		response_ms  INTEGER,
		sent_at      DATETIME NOT NULL,
		answered_at  DATETIME
	);
	CREATE TABLE IF NOT EXISTS settings (
		key   TEXT PRIMARY KEY,
		value TEXT NOT NULL
	);
	CREATE INDEX IF NOT EXISTS idx_history_agent ON captcha_history(agent_id);
	CREATE INDEX IF NOT EXISTS idx_history_sent ON captcha_history(sent_at);
	`
	_, err := s.db.Exec(schema)
	if err != nil {
		return err
	}

	// Migration: add hwid column to existing agents table (ignore if already exists)
	s.db.Exec("ALTER TABLE agents ADD COLUMN hwid TEXT NOT NULL DEFAULT ''")

	// Create hwid index after column is guaranteed to exist
	s.db.Exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_agents_hwid ON agents(hwid) WHERE hwid != ''")

	return nil
}

// --- Agents ---

func (s *Store) CreateAgent(id, name, token, hwid string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, err := s.db.Exec(
		"INSERT INTO agents (id, name, token, hwid, status, created_at) VALUES (?, ?, ?, ?, 'offline', CURRENT_TIMESTAMP)",
		id, name, token, hwid,
	)
	return err
}

// GetAgentByHWID finds an existing agent by HWID
func (s *Store) GetAgentByHWID(hwid string) (*model.Agent, error) {
	if hwid == "" {
		return nil, nil
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	row := s.db.QueryRow("SELECT id, name, token, hwid, status, last_seen, created_at FROM agents WHERE hwid = ?", hwid)
	return scanAgent(row)
}

func (s *Store) GetAgent(id string) (*model.Agent, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	row := s.db.QueryRow("SELECT id, name, token, hwid, status, last_seen, created_at FROM agents WHERE id = ?", id)
	return scanAgent(row)
}

func (s *Store) ListAgents() ([]model.Agent, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	rows, err := s.db.Query("SELECT id, name, token, hwid, status, last_seen, created_at FROM agents ORDER BY name")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var agents []model.Agent
	for rows.Next() {
		a, err := scanAgentRows(rows)
		if err != nil {
			return nil, err
		}
		agents = append(agents, *a)
	}
	return agents, rows.Err()
}

func (s *Store) UpdateAgentStatus(id string, status model.AgentStatus) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, err := s.db.Exec("UPDATE agents SET status = ?, last_seen = CURRENT_TIMESTAMP WHERE id = ?", status, id)
	return err
}

func (s *Store) UpdateAgentHWID(id, hwid string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, err := s.db.Exec("UPDATE agents SET hwid = ? WHERE id = ?", hwid, id)
	return err
}

func (s *Store) UpdateAgentName(id, name string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, err := s.db.Exec("UPDATE agents SET name = ? WHERE id = ?", name, id)
	return err
}

func (s *Store) DeleteAgent(id string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.Exec("DELETE FROM captcha_history WHERE agent_id = ?", id); err != nil {
		return err
	}
	if _, err := tx.Exec("DELETE FROM agents WHERE id = ?", id); err != nil {
		return err
	}
	return tx.Commit()
}

func (s *Store) AuthenticateAgent(id, token string) (*model.Agent, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	row := s.db.QueryRow("SELECT id, name, token, hwid, status, last_seen, created_at FROM agents WHERE id = ? AND token = ?", id, token)
	return scanAgent(row)
}

// --- Captcha History ---

func (s *Store) RecordCaptcha(agentID, captchaText string, sentAt time.Time) (int64, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	res, err := s.db.Exec(
		"INSERT INTO captcha_history (agent_id, captcha_text, result, sent_at) VALUES (?, ?, 'pending', ?)",
		agentID, captchaText, sentAt,
	)
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

func (s *Store) UpdateCaptchaResult(captchaID int64, answer *string, result model.CaptchaResult, responseMs *int64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	var answeredAt interface{}
	if result != model.ResultMiss {
		now := time.Now().UTC()
		answeredAt = now
	}
	_, err := s.db.Exec(
		"UPDATE captcha_history SET answer = ?, result = ?, response_ms = ?, answered_at = ? WHERE id = ?",
		answer, result, responseMs, answeredAt, captchaID,
	)
	return err
}

func (s *Store) GetAgentHistory(agentID string, limit int) ([]model.CaptchaRecord, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	rows, err := s.db.Query(
		`SELECT h.id, h.agent_id, a.name, h.captcha_text, h.answer, h.result, h.response_ms, h.sent_at, h.answered_at
		 FROM captcha_history h JOIN agents a ON h.agent_id = a.id
		 WHERE h.agent_id = ? ORDER BY h.sent_at DESC LIMIT ?`,
		agentID, limit,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return scanRecords(rows)
}

func (s *Store) GetRecentHistory(limit int) ([]model.CaptchaRecord, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	rows, err := s.db.Query(
		`SELECT h.id, h.agent_id, a.name, h.captcha_text, h.answer, h.result, h.response_ms, h.sent_at, h.answered_at
		 FROM captcha_history h JOIN agents a ON h.agent_id = a.id
		 ORDER BY h.sent_at DESC LIMIT ?`,
		limit,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	return scanRecords(rows)
}

type AgentStats struct {
	AgentID    string  `json:"agent_id"`
	AgentName  string  `json:"agent_name"`
	Total      int     `json:"total"`
	Success    int     `json:"success"`
	Fail       int     `json:"fail"`
	Miss       int     `json:"miss"`
	AvgRespMs  float64 `json:"avg_response_ms"`
	SuccessRate float64 `json:"success_rate"`
}

func (s *Store) GetAgentStats(agentID string) (*AgentStats, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	row := s.db.QueryRow(`
		SELECT
			a.id, a.name,
			COUNT(*) as total,
			SUM(CASE WHEN h.result='success' THEN 1 ELSE 0 END) as success,
			SUM(CASE WHEN h.result='fail' THEN 1 ELSE 0 END) as fail,
			SUM(CASE WHEN h.result='miss' THEN 1 ELSE 0 END) as miss,
			COALESCE(AVG(CASE WHEN h.response_ms IS NOT NULL THEN h.response_ms END), 0) as avg_resp
		FROM captcha_history h JOIN agents a ON h.agent_id = a.id
		WHERE h.agent_id = ? AND h.result != 'pending'
		GROUP BY a.id, a.name
	`, agentID)
	var st AgentStats
	err := row.Scan(&st.AgentID, &st.AgentName, &st.Total, &st.Success, &st.Fail, &st.Miss, &st.AvgRespMs)
	if err == sql.ErrNoRows {
		return &AgentStats{AgentID: agentID}, nil
	}
	if err != nil {
		return nil, err
	}
	if st.Total > 0 {
		st.SuccessRate = float64(st.Success) / float64(st.Total) * 100
	}
	return &st, nil
}

func (s *Store) GetAllStats() ([]AgentStats, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	rows, err := s.db.Query(`
		SELECT
			a.id, a.name,
			COUNT(*) as total,
			SUM(CASE WHEN h.result='success' THEN 1 ELSE 0 END),
			SUM(CASE WHEN h.result='fail' THEN 1 ELSE 0 END),
			SUM(CASE WHEN h.result='miss' THEN 1 ELSE 0 END),
			COALESCE(AVG(CASE WHEN h.response_ms IS NOT NULL THEN h.response_ms END), 0)
		FROM captcha_history h JOIN agents a ON h.agent_id = a.id
		WHERE h.result != 'pending'
		GROUP BY a.id, a.name
		ORDER BY a.name
	`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var stats []AgentStats
	for rows.Next() {
		var st AgentStats
		if err := rows.Scan(&st.AgentID, &st.AgentName, &st.Total, &st.Success, &st.Fail, &st.Miss, &st.AvgRespMs); err != nil {
			return nil, err
		}
		if st.Total > 0 {
			st.SuccessRate = float64(st.Success) / float64(st.Total) * 100
		}
		stats = append(stats, st)
	}
	return stats, rows.Err()
}

func (s *Store) GetTodayMissCount() (int, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	var count int
	err := s.db.QueryRow(
		"SELECT COUNT(*) FROM captcha_history WHERE result = 'miss' AND date(sent_at) = date('now')",
	).Scan(&count)
	return count, err
}

func (s *Store) DeleteCaptchaRecord(id int64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, err := s.db.Exec("DELETE FROM captcha_history WHERE id = ?", id)
	return err
}

func (s *Store) ClearAgentHistory(agentID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, err := s.db.Exec("DELETE FROM captcha_history WHERE agent_id = ?", agentID)
	return err
}

func (s *Store) ClearAllHistory() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	_, err := s.db.Exec("DELETE FROM captcha_history")
	return err
}

// --- Settings ---

func (s *Store) GetSettings() (model.Settings, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	def := model.DefaultSettings()
	rows, err := s.db.Query("SELECT key, value FROM settings")
	if err != nil {
		return def, err
	}
	defer rows.Close()
	m := make(map[string]string)
	for rows.Next() {
		var k, v string
		if err := rows.Scan(&k, &v); err != nil {
			return def, err
		}
		m[k] = v
	}
	if v, ok := m["min_interval_sec"]; ok {
		def.MinIntervalSec, _ = strconv.Atoi(v)
	}
	if v, ok := m["max_interval_sec"]; ok {
		def.MaxIntervalSec, _ = strconv.Atoi(v)
	}
	if v, ok := m["captcha_length"]; ok {
		def.CaptchaLength, _ = strconv.Atoi(v)
	}
	if v, ok := m["timeout_sec"]; ok {
		def.TimeoutSec, _ = strconv.Atoi(v)
	}
	if v, ok := m["noise_level"]; ok {
		def.NoiseLevel, _ = strconv.Atoi(v)
	}
	if v, ok := m["auto_send_enabled"]; ok {
		def.AutoSendEnabled = v == "1" || v == "true"
	}
	if v, ok := m["captcha_charset"]; ok && v != "" {
		def.CaptchaCharset = v
	}
	if v, ok := m["image_width"]; ok {
		def.ImageWidth, _ = strconv.Atoi(v)
	}
	if v, ok := m["image_height"]; ok {
		def.ImageHeight, _ = strconv.Atoi(v)
	}
	if v, ok := m["skip_time_ranges"]; ok {
		def.SkipTimeRanges = v
	}
	if v, ok := m["telegram_enabled"]; ok {
		def.TelegramEnabled = v == "1" || v == "true"
	}
	if v, ok := m["telegram_bot_token"]; ok {
		def.TelegramBotToken = v
	}
	if v, ok := m["telegram_chat_id"]; ok {
		def.TelegramChatID = v
	}
	if v, ok := m["msg_success"]; ok && v != "" {
		def.MsgSuccess = v
	}
	if v, ok := m["msg_fail"]; ok && v != "" {
		def.MsgFail = v
	}
	if v, ok := m["msg_miss"]; ok && v != "" {
		def.MsgMiss = v
	}
	if v, ok := m["msg_close"]; ok && v != "" {
		def.MsgClose = v
	}
	if v, ok := m["msg_uninstall"]; ok && v != "" {
		def.MsgUninstall = v
	}
	if v, ok := m["popup_message"]; ok && v != "" {
		def.PopupMessage = v
	}
	return def, rows.Err()
}

func (s *Store) SaveSettings(settings model.Settings) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	autoSend := "0"
	if settings.AutoSendEnabled {
		autoSend = "1"
	}
	pairs := map[string]string{
		"min_interval_sec":  strconv.Itoa(settings.MinIntervalSec),
		"max_interval_sec":  strconv.Itoa(settings.MaxIntervalSec),
		"captcha_length":    strconv.Itoa(settings.CaptchaLength),
		"timeout_sec":       strconv.Itoa(settings.TimeoutSec),
		"noise_level":       strconv.Itoa(settings.NoiseLevel),
		"auto_send_enabled": autoSend,
		"captcha_charset":   settings.CaptchaCharset,
		"image_width":       strconv.Itoa(settings.ImageWidth),
		"image_height":      strconv.Itoa(settings.ImageHeight),
		"skip_time_ranges":  settings.SkipTimeRanges,
		"telegram_enabled":  func() string { if settings.TelegramEnabled { return "1" }; return "0" }(),
		"telegram_bot_token": settings.TelegramBotToken,
		"telegram_chat_id":  settings.TelegramChatID,
		"msg_success":       settings.MsgSuccess,
		"msg_fail":          settings.MsgFail,
		"msg_miss":          settings.MsgMiss,
		"msg_close":         settings.MsgClose,
		"msg_uninstall":     settings.MsgUninstall,
		"popup_message":     settings.PopupMessage,
	}
	for k, v := range pairs {
		if _, err := tx.Exec("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)", k, v); err != nil {
			return err
		}
	}
	return tx.Commit()
}

// --- scan helpers ---

type scanner interface {
	Scan(dest ...interface{}) error
}

func scanAgent(row scanner) (*model.Agent, error) {
	var a model.Agent
	var lastSeen sql.NullTime
	err := row.Scan(&a.ID, &a.Name, &a.Token, &a.HWID, &a.Status, &lastSeen, &a.CreatedAt)
	if err != nil {
		return nil, err
	}
	if lastSeen.Valid {
		a.LastSeen = &lastSeen.Time
	}
	return &a, nil
}

func scanAgentRows(rows *sql.Rows) (*model.Agent, error) {
	return scanAgent(rows)
}

func scanRecords(rows *sql.Rows) ([]model.CaptchaRecord, error) {
	var records []model.CaptchaRecord
	for rows.Next() {
		var r model.CaptchaRecord
		var answer sql.NullString
		var responseMs sql.NullInt64
		var answeredAt sql.NullTime
		err := rows.Scan(&r.ID, &r.AgentID, &r.AgentName, &r.CaptchaText, &answer, &r.Result, &responseMs, &r.SentAt, &answeredAt)
		if err != nil {
			return nil, err
		}
		if answer.Valid {
			r.Answer = &answer.String
		}
		if responseMs.Valid {
			r.ResponseMs = &responseMs.Int64
		}
		if answeredAt.Valid {
			r.AnsweredAt = &answeredAt.Time
		}
		records = append(records, r)
	}
	return records, rows.Err()
}
