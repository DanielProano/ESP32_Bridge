# About 

This project serves as my communications hub 
for my autonomous drone, taking in outside telem, 
commands, and instructions over Wi-Fi & Radio
before forwarding them to my STM32 flight controller 

## Startup 

```bash 
source /opt/esp-idf/export.sh

idf.py set-target esp32 

idf.py -p <port> flash monitor
``` 

