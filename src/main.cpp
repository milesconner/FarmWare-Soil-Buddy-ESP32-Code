
#include <Arduino.h>

//libraries for the temperature sensor
#include <OneWire.h>
#include <DallasTemperature.h>

//libraries for the moisture sensor
#include <Adafruit_seesaw.h>

//libraries for the air quality sensor
#include <Adafruit_BME680.h> //basic adafruit library (can only do raw gas resistance)
#include <bsec.h> //more complicated bosch library (can do IAQ)

//libraries for LoRa comms
#include <LoRa.h>
#include <SPI.h>

//LoRa transceiver pins
#define G0 4
#define CS 17
#define RST 16

//temperature sensor is connected to GPIO 14
#define temperature_sensor_pin 14

//temperature sensor object
OneWire one_wire(temperature_sensor_pin);
DallasTemperature temperature_sensor(&one_wire);

//moisture sensor object
Adafruit_seesaw moisture_sensor;
const uint8_t moisture_sensor_I2C_address = 0x36;

//air quality sensor object
Adafruit_BME680 air_quality_sensor;
const uint8_t air_quality_sensor_I2C_address = 0x76;

//volatile flag that gets set every time a new LoRa transmission is received
volatile bool pending_lora_packet = false;

//temperature sensor functions
uint8_t init_temperature_sensor(); //sets up temp sensor, returns whether it succeeded
float read_temperature_sensor(); //returns temp in F

//moisture sensor functions
uint8_t init_moisture_sensor(); //sets up moisture sensor, returns whether it succeeded
uint16_t read_moisture_sensor(); //returns capacitance (higher = more moisture)

//air quality sensor functions
uint8_t init_air_quality_sensor(); //sets up air quality sensor, returns whether it succeeded
uint32_t read_air_quality_sensor(); //returns gas resistance (higher = better air quality)

//LoRa functions
uint8_t init_lora(); //sets up LoRa, returns whether it succeeded
void lora_packet_received(int packet_size); //gets called every time a new LoRa transmission is received
void process_lora_packet(); //function to process received LoRa packets
uint8_t send_config_request(); //function to request config from gateway
uint8_t send_soil_data(uint8_t* data_buffer, size_t buffer_size); //function to send soil data to gateway
uint8_t send_acknowledgement(); //function to send acknowledgement to gateway

//Setup for RGB LED
const int redPin = 27;
const int greenPin = 12;
const int bluePin = 13;

//Setup for photodiode
const int photodiodePin = 33;

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
}

void loop() {
  //if a LoRa transmission came in, process it and clear the flag
  if(pending_lora_packet) {
    process_lora_packet();
    pending_lora_packet = false;
  }

  //do stuff

  //TODO:
  //1.implement FSM to progress through a wake cycle using the loop
  //2.figure out how to measure battery life from software
  //3.figure out air quality sensor 
  //    -desoldering debacle
  //    -bosch library to get IAQ
  //    -calibration nightmare
  //    -look into using VPD instead of IAQ
  //4.design LoRa packet layout
  //5.figure out moisture sensor
  //    -find way make the adafruit STEMMA work for us?
  //    -buy new sensor?
  //6.figure out physical aspects of NPK
  //7.figure out software aspects of NPK
  //8.smartphone app
  //9.housing
  //10.LoRa network stuff (addressing, integrity, collisions, encryption?)
  //11.LoRa gateway code
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
  float temp_fahrenheit = temperature_sensor.getTempFByIndex(0);

  Serial.printf("Temperature reading in Fahrenheit: %.3f\n", temp_fahrenheit);

  return temp_fahrenheit;
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
  if(! air_quality_sensor.begin(air_quality_sensor_I2C_address)) {
    Serial.printf("Air quality sensor failed to initialize!\n");

    return 0;
  }

  air_quality_sensor.setTemperatureOversampling(BME680_OS_8X);
  air_quality_sensor.setHumidityOversampling(BME680_OS_2X);
  air_quality_sensor.setPressureOversampling(BME680_OS_4X);
  air_quality_sensor.setIIRFilterSize(BME680_FILTER_SIZE_3);
  air_quality_sensor.setGasHeater(320, 150);

  Serial.printf("Air quality sensor online...\n");

  return 1;
}

uint32_t read_air_quality_sensor() {
  air_quality_sensor.performReading();
  uint32_t gas_resistance = air_quality_sensor.gas_resistance;

  Serial.printf("Gas resistance reading (higher = better air quality): %hu \n", gas_resistance);

  return gas_resistance;
}

uint8_t init_lora() {
  uint64_t start_time = esp_timer_get_time();

  LoRa.setPins(CS, RST, G0);

  Serial.printf("Initializing LoRa...");

  while(! LoRa.begin(915E6)) {
    delay(500);
    Serial.printf(".");

    if(esp_timer_get_time() - start_time > 10000000) {
      Serial.printf("\nLoRa failed to initialize!\n\n");
      
      return 0;
    }
  }

  LoRa.setSyncWord(0xF3);

  LoRa.onReceive(lora_packet_received);
  LoRa.receive();

  Serial.printf("\nLoRa online!\n\n");

  return 1;
}

//function is called every time a LoRa packet is received
//this is called from an ISR and should use ISR best practices
void lora_packet_received(int packet_size) {
  pending_lora_packet = true;
}

void process_lora_packet() {

}

uint8_t send_config_request() {

}

uint8_t send_soil_data(uint8_t* data_buffer, size_t buffer_size) {

}

uint8_t send_acknowledgement() {

}
