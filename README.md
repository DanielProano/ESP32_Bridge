# About 

This project serves as my communications hub 
for my autonomous drone, taking in outside telem, 
commands, and instructions over Wi-Fi & Radio
before forwarding them to my STM32 flight controller 

## Wiring Diagram

<img width="1333" height="1652" alt="image" src="https://github.com/user-attachments/assets/c6bd0fb7-2364-4440-baf1-918c294a851b" />


## Startup 

```bash 
source /opt/esp-idf/export.sh

idf.py set-target esp32 

idf.py -p <port> flash monitor
``` 

## PINOUT 

https://mischianti.org/esp32-nodemcu-32s-esp-32s-kit-high-resolution-pinout-datasheet-and-specs/

## LIDAR docs

https://en.benewake.com/uploadfiles/2024/04/20240426135946148.pdf