#include "power_manager_port.h"

/*
 * Replace this file with the target MCU implementation.
 * Examples:
 * - STM32: HAL_ADC_Start/Poll/GetValue + HAL_GPIO_ReadPin/WritePin
 * - NXP: ADC_DoSoftwareTriggerConvSeqA + GPIO_PinRead/Write
 */

static PowerIoLevel_t s_io_shadow[POWER_IO_COUNT];

bool PowerPort_Init(void)
{
    for (uint32_t i = 0; i < POWER_IO_COUNT; i++)
    {
        s_io_shadow[i] = POWER_IO_LEVEL_LOW;
    }

    return true;
}

uint16_t PowerPort_ReadBatteryMv(void)
{
    /*
     * Convert ADC raw value to battery millivolts here.
     * Example:
     * raw_mv = raw * vref_mv / adc_full_scale;
     * vbatt_mv = raw_mv * (r_high + r_low) / r_low;
     */
    return 12000;
}

bool PowerPort_ReadKl30Present(void)
{
    /*
     * KL30 is the unswitched battery supply. Return true when battery input
     * is physically present and the input diagnosis is valid.
     */
    return true;
}

bool PowerPort_ReadKl15On(void)
{
    /*
     * KL15 is ignition switched supply. Return true when ignition is ON.
     * Add debounce or use an already debounced digital input if needed.
     */
    return true;
}

void PowerPort_WriteIo(PowerIoId_t io, PowerIoLevel_t level)
{
    if ((io >= POWER_IO_COUNT) || (level == POWER_IO_LEVEL_KEEP))
    {
        return;
    }

    s_io_shadow[io] = level;

    /*
     * Map logical outputs to real pins here, for example:
     * case POWER_IO_MAIN_RELAY:
     *     HAL_GPIO_WritePin(PWR_RELAY_GPIO_Port, PWR_RELAY_Pin,
     *                       level == POWER_IO_LEVEL_HIGH ? GPIO_PIN_SET : GPIO_PIN_RESET);
     *     break;
     */
}

PowerIoLevel_t PowerPort_ReadIo(PowerIoId_t io)
{
    if (io >= POWER_IO_COUNT)
    {
        return POWER_IO_LEVEL_LOW;
    }

    return s_io_shadow[io];
}

void PowerPort_PrepareMcuSleep(void)
{
    /*
     * Gate peripherals, stop periodic communication, or enter MCU low power
     * mode from a dedicated low-power task if your platform requires it.
     */
}

void PowerPort_PrepareMcuShutdown(void)
{
    /*
     * Persist state, notify other tasks, close relays, and prepare wakeup
     * sources. The state machine keeps MCU_HOLD high until sleep state.
     */
}
