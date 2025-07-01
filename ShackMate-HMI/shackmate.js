/*
Declaring enums
*/

const Radio_Type = {
  IC_7300: "IC-7300",
  IC_9700: "IC-9700"
};


class SMPolling {
  constructor(pollingInterval = 50) {
    this._arrObjPoll = [];
    this._pollingInterval = pollingInterval;
    this.lastPolled = new Date();
    this.eventTarget = new EventTarget();
    this._lastIndex = -1;
  }

  get interval() {return this._pollingInterval;}
  set interval(x) {this._pollingInterval = x;}

  add(objPoll) {
    if (this._arrObjPoll.length == 0) {
      this._arrObjPoll = [objPoll];
    } else {
      this._arrObjPoll.push(objPoll)
    }
  }

  sleep(ms) {

    return new Promise(resolve => setTimeout(resolve, ms));
  }

  addEventListener(eventName, callback) {

    this.eventTarget.addEventListener(eventName, callback);
  }

  removeEventListener(eventName, callback) {

    this.eventTarget.removeEventListener(eventName, callback);
  }

  dispatchEvent(eventName, detail) {
    const event = new CustomEvent(eventName, { detail: detail });
    //document.dispatchEvent(event);
    this.eventTarget.dispatchEvent(event);
  }

  start() {
    setInterval( () => {
      let pollRuntime = new Date();

      let boolPoll = true;

      this._arrObjPoll.forEach(async (poll) => {
        if (poll.lastPolled.valueOf() + poll.interval < pollRuntime.valueOf()) {
          if (boolPoll) {
            this._lastPolled = new Date();
            poll.execute();
            //boolPoll = false;
          }
        }
      });
    }, this._pollingInterval);
  }

  stop() {

  }
}

class SMPoll {
  constructor(id, strPoll, interval) {
    this._id = id;
    this._poll = strPoll;
    this._pollingInterval = interval;
    this._lastPolled = new Date();
  }

  get id() {return this._id;}
  set id(x) {this._id = x;}
  get interval() {return this._pollingInterval;}
  set interval(x) {this._pollingInterval = x;}
  get lastPolled() {return this._lastPolled;}
  set lastPolled(x) {this._lastPolled = x;} 
  get value() {return this._poll;}
  set value(x) {this._poll = x;} 

  execute() {
    this._lastPolled = new Date();
  }
}

class SMPollRadio extends SMPoll {
  constructor(id, strPoll, address, objRadio, interval) {
    super(id, strPoll, interval);
    this._address = address;
    this._objRadio = objRadio;
  }

  execute() {
    console.log("executing poll: " + this._poll);
    this._objRadio.sendCommand(this._poll);
    // add event to notify poll was executed...
    super.execute();
  }

  get address() {return this._address;}
  set address(x) {this._address = x;}
  get rig() {return this._objSMDevice;}
  set rig(x) {this._objSMDevice = x;}
}

class SMDevice { /* Base Object - Shack Mate Device */
  constructor(url, displayAddress = 'EE') {
    console.log("shackmate.js::SMDevice => NEW DEVICE CREATED!");
    this.url = url;
    this._socket = undefined;
    this.eventTarget = new EventTarget();
    this._arrSend = [];
    this._lastSend = new Date();
    this._minimumDuplicateInterval = 100;
    this._minimumSendInterval = 100;
    this._lastResponseDt = new Date(0);
    this._displayAddress = displayAddress;
    this._lastMessage = "";

    this.connectWebSocket();
  }

  get minimumSendInterval() {return this._minimumSendInterval;}
  set minimumSendInterval(x) {this._minimumSendInterval = x;}
  get displayAddress() {return this._displayAddress;}
  set displayAddress(x) {this._displayAddress = x;}

  connectWebSocket() {
    this._socket = new WebSocket(this.url);

    this._socket.onopen = () => {
      console.log("shackmate.js::SMDevice -> WebSocket connection opened to " + this.url);
      this.dispatchEvent("ready", "device is ready");
    };

    this._socket.onmessage = (event) => {
      const previousResponse = this._lastResponseDt;
      this._lastResponseDt = new Date();
      const message = event.data.toUpperCase().trim();
      const bytes = message.split(" ");
      if (bytes[2] == this._displayAddress || bytes[2] == "00") {
        if (message == this._lastMessage 
          && previousResponse.valueOf() + this._minimumDuplicateInterval > this._lastResponseDt.valueOf()) {
          //console.log("shackmate.js::SMDevice -> Received Duplicate: " + message);
        }
        else {
          console.log("shackmate.js::SMDevice -> Received: " + message);

          const decodedResponse = this.decodeResponse(message);

          if (decodedResponse.validMessage) {
            if (decodedResponse.bytesFrom[0] != this._displayAddress) {
              this.dispatchEvent(decodedResponse.bytesFrom[0], decodedResponse);
            }
          }
        }
        this._lastMessage = message;
      }
    };

    this._socket.onerror = function(error) {
      console.error("shackmate.js::SMDevice -> WebSocket error:", error);
    };

    this._socket.onclose = () => {
      console.log("shackmate.js::SMDevice -> WebSocket connection closed. Attempting to reconnect in 3 seconds...");
      setTimeout(() => {
        this.connectWebSocket();
      }, 2000);
    };
  }

  send(data) {
    // Only send if the socket is in the OPEN state.
    if (this._socket && this._socket.readyState === WebSocket.OPEN) {

      let d = new Date();
      if (this._lastSend.valueOf() + this._minimumSendInterval < d.valueOf()) {
        console.log("SMDevice::send() -> Sending: ", data);
        this._socket.send(data);
        this._lastSend = new Date();
      } else {
        console.log("SMDevice::send() -> Queueing outbound message: ", data);
        if (this._arrSend.length == 0) {
          this._arrSend = [data];
        } else {
          this._arrSend.push(data)
        }
        setTimeout(() => {this.sendFromQueue()}, this._minimumSendInterval);
      }
    } else {
      console.error("SMDevice::send() -> WebSocket is not open. Current state:"
                   , this.socket ? this.socket.readyState : "No socket");
    }
  }

  sendFromQueue() {
    // Only send if the socket is in the OPEN state.
    if (this._socket && this._socket.readyState === WebSocket.OPEN) {

      let d = new Date();
      if (this._lastSend.valueOf() + this._minimumSendInterval < d.valueOf()) {
        if (this._arrSend.length > 0) {
          let data = this._arrSend.shift();
          console.log("SMDevice::sendFromQueue() -> Sending: ", data);
          this._socket.send(data);
          this._lastSend = new Date();
        } else {
          console.log("SMDevice::sendFromQueue() -> No messages are pending");
        }
      } else {
        if (this._arrSend.length > 0) {
          let data = this._arrSend[0];
          console.log("SMDevice::sendFromQueue() -> Waiting to send queued outbound message: ", data);
          setTimeout(() => {this.sendFromQueue()}, this._minimumSendInterval);
        } else {
          console.log("SMDevice::sendFromQueue() -> No messages are pending");
        }
      }
    } else {
      console.error("SMDevice::sendFromQueue() -> WebSocket is not open. Current state:"
                   , this.socket ? this.socket.readyState : "No socket");
      setTimeout(() => {this.sendFromQueue()}, this._minimumSendInterval);
    }
  }

  addEventListener(eventName, callback) {

    this.eventTarget.addEventListener(eventName, callback);
  }

  removeEventListener(eventName, callback) {

    this.eventTarget.removeEventListener(eventName, callback);
  }

  dispatchEvent(eventName, detail) {
    const event = new CustomEvent(eventName, { detail: detail });
    //document.dispatchEvent(event);
    this.eventTarget.dispatchEvent(event);
  }

  decodeResponse(message) {
    let bytesPreamble = [];
    let bytesTo = [];
    let bytesFrom = [];
    let bytesOK = [];
    let bytesCommandNumber = [];
    let bytesSubCommandNumber = [];
    let bytesData = [];
    let bytesEndOfMessage = [];

    message = message.toUpperCase();
    message = message.trim();
    const bytes = message.split(" ");
    if (bytes.length >= 6) {
      //Valid to decode...
      bytesPreamble.push(bytes[0]);
      bytesPreamble.push(bytes[1]);
      
      bytesTo.push(bytes[2]);
      bytesFrom.push(bytes[3]);

      if (bytes.length === 6) {
        //  OK / NG Response
        bytesOK.push(bytes[4]);

      } else if (bytes.length > 6) {
        // Command Response
        bytesCommandNumber.push(bytes[4]);

        let intDataStart = 5;
        if (this.expectSubcommand(bytesCommandNumber[0])) {
          bytesSubCommandNumber.push(bytes[5]);
          intDataStart = 6;
        }

        for (let i = intDataStart; i < bytes.length - 1; i++) {
          bytesData.push(bytes[i]);
        }

      }

      bytesEndOfMessage.push(bytes[bytes.length - 1]);

      /*
      console.log("bytesPreamble: ", bytesPreamble);
      console.log("bytesTo: ", bytesTo);
      console.log("bytesFrom: ", bytesFrom);
      console.log("bytesCommandNumber: ", bytesCommandNumber);
      console.log("bytesSubCommandNumber: ", bytesSubCommandNumber);
      console.log("bytesData: ", bytesData);
      console.log("bytesOK: ", bytesOK);
      console.log("bytesEndOfMessage: ", bytesEndOfMessage);
      */

      return {
        validMessage: true,
        bytesPreamble: bytesPreamble,
        bytesTo: bytesTo,
        bytesFrom: bytesFrom,
        bytesCommandNumber: bytesCommandNumber,
        bytesSubCommandNumber: bytesSubCommandNumber,
        bytesData: bytesData,
        bytesOK: bytesOK,
        bytesEndOfMessage: bytesEndOfMessage
      };
    }
    else {
      // If the message doesn't match our expected format, return it as is.
      return {
        validMessage: false,
        unhandledResponse: message
      };/* Function to decode the received command. */
    }
  }

  expectSubcommand(command) {
    let boolReturn = false;

    switch(command) {
      case "07":
      case "0E":
      case "13":
      case "14":
      case "15":
      case "16":
      case "18":
      case "19":
      case "1A":
      case "1B":
      case "1C":
      case "1E":
      case "21":
      case "27":
      case "28":
        boolReturn = true;
    }

    return boolReturn;
  }
}

class Radio {
  constructor(objSMDevice) {
    this._objSMDevice = objSMDevice;
    this._activeRig = "";
    this._powerOn = false;
    this._poweringState = "unknown"; // Possible Values:  unknown, on, off, turning_on, turning_off
    this._lastResponseDt = new Date(0);

    // Global operate mode variables
    this._operateMode = "VFO"; // "VFO" or "MEM"
    this._memoryCH = "0001";   // default memory channel
    this._vfoToggle = false;   // false => side A, true => side B
    this._toneValue = "00";    // "00" => no TONE, "11" => DUP-, "12" => DUP+
    this._mode = "N/A";
    this._filter = "N/A";
  }

  initialize() { /* Send commmands to device to get intial settings */
  }
}

class RadioIcom extends Radio {
  modeList = ["00", "01", "02", "03", "04", "05", "07", "08"];
  filterList = ["01", "02", "03"];

  get address() {return this._address;}
  set address(x) {this._address = x;}

  constructor(objSMDevice, address) {
    super(objSMDevice);
    this._address = address;
    this._displayAddress = objSMDevice.displayAddress;

    this._currentModeIndex = 0;
    this._currentFilterIndex = 0;
    this._currentSplitValue = "00";
    this._subBandVisible = false;
    this._subBandHideTimer = null;
    this._dataMode = 0;
    this._splitMode = 0;

    this._objSMDevice.addEventListener('ready', (event) =>{
      console.log("shackmate.js::RadioIcom -> Heard this._objSMDevice ready event,  initializing radio...");
      this.initialize();
    });

    this._objSMDevice.addEventListener(this.address, (event) =>{
      console.log("shackmate.js::RadioIcom -> Heard Event " + this.address);

      const decodedResponse = event.detail;
      if (decodedResponse.validMessage != undefined) {
        this.handleResponse(decodedResponse);            
      }
    });
  }

  sendCommand(command, boolAddFE = false) {
    let a = this._address;
    let b = this._displayAddress;
    let c = command;

    let fe = "FE FE";
    if (boolAddFE) {
      for (let i = 0; i < 24; i++) {
        fe += " FE"
      }
    }


    if (!c) return;
    let data = (`${fe} ${a} ${b} ${c} FD`).replace(/\s+/g, " ").trim();
    if (this._objSMDevice._socket.readyState == WebSocket.OPEN) {
      let d = new Date();
      if (this._objSMDevice._lastSend.valueOf() + this._minimumSendInterval < d.valueOf()) {
        console.log("RadioIcom::sendCommand() -> Sending: ", data);
        this._objSMDevice._socket.send(data);
        this._objSMDevice._lastSend = new Date();
      } else {
        console.log("RadioIcom::sendCommand() -> Queueing outbound message: ", data);
        if (this._objSMDevice._arrSend.length == 0) {
          this._objSMDevice._arrSend = [data];
        } else {
          // Make sure the message is not already in the queue
          let boolContinue = true;
          for (let x in this._objSMDevice._arrSend) {
            if (x.value == command) {
              boolContinue = false;
              break;
            } 
          }
          if (boolContinue) {
            this._objSMDevice._arrSend.push(data);
          }
        }
        setTimeout(() => {this._objSMDevice.sendFromQueue()}, this._objSMDevice._minimumSendInterval);
      }
    }
  }

  powerOn(boolPowerOn = true) {
    let command = "";

    if (boolPowerOn) {
      command = ("18 01");
      this._poweringState = "turning_on";
      setTimeout(() => {this.initialize()}, 5000)
    } else {
      command = ("18 00");
      this._poweringState = "turning_off";
      setTimeout(() => {
        var e = new CustomEvent("rig_event", {detail: {rig: this, value: "power", power: false}});
        document.dispatchEvent(e);
      }, 2000);
    }

    console.log(`shackmate.js::RadioIcom.powerOn(${boolPowerOn})`);
    this.sendCommand(command, boolPowerOn);

    //arrRadio[rig_activeArrRadioIndex]._powerOn = powerOn;
  }

  sendFrequencyQuery() {

    this.sendCommand('03');
  }

  send0FCommand() {
    let a = this._address;
    let b = this._displayAddress;
    let msg = (`FE FE ${a} ${b} 0F FD`).replace(/\s+/g, " ").trim();
    this._objSMDevice.send(msg);
  }

  sendDefaultMode() {
    let a = this._address;
    let b = this._displayAddress;
    let msg = (`FE FE ${a} ${b} 04 FD`).replace(/\s+/g, " ").trim();
    this._objSMDevice.send(msg);
  }

  static getFrequencyFromHex(bytesData) {
    let frequency = "";
    for (let i = bytesData.length - 1; i >= 0; i--) {
      frequency += bytesData[i];
    }
    return frequency;
  }

  handleResponse(decodedResponse) {
    /*
    console.log("bytesPreamble: ", bytesPreamble);
    console.log("bytesTo: ", bytesTo);
    console.log("bytesFrom: ", bytesFrom);
    console.log("bytesCommandNumber: ", bytesCommandNumber);
    console.log("bytesSubCommandNumber: ", bytesSubCommandNumber);
    console.log("bytesData: ", bytesData);
    console.log("bytesEndOfMessage: ", bytesEndOfMessage);
    */


    if ( (decodedResponse.bytesTo[0] == this._displayAddress 
            || decodedResponse.bytesTo[0] == "00")
         && decodedResponse.bytesFrom[0] == this._address) {

      // Set power on indicator...
      if (decodedResponse.bytesCommandNumber.length == 0 && decodedResponse.bytesOK.length > 0) {
        if ((this._poweringState == "turning_on" || this._poweringState == "turning_off")
            && decodedResponse.bytesOK[0] == "FB") {
          if (this._poweringState == "turning_on") {
            this._poweringState = "on";
            this._powerOn = true;
          } 
          else {
            this._poweringState = "off";
            this._powerOn = false;
          }
          var e = new CustomEvent("rig_event", {detail: {rig: this, value: "power", power: this._powerOn}});
          document.dispatchEvent(e);
        }
        else {
          if (decodedResponse.bytesOK[0] != "FA" &&
                (
                   this._poweringState == "off"
                || this._poweringState == "unknown"
                )
             ) {
            this._poweringState = "on";
            this._powerOn = true;
            var e = new CustomEvent("rig_event", {detail: {rig: this, value: "power", power: this._powerOn}});
            document.dispatchEvent(e);
          }
        }
        this._lastResponseDt = new Date();
      }
      else {
        if (this._poweringState == "turning_on") {
          this._poweringState = "on";
          this._powerOn = true;
          var e = new CustomEvent("rig_event", {detail: {rig: this, value: "power", power: this._powerOn}});
          document.dispatchEvent(e);
        }
      }


      // Decide what to do based on response...
      switch (decodedResponse.bytesCommandNumber[0]) {
        case "00":
        case "03":
          let t = decodedResponse.bytesData;
          let freq = rig_decodeFrequencyReversedBCD(t[0], t[1], t[2], t[3], t[4]);
          var e = new CustomEvent("rig_event", {detail: {rig: this, value: "freq", freq: freq}});
          document.dispatchEvent(e);
          break;

        case "01":
        case "04":
          let mode = "";
          for (let i = 0; i < this.modeList.length; i ++) {
            if (this.modeList[i] == decodedResponse.bytesData[0]) {
              this._currentModeIndex = i;
              break;
            }
          }
          switch (decodedResponse.bytesData[0]) {
            case "00":
              mode = "LSB";
              break;
            case "01":
              mode = "USB";
              break;
            case "02":
              mode = "AM";
              break;
            case "03":
              mode = "CW";
              break;
            case "04":
              mode = "RTTY";
              break;
            case "05":
              mode = "FM";
              break;
            case "07":
              mode = "CW-R";
              break;
            case "08":
              mode = "RTTY-R";
              break;
          }
          
          this._mode = mode;
          var e = new CustomEvent("rig_event", {detail: {rig: this, value: "mode", mode: mode}});
          document.dispatchEvent(e);


          let filter = "";
          switch (decodedResponse.bytesData[1]) {
            case "01":
              filter = "FIL1";
              break;
            case "02":
              filter = "FIL2";
              break;
            case "03":
              filter = "FIL3";
              break;
          }
          this._filter = filter;
          //var e = new CustomEvent("rig_event", {detail: {rig: this, value: "filter", filter: `${filter}`}});
          var e = new CustomEvent("rig_event", {detail: {rig: this, value: "filter", filter: filter}});
          document.dispatchEvent(e);
          break;

        case "0F":
          let split = decodedResponse.bytesData[0];
          if (split == '01') {
            this._splitMode = 1;
          }
          else {
            this._splitMode = 0;
          }
          var e = new CustomEvent("rig_event", {detail: {rig: this, value: "split", split: split}});
          document.dispatchEvent(e);
          break;

        case "15":
          rig_updateSegmentedMeter("rig_spoMeterGroup"
                                  , scaleSMeter(decodedResponse.bytesData[0] * 100 
                                  + decodedResponse.bytesData[1] * 1, true));

          // Rig is on so update this for the Power Indicator
          this._lastResponseDt = new Date();

          break;

        case "1A":
          switch(decodedResponse.bytesSubCommandNumber[0]) {
            case "01":
              //bData:['03','01','00','40','07','07','00','01','01','10','00','08','85','00','08','85']
              bandIndex = Number(decodedResponse.bytesData[0]) - 1;
              stackIndex = Number(decodedResponse.bytesData[1]) - 1;
              freq = rig_decodeFrequencyReversedBCD(decodedResponse.bytesData[2]
                                                  , decodedResponse.bytesData[3]
                                                  , decodedResponse.bytesData[4]
                                                  , decodedResponse.bytesData[5]
                                                  , decodedResponse.bytesData[6]);
              switch (decodedResponse.bytesTransciever[0]) {
                case "94":
                  SMRig.bandStackingList7300[bandIndex][stackIndex] = freq;
                  break;
                case "A2":
                  SMRig.bandStackingList9700[bandIndex][stackIndex] = freq;
                  break;
              }
              break;
            case "06":
              let dataMode = ""
              switch (decodedResponse.bytesData[0]) {
                case "00":
                  dataMode = "";
                  this._dataMode = 0;
                  break;
                case "01":
                  dataMode = "-D";
                  this._dataMode = 1;
                  break;
              }
    
              var e = new CustomEvent("rig_event", {detail: {rig: this, value: "dataMode", dataMode: this._dataMode}});
              document.dispatchEvent(e);

              let filter = "";
              switch (decodedResponse.bytesData[1]) {
                case "01":
                  filter = "FIL1";
                  break;
                case "02":
                  filter = "FIL2";
                  break;
                case "03":
                  filter = "FIL3";
                  break;
              }
              if (filter != "") {
                this._filter = filter;
                var e = new CustomEvent("rig_event", {detail: {rig: this, value: "filter", filter: filter}});
                document.dispatchEvent(e);
              }
              break;
          }
          break;

        case "1C":
          switch(decodedResponse.bytesSubCommandNumber[0]) {
            case "00":  //  TX/RX
              switch (decodedResponse.bytesData[0]) {
                case "00":
                  // RX mode
                  var e = new CustomEvent("rig_event", {detail: {rig: this, value: "RX"}});
                  document.dispatchEvent(e);
                  break;
                case "01":
                  // TX mode
                  var e = new CustomEvent("rig_event", {detail: {rig: this, value: "TX"}});
                  document.dispatchEvent(e);
                  break;
              }
              break;

            case "01":  //  Ant Tuner on/off
            
              break;
          }
          break;
      }
    }
  }

  initialize() { /* Send commmands to device to get intial settings */
    console.log("RadioIcom::initialize() executing...");
    this.sendCommand("03");
    
    console.log("RadioIcom::initialize() sending 04...");
    this.sendCommand("04");
    
    console.log("RadioIcom::initialize() sending 1A 06...");
    this.sendCommand("1A 06");

    console.log("RadioIcom::initialize() sending 0F...");
    this.sendCommand("0F");

    console.log("RadioIcom::initialize() sending 07...");
    this.sendCommand("07 00");
  }


}

class RadioIcom7300 extends RadioIcom {
  static bandStackingList = [["1.800.000","1.800.000","1.800.000"]
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

  constructor(objSMDevice, port, displayAddress) {
    super(objSMDevice, port, displayAddress);

    this._type = Radio_Type.IC_7300;

    // Global operate mode variables
    this._operateMode = "VFO"; // "VFO" or "MEM"
    this._memoryCH = "0001";   // default memory channel
    this._vfoToggle = false;   // false => side A, true => side B
    this._toneValue = "00";    // "00" => no TONE, "11" => DUP-, "12" => DUP+
  }


}

class RadioIcom9700 extends RadioIcom {
  static modeList = ["00", "01", "02", "03", "04", "05", "07", "08", "17", "22"];
  static bandStackingList = [["144.000.000","144.000.000","144.000.000"]
                             ,["430.000.000","430.000.000","430.000.000"]
                             ,["1240.000.000","1240.000.000","1240.000.000"]
                             ];

  constructor(objSMDevice, port, displayAddress) {
    super(objSMDevice, port, displayAddress);

    this._type = Radio_Type.IC_9700;

    // Global operate mode variables
    this._operateMode = "VFO"; // "VFO" or "MEM"
    this._memoryCH = "0001";   // default memory channel
    this._vfoToggle = false;   // false => side A, true => side B
    this._toneValue = "00";    // "00" => no TONE, "11" => DUP-, "12" => DUP+
  }

}

class SMRotor extends SMDevice {
  constructor(name, type, url) {
    super(url);
    this.name = name;
    this.type = type;
  }
}

class SMAntenna extends SMDevice {
  constructor(name, type, url) {
    super(url);
    this.name = name;
    this.type = type;
  }
}

class SMSwitch extends SMDevice {
  constructor(name, type, url) {
    super(url);
    this.name = name;
    this.type = type;
  }
}

