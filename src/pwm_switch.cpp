/*
 * Copyright (c) 2018 Martin Jäger / Libre Solar
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pwm_switch.h"

#include <stdio.h>
#include <time.h>

#include "board.h"
#include "daq.h"
#include "debug.h"
#include "helper.h"

#if CONFIG_HAS_PWM_SWITCH

static bool _pwm_active;

#if defined(__MBED__) && !defined(UNIT_TEST)

static int _pwm_resolution;

void pwm_signal_init_registers(int freq_Hz)
{
    // Enable peripheral clock of GPIOB
    RCC->IOPENR |= RCC_IOPENR_IOPBEN;

    // Enable TIM3 clock
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;

    // Select alternate function mode on PB1 (first bit _1 = 1, second bit _0 = 0)
    GPIOB->MODER = (GPIOB->MODER & ~(GPIO_MODER_MODE1)) | GPIO_MODER_MODE1_1;

    // Select AF2 on PB1
    GPIOB->AFR[0] |= 0x2 << GPIO_AFRL_AFRL1_Pos;

    // Set timer clock to 10 kHz
    TIM3->PSC = SystemCoreClock / 10000 - 1;

    // Capture/Compare Mode Register 1
    // OCxM = 110: Select PWM mode 1 on OCx
    // OCxPE = 1:  Enable preload register on OCx (reset value)
    TIM3->CCMR2 |= TIM_CCMR2_OC4M_2 | TIM_CCMR2_OC4M_1 | TIM_CCMR2_OC4PE;

    // Capture/Compare Enable Register
    // CCxP: Active high polarity on OCx (default = 0)
    TIM3->CCER &= ~(TIM_CCER_CC4P);     // PB1 / TIM3_CH4: high-side

    // Interrupt on timer update
    TIM3->DIER |= TIM_DIER_UIE;

    // Auto Reload Register sets interrupt frequency
    TIM3->ARR = 10000 / freq_Hz - 1;

    // 1 = second-highest priority of STM32L0/F0
    NVIC_SetPriority(TIM3_IRQn, 1);
    NVIC_EnableIRQ(TIM3_IRQn);

    // Force update generation (UG = 1)
    TIM3->EGR |= TIM_EGR_UG;

    // set PWM frequency and resolution
    _pwm_resolution = 10000 / freq_Hz;

    // Auto Reload Register
    // Period goes from 0 to ARR (including ARR value), so substract 1 clock cycle
    TIM3->ARR = _pwm_resolution - 1;
}

extern "C" void TIM3_IRQHandler(void)
{
    TIM3->SR &= ~TIM_SR_UIF;       // clear update interrupt flag

    if ((int)TIM3->CCR4 < _pwm_resolution) {
        // turning the PWM switch on creates a short voltage rise, so inhibit alerts by 10 ms
        // at each rising edge if switch is not continuously on
        adc_upper_alert_inhibit(ADC_POS_V_LOW, 10);
    }
}

void pwm_signal_set_duty_cycle(float duty)
{
    TIM3->CCR4 = _pwm_resolution * duty;
}

void pwm_signal_duty_cycle_step(int delta)
{
    if ((int)TIM3->CCR4 + delta <= _pwm_resolution && (int)TIM3->CCR4 + delta >= 0) {
        TIM3->CCR4 += delta;
    }
}

float pwm_signal_get_duty_cycle()
{
    return (float)(TIM3->CCR4) / _pwm_resolution;
}

void pwm_signal_start(float pwm_duty)
{
    pwm_signal_set_duty_cycle(pwm_duty);

    // Control Register 1
    // TIM_CR1_CEN =  1: Counter enable
    TIM3->CR1 |= TIM_CR1_CEN;   // edge-aligned mode

    // Capture/Compare Enable Register
    // CCxE = 1: Enable the output on OCx
    // CCxP = 0: Active high polarity on OCx (default)
    // CCxNE = 1: Enable the output on OC1N
    // CCxNP = 0: Active high polarity on OC1N (default)
    TIM3->CCER |= TIM_CCER_CC4E;

    _pwm_active = true;
}

void pwm_signal_stop()
{
    TIM3->CR1 &= ~(TIM_CR1_CEN);
    TIM3->CCER &= ~(TIM_CCER_CC4E);
    TIM3->CCR4 = 0;
    _pwm_active = false;
}

bool pwm_signal_high()
{
    return (GPIOB->IDR & GPIO_PIN_1);
}

#else

// dummy functions for unit tests
float pwm_signal_get_duty_cycle() { return 0; }
void pwm_signal_set_duty_cycle(float duty) {;}
void pwm_signal_duty_cycle_step(int delta) {;}
void pwm_signal_init_registers(int freq_Hz) {;}
void pwm_signal_start(float pwm_duty) {;}
void pwm_signal_stop() {;}
bool pwm_signal_high() { return false; }

#endif

bool PwmSwitch::active()
{
    return _pwm_active;
}

bool PwmSwitch::signal_high()
{
    return pwm_signal_high();
}

PwmSwitch::PwmSwitch(DcBus *dc_bus) :
    PowerPort(dc_bus)
{
    off_timestamp = -10000;              // start immediately

    // calibration parameters
    offset_voltage_start = 2.0;     // V  charging switched on if Vsolar > Vbat + offset
    restart_interval = 60;          // s  When should we retry to start charging after low solar power cut-off?

    pwm_signal_init_registers(PWM_FREQUENCY);

    enable = true;     // enable charging in general
    _pwm_active = false;               // still disable actual switch
}

void PwmSwitch::control()
{
    if (_pwm_active) {
        if (neg_current_limit == 0
            || current > 0           // discharging battery into solar panel --> stop
            || bus->voltage < 9.0    // not enough voltage for MOSFET drivers anymore
            || enable == false)
        {
            pwm_signal_stop();
            off_timestamp = uptime();
            print_info("PWM charger stop.\n");
        }
        else if (bus->voltage > bus->sink_control_voltage()   // bus voltage above target
            || current < neg_current_limit                  // port current limit exceeded
            || current < -PWM_CURRENT_MAX)                  // PCB current limit exceeded
        {
            // decrease power, as limits were reached

            // the gate driver switch-off time is quite high (fall time around 1ms), so very short
            // on or off periods (duty cycle close to 0 and 1) should be avoided
            if (pwm_signal_get_duty_cycle() > 0.95) {
                // prevent very short off periods
                pwm_signal_set_duty_cycle(0.95);
            }
            else if (pwm_signal_get_duty_cycle() < 0.05) {
                // prevent very short on periods and switch completely off instead
                pwm_signal_stop();
                off_timestamp = uptime();
                print_info("PWM charger stop, no further derating possible.\n");
            }
            else {
                pwm_signal_duty_cycle_step(-1);
            }
        }
        else {
            // increase power (if not yet at 100% duty cycle)

            if (pwm_signal_get_duty_cycle() > 0.95) {
                // prevent very short off periods and switch completely on instead
                pwm_signal_set_duty_cycle(1);
            }
            else {
                pwm_signal_duty_cycle_step(1);
            }
        }
    }
    else {
        if (bus->sink_current_margin > 0          // charging allowed
            && bus->voltage < bus->sink_voltage_bound
            && ext_voltage > bus->voltage + offset_voltage_start
            && uptime() > (off_timestamp + restart_interval)
            && enable == true)
        {
            // turning the PWM switch on creates a short voltage rise, so inhibit alerts by 50 ms
            adc_upper_alert_inhibit(ADC_POS_V_LOW, 50);
            pwm_signal_start(1);
            print_info("PWM charger start.\n");
        }
    }
}

void PwmSwitch::emergency_stop()
{
    pwm_signal_stop();
    off_timestamp = uptime();
}

float PwmSwitch::get_duty_cycle()
{
    return pwm_signal_get_duty_cycle();
}

#endif /* CONFIG_HAS_PWM_SWITCH */
