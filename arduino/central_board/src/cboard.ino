/* Central Board Unit
   This script is a part of my home automation setup 
   Vasily Klenov, 2016

   to control pin state send mqtt message:
   home/control/cboard/pins/43, payload 0 or 1
*/

// mqtt
#include <SPI.h>
#include <Ethernet.h>
#include <PubSubClient.h>

// 1wire
#include <OneWire.h>
#include <DallasTemperature.h>

#include "advancedSerial.h"

#define SIZE(array) (sizeof(array) / sizeof(*array))


/* #################  1Wire                                                   */
// ar =[]; "1021715B020800AF".split(//).each_slice(2) { |e| ar+= [e.join] }; ar.map{ |e| "0x" + e }.join(', ')
#define TEMP_PUBLISH_DELAY 40000
uint32_t last_temp_read = 0;

#define TEMPERATURE_PRECISION   9
#define TEMP_MAX_FLOAT_LENGTH   5
#define TEMP_SENSOR_NAME_LENGTH 3

const uint8_t one_wire_pin_1 = 22;
const uint8_t one_wire_pin_2 = 23;

OneWire oneWire_0(one_wire_pin_1);
OneWire oneWire_1(one_wire_pin_2);

// Pass our oneWire reference to Dallas Temperature. 
DallasTemperature outside_temp(&oneWire_0);
DallasTemperature    room_temp(&oneWire_1);

struct TempSensor {
  DallasTemperature bus;
  DeviceAddress address;
  const char *name;
};

TempSensor room_low  = { room_temp, { 0x10, 0x13, 0xB0, 0x5B, 0x02, 0x08, 0x00, 0xC3 }, "RL" }; // !!! name can't be longer than TEMP_SENSOR_NAME_LENGTH - 1
TempSensor room_high = { room_temp, { 0x10, 0x57, 0x87, 0x5B, 0x02, 0x08, 0x00, 0xBF }, "RH" };

TempSensor out_1  = { outside_temp, { 0x10, 0x21, 0x71, 0x5B, 0x02, 0x08, 0x00, 0xAF }, "O1" };
TempSensor out_2  = { outside_temp, { 0x10, 0x2E, 0xA4, 0x5B, 0x02, 0x08, 0x00, 0x07 }, "O2" };
TempSensor chulan = { outside_temp, { 0x10, 0xB8, 0x9D, 0x5B, 0x02, 0x08, 0x00, 0x73 }, "CH" };

TempSensor temp_sensors[] = { room_low, room_high, out_1, out_2, chulan };


void setupTempSensors() {
  room_temp.begin();
  outside_temp.begin();

  if (room_temp.isParasitePowerMode())    aSerial.l(Level::vvv).pln("room_temp sensors parasite mode is ON");
  if (outside_temp.isParasitePowerMode()) aSerial.l(Level::vvv).pln("outside_temp sensors parasite mode is ON");

  // if (!room_temp.getAddress(room_low, 0)) aSerial.l(Level::v).println("Unable to find address for Device 0");
  // printAddress(room_low);

  // if (!room_temp.getAddress(room_high, 1)) aSerial.l(Level::v).println("Unable to find address for Device 1"); 
  // printAddress(room_high);

  // if (!outside_temp.getAddress(out1, 0)) aSerial.l(Level::v).println("Unable to find address for Device 0");
  // printAddress(out1);

  // if (!outside_temp.getAddress(out2, 1)) aSerial.l(Level::v).println("Unable to find address for Device 1"); 
  // printAddress(out2);

  // if (!outside_temp.getAddress(out3, 2)) aSerial.l(Level::v).println("Unable to find address for Device 2"); 
  // printAddress(out3);
  
  for (int i=0; i<SIZE(temp_sensors); i++) {
    temp_sensors[i].bus.setResolution( temp_sensors[i].address,  TEMPERATURE_PRECISION);
  }

}

// function to print a device address
void printAddress(DeviceAddress deviceAddress) {
  for (uint8_t i = 0; i < 8; i++) {
    // zero pad the address if necessary
    if (deviceAddress[i] < 16) aSerial.l(Level::v).p("0");
    aSerial.l(Level::v).p(deviceAddress[i], HEX);
  }
  aSerial.l(Level::v).pln("");
}

// function to print the temperature for a device
float getTemperature(DallasTemperature &bus, DeviceAddress deviceAddress) {
  return bus.getTempC(deviceAddress);
}

void tempSensorsLoop() {
  if(millis() - last_temp_read > TEMP_PUBLISH_DELAY) {
    aSerial.l(Level::vvv).pln("tempSensorsLoop");
    room_temp.requestTemperatures();
    outside_temp.requestTemperatures();

    publishTempSensors();

    last_temp_read = millis();
  }
}


/* #################  Ethernet                                                */

const uint8_t     cboard_mac[] = { 0xA9, 0x10, 0x14, 0xE7, 0xAC, 0xCC };
const IPAddress   cboard_ip(192, 168, 2, 15);

const IPAddress mqtt_server(192, 168, 2,  3);

EthernetClient ethClient;

/* #################  MQTT                                                    */
#define MQTT_RECONNECT_DELAY 5000
#define MQTT_PINS_TOPIC "home/control/cboard/pins/"
#define MQTT_INFO_TOPIC "home/info/cboard/pins/"
#define MQTT_TEMP_TOPIC "home/info/sensors/temp/"

#define MQTT_PINS_FORMAT "XX"

const char *mqtt_control_prefix = MQTT_PINS_TOPIC;
const char *mqtt_info_prefix    = MQTT_INFO_TOPIC;
const char *mqtt_temp_prefix    = MQTT_TEMP_TOPIC;

const char *mqtt_pins_format    = MQTT_PINS_FORMAT;

bool published = false;

struct MqttPin {
  int8_t pin_number;
  bool    pin_state;
};

MqttPin pin_40 = { 40, true };
MqttPin pin_42 = { 42, true };
MqttPin pin_43 = { 43, true };
MqttPin pin_44 = { 44, true };
MqttPin pin_45 = { 45, true };
MqttPin pin_46 = { 46, true };
MqttPin pin_47 = { 47, false }; // led strip

MqttPin mqtt_pins[] = { pin_40, pin_42, pin_43, pin_44, pin_45, pin_46, pin_47 };

PubSubClient client(ethClient);
long lastReconnectAttempt = 0;

boolean reconnect() {
  if (client.connect("centralBoard")) {
    // Once connected, publish an announcement...
    // client.publish("outTopic", "hello world");

    // ... and resubscribe
    client.subscribe( MQTT_PINS_TOPIC "+" ); // sic! '+' is a wildcard
  }

  return client.connected();
}


void publishPinState(int16_t pin_index) {
  aSerial.l(Level::v).pln("publishPinState");
  MqttPin mqtt_pin = mqtt_pins[pin_index];

  char char_buffer[ strlen(MQTT_INFO_TOPIC) + strlen(MQTT_PINS_FORMAT) ] = MQTT_INFO_TOPIC;

  char pin_as_str[3] = "";
  itoa(mqtt_pin.pin_number, pin_as_str, 10);
  aSerial.l(Level::vvvv).p("pin_as_str = ").pln(pin_as_str);

  strcat(char_buffer, pin_as_str);
  aSerial.l(Level::vvvv).p("char_buffer = ").pln(char_buffer);

  char payload[] = "0";
  if( mqtt_pin.pin_state )
    payload[0] = '1';

  boolean retained = true;
  client.publish(char_buffer, payload, retained);
  delay(20);
}

void publishPinStates() {
  for (int i=0; i<SIZE(mqtt_pins); i++) {
    publishPinState( i );
  }
}

void publishTemp(int16_t temp_index) {
  aSerial.l(Level::vvvv).p("publishTemp: ");

  char topic_plus_sensor_name[ strlen(MQTT_TEMP_TOPIC) + TEMP_SENSOR_NAME_LENGTH ] = "";
  char float_to_char_buffer[TEMP_MAX_FLOAT_LENGTH] = ""; // eg 25.9

  TempSensor t_sensor = temp_sensors[temp_index];

  float temp = getTemperature(t_sensor.bus, t_sensor.address);

  if( temp == DEVICE_DISCONNECTED_C ) {
    aSerial.l(Level::v).p("ERROR: Temp sensor error").pln(temp);
    return;
  }
  
  strcat(topic_plus_sensor_name, mqtt_temp_prefix);
  aSerial.l(Level::vvvv).p("topic_plus_sensor_name = ").p(topic_plus_sensor_name).p(" ");
  strcat(topic_plus_sensor_name, t_sensor.name);
  aSerial.l(Level::vvvv).pln(topic_plus_sensor_name);

  dtostrf(temp, 1, 1, float_to_char_buffer);
  aSerial.l(Level::vvvv).p("float_to_char_buffer = ").pln(float_to_char_buffer);

  client.publish(topic_plus_sensor_name, float_to_char_buffer);
  delay(20);
}

void publishTempSensors() {
  for (int i=0; i<SIZE(temp_sensors); i++) {
    publishTemp( i );
  }
}

bool equalsToPinsTopic(char* recieved_topic) {
  return strncmp( mqtt_control_prefix, recieved_topic, strlen(mqtt_control_prefix) ) == 0;
}

void mqttLoop() {
  if (!client.connected()) {
    uint32_t now = millis();
    if (now - lastReconnectAttempt > MQTT_RECONNECT_DELAY) {
      aSerial.l(Level::v).pln("Connecting to MQTT server...");
      lastReconnectAttempt = now;
      // Attempt to reconnect
      if (reconnect()) {
        lastReconnectAttempt = 0;

        if( !published ) {
          publishPinStates();
          published = true;
        }
      }
    }
  } else {
    // Client connected
    client.loop();
  }
}

int16_t getPinNumber(char first_digit, char second_digit) {
  char str[3] = "XX";
  str[0] = first_digit;
  str[1] = second_digit;

  return atoi(str);
}

int16_t find_pin_index(int16_t pin) {
   for (int i=0; i<SIZE(mqtt_pins); i++)
     if ( pin == mqtt_pins[i].pin_number )
        return(i);

   return(-1);
}

void changePinState(int16_t pin_number, bool new_state) {
  aSerial.l(Level::vvv).p("changePinState, pin = ").p(pin_number).p(" state = ").pln(new_state);

  int16_t pin_index = find_pin_index(pin_number);

  if(pin_index < 0)
    return;

  aSerial.l(Level::vvv).p("pin_state = ").p(mqtt_pins[pin_index].pin_state).p(" pin_index = ").pln(pin_index);

  if( mqtt_pins[pin_index].pin_state != new_state ) {
    mqtt_pins[pin_index].pin_state = new_state;
    digitalWrite(mqtt_pins[pin_index].pin_number, new_state);
    publishPinState(pin_index);
  }
}

void callback(char* recieved_topic, byte* payload, unsigned int length) {
  aSerial.l(Level::v).p("Message arrived : ").pln(recieved_topic);

  uint16_t recieved_topic_len = strlen(recieved_topic);
  uint16_t pins_topic_len     = strlen(mqtt_control_prefix);

  uint16_t len_diff = recieved_topic_len - pins_topic_len;

  if( len_diff !=  strlen(mqtt_pins_format) ) {
    aSerial.l(Level::v).pln("ERROR: incorrect mqtt topic lenght");
    return;
  }

  if( !equalsToPinsTopic(recieved_topic) ) {
    aSerial.l(Level::v).pln("ERROR: unknown mqtt topic");
    return;
  }

  char first_digit  = recieved_topic[recieved_topic_len - 2];
  char second_digit = recieved_topic[recieved_topic_len - 1];

  if( !isdigit(first_digit) || !isdigit(second_digit) ) {
    aSerial.l(Level::v).pln("ERROR: mailformed mqtt topic tail - NAN");
    return;
  }

  if( payload[0] != '0' && payload[0] != '1') {
    aSerial.l(Level::v).pln("ERROR: mailformed payload");
    return;
  }

  bool state = ( payload[0] == '1' );
  int16_t p_number = getPinNumber(first_digit, second_digit);
  changePinState(p_number, state);

}

void setupInputPins() {
  for (int i=0; i<SIZE(mqtt_pins); i++) {
    digitalWrite(mqtt_pins[i].pin_number, mqtt_pins[i].pin_state);
    pinMode(mqtt_pins[i].pin_number, OUTPUT);
  }
}

/* #################  SETUP                                                   */

void setup(void) {
  Serial.begin(9600);
  aSerial.setPrinter(Serial);
  aSerial.setFilter(Level::vvv);

  aSerial.l(Level::v).pln("Setup ...");

  setupTempSensors();
  setupInputPins();

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  Ethernet.begin((uint8_t*)cboard_mac, cboard_ip);
  delay(3000);
}


/* #################  LOOP                                                    */

void loop() {
  mqttLoop();
  tempSensorsLoop();
}
