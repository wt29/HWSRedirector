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
  #define NODENAME "HWSRedirector"                  // eg "Kitchen"  Required and UNIQUE per site.  Also used to find mdns eg NODENAME.local
  #define LOCAL_SSID "Your SSID"                                // How many you have defined
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
#define VERSION 0.2            // First Cut

#define C3MINI

#include <EEPROM.h>           // Going to save some Max/Min stuff
#define EEPROM_SIZE 32        // 4 bytes each for largestGust, timeofLargestGust

//Node and Network Setup

#ifdef C3MINI
 #include <WiFi.h>
 #include <ESPmDNS.h>
 #include <WebServer.h>
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
 WebServer server(80);
#else
 ESP8266WiFiMulti wifiMulti;      // Create an instance of the ESP8266WiFiMulti class, called 'wifiMulti'
 ESP8266WebServer server(80);     // Create a webserver object that listens for HTTP request on port 80
#endif
WiFiClient client;                // Instance of WiFi Client

void handleRoot();                // function prototypes for HTTP handlers
void handleNotFound();            // Something it don't understand
void rebootDevice();              // Kick it over remotely
void millisDelay(long unsigned int);
bool connectWiFi();
void handleRebootDevice();
String getInternetTime();

int gridWatts;
int wattsEnough;
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


//-----------------------------------------
void setup()
{
  
  Serial.begin(115200);     // baud rate
  millisDelay(1) ;          // allow the serial to init
  Serial.println();         // clean up a little

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

  server.on("/", handleRoot);                   // Call the 'handleRoot' function when a client requests URI "/"
  server.onNotFound(handleNotFound);            // When a client requests an unknown URI (i.e. something other than "/"), call function "handleNotFound"
  server.on("/reboot", handleRebootDevice);     // Kick over remotely
  server.begin();                               // Actually start the server
  
  Serial.println("HTTP server started");
  ArduinoOTA.begin();                           // Remote updates
  ArduinoOTA.setHostname( nodeName );

  timeClient.begin();
  startTime = getInternetTime();
  startAbsoluteTime = timeClient.getEpochTime();

}       // Setup

//------------------------------------------------------

void loop() {

  server.handleClient();                         // Listen for HTTP requests from clients
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
        gridWatts = API_payload.substring(API_payload.indexOf(',')+ 1).toInt();
#else        
        // Required bits to extract Shelly GRID payload.
        // This is a sample Shelly EM outpu
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

       if (gridWatts < WATTS_ENOUGH) {      // When in export, the IW_GRID will be in negative
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

void handleRoot() {
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
  response += "</table>";
  server.send(200, "text/html", response );   // Send HTTP status 200 (Ok) and send some text to the browser/client
}

void handleNotFound() {
  server.send(404, "text/plain", "404: Not found"); // Send HTTP status 404 (Not Found) when there's no handler for the URI in the request
}


void handleRebootDevice() {
  Serial.println( "In reboot Device" );
  server.send(200, "text/html", "<h1>Rebooting " + String(nodeName) + " in 5 seconds</h1>"); // Warn em
  millisDelay( 5000 );
  ESP.restart();
}


String getInternetTime() {
  timeClient.update();
  return String( timeClient.getFormattedTime() );

}

String fullDate ( unsigned long epoch ) {
  static unsigned char month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  static unsigned char week_days[7] = {4, 5, 6, 0, 1, 2, 3}; //Thu=4, Fri=5, Sat=6, Sun=0, Mon=1, Tue=2, Wed=3
  unsigned char ntp_hour, ntp_minute, ntp_second, ntp_week_day, ntp_date, ntp_month, leap_days=0 ;
  String dow, sMonth;
  unsigned short temp_days;
  unsigned int ntp_year, days_since_epoch, day_of_year;

  ntp_second = epoch % 60;
  epoch /= 60;
  ntp_minute = epoch % 60;
  epoch /= 60;
  ntp_hour  = epoch % 24;
  epoch /= 24;

  days_since_epoch = epoch;                       //number of days since epoch
  ntp_week_day = week_days[days_since_epoch % 7]; //Calculating WeekDay

  ntp_year = 1970 + (days_since_epoch / 365);     // ball parking year, may not be accurate!

  unsigned int i;
  for (i = 1972; i < ntp_year; i += 4)            // Calculating number of leap days since epoch/1970
    if (((i % 4 == 0) && (i % 100 != 0)) || (i % 400 == 0)) leap_days++;

  ntp_year = 1970 + ((days_since_epoch - leap_days) / 365); // Calculating accurate current year by (days_since_epoch - extra leap days)
  day_of_year = ((days_since_epoch - leap_days) % 365) + 1;


  if (((ntp_year % 4 == 0) && (ntp_year % 100 != 0)) || (ntp_year % 400 == 0))
  {
    month_days[1] = 29;   //February = 29 days for leap years
//    leap_year_ind = 1;    //if current year is leap, set indicator to 1
  }
  else month_days[1] = 28; //February = 28 days for non-leap years

  temp_days = 0;

  for (ntp_month = 0 ; ntp_month <= 11 ; ntp_month++) //calculating current Month
  {
    if (day_of_year <= temp_days) break;
    temp_days = temp_days + month_days[ntp_month];
  }

  temp_days = temp_days - month_days[ntp_month - 1]; //calculating current Date
  ntp_date = day_of_year - temp_days;


  switch (ntp_week_day) {

    case 0: dow = "Sunday";
      break;
    case 1: dow = "Monday" ;
      break;
    case 2: dow = "Tuesday";
      break;
    case 3: dow = "Wednesday";
      break;
    case 4: dow = "Thursday";
      break;
    case 5: dow = "Friday";
      break;
    case 6: dow = "Saturday";
      break;
    default: break;
  }

  switch (ntp_month) {

    case 1: sMonth = "January";
      break;
    case 2: sMonth = "February";
      break;
    case 3: sMonth = "March";
      break;
    case 4: sMonth = "April";
      break;
    case 5: sMonth = "May";
      break;
    case 6: sMonth = "June";
      break;
    case 7: sMonth = "July";
      break;
    case 8: sMonth = "August";
      break;
    case 9: sMonth = "September";
      break;
    case 10: sMonth = "October";
      break;
    case 11: sMonth = "November";
      break;
    case 12: sMonth = "December";
    default: break;
  }
  return String( dow + " " + ntp_date + " " + sMonth + " " + ntp_hour + ":" + ntp_minute + ":" + ntp_second );
}

// Wait around for a bit
void millisDelay ( long unsigned int mDelay )
{
  long unsigned int now = millis();
  do {
    // Do nothing
  } while ( millis() < now + mDelay);

}
