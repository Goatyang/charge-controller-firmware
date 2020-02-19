/*
 * Copyright (c) 2016 Martin Jäger / Libre Solar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifdef __MBED__

/** @file
 *
 * @brief Entry point of charge controller firmware
 */

#include "mbed.h"

#include "thingset.h"           // handles access to internal data via communication interfaces
#include "pcb.h"                // hardware-specific settings
#include "config.h"             // user-specific configuration
#include "setup.h"

#include "half_bridge.h"        // PWM generation for DC/DC converter
#include "hardware.h"           // hardware-related functions like watchdog, etc.
#include "dcdc.h"               // DC/DC converter control (hardware independent)
#include "pwm_switch.h"         // PWM charge controller
#include "bat_charger.h"        // battery settings and charger state machine
#include "daq.h"                // ADC using DMA and conversion to measurement values
#include "ext/ext.h"            // communication interfaces, displays, etc. in UEXT connector
#include "eeprom.h"             // external I2C EEPROM
#include "load.h"               // load and USB output management
#include "leds.h"               // LED switching using charlieplexing
#include "device_status.h"      // log data (error memory, min/max measurements, etc.)
#include "data_objects.h"       // for access to internal data via ThingSet
#include "bl_support.h"         // Bootloader support from the application side

Serial serial(PIN_SWD_TX, PIN_SWD_RX, "serial", 115200);

/** Main function including initialization and continuous loop
 */
int main()
{
    #ifdef BOOTLOADER_ENABLED
    check_bootloader(); // Update the bootloader status in flash to a stable state.
    #endif

    leds_init();

    battery_conf_init(&bat_conf, BATTERY_TYPE, BATTERY_NUM_CELLS, BATTERY_CAPACITY);
    battery_conf_overwrite(&bat_conf, &bat_conf_user);  // initialize conf_user with same values

    // Configuration from EEPROM
    data_objects_read_eeprom();
    ts.set_conf_callback(data_objects_update_conf);     // after each configuration change, data should be written back to EEPROM
    ts.set_user_password(THINGSET_USER_PASSWORD);       // passwords defined in config.h (see template)
    ts.set_maker_password(THINGSET_MAKER_PASSWORD);

    // Data Acquisition (DAQ) setup
    daq_setup();

    // initialize all extensions and external communication interfaces
    ext_mgr.enable_all();

    init_watchdog(10);      // 10s should be enough for communication ports

    #if CONFIG_HV_TERMINAL_SOLAR || CONFIG_LV_TERMINAL_SOLAR || CONFIG_PWM_TERMINAL_SOLAR
    solar_terminal.init_solar();
    #endif

    #if CONFIG_HV_TERMINAL_NANOGRID
    grid_terminal.init_nanogrid();
    #endif

    charger.detect_num_batteries(&bat_conf);     // check if we have 24V instead of 12V system
    charger.init_terminal(&bat_conf);
    load.set_voltage_limits(
        bat_conf.voltage_load_disconnect * charger.num_batteries,
        bat_conf.voltage_load_reconnect * charger.num_batteries,
        bat_conf.voltage_absolute_max * charger.num_batteries);

    wait(2);    // safety feature: be able to re-flash before starting
    control_timer_start(CONTROL_FREQUENCY);
    wait(0.1);  // necessary to prevent MCU from randomly getting stuck here if PV panel is connected before battery

    // The Mbed Serial class calls sleep_manager_lock_deep_sleep() internally during attach. If no
    // Serial is enabled, sleep does not always return in STM32F072, so we lock deep sleep manually
    sleep_manager_lock_deep_sleep();

    // the main loop is suitable for slow tasks like communication (even blocking wait allowed)
    time_t last_call = timestamp;
    while (1) {

        ext_mgr.process_asap();

        time_t now = timestamp;
        if (now >= last_call + 1 || now < last_call) {
            // called once per second (or slower if blocking wait occured somewhere)

            charger.discharge_control(&bat_conf);
            charger.charge_control(&bat_conf);

            eeprom_update();

            leds_update_1s();
            leds_update_soc(charger.soc, dev_stat.has_error(ERR_LOAD_LOW_SOC));

            ext_mgr.process_1s();

            #if CONFIG_HS_MOSFET_FAIL_SAFE_PROTECTION && CONFIG_HAS_DCDC_CONVERTER
            if (dev_stat.has_error(ERR_DCDC_HS_MOSFET_SHORT)) {
                dcdc.fuse_destruction();
            }
            #endif

            last_call = now;
        }
        feed_the_dog();
        sleep();    // wake-up by timer interrupts
    }
}

/** High priority function for DC/DC / PWM control and safety functions
 *
 * Called by control timer with 10 Hz frequency (see hardware.cpp).
 */
void system_control()
{
    static int counter = 0;

    // convert ADC readings to meaningful measurement values
    daq_update();

    // alerts should trigger only for transients, so update based on actual voltage
    daq_set_lv_alerts(lv_terminal.bus->voltage * 1.2, lv_terminal.bus->voltage * 0.8);

    bat_terminal.update_bus_current_margins();

    #if CONFIG_HAS_PWM_SWITCH
    pwm_switch.control();
    leds_set_charging(pwm_switch.active());
    #endif

    #if CONFIG_HAS_DCDC_CONVERTER
    hv_terminal.update_bus_current_margins();
    dcdc.control();     // control of DC/DC including MPPT algorithm
    leds_set_charging(half_bridge_enabled());
    #endif

    load.control();

    if (counter % CONTROL_FREQUENCY == 0) {
        // called once per second (this timer is much more accurate than time(NULL) based on LSI)
        // see also here: https://github.com/ARMmbed/mbed-os/issues/9065
        timestamp++;
        counter = 0;

        // energy + soc calculation must be called exactly once per second
        #if CONFIG_HAS_DCDC_CONVERTER
        hv_terminal.energy_balance();
        #endif

        #if CONFIG_HAS_PWM_SWITCH
        pwm_switch.energy_balance();
        #endif

        lv_terminal.energy_balance();
        load_terminal.energy_balance();
        dev_stat.update_energy();
        dev_stat.update_min_max_values();
        charger.update_soc(&bat_conf);
    }
    counter++;
}

#endif // __MBED__
