document.addEventListener("DOMContentLoaded", function() {
  // Default manual/automatic button state – using a dedicated variable (autoTrackState)
  const modeToggle = document.getElementById("mode-toggle");
  const toggleRect = document.getElementById("toggle-button-rect");
  let autoTrackState = true;  // true = AUTOMATIC, false = MANUAL
  if (modeToggle) modeToggle.textContent = "AUTOMATIC";
  if (toggleRect) toggleRect.setAttribute("fill", "red");

  // Global flag for the satellite link (port 4532 connection)
  let port4532Connected = false; // Updated elsewhere

  // GLOBAL VARIABLES for simulation and display
  let rotorAZPosition = 0;
  let rotorELPosition = 0;
  let targetAZ = 0;
  let targetEL = 0;
  const AZ_SPEED = 360 / 58;  // ~6.21°/s
  const EL_SPEED = 180 / 67;  // ~2.69°/s
  let lastTimestamp = null;

  let calibrateActive = false;
  let rotorsEnabled = true;

  // Satellite info – updated via macDoppler messages.
  let satName = "";
  let channelName = "";
  let lastSatName = "";
  let lastChannelName = "";

  // To avoid duplicate broadcasts
  let prevBroadcast = "";
  let lastBroadcastTime = 0;

  // Keypad variables
  let keypadInput = "";
  let activeMeter = null; // "AZ" or "EL"

  // --- Global Variables for computed lat/lon ---
  // gridSQ is now defined in the HTML template.
  let storedLat = null;
  let storedLon = null;
  let lastGridSquare = "";

  // For throttling grid info update (in ms)
  let lastGridInfoUpdate = 0;

  // --- WebSocket Connection ---
  const ws = new WebSocket("ws://" + window.location.hostname + ":" + WEBSOCKET_PORT + "/ws");
  ws.onopen = () => {
    console.log("WebSocket connected on port " + WEBSOCKET_PORT);
  };
  ws.onmessage = (event) => {
    console.log("Received:", event.data);
    try {
      const data = JSON.parse(event.data);
      if (data.command && data.command === "stateUpdate") {
        broadcastStateUpdate();
        return;
      }
      if (data.type === "stateUpdate") {
        if (data.targetAZ !== undefined) targetAZ = data.targetAZ;
        if (data.targetEL !== undefined) targetEL = data.targetEL;
        if (data.autoButton) {
          autoTrackState = (data.autoButton === "AUTOMATIC");
          if (autoTrackState) {
            if (modeToggle) modeToggle.textContent = "AUTOMATIC";
            if (toggleRect) toggleRect.setAttribute("fill", "red");
          } else {
            if (modeToggle) modeToggle.textContent = "MANUAL";
            if (toggleRect) toggleRect.setAttribute("fill", "blue");
          }
        }
        if (typeof data.rotorsEnabled === "boolean") {
          rotorsEnabled = data.rotorsEnabled;
          const rotorsToggleText = document.getElementById("rotors-toggle-text");
          const rotorsToggleRect = document.getElementById("rotors-toggle-rect");
          if (rotorsEnabled) {
            if (rotorsToggleText) rotorsToggleText.textContent = "ROTORS ENABLED";
            if (rotorsToggleRect) rotorsToggleRect.setAttribute("fill", "green");
          } else {
            if (rotorsToggleText) rotorsToggleText.textContent = "ROTORS STOPPED";
            if (rotorsToggleRect) rotorsToggleRect.setAttribute("fill", "red");
          }
        }
        if (data.satName && data.satName !== "") {
          satName = data.satName;
          lastSatName = satName;
        } else {
          satName = lastSatName;
        }
        if (data.channelName && data.channelName !== "") {
          channelName = data.channelName;
          lastChannelName = channelName;
        } else {
          channelName = lastChannelName;
        }
        if (data.satIndicator) {
          port4532Connected = (data.satIndicator === "ENABLED");
        }
        // Update gridSQ if sent from the ESP preferences
        if (data.gridSquare && data.gridSquare !== "") {
          gridSQ = data.gridSquare;
        }
        updateDisplay();
      } else if (data.type === "macDoppler") {
        if (data.satName && data.satName !== "") {
          satName = data.satName;
          lastSatName = satName;
        }
        if (data.channelName && data.channelName !== "") {
          channelName = data.channelName;
          lastChannelName = channelName;
        }
        console.log("Updated Sat info:", satName, channelName);
        updateDisplay();
      }
    } catch (e) {
      console.error("Error parsing message", e);
    }
  };
  ws.onerror = (error) => { console.error("WebSocket error:", error); };
  ws.onclose = () => {
    console.log("WebSocket connection closed");
    if (modeToggle) modeToggle.textContent = "MANUAL";
    if (toggleRect) toggleRect.setAttribute("fill", "blue");
    updateDisplay();
  };

  function broadcastStateUpdate() {
    if (ws.readyState !== WebSocket.OPEN) return;
    const now = Date.now();
    if (now - lastBroadcastTime < 100) return; // Debounce
    lastBroadcastTime = now;
    const newState = {
      type: "stateUpdate",
      targetAZ: Math.round(targetAZ),
      targetEL: Math.round(targetEL),
      rotorsEnabled: rotorsEnabled,
      satName: satName,
      channelName: channelName,
      satIndicator: port4532Connected ? "ENABLED" : "DISABLED",
      autoButton: autoTrackState ? "AUTOMATIC" : "MANUAL",
      gridSquare: gridSQ
    };
    const stateStr = JSON.stringify(newState);
    if (stateStr !== prevBroadcast) {
      ws.send(stateStr);
      prevBroadcast = stateStr;
      console.log("Broadcast state:", stateStr);
    }
  }

  // Load rotor positions from localStorage (if any)
  let storedAZ = localStorage.getItem("rotorAZPosition");
  rotorAZPosition = (storedAZ !== null && !isNaN(parseFloat(storedAZ))) ? parseFloat(storedAZ) : 0;
  let storedEL = localStorage.getItem("rotorELPosition");
  rotorELPosition = (storedEL !== null && !isNaN(parseFloat(storedEL))) ? parseFloat(storedEL) : 0;
  targetAZ = rotorAZPosition;
  targetEL = rotorELPosition;

  // Optional: Set M1–M6 memory buttons to gray
  for (let i = 1; i <= 6; i++) {
    const rect = document.getElementById("M" + i + "_rect");
    if (rect) rect.style.fill = "gray";
  }
  // Check stored memory for each memory button and update its background
  function checkMemoryButtons() {
    for (let i = 1; i <= 6; i++) {
      const rect = document.getElementById("M" + i + "_rect");
      if (rect) {
        fetch(`/getMemory?slot=${i}`)
          .then(response => response.json())
          .then(data => {
            if (data.az !== 0 || data.el !== 0) {
              rect.style.fill = "blue";
            } else {
              rect.style.fill = "gray";
            }
          })
          .catch(err => console.error("Error checking memory for M" + i, err));
      }
    }
  }
  checkMemoryButtons();

  // Build analog meter scales
  buildAnalogAZ();
  buildAnalogEL();

  updateDisplay();
  requestAnimationFrame(animate);

  // -----------------------------
  // Keypad Setup (show/hide, display update)
  // -----------------------------
  function showKeypad() {
    const keypadGroup = document.getElementById("keypadGroup");
    if (keypadGroup) {
      keypadGroup.style.display = "block";
      console.log("Keypad shown");
    }
  }
  function hideKeypad() {
    const keypadGroup = document.getElementById("keypadGroup");
    if (keypadGroup) {
      keypadGroup.style.display = "none";
      console.log("Keypad hidden");
    }
  }
  function updateKeypadDisplay() {
    const keypadDisplay = document.getElementById("keypadDisplay");
    if (keypadDisplay) {
      keypadDisplay.textContent = keypadInput;
    }
  }
  function updateKeypadPrompt() {
    const keypadPrompt = document.getElementById("keypadPrompt");
    if (keypadPrompt) {
      keypadPrompt.textContent = (activeMeter === "AZ") ? "Enter AZ Value:" :
                                 (activeMeter === "EL") ? "Enter EL Value:" : "";
    }
  }

  // Event listeners for meter groups to show/hide keypad
  ["analogAZGroup", "analogELGroup", "digitalAZGroup", "digitalELGroup"].forEach(id => {
    const el = document.getElementById(id);
    if (el) {
      el.addEventListener("mousedown", function(evt) {
        evt.stopPropagation();
        const keypadGroup = document.getElementById("keypadGroup");
        if (keypadGroup && keypadGroup.style.display === "block") {
          hideKeypad();
        } else {
          activeMeter = (id.indexOf("AZ") > -1) ? "AZ" : "EL";
          keypadInput = "";
          updateKeypadPrompt();
          updateKeypadDisplay();
          showKeypad();
        }
      });
      el.addEventListener("click", function(evt) {
        evt.stopPropagation();
        const keypadGroup = document.getElementById("keypadGroup");
        if (keypadGroup && keypadGroup.style.display === "block") {
          hideKeypad();
        } else {
          activeMeter = (id.indexOf("AZ") > -1) ? "AZ" : "EL";
          keypadInput = "";
          updateKeypadPrompt();
          updateKeypadDisplay();
          showKeypad();
        }
      });
    }
  });

  // Hide keypad when clicking outside of its area
  const mainSvg = document.getElementById("mainSvg");
  if (mainSvg) {
    mainSvg.addEventListener("click", function(evt) {
      if (!evt.target.closest("#keypadGroup") &&
          !evt.target.closest("#analogAZGroup") &&
          !evt.target.closest("#analogELGroup") &&
          !evt.target.closest("#digitalAZGroup") &&
          !evt.target.closest("#digitalELGroup")) {
        hideKeypad();
      }
      const calPopup = document.getElementById("calibrate-popup");
      if (calPopup && !evt.target.closest("#calibrate-popup") &&
          !evt.target.closest("#cal-button")) {
        calPopup.style.display = "none";
      }
    });
  } else {
    console.error("mainSvg element not found");
  }

  document.querySelectorAll(".keypad-btn").forEach(function(btn) {
    btn.addEventListener("click", function(evt) {
      evt.stopPropagation();
      const val = btn.getAttribute("data-value");
      if (val === "CLEAR") {
        keypadInput = "";
      } else if (val === "ENTER") {
        if (activeMeter === "AZ") {
          const newVal = parseInt(keypadInput);
          if (!isNaN(newVal)) targetAZ = (newVal + 360) % 360;
        } else if (activeMeter === "EL") {
          const newVal = parseInt(keypadInput);
          if (!isNaN(newVal)) targetEL = Math.min(Math.max(newVal, 0), 90);
        }
        hideKeypad();
        updateDisplay();
        broadcastStateUpdate();
      } else {
        keypadInput += val;
      }
      updateKeypadDisplay();
    });
  });

  // -----------------------------
  // Manual/Automatic Button Setup
  // -----------------------------
  const toggleButton = document.getElementById("toggle-button");
  if (toggleButton) {
    toggleButton.addEventListener("click", function() {
      autoTrackState = !autoTrackState;
      if (autoTrackState) {
        if (modeToggle) modeToggle.textContent = "AUTOMATIC";
        if (toggleRect) toggleRect.setAttribute("fill", "red");
      } else {
        if (modeToggle) modeToggle.textContent = "MANUAL";
        if (toggleRect) toggleRect.setAttribute("fill", "blue");
      }
      console.log("Manual/Automatic toggled:", modeToggle ? modeToggle.textContent : "");
      broadcastStateUpdate();
    });
  }

  // -----------------------------
  // Calibrate Button & Popup Setup
  // -----------------------------
  const calButton = document.getElementById("cal-button");
  if (calButton) {
    calButton.addEventListener("click", function(evt) {
      evt.stopPropagation();
      const popup = document.getElementById("calibrate-popup");
      if (popup) {
        popup.style.display = (popup.style.display === "none" || popup.style.display === "") ? "block" : "none";
      }
    });
  }
  const popupMagnetic = document.getElementById("popup-magnetic");
  const popupSolar = document.getElementById("popup-solar");
  const popupManual = document.getElementById("popup-manual");
  if (popupMagnetic) {
    popupMagnetic.addEventListener("click", function(evt) {
      evt.stopPropagation();
      console.log("MAGNETIC ALIGN pressed");
      const popup = document.getElementById("calibrate-popup");
      if (popup) popup.style.display = "none";
    });
  }
  if (popupSolar) {
    popupSolar.addEventListener("click", function(evt) {
      evt.stopPropagation();
      console.log("SOLAR ALIGN pressed");
      const popup = document.getElementById("calibrate-popup");
      if (popup) popup.style.display = "none";
    });
  }
  if (popupManual) {
    popupManual.addEventListener("click", function(evt) {
      evt.stopPropagation();
      console.log("MANUAL OFFSET pressed");
      const popup = document.getElementById("calibrate-popup");
      if (popup) popup.style.display = "none";
    });
  }

  // -----------------------------
  // Rotors Toggle Button Setup
  // -----------------------------
  const rotorsToggleButton = document.getElementById("rotors-toggle-button");
  if (rotorsToggleButton) {
    rotorsToggleButton.addEventListener("click", function() {
      rotorsEnabled = !rotorsEnabled;
      const rotorsToggleText = document.getElementById("rotors-toggle-text");
      const rotorsToggleRect = document.getElementById("rotors-toggle-rect");
      if (rotorsEnabled) {
        if (rotorsToggleText) rotorsToggleText.textContent = "ROTORS ENABLED";
        if (rotorsToggleRect) rotorsToggleRect.setAttribute("fill", "green");
      } else {
        if (rotorsToggleText) rotorsToggleText.textContent = "ROTORS STOPPED";
        if (rotorsToggleRect) rotorsToggleRect.setAttribute("fill", "red");
      }
      console.log("Rotors toggled:", rotorsToggleText ? rotorsToggleText.textContent : "");
      broadcastStateUpdate();
    });
  }

  // -----------------------------
  // Plot Click Handler (to set targetAZ and targetEL)
  // -----------------------------
  (function(){
    const posPlot = document.getElementById("posPlot");
    if (posPlot) {
      posPlot.style.cursor = "crosshair";
      posPlot.addEventListener("pointerdown", function(evt) {
        const svg = posPlot.ownerSVGElement;
        const pt = svg.createSVGPoint();
        pt.x = evt.clientX;
        pt.y = evt.clientY;
        const matrix = posPlot.getScreenCTM().inverse();
        const localPt = pt.matrixTransform(matrix);
        const dx = localPt.x - 100;
        const dy = localPt.y - 100;
        let r = Math.sqrt(dx * dx + dy * dy);
        if (r > 90) r = 90;
        const angleDeg = Math.atan2(dy, dx) * (180 / Math.PI);
        targetAZ = (angleDeg + 90 + 360) % 360;
        targetEL = 90 - r;
        console.log("Plot clicked => targetAZ =", targetAZ, "targetEL =", targetEL);
        updateDisplay();
        broadcastStateUpdate();
      });
    }
  })();

  // -----------------------------
  // M1–M6 Buttons Setup (Memory Buttons)
  // -----------------------------
  document.querySelectorAll("#m-buttons .m-button").forEach((btn) => {
    const btnID = btn.getAttribute("data-btn");
    const rect = btn.querySelector("rect");
    let pointerDownTime = 0;
    let pointerPressed = false;
    let storeTimer = null;
    let clearTimer = null;
    let storedThisPress = false;
    let clearedThisPress = false;

    btn.addEventListener("pointerdown", (evt) => {
      evt.preventDefault();
      pointerPressed = true;
      pointerDownTime = Date.now();
      storedThisPress = false;
      clearedThisPress = false;

      // After 500ms, store the current rotor positions
      storeTimer = setTimeout(() => {
        const slot = btnID.replace("M", "");
        fetch(`/saveMemory?slot=${slot}&az=${rotorAZPosition}&el=${rotorELPosition}`)
          .then(response => response.text())
          .then(resp => {
            console.log(`(500ms) Stored for ${btnID}: AZ=${rotorAZPosition}, EL=${rotorELPosition}`);
            if (rect) rect.style.fill = "blue";
            checkMemoryButtons();
          })
          .catch(err => console.error("Error storing memory:", err));
        storedThisPress = true;
      }, 500);

      // After 3000ms, clear the memory for that slot
      clearTimer = setTimeout(() => {
        const slot = btnID.replace("M", "");
        fetch(`/saveMemory?slot=${slot}&az=0&el=0`)
          .then(response => response.text())
          .then(resp => {
            console.log(`(3000ms) Cleared stored location for ${btnID}`);
            if (rect) rect.style.fill = "gray";
            checkMemoryButtons();
          })
          .catch(err => console.error("Error clearing memory:", err));
        clearedThisPress = true;
        storedThisPress = false;
      }, 3000);
    });

    btn.addEventListener("pointerup", (evt) => {
      evt.preventDefault();
      if (!pointerPressed) return;
      pointerPressed = false;
      clearTimeout(storeTimer);
      clearTimeout(clearTimer);
      const duration = Date.now() - pointerDownTime;
      if (duration < 500) {
        // Short press: load memory
        const slot = btnID.replace("M", "");
        fetch(`/getMemory?slot=${slot}`)
          .then(response => response.json())
          .then(data => {
            if (data.az !== 0 || data.el !== 0) {
              targetAZ = data.az;
              targetEL = data.el;
              if (rect) rect.style.fill = "blue";
              console.log(`Short press: Loaded for ${btnID}: AZ=${targetAZ}, EL=${targetEL}`);
              updateDisplay();
              broadcastStateUpdate();
            } else {
              console.log(`Short press: No stored location for ${btnID}`);
            }
          })
          .catch(err => console.error("Error loading memory:", err));
      }
      else if (storedThisPress && !clearedThisPress) {
        console.log(`Medium press: Location stored for ${btnID}`);
      }
      else if (clearedThisPress) {
        console.log(`Long press: Location cleared for ${btnID}`);
      }
      evt.stopPropagation();
    });

    btn.addEventListener("pointerleave", () => {
      pointerPressed = false;
      clearTimeout(storeTimer);
      clearTimeout(clearTimer);
    });
  });

  // -----------------------------
  // Arrow Buttons Setup
  // -----------------------------
  const btnLeftCircle = document.getElementById("btnLeftCircle");
  if (btnLeftCircle) {
    btnLeftCircle.addEventListener("click", () => {
      targetAZ = (targetAZ + 1) % 360;
      console.log("Arrow Left => targetAZ =", targetAZ);
      updateDisplay();
      broadcastStateUpdate();
    });
  }
  const btnRightCircle = document.getElementById("btnRightCircle");
  if (btnRightCircle) {
    btnRightCircle.addEventListener("click", () => {
      targetAZ = (targetAZ - 1 + 360) % 360;
      console.log("Arrow Right => targetAZ =", targetAZ);
      updateDisplay();
      broadcastStateUpdate();
    });
  }
  const btnUpArrow = document.getElementById("btnUpArrow");
  if (btnUpArrow) {
    btnUpArrow.addEventListener("click", () => {
      targetEL = Math.min(targetEL + 1, 90);
      console.log("Arrow Up => targetEL =", targetEL);
      updateDisplay();
      broadcastStateUpdate();
    });
  }
  const btnDownArrow = document.getElementById("btnDownArrow");
  if (btnDownArrow) {
    btnDownArrow.addEventListener("click", () => {
      targetEL = Math.max(targetEL - 1, 0);
      console.log("Arrow Down => targetEL =", targetEL);
      updateDisplay();
      broadcastStateUpdate();
    });
  }

  // -----------------------------
  // Long-Press on Meters: Toggle Analog/Digital View
  // -----------------------------
  (function(){
    const longClickThreshold = 500;
    let pressTimer = null;
    const analogAZ = document.getElementById("analogAZGroup");
    const digitalAZ = document.getElementById("digitalAZGroup");
    const analogEL = document.getElementById("analogELGroup");
    const digitalEL = document.getElementById("digitalELGroup");
    if (!analogAZ || !digitalAZ || !analogEL || !digitalEL) return;
    function toggleMeters() {
      if (analogAZ.style.display === "none") {
        analogAZ.style.display = "block";
        digitalAZ.style.display = "none";
        analogEL.style.display = "block";
        digitalEL.style.display = "none";
      } else {
        analogAZ.style.display = "none";
        digitalAZ.style.display = "block";
        analogEL.style.display = "none";
        digitalEL.style.display = "block";
      }
      console.log("Meters toggled =>", (analogAZ.style.display === "none") ? "Digital" : "Analog");
      updateDisplay();
    }
    [analogAZ, digitalAZ, analogEL, digitalEL].forEach((g) => {
      g.style.cursor = "pointer";
      g.addEventListener("pointerdown", (evt) => {
        evt.preventDefault();
        pressTimer = setTimeout(toggleMeters, longClickThreshold);
      });
      g.addEventListener("pointerup", () => { clearTimeout(pressTimer); });
      g.addEventListener("pointerleave", () => { clearTimeout(pressTimer); });
    });
  })();

  // -----------------------------
  // Build Analog AZ Scale
  // -----------------------------
  function buildAnalogAZ() {
    const P0 = { x:50, y:300 }, P1 = { x:300, y:100 }, P2 = { x:550, y:300 };
    function headingToT(h) { return h / 450; }
    function bezierPoint(t, A, B, C) {
      const mt = 1 - t;
      return { 
        x: mt * mt * A.x + 2 * mt * t * B.x + t * t * C.x,
        y: mt * mt * A.y + 2 * mt * t * B.y + t * t * C.y
      };
    }
    function bezierTangent(t, A, B, C) {
      const mt = 1 - t;
      return {
        x: 2 * mt * (B.x - A.x) + 2 * t * (C.x - B.x),
        y: 2 * mt * (B.y - A.y) + 2 * t * (C.y - B.y)
      };
    }
    const midPt = bezierPoint(0.5, P0, P1, P2);
    function inwardNormal(t) {
      const pt = bezierPoint(t, P0, P1, P2);
      const tan = bezierTangent(t, P0, P1, P2);
      let nx = -tan.y, ny = tan.x;
      const len = Math.sqrt(nx * nx + ny * ny);
      nx /= len; ny /= len;
      const toMidX = midPt.x - pt.x, toMidY = midPt.y - pt.y;
      if (nx * toMidX + ny * toMidY < 0) { nx = -nx; ny = -ny; }
      return { x: nx, y: ny };
    }
    const TICKS = document.getElementById("azTicks");
    const LABELS = document.getElementById("azLabels");
    const PTR = document.getElementById("azPointer");
    if (!TICKS || !LABELS || !PTR) return;
    const midHeads = [45, 135, 225, 315, 405];
    function barCategory(h) {
      if (h % 90 === 0) return "wide";
      if (midHeads.indexOf(h) > -1) return "mid";
      return "narrow";
    }
    function barLength(cat) { return cat === "wide" ? 50 : 25; }
    function headingLabel(h) {
      if (h === 360) return "0°";
      if (h === 450) return "90°";
      return h + "°";
    }
    function setTickStyle(line, cat) {
      line.setAttribute("stroke", "white");
      line.setAttribute("stroke-width", (cat === "wide" || cat === "mid") ? "4" : "2");
    }
    for (let h = 0; h <= 450; h += 15) {
      const cat = barCategory(h);
      const t = headingToT(h);
      const pt = bezierPoint(t, P0, P1, P2);
      const normalUp = inwardNormal(t);
      const length = barLength(cat);
      const x2 = pt.x + length * normalUp.x;
      const y2 = pt.y + length * normalUp.y;
      const line = document.createElementNS("http://www.w3.org/2000/svg", "line");
      line.setAttribute("x1", pt.x);
      line.setAttribute("y1", pt.y);
      line.setAttribute("x2", x2);
      line.setAttribute("y2", y2);
      setTickStyle(line, cat);
      TICKS.appendChild(line);
      if (cat === "wide") {
        const labelDist = length + 15;
        const lx = pt.x + labelDist * normalUp.x;
        const ly = pt.y + labelDist * normalUp.y;
        const txt = document.createElementNS("http://www.w3.org/2000/svg", "text");
        txt.setAttribute("x", lx);
        txt.setAttribute("y", ly);
        txt.setAttribute("fill", "white");
        txt.setAttribute("font-size", "16");
        txt.setAttribute("font-weight", "bold");
        txt.setAttribute("text-anchor", "middle");
        txt.setAttribute("dominant-baseline", "middle");
        txt.textContent = headingLabel(h);
        LABELS.appendChild(txt);
      }
    }
    const pointerLine = document.createElementNS("http://www.w3.org/2000/svg", "line");
    pointerLine.setAttribute("id", "azPointerLine");
    pointerLine.setAttribute("x1", "300");
    pointerLine.setAttribute("y1", "420");
    pointerLine.setAttribute("x2", "300");
    pointerLine.setAttribute("y2", "420");
    pointerLine.setAttribute("stroke", "red");
    pointerLine.setAttribute("stroke-width", "3");
    pointerLine.setAttribute("stroke-linecap", "round");
    PTR.appendChild(pointerLine);
  }

  // -----------------------------
  // Build Analog EL Scale
  // -----------------------------
  function buildAnalogEL() {
    const P0 = { x:50, y:300 }, P1 = { x:300, y:100 }, P2 = { x:550, y:300 };
    function headingToT_el(h) { return h / 180; }
    function bezierPoint_el(t, A, B, C) {
      const mt = 1 - t;
      return { 
        x: mt * mt * A.x + 2 * mt * t * B.x + t * t * C.x,
        y: mt * mt * A.y + 2 * mt * t * B.y + t * t * C.y
      };
    }
    function bezierTangent_el(t, A, B, C) {
      const mt = 1 - t;
      return {
        x: 2 * mt * (B.x - A.x) + 2 * t * (C.x - B.x),
        y: 2 * mt * (B.y - A.y) + 2 * t * (C.y - B.y)
      };
    }
    const midPt_el = bezierPoint_el(0.5, P0, P1, P2);
    function inwardNormal_el(t) {
      const pt = bezierPoint_el(t, P0, P1, P2);
      const tan = bezierTangent_el(t, P0, P1, P2);
      let nx = -tan.y, ny = tan.x;
      const len = Math.sqrt(nx * nx + ny * ny);
      nx /= len; ny /= len;
      const toMidX = midPt_el.x - pt.x, toMidY = midPt_el.y - pt.y;
      if (nx * toMidX + ny * toMidY < 0) { nx = -nx; ny = -ny; }
      return { x: nx, y: ny };
    }
    const TICKS_EL = document.getElementById("elTicks");
    const LABELS_EL = document.getElementById("elLabels");
    const PTR_EL = document.getElementById("elPointer");
    if (!TICKS_EL || !LABELS_EL || !PTR_EL) return;
    function isMajorTick_el(h) { return Math.abs(h % 22.5) < 1e-9; }
    function shouldLabel_el(h) { return Math.abs(h % 45) < 1e-9; }
    function tickLength_el(isMajor) { return isMajor ? 50 : 25; }
    function setTickStyle_el(line, isMajor) {
      line.setAttribute("stroke", "white");
      line.setAttribute("stroke-width", isMajor ? "4" : "2");
    }
    function headingLabel_el(h) { return h + "°"; }
    for (let h = 0; h <= 180; h += 7.5) {
      const t = headingToT_el(h);
      const pt = bezierPoint_el(t, P0, P1, P2);
      const normalUp = inwardNormal_el(t);
      const major = isMajorTick_el(h);
      const len = tickLength_el(major);
      const x2 = pt.x + len * normalUp.x;
      const y2 = pt.y + len * normalUp.y;
      const line = document.createElementNS("http://www.w3.org/2000/svg", "line");
      line.setAttribute("x1", pt.x);
      line.setAttribute("y1", pt.y);
      line.setAttribute("x2", x2);
      line.setAttribute("y2", y2);
      setTickStyle_el(line, major);
      TICKS_EL.appendChild(line);
      if (shouldLabel_el(h)) {
        const labelDist = len + 15;
        const lx = pt.x + labelDist * normalUp.x;
        const ly = pt.y + labelDist * normalUp.y;
        const txt = document.createElementNS("http://www.w3.org/2000/svg", "text");
        txt.setAttribute("x", lx);
        txt.setAttribute("y", ly);
        txt.setAttribute("fill", "white");
        txt.setAttribute("font-size", "16");
        txt.setAttribute("font-weight", "bold");
        txt.setAttribute("text-anchor", "middle");
        txt.setAttribute("dominant-baseline", "middle");
        txt.textContent = headingLabel_el(h);
        LABELS_EL.appendChild(txt);
      }
    }
    const pointerLine_el = document.createElementNS("http://www.w3.org/2000/svg", "line");
    pointerLine_el.setAttribute("id", "elPointerLine");
    pointerLine_el.setAttribute("x1", "300");
    pointerLine_el.setAttribute("y1", "420");
    pointerLine_el.setAttribute("x2", "300");
    pointerLine_el.setAttribute("y2", "420");
    pointerLine_el.setAttribute("stroke", "red");
    pointerLine_el.setAttribute("stroke-width", "3");
    pointerLine_el.setAttribute("stroke-linecap", "round");
    PTR_EL.appendChild(pointerLine_el);
  }

  // -----------------------------
  // Update Display, Pointer Positions & Colors
  // -----------------------------
  function updateDisplay() {
    const rAZ = Math.round(rotorAZPosition);
    const rEL = Math.round(rotorELPosition);
    updateAzPointerLine();
    updateElPointerLine();
    const rotorAZBigDigital = document.getElementById("rotorAZBigDigital");
    if (rotorAZBigDigital) rotorAZBigDigital.textContent = ("000" + rAZ).slice(-3);
    const rotorELBigDigital = document.getElementById("rotorELBigDigital");
    if (rotorELBigDigital) rotorELBigDigital.textContent = ("000" + rEL).slice(-3);
    const tAAnalog = document.getElementById("targetAZDisplayAnalog");
    if (tAAnalog) tAAnalog.textContent = Math.round(targetAZ);
    const tADigital = document.getElementById("targetAZDisplayDigital");
    if (tADigital) tADigital.textContent = Math.round(targetAZ);
    const tEAnalog = document.getElementById("targetELDisplayAnalog");
    if (tEAnalog) tEAnalog.textContent = Math.round(targetEL);
    const tEDigital = document.getElementById("targetELDisplayDigital");
    if (tEDigital) tEDigital.textContent = Math.round(targetEL);

    let rTarget = 90 - targetEL;
    if (rTarget < 0) rTarget = 0;
    if (rTarget > 90) rTarget = 90;
    const angleTarget = (targetAZ - 90) * Math.PI / 180;
    const tx = 100 + rTarget * Math.cos(angleTarget);
    const ty = 100 + rTarget * Math.sin(angleTarget);
    const targetIndicator = document.getElementById("targetIndicator");
    if (targetIndicator) targetIndicator.setAttribute("transform", "translate(" + tx + "," + ty + ")");

    let rRotor = 90 - rotorELPosition;
    if (rRotor < 0) rRotor = 0;
    if (rRotor > 90) rRotor = 90;
    const angleRotor = (rotorAZPosition - 90) * Math.PI / 180;
    const rx = 100 + rRotor * Math.cos(angleRotor);
    const ry = 100 + rRotor * Math.sin(angleRotor);
    const rotorIndicator = document.getElementById("rotorIndicator");
    if (rotorIndicator) {
      rotorIndicator.setAttribute("cx", rx);
      rotorIndicator.setAttribute("cy", ry);
    }
    // Save rotor positions to localStorage as a fallback
    localStorage.setItem("rotorAZPosition", rotorAZPosition.toString());
    localStorage.setItem("rotorELPosition", rotorELPosition.toString());

    const satIndicatorGroup = document.getElementById("satIndicatorGroup");
    if (satIndicatorGroup) {
      satIndicatorGroup.style.display = port4532Connected ? "block" : "none";
    }
    const satInfo = document.getElementById("satInfo");
    if (satInfo) {
      satInfo.style.display = port4532Connected ? "block" : "none";
      const satNameText = document.getElementById("satNameText");
      const satChannelText = document.getElementById("satChannelText");
      if (satNameText) satNameText.textContent = satName;
      if (satChannelText) satChannelText.textContent = channelName;
    }
    
    // Update pointer colors (dummy function)
    updatePointerColor();
    // Update meter ellipse colors based on rotor movement
    updateMeterEllipseColors();
    // Throttle GridSQ info update: call updateGridInfo() at most once per second
    const nowGrid = Date.now();
    if (nowGrid - lastGridInfoUpdate > 1000) {
      updateGridInfo();
      lastGridInfoUpdate = nowGrid;
    }
  }

  // -----------------------------
  // Dummy Update Pointer Color Function
  // -----------------------------
  function updatePointerColor() {
    // This function is a placeholder.
  }

  // -----------------------------
  // Update only the pointer positions based on rotor positions
  // -----------------------------
  function updateAzPointerLine() {
    const t = rotorAZPosition / 450;
    const P0 = { x:50, y:300 }, P1 = { x:300, y:100 }, P2 = { x:550, y:300 };
    function bezierPoint(tt, A, B, C) {
      const mt = 1 - tt;
      return {
        x: mt * mt * A.x + 2 * mt * tt * B.x + tt * tt * C.x,
        y: mt * mt * A.y + 2 * mt * tt * B.y + tt * tt * C.y
      };
    }
    const line = document.getElementById("azPointerLine");
    if (!line) return;
    const end = bezierPoint(t, P0, P1, P2);
    line.setAttribute("x2", end.x);
    line.setAttribute("y2", end.y);
  }

  function updateElPointerLine() {
    let t;
    if (rotorELPosition >= 0) {
      t = (rotorELPosition / 90) * 0.5;
    } else {
      t = 0.5 + ((0 - rotorELPosition) / 180) * 0.5;
    }
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const P0 = { x:50, y:300 }, P1 = { x:300, y:100 }, P2 = { x:550, y:300 };
    function bezierPoint_el(tt, A, B, C) {
      const mt = 1 - tt;
      return {
        x: mt * mt * A.x + 2 * mt * tt * B.x + tt * tt * C.x,
        y: mt * mt * A.y + 2 * mt * tt * B.y + tt * tt * C.y
      };
    }
    const line = document.getElementById("elPointerLine");
    if (!line) return;
    const end = bezierPoint_el(t, P0, P1, P2);
    line.setAttribute("x2", end.x);
    line.setAttribute("y2", end.y);
  }

  // -----------------------------
  // Update Meter Ellipse Colors
  // -----------------------------
  function updateMeterEllipseColors() {
    const threshold = 0.1;
  
    // Analog Meters
    const analogAz = document.getElementById("azMeterEllipse");
    const analogEl = document.getElementById("elMeterEllipse");
  
    if (analogAz) {
      if (Math.abs(targetAZ - rotorAZPosition) > threshold) {
        analogAz.setAttribute("fill", "red");
      } else {
        analogAz.setAttribute("fill", "green");
      }
    }
    if (analogEl) {
      if (Math.abs(targetEL - rotorELPosition) > threshold) {
        analogEl.setAttribute("fill", "red");
      } else {
        analogEl.setAttribute("fill", "green");
      }
    }
  
    // Digital Meters
    const digitalAz = document.getElementById("azMeterEllipseDigital");
    const digitalEl = document.getElementById("elMeterEllipseDigital");
  
    if (digitalAz) {
      if (Math.abs(targetAZ - rotorAZPosition) > threshold) {
        digitalAz.setAttribute("fill", "red");
      } else {
        digitalAz.setAttribute("fill", "green");
      }
    }
    if (digitalEl) {
      if (Math.abs(targetEL - rotorELPosition) > threshold) {
        digitalEl.setAttribute("fill", "red");
      } else {
        digitalEl.setAttribute("fill", "green");
      }
    }
  }

  // -----------------------------
  // New: Convert Maidenhead GridSQ to Latitude/Longitude
  // -----------------------------
  function convertGridToLatLon(grid) {
    grid = grid.toUpperCase();
    if (grid.length < 6) return null;
    const A = grid.charCodeAt(0) - 65;
    const B = grid.charCodeAt(1) - 65;
    const C = parseInt(grid.charAt(2), 10);
    const D = parseInt(grid.charAt(3), 10);
    const E = grid.charCodeAt(4) - 65;
    const F = grid.charCodeAt(5) - 65;
    // Calculate the center of the 6-character grid square
    const lon = (A * 20) - 180 + (C * 2) + ((E + 0.5) * (5 / 60));
    const lat = (B * 10) - 90 + D + ((F + 0.5) * (2.5 / 60));
    return { lat: lat, lon: lon };
  }

  // -----------------------------
  // New: Calculate Magnetic Deviation (Rough Approximation)
  // -----------------------------
  function calculateMagneticDeviation(lat, lon, date) {
    // Rough approximation adjusted to yield ~9.92° for grid "FM08TO"
    return 0.1 * lat - 0.2 * lon - 9.6;
  }

  // -----------------------------
  // New: Update GridSQ Info Display & Magnetic Deviation
  // -----------------------------
  function updateGridInfo() {
    console.log("updateGridInfo called, gridSQ =", gridSQ);
    if (gridSQ === "" || gridSQ.length < 4) return;
    if (storedLat === null || storedLon === null || gridSQ !== lastGridSquare) {
      const result = convertGridToLatLon(gridSQ);
      if (result) {
        storedLat = result.lat;
        storedLon = result.lon;
        lastGridSquare = gridSQ;
      }
    }
    const gridInfoEl = document.getElementById("gridInfo");
    if (gridInfoEl && storedLat !== null && storedLon !== null) {
      gridInfoEl.textContent = "Grid: " + gridSQ + "    LAT: " + storedLat.toFixed(4) + "  /  LON: " + storedLon.toFixed(4);
      gridInfoEl.style.fontSize = "14px";
      gridInfoEl.style.fill = "white";
      gridInfoEl.style.fontWeight = "bold";
    }
    
    // Calculate magnetic deviation
    const currentDate = new Date();
    const magDeviation = calculateMagneticDeviation(storedLat, storedLon, currentDate);
    console.log("Magnetic Deviation:", magDeviation.toFixed(2), "degrees");
    const magDevEl = document.getElementById("magDev");
    if (magDevEl) {
      magDevEl.textContent = "Magnetic Deviation: " + magDeviation.toFixed(2) + "\u00b0";
      magDevEl.style.fontSize = "14px";
      magDevEl.style.fill = "blue";
      magDevEl.style.fontWeight = "bold";
    }
  }

  // -----------------------------
  // Animation Loop (for continuous updates)
  // -----------------------------
  function animate(timestamp) {
    if (!lastTimestamp) lastTimestamp = timestamp;
    const delta = timestamp - lastTimestamp;
  
    // Calculate the step for azimuth using AZ_SPEED (degrees per second)
    const azDifference = targetAZ - rotorAZPosition;
    const azStep = AZ_SPEED * (delta / 1000);
    if (Math.abs(azDifference) > azStep) {
      rotorAZPosition += (azDifference > 0 ? azStep : -azStep);
    } else {
      rotorAZPosition = targetAZ;
    }
  
    // Calculate the step for elevation using EL_SPEED (degrees per second)
    const elDifference = targetEL - rotorELPosition;
    const elStep = EL_SPEED * (delta / 1000);
    if (Math.abs(elDifference) > elStep) {
      rotorELPosition += (elDifference > 0 ? elStep : -elStep);
    } else {
      rotorELPosition = targetEL;
    }
  
    updateDisplay();
    lastTimestamp = timestamp;
    requestAnimationFrame(animate);
  }
});