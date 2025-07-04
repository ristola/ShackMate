// Global variables
let ws;
let wsConnected = false;
let reconnectAttempts = 0;
let maxReconnectAttempts = 10;
let reconnectDelay = 1000; // Start with 1 second
let lastPingSent = 0;
let pingInterval = 30000; // 30 seconds
let pingTimeout = 5000; // 5 seconds

// Initialize the app
document.addEventListener('DOMContentLoaded', function() {
    // Update the timestamp
    updateTimestamp();
    
    // Set up the reset stats button
    const resetBtn = document.getElementById('reset-stats-btn');
    if (resetBtn) {
        resetBtn.addEventListener('click', resetStats);
    }
    
    // Start WebSocket connection
    connectWebSocket();
    
    // Set up periodic ping
    setInterval(sendPing, pingInterval);
    
    // Update timestamp every minute
    setInterval(updateTimestamp, 60000);
});

function connectWebSocket() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.host}/ws`;
    
    try {
        ws = new WebSocket(wsUrl);
        
        ws.onopen = function(event) {
            console.log('WebSocket connected');
            wsConnected = true;
            reconnectAttempts = 0;
            reconnectDelay = 1000; // Reset delay
            updateConnectionStatus('connected');
        };
        
        ws.onmessage = function(event) {
            try {
                const data = JSON.parse(event.data);
                updateDashboard(data);
            } catch (e) {
                console.log('Received non-JSON message:', event.data);
            }
        };
        
        ws.onclose = function(event) {
            console.log('WebSocket disconnected');
            wsConnected = false;
            updateConnectionStatus('disconnected');
            scheduleReconnect();
        };
        
        ws.onerror = function(error) {
            console.error('WebSocket error:', error);
            wsConnected = false;
            updateConnectionStatus('error');
        };
        
    } catch (error) {
        console.error('Failed to create WebSocket:', error);
        scheduleReconnect();
    }
}

function scheduleReconnect() {
    if (reconnectAttempts < maxReconnectAttempts) {
        reconnectAttempts++;
        console.log(`Attempting to reconnect in ${reconnectDelay}ms (attempt ${reconnectAttempts}/${maxReconnectAttempts})`);
        
        setTimeout(() => {
            if (!wsConnected) {
                connectWebSocket();
            }
        }, reconnectDelay);
        
        // Exponential backoff with jitter
        reconnectDelay = Math.min(reconnectDelay * 1.5 + Math.random() * 1000, 30000);
    } else {
        console.log('Max reconnection attempts reached');
        updateConnectionStatus('failed');
    }
}

function updateConnectionStatus(status) {
    const statusElement = document.getElementById('ws-connection');
    if (statusElement) {
        statusElement.textContent = status === 'connected' ? 'Connected' : 
                                  status === 'error' ? 'Error' : 
                                  status === 'failed' ? 'Failed' : 'Disconnected';
        statusElement.className = `status ${status === 'connected' ? 'connected' : 'disconnected'}`;
    }
}

function sendPing() {
    if (wsConnected && ws && ws.readyState === WebSocket.OPEN) {
        const now = Date.now();
        if (now - lastPingSent >= pingInterval) {
            ws.send(JSON.stringify({type: 'ping', timestamp: now}));
            lastPingSent = now;
            console.log('Ping sent');
        }
    }
}

function updateDashboard(data) {
    // Update all the dashboard elements based on received data
    for (const [key, value] of Object.entries(data)) {
        const element = document.getElementById(key);
        if (element) {
            if (typeof value === 'object' && value !== null) {
                if (value.value !== undefined) {
                    element.querySelector('.value').textContent = value.value;
                }
            } else {
                element.textContent = value;
            }
        }
    }
    
    // Special handling for specific fields
    if (data['ws-connection-quality'] !== undefined) {
        const qualityElement = document.getElementById('ws-quality');
        if (qualityElement) {
            const valueSpan = qualityElement.querySelector('.value');
            if (valueSpan) {
                valueSpan.textContent = data['ws-connection-quality'];
            }
        }
    }
    
    if (data['ws-ping-rtt'] !== undefined) {
        const rttElement = document.getElementById('ws-ping-rtt');
        if (rttElement) {
            const valueSpan = rttElement.querySelector('.value');
            if (valueSpan) {
                valueSpan.textContent = data['ws-ping-rtt'];
            }
        }
    }
    
    if (data.cpu0_usage !== undefined) {
        const cpu0Element = document.getElementById('cpu0-usage');
        if (cpu0Element) {
            const valueSpan = cpu0Element.querySelector('.value');
            if (valueSpan) {
                valueSpan.textContent = data.cpu0_usage;
            }
        }
    }
    
    if (data.cpu1_usage !== undefined) {
        const cpu1Element = document.getElementById('cpu1-usage');
        if (cpu1Element) {
            const valueSpan = cpu1Element.querySelector('.value');
            if (valueSpan) {
                valueSpan.textContent = data.cpu1_usage;
            }
        }
    }
    
    if (data.free_heap !== undefined) {
        const heapElement = document.getElementById('free-heap');
        if (heapElement) {
            const valueSpan = heapElement.querySelector('.value');
            if (valueSpan) {
                valueSpan.textContent = Math.round(data.free_heap / 1024);
            }
        }
    }
    
    if (data.cpu_freq !== undefined) {
        const freqElement = document.getElementById('cpu-freq');
        if (freqElement) {
            const valueSpan = freqElement.querySelector('.value');
            if (valueSpan) {
                valueSpan.textContent = data.cpu_freq;
            }
        }
    }
    
    // Update serial statistics
    if (data.serial1_valid !== undefined) {
        const serial1Element = document.getElementById('serial1-stats-values');
        if (serial1Element) {
            serial1Element.textContent = `✅ ${data.serial1_valid} / ❌ ${data.serial1_corrupt || 0} | 📢 ${data.serial1_broadcast || 0}`;
        }
    }
    
    if (data.serial2_valid !== undefined) {
        const serial2Element = document.getElementById('serial2-stats-values');
        if (serial2Element) {
            serial2Element.textContent = `✅ ${data.serial2_valid} / ❌ ${data.serial2_corrupt || 0} | 📢 ${data.serial2_broadcast || 0}`;
        }
    }
    
    if (data.ws_rx !== undefined && data.ws_tx !== undefined) {
        const wsStatsElement = document.getElementById('ws-stats-values');
        if (wsStatsElement) {
            wsStatsElement.textContent = `RX ${data.ws_rx} / TX ${data.ws_tx}`;
        }
    }
}

function resetStats() {
    if (wsConnected && ws && ws.readyState === WebSocket.OPEN) {
        fetch('/reset-stats', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            }
        })
        .then(response => response.json())
        .then(data => {
            console.log('Stats reset successful:', data);
            // The dashboard will be updated via WebSocket
        })
        .catch(error => {
            console.error('Error resetting stats:', error);
        });
    } else {
        // Fallback to direct fetch if WebSocket is not available
        fetch('/reset-stats', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            }
        })
        .then(response => response.json())
        .then(data => {
            console.log('Stats reset successful:', data);
            // Reload the page to get updated stats
            window.location.reload();
        })
        .catch(error => {
            console.error('Error resetting stats:', error);
        });
    }
}

function updateTimestamp() {
    const timestampElements = document.querySelectorAll('.timestamp');
    const now = new Date();
    const timeString = now.toLocaleString();
    
    timestampElements.forEach(element => {
        element.textContent = timeString;
    });
}