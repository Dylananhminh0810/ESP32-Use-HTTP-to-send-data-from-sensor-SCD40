/* MIT License
*
* Copyright (c) 2022 ma-lwa-re
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/
#include "scd4x.h"
#include "math.h"
#include "esp_log.h"
#include <rom/ets_sys.h>
static const char *SCD4X_TAG = "scd4x";

typedef struct measurements {
    scd4x_sensor_value_t co2;
    scd4x_sensor_value_t temperature;
    scd4x_sensor_value_t humidity;
} measurements_t;

uint8_t start_periodic_measurement[]             = {0x21, 0xB1};
uint8_t read_measurement[]                       = {0xEC, 0x05};
uint8_t stop_periodic_measurement[]              = {0x3F, 0x86};
uint8_t set_temperature_offset[]                 = {0x24, 0x1D};
uint8_t get_temperature_offset[]                 = {0x23, 0x18};
uint8_t set_sensor_altitude[]                    = {0x24, 0x27};
uint8_t get_sensor_altitude[]                    = {0x23, 0x22};
uint8_t set_ambient_pressure[]                   = {0xE0, 0x00};
uint8_t perform_forced_recalibration[]           = {0x36, 0x2F};
uint8_t set_automatic_self_calibration_enabled[] = {0x24, 0x16};
uint8_t get_automatic_self_calibration_enabled[] = {0x23, 0x13};
uint8_t start_low_power_periodic_measurement[]   = {0x21, 0xAC};
uint8_t get_data_ready_status[]                  = {0xE4, 0xB8};
uint8_t persist_settings[]                       = {0x36, 0x15};
uint8_t get_serial_number[]                      = {0x36, 0x82};
uint8_t perform_self_test[]                      = {0x36, 0x39};
uint8_t perfom_factory_reset[]                   = {0x36, 0x32};
uint8_t reinit[]                                 = {0x36, 0x46};
uint8_t measure_single_shot[]                    = {0x21, 0x9D};
uint8_t measure_single_shot_rht_only[]           = {0x21, 0x96};
uint8_t power_down[]                             = {0x36, 0xE0};
uint8_t wake_up[]                                = {0x36, 0xF6};

/*
* Delay the execution in ms
*/
void delay_ms(uint16_t delay) {
    ets_delay_us(delay * 1000);
}

/*
* The 8-bit CRC checksum transmitted after each data word is generated by a CRC algorithm.
* The CRC covers the contents of the two previously transmitted data bytes.
* To calculate the checksum only these two previously transmitted data bytes are used.
* Note that command words are not followed by CRC.
*/
uint8_t scd4x_generate_crc(const uint8_t* data, uint16_t count) {
    uint16_t current_byte;
    uint8_t crc = CRC8_INIT;
    uint8_t crc_bit;

    for(current_byte = 0; current_byte < count; ++current_byte) {
        crc ^= (data[current_byte]);
        for(crc_bit = 8; crc_bit > 0; --crc_bit) {

            if (crc & 0x80) {
                crc = (crc << 1) ^ CRC8_POLYNOMIAL;
            } else {
                crc = (crc << 1);
            }
        }
    }
    return crc;
}

/*
* For the send command sequences, after writing the address and/or data to the sensor
* and sending the ACK bit, the sensor needs the execution time to respond to the I2C read header with an ACK bit.
* Hence, it is required to wait the command execution time before issuing the read header.
* Commands must not be sent while a previous command is being processed.
*/
esp_err_t scd4x_send_command(uint8_t *command) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_start(cmd));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write_byte(cmd, (SCD41_SENSOR_ADDR << 1) | I2C_MASTER_WRITE, I2C_ACK_CHECK_EN));

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write(cmd, command, sizeof(command), I2C_ACK_CHECK_EN));

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_stop(cmd));
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);

    i2c_cmd_link_delete(cmd);
    return err;
}

/*
* Data sent to and received from the sensor consists of a sequence of 16-bit commands and/or 16-bit words
* (each to be interpreted as unsigned integer, most significant byte transmitted first). Each data word is
* immediately succeeded by an 8-bit CRC. In write direction it is mandatory to transmit the checksum.
* In read direction it is up to the master to decide if it wants to process the checksum.
*/
esp_err_t scd4x_read(uint8_t *hex_code, uint8_t *measurements, uint8_t size) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_start(cmd));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write_byte(cmd, (SCD41_SENSOR_ADDR << 1) | I2C_MASTER_WRITE, I2C_ACK_CHECK_EN));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write(cmd, hex_code, SCD41_HEX_CODE_SIZE, I2C_ACK_CHECK_EN));

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_start(cmd));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write_byte(cmd, (SCD41_SENSOR_ADDR << 1) | I2C_MASTER_READ, I2C_ACK_CHECK_EN));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_read(cmd, measurements, size, I2C_MASTER_LAST_NACK));

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_stop(cmd));
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);

    i2c_cmd_link_delete(cmd);
    return err;
}

/*
* Data sent to and received from the sensor consists of a sequence of 16-bit commands and/or 16-bit words
* (each to be interpreted as unsigned integer, most significant byte transmitted first). Each data word is
* immediately succeeded by an 8-bit CRC. In write direction it is mandatory to transmit the checksum.
* In read direction it is up to the master to decide if it wants to process the checksum.
*/
esp_err_t scd4x_write(uint8_t *hex_code, uint8_t *measurements, uint8_t size) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_start(cmd));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write_byte(cmd, (SCD41_SENSOR_ADDR << 1) | I2C_MASTER_WRITE, I2C_ACK_CHECK_EN));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write(cmd, hex_code, SCD41_HEX_CODE_SIZE, I2C_ACK_CHECK_EN));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write(cmd, measurements, size, I2C_ACK_CHECK_EN));

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_stop(cmd));
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);

    i2c_cmd_link_delete(cmd);
    return err;
}

/*
* For the send command and fetch results sequences, after writing the address and/or data to the sensor
* and sending the ACK bit, the sensor needs the execution time to respond to the I2C read header with an ACK bit.
* Hence, it is required to wait the command execution time before issuing the read header.
* Commands must not be sent while a previous command is being processed.
*/
esp_err_t scd4x_send_command_and_fetch_result(uint8_t *command, uint8_t *measurements, uint8_t size) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_start(cmd));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write_byte(cmd, (SCD41_SENSOR_ADDR << 1) | I2C_MASTER_WRITE, I2C_ACK_CHECK_EN));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write(cmd, command, sizeof(command), I2C_ACK_CHECK_EN));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write(cmd, measurements, size, I2C_ACK_CHECK_EN));

    delay_ms(1000);

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_start(cmd));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write_byte(cmd, (SCD41_SENSOR_ADDR << 1) | I2C_MASTER_READ, I2C_ACK_CHECK_EN));
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_read(cmd, measurements, size, I2C_MASTER_LAST_NACK));

    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_stop(cmd));
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);

    i2c_cmd_link_delete(cmd);
    return err;
}

/*
* Start periodic measurement, signal update interval is 5 seconds.
*/
esp_err_t scd4x_start_periodic_measurement() {
    return scd4x_send_command(start_periodic_measurement);
}

/*
* Read sensor output. The measurement data can only be read out once per signal update interval
* as the buffer is emptied upon read-out.
*/
esp_err_t scd4x_read_measurement(scd4x_sensors_values_t *sensors_values) {
    measurements_t measurements = {
        .co2 = {{0x00, 0x00}, 0x00},
        .temperature = {{0x00, 0x00}, 0x00},
        .humidity = {{0x00, 0x00}, 0x00}
    };

    if(!scd4x_get_data_ready_status()) {
        return ESP_FAIL;
    }

    esp_err_t err = scd4x_read(read_measurement, (uint8_t *) &measurements, sizeof(measurements));

    sensors_values->co2 = (measurements.co2.value.msb << 8) + measurements.co2.value.lsb;
    sensors_values->temperature = (175.0 * (((measurements.temperature.value.msb << 8) + measurements.temperature.value.lsb) / 65535.0)) - 45.0;
    sensors_values->humidity = 100.0 * ((measurements.humidity.value.msb << 8) + measurements.humidity.value.lsb) / 65535.0;
    return err;
}

/*
* Stop periodic measurement to change the sensor configuration or to save power. Note that the sensor will only
* respond to other commands after waiting 500 ms after issuing the stop_periodic_measurement command.
*/
esp_err_t scd4x_stop_periodic_measurement() {
    return scd4x_send_command(stop_periodic_measurement);
}

/*
* The temperature offset has no influence on the SCD4x CO2 accuracy. Setting the temperature offset of the SCD4x inside
* the customer device correctly allows the user to leverage the RH and T output signal. Note that the temperature offset
* can depend on various factors such as the SCD4x measurement mode, self-heating of close components, the ambient temperature
* and air flow. Thus, the SCD4x temperature offset should be determined inside the customer device under its typical operation
* conditions (including the operation mode to be used in the application) and in thermal equilibrium. Per default,
* the temperature offset is set to 4° C. To save the setting to the EEPROM, the persist setting command must be issued.
*/
esp_err_t scd4x_set_temperature_offset(float temperature) {
    uint16_t offset = (temperature * 65536.0) / 175.0;

    scd4x_sensor_value_t temperature_offset = {
        .value = {offset >> 8, offset & 0xFF}
    };
    temperature_offset.crc = scd4x_generate_crc((uint8_t *) &temperature_offset.value,
                                                sizeof(temperature_offset.value));

    return scd4x_write(set_temperature_offset, (uint8_t *) &temperature_offset, sizeof(temperature_offset));
}

/*
* Getting the temperature offset of the SCD4x from the EEPROM.
*/
float scd4x_get_temperature_offset() {
    scd4x_sensor_value_t temperature_offset = {
        .value = {0x00, 0x00},
        .crc = 0x00
    };

    esp_err_t err = scd4x_read(get_temperature_offset, (uint8_t *)
                               &temperature_offset, sizeof(temperature_offset));

    if(err != ESP_OK) {
        ESP_LOGE(SCD4X_TAG, "get_temperature_offset failed with status code: %s", esp_err_to_name(err));
        return SCD41_READ_ERROR;
    }
    return round((175 * (((temperature_offset.value.msb << 8) +
                 temperature_offset.value.lsb) / 65536.0)) * 10.0) / 10.0;
}

/*
* Reading and writing of the sensor altitude must be done while the SCD4x is in idle mode.
* Typically, the sensor altitude is set once after device installation. To save the setting to the EEPROM,
* the persist setting command must be issued. Per default, the sensor altitude is set to 0 meter above sea-level.
*/
esp_err_t scd4x_set_sensor_altitude(float altitude) {
    uint16_t offset = altitude;

    scd4x_sensor_value_t sensor_altitude = {
        .value = {offset >> 8, offset & 0xFF}
    };
    sensor_altitude.crc = scd4x_generate_crc((uint8_t *) &sensor_altitude.value, sizeof(sensor_altitude.value));

    return scd4x_write(set_sensor_altitude, (uint8_t *) &sensor_altitude, sizeof(sensor_altitude));
}

/*
* Getting the sensor altitude of the SCD4x from the EEPROM.
*/
uint16_t scd4x_get_sensor_altitude() {
    scd4x_sensor_value_t sensor_altitude = {
        .value = {0x00, 0x00},
        .crc = 0x00
    };

    esp_err_t err = scd4x_read(get_sensor_altitude, (uint8_t *) &sensor_altitude, sizeof(sensor_altitude));

    if(err != ESP_OK) {
        ESP_LOGE(SCD4X_TAG, "get_sensor_altitude failed with status code: %s", esp_err_to_name(err));
        return SCD41_READ_ERROR;
    }
    return ((sensor_altitude.value.msb << 8) + sensor_altitude.value.lsb);
}

/*
* The set_ambient_pressure command can be sent during periodic measurements to enable continuous pressure compensation.
* Note that setting an ambient pressure using set_ambient_pressure overrides any pressure compensation based on
* a previously set sensor altitude. Use of this command is highly recommended for applications experiencing significant
* ambient pressure changes to ensure sensor accuracy.
*/
esp_err_t scd4x_set_ambient_pressure(uint32_t pressure) {
    uint16_t offset = pressure / 100.0;

    scd4x_sensor_value_t ambient_pressure = {
        .value = {offset >> 8, offset & 0xFF}
    };
    ambient_pressure.crc = scd4x_generate_crc((uint8_t *) &ambient_pressure.value, sizeof(ambient_pressure.value));

    return scd4x_write(set_ambient_pressure, (uint8_t *) &ambient_pressure, sizeof(ambient_pressure));
}

/*
* To successfully conduct an accurate forced recalibration, the following steps need to be carried out:
*   1. Operate the SCD4x in the operation mode later used in normal sensor operation
*      (periodic measurement, low power periodic measurement or single shot) for > 3 minutes in an environment
*      with homogenous and constant CO2 concentration.
*   2. Issue stop_periodic_measurement. Wait 500 ms for the stop command to complete.
*   3. Subsequently issue the perform_forced_recalibration command and optionally read out the FRC correction
*      (i.e. themagnitude of the correction) after waiting for 400 ms for the command to complete.
*      A return value of 0xffff indicates that the forced recalibration has failed.
* Note that the sensor will fail to perform a forced recalibration if it was not operated before sending the command. Please make sure that the sensor is operated at the voltage desired for the application when applying the forced recalibration sequence.
*/
uint16_t scd4x_perform_forced_recalibration(uint16_t target_concentration) {
    scd4x_sensor_value_t co2_concentration = {
        .value = {target_concentration >> 8, target_concentration & 0xFF}
    };
    co2_concentration.crc = scd4x_generate_crc((uint8_t *) &co2_concentration.value, sizeof(co2_concentration.value));

    esp_err_t err = scd4x_send_command_and_fetch_result(perform_forced_recalibration, (uint8_t *) &co2_concentration, sizeof(co2_concentration));

    if(err != ESP_OK || (co2_concentration.value.msb = 0xFF && co2_concentration.value.lsb == 0xFF)) {
        ESP_LOGE(SCD4X_TAG, "perform_forced_recalibration failed with status code: %s", esp_err_to_name(err));
        return SCD41_READ_ERROR;
    }
    return ((co2_concentration.value.msb << 8) + co2_concentration.value.lsb) - 0x8000;
}

/*
* Set the current state (enabled / disabled) of the automatic self-calibration. By default, ASC is enabled.
* To save the setting to the EEPROM, the persist_setting command must be issued.
*/
esp_err_t scd4x_set_automatic_self_calibration_enabled(bool asc_enabled) {
    scd4x_sensor_value_t automatic_self_calibration = {
        .value = {0x00, asc_enabled ? 0x01 : 0x00},
        .crc = asc_enabled ? 0xB1 : 0x81
    };

    return scd4x_write(set_automatic_self_calibration_enabled, (uint8_t *) &automatic_self_calibration, sizeof(automatic_self_calibration));
}

/*
* Getting the automatic self calibration status of the SCD4x from the EEPROM.
*/
bool scd4x_get_automatic_self_calibration_enabled() {
    scd4x_sensor_value_t automatic_self_calibration = {
        .value = {0x00, 0x00},
        .crc = 0x00
    };

    esp_err_t err = scd4x_read(get_automatic_self_calibration_enabled, (uint8_t *) &automatic_self_calibration, sizeof(automatic_self_calibration));

    if(err != ESP_OK) {
        ESP_LOGE(SCD4X_TAG, "get_automatic_self_calibration_enabled failed with status code: %s", esp_err_to_name(err));
        return false;
    }
    return automatic_self_calibration.value.lsb ? true : false;
}

/*
* start low power periodic measurement, signal update interval is approximately 30 seconds.
*/
esp_err_t scd4x_start_low_power_periodic_measurement() {
    return scd4x_send_command(start_low_power_periodic_measurement);
}

/*
* Getting the SCD4x sensor output read status.
*/
bool scd4x_get_data_ready_status() {
    scd4x_sensor_value_t data_ready_status = {
        .value = {0x00, 0x00},
        .crc = 0x00
    };

    esp_err_t err = scd4x_read(get_data_ready_status, (uint8_t *) &data_ready_status, sizeof(data_ready_status));

    if(err != ESP_OK) {
        ESP_LOGE(SCD4X_TAG, "get_data_ready_status failed with status code: %s", esp_err_to_name(err));
        return false;
    }
    return ((data_ready_status.value.msb & 0x0F) == 0x00 && data_ready_status.value.lsb == 0x00) ? false : true;
}

/*
* Configuration settings such as the temperature offset, sensor altitude and the ASC enabled/disabled parameter
* are by default stored in the volatile memory (RAM) only and will be lost after a power-cycle.
* The persist_settings command stores the current configuration in the EEPROM of the SCD4x, making them persistent
* across power-cycling. To avoid unnecessary wear of the EEPROM, the persist_settings command should only be sent when
* persistence is required and if actual changes to the configuration have been made. The EEPROM is guaranteed to endure
* at least 2000 write cycles before failure. Note that field calibration history is automatically stored in
* a separate EEPROM dimensioned for the specified sensor lifetime.
*/
esp_err_t scd4x_persist_settings() {
    return scd4x_send_command(persist_settings);
}

/*
* Reading out the serial number can be used to identify the chip and to verify the presence of the sensor.
* The get serial number command returns 3 words, and every word is followed by an 8-bit CRC checksum.
* Together, the 3 words constitute a unique serial number with a length of 48 bits (big endian format).
*/
uint64_t scd4x_get_serial_number() {
    measurements_t serial_number = {
        .co2 = {{0x00, 0x00}, 0x00},
        .temperature = {{0x00, 0x00}, 0x00},
        .humidity = {{0x00, 0x00}, 0x00}
    };

    esp_err_t err = scd4x_read(get_serial_number, (uint8_t *) &serial_number, sizeof(serial_number));

    if(err != ESP_OK) {
        ESP_LOGE(SCD4X_TAG, "get_serial_number failed with status code: %s", esp_err_to_name(err));
        return SCD41_READ_ERROR;
    }
    return ((uint64_t) serial_number.co2.value.msb << 40 | (uint64_t) serial_number.co2.value.lsb << 32) |
           ((uint64_t) serial_number.temperature.value.msb << 24 | (uint64_t) serial_number.temperature.value.lsb << 16) |
           ((uint64_t) serial_number.humidity.value.msb << 8 | (uint64_t) serial_number.humidity.value.lsb);
}

/*
* The perform_self_test feature can be used as an end-of-line test to check sensor functionality and the customer
* power supply to the sensor.
*/
bool scd4x_perform_self_test() {
    scd4x_sensor_value_t self_test = {
        .value = {0x00, 0x00},
        .crc = 0x00
    };

    esp_err_t err = scd4x_read(perform_self_test, (uint8_t *) &self_test, sizeof(self_test));

    if(err != ESP_OK) {
        ESP_LOGE(SCD4X_TAG, "perform_self_test failed with status code: %s", esp_err_to_name(err));
        return false;
    }
    return (self_test.value.msb == 0x00 && self_test.value.lsb == 0x00) ? true : false;
}

/*
* The perform_factory_reset command resets all configuration settings stored in the EEPROM and erases the
* FRC and ASC algorithm history.
*/
esp_err_t scd4x_perfom_factory_reset() {
    return scd4x_send_command(perfom_factory_reset);
}

/*
* The reinit command reinitializes the sensor by reloading user settings from EEPROM.
* Before sending the reinit command, the stop measurement command must be issued.
* If the reinit command does not trigger the desired re-initialization, a power-cycle should be applied to the SCD4x.
*/
esp_err_t scd4x_reinit() {
    return scd4x_send_command(reinit);
}

/* 
* On-demand measurement of CO2 concentration, relative humidity and temperature. The sensor output is read
* using the read_measurement command.
*/
esp_err_t scd4x_measure_single_shot() {
    return scd4x_send_command(measure_single_shot);
}

/*
* On-demand measurement of relative humidity and temperature only. The sensor output is read using the
* read_measurement command. CO2 output is returned as 0 ppm.
*/
esp_err_t scd4x_measure_single_shot_rht_only() {
    return scd4x_send_command(measure_single_shot_rht_only);
}

/*
* Put the sensor from idle to sleep to reduce current consumption. Can be used to power down when operating the
* sensor in power-cycled single shot mode.
*/
esp_err_t scd4x_power_down() {
    return scd4x_send_command(power_down);
}

/*
* Wake up the sensor from sleep mode into idle mode. Note that the SCD4x does not acknowledge the wake_up command.
* To verify that the sensor is in the idle state after issuing the wake_up command, the serial number can be read out.
* Note that the first reading obtained using measure_single_shot after waking up the sensor should be discarded.
*/
esp_err_t scd4x_wake_up() {
    return scd4x_send_command(wake_up);
}
