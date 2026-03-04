package main

import (
	"captcha-server/internal/alert"
	"captcha-server/internal/captcha"
	"captcha-server/internal/db"
	"captcha-server/internal/web"
	"captcha-server/internal/ws"
	"flag"
	"fmt"
	"html/template"
	"log"
	"net/http"
	"os"
	"path/filepath"
)

func main() {
	addr := flag.String("addr", ":8080", "HTTP listen address")
	dbPath := flag.String("db", "captcha.db", "SQLite database path")
	flag.Parse()

	// Database
	store, err := db.New(*dbPath)
	if err != nil {
		log.Fatalf("failed to open database: %v", err)
	}
	defer store.Close()
	log.Println("database initialized")

	// Components
	gen := captcha.NewGenerator()
	notifier := alert.NewNotifier()
	hub := ws.NewHub(store, gen, notifier)

	// Templates
	templates := loadTemplates()

	// HTTP
	handler := web.NewHandler(store, hub, notifier, templates)
	mux := http.NewServeMux()
	handler.RegisterRoutes(mux)

	// WebSocket
	mux.HandleFunc("/ws", hub.HandleWS)

	// Static files
	staticDir := findStaticDir()
	mux.Handle("/static/", http.StripPrefix("/static/", http.FileServer(http.Dir(staticDir))))

	log.Printf("server starting on %s", *addr)
	if err := http.ListenAndServe(*addr, mux); err != nil {
		log.Fatalf("server error: %v", err)
	}
}

func loadTemplates() web.TemplateMap {
	funcMap := template.FuncMap{
		"map": func(pairs ...interface{}) map[string]interface{} {
			m := make(map[string]interface{})
			for i := 0; i < len(pairs)-1; i += 2 {
				key, _ := pairs[i].(string)
				m[key] = pairs[i+1]
			}
			return m
		},
		"deref": func(s *string) string {
			if s == nil {
				return ""
			}
			return *s
		},
		"deref_i64": func(i *int64) int64 {
			if i == nil {
				return 0
			}
			return *i
		},
	}

	templatesDir := findTemplatesDir()
	layoutFile := filepath.Join(templatesDir, "layout.html")

	// Parse each page template individually with layout to avoid "content" block conflicts
	pages := map[string]string{
		"dashboard":    "dashboard.html",
		"agents":       "agents.html",
		"agent_detail": "agent_detail.html",
		"settings":     "settings.html",
		"history":      "history.html",
	}

	templates := make(web.TemplateMap)
	for name, file := range pages {
		t := template.Must(
			template.New("").Funcs(funcMap).ParseFiles(layoutFile, filepath.Join(templatesDir, file)),
		)
		templates[name] = t
	}
	return templates
}

func findTemplatesDir() string {
	candidates := []string{
		"templates",
		"server/templates",
		"../templates",
		"../../templates",
	}
	for _, c := range candidates {
		if info, err := os.Stat(c); err == nil && info.IsDir() {
			return c
		}
	}
	log.Fatal("templates directory not found")
	return ""
}

func findStaticDir() string {
	candidates := []string{
		"static",
		"server/static",
		"../static",
		"../../static",
	}
	for _, c := range candidates {
		if info, err := os.Stat(c); err == nil && info.IsDir() {
			return c
		}
	}
	fmt.Println("warning: static directory not found, serving from ./static")
	return "static"
}
