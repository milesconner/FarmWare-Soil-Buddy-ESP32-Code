
#include <Arduino.h>

//libraries for the temperature sensor
#include <OneWire.h>
#include <DallasTemperature.h>

//libraries for the moisture sensor
#include <Adafruit_seesaw.h>

//libraries for the air quality sensor
#include <Wire.h>
#include <bsec.h>

//libraries for LoRa comms
#include <SPI.h>
#include <LoRa.h>

//LoRa transceiver pins
#define G0 4
#define CS 17
#define RST 16

//LoRa defines
#define FREQUENCY_BAND 915E6
#define SYNC_WORD 0x12
#define NETWORK_CODE 0x3D8A
#define GATEWAY_ID 0x00
#define SOIL_BUDDY_ID 0x01

//temperature sensor is connected to GPIO 14
#define temperature_sensor_pin 14

//enum for the different states of the wake cycle
enum SystemState {
  UPDATE_CONFIG,
  WAIT_FOR_BURN_IN,
  TAKE_MEASUREMENTS,
  TRANSMIT_DATA,
  GO_TO_SLEEP
};

//enum for the different kinds of LoRa packets we'll be sending/receiving
enum MessageType {
  CONFIG,
  DATA,
  ACK
};

//defines LoRa packet header
struct Header {
  uint16_t network_code;
  uint8_t recipient_id;
  uint8_t sender_id;
  uint8_t message_type;
  uint8_t payload_length;
};

//defines what a config packet will contain
struct ConfigPayload {
  uint16_t sleep_duration;
  uint8_t num_of_samples;
  uint8_t sampling_interval;
  uint8_t burn_in_time;
};

//defines what a data packet will contain
struct DataPayload {
  float probe_temperature;

  uint16_t stemma_moisture;

  float bme_raw_gas;
  float bme_iaq;
  uint8_t bme_iaq_acc;
  float bme_co2;
  uint8_t bme_co2_acc;
  float bme_bvoc;
  uint8_t bme_bvoc_acc;
  float bme_comp_temp;
  float bme_comp_humid;
  float bme_pressure;
  float bme_vpd;
};

//temperature sensor object
OneWire one_wire(temperature_sensor_pin);
DallasTemperature temperature_sensor(&one_wire);

//moisture sensor object
Adafruit_seesaw moisture_sensor;
const uint8_t moisture_sensor_I2C_address = 0x36;

//air quality sensor object
Bsec air_quality_sensor;
const uint8_t air_quality_sensor_I2C_address = 0x76;

//volatile flag that gets set every time a new LoRa transmission is received
volatile bool pending_lora_packet = false;
//size of the pending packet
int pending_packet_size = 0;

//time (since boot) in microseconds that we last sent a LoRa transmission
uint64_t last_transmission_time = 0;

//store config in RTC memory so it survives deep sleep
//we can just shove the entire packet's payload in when we get it from the gateway
RTC_DATA_ATTR ConfigPayload config;

//current state of wake cycle
SystemState system_state;

//temperature sensor functions
uint8_t init_temperature_sensor(); //sets up temp sensor, returns whether it succeeded
float read_temperature_sensor(); //returns temp in C

//moisture sensor functions
uint8_t init_moisture_sensor(); //sets up moisture sensor, returns whether it succeeded
uint16_t read_moisture_sensor(); //returns capacitance (higher = more moisture)

//air quality sensor functions
uint8_t init_air_quality_sensor(); //sets up air quality sensor, returns whether it succeeded
uint16_t read_air_quality_index(); // returns IAQ (0-500, lower = better air quality)
uint16_t read_vapor_pressure_defecit(); //returns VPD (<0.4: low, 0.4-1.2: sweet spot, 1.2-1.6: high, >1.6: dangerous)
uint32_t read_air_quality_sensor(); //returns gas resistance (higher = better air quality)

//LoRa functions
uint8_t init_lora(); //sets up LoRa, returns whether it succeeded
void lora_packet_received(int packet_size); //gets called every time a new LoRa transmission is received
void process_lora_packet(); //function to process received LoRa packets

//state functions
void update_config();

//Setup for RGB LED
const int redPin = 27;
const int greenPin = 12;
const int bluePin = 13;

void setup() {
  //RGB LED stuff
  pinMode(redPin, OUTPUT);
  pinMode(greenPin, OUTPUT);
  pinMode(bluePin, OUTPUT);
  digitalWrite(redPin, HIGH);
  digitalWrite(greenPin, HIGH);
  digitalWrite(bluePin, HIGH);

  //begin serial monitor
  Serial.begin(115200);
  Serial.printf("\n\nSoil Buddy Prototype is awake...\n");

  //initialize temperature sensor
  init_temperature_sensor();

  //initialize moisture sensor
  init_moisture_sensor();

  //initialize air quality sensor
  init_air_quality_sensor();

  //initialize LoRa
  init_lora();

  //initialize state machine
  system_state = UPDATE_CONFIG;
}

void loop() {
  //if a LoRa transmission came in, process it and clear the flag
  if(pending_lora_packet) {
    process_lora_packet();

    pending_packet_size = 0;
    pending_lora_packet = false;
  }

  //call the corresponding function for whatever state we're in
  switch(system_state) {
  case UPDATE_CONFIG:
    update_config();
    break;
  }
}

void update_config() {
  if(last_transmission_time == 0) {
    Header header;

    header.network_code = NETWORK_CODE;
    header.recipient_id = GATEWAY_ID;
    header.sender_id = SOIL_BUDDY_ID;
    header.message_type = CONFIG;
    header.payload_length = 0;

    LoRa.beginPacket();
    LoRa.write((uint8_t*) &header, sizeof(Header));
    LoRa.endPacket();

    last_transmission_time = esp_timer_get_time();
  }

  if((esp_timer_get_time() - last_transmission_time) > 5000000) {
    last_transmission_time = 0;
  }
}

uint8_t init_temperature_sensor() {
  temperature_sensor.begin();

  DeviceAddress device_address;

  if(! temperature_sensor.getAddress(device_address, 0)) {
    Serial.printf("Temperature sensor failed to initialize!\n");

    return 0;
  }

  Serial.printf("Temperature sensor online...\n");

  return 1;
}

float read_temperature_sensor() {
  temperature_sensor.requestTemperatures();
  float temp_celsius = temperature_sensor.getTempCByIndex(0);

  Serial.printf("Temperature reading in Celsius: %.3f\n", temp_celsius);

  return temp_celsius;
}

uint8_t init_moisture_sensor() {
  if(! moisture_sensor.begin(moisture_sensor_I2C_address)) {
    Serial.printf("Moisture sensor failed to initialize!\n");

    return 0;
  }

  Serial.printf("Moisture sensor online...\n");

  return 1;
}

uint16_t read_moisture_sensor() {
  uint16_t capacitance = moisture_sensor.touchRead(0);

  Serial.printf("Capacitance reading (higher = more moisture): %hu\n", capacitance);

  return capacitance;
}

uint8_t init_air_quality_sensor() {
  air_quality_sensor.begin(air_quality_sensor_I2C_address, Wire);
  if(! air_quality_sensor.bme68xStatus != BME68X_OK) {
    Serial.printf("Air quality sensor failed to initialize!\n");

    return 0;
  }

  bsec_virtual_sensor_t sensor_list[] = {BSEC_OUTPUT_IAQ, BSEC_OUTPUT_RAW_TEMPERATURE, BSEC_OUTPUT_RAW_PRESSURE, BSEC_OUTPUT_RAW_HUMIDITY, BSEC_OUTPUT_RAW_GAS, BSEC_OUTPUT_STABILIZATION_STATUS, BSEC_OUTPUT_RUN_IN_STATUS};
  air_quality_sensor.updateSubscription(sensor_list, sizeof(sensor_list) / sizeof(sensor_list[0]), BSEC_SAMPLE_RATE_ULP);

  Serial.printf("Air quality sensor online...\n");

  return 1;
}

uint32_t read_air_quality_sensor() {
  air_quality_sensor.run();
  uint32_t gas_resistance = (uint32_t)air_quality_sensor.gasResistance;

  Serial.printf("Gas resistance reading (higher = better air quality): %hu \n", gas_resistance);

  return gas_resistance;
}

uint16_t read_air_quality_index() {
  air_quality_sensor.run();
  uint16_t iaq = (uint16_t)air_quality_sensor.iaq;

  Serial.printf("IAQ reading (lower = better air quality): %hu \n", iaq);

  return iaq;
}

uint16_t read_vapor_pressure_deficit() {
  air_quality_sensor.run();

  float temperature = air_quality_sensor.temperature;
  float humidity = air_quality_sensor.humidity;

  float saturation_vapor_pressure = 0.6108 * exp((17.27 * temperature) / (temperature + 237.3));
  float actual_vapor_pressure = (humidity / 100.0) * saturation_vapor_pressure;
  float vapor_pressure_deficit = saturation_vapor_pressure - actual_vapor_pressure;

  uint16_t vpd = (uint16_t)(vapor_pressure_deficit * 100); // Scale up for better resolution

  Serial.printf("VPD Reading: %hu \n", vpd);

  return vpd;
}

uint8_t init_lora() {
  uint64_t start_time = esp_timer_get_time();

  LoRa.setPins(CS, RST, G0);

  Serial.printf("Initializing LoRa...");

  while(! LoRa.begin(FREQUENCY_BAND)) {
    delay(500);
    Serial.printf(".");

    if(esp_timer_get_time() - start_time > 10000000) {
      Serial.printf("\nLoRa failed to initialize!\n\n");
      
      return 0;
    }
  }

  LoRa.setSyncWord(SYNC_WORD);

  LoRa.onReceive(lora_packet_received);
  LoRa.receive();

  Serial.printf("\nLoRa online!\n\n");

  return 1;
}

//function is called every time a LoRa packet is received
//this is called from an ISR and should use ISR best practices
void lora_packet_received(int packet_size) {
  pending_packet_size = packet_size;
  pending_lora_packet = true;
}

void process_lora_packet() {
  if(pending_packet_size < sizeof(Header)) {
    return;
  }

  Header header;
  LoRa.readBytes((uint8_t*) &header, sizeof(Header));

  if(header.network_code != NETWORK_CODE) {
    return;
  }

  if(header.recipient_id != SOIL_BUDDY_ID) {
    return;
  }

  if(pending_packet_size < (sizeof(Header) + header.payload_length)) {
    return;
  }

  switch(header.message_type) {
  case CONFIG:
    ConfigPayload config_payload;
    LoRa.readBytes((uint8_t*) &config_payload, sizeof(ConfigPayload));

    config.sleep_duration = config_payload.sleep_duration;
    config.num_of_samples = config_payload.num_of_samples;
    config.sampling_interval = config_payload.sampling_interval;
    config.burn_in_time = config_payload.burn_in_time;

    system_state = WAIT_FOR_BURN_IN;

    break;
    
  case ACK:
    break;
  }
}
