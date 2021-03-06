/* LibreSolar MPPT charge controller firmware
 * Copyright (c) 2016-2018 Martin Jäger (www.libre.solar)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef UNIT_TEST

#include "mbed.h"
#include "config.h"

// don't use this file during processor-in-the-loop tests
#ifndef PIL_TESTING

#include "adc_dma.h"
#include "pcb.h"        // contains defines for pins
#include <math.h>       // log for thermistor calculation
#include "log.h"
#include "pwm_switch.h"

// factory calibration values for internal voltage reference and temperature sensor (see MCU datasheet, not RM)
#if defined(STM32F0)
    const uint16_t VREFINT_CAL = *((uint16_t *)0x1FFFF7BA); // VREFINT @3.3V/30°C
    #define VREFINT_VALUE 3300 // mV
    const uint16_t TSENSE_CAL1 = *((uint16_t *)0x1FFFF7B8);
    const uint16_t TSENSE_CAL2 = *((uint16_t *)0x1FFFF7C2);
    #define TSENSE_CAL1_VALUE 30.0   // temperature of first calibration point
    #define TSENSE_CAL2_VALUE 110.0  // temperature of second calibration point
#elif defined(STM32L0)
    const uint16_t VREFINT_CAL = *((uint16_t *)0x1FF80078);   // VREFINT @3.0V/25°C
    #define VREFINT_VALUE 3000 // mV
    const uint16_t TSENSE_CAL1 = *((uint16_t *)0x1FF8007A);
    const uint16_t TSENSE_CAL2 = *((uint16_t *)0x1FF8007E);
    #define TSENSE_CAL1_VALUE 30.0   // temperature of first calibration point
    #define TSENSE_CAL2_VALUE 130.0  // temperature of second calibration point
#endif

#ifdef PIN_REF_I_DCDC
AnalogOut ref_i_dcdc(PIN_REF_I_DCDC);
#endif

#ifdef PIN_TEMP_INT_PD
DigitalInOut temp_pd(PIN_TEMP_INT_PD);
#endif

float dcdc_current_offset;
float load_current_offset;

// for ADC and DMA
volatile uint16_t adc_readings[NUM_ADC_CH] = {0};
volatile uint32_t adc_filtered[NUM_ADC_CH] = {0};
//volatile int num_adc_conversions;

#define ADC_FILTER_CONST 5          // filter multiplier = 1/(2^ADC_FILTER_CONST)

extern Serial serial;
extern log_data_t log_data;
extern float mcu_temp;

void calibrate_current_sensors(dcdc_t *dcdc, load_output_t *load)
{
    dcdc_current_offset = -dcdc->ls_current;
    load_current_offset = -load->current;
}

//----------------------------------------------------------------------------
void update_measurements(dcdc_t *dcdc, battery_state_t *bat, load_output_t *load, power_port_t *hs, power_port_t *ls)
{
    //int v_temp, rts;

    // reference voltage of 2.5 V at PIN_V_REF
    //int vcc = 2500 * 4096 / (adc_filtered[ADC_POS_V_REF] >> (4 + ADC_FILTER_CONST));

    // internal STM reference voltage
    int vcc = VREFINT_VALUE * VREFINT_CAL / (adc_filtered[ADC_POS_VREF_MCU] >> (4 + ADC_FILTER_CONST));

    // rely on LDO accuracy
    //int vcc = 3300;

    ls->voltage =
        (float)(((adc_filtered[ADC_POS_V_BAT] >> (4 + ADC_FILTER_CONST)) * vcc) / 4096) *
        ADC_GAIN_V_BAT / 1000.0;
    load->voltage = ls->voltage;

    hs->voltage =
        (float)(((adc_filtered[ADC_POS_V_SOLAR] >> (4 + ADC_FILTER_CONST)) * vcc) / 4096) *
        ADC_GAIN_V_SOLAR / 1000.0;

#ifdef ADC_OFFSET_V_SOLAR
    hs->voltage = ls->voltage + -(vcc * ADC_OFFSET_V_SOLAR / 1000.0 + hs->voltage);
#endif

    load->current =
        (float)(((adc_filtered[ADC_POS_I_LOAD] >> (4 + ADC_FILTER_CONST)) * vcc) / 4096) *
        ADC_GAIN_I_LOAD / 1000.0 + load_current_offset;

    /// \todo Multiply current with PWM duty cycle for PWM charger to get avg current.
#ifdef CHARGER_TYPE_PWM
    hs->current =
        (float)(((adc_filtered[ADC_POS_I_SOLAR] >> (4 + ADC_FILTER_CONST)) * vcc) / 4096) *
        ADC_GAIN_I_SOLAR / 1000.0 + dcdc_current_offset;
    ls->current = hs->current - load->current;
#else // MPPT
    dcdc->ls_current =
        (float)(((adc_filtered[ADC_POS_I_DCDC] >> (4 + ADC_FILTER_CONST)) * vcc) / 4096) *
        ADC_GAIN_I_DCDC / 1000.0 + dcdc_current_offset;
    ls->current = dcdc->ls_current - load->current;
    hs->current = -dcdc->ls_current * ls->voltage / hs->voltage;
#endif


    /** \todo Improved (faster) temperature calculation:
       https://www.embeddedrelated.com/showarticle/91.php
    */

    float v_temp, rts;
    float bat_temp = 25.0;

#ifdef PIN_ADC_TEMP_BAT
    // battery temperature calculation
    v_temp = ((adc_filtered[ADC_POS_TEMP_BAT] >> (4 + ADC_FILTER_CONST)) * vcc) / 4096;  // voltage read by ADC (mV)
    rts = NTC_SERIES_RESISTOR * v_temp / (vcc - v_temp); // resistance of NTC (Ohm)

    // Temperature calculation using Beta equation for 10k thermistor
    // (25°C reference temperature for Beta equation assumed)
    bat_temp = 1.0/(1.0/(273.15+25) + 1.0/NTC_BETA_VALUE*log(rts/10000.0)) - 273.15; // °C
#endif

    detect_battery_temperature(bat, bat_temp);

#ifdef PIN_ADC_TEMP_FETS
    // MOSFET temperature calculation
    v_temp = ((adc_filtered[ADC_POS_TEMP_FETS] >> (4 + ADC_FILTER_CONST)) * vcc) / 4096;  // voltage read by ADC (mV)
    rts = 10000 * v_temp / (vcc - v_temp); // resistance of NTC (Ohm)
    dcdc->temp_mosfets = 1.0/(1.0/(273.15+25) + 1.0/NTC_BETA_VALUE*log(rts/10000.0)) - 273.15; // °C
#endif

    // internal MCU temperature
    uint16_t adcval = (adc_filtered[ADC_POS_TEMP_MCU] >> (4 + ADC_FILTER_CONST)) * vcc / VREFINT_VALUE;
    mcu_temp = (TSENSE_CAL2_VALUE - TSENSE_CAL1_VALUE) / (TSENSE_CAL2 - TSENSE_CAL1) * (adcval - TSENSE_CAL1) + TSENSE_CAL1_VALUE;
    //printf("TS_CAL1:%d TS_CAL2:%d ADC:%d, temp_int:%f\n", TS_CAL1, TS_CAL2, adcval, meas->temp_int);

    if (ls->voltage > log_data.battery_voltage_max) {
        log_data.battery_voltage_max = ls->voltage;
    }

    if (hs->voltage > log_data.solar_voltage_max) {
        log_data.solar_voltage_max = hs->voltage;
    }

    if (dcdc_current_offset < 0.1) {    // already calibrated
        if (ls->current > log_data.dcdc_current_max) {
            log_data.dcdc_current_max = ls->current;
        }

        if (load->current > log_data.load_current_max) {
            log_data.load_current_max = load->current;
        }

        if (ls->current > 0) {
            uint16_t solar_power = ls->voltage * ls->current;
            if (solar_power > log_data.solar_power_max_day) {
                log_data.solar_power_max_day = solar_power;
                if (log_data.solar_power_max_day > log_data.solar_power_max_total) {
                    log_data.solar_power_max_total = log_data.solar_power_max_day;
                }
            }
        }

        if (load->current > 0) {
            uint16_t load_power = ls->voltage * load->current;
            if (load_power > log_data.load_power_max_day) {
                log_data.load_power_max_day = load_power;
                if (log_data.load_power_max_day > log_data.load_power_max_total) {
                    log_data.load_power_max_total = log_data.load_power_max_day;
                }
            }
        }
    }

    if (dcdc->temp_mosfets > log_data.mosfet_temp_max) {
        log_data.mosfet_temp_max = dcdc->temp_mosfets;
    }

    if (bat->temperature > log_data.bat_temp_max) {
        log_data.bat_temp_max = bat->temperature;
    }

    if (mcu_temp > log_data.int_temp_max) {
        log_data.int_temp_max = mcu_temp;
    }
}

void detect_battery_temperature(battery_state_t *bat, float bat_temp)
{
#ifdef PIN_TEMP_INT_PD

    // state machine for external sensor detection
    enum temp_sense_state {
        TSENSE_STATE_CHECK,
        TSENSE_STATE_CHECK_WAIT,
        TSENSE_STATE_MEASURE,
        TSENSE_STATE_MEASURE_WAIT
    };
    static temp_sense_state ts_state = TSENSE_STATE_CHECK;

    static int sensor_change_counter = 0;

    switch (ts_state) {
        case TSENSE_STATE_CHECK:
            if (bat_temp < -50) {
                bat->ext_temp_sensor = false;
                temp_pd.output();
                temp_pd = 0;
                ts_state = TSENSE_STATE_CHECK_WAIT;
            }
            else {
                bat->ext_temp_sensor = true;
                ts_state = TSENSE_STATE_MEASURE;
            }
            break;
        case TSENSE_STATE_CHECK_WAIT:
            sensor_change_counter++;
            if (sensor_change_counter > 5) {
                sensor_change_counter = 0;
                ts_state = TSENSE_STATE_MEASURE;
            }
            break;
        case TSENSE_STATE_MEASURE:
            bat->temperature = bat->temperature * 0.8 + bat_temp * 0.2;
            //printf("Battery temperature: %.2f (%s sensor)\n", bat_temp, (bat->ext_temp_sensor ? "external" : "internal"));
            temp_pd.input();
            ts_state = TSENSE_STATE_MEASURE_WAIT;
            break;
        case TSENSE_STATE_MEASURE_WAIT:
            sensor_change_counter++;
            if (sensor_change_counter > 10) {
                sensor_change_counter = 0;
                ts_state = TSENSE_STATE_CHECK;
            }
            break;
    }
#else
    bat->temperature = bat_temp;
#endif
}


void dma_setup()
{
    //__HAL_RCC_DMA1_CLK_ENABLE();

    /* Enable the peripheral clock on DMA */
    RCC->AHBENR |= RCC_AHBENR_DMA1EN;

    /* Enable DMA transfer on ADC and circular mode */
    ADC1->CFGR1 |= ADC_CFGR1_DMAEN | ADC_CFGR1_DMACFG;

    /* Configure the peripheral data register address */
    DMA1_Channel1->CPAR = (uint32_t)(&(ADC1->DR));

    /* Configure the memory address */
    DMA1_Channel1->CMAR = (uint32_t)(&(adc_readings[0]));

    /* Configure the number of DMA tranfer to be performed on DMA channel 1 */
    DMA1_Channel1->CNDTR = NUM_ADC_CH;

    /* Configure increment, size, interrupts and circular mode */
    DMA1_Channel1->CCR =
        DMA_CCR_MINC |          /* memory increment mode enabled */
        DMA_CCR_MSIZE_0 |       /* memory size 16-bit */
        DMA_CCR_PSIZE_0 |       /* peripheral size 16-bit */
        DMA_CCR_TEIE |          /* transfer error interrupt enable */
        DMA_CCR_TCIE |          /* transfer complete interrupt enable */
        DMA_CCR_CIRC;           /* circular mode enable */
                                /* DIR = 0: read from peripheral */

    /* Enable DMA Channel 1 */
    DMA1_Channel1->CCR |= DMA_CCR_EN;

    /* Configure NVIC for DMA (priority 2: second-lowest value for STM32L0/F0) */
    NVIC_SetPriority(DMA1_Channel1_IRQn, 2);
    NVIC_EnableIRQ(DMA1_Channel1_IRQn);

    // Trigger ADC conversions
    ADC1->CR |= ADC_CR_ADSTART;
}

extern "C" void DMA1_Channel1_IRQHandler(void)
{
    if ((DMA1->ISR & DMA_ISR_TCIF1) != 0) // Test if transfer completed on DMA channel 1
    {
        // low pass filter with filter constant c = 1/16
        // y(n) = c * x(n) + (c - 1) * y(n-1)
#ifdef CHARGER_TYPE_PWM
        for (unsigned int i = 0; i < NUM_ADC_CH; i++) {
            if (i == ADC_POS_V_SOLAR || i == ADC_POS_I_SOLAR) {
                // only read input voltage and current when switch is on or permanently off
                if (GPIOB->IDR & GPIO_PIN_1 || pwm_switch_enabled() == false) {
                    adc_filtered[i] += (uint32_t)adc_readings[i] - (adc_filtered[i] >> ADC_FILTER_CONST);
                }
            }
            else {
                adc_filtered[i] += (uint32_t)adc_readings[i] - (adc_filtered[i] >> ADC_FILTER_CONST);
            }
        }
#else
        for (unsigned int i = 0; i < NUM_ADC_CH; i++) {
            // adc_readings: 12-bit ADC values left-aligned in uint16_t
            adc_filtered[i] += (uint32_t)adc_readings[i] - (adc_filtered[i] >> ADC_FILTER_CONST);
        }
#endif
    }
    DMA1->IFCR |= 0x0FFFFFFF;       // clear all interrupt registers
}

void adc_setup()
{
#ifdef PIN_REF_I_DCDC
    ref_i_dcdc = 0.1;    // reference voltage for zero current (0.1 for buck, 0.9 for boost, 0.5 for bi-directional)
#endif

    ADC_HandleTypeDef hadc;
    ADC_ChannelConfTypeDef sConfig = {0};

    __HAL_RCC_ADC1_CLK_ENABLE();

    // Configure ADC object structures
    hadc.Instance                   = ADC1;
    hadc.State                      = HAL_ADC_STATE_RESET;
    hadc.Init.ClockPrescaler        = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc.Init.Resolution            = ADC_RESOLUTION_12B;
    hadc.Init.DataAlign             = ADC_DATAALIGN_LEFT;       // for exponential moving average filter
    hadc.Init.ScanConvMode          = ADC_SCAN_DIRECTION_FORWARD;
    hadc.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
    hadc.Init.LowPowerAutoWait      = DISABLE;
    hadc.Init.LowPowerAutoPowerOff  = DISABLE;
    hadc.Init.ContinuousConvMode    = DISABLE;
    hadc.Init.DiscontinuousConvMode = DISABLE;
    hadc.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    hadc.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc.Init.DMAContinuousRequests = ENABLE; //DISABLE;
    hadc.Init.Overrun               = ADC_OVR_DATA_OVERWRITTEN;

    if (HAL_ADC_Init(&hadc) != HAL_OK) {
        error("Cannot initialize ADC");
    }

#if defined(STM32L0)
    HAL_ADCEx_Calibration_Start(&hadc, ADC_SINGLE_ENDED);
#else
    HAL_ADCEx_Calibration_Start(&hadc);
#endif

    // Configure ADC channel
    sConfig.Channel     = ADC_CHANNEL_0;            // can be any channel for initialization
    sConfig.Rank        = ADC_RANK_CHANNEL_NUMBER;

    // Clear all channels as it is not done in HAL_ADC_ConfigChannel()
    hadc.Instance->CHSELR = 0;

    if (HAL_ADC_ConfigChannel(&hadc, &sConfig) != HAL_OK) {
        error("Cannot initialize ADC");
    }

    HAL_ADC_Start(&hadc); // Start conversion

    // Read out value one time to finish ADC configuration
    if (HAL_ADC_PollForConversion(&hadc, 10) == HAL_OK) {
        HAL_ADC_GetValue(&hadc);
    }

    // ADC sampling time register
    // 000: 1.5 ADC clock cycles
    // 001: 7.5 ADC clock cycles
    // 010: 13.5 ADC clock cycles
    // 011: 28.5 ADC clock cycles
    // 100: 41.5 ADC clock cycles
    // 101: 55.5 ADC clock cycles
    // 110: 71.5 ADC clock cycles
    // 111: 239.5 ADC clock cycles
    //ADC1->SMPR = ADC_SMPR_SMP_1;      // for normal ADC OK
    ADC1->SMPR |= ADC_SMPR_SMP_0 | ADC_SMPR_SMP_1 | ADC_SMPR_SMP_2;      // necessary for internal reference and temperature

    // Select ADC channels based on setup in config.h
    ADC1->CHSELR = ADC_CHSEL;

    // Enable internal voltage reference and temperature sensor
    // ToDo check sample rate
    ADC->CCR |= ADC_CCR_TSEN | ADC_CCR_VREFEN;
}

#if defined(STM32F0)

void adc_timer_start(int freq_Hz)   // max. 10 kHz
{
    // Enable TIM15 clock
    RCC->APB2ENR |= RCC_APB2ENR_TIM15EN;

    // Set timer clock to 10 kHz
    TIM15->PSC = SystemCoreClock / 10000 - 1;

    // Interrupt on timer update
    TIM15->DIER |= TIM_DIER_UIE;

    // Auto Reload Register sets interrupt frequency
    TIM15->ARR = 10000 / freq_Hz - 1;

    // 2 = second-lowest priority of STM32L0/F0
    NVIC_SetPriority(TIM15_IRQn, 2);
    NVIC_EnableIRQ(TIM15_IRQn);

    // Control Register 1
    // TIM_CR1_CEN =  1: Counter enable
    TIM15->CR1 |= TIM_CR1_CEN;
}

extern "C" void TIM15_IRQHandler(void)
{
    TIM15->SR &= ~(1 << 0);
    ADC1->CR |= ADC_CR_ADSTART;
}

#elif defined(STM32L0)

void adc_timer_start(int freq_Hz)   // max. 10 kHz
{
    // Enable TIM6 clock
    RCC->APB1ENR |= RCC_APB1ENR_TIM6EN;

    // Set timer clock to 10 kHz
    TIM6->PSC = SystemCoreClock / 10000 - 1;

    // Interrupt on timer update
    TIM6->DIER |= TIM_DIER_UIE;

    // Auto Reload Register sets interrupt frequency
    TIM6->ARR = 10000 / freq_Hz - 1;

    // 2 = second-lowest priority of STM32L0/F0
    NVIC_SetPriority(TIM6_IRQn, 2);
    NVIC_EnableIRQ(TIM6_IRQn);

    // Control Register 1
    // TIM_CR1_CEN =  1: Counter enable
    TIM6->CR1 |= TIM_CR1_CEN;
}

extern "C" void TIM6_IRQHandler(void)
{
    TIM6->SR &= ~(1 << 0);
    ADC1->CR |= ADC_CR_ADSTART;
}

#endif

#endif /* TESTING_PIL */

#endif /* UNIT_TEST */
