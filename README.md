ESP-IDF A2DP-SOURCE demo
========================

This is a portion of the program I am developing.  The goal is too play a wav file over Classic Blutooth using the A2DP profile.  I have commented on the function where I think the wav file is READ as part of the callback function but I mush admit I am struggling to understand it.

In addition I have set the BT debug logging to VERBOSE for A2D, APPL, and BTC layers.  This has been very helpful in understanding the function and purpose of the various layers!  Below is some of the key info from the logs.  To confirm I can access the WAV file I perform a "stat" call to get the file properties...this can be seen in the serial output as well.

MKSPIFFS
==========
I created the spiff.bin file using mkspiffs from this repo:

https://github.com/igrr/mkspiffs

Pull this repo and copy the /data directory (in this repo) into the mkspiffs directory

You need to run "make dist" to create the mkspiffs executable

This command will create the spiffs.bin file with the contents in the data directory:

    ./mkspiffs -c ./data -b 4096 -p 256 -s 0x100000 spiffs.bin

This comman will upload the spiffs.bin file to flash:

    python esptool.py --chip esp32 --port /dev/tty.usbserial-XXXXXXX --baud 115200 write_flash -z 0x190000 spiffs.bin

    Replace the XXXXXXX with your serial port

The csv file upperhand_partition.csv must be updated in menuconfig Partition Table.

I think there might be other ways to create the spiff partition.  Also there is a Windows version too.  Let me know if you have questions about this process.

Demo of A2DP audio source role

This is the demo for user to use ESP_APIs to use Advanced Audio Distribution Profile in transmitting audio stream

Options choose step:
    1. make menuconfig.
    2. enter menuconfig "Component config", choose "Bluetooth"
    3. enter menu Bluetooth, choose "Bluedroid Enable"
    4. enter menu Bluedroid Enable, choose "Classic Bluetooth"
    5. select "A2DP" and choose "SOURCE"
    
In this example, the bluetooth device implements A2DP source. The A2DP sink device to be connected to can be set up with the example "A2DP sink" in another folder in ESP-IDF example directory.
For the first step, the device performs device discovery to find a target device(A2DP sink) named "ESP_SPEAKER". Then it initiate connection with the target device.
After connection is established, the device then start media transmission. The raw PCM media stream to be encoded and transmited in this example is random sequence therefore continuous noise can be heard if the stream is decoded and played on the sink side.
After a period of time, media stream suspend, disconnection and reconnection procedure will be performed.
