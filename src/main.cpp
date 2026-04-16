
#include <Arduino.h>

//libraries for the temperature sensor
#include <OneWire.h>
#include <DallasTemperature.h>

//temperature sensor is connected to GPIO 4
#define temperature_sensor_pin 4

//temperature sensor object
OneWire one_wire(temperature_sensor_pin);
DallasTemperature temperature_sensor(&one_wire);

void setup() {
  //initialize temperature sensor
  temperature_sensor.begin();

  //begin serial port
  Serial.begin(115200);

  Serial.printf("\n\n");
  Serial.printf("ESP32 is online...\n\n");
}

void loop() {
  //get temperature readings from temperature sensor
  temperature_sensor.requestTemperatures();

  float temp_celsius = temperature_sensor.getTempCByIndex(0);
  float temp_fahrenheit = temperature_sensor.getTempFByIndex(0);
  
  //print temperature readings to serial monitor
  if(temp_celsius != DEVICE_DISCONNECTED_C) {
    Serial.printf("Temperature reading in Celsius: %.2f\n", temp_celsius);

  } else {
    Serial.printf("Failed to get Celsius reading!\n");
  }

  if(temp_fahrenheit >= DEVICE_DISCONNECTED_F) {
    Serial.printf("Temperature reading in Fahrenheit: %.2f\n\n", temp_fahrenheit);

  } else {
    Serial.printf("Failed to get Fahrenheit reading!\n\n");
  }

  //5 sec delay
  delay(5000);
}
