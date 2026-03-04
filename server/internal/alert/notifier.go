package alert

import (
	"captcha-server/internal/model"
	"encoding/json"
	"fmt"
	"sync"
)

type Notifier struct {
	mu      sync.RWMutex
	clients map[chan string]struct{}
}

func NewNotifier() *Notifier {
	return &Notifier{
		clients: make(map[chan string]struct{}),
	}
}

func (n *Notifier) Subscribe() chan string {
	ch := make(chan string, 16)
	n.mu.Lock()
	n.clients[ch] = struct{}{}
	n.mu.Unlock()
	return ch
}

func (n *Notifier) Unsubscribe(ch chan string) {
	n.mu.Lock()
	delete(n.clients, ch)
	n.mu.Unlock()
	close(ch)
}

func (n *Notifier) Send(alert model.Alert) {
	data, err := json.Marshal(alert)
	if err != nil {
		return
	}
	msg := fmt.Sprintf("data: %s\n\n", data)
	n.mu.RLock()
	defer n.mu.RUnlock()
	for ch := range n.clients {
		select {
		case ch <- msg:
		default:
			// drop if client is slow
		}
	}
}
