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
  
  #define WATTS_ENOUGH  -2200                  // When there are enough export watts to trigger the contactor
  #define WAIT_TIME     300000                 // How long to wait until trigger in seconds - stops the contactor from cycling

  #define EMONCMS                              // You are logging to EMONCMS
  #define HOST       "Your EMONCMS HOST"       // Not required if not logging
  #define APIKEY     "Your EMONCMS API Key"    // Not required if not logging

  #define SHELLY                               // Using the Shelly EM and API
  #define API_CALL "http://192.168.1.100/query?select=[time.local.iso,Mains.Watts]&begin=m-1m&end=m&group=m&format=csv"
 -------------------------------------

*/
#define VERSION 0.2             // First Cut

#define C3MINI                  // This shoule work on almost any ESP32 BUT.... you will need to check your pin mappings.

#include <EEPROM.h>             // Going to save some the threshold value so it can be changed on the fly
#define EEPROM_SIZE 32          // 4 bytes each for wattsEnough and may be some other

#ifdef C3MINI               
 #include <WiFi.h>
 #include <ESPmDNS.h>
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

String handleRoot();                // function prototypes for HTTP handlers
void handleNotFound();            // Something it don't understand
void rebootDevice();              // Kick it over remotely
const char* PARAM_INPUT_1 = "watts";
const char* PARAM_INPUT_2 = "seconds";

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
  <title>ESP Input Form</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  </head><body>
  <form action="/get">
    input1: <input type="number" name="watts" value=2200 >
    <input type="submit" value="Submit">
  </form><br>
  <form action="/get">
    input2: <input type="number" name="seconds" value=300>
    <input type="submit" value="Submit">
  </form><br>
  <form action="/get">
    input3: <input type="text" name="input3">
    <input type="submit" value="Submit">
  </form>
</body></html>)rawliteral";


void millisDelay(long unsigned int);
bool connectWiFi();
String getInternetTime();

int gridWatts;
int wattsEnough = WATTS_ENOUGH;
int contactorPin = 10;                  // A relay or MOSFET to trigger the contactor - its likely to be 12V
bool contactorStatus = LOW;

#ifdef EMONCMS
const char* host = HOST;
const char* APIKEY = MYAPIKEY;
#endif

const uint32_t waitForWiFi = 5000 ;        // How long to wait for the WiFi to connect - 5 Seconds should be enough
int startWiFi;

int connectMillis = millis();              // this gets reset after every successful data push

long unsigned int lastRun = -WAIT_TIME;    // Force a run on boot. Includes connect to WiFi

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

void rebootDevice(AsyncWebServerRequest* request) {
 // Serial.println( "In reboot Device" );
 // millisDelay( 5000 );
 // ESP.restart();
  String html = "<h1>Rebooting " + String(nodeName) + " in 5 seconds</h1>";
  request->send(200, "text/html", html );  // Warn them
}

//-----------------------------------------
void setup()
{
  
  Serial.begin(115200);     // baud rate
  millisDelay(1) ;          // allow the serial to init
  Serial.println();         // clean up a little
  
  int EEWatts;              // a temp
  EEPROM.begin(32);                 // this number in "begin()" is ESP8266. EEPROM is emulated so you need buffer to emulate.
  EEWatts = EEPROM.read(0);            // Should read 4 bytes for each of these thangs
  Serial.println(EEWatts);
  if ( isnan( EEWatts ) ) { EEWatts = 0; }  // Having issues during devel with "nan" (not a number) being written to EEPROM
  
  if ( EEWatts > -10 ) {
   EEWatts = WATTS_ENOUGH ;
   EEPROM.write(0,EEWatts) ;
   EEPROM.commit();
  } 
  Serial.println(EEWatts);
  wattsEnough = EEWatts;
  
  connectWiFi();        // This thing isn't any use without WiFi

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

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    Serial.println("ESP32 Web Server: New request received:");  // for debugging
    Serial.println("GET /");                                    // for debugging

    String html = handleRoot(); 
    request->send(200, "text/html", html );
    
    }
  );

  server.on("/get", HTTP_GET, [] (AsyncWebServerRequest *request) {
  String inputWatts = String(wattsEnough);
  String inputSeconds = String(WAIT_TIME);
  String inputMessage;
  String inputParam;
    // GET input1 value on <ESP_IP>/get?input1=<inputMessage>
  if (request->hasParam(PARAM_INPUT_1)) {
     inputMessage = request->getParam(PARAM_INPUT_1)->value();
     inputParam = PARAM_INPUT_1;
     wattsEnough = inputMessage.toInt();
     EEPROM.write(0, wattsEnough);
     EEPROM.commit();
   }
    // GET input2 value on <ESP_IP>/get?input2=<inputMessage>
   else if (request->hasParam(PARAM_INPUT_2)) {
     Serial.println("in 2nd Param");
     inputMessage = request->getParam(PARAM_INPUT_2)->value();
     inputParam = PARAM_INPUT_2;
   }
   else {
//      inputWatts = "No change to Input Watts";
  //    inputSeconds = "No change to timout seconds";
      inputMessage = "No message";
      inputParam = "None";
    }
    
    Serial.println(inputMessage);
    Serial.println(inputParam);
    request->send(200, "text/html", "HTTP GET request sent to your ESP on input field (" 
                                     + inputParam + ") with value: " + inputMessage +
                                     "<br><br><a href=\"/\">Return to Home Page</a>");
  });
  server.onNotFound( notFound );
  
  server.on("/reboot", HTTP_GET, [](AsyncWebServerRequest* request) {
    // String html = "Rebooting in 5 seconds"; 
    request->send(200, "text/html", "Rebooting in 5 seconds" );;
    Serial.println( "In reboot Device" );
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

}       // Setup

//------------------------------------------------------

void loop() {

  // server.handleClient();                         // Listen for HTTP requests from clients
  ArduinoOTA.handle();
  if ( millis() > lastRun + WAIT_TIME )   {       // only want this happening every 5 minutes (or so) 
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

       if (gridWatts < wattsEnough ) {      // When in export, the IW_GRID will be in negative
        contactorStatus = LOW;            // Relay is energised on LOW signal
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
 
if ( WiFi.status() == WL_CONNECTED) {            // No point doing this if connected
 return true;
}
else                                 // Attempt
{ 
 WiFi.mode(WIFI_STA);               // Station Mode
 WiFi.hostname( nodeName );         // This will show up in your DHCP server
 WiFi.disconnect();  
 WiFi.begin(LOCAL_SSID, LOCAL_PASSWD);
 
 WiFi.setTxPower(WIFI_POWER_8_5dBm);   // This is a C3 thing

 Serial.print("Connecting");
 while ( millis() < start + 5000) {    // 5 seconds to connect 
 
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
  response += "<b>This triggers at " + String(WAIT_TIME/60000) + " minute intervals and when the export power value reaches " + String( abs( WATTS_ENOUGH ) ) + " Watts</b>";
  response += "<p></p><table style=\"width:600\">";
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
  response += "<tr><td>Contactor Status </td><td><b>" + String( (contactorStatus ? "Off" : "On" ) ) + "</td></tr>";
  response += "<tr><td>Current trigger value (in watts)  </td><td><b>" + String(abs(wattsEnough)) + "</b></td></tr>";  // this value is negative in the code but don't want to confuse users
  response += "<tr><td>Current poll time in seconds </td><td><b>" + String( WAIT_TIME / 1000 ) + "</b></td></tr>";
  response += "</table>";
  response += "<br>";
  response += "<form action=\"/get\">";
  response += "Trigger value in watts: <input type=\"text\" name=\"watts\">";
  response += "<input type=\"submit\" value=\"Submit\">";
  response += "</form>";
  response += "<form action=\"/get\">";
  response += "Poll time in seconds: <input type=\"text\" name=\"seconds\">";
  response += "<input type=\"submit\" value=\"Submit\">";
  response += "</form><br>";
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
