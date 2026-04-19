
#include <Arduino.h>

//libraries for the temperature sensor
#include <OneWire.h>
#include <DallasTemperature.h>

//libraries for the moisture sensor
#include <Adafruit_seesaw.h>

//temperature sensor is connected to GPIO 4
#define temperature_sensor_pin 4

//temperature sensor object
OneWire one_wire(temperature_sensor_pin);
DallasTemperature temperature_sensor(&one_wire);

//moisture sensor object
Adafruit_seesaw moisture_sensor;
uint8_t moisture_sensor_I2C_address = 0x36;

void setup() {
  //initialize temperature sensor
  temperature_sensor.begin();

  //initialize moisture sensor
  moisture_sensor.begin(moisture_sensor_I2C_address);

  //begin serial port
  Serial.begin(115200);

  Serial.printf("\n\n");
  Serial.printf("ESP32 is online...\n\n");
}

void loop() {
  //get temperature readings from the temperature sensor
  temperature_sensor.requestTemperatures();

  float temp_celsius = temperature_sensor.getTempCByIndex(0);
  float temp_fahrenheit = temperature_sensor.getTempFByIndex(0);
  
  //print temperature readings from the temperature sensor to the serial monitor
  if(temp_celsius != DEVICE_DISCONNECTED_C) {
    Serial.printf("Temperature reading in Celsius: %.2f (acquired from temperature sensor)\n", temp_celsius);

  } else {
    Serial.printf("Failed to get Celsius reading from temperature sensor!\n");
  }

  if(temp_fahrenheit >= DEVICE_DISCONNECTED_F) {
    Serial.printf("Temperature reading in Fahrenheit: %.2f (acquired from temperature sensor)\n\n", temp_fahrenheit);

  } else {
    Serial.printf("Failed to get Fahrenheit reading from temperature sensor!\n\n");
  }

  //get capacitance reading from the moisture sensor
  //capacitance ranges from 200 (bone dry) to 2000 (soaked)
  //typically falls between 300 and 500 in real soil, but depends on how loose the soil is
  //touching the moisture sensor will change the capacitance to verify it's working
  uint16_t capacitance = moisture_sensor.touchRead(0);

  //also get temperature readings from the moisture sensor because why not
  //much less accurate than the temperature sensor (about +/- 2 C)
  temp_celsius = moisture_sensor.getTemp();
  temp_fahrenheit = (temp_celsius * 1.8F) + 32.0F;

  //print capacitance (moisture) and temperature readings from the moisture sensor to the serial monitor
  Serial.printf("Capacitance reading (moisture level, higher = more moisture): %hu\n\n", capacitance);

  Serial.printf("Temperature reading in Celsius: %.2f (acquired from moisture sensor)\n", temp_celsius);
  Serial.printf("Temperature reading in Fahrenheit: %.2f (acquired from moisture sensor)\n\n", temp_fahrenheit);

  //5 sec delay
  delay(5000);
}
