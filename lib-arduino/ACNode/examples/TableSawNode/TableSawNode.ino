/*
      Copyright 2015-2018 Dirk-Willem van Gulik <dirkx@webweaving.org>
                          Stichting Makerspace Leiden, the Netherlands.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, softwareM
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF
   ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
// Wiring of Power Node v.1.1
//
#include <PowerNodeV11.h>
#include <ACNode.h>
#include <RFID.h>   // SPI version

#include <CurrentTransformer.h>     // https://github.com/dirkx/CurrentTransformer
#include <ButtonDebounce.h>         // https://github.com/craftmetrics/esp32-button

// Triac -- switches the DeWalt
// Relay -- switches the table saw itself.

#define MACHINE             "tablesaw"
#define OFF_BUTTON          (SW2_BUTTON)
#define MAX_IDLE_TIME       (35 * 60 * 1000) // auto power off after 35 minutes of no use.
#define EXTRACTION_EXTRA    (20 * 1000) // 20 seconds extra time on the fan.
#define CURRENT_THRESHHOLD  (0.01)

//#define OTA_PASSWD          "SomethingSecrit"

CurrentTransformer currentSensor = CurrentTransformer(CURRENT_GPIO);

#include <ACNode.h>
#include <RFID.h>   // SPI version

// ACNode node = ACNode(MACHINE, WIFI_NETWORK, WIFI_PASSWD); // wireless, fixed wifi network.
// ACNode node = ACNode(MACHINE, false); // wireless; captive portal for configure.
// ACNode node = ACNode(MACHINE, true); // wired network (default).
ACNode node = ACNode(MACHINE);

// RFID reader = RFID(RFID_SELECT_PIN, RFID_RESET_PIN, -1, RFID_CLK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN); //polling
// RFID reader = RFID(RFID_SELECT_PIN, RFID_RESET_PIN, RFID_IRQ_PIN, RFID_CLK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN); //iRQ
RFID reader = RFID();

#ifdef OTA_PASSWD
OTA ota = OTA(OTA_PASSWD);
#endif

LED aartLed = LED();    // defaults to the aartLed - otherwise specify a GPIO.

ButtonDebounce offButton(SW2_BUTTON, 150 /* mSeconds */);

MqttLogStream mqttlogStream = MqttLogStream();

typedef enum {
  BOOTING, OUTOFORDER,      // device not functional.
  REBOOT,                   // forcefull reboot
  TRANSIENTERROR,           // hopefully goes away level error
  NOCONN,                   // sort of fairly hopless (though we can cache RFIDs!)
  WAITINGFORCARD,           // waiting for card.
  CHECKINGCARD,
  REJECTED,
  POWERED,                  // this is where we engage the relay.
  EXTRACTION,               // run the extractor fan a bit longer.
  RUNNING,                  // this is when we detect a current.
} machinestates_t;

#define NEVER (0)

struct {
  const char * label;                   // name of this state
  LED::led_state_t ledState;            // flashing pattern for the aartLED. Zie ook https://wiki.makerspaceleiden.nl/mediawiki/index.php/Powernode_1.1.
  time_t maxTimeInMilliSeconds;         // how long we can stay in this state before we timeout.
  machinestates_t failStateOnTimeout;   // what state we transition to on timeout.
  unsigned long timeInState;
  unsigned long timeoutTransitions;
} state[RUNNING + 1] =
{
  { "Booting",              LED::LED_ERROR,           120 * 1000, REBOOT },
  { "Out of order",         LED::LED_ERROR,           120 * 1000, REBOOT },
  { "Rebooting",            LED::LED_ERROR,           120 * 1000, REBOOT },
  { "Transient Error",      LED::LED_ERROR,             5 * 1000, WAITINGFORCARD },
  { "No network",           LED::LED_FLASH,                NEVER, NOCONN },           // should we reboot at some point ?
  { "Waiting for card",     LED::LED_IDLE,                 NEVER, WAITINGFORCARD },
  { "Checking card",        LED::LED_PENDING,           5 * 1000, WAITINGFORCARD },
  { "Rejecting noise/card", LED::LED_ERROR,             5 * 1000, WAITINGFORCARD },
  { "Powered - but idle",   LED::LED_ON,           MAX_IDLE_TIME, WAITINGFORCARD },
  { "Powered + extracton",  LED::LED_ON,        EXTRACTION_EXTRA, WAITINGFORCARD },
  { "Running",              LED::LED_ON,                   NEVER, WAITINGFORCARD },
};

unsigned long laststatechange = 0;
static machinestates_t laststate = BOOTING;
machinestates_t machinestate = BOOTING;

unsigned long bad_poweroff = 0;

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n\n");
  Serial.println("Booted: " __FILE__ " " __DATE__ " " __TIME__ );

  // Init the hardware and get it into a safe state.
  //
  pinMode(RELAY_GPIO, OUTPUT);
  digitalWrite(RELAY_GPIO, 0);

  pinMode(CURRENT_GPIO, INPUT); // analog input.
  pinMode(SW1_BUTTON, INPUT_PULLUP);
  pinMode(SW2_BUTTON, INPUT_PULLUP);

  Serial.printf("Boot state: SW1:%d SW2:%d\n",
                digitalRead(SW1_BUTTON), digitalRead(SW2_BUTTON));

  // the default is space.makerspaceleiden.nl, prefix test
  // node.set_mqtt_host("mymqtt-server.athome.nl");
  // node.set_mqtt_prefix("test-1234");

  // specify this when using your own `master'.
  //
  node.set_master("test-master");

  node.set_report_period(10 * 1000);

  node.onConnect([]() {
    machinestate = WAITINGFORCARD;
  });
  node.onDisconnect([]() {
    machinestate = NOCONN;
  });
  node.onError([](acnode_error_t err) {
    Log.printf("Error %d\n", err);
    machinestate = TRANSIENTERROR;
  });
  node.onApproval([](const char * machine) {
    machinestate = POWERED;
  });
  node.onDenied([](const char * machine) {
    machinestate = REJECTED;
  });

  reader.onSwipe([](const char * tag) -> ACBase::cmd_result_t  {
    // avoid swithing off a machine unless we have to.
    //
    if (machinestate < POWERED)
      machinestate = CHECKINGCARD;

    // We'r declining so that the core library handle sending
    // an approval request, keep state, and so on.
    //
    return ACBase::CMD_DECLINE;
  });

  currentSensor.setOnLimit(CURRENT_THRESHHOLD);

  currentSensor.onCurrentOn([](void) {
    if (machinestate != RUNNING) {
      machinestate = RUNNING;
      Log.println("Motor started");
    };

    if (machinestate < POWERED) {
      static unsigned long last = 0;
      if (millis() - last > 1000)
        Log.println("Very strange - current observed while we are 'off'. Should not happen.");
    }
  });

  currentSensor.onCurrentOff([](void) {
    // We let the auto-power off on timeout do its work.
    if (machinestate > POWERED) {
      machinestate = EXTRACTION;
      Log.println("Motor stopped, extractor fan still on");
    };

  });

  offButton.setCallback([](int buttonState) {
    Debug.printf("Button 2 %s\n", buttonState == LOW ? "Pressed" : "Released again");
    if (buttonState != LOW)
      return;

    if (machinestate < POWERED) {
      // also force extractor fan off.
      if (machinestate == EXTRACTION)
        machinestate = WAITINGFORCARD;
      return;
    };

    if (machinestate == RUNNING) {
      Log.printf("Machine switched off with button while running (bad!)\n");
      bad_poweroff++;
    } else if (machinestate == POWERED) {
      Log.printf("Machine switched OFF with the off-button.\n");;
    } else {
      Log.printf("Off button pressed (currently in state %s). Weird.\n",
                 state[machinestate].label);
    }
    machinestate = WAITINGFORCARD;
  });

  node.onReport([](JsonObject  & report) {
    report["state"] = state[machinestate].label;

    report["powered_time"] = state[POWERED].timeInState + state[RUNNING].timeInState +  state[EXTRACTION].timeInState
                             + ((machinestate >= POWERED) ? ((millis() - laststatechange) / 1000) : 0);

    report["extract_time"] = state[RUNNING].timeInState +  state[EXTRACTION].timeInState
                             + ((machinestate >= EXTRACTION) ? ((millis() - laststatechange) / 1000) : 0);

    report["running_time"] = state[RUNNING].timeInState
                             + ((machinestate >= RUNNING) ? ((millis() - laststatechange) / 1000) : 0);

    report["idle_poweroff"] = state[POWERED].timeoutTransitions;
    report["fan_poweroff"] = state[EXTRACTION].timeoutTransitions;

    report["bad_poweroff"] = bad_poweroff;

    report["current"] = currentSensor.sd();

#ifdef OTA_PASSWD
    report["ota"] = true;
#else
    report["ota"] = false;
#endif
  });

  // This reports things such as FW version of the card; which can 'wedge' it. So we
  // disable it unless we absolutely positively need that information.
  //
  reader.set_debug(false);

  node.addHandler(&reader);
  // default syslog port and destination (gateway address or broadcast address).
  //

  // General normal log goes to MQTT and Syslog (UDP).
  Log.addPrintStream(std::make_shared<MqttLogStream>(mqttlogStream));

#ifdef OTA_PASSWD
  node.addHandler(&ota);
#endif

  // node.set_debug(true);
  // node.set_debugAlive(true);
  node.begin();
  Log.println("Booted: " __FILE__ " " __DATE__ " " __TIME__ );
}

void loop() {
  node.loop();
  offButton.update();

  if (state[machinestate].maxTimeInMilliSeconds != NEVER &&
      (millis() - laststatechange > state[machinestate].maxTimeInMilliSeconds))
  {
    state[machinestate].timeoutTransitions++;

    laststate = machinestate;
    machinestate = state[machinestate].failStateOnTimeout;

    Debug.printf("Time-out; transition from %s to %s\n",
                 state[laststate].label, state[machinestate].label);
  };

  if (laststate != machinestate) {
    Debug.printf("Changed from state <%s> to state <%s>\n",
                 state[laststate].label, state[machinestate].label);

    state[laststate].timeInState += (millis() - laststatechange) / 1000;
    laststate = machinestate;
    laststatechange = millis();
  }

  digitalWrite(RELAY_GPIO,  (laststate >= POWERED) ? 1 : 0);
  digitalWrite(TRIAC_GPIO,  (laststate >= EXTRACTION) ? 1 : 0);

  aartLed.set(state[machinestate].ledState);

  switch (machinestate) {
    case WAITINGFORCARD:
      break;

    case REBOOT:
      node.delayedReboot();
      break;

    case CHECKINGCARD:
      break;

    case POWERED:
      break;

    case EXTRACTION:
      break;

    case RUNNING:
      break;

    case REJECTED:
      break;

    case TRANSIENTERROR:
      break;
    case OUTOFORDER:
    case NOCONN:
    case BOOTING:
      break;
  };
}