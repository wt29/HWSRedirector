# Hot Water System Redirector

Uses a 240V 25A contactor (with 12V triggering) to switch a resistive HWS into direct connection to the exported (from solar) grid. My
basic reason for this is - Heat Pumps are great for outright efficiency but are mechanical, expensive and will eventually wear out. A resistive HWS coupled with a 
decent sized solar array should be a better fit for a many homes (IMHO) especially those with limited funds for HPHWS installation.

This sketch uses an API to read from a power monitoring system - I use an IOTAWATT however it also has a "Shelly EM" interface as well.

If you are using EMONCMS as your data capture and management system, this will also log the changes there.

The sketch uses a small data.h file to store local values. It also allows me to upload this project without revealing local data. 
There is a template in the header of main.cpp. 

You can adjust the threshold value and the "wait time" depending on your local requirement.

Its currently set up for an Espressif ESP32 C3 (I use the LOLIN C3 Mini) however any ESP microcontroller should work. It assumes
the power monitoring is available via Wifi (they aren't much use without it).

Here is a basic BOM  

1. An ESP32/8266 with WiFi (they all have them) - I used a Lolin C3 Mini but almost any ESP thing would work. https://www.aliexpress.com/item/1005004740051202.html
2. 25A Contactor with 12V trigger - I used a "Finder 22.32" with both NC and NO contacts
3. 12V power supply - a 1A wall wart will do the job fine.
4. Small DC-DC converter to provide 5V to the ESP - https://core-electronics.com.au/dc-dc-adjustable-step-down-module-5a-75w.html  as an example
5. An Arduino style relay - https://core-electronics.com.au/5v-single-channel-relay-module-10a.html
6. Some sort of enclosure - this is only 12V and mounted outside the swithwoard so weather proofing is the main aim here.

Get a local sparky to hook up the contactor and required isolators. They can also expose the 12V terminals somewhere you can get to them outside the board. You may as well get them to install a local power point as well.

The repo contains a Fusion (360) project and STL to build a mounting board for the 3 electronic boards. There is also the KiCAD files for the (somewhat) basic circuit involved.

This project is also under a bit of occasional development, mainly around getting a small UI on it.


