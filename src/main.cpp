
//BME680 low power test

#include <Arduino.h>

#include <Wire.h>
#include <bsec.h>

Bsec air_quality_sensor;
const uint8_t air_quality_sensor_I2C_address = 0x76;

uint64_t measurement_period = 300000000; //bme will have a new measurement ready every 5 minutes
uint64_t grace_window = 15000000; //wake up 15 seconds early to make sure we're ready to call run() before the bme has a new measurement ready

//need to save bme state across sleep cycles
RTC_DATA_ATTR uint8_t air_quality_state[BSEC_MAX_STATE_BLOB_SIZE]; //bme state
RTC_DATA_ATTR bool air_quality_state_available = false; //will be false on our first cycle, then should remain true. this prevents trying to load a state that doesn't exist yet

//need to keep time across sleep cycles
RTC_DATA_ATTR uint64_t total_time_since_cold_boot = 0; //how long we've been running since first turning on the mcu
RTC_DATA_ATTR uint64_t measurement_counter = 0; //how many measurements we've made since first turning on the mcu

float calculate_VPD(float temperature, float humidity); //function to calculate VPD

void setup() {
  Serial.begin(115200);

  //initialize i2c bus and bme
  Wire.begin();
  air_quality_sensor.begin(air_quality_sensor_I2C_address, Wire);
  
  bsec_virtual_sensor_t sensor_list[13] = {
    BSEC_OUTPUT_RAW_GAS,
    BSEC_OUTPUT_GAS_PERCENTAGE,
    BSEC_OUTPUT_IAQ,
    BSEC_OUTPUT_STATIC_IAQ,
    BSEC_OUTPUT_CO2_EQUIVALENT,
    BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
    BSEC_OUTPUT_RAW_TEMPERATURE,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE,
    BSEC_OUTPUT_RAW_HUMIDITY,
    BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
    BSEC_OUTPUT_RAW_PRESSURE,
    BSEC_OUTPUT_STABILIZATION_STATUS,
    BSEC_OUTPUT_RUN_IN_STATUS
  };

  air_quality_sensor.updateSubscription(sensor_list, 13, BSEC_SAMPLE_RATE_ULP);

  //restore bme state from rtc memory, unless this is our first cycle
  if(air_quality_state_available) {
    air_quality_sensor.setState(air_quality_state);
  }

  //loop until the bme has its measurement ready
  while(! air_quality_sensor.run()) {
    delay(1);
  }

  uint64_t measurement_time = esp_timer_get_time(); //log measurement time relative to boot from sleep
  measurement_counter++; //increment our measurement counter

  //calculate hours:minutes:seconds elapsed between cold boot and measurement
  //this is the measurement time as tracked by the mcu
  uint64_t time_running_seconds = (total_time_since_cold_boot + measurement_time) / 1000000;
  uint64_t rtc_hours = time_running_seconds / 3600;
  uint64_t rtc_minutes = (time_running_seconds % 3600) / 60;
  uint64_t rtc_seconds = (time_running_seconds % 3600) % 60;

  //convert bme's reported measurement timestamp to hours:minutes:seconds
  //this is the measurement time as reported by the bme, it should be close to what we calculated above
  uint64_t timestamp_seconds = air_quality_sensor.outputTimestamp / 1000;
  uint64_t bme_hours = timestamp_seconds / 3600;
  uint64_t bme_minutes = (timestamp_seconds % 3600) / 60;
  uint64_t bme_seconds = (timestamp_seconds % 3600) % 60;

  //log literally everything
  Serial.printf("\n\n\n\n\n");
  Serial.printf("Measurement Number: %hu \n", measurement_counter);
  Serial.printf("Measurement Time (As Reported By MCU): %hu::%hu::%hu (hours:minutes:seconds) \n", rtc_hours, rtc_minutes, rtc_seconds);
  Serial.printf("Measurement Time (As Reported By BME): %hu::%hu::%hu (hours:minutes:seconds) \n", bme_hours, bme_minutes, bme_seconds);
  Serial.printf("BME Status Code: %d \n", air_quality_sensor.bme68xStatus);
  Serial.printf("BSEC Status Code: %d \n", air_quality_sensor.bsecStatus);
  Serial.printf("BSEC Version: %d \n", air_quality_sensor.version);
  Serial.printf("Stabilization Status: %f (has the MOX heater stablized, 0 = no, 1 = yes) \n", air_quality_sensor.stabStatus);
  Serial.printf("Run In Status: %f (has the burn-in profile finished, 0 = no, 1 = yes) \n", air_quality_sensor.runInStatus);
  Serial.printf("----------------------------------IAQ---------------------------------- \n");
  Serial.printf("Raw Gas Resistance: %f Ohms (higher = cleaner air) \n", air_quality_sensor.gasResistance);
  Serial.printf("Compensated Gas Value: %f Ohms (gas resistance adjusted for temperature and humidity, is only supposed to be used internally by the library) \n", air_quality_sensor.compGasValue);
  Serial.printf("Compensated Gas Value Accuracy: %hu (0-3, higher = more reliable Compensated Gas Value, may always be reported as 0) \n", air_quality_sensor.compGasAccuracy);
  Serial.printf("Gas Percentage: %f % (how much the current gas composition deviates from the cleanest air the sensor has ever recorded, alternative to IAQ) \n", air_quality_sensor.gasPercentage);
  Serial.printf("Gas Percentage Accuracy: %hu (0-3, higher = more reliable Gas Percentage) \n", air_quality_sensor.gasPercentageAccuracy);
  Serial.printf("IAQ: %f (0-500, lower = cleaner air) \n", air_quality_sensor.iaq);
  Serial.printf("IAQ Accuracy: %hu (0-3, higher = more reliable IAQ) \n", air_quality_sensor.iaqAccuracy);
  Serial.printf("Static IAQ: %f (doesn't factor rapid temperature and humidity changes into IAQ calculation, may be more reliable in some cases) \n", air_quality_sensor.staticIaq);
  Serial.printf("Static IAQ Accuracy: %hu (0-3, higher = more reliable Static IAQ) \n", air_quality_sensor.staticIaqAccuracy);
  Serial.printf("----------------------------------CO2---------------------------------- \n");
  Serial.printf("CO2 Equivalent: %f PPM (estimate of how much CO2 is present) \n", air_quality_sensor.co2Equivalent);
  Serial.printf("CO2 Accuracy: %hu (0-3, higher = more reliable CO2) \n", air_quality_sensor.co2Accuracy);
  Serial.printf("-------------------------------BREATH VOC------------------------------ \n");
  Serial.printf("Breath VOC Equivalent: %f PPM (estimate of how many VOCs are present that are typically caused by human presence) \n", air_quality_sensor.breathVocEquivalent);
  Serial.printf("Breath VOC Accuracy: %hu (0-3, higher = more reliable Breath VOC) \n", air_quality_sensor.breathVocAccuracy);
  Serial.printf("-------------------------------TEMPERATURE----------------------------- \n");
  Serial.printf("Raw Temperature: %f C \n", air_quality_sensor.rawTemperature);
  Serial.printf("Compensated Temperature: %f C (adjusted for the heat from the MOX heater, usually more accurate) \n", air_quality_sensor.temperature);
  Serial.printf("--------------------------------HUMIDITY------------------------------- \n");
  Serial.printf("Raw Humidity: %f % \n", air_quality_sensor.rawHumidity);
  Serial.printf("Compensated Humidity: %f % (adjusted for the heat from the MOX heater, usually more accurate) \n", air_quality_sensor.humidity);
  Serial.printf("--------------------------------PRESSURE------------------------------- \n");
  Serial.printf("Pressure: %f PA \n", air_quality_sensor.pressure);
  Serial.printf("----------------------------------VPD---------------------------------- \n");
  Serial.printf("VPD (Using Raw Values): %f PA (7000-15000 is a healthy range for crops) \n", calculate_VPD(air_quality_sensor.rawTemperature, air_quality_sensor.rawHumidity));
  Serial.printf("VPD (Using Compensated Values): %f PA (7000-15000 is a healthy range for crops) \n", calculate_VPD(air_quality_sensor.temperature, air_quality_sensor.humidity));
  
  Serial.flush();

  //store the bme state in rtc memory
  air_quality_sensor.getState(air_quality_state);
  air_quality_state_available = true;

  //calculate how much time we've wasted since the measurement came in, use it to calculate sleep duration
  uint64_t time_since_last_measurement = esp_timer_get_time() - measurement_time;
  uint64_t sleep_duration = measurement_period - time_since_last_measurement - grace_window;

  //update rtc global timer, accounting for the time we're about to spend sleeping and the time we've just spent awake
  total_time_since_cold_boot += (sleep_duration + esp_timer_get_time());

  //go to sleep
  esp_sleep_enable_timer_wakeup(sleep_duration);
  esp_deep_sleep_start();
}

void loop() {
  
}

float calculate_VPD(float temperature, float humidity) {
  float SVP = 0.61078 * exp((17.27 * temperature) / (temperature + 237.3)); //Saturation Vapor Pressure  
  float AVP = SVP * (humidity / 100.0); //Actual Vapor Pressure
  float VPD = SVP - AVP; //Vapor Pressure Deficit
    
  return (VPD * 1000.0); //Convert to Pa
}
