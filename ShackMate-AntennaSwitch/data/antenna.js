// antenna.js

/*
 * ANTENNA DETAILS PERSISTENCE FEATURE
 * ===================================
 * Each antenna button now has persistent storage for its details:
 * - TYPE (Beam, Dipole, Loop, etc.)
 * - STYLE (Yagi, Log Periodic, Quad, etc.)
 * - POL (Horizontal, Vertical, etc.)
 * - MFG (Manufacturer)
 * - Band Pattern (LED selections)
 * - Disabled state
 * 
 * All changes are automatically saved to ESP32 device NVS storage and 
 * synchronized across all web clients. This means each antenna remembers 
 * its configuration across browser sessions, device reboots, and is shared
 * between all users accessing the web interface.
 * 
 * Storage is handled by the ESP32 device - no localStorage used.
 */

// Global WebSocket variable
let ws = null;

// Helper variables
let rcsType = 1; // default to RCS-10 (shows all 8 buttons)
let deviceNumber = 1; // default to 1

// Model/Device UI update functions removed - controls are only on config page

function updateAntennaButtonVisibility() {
  // Try to get rcsType from multiple sources
  var currentRcsType = rcsType; // Global variable
  
  // If global rcsType is not set, try to get from template
  if (typeof currentRcsType !== "number" || isNaN(currentRcsType)) {
    const rcsTypeInput = document.getElementById('rcsTypeValue');
    if (rcsTypeInput) {
      currentRcsType = parseInt(rcsTypeInput.value, 10);
      if (!isNaN(currentRcsType)) {
        rcsType = currentRcsType; // Update global variable
      }
    }
  }
  
  // Default to RCS-10 if still not valid
  if (typeof currentRcsType !== "number" || isNaN(currentRcsType)) {
    currentRcsType = 1; // Default to RCS-10
    rcsType = currentRcsType;
  }
  
  console.log("Updating antenna button visibility for rcsType:", currentRcsType);
  
  // For RCS-8 (value 0): hide buttons 6, 7, 8 (only show 1-5)
  // For RCS-10 (value 1): show all buttons 1-8
  for (let i = 6; i <= 8; i++) {
    let btn = document.getElementById('ant' + i);
    if (btn) {
      if (currentRcsType === 0) {
        // For SVG elements, use visibility and pointer-events
        btn.style.visibility = 'hidden';
        btn.style.pointerEvents = 'none';
        btn.style.display = 'none';
        // Also set opacity for extra hiding
        btn.style.opacity = '0';
        console.log("Hiding antenna button", i, "for RCS-8");
      } else {
        // Show the button for RCS-10
        btn.style.visibility = 'visible';
        btn.style.pointerEvents = 'auto';
        btn.style.display = '';
        btn.style.opacity = '1';
        console.log("Showing antenna button", i, "for RCS-10");
      }
    } else {
      console.warn("Could not find antenna button element: ant" + i);
    }
  }
  
  // Always show buttons 1-5 (available on both RCS-8 and RCS-10)
  for (let i = 1; i <= 5; i++) {
    let btn = document.getElementById('ant' + i);
    if (btn) {
      btn.style.visibility = 'visible';
      btn.style.pointerEvents = 'auto';
      btn.style.display = '';
      btn.style.opacity = '1';
    }
  }
}

// Helper function to manually test antenna button visibility
function testAntennaVisibility() {
  console.log("=== ANTENNA VISIBILITY TEST ===");
  console.log("Current rcsType:", rcsType);
  console.log("Testing antenna button visibility...");
  
  // Check if elements exist
  for (let i = 1; i <= 8; i++) {
    let btn = document.getElementById('ant' + i);
    if (btn) {
      console.log(`Button ant${i} found:`, btn);
      console.log(`  - visibility: ${btn.style.visibility}`);
      console.log(`  - display: ${btn.style.display}`);
      console.log(`  - opacity: ${btn.style.opacity}`);
      console.log(`  - pointerEvents: ${btn.style.pointerEvents}`);
    } else {
      console.log(`Button ant${i} NOT found`);
    }
  }
  
  updateAntennaButtonVisibility();
  console.log("=== END TEST ===");
}

// Function to manually set RCS type and update visibility
function setRcsType(type) {
  console.log("Manually setting rcsType to:", type);
  rcsType = type;
  updateAntennaButtonVisibility();
}

// Flag to track if we've received fresh state from server
let hasReceivedFreshState = false;
let webSocketConnected = false;

document.addEventListener("DOMContentLoaded", function() {
  console.log("=== DOM CONTENT LOADED ===");
  
  // Reset flags for new page load
  hasReceivedFreshState = false;
  webSocketConnected = false;
  
  // Antenna details will be loaded from ESP device via WebSocket
  
  // --- Initialize rcsType from template if available ---
  const rcsTypeInput = document.getElementById('rcsTypeValue');
  console.log("Looking for rcsTypeValue element:", rcsTypeInput);
  if (rcsTypeInput) {
    console.log("Found rcsTypeValue element with value:", rcsTypeInput.value);
    const templateRcsType = parseInt(rcsTypeInput.value, 10);
    console.log("Parsed templateRcsType:", templateRcsType, "isNaN:", isNaN(templateRcsType));
    if (!isNaN(templateRcsType)) {
      rcsType = templateRcsType;
      console.log("Set global rcsType from template:", rcsType);
    }
  } else {
    console.warn("Could not find rcsTypeValue element in DOM");
  }
  
  console.log("Final rcsType before initial visibility update:", rcsType);
  
  // Apply initial button visibility immediately
  updateAntennaButtonVisibility();
  
  // Add multiple backup calls with increasing delays to ensure visibility gets applied
  setTimeout(function() {
    console.log("=== BACKUP VISIBILITY UPDATE (200ms delay) ===");
    updateAntennaButtonVisibility();
  }, 200);
  
  setTimeout(function() {
    console.log("=== BACKUP VISIBILITY UPDATE (500ms delay) ===");
    console.log("Current rcsType:", rcsType, "hasReceivedFreshState:", hasReceivedFreshState);
    updateAntennaButtonVisibility();
  }, 500);
  
  setTimeout(function() {
    console.log("=== BACKUP VISIBILITY UPDATE (1000ms delay) ===");
    console.log("Current rcsType:", rcsType, "hasReceivedFreshState:", hasReceivedFreshState);
    updateAntennaButtonVisibility();
  }, 1000);
  
  // Add another backup call after WebSocket potentially connects
  setTimeout(function() {
    console.log("=== BACKUP VISIBILITY UPDATE (2000ms delay) ===");
    console.log("Current rcsType:", rcsType, "hasReceivedFreshState:", hasReceivedFreshState);
    updateAntennaButtonVisibility();
  }, 2000);
  
  // Add final backup call for slow connections
  setTimeout(function() {
    console.log("=== FINAL BACKUP VISIBILITY UPDATE (4000ms delay) ===");
    console.log("Current rcsType:", rcsType, "hasReceivedFreshState:", hasReceivedFreshState);
    updateAntennaButtonVisibility();
  }, 4000);
  
  // --- Hide Antenna buttons 6-8 if rcsType == 0 (RCS-8) ---
  // Initial visibility of antenna buttons 6-8 will be handled by updateAntennaButtonVisibility later.
  // -------------------------------------------------------------------------
  // New state variables for model selection and device number
  // modelValue: 0 = RCS-8, 1 = RCS-10
  // deviceNumber: 1-4
  // -------------------------------------------------------------------------
// let modelValue = 0; // replaced by rcsType above

// Auto-save config helper
function autoSaveConfig(key, value) {
  var xhr = new XMLHttpRequest();
  xhr.open("POST", "/saveConfig?action=autosave", true);
  xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded");
  xhr.send(key + "=" + encodeURIComponent(value));
}
  // Set up WebSocket connection using the /ws endpoint on port 4000.
  const wsUrl = `ws://${window.location.hostname}:4000/ws`;
  ws = new WebSocket(wsUrl);

  ws.onopen = function() {
    console.log("Connected to WebSocket server on port 4000.");
    webSocketConnected = true;
    
    // Update visibility immediately when WebSocket connects
    updateAntennaButtonVisibility();
    
    // Request fresh state from server - this is critical for getting current rcsType
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: "requestState" }));
      console.log("Sent state request to get fresh rcsType from server.");
      
      // Add backup state requests in case the first one gets lost
      setTimeout(function() {
        if (ws.readyState === WebSocket.OPEN && !hasReceivedFreshState) {
          console.log("Sending backup state request (1s delay)");
          ws.send(JSON.stringify({ type: "requestState" }));
        }
      }, 1000);
      
      setTimeout(function() {
        if (ws.readyState === WebSocket.OPEN && !hasReceivedFreshState) {
          console.log("Sending backup state request (3s delay)");
          ws.send(JSON.stringify({ type: "requestState" }));
        }
      }, 3000);
    }
  };

  ws.onmessage = function(event) {
    if (event.data.trim().startsWith("{")) {
      try {
        const data = JSON.parse(event.data);
        if (data.type === "stateUpdate") {
          hasReceivedFreshState = true; // Mark that we've received state from server
          console.log("=== RECEIVED STATE UPDATE ===");
          console.log("Received data:", data);
          console.log("Current local state - index:", currentAntennaIndex, "rcsType:", rcsType);
          
          if (data.antennaNames) {
            antennaNames = data.antennaNames.slice(0, 8);
            console.log("Updated antenna names:", antennaNames);
          }
          if (typeof data.rcsType === "number") {
            const oldRcsType = rcsType;
            rcsType = data.rcsType;
            console.log("Received rcsType from server:", rcsType, "(was:", oldRcsType, ")");
            // Model UI update removed - controls are only on config page
            // Force visibility update whenever we get rcsType from server
            updateAntennaButtonVisibility();
            
            // Add a delayed update to ensure it sticks
            setTimeout(function() {
              console.log("Delayed visibility update after receiving rcsType from server");
              updateAntennaButtonVisibility();
            }, 100);
          }
          if (typeof data.deviceNumber === "number") {
            deviceNumber = data.deviceNumber;
            // Device UI update removed - controls are only on config page
          }
          
          // Always update antenna state and selection, even if state appears equal
          // This ensures we catch CI-V induced changes
          const stateChanged = !isStateEqual(data);
          const antennaIndexChanged = data.currentAntennaIndex !== currentAntennaIndex;
          
          console.log("State comparison - changed:", stateChanged, "antenna index changed:", antennaIndexChanged);
          console.log("New antenna index:", data.currentAntennaIndex, "old:", currentAntennaIndex);
          
          if (stateChanged || antennaIndexChanged || !hasReceivedFreshState) {
            antennaState = data.antennaState;
            currentAntennaIndex = data.currentAntennaIndex;
            
            console.log("Updating antenna selection UI...");
            
            // Update all antenna button states
            document.querySelectorAll('.antenna-button').forEach((b, idx) => {
              b.classList.remove('selected');
              if (antennaState[idx] && antennaState[idx].disabled) {
                b.classList.add('disabled');
              } else {
                b.classList.remove('disabled');
              }
            });
            
            // Set the selected antenna
            const selButton = document.querySelector(`.antenna-button[data-index="${currentAntennaIndex}"]`);
            if (selButton && antennaState[currentAntennaIndex] && !antennaState[currentAntennaIndex].disabled) {
              selButton.classList.add('selected');
              console.log("Set antenna button", currentAntennaIndex, "as selected");
            } else {
              console.log("Could not select antenna button", currentAntennaIndex, "- button found:", !!selButton);
            }
            
            refreshUIForAntenna(currentAntennaIndex);
            updateOptionButtons();
            updateAntennaButtonVisibility();
            console.log("=== STATE UPDATE COMPLETE ===");
            console.log("Final state - index:", currentAntennaIndex, "rcsType:", rcsType);
          } else {
            console.log("State unchanged, skipping UI update");
          }
        } else {
          console.log("Received message:", data);
        }
      } catch (err) {
        console.error("Error parsing WebSocket message:", err);
      }
    } else {
      console.log("Non-JSON message received:", event.data);
    }
  };

  ws.onerror = function(error) {
    console.error("WebSocket error:", error);
  };

  ws.onclose = function(event) {
    console.log("WebSocket connection closed:", event.code, event.reason);
    webSocketConnected = false;
    hasReceivedFreshState = false;
  };

  /*----------------------------------------------------------------
    1) State Variables: 10 antenna objects, current antenna index,
       and antenna names.
  ----------------------------------------------------------------*/
  let antennaState = new Array(10).fill(null).map(() => ({
    bandPattern: 0,
    typeIndex: 0,
    styleIndex: 0,
    polIndex: 0,
    mfgIndex: 0,
    disabled: false
  }));
  let currentAntennaIndex = 0;

  // Default 8 names: "Antenna #1" ... "Antenna #8"
  let antennaNames = [
    "Antenna #1",
    "Antenna #2",
    "Antenna #3",
    "Antenna #4",
    "Antenna #5",
    "Antenna #6",
    "Antenna #7",
    "Antenna #8"
  ];

  let typeDropdownOpen = false;

  // Debounce timer for state update broadcasts
  let stateUpdateTimeout = null;

  function sendStateUpdate() {
    // Clear any pending update
    if (stateUpdateTimeout !== null) {
      clearTimeout(stateUpdateTimeout);
    }
    // Set a new timeout (200ms delay)
    stateUpdateTimeout = setTimeout(() => {
      // ---------------------------------------------------------------------
      // New: include rcsType and deviceNumber in state
      // ---------------------------------------------------------------------
      const state = {
        type: "stateUpdate",
        antennaState: antennaState,
        currentAntennaIndex: currentAntennaIndex,
        antennaNames: antennaNames,
        rcsType: rcsType,
        deviceNumber: deviceNumber
      };
      if (ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify(state));
        console.log("State update sent:", state);
      } else {
        console.warn("WebSocket not open (readyState:", ws.readyState, "); state update not sent.");
      }
      stateUpdateTimeout = null;
    }, 200);
  }

  /*----------------------------------------------------------------
    2) Helper: Compare received state to current local state.
  ----------------------------------------------------------------*/
  function isStateEqual(received) {
    return (
      JSON.stringify(received.antennaState) === JSON.stringify(antennaState) &&
      received.currentAntennaIndex === currentAntennaIndex &&
      received.rcsType === rcsType &&
      received.deviceNumber === deviceNumber
    );
  }

  /*----------------------------------------------------------------
    3) UI Helper Functions for LED, Tuner, and selected antenna label.
  ----------------------------------------------------------------*/
  function updateLEDsFromConfig(index) {
    const pattern = antennaState[index].bandPattern;
    document.querySelectorAll('.led').forEach((led, i) => {
      if (pattern & (1 << i)) {
        led.classList.remove('off');
        led.classList.add('on');
      } else {
        led.classList.remove('on');
        led.classList.add('off');
      }
    });
  }

  function updateTunerButton(index) {
    const tunerText = document.getElementById('tunerText');
    if (index === null) {
      tunerText.style.fill = '#fff';
      return;
    }
    const pattern = antennaState[index].bandPattern;
    tunerText.style.fill = (pattern & (1 << 14)) ? 'red' : '#fff';
  }

  function updateSelectedAntennaName(index) {
    const nameElem = document.getElementById("selectedAntennaName");
    const portNum = (index !== null) ? (index + 1) : "-";
    if (index === null) {
      nameElem.textContent = `Port ${portNum}: No Antenna Selected`;
    } else {
      // Use the updated antennaNames array
      nameElem.textContent = `Port ${portNum}: ${antennaNames[index]}`;
    }
  }

  function refreshUIForAntenna(index) {
    updateLEDsFromConfig(index);
    updateTunerButton(index);
    updateSelectedAntennaName(index);
    updateOptionButtons(); // Update antenna details display for selected antenna
    console.log(`Antenna ${index} selected: ${antennaNames[index]}`);
    sendStateUpdate();
  }

  /*----------------------------------------------------------------
    4) Data Arrays for TYPE/STYLE/POL/MFG and helper functions.
  ----------------------------------------------------------------*/
  function stripNumbering(str) {
    const dashIndex = str.indexOf("-");
    if (dashIndex >= 0) {
      return str.substring(dashIndex + 1).trim();
    }
    return str.trim();
  }

  function stripPolarizationNumbering(str) {
    const spaceIndex = str.indexOf(" ");
    if (spaceIndex >= 0) {
      return str.substring(spaceIndex + 1).trim();
    }
    return str.trim();
  }

  const antennaTypes = [
    "0 - None",
    "1 - Wire",
    "2 - Directional",
    "3 - Satellite"
  ];
  const antennaStyles = {
    0: ["0.0 - None"],
    1: ["0.0 - None", "1.1 - Dipoles", "1.2 - End Fed", "1.3 - Random Wire", "1.4 - Quarter-Wave", "1.5 - Loop", "1.6 - Magnetic Loop"],
    2: ["0.0 - None", "2.1 - Beam", "2.2 - Yagi", "2.3 - Log-Periodic", "2.4 - Dish", "2.5 - Omni"],
    3: ["0.0 - None", "3.1 - Beam", "3.2 - Yagi", "3.3 - Helical"]
  };

  const antennaPols = {
    0: { 0: ["0.0-0.0 None"] },
    1: {
      0: ["0.0-0.0 None"],
      1: ["1.1-1.1 Vertical", "1.1-1.2 Horizontal"],
      2: ["1.1-2.1 Vertical", "1.1-2.2 Horizontal"],
      3: ["1.1-3.1 Vertical", "1.1-3.2 Horizontal"],
      4: ["1.1-4.1 Vertical"],
      5: ["1.1-5.1 Horizontal"],
      6: ["1.1-6.1 Horizontal"]
    },
    2: {
      0: ["0.0-0.0 None"],
      1: ["0.0-0.0 None", "2.1-1.1 Vertical", "2.1-1.2 Horizontal"],
      2: ["2.2-1.1 Vertical", "2.2-1.2 Horizontal"],
      3: ["2.3-1.1 Vertical", "2.3-1.2 Horizontal"],
      4: ["2.4-1.1 Circular", "2.4-1.2 Linear"],
      5: ["2.5-1.1 Vertical"]
    },
    3: {
      0: ["0.0-0.0 None"],
      1: ["3.1-1.1 Vertical", "3.1-1.2 Horizontal"],
      2: ["3.2-1.1 Vertical", "3.2-1.2 Horizontal"],
      3: ["3.3-1.1 Vertical", "3.3-1.2 Horizontal", "3.3-1.3 Circular"]
    }
  };
  const antennaMfg = [
    "0 - None",
    "Chameleon",
    "Comet",
    "Cushcraft",
    "Diamond",
    "GreyLine",
    "HomeBrew",
    "Hustler",
    "M2",
    "MFJ",
    "SteppIR",
    "SolarCON"
  ];

  /*----------------------------------------------------------------
    5) updateOptionButtons: updates UI and then sends a debounced state update.
  ----------------------------------------------------------------*/
  function updateOptionButtons() {
    const st = antennaState[currentAntennaIndex];
    let typeStr = stripNumbering(antennaTypes[st.typeIndex]);
    let styleStr = "None";
    let polStr = "None";
    let mfgStr = stripNumbering(antennaMfg[st.mfgIndex]);

    if (st.typeIndex !== 0) {
      let styleArray = antennaStyles[st.typeIndex];
      if (styleArray && st.styleIndex < styleArray.length) {
        styleStr = stripNumbering(styleArray[st.styleIndex]);
      }
      if (st.styleIndex === 0) styleStr = "None";
    } else {
      typeStr = "None";
    }

    if (st.typeIndex !== 0 && st.styleIndex !== 0) {
      let polArray = antennaPols[st.typeIndex][st.styleIndex];
      if (polArray && st.polIndex < polArray.length) {
        polStr = stripPolarizationNumbering(polArray[st.polIndex]);
      }
    }

    document.getElementById("antennaTypeButton").textContent = `TYPE: ${typeStr}`;
    document.getElementById("antennaStyleButton").textContent = `STYLE: ${styleStr}`;
    document.getElementById("antennaPolButton").textContent = `POL: ${polStr}`;
    document.getElementById("antennaMfgButton").textContent = `MFG: ${mfgStr}`;

    // Antenna details are automatically saved to ESP device via sendStateUpdate()
    sendStateUpdate();
  }

  /*----------------------------------------------------------------
    6) positionDropdownBelowButton: positions dropdown menus.
  ----------------------------------------------------------------*/
  function positionDropdownBelowButton(dropdown, button) {
    const container = document.getElementById("antenna-details");
    const containerRect = container.getBoundingClientRect();
    const buttonRect = button.getBoundingClientRect();
    const offsetTop = (buttonRect.bottom - containerRect.top);
    const offsetLeft = (buttonRect.left - containerRect.left) - 7;
    dropdown.style.position = "absolute";
    dropdown.style.top = offsetTop + "px";
    dropdown.style.left = offsetLeft + "px";
    dropdown.style.display = "block";
  }

  /*----------------------------------------------------------------
    7) Dropdown show/hide for TYPE/STYLE/POL/MFG.
  ----------------------------------------------------------------*/
  function showTypeDropdown() {
    console.log("showTypeDropdown() called...");
    const button = document.getElementById("antennaTypeButton");
    const dropdown = document.getElementById("typeDropdown");
    dropdown.innerHTML = "";
    antennaTypes.forEach((typeStr, index) => {
      const displayText = stripNumbering(typeStr);
      const btn = document.createElement("button");
      btn.textContent = displayText;
      btn.addEventListener("click", () => {
        let st = antennaState[currentAntennaIndex];
        st.typeIndex = index;
        st.styleIndex = 0;
        st.polIndex = 0;
        if (st.typeIndex === 0) st.mfgIndex = 0;
        updateOptionButtons();
        dropdown.style.display = "none";
      });
      dropdown.appendChild(btn);
    });
    positionDropdownBelowButton(dropdown, button);
  }

  function showStyleDropdown() {
    console.log("showStyleDropdown() called...");
    const button = document.getElementById("antennaStyleButton");
    const dropdown = document.getElementById("styleDropdown");
    dropdown.innerHTML = "";
    let st = antennaState[currentAntennaIndex];
    if (st.typeIndex === 0) return;
    const styleArray = antennaStyles[st.typeIndex];
    styleArray.forEach((styleStr, index) => {
      const displayText = stripNumbering(styleStr);
      const btn = document.createElement("button");
      btn.textContent = displayText;
      btn.addEventListener("click", () => {
        st.styleIndex = index;
        st.polIndex = 0;
        updateOptionButtons();
        dropdown.style.display = "none";
      });
      dropdown.appendChild(btn);
    });
    positionDropdownBelowButton(dropdown, button);
  }

  function showPolDropdown() {
    console.log("showPolDropdown() called...");
    const button = document.getElementById("antennaPolButton");
    const dropdown = document.getElementById("polDropdown");
    dropdown.innerHTML = "";
    let st = antennaState[currentAntennaIndex];
    if (st.typeIndex === 0 || st.styleIndex === 0) return;
    const polArray = antennaPols[st.typeIndex][st.styleIndex];
    if (!polArray) return;
    polArray.forEach((polStr, index) => {
      const displayText = stripPolarizationNumbering(polStr);
      const btn = document.createElement("button");
      btn.textContent = displayText;
      btn.addEventListener("click", () => {
        st.polIndex = index;
        updateOptionButtons();
        dropdown.style.display = "none";
      });
      dropdown.appendChild(btn);
    });
    positionDropdownBelowButton(dropdown, button);
  }

  function showMfgDropdown() {
    console.log("showMfgDropdown() called...");
    let st = antennaState[currentAntennaIndex];
    if (st.typeIndex === 0) {
      console.log("TYPE is None => MFG popup canceled.");
      return;
    }
    const button = document.getElementById("antennaMfgButton");
    const dropdown = document.getElementById("mfgDropdown");
    dropdown.innerHTML = "";
    antennaMfg.forEach((mfgStr, index) => {
      const displayText = stripNumbering(mfgStr);
      const btn = document.createElement("button");
      btn.textContent = displayText;
      btn.addEventListener("click", () => {
        st.mfgIndex = index;
        updateOptionButtons();
        dropdown.style.display = "none";
      });
      dropdown.appendChild(btn);
    });
    positionDropdownBelowButton(dropdown, button);
  }

  /*----------------------------------------------------------------
    8) Attach event listeners for interactive elements.
  ----------------------------------------------------------------*/
  window.addEventListener('DOMContentLoaded', () => {
    // Model/Device controls removed from switch page - they belong only on config page

    // Model/Device event listeners removed - controls are only on config page
    function addLongPress(button, longPressFn, shortPressFn) {
      let holdTimeout = null;
      let holdFired = false;
      button.addEventListener("pointerdown", () => {
        holdFired = false;
        holdTimeout = setTimeout(() => {
          holdFired = true;
          longPressFn();
        }, 750);
      });
      button.addEventListener("pointerup", () => {
        if (holdTimeout) {
          clearTimeout(holdTimeout);
          holdTimeout = null;
          if (!holdFired) {
            shortPressFn();
          }
        }
      });
    }

    // TYPE button
    const typeButton = document.getElementById("antennaTypeButton");
    addLongPress(typeButton,
      () => { showTypeDropdown(); },
      () => {
        let st = antennaState[currentAntennaIndex];
        st.typeIndex = (st.typeIndex + 1) % antennaTypes.length;
        st.styleIndex = 0;
        st.polIndex = 0;
        if (st.typeIndex === 0) st.mfgIndex = 0;
        updateOptionButtons();
      }
    );

    // STYLE button
    const styleButton = document.getElementById("antennaStyleButton");
    addLongPress(styleButton,
      () => { showStyleDropdown(); },
      () => {
        let st = antennaState[currentAntennaIndex];
        if (st.typeIndex === 0) return;
        let styleArray = antennaStyles[st.typeIndex];
        st.styleIndex = (st.styleIndex + 1) % styleArray.length;
        st.polIndex = 0;
        updateOptionButtons();
      }
    );

    // POL button
    const polButton = document.getElementById("antennaPolButton");
    addLongPress(polButton,
      () => { showPolDropdown(); },
      () => {
        let st = antennaState[currentAntennaIndex];
        if (st.typeIndex === 0 || st.styleIndex === 0) return;
        let polArray = antennaPols[st.typeIndex][st.styleIndex];
        if (!polArray) return;
        st.polIndex = (st.polIndex + 1) % polArray.length;
        updateOptionButtons();
      }
    );

    // MFG button
    const mfgButton = document.getElementById("antennaMfgButton");
    addLongPress(mfgButton,
      () => { showMfgDropdown(); },
      () => {
        let st = antennaState[currentAntennaIndex];
        if (st.typeIndex === 0) {
          console.log("MFG short press canceled because TYPE is None.");
          return;
        }
        st.mfgIndex = (st.mfgIndex + 1) % antennaMfg.length;
        updateOptionButtons();
      }
    );

    // Hide dropdowns when clicking outside
    document.addEventListener("click", (e) => {
      const typeDropdown = document.getElementById("typeDropdown");
      const styleDropdown = document.getElementById("styleDropdown");
      const polDropdown = document.getElementById("polDropdown");
      const mfgDropdown = document.getElementById("mfgDropdown");

      if (typeDropdown && !typeDropdown.contains(e.target) && e.target !== typeButton) {
        typeDropdown.style.display = "none";
      }
      if (styleDropdown && !styleDropdown.contains(e.target) && e.target !== styleButton) {
        styleDropdown.style.display = "none";
      }
      if (polDropdown && !polDropdown.contains(e.target) && e.target !== polButton) {
        polDropdown.style.display = "none";
      }
      if (mfgDropdown && !mfgDropdown.contains(e.target) && e.target !== mfgButton) {
        mfgDropdown.style.display = "none";
      }
    });

    // Antenna selection buttons
    document.querySelectorAll('.antenna-button').forEach((button, idx) => {
      let holdTimeout = null;
      let holdFired = false;
      button.addEventListener('mousedown', () => {
        holdFired = false;
        holdTimeout = setTimeout(() => {
          // Toggle disabled state on long press
          button.classList.toggle('disabled');
          if (button.classList.contains('disabled')) {
            button.classList.remove('selected');
            antennaState[idx].disabled = true;
          } else {
            antennaState[idx].disabled = false;
          }
          holdFired = true;
          sendStateUpdate(); // Antenna details auto-saved to ESP device
        }, 500);
      });
      button.addEventListener('mouseleave', () => {
        if (holdTimeout) {
          clearTimeout(holdTimeout);
          holdTimeout = null;
        }
      });
      button.addEventListener('mouseup', () => {
        if (holdTimeout) {
          clearTimeout(holdTimeout);
          holdTimeout = null;
          if (!button.classList.contains('disabled') && !holdFired) {
            document.querySelectorAll('.antenna-button').forEach(b => b.classList.remove('selected'));
            button.classList.add('selected');
            currentAntennaIndex = parseInt(button.dataset.index, 10);
            refreshUIForAntenna(currentAntennaIndex);
            updateOptionButtons();
            sendStateUpdate(); // Send the antenna change to the server
          }
        }
      });
    });

    // LED circles
    document.querySelectorAll('.led').forEach(led => {
      led.addEventListener('click', (e) => {
        e.stopPropagation();
        if (currentAntennaIndex === null) return;
        let st = antennaState[currentAntennaIndex];
        const ledIndex = parseInt(led.getAttribute('data-index'), 10);
        st.bandPattern ^= (1 << ledIndex);
        updateLEDsFromConfig(currentAntennaIndex);
        console.log(`Antenna ${currentAntennaIndex} LED pattern: ${st.bandPattern.toString(2).padStart(16, '0')}`);
        sendStateUpdate(); // Antenna details auto-saved to ESP device
      });
    });

    // Tuner toggle
    function toggleTuner() {
      if (currentAntennaIndex === null) return;
      const antennaBtn = document.querySelector(`.antenna-button[data-index="${currentAntennaIndex}"]`);
      if (!antennaBtn || antennaBtn.classList.contains('disabled')) return;
      let st = antennaState[currentAntennaIndex];
      st.bandPattern ^= (1 << 14);
      updateTunerButton(currentAntennaIndex);
      console.log(`Tuner toggled for antenna ${currentAntennaIndex}, pattern: ${st.bandPattern.toString(2).padStart(16, '0')}`);
      sendStateUpdate(); // Antenna details auto-saved to ESP device
    }
    document.getElementById('btnTuner').addEventListener('click', (e) => {
      e.stopPropagation();
      toggleTuner();
    });

    // On initial load, request state from the server.
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: "requestState" }));
      console.log("Initial state request sent.");
    }

    // Set initial visibility of antenna buttons 6-8
    updateAntennaButtonVisibility();
  });
});

// Add page visibility event handlers to refresh UI when switching browser tabs
document.addEventListener('visibilitychange', function() {
  if (!document.hidden) {
    console.log("=== PAGE BECAME VISIBLE ===");
    console.log("Current antenna state - index:", currentAntennaIndex, "rcsType:", rcsType);
    hasReceivedFreshState = false; // Reset flag to request fresh state
    setTimeout(updateAntennaButtonVisibility, 100); // Small delay to ensure DOM is ready
    
    // Request fresh state from server if WebSocket is connected
    if (webSocketConnected && ws && ws.readyState === WebSocket.OPEN) {
      console.log("Requesting fresh state from server after page became visible");
      ws.send(JSON.stringify({ type: "requestState" }));
      
      // Add backup request after a short delay
      setTimeout(function() {
        if (ws && ws.readyState === WebSocket.OPEN) {
          console.log("Backup state request after page visibility change");
          ws.send(JSON.stringify({ type: "requestState" }));
        }
      }, 500);
    } else {
      console.warn("WebSocket not available for state request on visibility change");
    }
  }
});

// Add window focus event handler as backup
window.addEventListener('focus', function() {
  console.log("=== WINDOW FOCUSED ===");
  console.log("Current antenna state - index:", currentAntennaIndex, "rcsType:", rcsType);
  hasReceivedFreshState = false; // Reset flag to request fresh state
  setTimeout(updateAntennaButtonVisibility, 100);
  
  // Request fresh state from server if WebSocket is connected
  if (webSocketConnected && ws && ws.readyState === WebSocket.OPEN) {
    console.log("Requesting fresh state from server after window focus");
    ws.send(JSON.stringify({ type: "requestState" }));
    
    // Add backup request
    setTimeout(function() {
      if (ws && ws.readyState === WebSocket.OPEN) {
        console.log("Backup state request after window focus");
        ws.send(JSON.stringify({ type: "requestState" }));
      }
    }, 500);
  } else {
    console.warn("WebSocket not available for state request on focus");
  }
});

// Add page show event handler for back/forward navigation
window.addEventListener('pageshow', function(event) {
  console.log("=== PAGE SHOW EVENT ===");
  console.log("Current antenna state - index:", currentAntennaIndex, "rcsType:", rcsType);
  console.log("Event persisted:", event.persisted);
  hasReceivedFreshState = false; // Reset flag to request fresh state
  setTimeout(updateAntennaButtonVisibility, 100);
  
  // Request fresh state from server if WebSocket is connected
  if (webSocketConnected && ws && ws.readyState === WebSocket.OPEN) {
    console.log("Requesting fresh state from server after page show");
    ws.send(JSON.stringify({ type: "requestState" }));
    
    // Add backup request
    setTimeout(function() {
      if (ws && ws.readyState === WebSocket.OPEN) {
        console.log("Backup state request after page show");
        ws.send(JSON.stringify({ type: "requestState" }));
      }
    }, 500);
  } else {
    console.warn("WebSocket not available for state request on page show");
  }
});

// Add hashchange event handler in case there's client-side navigation
window.addEventListener('hashchange', function() {
  console.log("=== HASH CHANGE DETECTED ===");
  console.log("Current antenna state - index:", currentAntennaIndex, "rcsType:", rcsType);
  hasReceivedFreshState = false; // Reset flag to request fresh state
  setTimeout(updateAntennaButtonVisibility, 100);
  
  // Request fresh state from server if WebSocket is connected
  if (webSocketConnected && ws && ws.readyState === WebSocket.OPEN) {
    console.log("Requesting fresh state from server after hash change");
    ws.send(JSON.stringify({ type: "requestState" }));
  } else {
    console.warn("WebSocket not available for state request on hash change");
  }
});

// Add beforeunload event to clean up
window.addEventListener('beforeunload', function() {
  console.log("=== PAGE UNLOADING ===");
  hasReceivedFreshState = false;
  webSocketConnected = false;
});

// Function to force refresh state from server
function forceRefreshState() {
  console.log("=== FORCE REFRESH STATE ===");
  hasReceivedFreshState = false;
  if (webSocketConnected && ws && ws.readyState === WebSocket.OPEN) {
    console.log("Sending force refresh state request");
    ws.send(JSON.stringify({ type: "requestState" }));
    return true;
  } else {
    console.warn("Cannot force refresh - WebSocket not connected");
    return false;
  }
}

// Make forceRefreshState globally available for debugging
window.forceRefreshState = forceRefreshState;

// Add a periodic check to ensure buttons stay properly hidden/shown and state is fresh
setInterval(function() {
  if (typeof rcsType === "number" && !isNaN(rcsType)) {
    updateAntennaButtonVisibility();
    
    // If we haven't received fresh state recently and WebSocket is connected, request it
    if (!hasReceivedFreshState && webSocketConnected && ws && ws.readyState === WebSocket.OPEN) {
      console.log("Periodic check: requesting fresh state from server");
      ws.send(JSON.stringify({ type: "requestState" }));
    }
  }
}, 3000); // Check every 3 seconds (increased from 2s for less aggressive polling)

// Add immediate check after a short delay for initial page load
setTimeout(function() {
  console.log("Initial delayed visibility check");
  updateAntennaButtonVisibility();
  
  // Also request fresh state if WebSocket is available
  if (webSocketConnected && ws && ws.readyState === WebSocket.OPEN) {
    console.log("Initial state request after page load");
    ws.send(JSON.stringify({ type: "requestState" }));
  }
}, 500);

// Add keyboard shortcut for manual state refresh (F5 or Ctrl+R)
document.addEventListener('keydown', function(event) {
  // F5 key or Ctrl+R for manual refresh
  if (event.key === 'F5' || (event.ctrlKey && event.key === 'r')) {
    event.preventDefault(); // Prevent default page refresh
    console.log("=== MANUAL REFRESH TRIGGERED ===");
    forceRefreshState();
    updateAntennaButtonVisibility();
  }
  // Also add 'R' key for quick refresh (for debugging)
  else if (event.key === 'R' && !event.ctrlKey && !event.altKey && !event.shiftKey) {
    console.log("=== QUICK REFRESH (R key) ===");
    forceRefreshState();
    updateAntennaButtonVisibility();
  }
});

// Make debugging functions globally available
window.testAntennaVisibility = testAntennaVisibility;
window.setRcsType = setRcsType;
window.updateAntennaButtonVisibility = updateAntennaButtonVisibility;

/*----------------------------------------------------------------
  Server-Based Persistence Functions
----------------------------------------------------------------*/
// All antenna details are now stored on the ESP32 device and synchronized
// via WebSocket. No localStorage is used - everything is server-side.
// Changes are automatically saved when sendStateUpdate() is called.

/*----------------------------------------------------------------
  Global Utility Functions (accessible from browser console)
----------------------------------------------------------------*/
// Function to reset all antenna details (useful for debugging)
window.resetAllAntennaDetails = function() {
  console.log("Resetting all antenna details to defaults...");
  antennaState.forEach(antenna => {
    antenna.typeIndex = 0;
    antenna.styleIndex = 0;
    antenna.polIndex = 0;
    antenna.mfgIndex = 0;
    antenna.disabled = false;
    antenna.bandPattern = 0;
  });
  updateOptionButtons();
  refreshUIForAntenna(currentAntennaIndex);
  sendStateUpdate(); // Save to ESP device
  console.log("All antenna details reset to defaults and saved to ESP device.");
};

// Function to show current antenna state (useful for debugging)
window.showCurrentAntennaState = function() {
  console.log("Current antenna state:", antennaState);
  return antennaState;
};

// Function to manually trigger save to ESP device (useful for debugging)
window.saveToESP = function() {
  sendStateUpdate();
  console.log("Current antenna state sent to ESP device");
};