// ShackMate CI-V Controller Web Interface
// =============================================

class ShackMateUI {
    constructor() {
        this.ws = null;
        this.pingInterval = null;
        this.init();
    }

    init() {
        document.addEventListener('DOMContentLoaded', () => {
            this.updateTimestamp();
            setInterval(() => this.updateTimestamp(), 1000);
            this.connectWebSocket();
            this.setupEventHandlers();
        });
    }

    // Update timestamp display
    updateTimestamp() {
        const now = new Date();
        let hours = now.getHours();
        const minutes = now.getMinutes().toString().padStart(2, '0');
        const seconds = now.getSeconds().toString().padStart(2, '0');
        const ampm = hours >= 12 ? 'PM' : 'AM';
        hours = hours % 12;
        hours = hours ? hours : 12;
        const timeStr = hours + ':' + minutes + ':' + seconds + ' ' + ampm;
        document.querySelectorAll('.timestamp').forEach(el => el.textContent = timeStr);
    }

    // WebSocket connection management
    connectWebSocket() {
        this.ws = new WebSocket('ws://' + location.hostname + '/ws');
        
        this.ws.onopen = () => {
            console.log('WebSocket connected');
            this.updateWsStatus('connected');
            this.startPing();
        };

        this.ws.onmessage = (event) => {
            try {
                const data = JSON.parse(event.data);
                this.updateStatusFromData(data);
                if (data.ws_status) this.updateWsStatus(data.ws_status);
                if (data.uptime) this.updateElement('uptime', data.uptime);
            } catch (e) {
                // Not JSON or not a status update
                console.debug('Non-JSON message received:', event.data);
            }
        };

        this.ws.onclose = () => {
            console.log('WebSocket closed, retrying in 5s...');
            this.updateWsStatus('disconnected');
            this.stopPing();
            setTimeout(() => this.connectWebSocket(), 5000);
        };

        this.ws.onerror = (err) => {
            console.error('WebSocket error:', err);
        };
    }

    // Start ping interval
    startPing() {
        if (this.pingInterval) clearInterval(this.pingInterval);
        this.pingInterval = setInterval(() => {
            if (this.ws.readyState === WebSocket.OPEN) {
                this.ws.send('ping');
            }
        }, 30000);
    }

    // Stop ping interval
    stopPing() {
        if (this.pingInterval) {
            clearInterval(this.pingInterval);
            this.pingInterval = null;
        }
    }

    // Update WebSocket status in UI
    updateWsStatus(status) {
        const wsElem = document.getElementById('ws-connection');
        if (wsElem) {
            wsElem.textContent = (status === 'connected') ? 'Connected' : 'Disconnected';
            wsElem.className = 'status ' + (status === 'connected' ? 'connected' : 'disconnected');
        }
    }

    // Update UI elements from received data
    updateStatusFromData(data) {
        // Map WebSocket data keys to HTML element IDs
        const keyMapping = {
            'ip': 'ip-address',
            'chip_id': 'chip-id',
            'cpu_freq': 'cpu-freq',
            'free_heap': 'free-heap',
            'civ_baud': 'civ-baud',
            'civ_addr': 'civ-addr',
            'version': 'version',
            'uptime': 'uptime'
        };

        // Update mapped elements
        Object.keys(keyMapping).forEach(key => {
            if (data[key] !== undefined) {
                this.updateElement(keyMapping[key], data[key]);
            }
        });

        // Handle WebSocket server info immediately (higher priority)
        if (data.ws_server_ip !== undefined) {
            console.log('Updating WebSocket server info:', data.ws_server_ip, data.ws_server_port);
            const wsServerElem = document.getElementById('ws-server');
            if (wsServerElem) {
                if (data.ws_server_ip === "Not discovered" || data.ws_server_ip === "") {
                    wsServerElem.textContent = "Not discovered";
                    wsServerElem.className = "info-value";
                } else {
                    const serverInfo = data.ws_server_port ? 
                        `${data.ws_server_ip}:${data.ws_server_port}` : 
                        data.ws_server_ip;
                    wsServerElem.textContent = serverInfo;
                    wsServerElem.className = "info-value discovered";
                }
            } else {
                console.error('ws-server element not found');
            }
        }

        // Serial1 statistics
        if (data.serial1_valid !== undefined && data.serial1_invalid !== undefined && data.serial1_broadcast !== undefined) {
            this.updateElement('serial1-stats-values', 
                `✅ ${data.serial1_valid} / ❌ ${data.serial1_invalid} | 📢 ${data.serial1_broadcast}`);
        }

        // Serial2 statistics
        if (data.serial2_valid !== undefined && data.serial2_invalid !== undefined && data.serial2_broadcast !== undefined) {
            this.updateElement('serial2-stats-values', 
                `✅ ${data.serial2_valid} / ❌ ${data.serial2_invalid} | 📢 ${data.serial2_broadcast}`);
        }

        // WebSocket statistics
        if (data.ws_rx !== undefined && data.ws_tx !== undefined) {
            this.updateElement('ws-stats-values', `RX ${data.ws_rx} / TX ${data.ws_tx}`);
        }

        if (data.ws_dup !== undefined) {
            this.updateElement('ws-dup', data.ws_dup);
        }

        // WebSocket status
        if (data.ws_status !== undefined) {
            this.updateWsStatus(data.ws_status);
        }
    }

    // Helper to update element content
    updateElement(id, content) {
        const element = document.getElementById(id);
        if (element) {
            if (typeof content === 'object') {
                element.innerHTML = content;
            } else {
                element.textContent = content;
            }
        }
    }

    // Setup event handlers
    setupEventHandlers() {
        // Reset stats button
        const resetBtn = document.getElementById('reset-stats-btn');
        if (resetBtn) {
            resetBtn.addEventListener('click', () => {
                this.resetStats();
            });
        }
    }

    // Reset statistics
    async resetStats() {
        try {
            const response = await fetch('/reset_stats', { method: 'POST' });
            const data = await response.json();
            console.log('Stats reset:', data);
            
            // Reset UI counters immediately for better UX
            this.updateElement('serial1-stats-values', '✅ 0 / ❌ 0 | 📢 0');
            this.updateElement('serial2-stats-values', '✅ 0 / ❌ 0 | 📢 0');
            this.updateElement('ws-stats-values', 'RX 0 / TX 0');
            this.updateElement('ws-dup', '0');
        } catch (err) {
            console.error('Error resetting stats:', err);
        }
    }
}

// Initialize the UI when the script loads
new ShackMateUI();
