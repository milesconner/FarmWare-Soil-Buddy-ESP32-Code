
#include <Arduino.h>

//libraries for the temperature sensor
#include <OneWire.h>
#include <DallasTemperature.h>

//libraries for the moisture sensor
#include <Adafruit_seesaw.h>

//libraries for WiFi and MQTT server
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

//temperature sensor is connected to GPIO 4
#define temperature_sensor_pin 4

//how long the Soil Buddy should sleep for by default (in mins)
#define default_sleep_period 1

//temperature sensor object
OneWire one_wire(temperature_sensor_pin);
DallasTemperature temperature_sensor(&one_wire);

//moisture sensor object
Adafruit_seesaw moisture_sensor;
const uint8_t moisture_sensor_I2C_address = 0x36;

//WiFi credentials (only one should be uncommented)

//Miles' apartment
const char* wifi_ssid = "Heritage Apartments (hacooh)";
const char* wifi_password = "owl601gentle";

//OSU
//const char* wifi_ssid = "";
//const char* wifi_password = "";

//MQTT server credentials
const char* mqtt_server_url = "9ccaa78e62aa4522ab541b0ca1426b5c.s1.eu.hivemq.cloud";
const int mqtt_server_port = 8883; //use 8884 in the web socket
const char* mqtt_server_username = "SoilBuddyPrototype";
const char* mqtt_server_password = "FarmWareTopSecretPassword12";

//WiFi and MQTT client objects
WiFiClientSecure wifi_client;
PubSubClient mqtt_client(wifi_client);

//device name
const char* device_name = "Soil Buddy Prototype";

//MQTT topic for finding sleep configuration
const char* sleep_config_topic = "soil-buddy-prototype/sleep-config";

//MQTT topic for publishing temperature data
const char* temperature_data_topic = "soil-buddy-prototype/temperature-data";

//MQTT topic for publishing moisture data
const char* moisture_data_topic = "soil-buddy-prototype/moisture-data";

//how long the Soil Buddy will sleep for (in mins)
//gets set by the sleep config topic, otherwise it'll use the default value
int sleep_period = default_sleep_period;

void connect_to_wifi();
void connect_to_mqtt_server();
void mqtt_callback_function(char* topic, uint8_t* message, unsigned int length);
float read_temperature_sensor();
uint16_t read_moisture_sensor();

void setup() {
  //begin serial monitor
  Serial.begin(115200);

  Serial.printf("\n\n");
  Serial.printf("Soil Buddy Prototype is awake...\n\n");

  //initialize temperature sensor
  temperature_sensor.begin();

  Serial.printf("Temperature sensor online...\n");

  //initialize moisture sensor
  moisture_sensor.begin(moisture_sensor_I2C_address);

  Serial.printf("Moisture sensor online...\n\n");

  //connect to WiFi
  connect_to_wifi();

  //connect to MQTT server
  connect_to_mqtt_server();

  //give the MQTT client time to receive the sleep config message
  uint64_t start_time = esp_timer_get_time();

  while(esp_timer_get_time() - start_time < 5000000) {
    mqtt_client.loop();
  }

  //read sensors
  float temp = read_temperature_sensor();
  uint16_t cap = read_moisture_sensor();

  //convert sensor data to strings
  char temp_message[10];
  char cap_message[10];

  dtostrf(temp, 1, 2, temp_message);
  itoa(cap, cap_message, 10);

  //publish sensor data to the MQTT server
  mqtt_client.publish(temperature_data_topic, temp_message);
  mqtt_client.publish(moisture_data_topic, cap_message);

  Serial.printf("\nPublished data to MQTT server.\n\nGoing into deep sleep for %d minutes.\nSee you then!\n\n", sleep_period);

  //forces program to wait for the serial monitor buffer to empty out, otherwise it'll go to sleep before everything has been printed
  Serial.flush();

  //go into deep sleep
  //ESP32 commits suicide here, when it wakes it'll completely reboot and execute all the code from the beginning
  esp_sleep_enable_timer_wakeup(sleep_period * 60000000);
  esp_deep_sleep_start();
}

//unused
void loop() {
  //loop should never be reached unless deep sleep is broken
  Serial.printf("YOU SHOULDN'T BE SEEING THIS!!\n\n");
}

//function connects to the WiFi network
void connect_to_wifi() {
  wifi_client.setInsecure();

  Serial.printf("Connecting to WiFi network...");
  
  WiFi.begin(wifi_ssid, wifi_password);

  while(WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.printf(".");
  }

  Serial.printf("\nConnected to WiFi network!\n\n");
}

//function connects to the MQTT server
void connect_to_mqtt_server() {
  mqtt_client.setServer(mqtt_server_url, mqtt_server_port);
  mqtt_client.setCallback(mqtt_callback_function);
  
  Serial.printf("Connecting to MQTT server...");

  while(! mqtt_client.connected()) {
    delay(500);
    mqtt_client.connect(device_name, mqtt_server_username, mqtt_server_password);
    Serial.printf(".");
  }

  Serial.printf("\nConnected to MQTT server!\n\n");

  mqtt_client.subscribe(sleep_config_topic);
}

//this function is called whenever a message is posted to a topic the Soil Buddy has subscribed to
//config messages will be marked with the retain flag so that they get sent in response to us subscribing
void mqtt_callback_function(char* topic, uint8_t* message, unsigned int length) {
  String incoming_message = "";

  for(int i = 0; i < length; i++) {
    incoming_message += (char) message[i];
  }

  if(String(topic) == sleep_config_topic) {
    sleep_period = incoming_message.toInt();
  }
}

//function returns temperature value
float read_temperature_sensor() {
  temperature_sensor.requestTemperatures();
  float temp_fahrenheit = temperature_sensor.getTempFByIndex(0);

  Serial.printf("Temperature reading in Fahrenheit: %.2f\n", temp_fahrenheit);

  return temp_fahrenheit;
}

//function returns moisture value
uint16_t read_moisture_sensor() {
  uint16_t capacitance = moisture_sensor.touchRead(0);

  Serial.printf("Capacitance reading (moisture level, higher = more moisture): %hu\n", capacitance);

  return capacitance;
}
