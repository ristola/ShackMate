//Web Socket
let ws;

// Global lists and indexes
const rig_modeList = ["00", "01", "02", "03", "04", "05", "07", "08", "17", "22"];
const rig_filterList = ["01", "02", "03"];
let rig_bandStackingList7300 = [["1.800.000","1.800.000","1.800.000"]
                               ,["3.500.000","3.500.000","3.500.000"]
                               ,["7.000.000","7.000.000","7.000.000"]
                               ,["10.000.000","10.000.000","10.000.000"]
                               ,["14.000.000","14.000.000","14.000.000"]
                               ,["18.068.000","18.068.000","18.068.000"]
                               ,["21.000.000","21.000.000","21.000.000"]
                               ,["24.890.000","24.890.000","24.890.000"]
                               ,["28.000.000","28.000.000","28.000.000"]
                               ,["53.000.000","53.000.000","53.000.000"]
                               ];
let rig_bandStackingList9700 = [["144.000.000","144.000.000","144.000.000"]
                               ,["430.000.000","430.000.000","430.000.000"]
                               ,["1240.000.000","1240.000.000","1240.000.000"]
                               ];
let rig_currentModeIndex = 0,
    rig_currentFilterIndex = 0,
    rig_currentSplitValue = "00";
let rig_subBandVisible = false, subBandHideTimer = null;
let rig_dataMode = 0;
let rig_activeRig = "";
let rig_activeArrRadioIndex = 0;


// Global operate mode variables
let rig_operateMode = "VFO"; // "VFO" or "MEM"
let rig_memoryCH = "0001";   // default memory channel
let rig_vfoToggle = false;   // false => side A, true => side B
let rig_toneValue = "00";    // "00" => no TONE, "11" => DUP-, "12" => DUP+

window.addEventListener("load", () => { /* Set memLabels */
  const s = localStorage.getItem("memLabels");
  if (s) {
    memLabels = JSON.parse(s);
    for (let i = 0; i < 5; i++) {
      const btn = document.getElementById("memBtn" + i);
      if (btn) btn.textContent = memLabels[i];
    }
  }
});

window.addEventListener('DOMContentLoaded', () => { /* Meters SVG */
  // ---------------------------
  // 1) S/PO Meter (range 0–100) with dual scales
  // ---------------------------
  const spoGroup = document.getElementById('rig_spoMeterGroup');
  const spoConfig = {
    maxSegments: 101,
    minValue: 0,
    maxValue: 100,
    value: 50,
    segmentWidth: 2,
    gap: 1,
    segmentHeight: 10,
    baselineOffset: 2,
    filledColor: "blue",
    emptyColor: "#333",
    baselineColor: "white"
  };
  const spoInfo = rig_createSegmentedMeter(spoGroup, spoConfig);
  rig_drawBottomScale(spoGroup, spoInfo.totalWidth, spoInfo.baselineY, [0,20,40,60,80,100], {
    dotOffset: 4,
    textOffset: 12,
    fontSize: "10",
    label: "PO:"
  });
  rig_drawTopScale(spoGroup, spoInfo.totalWidth, {
    dotOffset: -4,
    textOffset: -12,
    fontSize: "10"
  });

  // ---------------------------
  // 2) ALC Meter (range 0–100) with NO scale (labeled "ALC:" at x=40,y=100).
  // ---------------------------
  const alcGroup = document.getElementById('rig_alcMeterGroup');
  const alcConfig = {
    maxSegments: 101,
    minValue: 0,
    maxValue: 100,
    value: 60,
    segmentWidth: 2,
    gap: 1,
    segmentHeight: 10,
    baselineOffset: 2,
    filledColor: "blue",
    emptyColor: "#333",
    baselineColor: "white"
  };
  rig_createSegmentedMeter(alcGroup, alcConfig);

  // ---------------------------
  // 3) COMP Meter (range 0–25), scale [0,5,10,15,20,25], replace "25" => "dB".
  // ---------------------------
  const compGroup = document.getElementById('rig_compMeterGroup');
  const compConfig = Object.assign({}, alcConfig, { maxValue: 25, value: 8 });
  const compInfo = rig_createSegmentedMeter(compGroup, compConfig);
  rig_drawBottomScale(compGroup, compInfo.totalWidth, compInfo.baselineY, [0,5,10,15,20,25], {
    dotOffset: 4,
    textOffset: 12,
    fontSize: "10"
  });
  Array.from(compGroup.querySelectorAll("text")).forEach(txt => {
    if (txt.textContent === "25") {
      txt.textContent = "dB";
    }
  });

  // ---------------------------
  // 4) SWR Meter (range 0–100) with custom bottom scale => 0..3,∞ labeled "SWR:"
  // ---------------------------
  const swrGroup = document.getElementById('rig_swrMeterGroup');
  const swrConfig = Object.assign({}, alcConfig, { value: 65, filledColor: "cyan" });
  const swrInfo = rig_createSegmentedMeter(swrGroup, swrConfig);
  rig_drawSWRBottomScale(swrGroup, swrInfo.totalWidth, swrInfo.baselineY, {
    dotOffset: 4,
    textOffset: 12,
    fontSize: "10",
    label: "SWR:"
  });

  // ---------------------------
  // 5) AMP Meter (range 0–20), scale [0,5,10,15,20]. 0->"0A", 20->"20A".
  // ---------------------------
  const idGroup = document.getElementById('rig_idMeterGroup');
  const idConfig = Object.assign({}, alcConfig, { maxValue: 20, value: 55, filledColor: "cyan" });
  const idInfo = rig_createSegmentedMeter(idGroup, idConfig);
  function ampFormatter(v) {
    return (v === 0 || v === 20) ? v + "A" : v;
  }
  rig_drawBottomScale(idGroup, idInfo.totalWidth, idInfo.baselineY, [0,5,10,15,20], {
    dotOffset: 4,
    textOffset: 12,
    fontSize: "10"
  });
  Array.from(idGroup.querySelectorAll("text")).forEach(txt => {
    let num = parseFloat(txt.textContent);
    if(!isNaN(num)) {
      txt.textContent = ampFormatter(num);
    }
  });

  // ---------------------------
  // 6) Right-side VCC Meter (range 10–16), 51 segments, scale [10,12,14,16]
  // ---------------------------
  const vdGroup = document.getElementById('rig_vdMeterGroup');
  const vdConfig = {
    maxSegments: 51,
    minValue: 10,
    maxValue: 16,
    value: 14,
    segmentWidth: 2,
    gap: 1,
    segmentHeight: 10,
    baselineOffset: 2,
    filledColor: "red",
    emptyColor: "#333",
    baselineColor: "white"
  };
  const vdInfo = rig_createSegmentedMeter(vdGroup, vdConfig);
  rig_drawBottomScale(vdGroup, vdInfo.totalWidth, vdInfo.baselineY, [10,12,14,16], {
    dotOffset: 4,
    textOffset: 12,
    fontSize: "10",
    suffix: "V"
  });

  // ---------------------------
  // 7) Right-side TEMP Meter (range 0–100), 51 segments, scale [0,50,100]
  // ---------------------------
  const tempGroup = document.getElementById('rig_tempMeterGroup');
  const tempConfig = {
    maxSegments: 51,
    minValue: 0,
    maxValue: 100,
    value: 60,
    segmentWidth: 2,
    gap: 1,
    segmentHeight: 10,
    baselineOffset: 2,
    filledColor: "red",
    emptyColor: "#333",
    baselineColor: "white"
  };
  const tempInfo = rig_createSegmentedMeter(tempGroup, tempConfig);
  rig_drawBottomScale(tempGroup, tempInfo.totalWidth, tempInfo.baselineY, [0,50,100], {
    dotOffset: 4,
    textOffset: 12,
    fontSize: "10"
  });
});

document.addEventListener('rig_event', (e) => { /* Rig Event Handling */

  console.log(`${e.detail.rig._type} device fired ${e.detail.value} event`);

  switch (e.detail.value) {
    case "deviceReady":
      e.detail.rig.initialize();
      break;
      
    case "TX":
      rig_setTXIndicatorOn(true);
      break;
      
    case "RX":
      rig_setTXIndicatorOn(false);
      break;

    case "freq":
      let freq = e.detail.freq;
      if (arrRadio[rig_activeArrRadioIndex].address == e.detail.rig.address) {
      if (!Number.isNaN(parseFloat(freq.replace(".", "")))) {
        document.getElementById("rig_Main_Band").style.display = "block";
        document.getElementById("rig_mainBandIndicators").style.display = "block";
        let last = freq.slice(-1), rest = freq.slice(0, -1);
        let first = rest.split(".")[0] + ".";
        rest = rest.replace(first, "");
        document.getElementById("rig_mainFrequency").innerHTML =
          '<tspan style="font-size:38px;" onclick="hmi_open_popup(\'hmi_rig_panel_band\')">' + first + "</tspan>" + rest + '<tspan style="font-size:22px;">' + last + "</tspan>";
        console.log("Decoded frequency =>", freq);
      } else {
        console.log("Error Decoding frequency =>", freq, "  -- type = ", typeof freq.replace(".", ""));
      }
      }
      break;      

    case "mode":
      let newMode = e.detail.mode;
      for (let i = 0; i < rig_modeList.length; i++) {
        if (rig_decodeMode(rig_modeList[i]) == newMode) {
          rig_currentModeIndex = i;
          break;
        }
      }
      let dataMode = "";
      if (rig_dataMode == 1) dataMode = "-D";
      document.getElementById("rig_modeBtnLabel").textContent = rig_decodeMode(rig_modeList[rig_currentModeIndex]) + dataMode;
      console.log("Setting Mode =>", newMode + dataMode);
      break;

    case "filter":
      let newFilter = e.detail.filter;
      for (let i = 0; i < rig_filterList.length; i++) {
        if (rig_decodeFilter(rig_filterList[i]) == newFilter) {
          rig_currentFilterIndex = i;
          break;
        }
      }
      document.getElementById("rig_filterBtnLabel").textContent = rig_decodeFilter(rig_filterList[rig_currentFilterIndex]);
      console.log("Setting Filter =>", newFilter);
      break;

    case "dataMode":
      rig_dataMode = e.detail.dataMode;
      let dataMode1 = "";
      if (rig_dataMode == 1) {
        dataMode1 = "-D";
        console.log("Setting DataMode =>", dataMode1);
        document.getElementById("rig_modeBtnLabel").textContent = rig_decodeMode(rig_modeList[rig_currentModeIndex]) + dataMode1;
      }
      break;    

    case "split":
      // Update SPLIT button text color accordingly:
      const splitValue = e.detail.split;
      const splitBtn = document.getElementById("rig_splitBtnLabel");
      rig_currentSplitValue = splitValue;
      let boolSplit = false;
      if (splitValue === "01") boolSplit = true;
      if (splitBtn) {
        if (boolSplit) {
          splitBtn.setAttribute("fill", "#FFA500");
        } else {
          splitBtn.setAttribute("fill", "#fff");
        }
      }
      break;

    case "power":
      const b = document.getElementById("rig_powerButtonIndicator");
      if (e.detail.power) {
        b.className.baseVal = "solid-power-indicator-on";
        b.className.animVal = "";
      }
      else {
        b.className.baseVal = "solid-power-indicator-off";
        b.className.animVal = "";
      }
      break;
  }
});

setInterval(function() { /* Updates the header text every second (or current refresh rate) */
  const currentDate = Date.now();
  for (let i = 0; i < arrRadio.length; i++) {
    const lastDate = arrRadio[i]._lastResponseDt.valueOf();
    const diffDate = currentDate - lastDate;
    if (diffDate > 2000) {
      arrRadio[i]._powerOn = false;
      arrRadio[i]._poweringState = "off";
    } else {
      arrRadio[i]._powerOn = true;
      arrRadio[i]._poweringState = "on";
    }

    if (rig_activeArrRadioIndex == i) {
      const b = document.getElementById("rig_powerButtonIndicator");
      if (arrRadio[i]._powerOn) {
        b.className.baseVal = "solid-power-indicator-on";
        b.className.animVal = "";
      }
      else {
        b.className.baseVal = "solid-power-indicator-off";
        b.className.animVal = "";
      }
    }
  }
}, 500);

// Highlight selected rig button
function rig_highlightSelectedRig(radioAddress) {
  const btn7300Rect = document.querySelector("#rig_btnIC7300 rect");
  const btn7300Text = document.querySelector("#rig_btnIC7300 text");
  const btn9700Rect = document.querySelector("#rig_btnIC9700 rect");
  const btn9700Text = document.querySelector("#rig_btnIC9700 text");

  // Reset both to dark
  btn7300Rect.setAttribute("fill", "#333");
  btn7300Text.setAttribute("fill", "#fff");
  btn9700Rect.setAttribute("fill", "#333");
  btn9700Text.setAttribute("fill", "#fff");

  if (radioAddress === "94") {
    btn7300Rect.setAttribute("fill", "#ccff66");
    btn7300Text.setAttribute("fill", "#000");
  } else if (radioAddress === "A2") {
    btn9700Rect.setAttribute("fill", "#ccff66");
    btn9700Text.setAttribute("fill", "#000");
  }
}

function rig_getArrRadioIndex(address) {
  arrRadioIndex = -1;  
  for (let i = 0; i < arrRadio.length; i++) {
    if (arrRadio[i].address == address) {
      arrRadioIndex = i;
      break;
    }
  }

  return arrRadioIndex;  
}

// Called when the user selects a rig model
function rig_selectModel(modelName, radioAddress) {

  arrRadioIndex = rig_getArrRadioIndex(radioAddress);  
  rig_activeArrRadioIndex = arrRadioIndex;

  console.log("rig_selectModel =>", modelName, radioAddress);
  document.getElementById("rig_addressBox").value = radioAddress;
  hmi_header_rig = modelName;
  rig_activeRig = modelName;
  rig_highlightSelectedRig(radioAddress);

  // Show common UI elements
  document.getElementById("rig_transmitButtonGroup").style.display = "block";
  document.getElementById("rig_ampNotchNBGroup").style.display = "block";
  document.getElementById("rig_bottomRowGroup").style.display = "block";
  document.getElementById("rig_txIndicator").style.display = "block";
  document.getElementById("rig_Main_Band").style.display = "block";

  // Hide sub-band by default
  rig_subBandVisible = false;
  document.getElementById("rig_Sub_Band").style.display = "none";

  // Default operate mode => VFO side A
  rig_operateMode = "VFO";
  rig_vfoToggle = false;
  document.getElementById("rig_operateModeIndicator").textContent = "VFO A";
  document.getElementById("rig_operateModeIndicatorSub").textContent = "VFO A";

  // Hide tuner/call and button groups initially
  document.getElementById("rig_ic7300TunerVoxGroup").style.display = "none";
  document.getElementById("rig_ic9700CallVoxGroup").style.display = "none";
  document.getElementById("rig_ic7300ButtonsGroup").style.display = "none";
  document.getElementById("rig_ic9700ButtonsGroup").style.display = "none";

  if (radioAddress === "A2") {
    document.getElementById("rig_ic9700ButtonsGroup").style.display = "block";
    document.getElementById("rig_ic9700CallVoxGroup").style.display = "block";
    document.getElementById("rig_bannerDeviceRight").textContent = "IC‑9700";
    document.getElementById("rig_bannerDeviceCenter").textContent = "VHF/UHF ALL MODE TRANSCEIVER";
    document.getElementById("rig_subBandToggleButton").style.display = "block";
    document.getElementById("rig_subBandHideButton").style.display = "none";

    rig_sendReceiverIDCommand();
    //setTimeout(objRig.sendFrequencyQuery, 150);
    //setTimeout(rig_sendDefaultMode, 350);
    //setTimeout(rig_send0FCommand, 600);
    setTimeout(() => {arrRadio[arrRadioIndex].sendFrequencyQuery();}, 150);
    setTimeout(() => {arrRadio[arrRadioIndex].sendDefaultMode();}, 350);
    setTimeout(() => {arrRadio[arrRadioIndex].send0FCommand();}, 600);
  } else if (radioAddress === "94") {
    document.getElementById("rig_ic7300ButtonsGroup").style.display = "block";
    document.getElementById("rig_ic7300TunerVoxGroup").style.display = "block";
    document.getElementById("rig_bannerDeviceRight").textContent = "IC‑7300";
    document.getElementById("rig_bannerDeviceCenter").textContent = "HF/50MHz TRANSCEIVER";
    document.getElementById("rig_subBandToggleButton").style.display = "none";
    document.getElementById("rig_subBandHideButton").style.display = "none";

    //setTimeout(objRig.sendFrequencyQuery, 150);
    //setTimeout(rig_sendDefaultMode, 350);
    //setTimeout(rig_send0FCommand, 600);
    setTimeout(() => {arrRadio[arrRadioIndex].sendFrequencyQuery();}, 150);
    setTimeout(() => {arrRadio[arrRadioIndex].sendDefaultMode();}, 350);
    setTimeout(() => {arrRadio[arrRadioIndex].send0FCommand();}, 600);
  }
}

function rig_incrementMode() {
  let dataMode = "";
  if (rig_dataMode == 1) dataMode = "-D";

  rig_currentModeIndex = (rig_currentModeIndex + 1) % rig_modeList.length;
  // Immediately update the display for the Mode button
  document.getElementById("rig_modeBtnLabel").textContent = rig_decodeMode(rig_modeList[rig_currentModeIndex]) + dataMode;
  rig_sendModeFilterThen04();
}

function rig_incrementFilter() {
  rig_currentFilterIndex = (rig_currentFilterIndex + 1) % rig_filterList.length;
  // Immediately update the display for the FIL button
  document.getElementById("rig_filterBtnLabel").textContent = rig_decodeFilter(rig_filterList[rig_currentFilterIndex]);

  if (rig_dataMode == 0) {
    rig_sendModeFilterThen04();
  } else {
    setTimeout(()=> {
      arrRadio[rig_activeArrRadioIndex].sendCommand(`1A 06 01 ${rig_filterList[rig_currentFilterIndex]}`); // turn on DATA mode
    }, 250);
  }
}

function rig_sendModeFilterThen04(modeValue, filterValue) {
  //const a = document.getElementById("rig_addressBox").value.trim();
  const arrRadioIndex = rig_activeArrRadioIndex;
  const a = arrRadio[arrRadioIndex].address;
  const b = hmi_display_address;
  let m = rig_modeList[rig_currentModeIndex],
      f = rig_filterList[rig_currentFilterIndex];
  if (modeValue != undefined) m = modeValue;
  if (filterValue != undefined) f = filterValue;
  let msg = (`FE FE ${a} ${b} 06 ${m} ${f} FD`).replace(/\s+/g, " ").trim();
  objSMDevice.send(msg);
  let msg2 = (`FE FE ${a} ${b} 04 FD`).replace(/\s+/g, " ").trim();
  objSMDevice.send(msg2);
}

function rig_toggleDataMode(setNewMode) {
  //const radioAddress = arrRadio[rig_activeArrRadioIndex].address;
  const oldMode = arrRadio[rig_activeArrRadioIndex]._dataMode;
  const arrRadioIndex = rig_activeArrRadioIndex;
  let f = rig_filterList[rig_currentFilterIndex];

  if (setNewMode == undefined) {
    setNewMode = (oldMode + 1) % 2;
  }

  if (setNewMode == 1) {
    arrRadio[rig_activeArrRadioIndex].sendCommand(`1A 06 01 ${f}`); // turn on DATA mode
  } else {
    arrRadio[rig_activeArrRadioIndex].sendCommand("1A 06 00 00"); // turn off DATA mode
  }
  setTimeout(() => {arrRadio[rig_activeArrRadioIndex].sendCommand("1A 06")}, 500);
  // temporary rig variable set below, replace with event from radio object (read by handler here)
  rig_dataMode = setNewMode;
}

function rig_splitButtonToggle() {
  let newVal = (rig_currentSplitValue == "00") ? "01" : "00";
  if (rig_currentSplitValue !== "00" && rig_currentSplitValue !== "01") newVal = "01";

  arrRadio[rig_activeArrRadioIndex].sendCommand(`0F ${newVal}`);

  //setTimeout(rig_send0FCommand, 600);
  setTimeout(() => {arrRadio[rig_activeArrRadioIndex].send0FCommand();}, 600);
}

function rig_toneButtonToggle() {
  const a = document.getElementById("rig_addressBox").value.trim();
  const b = hmi_display_address;
  if (!a) return;
  if (rig_toneValue === "00") rig_toneValue = "11";
  else if (rig_toneValue === "11") rig_toneValue = "12";
  else rig_toneValue = "00";
  let toneMsg = (`FE FE ${a} ${b} 0F ${rig_toneValue} FD`).replace(/\s+/g, " ").trim();
  ws.send(toneMsg);
  setTimeout(rig_send0FCommand, 600);
  const toneLabel = document.getElementById("toneBtnLabel");
  if (toneLabel) {
    if (rig_toneValue === "00") {
      toneLabel.textContent = "TONE";
      toneLabel.setAttribute("fill", "#fff");
    } else if (rig_toneValue === "11") {
      toneLabel.textContent = "DUP -";
      toneLabel.setAttribute("fill", "#FFA500");
    } else if (rig_toneValue === "12") {
      toneLabel.textContent = "DUP +";
      toneLabel.setAttribute("fill", "#FFA500");
    }
  }
}

function rig_operateModeToggle() {
  if (rig_operateMode === "VFO") {
    //ws.send(`FE FE ${a} E0 08 ${rig_memoryCH} FD`);
    arrRadio[rig_activeArrRadioIndex].sendCommand('08');
    setTimeout(() => {arrRadio[rig_activeArrRadioIndex].sendFrequencyQuery();}, 200);
    rig_operateMode = "MEM";
    document.getElementById("rig_operateModeIndicator").textContent = "MEM " + parseInt(rig_memoryCH, 10);
    document.getElementById("rig_operateModeIndicatorSub").textContent = "MEM " + parseInt(rig_memoryCH, 10);
  } else {
    rig_selectVfo();
    rig_operateMode = "VFO";
  }
}

function rig_selectVfo(vfoSide) {
  if (rig_operateMode !== "VFO" && vfoSide == undefined) {
    // Switch rig to VFO Mode
    arrRadio[rig_activeArrRadioIndex].sendCommand('07');
  } else if (vfoSide == "A") {
    arrRadio[rig_activeArrRadioIndex].sendCommand('07 00');
    rig_vfoToggle = false;
  } else {
    arrRadio[rig_activeArrRadioIndex].sendCommand('07 01');
    rig_vfoToggle = true;
  }
  setTimeout(() => {arrRadio[rig_activeArrRadioIndex].sendFrequencyQuery();}, 200);
  setTimeout(() => {arrRadio[rig_activeArrRadioIndex].sendDefaultMode();}, 400);
  document.getElementById("rig_operateModeIndicator").textContent = "VFO " + (rig_vfoToggle ? "B" : "A");
  document.getElementById("rig_operateModeIndicatorSub").textContent = "VFO " + (rig_vfoToggle ? "B" : "A");
}

function rig_toggleVfoSide() {
  const a = document.getElementById("rig_addressBox").value.trim();
  if (!a) return;
  if (rig_vfoToggle) {
    rig_selectVfo("A");
  } else {
    rig_selectVfo("B");
  }
}

function rig_sendReceiverIDCommand() {
  // for the 9700 only?
  arrRadio[rig_activeArrRadioIndex].sendCommand("1F 00");
}

function rig_setPowerOn(powerOn = !arrRadio[rig_activeArrRadioIndex]._powerOn) {
  const b = document.getElementById("rig_powerButtonIndicator");

  if (powerOn && !arrRadio[rig_activeArrRadioIndex]._powerOn) {
    b.className.baseVal = "blinking-power-indicator-on";
    b.className.animVal = "blinking-power-indicator-on";
  } else {
    b.className.baseVal = "blinking-power-indicator-off";
    b.className.animVal = "blinking-power-indicator-off";
  }

  if (powerOn) {
    b.style.fill = "#ccff66";
  } else {
    b.style.fill = "#be0000";
  }

  arrRadio[rig_activeArrRadioIndex].powerOn(powerOn);
}

function rig_setTXIndicatorOn(boolTX = true) {
  const a = document.getElementById("rig_txRect");
  const b = document.getElementById("rig_txText");

  /*
  <rect id="rig_txRect" width="30" height="20" fill="none" stroke="red" stroke-width="2" rx="3" ry="3"/>
  <text id="rig_txText" x="15" y="14" fill="red" font-size="12" font-weight="bold" text-anchor="middle">TX</text>
  */

  if (boolTX) {
    a.style.fill = "red";
    a.style.stroke = "white";
    b.style.fill = "white";
  } else {
    a.style.fill = "none";
    a.style.stroke = "#333333";
    b.style.fill = "#333333";
  }
}

function rig_sendHex(h) {
  ws.send(h);
  document.getElementById("sentCommand").innerText = h;
}

function rig_decodeFrequencyReversedBCD(b5, b6, b7, b8, b9) {
  let arr = [b9, b8, b7, b6, b5], joined = arr.join("");
  while (joined.length < 10) { joined = "0" + joined; }
  let front = joined.substring(0, 4),
      mid = joined.substring(4, 7),
      last = joined.substring(7);
  front = front.replace(/^0+/, '');
  if (front === "") front = "0";
  return front + "." + mid + "." + last;
}

// Sub_Band toggling
function rig_toggleSubBand() {
  const a = document.getElementById("rig_addressBox").value.trim().toUpperCase();
  if (a !== "A2") {
    console.log("Sub_Band toggle only works for IC‑9700");
    return;
  }
  rig_subBandVisible = !rig_subBandVisible;
  if (rig_subBandVisible) {
    document.getElementById("Sub_Band").style.display = "block";
    document.getElementById("subBandIndicators").style.display = "block";
    document.getElementById("subBandToggleButton").style.display = "none";
    document.getElementById("subBandHideButton").style.display = "block";
  } else {
    document.getElementById("Sub_Band").style.display = "none";
    document.getElementById("subBandIndicators").style.display = "none";
    document.getElementById("subBandToggleButton").style.display = "none";
    document.getElementById("subBandHideButton").style.display = "none";
  }
}

function rig_startSubBandHideTimer() {
  subBandHideTimer = setTimeout(() => {
    rig_subBandVisible = false;
    document.getElementById("Sub_Band").style.display = "none";
    document.getElementById("subBandIndicators").style.display = "none";
    document.getElementById("subBandToggleButton").style.display = "block";
    document.getElementById("subBandHideButton").style.display = "none";
    subBandHideTimer = null;
  }, 500);
}

function rig_cancelSubBandHideTimer() {
  if (subBandHideTimer) {
    clearTimeout(subBandHideTimer);
    subBandHideTimer = null;
  }
}

// SPLIT / DUP Indicator
function rig_setSplitDupIndicator(m) {
  const grp = document.getElementById("rig_splitDupIndicator");
  if (m === "hide") {
    grp.style.display = "none";
    return;
  }
  grp.style.display = "block";
  const rect = document.getElementById("rig_splitDupRect");
  const txt = document.getElementById("rig_splitDupText");
  switch (m) {
    case "split":
      rect.setAttribute("fill", "#FFA500");
      rect.setAttribute("stroke", "none");
      txt.textContent = "SPLIT";
      txt.setAttribute("fill", "#fff");
      // Also update the SPLIT button text color if available
      const splitBtn = document.getElementById("rig_splitBtnLabel");
      if (splitBtn) splitBtn.setAttribute("fill", "#FFA500");
      break;
    case "dupMinus":
      rect.setAttribute("fill", "none");
      rect.setAttribute("stroke", "#888");
      rect.setAttribute("stroke-width", "2");
      txt.textContent = "DUP -";
      txt.setAttribute("fill", "#ccc");
      break;
    case "dupPlus":
      rect.setAttribute("fill", "none");
      rect.setAttribute("stroke", "#888");
      rect.setAttribute("stroke-width", "2");
      txt.textContent = "DUP +";
      txt.setAttribute("fill", "#ccc");
      break;
  }
  txt.setAttribute("font-weight", "bold");
}

function rig_decodeMode(h) {
  switch (h) {
    case "00": return "LSB";
    case "01": return "USB";
    case "02": return "AM";
    case "03": return "CW";
    case "04": return "RTTY";
    case "05": return "FM";
    case "07": return "CW-R";
    case "08": return "RTTY-R";
    case "17": return "DV";
    case "22": return "DD";
    default:   return "Unknown";
  }
}

function rig_decodeDataMode(d) {
  if (d == undefined) {
    d = rig_dataMode;
  }
  switch (d) {
    case 0: return "";
    case 1: return "-D";
    default: return "";
  }
}

function rig_selectMode(modeName) {
  let filterValue = rig_filterList[rig_currentFilterIndex];

  switch (modeName) {
    case "LSB":
      rig_sendModeFilterThen04("00", filterValue);
      break;
    case "USB":
      rig_sendModeFilterThen04("01", filterValue);
      break;
    case "AM":
      rig_sendModeFilterThen04("02", filterValue);
      break;
    case "CW":
      rig_sendModeFilterThen04("03", filterValue);
      break;
    case "RTTY":
      rig_sendModeFilterThen04("04", filterValue);
      break;
    case "FM":
      rig_sendModeFilterThen04("05", filterValue);
      break;
    case "CW-R":
      rig_sendModeFilterThen04("07", filterValue);
      break;
    case "RTTY-R":
      rig_sendModeFilterThen04("08", filterValue);
      break;
    case "DV":
      rig_sendModeFilterThen04("17", filterValue);
      break;
    case "DD":
      rig_sendModeFilterThen04("22", filterValue);
      break;
  }
  if (rig_dataMode == 0) {
    setTimeout(()=> {console.log("setting DATA mode: 0");rig_toggleDataMode(0)}, 250);
  } else {
    setTimeout(()=> {console.log("setting DATA mode: 1");rig_toggleDataMode(1)}, 250);
  }
}

function rig_decodeFilter(h) {
  switch (h) {
    case "01": return "FIL1";
    case "02": return "FIL2";
    case "03": return "FIL3";
    default:   return "";
  }
}

function rig_selectFilter(filterName) {
  let filterIndex = 0;
  switch (filterName) {
    case "FIL1":
      filterIndex = 0;
      break;
    case "FIL2":
      filterIndex = 1;
      break;
    case "FIL3":
      filterIndex = 2;
      break;
  }
  rig_currentFilterIndex = filterIndex;

  rig_sendModeFilterThen04(rig_modeList[rig_currentModeIndex], rig_filterList[rig_currentFilterIndex]);  
}

function rig_selectBand(band) {
  let bandIndex = 0;
  let stackIndex = 0;

  switch (band) {
    case "1.8":
      objRig.sendCommand("1A 01 01 01");
      bandIndex = 0;
      break;
    case "3.5":
      objRig.sendCommand("1A 01 02 01");
      bandIndex = 1;
      break;
    case "7":
      objRig.sendCommand("1A 01 03 01");
      bandIndex = 2;
      break;
    case "10":
      objRig.sendCommand("1A 01 04 01");
      bandIndex = 3;
      break;
    case "14":
      objRig.sendCommand("1A 01 05 01");
      bandIndex = 4;
      break;
    case "18":
      objRig.sendCommand("1A 01 06 01");
      bandIndex = 5;
      break;
    case "21":
      objRig.sendCommand("1A 01 07 01");
      bandIndex = 6;
      break;
    case "24":
      objRig.sendCommand("1A 01 08 01");
      bandIndex = 7;
      break;
    case "28":
      objRig.sendCommand("1A 01 09 01");
      bandIndex = 8;
      break;
    case "50":
      objRig.sendCommand("1A 01 10 01");
      bandIndex = 9;
      break;
    case "144":
      objRig.sendCommand("1A 01 01 01");
      bandIndex = 0;
      break;
    case "430":
      objRig.sendCommand("1A 01 02 01");
      bandIndex = 1;
      break;
    case "1240":
      objRig.sendCommand("1A 01 03 01");
      bandIndex = 2;
      break;
  }

  setTimeout(()=>{
    const a = document.getElementById("rig_addressBox").value.trim();
    switch (a) {
      case "94":
        rig_setFrequency(rig_bandStackingList7300[bandIndex][stackIndex]);
        break;
      case "A2":
        rig_setFrequency(rig_bandStackingList9700[bandIndex][stackIndex]);
        break;
    }
  }, 400);

  setTimeout(()=>{
    objRig.sendCommand("03");
  }, 600);


  /*
    SENT:      FE FE 94 E0 1A 01 03 01 FD 
    RECEIVED:  FE FE E0 94 1A 01 03 01 00 40 07 07 00 01 01 10 00 08 85 00 08 85 FD


    IC-7300
     FE FE                   # 2 byte, CI-V header
     E0 XX 1A 01 03 01       # 6 bytes, The command payload, XX is the rig's address
     00 40 07 07 00          # 5 bytes, Operating frequency setting
     01 01                   # 2 bytes, Operating mode setting
     10                      # 1 byte, Data and Tone Type settings
     00 08 85                # 3 bytes, Repeater tone frequency setting
     00 08 85                # 3 bytes, Repeater tone frequency setting
     FD                      # 1 byte, CI-V tail


    IC-9700
     FE FE                   # 2 byte, CI-V header
     E0 XX 1A 01 01 01       # 6 bytes, The command payload, XX is the rig's address
     00 00 80 01 00          # 5 bytes, Operating frequency setting
     03 02                   # 2 bytes, Operating mode setting
     00                      # 1 byte, Data mode setting 
     00                      # 1 byte, Duplex and Tone settings
     00                      # 1 byte, Digital squelch setting
     00 08 85                # 3 bytes, Repeater tone frequency setting
     00 08 85                # 3 bytes, Repeater tone frequency setting
     00 00 23                # 3 bytes, DTCS code setting
     00                      # 1 byte, DV Digital code squelch setting
     00 50 00                # 3 bytes, Duplex offset frequency setting
     58 36 31 30 30 20 20 20 # 8 bytes, UR (Destination) call sign setting
     20 20 20 20 20 20 20 20 # 8 bytes, R1 (Access repeater) call sign setting
     20 20 20 20 20 20 20 20 # 8 bytes, R2 (Gateway/Link repeater) call sign setting
     FD                      # 1 byte, CI-V tail
  */
}

function rig_setFrequency(decimalFrequency) {
  arrFreq = rig_encodeFrequencyReversedBCD(decimalFrequency);
  strFreq = arrFreq[0];
  strFreq += " " + arrFreq[1];
  strFreq += " " + arrFreq[2];
  strFreq += " " + arrFreq[3];
  strFreq += " " + arrFreq[4];

  objRig.sendCommand("25 00 " + strFreq); // turn off DATA mode
}

function rig_encodeFrequencyReversedBCD(decimalFrequency) {
  let arrFreq = [5];
  decimalFrequency = "0000000000" + decimalFrequency;
  decimalFrequency = decimalFrequency.replace(".", "");
  decimalFrequency = decimalFrequency.replace(".", "");

  arrFreq[0] = decimalFrequency.at(decimalFrequency.length - 2) + decimalFrequency.at(decimalFrequency.length - 1);
  arrFreq[1] = decimalFrequency.at(decimalFrequency.length - 4) + decimalFrequency.at(decimalFrequency.length - 3);
  arrFreq[2] = decimalFrequency.at(decimalFrequency.length - 6) + decimalFrequency.at(decimalFrequency.length - 5);
  arrFreq[3] = decimalFrequency.at(decimalFrequency.length - 8) + decimalFrequency.at(decimalFrequency.length - 7);
  arrFreq[4] = decimalFrequency.at(decimalFrequency.length - 10) + decimalFrequency.at(decimalFrequency.length - 9);

  return arrFreq;
}

/**
 * Creates a segmented meter (bar plus baseline) in a given container.
 * Returns { totalWidth, baselineY, segmentHeight }.
 */
function rig_createSegmentedMeter(container, config) {
  config = config || {};
  const maxSegments   = config.maxSegments   ?? 101;
  const minValue      = config.minValue      ?? 0;
  const maxValue      = config.maxValue      ?? 100;
  const currentValue  = config.value         ?? 50;
  const segmentWidth  = config.segmentWidth  ?? 2;
  const gap           = config.gap           ?? 1;
  const segmentHeight = config.segmentHeight ?? 10;
  const baselineOffset= config.baselineOffset?? 2;
  const filledColor   = config.filledColor   ?? "blue";
  const emptyColor    = config.emptyColor    ?? "#333";
  const baselineColor = config.baselineColor ?? "white";

  container.innerHTML = "";

  for (let i = 0; i < maxSegments; i++) {
    const rect = document.createElementNS("http://www.w3.org/2000/svg", "rect");
    const x = i * (segmentWidth + gap);
    rect.setAttribute("x", x);
    rect.setAttribute("y", 0);
    rect.setAttribute("width", segmentWidth);
    rect.setAttribute("height", segmentHeight);

    const segVal = minValue + i * (maxValue - minValue) / (maxSegments - 1);
    rect.setAttribute("fill", segVal <= currentValue ? filledColor : emptyColor);
    container.appendChild(rect);
  }

  const totalWidth = maxSegments * (segmentWidth + gap) - gap;
  const baselineY = segmentHeight + baselineOffset;
  const baseline = document.createElementNS("http://www.w3.org/2000/svg", "line");
  baseline.setAttribute("x1", 0);
  baseline.setAttribute("y1", baselineY);
  baseline.setAttribute("x2", totalWidth);
  baseline.setAttribute("y2", baselineY);
  baseline.setAttribute("stroke", baselineColor);
  baseline.setAttribute("stroke-width", "1");
  container.appendChild(baseline);

  return { totalWidth, baselineY, segmentHeight };
}

function rig_updateSegmentedMeter(meter, value) {
  let m = document.getElementById(meter);
  let s = 0;

  for (let i = 0; i < m.children.length; i++) {
    if (m.children[i].tagName == "rect") {
      s++;
    }
  }

  let v = s * (value / 100);



  for (let i = 0; i < s; i++) {
    if (i < v) {
      m.children[i].style.fill = "blue";
    } else {
      m.children[i].style.fill = "white";
    }
  }
}

/**
 * Draws a bottom scale below a meter.
 * Options: dotOffset, textOffset, fontSize, label, suffix.
 */
function rig_drawBottomScale(container, totalWidth, baselineY, scaleVals, options) {
  options = options || {};
  const dotOffset = options.dotOffset ?? 4;
  const textOffset = options.textOffset ?? 12;
  const fontSize = options.fontSize || "10";
  const suffix = options.suffix || "";
  const minScale = scaleVals[0];
  const maxScale = scaleVals[scaleVals.length - 1];

  scaleVals.forEach(val => {
    const ratio = (val - minScale) / (maxScale - minScale);
    const x = ratio * totalWidth;

    const dot = document.createElementNS("http://www.w3.org/2000/svg", "circle");
    dot.setAttribute("cx", x);
    dot.setAttribute("cy", baselineY + dotOffset);
    dot.setAttribute("r", 2);
    dot.setAttribute("fill", "white");
    container.appendChild(dot);

    const txt = document.createElementNS("http://www.w3.org/2000/svg", "text");
    txt.setAttribute("x", x - 8);
    txt.setAttribute("y", baselineY + textOffset);
    txt.setAttribute("fill", "white");
    txt.setAttribute("font-size", fontSize);
    txt.textContent = val + suffix;
    container.appendChild(txt);
  });

  if (options.label) {
    const lbl = document.createElementNS("http://www.w3.org/2000/svg", "text");
    lbl.setAttribute("x", -40);
    lbl.setAttribute("y", baselineY + textOffset);
    lbl.setAttribute("fill", "white");
    lbl.setAttribute("font-size", fontSize);
    lbl.textContent = options.label;
    container.appendChild(lbl);
  }
}

/**
 * Draws a top scale above a meter (the S scale).
 */
function rig_drawTopScale(container, totalWidth, options) {
  options = options || {};
  const dotOffset = options.dotOffset ?? -4;
  const textOffset = options.textOffset ?? -12;
  const fontSize = options.fontSize || "10";

  // "S:" label
  const sLabel = document.createElementNS("http://www.w3.org/2000/svg", "text");
  sLabel.setAttribute("x", -40);
  sLabel.setAttribute("y", textOffset);
  sLabel.setAttribute("fill", "white");
  sLabel.setAttribute("font-size", fontSize);
  sLabel.textContent = "S:";
  container.appendChild(sLabel);

  // Left half: 0..9
  const leftWidth = totalWidth / 2;
  for (let i = 0; i < 10; i++) {
    const x = (i / 9) * leftWidth;
    const dot = document.createElementNS("http://www.w3.org/2000/svg", "circle");
    dot.setAttribute("cx", x);
    dot.setAttribute("cy", dotOffset);
    dot.setAttribute("r", 2);
    dot.setAttribute("fill", "white");
    container.appendChild(dot);

    const text = document.createElementNS("http://www.w3.org/2000/svg", "text");
    text.setAttribute("x", x - 6);
    text.setAttribute("y", textOffset);
    text.setAttribute("fill", "white");
    text.setAttribute("font-size", fontSize);
    text.textContent = i;
    container.appendChild(text);
  }

  // Right half: +10..+60
  const rightWidth = totalWidth / 2;
  for (let j = 0; j < 6; j++) {
    const x = leftWidth + (j / 5) * rightWidth;
    const dot = document.createElementNS("http://www.w3.org/2000/svg", "circle");
    dot.setAttribute("cx", x);
    dot.setAttribute("cy", dotOffset);
    dot.setAttribute("r", 2);
    dot.setAttribute("fill", "white");
    container.appendChild(dot);

    if (j > 0) {
      const text = document.createElementNS("http://www.w3.org/2000/svg", "text");
      text.setAttribute("x", x - 10);
      text.setAttribute("y", textOffset);
      text.setAttribute("fill", "white");
      text.setAttribute("font-size", fontSize);
      text.textContent = "+" + ((j + 1) * 10);
      container.appendChild(text);
    }
  }
}

/**
 * Draws a custom bottom scale for the SWR meter (0..3 + ∞).
 */
function rig_drawSWRBottomScale(container, totalWidth, baselineY, options) {
  options = options || {};
  const dotOffset = options.dotOffset ?? 4;
  const textOffset = options.textOffset ?? 12;
  const fontSize = options.fontSize || "10";

  // Left half: 0..3
  const leftWidth = totalWidth / 2;
  for (let i = 0; i < 4; i++) {
    const x = (i / 3) * leftWidth;
    const dot = document.createElementNS("http://www.w3.org/2000/svg", "circle");
    dot.setAttribute("cx", x);
    dot.setAttribute("cy", baselineY + dotOffset);
    dot.setAttribute("r", 2);
    dot.setAttribute("fill", "white");
    container.appendChild(dot);

    const text = document.createElementNS("http://www.w3.org/2000/svg", "text");
    text.setAttribute("x", x - 8);
    text.setAttribute("y", baselineY + textOffset);
    text.setAttribute("fill", "white");
    text.setAttribute("font-size", fontSize);
    text.textContent = i;
    container.appendChild(text);
  }

  // Right half: ∞ at the far right
  const dotRight = document.createElementNS("http://www.w3.org/2000/svg", "circle");
  dotRight.setAttribute("cx", totalWidth);
  dotRight.setAttribute("cy", baselineY + dotOffset);
  dotRight.setAttribute("r", 2);
  dotRight.setAttribute("fill", "white");
  container.appendChild(dotRight);

  const txtRight = document.createElementNS("http://www.w3.org/2000/svg", "text");
  txtRight.setAttribute("x", totalWidth - 10);
  txtRight.setAttribute("y", baselineY + textOffset);
  txtRight.setAttribute("fill", "white");
  txtRight.setAttribute("font-size", fontSize);
  txtRight.textContent = "∞";
  container.appendChild(txtRight);

  if (options.label) {
    const lbl = document.createElementNS("http://www.w3.org/2000/svg", "text");
    lbl.setAttribute("x", -40);
    lbl.setAttribute("y", baselineY + textOffset);
    lbl.setAttribute("fill", "white");
    lbl.setAttribute("font-size", fontSize);
    lbl.textContent = options.label;
  }
}

function scaleSMeter(value, returnPercent = true) {
  let s;
  if (value <= 120) {
    s = 9.0 * value / 120.0;
  }
  else if (value <= 241) {
    s = 9.0 + 6.0 * (value - 120) / 121.0;
  }
  else {
    s = 15.0;
  }
  p = (s / 15.0) * 100.0;

  console.log("scaleSMeter -> value = " + value + ", units = " + s + ", percent = " + p);

  if (returnPercent) {
    return p;
  } else {
    return s;
  }
}

function scaleP0(value) {
  if (value <= 143) {
    // 0% to 50%
    return (value / 143.0) * 50.0;
  } else if (value <= 213) {
    // 50% to 100%
    return 50.0 + ((value - 143) / 70.0) * 50.0;
  } else {
    // Over-range
    return 100.0;
  }
}

function hexToVoltage(value) {
  // Lower segment: 0x0000 - 0x0013 (0 - 19)
  if (value <= 19) {
    // Linear from OV to 10V
    return (10.0 / 19.0) * value;
  }
    // Upper segment: 0x0013 - 0x0241 (19 - 577)
  else if (value <= 577) {
    // Linear from 10V to 16V
    return 10.0 + (float) (value - 19) * (6.0 / (577.0 - 19.0));
  }
  else {
    // If out of range, clamp or return max voltage
    return 16.0;
  }
}

function scaleAmps(value) {
  if (value <= 151) {
    // 0 to 10 A
    return (value / 151.0) * 10.0;
  } else if (value <= 326) {
    // 10 to 15 A
    return 10.0 + ( (value - 151) / 175.0) * 5.0;
  } else if (value <= 577) {
    // 15 to 25 A
    return 15.0 + ((value - 326) / 251.0) * 10.0;
  } else {
    // Over-range
    return 25.0;
  }
}

function scaleSW(value) {
  if (value <= 72) {
    // SWR 1.0 - 1.5
    return 1.0 + (value / 72.0) * 0.5;
  } else if (value <= 128) {
    // SWR 1.5 - 2.0
    return 1.5 + ((value - 72) / 56.0) * 0.5;
  } else if (value <= 288) {
    // SWR 2.0 - 3.0
    return 2.0 + ( (value - 128) / 160.0) * 1.0;
  } else {
    // Over-range
    return 3.0;
  }
}

function scaleCOMP (value) {
  if (value <= 304) {
    // 0-15 dB segment
    return (value / 304.0) * 15.0;
  } else if (value <= 577) {
    // 15-30 dB segment
    return 15.0 + ( (value - 304) / 273.0) * 15.0;
  } else {
    //Over-range return
    return 30.0;
  }
}

function scaleALCpercent (value) {
  if (value <= 288) {
    return 100.0 * value / 288.0;
  } else {
    return 100.0;
  }
}

