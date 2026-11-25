# Live ESP32 pin plotter

The separate [ESP32 pin-monitor firmware](../../firmware/pin-monitor) samples up to eight GPIO inputs at a nominal 5,000 samples per second. This Python app reads the stream over USB serial and displays a live step trace for each input on a shared time axis.

## Wire and flash the monitor ESP32

The default inputs are **GPIO4 and GPIO5** on the ESP32-S3 and **GPIO34 and GPIO35** on the classic ESP32. Edit [input_pins.h](../../firmware/pin-monitor/src/input_pins.h) to select one to eight distinct pins, then rebuild. Connect each signal to its own input and share ground with the signal source. GPIO34 and GPIO35 on the classic ESP32 have no internal pull resistors, so drive them from a signal or provide an external pull resistor. Use only 3.3 V-compatible logic signals. For 5 V Hall signals, use a suitable level shifter or divider. [Espressif GPIO reference](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/gpio.html)

From the repository root, upload to the **monitor** board. These commands use the ESP32-S3 with 4 MB of flash, 2 MB of PSRAM, and native USB. For a classic ESP32 with a USB-to-UART bridge, replace `esp32s3_usb` with `esp32dev`. Use the monitor board’s actual port in both commands.

```sh
pio run -d firmware/pin-monitor -e esp32s3_usb -t upload --upload-port /dev/ttyACM0
python3 -m venv tools/pin-plotter/.venv
tools/pin-plotter/.venv/bin/python -m pip install -r tools/pin-plotter/requirements.txt
tools/pin-plotter/.venv/bin/python tools/pin-plotter/plot_pins.py /dev/ttyACM0
```

Close other serial monitors using the monitor board’s port before starting the plotter. The Python app learns the selected GPIOs from the firmware. Use `--window 0.04` for a close view of PWM or `--window 2` to watch slow transitions.

LOW and HIGH are shown at nominal 0 V and 3.3 V. This is logic-level sampling. For actual voltage, use an ADC or scope. Transitions shorter than the sampling interval may be missed. The polyline breaks where a gap in sampling or transmission is detected.
