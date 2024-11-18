Hot Water System Redirector

Uses a 240V 30A contactor (with 12V triggering) to switch a resistive HWS into direct connection to the exported (from solar) grid. My
basic reason for this is - Heat Pumps are great but are mechanical, expensive and will eventually wear out. A resistive HWS coupled with and 
decent sized solar array should be a better fit for a large number of homes.

This sketch uses an API to read from a power monitoring system - I use an IOTAWATT however I'm also planning a "Shelly EM" interface as well.
If you are using EMONCMS as your data capture and management system, this will also log the changes there.

The sketch uses a small data.h file to store local values. It also allows me to upload this without revealing local data.

You can adjust the threshold value and the "wait time" depending on your local requirement.


Its currently set up for an Espresif ESP32 C3 (I use the LOLIN C3 Mini) however any WiFi enabled microcontroller should work. It assumes
the power monitoring is available via Wifi (they aren't much use without).

I'll add some basic HW but I think you will need 

1. 30A Contactor with 12V trigger
2. 12V power supply - a wall wart would do
3. Small DC-DC converter to provide 3.3 (or 5V) to the ESP
4. A FET switch - these are about $AU10 on ebay
5. Some sort of enclosure.

Get a local sparky to hook up the contactor and expose the 12V terminals somewhere you can get to them outside the board.
