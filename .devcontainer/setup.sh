#!/bin/bash
set -e

echo "Installing Arduino CLI..."
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
export PATH=$PATH:/opt/arduino-cli/bin

echo "Setting up Arduino CLI configuration..."
mkdir -p ~/.config/arduino15
arduino-cli config init

echo "Adding ESP32 board package..."
arduino-cli core install esp32:esp32@3.0.0

echo "Installing required Arduino libraries..."
arduino-cli lib install "FastLED"
arduino-cli lib install "ArduinoJson"

echo "Libraries installed:"
arduino-cli lib list

echo "Setup complete! Your ESP32 C6 Arduino development environment is ready."
