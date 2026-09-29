
#include <Arduino.h>

#include <Wire.h>
#include <SPI.h>

//libraries for the temperature sensor
#include <OneWire.h>
#include <DallasTemperature.h>

//libraries for the moisture sensor
#include <Adafruit_seesaw.h>

//libraries for the air quality sensor
#include <Adafruit_BME680.h>
#include <bsec.h>

//libraries for WiFi and MQTT server
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

//libraries for LoRa comms
#include <LoRa.h>

//LoRa transceiver pins
#define G0 4 //orange (IRQ/DIO0)
#define SCK 18 //blue
#define MISO 19 //green
#define MOSI 23 //yellow
#define CS 17 //white (NSS/SS)
#define RST 16 //red (NRESET/RESET)

//temperature sensor is connected to GPIO 14
#define temperature_sensor_pin 14

//how long the Soil Buddy should sleep for by default (in mins)
#define default_sleep_period 1

//for the air quality sensor (unused)
#define SEALEVELPRESSURE_HPA (1013.25)

//temperature sensor object
OneWire one_wire(temperature_sensor_pin);
DallasTemperature temperature_sensor(&one_wire);

//moisture sensor object
Adafruit_seesaw moisture_sensor;
const uint8_t moisture_sensor_I2C_address = 0x36;

//air quality sensor object
Adafruit_BME680 air_quality_sensor;
const uint8_t air_quality_sensor_I2C_address = 0x76;

//WiFi credentials (only one should be uncommented)

//Miles' apartment
//const char* wifi_ssid = "Tremors 7";
//const char* wifi_password = "gloverforn64";

//OSU
const char* wifi_ssid = "Registered4OSU";
const char* wifi_password = "aC7Yb6rcLu5xuWe3";

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

//MQTT topic for publishing air quality data
const char* air_quality_data_topic = "soil-buddy-prototype/air-quality-data";

//how long the Soil Buddy will sleep for (in mins)
//gets set by the sleep config topic, otherwise it'll use the default value
int sleep_period = default_sleep_period;

void connect_to_wifi();
void connect_to_mqtt_server();
void mqtt_callback_function(char* topic, uint8_t* message, unsigned int length);
float read_temperature_sensor();
uint16_t read_moisture_sensor();
uint32_t read_air_quality_sensor();
void init_lora();
void send_lora_string(const char* message);
void lora_received(int packet_size);

//Setup for RGB LED
const int redPin = 27;
const int greenPin = 12;
const int bluePin = 13;

void setup() {
  //begin serial monitor
  Serial.begin(115200);

  pinMode(redPin, OUTPUT);
  pinMode(greenPin, OUTPUT);
  pinMode(bluePin, OUTPUT);
  digitalWrite(redPin, HIGH);
  digitalWrite(greenPin, HIGH);
  digitalWrite(bluePin, HIGH);

  Serial.printf("\n\n");
  Serial.printf("Soil Buddy Prototype is awake...\n\n");

  //initialize temperature sensor
  temperature_sensor.begin();

  Serial.printf("Temperature sensor online...\n");

  //initialize moisture sensor
  moisture_sensor.begin(moisture_sensor_I2C_address);

  Serial.printf("Moisture sensor online...\n\n");

  //initialize air quality sensor
  air_quality_sensor.begin(air_quality_sensor_I2C_address);

  air_quality_sensor.setTemperatureOversampling(BME680_OS_8X);
  air_quality_sensor.setHumidityOversampling(BME680_OS_2X);
  air_quality_sensor.setPressureOversampling(BME680_OS_4X);
  air_quality_sensor.setIIRFilterSize(BME680_FILTER_SIZE_3);
  air_quality_sensor.setGasHeater(320, 150);

  Serial.printf("Air quality sensor online...\n\n");

  //initialize LoRa
  init_lora();

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
  uint32_t air = read_air_quality_sensor();

  //convert sensor data to strings
  char temp_message[10];
  char cap_message[10];
  char air_message[10];

  dtostrf(temp, 1, 2, temp_message);
  itoa(cap, cap_message, 10);
  itoa(air, air_message, 10);

  //publish sensor data to the MQTT server
  mqtt_client.publish(temperature_data_topic, temp_message);
  mqtt_client.publish(moisture_data_topic, cap_message);
  mqtt_client.publish(air_quality_data_topic, air_message);

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

//function initializes the LoRa transceiver
void init_lora() {
  LoRa.setPins(CS, RST, G0);

  Serial.printf("Initializing LoRa...");

  while(! LoRa.begin(915E6)) {
    delay(500);
    Serial.printf(".");
  }

  LoRa.setSyncWord(0xF3);

  LoRa.onReceive(lora_received);
  LoRa.receive();

  Serial.printf("\nLoRa online!\n\n");
}

//function transmits a string over LoRa
void send_lora_string(const char* message) {
  LoRa.beginPacket();
  LoRa.print(message);
  LoRa.endPacket();

  Serial.printf("Sent '");
  Serial.printf(message);
  Serial.printf("' over LoRa.\n\n");
}

//function is called everytime a LoRa packet is received
//this is called from an ISR and shouldn't be doing serial prints, but it's whatever for now
void lora_received(int packet_size) {
  Serial.printf("LoRa packet received.\n");
  Serial.printf("Packet size: %d\n", packet_size);
  Serial.printf("Message: ");

  while(LoRa.available()) {
    String data = LoRa.readString();
    Serial.print(data);
  }

  Serial.printf("\n");
  Serial.printf("RSSI: %d\n", LoRa.packetRssi());
  Serial.printf("SNR: %.3f\n\n", LoRa.packetSnr());
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

  Serial.printf("Temperature reading in Fahrenheit: %.3f\n", temp_fahrenheit);

  return temp_fahrenheit;
}

//function returns moisture value
uint16_t read_moisture_sensor() {
  uint16_t capacitance = moisture_sensor.touchRead(0);

  Serial.printf("Capacitance reading (moisture, higher = more moisture): %hu\n", capacitance);

  return capacitance;
}

//function returns air quality value
uint32_t read_air_quality_sensor() {
  air_quality_sensor.performReading();
  uint32_t air_quality = air_quality_sensor.gas_resistance;

  Serial.printf("Gas resistance reading in Ohms (air pollutants (VOCs), higher = better air quality): %hu \n", air_quality);

  return air_quality;
}
