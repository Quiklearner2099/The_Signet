#!/bin/bash
set -e

echo "Installing Arduino CLI..."
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
export PATH=$PATH:/opt/arduino-cli/bin

echo "Setting up Arduino CLI configuration..."
mkdir -p ~/.config/arduino15
arduino-cli config init

echo "Adding ESP32 board package..."
arduino-cli core install esp32:esp32

echo "Installing ESP32 C6 board..."
arduino-cli board search "ESP32 C6" 

echo "Setup complete! Your ESP32 development environment is ready."
