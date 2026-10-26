
#include <Arduino.h>

//libraries for the air quality sensor
#include <Wire.h>
#include <bsec.h>

//libraries for the temperature sensor
#include <OneWire.h>
#include <DallasTemperature.h>

//libraries for the moisture sensor
#include <Adafruit_seesaw.h>

//libraries for LoRa comms
#include <SPI.h>
#include <LoRa.h>

//temperature sensor is connected to GPIO 14
#define temperature_sensor_pin 14

//LoRa transceiver pins
#define G0 4
#define CS 17
#define RST 16

//LoRa defines
#define FREQUENCY_BAND 915E6
#define SYNC_WORD 0x12
#define SPREADING_FACTOR 10
#define BANDWIDTH 125E3
#define CODING_RATE 8
#define TRANSMIT_POWER 20
#define NETWORK_CODE 0x3D8A
#define GATEWAY_ID 0x00
#define SOIL_BUDDY_ID 0x01
#define TIMEOUT 15000000
#define MAX_ATTEMPTS 10

//system configuration defaults
#define NEXT_BIG_CYCLE_DEFAULT 1
#define SAMPLING_INTERVAL_DEFAULT 500
#define NUMBER_OF_STABILIZING_SAMPLES_DEFAULT 5
#define NUMBER_OF_ACTUAL_SAMPLES_DEFAULT 10

//enum for the different states of the state machine
//small cycle: BME_UPKEEP -> GO_TO_SLEEP
//big cycle:   BME_UPKEEP -> TAKE_MEASUREMENTS -> SEND_DATA -> GO_TO_SLEEP
enum SystemState {
  BME_UPKEEP, //small cycle work to keep the bme happy
  TAKE_MEASUREMENTS, //read other sensors on a big cycle
  SEND_DATA, //send data to Gateway
  GO_TO_SLEEP //enter deep sleep
};

//current state of the state machine
SystemState system_state;

//air quality sensor config
const uint8_t air_quality_sensor_config[] = {
  #include "config/generic_33v_300s_4d/bsec_iaq.txt"
};

//air quality sensor state
RTC_DATA_ATTR uint8_t air_quality_sensor_state[BSEC_MAX_STATE_BLOB_SIZE];
RTC_DATA_ATTR bool air_quality_sensor_state_available = false;

//air quality sensor virtual sensor list
bsec_virtual_sensor_t air_quality_sensor_virtual_sensor_list[6] = {
  BSEC_OUTPUT_IAQ,
  BSEC_OUTPUT_CO2_EQUIVALENT,
  BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
  BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
  BSEC_OUTPUT_STABILIZATION_STATUS,
  BSEC_OUTPUT_RUN_IN_STATUS
};

//air quality sensor object
Bsec air_quality_sensor;
const uint8_t air_quality_sensor_I2C_address = 0x76;

//temperature sensor object
OneWire one_wire(temperature_sensor_pin);
DallasTemperature temperature_sensor(&one_wire);

//moisture sensor object
Adafruit_seesaw moisture_sensor;
const uint8_t moisture_sensor_I2C_address = 0x36;

//LoRa packet header struct
struct __attribute__((packed)) Header {
  uint16_t network_code;
  uint8_t recipient_id;
  uint8_t sender_id;
};

//LoRa packet config payload struct
//Gateway -> Soil Buddy
struct __attribute__((packed)) ConfigPayload {
  uint16_t next_big_cycle = NEXT_BIG_CYCLE_DEFAULT;
  uint16_t sampling_interval = SAMPLING_INTERVAL_DEFAULT;
  uint8_t number_of_stabilizing_samples = NUMBER_OF_STABILIZING_SAMPLES_DEFAULT;
  uint8_t number_of_actual_samples = NUMBER_OF_ACTUAL_SAMPLES_DEFAULT;
};

//LoRa packet data payload struct
//Soil Buddy -> Gateway
//uses fixed-point arithmetic scaling to reduce the size of data
struct __attribute__((packed)) DataPayload {
  int16_t probe_temp;

  uint16_t stemma_moisture;

  uint8_t bme_acc;
  uint16_t bme_iaq;
  uint16_t bme_co2;
  int16_t bme_temp;
  uint16_t bme_humid;
};

//volatile flag that gets set every time a new LoRa transmission is received
volatile bool pending_lora_packet = false;
//size of the pending packet
int pending_packet_size = 0;

//time (since waking up) in microseconds that we last sent a LoRa transmission
int64_t last_transmission_time = 0;
//number of times we've attempted to transmit a LoRa packet
uint8_t transmission_attempts = 0;

//total time since first powering on in microseconds (minus the time spent awake on the current cycle)
RTC_DATA_ATTR int64_t time_alive_microseconds = 0;

//this struct holds the user configuration that the Gateway will send to us
RTC_DATA_ATTR ConfigPayload config_settings;

//this struct holds the final sensor readings that will be sent to the Gateway
DataPayload sensor_data;

//these variables are used to compute the average of several samples
//basically just used as intermediates, the final average goes in the sensor_data struct
float temperature_value = 0;
uint16_t moisture_value = 0;

//various counters used in take_measurements state
int64_t measurement_time = 0;
uint8_t stabilizing_measurements = 0;
uint8_t actual_measurements = 0;

//whether or not the current cycle is a big cycle
bool big_cycle = false;

//number of times we've woken up from deep sleep since the last big cycle
RTC_DATA_ATTR uint16_t wake_up_counter = 0;

//state functions
//these get called repeatedly from the loop depending on what state the state machine is in
void bme_upkeep(); //small cycle work to keep the bme happy
void take_measurements(); //read other sensors on a big cycle
void send_data(); //send data to Gateway
void go_to_sleep(); //enter deep sleep

//air quality sensor functions
bool init_air_quality_sensor(); //sets up air quality sensor, returns whether it succeeded
bool air_quality_sensor_status(); //checks for BSEC and BME errors/warnings

//temperature sensor functions
bool init_temperature_sensor(); //sets up temp sensor, returns whether it succeeded
float read_temperature_sensor(); //returns temp in C

//moisture sensor functions
bool init_moisture_sensor(); //sets up moisture sensor, returns whether it succeeded
uint16_t read_moisture_sensor(); //returns capacitance (higher = more moisture)

//LoRa functions
bool init_lora(); //sets up LoRa, returns whether it succeeded
void lora_packet_received(int packet_size); //gets called every time a new LoRa transmission is received
void process_lora_packet(); //function to process received LoRa packets

//utility functions
int64_t time_alive_milliseconds(); //returns total uptime in milliseconds since first powering on
void print_timestamp_milliseconds(int64_t timestamp); //takes a time in milliseconds and prints it in hours::minutes::seconds::milliseconds format
void print_system_configuration(); //prints the current user configuration settings

void setup() {
  //begin the serial monitor
  Serial.begin(115200);

  //log a bunch of stuff and check if this is a big cycle
  Serial.printf("\n\nSoil Buddy Prototype is awake...\n");
  print_system_configuration();

  wake_up_counter++;
  Serial.printf("Beginning wake cycle [%hu/%hu]...\n", wake_up_counter, config_settings.next_big_cycle);

  if(wake_up_counter == config_settings.next_big_cycle) {
    Serial.printf("This is a big cycle.\n");
    big_cycle = true;

  } else {
    Serial.printf("This is a small cycle.\n");
  }

  Serial.printf("Total uptime at beginning of wake cycle: ");
  print_timestamp_milliseconds(time_alive_milliseconds());
  Serial.printf("\n");

  //initialize the air quality sensor
  init_air_quality_sensor();

  //initialize the state machine
  system_state = BME_UPKEEP;
}

void loop() {
  //if a LoRa transmission came in, process it and clear the flag
  //will never happen during a small cycle because the LoRa transceiver is disabled
  if(pending_lora_packet) {
    process_lora_packet();

    pending_packet_size = 0;
    pending_lora_packet = false;
  }

  //call the corresponding function for whatever state we're in
  switch(system_state) {
  case BME_UPKEEP:
    bme_upkeep();
    break;

  case TAKE_MEASUREMENTS:
    take_measurements();
    break;

  case SEND_DATA:
    send_data();
    break;

  case GO_TO_SLEEP:
    go_to_sleep();
    break;

  default:
    break;
  }
}

void bme_upkeep() {
  //check for bme readings
  if(air_quality_sensor.run(time_alive_milliseconds())) {
    //bme readings came in

    if(! air_quality_sensor_status()) {
      Serial.printf("There was a problem taking air quality sensor readings!\n");
    }

    //log results
    Serial.printf("------------------------- BME680 READINGS --------------------------\n");
    Serial.printf("BME680 Measurement Timestamp: ");
    print_timestamp_milliseconds(air_quality_sensor.outputTimestamp);
    Serial.printf("\n");
    Serial.printf("BME680 Next Call: ");
    print_timestamp_milliseconds(air_quality_sensor.nextCall);
    Serial.printf("\n");
    Serial.printf("BME680 Stabilization Status: %f\n", air_quality_sensor.stabStatus);
    Serial.printf("BME680 Run-In Status: %f\n", air_quality_sensor.runInStatus);
    Serial.printf("BME680 Accuracy Score: %u\n", air_quality_sensor.iaqAccuracy);
    Serial.printf("BME680 IAQ: %f\n", air_quality_sensor.iaq);
    Serial.printf("BME680 CO2: %f PPM\n", air_quality_sensor.co2Equivalent);
    Serial.printf("BME680 Compensated Temperature: %f C\n", air_quality_sensor.temperature);
    Serial.printf("BME680 Compensated Humidity: %f %%\n", air_quality_sensor.humidity);
    Serial.printf("--------------------------------------------------------------------\n");

    if(big_cycle) {
      //if we're on a big cycle, store bme readings
      sensor_data.bme_acc = air_quality_sensor.iaqAccuracy;
      sensor_data.bme_iaq = (uint16_t) (air_quality_sensor.iaq * 100.0F);
      sensor_data.bme_co2 = (uint16_t) (air_quality_sensor.co2Equivalent * 10.0F);
      sensor_data.bme_temp = (int16_t) (air_quality_sensor.temperature * 100.0F);
      sensor_data.bme_humid = (uint16_t) (air_quality_sensor.humidity * 100.0F);

      //initialize the temperature sensor
      init_temperature_sensor();

      //initialize the moisture sensor
      init_moisture_sensor();

      //begin taking other measurements
      measurement_time = esp_timer_get_time();
      Serial.printf("-------------------- TEMP + MOISTURE READINGS ----------------------\n");

      system_state = TAKE_MEASUREMENTS;

    } else {
      //small cycle, don't need to do anything else and can go to sleep
      system_state = GO_TO_SLEEP;
    }
  }
}

void take_measurements() {
  //check if sampling interval has passed (how long to wait in between samples)
  if(((esp_timer_get_time() - measurement_time) / 1000) > config_settings.sampling_interval) {

    //first we take a few readings but don't save them to let the sensors stabilize
    //should hopefully get rid of most outliers
    if(stabilizing_measurements < config_settings.number_of_stabilizing_samples) {
      Serial.printf("Stabilizing... [%u/%u]\n", stabilizing_measurements + 1, config_settings.number_of_stabilizing_samples);
      read_temperature_sensor();
      read_moisture_sensor();
      stabilizing_measurements++;
      measurement_time = esp_timer_get_time();
    
    //then we take a bunch of samples  
    } else if(actual_measurements < config_settings.number_of_actual_samples) {
      Serial.printf("Taking samples... [%u/%u]\n", actual_measurements + 1, config_settings.number_of_actual_samples);
      temperature_value += read_temperature_sensor();
      moisture_value += read_moisture_sensor();
      actual_measurements++;
      measurement_time = esp_timer_get_time();

    //then we average the samples to get a final reading
    } else {
      temperature_value /= actual_measurements;
      moisture_value /= actual_measurements;

      Serial.printf("FINAL TEMPERATURE RESULT: %f C\n", temperature_value);
      Serial.printf("FINAL MOISTURE RESULT: %hu\n", moisture_value);
      Serial.printf("--------------------------------------------------------------------\n");

      sensor_data.probe_temp = (int16_t) (temperature_value * 100.0F);
      sensor_data.stemma_moisture = moisture_value;

      //initialize LoRa
      init_lora();

      //finished with measurements, time to send the data
      system_state = SEND_DATA;
    }
  }
}

void send_data() {
  //check if we have already sent a packet
  if(last_transmission_time == 0) {
    //build the header
    Header header;

    header.network_code = NETWORK_CODE;
    header.recipient_id = GATEWAY_ID;
    header.sender_id = SOIL_BUDDY_ID;

    //send header + data
    LoRa.beginPacket();
    LoRa.write((uint8_t*) &header, sizeof(Header));
    LoRa.write((uint8_t*) &sensor_data, sizeof(DataPayload));
    LoRa.endPacket();

    //start listening for Gateway response
    LoRa.onReceive(lora_packet_received);
    LoRa.receive();

    last_transmission_time = esp_timer_get_time();
    transmission_attempts++;

    Serial.printf("Sent data to Gateway over LoRa...\n");
    Serial.printf("Transmission attempts: [%u/%u]\n", transmission_attempts, MAX_ATTEMPTS);
    Serial.printf("Transmission Timestamp: ");
    print_timestamp_milliseconds(time_alive_milliseconds());
    Serial.printf("\n");
  }

  //check if we've timed out (waited too long for Gateway response)
  if(esp_timer_get_time() - last_transmission_time > TIMEOUT) {
    //resend packet
    if(transmission_attempts < MAX_ATTEMPTS) {
      Serial.printf("LoRa timed out! Haven't heard from Gateway... Retransmitting data...\n");
      last_transmission_time = 0;

    //if we've resent the packet too many times, just give up on sending
    } else {
      Serial.printf("LoRa timed out! Haven't heard from Gateway... Max transmission attempts reached, giving up on reaching Gateway...\n");
      system_state = GO_TO_SLEEP;
    }
  }
}

void go_to_sleep() {
  //if we're on a big cycle, reset wake up counter and disable LoRa transceiver
  if(big_cycle) {
    LoRa.end();
    wake_up_counter = 0;
  }

  //save the bme state to RTC memory
  air_quality_sensor.getState(air_quality_sensor_state);
  air_quality_sensor_state_available = true;

  if(! air_quality_sensor_status()) {
    Serial.printf("There was a problem saving the air quality sensor's state!\n");
  }

  Serial.printf("Ending wake cycle...\n");
  Serial.printf("Total uptime at end of wake cycle: ");
  print_timestamp_milliseconds(time_alive_milliseconds());
  Serial.printf("\n");
  Serial.printf("Entering deep sleep...\n");

  Serial.flush();

  //calculate sleep duration and update our uptime RTC counter
  int64_t sleep_duration = air_quality_sensor.nextCall - time_alive_milliseconds();
  time_alive_microseconds += (esp_timer_get_time() + (sleep_duration * 1000));

  //go to sleep
  esp_sleep_enable_timer_wakeup(sleep_duration * 1000);
  esp_deep_sleep_start();
}

bool init_air_quality_sensor() {
  //initialize the I2C bus and bme
  Wire.begin();
  air_quality_sensor.begin(air_quality_sensor_I2C_address, Wire);

  if(! air_quality_sensor_status()) {
    Serial.printf("Failed to initialize air quality sensor at .begin() phase!\n");
    return false;
  }

  //set config
  air_quality_sensor.setConfig(air_quality_sensor_config);

  if(! air_quality_sensor_status()) {
    Serial.printf("Failed to initialize air quality sensor at .setConfig() phase!\n");
    return false;
  }

  //set state
  if(air_quality_sensor_state_available) {
    air_quality_sensor.setState(air_quality_sensor_state);

    if(! air_quality_sensor_status()) {
      Serial.printf("Failed to initialize air quality sensor at .setState() phase!\n");
      return false;
    }

  } else {
    Serial.printf("Warning: No BSEC state found! Is this the first cycle?\n");
  }

  //set virtual sensor list and ULP sample rate
  air_quality_sensor.updateSubscription(air_quality_sensor_virtual_sensor_list, 6, BSEC_SAMPLE_RATE_ULP);

  if(! air_quality_sensor_status()) {
    Serial.printf("Failed to initialize air quality sensor at .updateSubscription() phase!\n");
    return false;
  }

  Serial.printf("Air quality sensor online...\n");
  return true;
}

bool air_quality_sensor_status() {
  bool status = true;

  if(air_quality_sensor.bsecStatus < BSEC_OK) {
    Serial.printf("BSEC library error: %d\n", air_quality_sensor.bsecStatus);
    status = false;

  } else if(air_quality_sensor.bsecStatus > BSEC_OK) {
    Serial.printf("BSEC library warning: %d\n", air_quality_sensor.bsecStatus);
  }

  if(air_quality_sensor.bme68xStatus < BME68X_OK) {
    Serial.printf("BME680 error: %d\n", air_quality_sensor.bme68xStatus);
    status = false;

  } else if(air_quality_sensor.bme68xStatus > BME68X_OK) {
    Serial.printf("BME680 warning: %d\n", air_quality_sensor.bme68xStatus);
  }

  return status;
}

bool init_temperature_sensor() {
  temperature_sensor.begin();

  DeviceAddress device_address;

  if(! temperature_sensor.getAddress(device_address, 0)) {
    Serial.printf("Temperature sensor failed to initialize!\n");

    return false;
  }

  Serial.printf("Temperature sensor online...\n");

  return true;
}

float read_temperature_sensor() {
  temperature_sensor.requestTemperatures();
  float temp_celsius = temperature_sensor.getTempCByIndex(0);

  Serial.printf("DS18B20 Temperature Probe: %f C\n", temp_celsius);

  return temp_celsius;
}

bool init_moisture_sensor() {
  if(! moisture_sensor.begin(moisture_sensor_I2C_address)) {
    Serial.printf("Moisture sensor failed to initialize!\n");

    return false;
  }

  Serial.printf("Moisture sensor online...\n");

  return true;
}

uint16_t read_moisture_sensor() {
  uint16_t capacitance = moisture_sensor.touchRead(0);

  Serial.printf("STEMMA Capacitance: %hu\n", capacitance);

  return capacitance;
}

bool init_lora() {
  LoRa.setPins(CS, RST, G0);

  if(! LoRa.begin(FREQUENCY_BAND)) {
    Serial.printf("LoRa failed to initialize!\n");
    return false;
  }

  LoRa.setSyncWord(SYNC_WORD);
  LoRa.setSpreadingFactor(SPREADING_FACTOR);       // Choices: 7 to 12 (Higher = more range, slower data)
  LoRa.setSignalBandwidth(BANDWIDTH);              // 125E3 (125 kHz) or 62.5E3 for narrower/deeper reach
  LoRa.setCodingRate4(CODING_RATE);                // 4/5, 4/6, 4/7, or 4/8 (4/8 gives max error correction)
  LoRa.setTxPower(TRANSMIT_POWER);                 // Max power (adjust based on your module's legal/hardware limits, e.g., 17-20 dBm)
  LoRa.enableCrc();                                // Enable cyclic redundancy check for packet integrity

  LoRa.onReceive(lora_packet_received);
  LoRa.receive();

  Serial.printf("LoRa online...\n");

  return true;
}

void lora_packet_received(int packet_size) {
  pending_packet_size = packet_size;
  pending_lora_packet = true;
}

void process_lora_packet() {
  //packet size mismatch, throw it away
  if(pending_packet_size < (sizeof(Header) + sizeof(ConfigPayload))) {
    return;
  }

  //read header
  Header header;
  LoRa.readBytes((uint8_t*) &header, sizeof(Header));

  //network code doesn't match, this packet isn't from one of our devices, throw it away
  if(header.network_code != NETWORK_CODE) {
    return;
  }

  //we aren't the intended recipient, packet was meant for another device, throw it away
  if(header.recipient_id != SOIL_BUDDY_ID) {
    return;
  }

  //read configuration payload
  ConfigPayload config_payload;
  LoRa.readBytes((uint8_t*) &config_payload, sizeof(ConfigPayload));

  //update our stored system configuration accordingly
  config_settings.next_big_cycle = config_payload.next_big_cycle;
  config_settings.sampling_interval = config_payload.sampling_interval;
  config_settings.number_of_actual_samples = config_payload.number_of_actual_samples;
  config_settings.number_of_stabilizing_samples = config_payload.number_of_stabilizing_samples;

  Serial.printf("Received acknowledgement packet with system configuration update from Gateway.\n");
  Serial.printf("Packet Timestamp: ");
  print_timestamp_milliseconds(time_alive_milliseconds());
  Serial.printf("\n");
  print_system_configuration();

  //now that we've received a valid Gateway response, it's time to go to sleep
  system_state = GO_TO_SLEEP;
}

int64_t time_alive_milliseconds() {
  return ((esp_timer_get_time() + time_alive_microseconds) / 1000);
}

void print_timestamp_milliseconds(int64_t timestamp) {
  int64_t hours = timestamp / 3600000;
  int64_t minutes = (timestamp % 3600000) / 60000;
  int64_t seconds = ((timestamp % 3600000) % 60000) / 1000;
  int64_t milliseconds = ((timestamp % 3600000) % 60000) % 1000;

  Serial.printf("%lld::%lld::%lld::%lld (hours::minutes::seconds::milliseconds)", hours, minutes, seconds, milliseconds);
}

void print_system_configuration() {
  Serial.printf("----------------------- SYSTEM CONFIGURATION -----------------------\n");
  Serial.printf("Next Big Cycle:                     wake cycle %hu\n", config_settings.next_big_cycle);
  Serial.printf("Sampling Interval:                  %hu milliseconds\n", config_settings.sampling_interval);
  Serial.printf("Number of Stabilizing Samples:      %u\n", config_settings.number_of_stabilizing_samples);
  Serial.printf("Number of Actual Samples:           %u\n", config_settings.number_of_actual_samples);

  if(config_settings.next_big_cycle == NEXT_BIG_CYCLE_DEFAULT && config_settings.sampling_interval == SAMPLING_INTERVAL_DEFAULT && config_settings.number_of_stabilizing_samples == NUMBER_OF_STABILIZING_SAMPLES_DEFAULT && config_settings.number_of_actual_samples == NUMBER_OF_ACTUAL_SAMPLES_DEFAULT) {
    Serial.printf("Warning: This is the default configuration! Is this the first cycle?\n");
  }

  Serial.printf("--------------------------------------------------------------------\n");
}
