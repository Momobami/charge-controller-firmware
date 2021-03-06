# Libre Solar Charge Controller Firmware

Software based on ARM mbed framework for the Libre Solar MPPT/PWM solar charge controllers

## Supported devices

The software is configurable to support different charge controller PCBs with either STM32F072 (including CAN support) or low-power STM32L072/3 MCUs.

- [Libre Solar MPPT 20A with CAN (v0.10)](https://github.com/LibreSolar/MPPT-Charger_20A)
- [Libre Solar MPPT 12A (v0.5)](https://github.com/LibreSolar/MPPT-Charger_20A/tree/legacy-12A-version)
- [Libre Solar MPPT 10A with USB (v0.2 and v0.4)](https://github.com/LibreSolar/MPPT-Charger_10A)
- [Libre Solar PWM 20A (preliminary support)](https://github.com/LibreSolar/PWM-Charger_20A)

## Toolchain and flashing instructions

See the Libre Solar website for a detailed instruction how to [develop software](http://libre.solar/docs/toolchain) and [flash new firmware](http://libre.solar/docs/flashing).

**Remark:** Flashing the STM32L072 MCU (as used in the 10A MPPT and PWM charge controller) using OpenOCD with the standard settings from PlatformIO fails in many cases. Possible workarounds:

1. Change OpenOCD settings to `set WORKAREASIZE 0x1000` in the file `~/.platformio/packages/tool-openocd/scripts/board/st_nucleo_l073rz.cfg`.

2. Use ST-Link tools. For Windows there is a GUI tool. Under Linux use following command:

    st-flash write .pioenvs/cloudsolar_0_4/firmware.bin 0x08000000

3. Use other debuggers and tools, e.g. Segger J-Link.


## Initial software setup (IMPORTANT!)

1. Select the correct board in `platformio.ini` by removing the comment before the board name under [platformio]
2. Copy `config.h_template` to `config.h` and adjust basic settings (`config.h` is ignored by git, so your changes are kept after software updates using `git pull`)

## API documentation

The documentation auto-generated by Doxygen can be found [here](https://libre.solar/charge-controller-firmware/).

## Additional firmware documentation (docs folder)

- [MPPT charger firmware details](docs/firmware.md)
- [Charger state machine](docs/charger.md)
