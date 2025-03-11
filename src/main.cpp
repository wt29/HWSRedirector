#include <Arduino.h>

/*
  HWS Redirector

  This is designed to switch a resistive How Water System into grid power once a 
  local solar is exporting sufficient power.

  It utilises a 240V 25A contactor, with a 12V trigger to connect the HWS to the main/grid circuit.

  I uses a "Shelly EM" or and Iotawatt as the sense circuit and reads the local API for grid values

  It has a 5 minute timer to prevent the contactor cycling too often

  It is designed to use a Lolin C3 Mini but almost and ESP32/8266 with WiFi should work.
  
  https://lolin.aliexpress.com/store

  You will need a file "data.h" - copy the template below.
  
     -------------------------------------
  // template for data.h

  #define C3MINI                                 // Just incase you want to use something else
  
  // Node and Network Setup
  #define NODENAME "HWSRedirector"               // eg "Kitchen"  Required and UNIQUE per site.  Also used to find mdns eg NODENAME.local
  #define LOCAL_SSID "Your SSID"                 // How many you have defined
  #define LOCAL_PASSWD "Your local password"

  // API call to get grid value - sample Iotawatt shown - YMMV
    
  #define EMONCMS                              // You are logging to EMONCMS
  #define HOST       "Your EMONCMS HOST"       // Not required if not logging
  #define APIKEY     "Your EMONCMS API Key"    // Not required if not logging

  #define SHELLY                               // Using the Shelly EM and API
  #define API_CALL "http://192.168.1.100/query?select=[time.local.iso,Mains.Watts]&begin=m-1m&end=m&group=m&format=csv"
 -------------------------------------

How to put C3 boards into Device Firmware Upgrade (DFU) mode.

Hold on Button 9
Press Button Reset
Release Button 9 When you hear the prompt tone on usb reconnection

IMPORTANT -> if you don't setup the C3 WiFI output power parameter.... It doesn't connect!

*/
#define VERSION 0.2             // First Cut

#define C3MINI                  // This shoule work on almost any ESP32 BUT.... you will need to check your pin mappings.

#include <EEPROM.h>             // Going to save some the threshold value so it can be changed on the fly
#define EEPROM_SIZE 32          // 4 bytes each for wattsEnough and may be some other

#ifdef C3MINI               
 #include <WiFi.h>
 #include <ESPmDNS.h>
 #include <AsyncTCP.h>
//# include <WebServer.h>
 # include <ESPAsyncWebServer.h>
#else                            
// This should work if you are using an ESP8266
 #include <ESP8266WiFi.h>        // Should all works for and ESP8266
 #include <ESP8266WiFiMulti.h>   // Include the Wi-Fi-Multi library
 #include <ESP8266mDNS.h>
 #include <ESP8266WebServer.h>   // Include the WebServer library
#endif

#include <WiFiClient.h>
#include <ArduinoOTA.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>

// Needed to move this here as the IPAddress types aren't declared until the WiFi libs are loaded
// This means we dont keep uploading API key+password to GitHub. (data.h should be ignored in repository)
#include "data.h"             // Create this file from template above.  

const char* nodeName = NODENAME;

#ifdef C3MINI
 AsyncWebServer server(80);
#else
 ESP8266WiFiMulti wifiMulti;      // Create an instance of the ESP8266WiFiMulti class, called 'wifiMulti'
 ESP8266WebServer server(80);     // Create a webserver object that listens for HTTP request on port 80
#endif
WiFiClient client;                // Instance of WiFi Client

String handleRoot();              // function prototypes for HTTP handlers
void handleNotFound();            // Something it don't understand

void millisDelay(long unsigned int);
bool connectWiFi();
String getInternetTime();

int gridWatts;

int wattsEnough = -2200;          // Gets startup value, may not need once EEPROM code is OK
int waitTime = 300000;            // This is in milliseconds so 300000 is 60 x 5 x 1000
int contactorPin = 10;            // A relay or MOSFET to trigger the contactor - its likely to be 12V
bool contactorStatus = LOW;

const char* PARAM_INPUT_1 = "watts";
const char* PARAM_INPUT_2 = "seconds";

String inputWatts;
String inputSeconds;
String inputMessage;
String inputParam;
String inputMessage2;
String inputParam2;
    
#ifdef EMONCMS
const char* host = HOST;
const char* APIKEY = MYAPIKEY;
#endif

const uint32_t waitForWiFi = 5000 ;        // How long to wait for the WiFi to connect - 5 Seconds should be enough
int startWiFi;

int connectMillis = millis();              // this gets reset after every successful data push

int lastRun ;             

const long utcOffsetInSeconds = 36000;      // Sydney is 10 hours ahead - you will have to readjust
char daysOfTheWeek[7][12] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// Define NTP Client to get time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);
String timeString;
String startTime;
unsigned long startAbsoluteTime;    // How long have we been running for?

HTTPClient http;

const int ledPin = LED_BUILTIN;
int ledState = LOW;

void notFound(AsyncWebServerRequest* request) {
  request->send(404, "text/plain", "404: Not found"); // Send HTTP status 404 (Not Found) when there's no handler for the URI in the request
}

//-----------------------------------------
void setup()
{
  
  Serial.begin(115200);     // baud rate
  millisDelay(1) ;          // allow the serial to init
  Serial.println("--------Restart-------");         // clean up a little
  
  EEPROM.begin(32);                 // this number in "begin()" is ESP8266. EEPROM is emulated so you need buffer to emulate.

  int EEWatts;
  EEPROM.get(0, EEWatts );    // Should read 4 bytes for each of these thangs
  if ( isnan( EEWatts ) ) { 
    Serial.print("IsNan on wattsEnough");
    EEWatts = wattsEnough;
    EEPROM.put( 0, wattsEnough ) ;
    EEPROM.commit();
  } 
  Serial.print("Threshold watts from EEPROM:");
  Serial.println(EEWatts);
  wattsEnough = EEWatts;
  
  int EEWait;
  EEPROM.get(4,EEWait);        // Should read 4 bytes for each of these thangs
  if ( isnan( EEWait ) ) { 
    Serial.print("IsNan on WaitTime");
    EEWait = waitTime;               // Having issues during devel with "nan" (not a number) being written to EEPROM
    EEPROM.put(4,waitTime) ;
    EEPROM.commit();
  } 
  Serial.print("Threshold wait time from EEPROM:");
  Serial.println(EEWait);
  waitTime = EEWait;
  
  connectWiFi();                              // This thing isn't any use without WiFi

  pinMode( ledPin, OUTPUT );                  // The C3 V1 doesn't have an onboard LED
  pinMode( contactorPin, OUTPUT );            // Setup the relay/contactor
  digitalWrite( contactorPin, HIGH );         // Start with the (confusing) contactor open

  if (MDNS.begin( nodeName )) {               // Start the mDNS responder for <nodeName>.local
    Serial.println("mDNS responder started");
  }
  else
  {
    Serial.println("Error setting up MDNS responder!");
  }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("ESP32 Web Server: New request received:");  // for debugging
    Serial.println("/");                                    // for debugging
    String html = handleRoot(); 
    request->send(200, "text/html", html );
    
    }
  );

  server.on("/ep", HTTP_GET, [] (AsyncWebServerRequest *request) {
    int W,S;
    EEPROM.get(0,W);
    EEPROM.get(4,S);
    Serial.println( "EEpromRead0 " + String(W) );
    Serial.println( "EEpromRead4 " + String(S) );
    request->send(200, "text/html", "EEPROM 0 returns: " + String(W) + "<br> EEPROM 4 returns: " + String(S) +
     "<br><br><a href=\"/\">Return to Home Page</a>");

    }
  );

  server.on("/get", HTTP_GET, [] (AsyncWebServerRequest *request) {
    Serial.println("ESP32 Web Server: New request received:");  // for debugging
    Serial.println("/get");                                    // for debugging
    inputWatts = String(wattsEnough);
    inputSeconds = String(waitTime);
    // GET input1 value on <ESP_IP>/get?input1=<inputMessage>
    if (request->hasParam(PARAM_INPUT_1)) {
      inputMessage = request->getParam(PARAM_INPUT_1)->value();
      inputParam = PARAM_INPUT_1;
      wattsEnough = inputMessage.toInt();
      Serial.println("wattsEnough:" + wattsEnough );
      
      EEPROM.put(0, wattsEnough);
      EEPROM.commit();
      request->send(200, "text/html", "HTTP GET request sent to your ESP on input field (" 
        + inputParam + ") with value: " + inputMessage +
        "<br><br><a href=\"/\">Return to Home Page</a>");
    }
    // GET input2 value on <ESP_IP>/get?input2=<inputMessage>
    if (request->hasParam(PARAM_INPUT_2)) {
      inputMessage2 = request->getParam(PARAM_INPUT_2)->value();
      inputParam2 = PARAM_INPUT_2;
      waitTime = inputMessage2.toInt();    // Need to get this up from millis
      waitTime=waitTime*1000;
      Serial.println("waitTime:" + String(waitTime) );

      EEPROM.put(4, waitTime);
      EEPROM.commit();
      request->send(200, "text/html", "HTTP GET request sent to your ESP on input field (" 
        + inputParam2 + ") with value: " + inputMessage2 +
        "<br><br><a href=\"/\">Return to Home Page</a>");
    }
  }  
);
  server.onNotFound( notFound );
  
  server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", "Rebooting in 5 seconds <br>After reboot, click <a href=\"/\">Return to Home Page</a>");
    Serial.println( "Rebooting....." );
    millisDelay( 5000 );
    ESP.restart();
}
  );

  // Start the server
  server.begin();

  ArduinoOTA.begin();                           // Remote updates
  ArduinoOTA.setHostname( nodeName );

  timeClient.begin();
  startTime = getInternetTime();
  startAbsoluteTime = timeClient.getEpochTime();

  lastRun -= waitTime;  // Should force a poll on startup

}       // Setup

//------------------------------------------------------

void loop() {

  ArduinoOTA.handle();                            // Process updates

  bool trigger =  ( millis() > lastRun + waitTime ) ;
  if ( trigger )   {                                // only want this happening every 5 minutes (or so) 
    lastRun = millis();                           // don't want to add Wifi Connection latency to the poll
   
    Serial.println();
    Serial.print("Free Heap : ");
    Serial.println(ESP.getFreeHeap());

    if ( connectWiFi() ) {;                        // Also just checks if Wifi is connected
    
      String serverPath = IW_GRID;
      http.begin(client,serverPath);
      
      // Send HTTP GET request
      int httpResponseCode = http.GET();
      
      if (httpResponseCode>0) {
        Serial.print("API HTTP Response code: ");
        Serial.println(httpResponseCode);

        String API_payload = http.getString();
#ifndef SHELLY    
        // Iotawatt    
        gridWatts = API_payload.substring(API_payload.indexOf(',')+ 1).toInt();
#else        
        // Required bits to extract Shelly GRID payload.
        // This is a sample Shelly EM output
        // String API_payload = "{\"power\":-3000.76,\"reactive\":105.32,\"pf\":-0.49,\"voltage\":247.49,\"is_valid\":true,\"total\":323084.2,\"total_returned\":2311187.4}";
        gridWatts = API_payload.substring(API_payload.indexOf(':')+ 1).toInt();

#endif
        Serial.print("Grid Value: ");
        Serial.println(gridWatts);
       }
       else 
       {
        Serial.print("Error code: ");
        Serial.println(httpResponseCode);
       }
      // Free resources
       http.end();
       Serial.println( "WattEnough * 1-: " + String( wattsEnough *-1 ));
       if (gridWatts < (wattsEnough*-1) ) {   // When in export, grid watts will be in negative so we need the grid
                                              // to be less (more negative) then 'wattsEnough'
        contactorStatus = LOW;                // Relay is energised on LOW signal
        Serial.print( "Contactor on at " + getInternetTime() );
       }
       else
       {
        contactorStatus = HIGH;           // Default position
        Serial.print( "Contactor off at " + getInternetTime() );
       }
       digitalWrite( contactorPin, contactorStatus );    // confusing because the relay is energised on a low signal

#ifdef EMONCMS       
       Serial.printf("[Connecting to %s ... \n", host );
      
       if (client.connect(host, 80))     {
        Serial.println("Connected]");
        Serial.println("[Sending a request]");

        String request  = "GET " ;
            request += "/input/post?node=";
            request += nodeName;
            request += "&fulljson={\"HWSRedirector\":";
            request += ( contactorStatus ? 0 : 1 ) ;    // Reverse logic again
            request += "}&apikey=";
            request += APIKEY; 

        Serial.println( request );
        client.println( request );

        Serial.println("[Response:]");

        while (client.connected()) {
         if (client.available()) {
          String resp = "Null";
          resp = client.readStringUntil('\n');  // See what the host responds with.
          Serial.println( resp );

        }
#endif
      } 
     }
    }     // Connect WiFi
  }      // Millis Loop
}       // Loop

boolean connectWiFi() {
int start = millis();  
 
if ( WiFi.status() == WL_CONNECTED) {   // No point doing this if connected
 return true;
}
else                                    // Attempt
{ 
 WiFi.mode(WIFI_STA);                   // Station Mode
 WiFi.hostname( nodeName );             // This will show up in your DHCP server
 WiFi.disconnect();  
 WiFi.begin(LOCAL_SSID, LOCAL_PASSWD);
 
#ifdef C3MINI
 WiFi.setTxPower(WIFI_POWER_8_5dBm);    // This is a C3 thing
#endif

Serial.print("Connecting");
 while ( millis() < start + 5000) {     // 5 seconds to connect 
 
   delay(500);
   Serial.print(".");
  
   if ( WiFi.status() == WL_CONNECTED ) {
    Serial.println();
    Serial.print("Connected, IP address: ");
    Serial.println(WiFi.localIP());

    Serial.println("");
    Serial.print("IP Address: ");
    Serial.println( WiFi.localIP());
    Serial.printf("Connection status: %d\n", WiFi.status());
    return true;
    }
  }
 }
 // Abject failure - we couldn't connect
 Serial.println("");
 Serial.printf("Connection status: %d\n", WiFi.status());
 return false;              
}

String handleRoot() {
  String response =  "<h2>You have reached the Hot Water System Redirector</h2>";
  response += "<b>This triggers at " + String( waitTime/60000 ) + " minute intervals and when the export power value reaches " + String( abs( wattsEnough ) ) + " Watts</b>";
  response += "<body><p></p><table style=\"width:600\">";
  response += "<tr><td>Current time </td><td><b>" + getInternetTime() + "</b></td></tr>";
  int runSecs = timeClient.getEpochTime() - startAbsoluteTime;
  int upDays = abs( runSecs / 86400 );
  int upHours = abs( runSecs - ( upDays * 86400 ) ) / 3600;
  int upMins = abs( ( runSecs - (upDays * 86400) - ( upHours * 3600 ) ) / 60 ) ;
  int upSecs = abs( runSecs - (upDays * 86400) - ( upHours * 3600 ) - ( upMins * 60 ) );
  String upTime = String(upDays) + "d " + String( upHours ) + "h " + String(upMins) + "m " + String(upSecs) + "s";

  response += "<tr><td>Uptime  </td><td><b>" + upTime + "</b></td></tr>";
  response += "<tr><td>Node Name </td><td><b>" + String(nodeName) + "</b></td></tr>";
  response += "<tr><td>Local IP is: </td><td><b>" + WiFi.localIP().toString() + "</b></td></tr>";
  response += "<tr><td>Connected via AP:</td><td> <b>" + WiFi.SSID() + "</b></td></tr>";
  response += "<tr><td>Free Heap Space </td><td><b>" + String(ESP.getFreeHeap()) + " bytes</b></td></tr>";
  response += "<tr><td>Software Version</td><td><b>" + String(VERSION) + "</b></td></tr>";
  response += "<tr></tr>";
  response += "<tr><td>Grid Value </td><td><b>" + String(gridWatts) + "</td></tr>";
  response += "<tr><td>Contactor Status </td><td><b>" + String( (contactorStatus ? "On" : "Off" ) ) + "</td></tr>";
  response += "<tr><td>Current trigger value (in watts) </td><td><b>" + String( wattsEnough ) + "</b></td></tr>";  // this value is negative in the code but don't want to confuse users
  response += "<tr><td>    Current poll time in milliseconds </td><td><b>" + String( waitTime ) + "</b></td></tr>";
  response += "</table>";
  response += "<br>";
  response += "<form action=\"/get\">";
  response += "Trigger value in watts: <input type=\"text\" name=\"watts\">";
  response += "<input type=\"submit\" value=\"Submit\">";
  response += "</form>";
  response += "<form action=\"/get\">";
  response += " Poll time in seconds: <input type=\"text\" name=\"seconds\">";
  response += "<input type=\"submit\" value=\"Submit\">";
  response += "</form><br>";
  response += "This server also responds to /reboot</body>";
  return response;
}


String getInternetTime() {
  timeClient.update();
  return String( timeClient.getFormattedTime() );
}

// Wait around for a bit - This allows interupts to run whilst waiting
void millisDelay ( long unsigned int mDelay )
{
  long unsigned int now = millis();
  do {
    // Do nothing
  } while ( millis() < now + mDelay);

}
