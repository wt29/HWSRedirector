#define NODENAME "HWSRedirector"                 // eg "Kitchen"  Required and UNIQUE per site.  Also used to find mdns eg NODENAME.local
#define LOCAL_SSID "chapman"                                // How many you have defined
#define LOCAL_PASSWD  "BigFurryCat"
#define IW_GRID "http://192.168.1.100/query?select=[time.local.iso,Mains.Watts]&begin=m-1m&end=m&group=m&format=csv"
#define WATTS_ENOUGH  -2200                       // When there are enough export watts to trigger the contactor Negative is EXPORT value
#define WAIT_TIME     300000                      // This is in milliseconds so 300000 is 60x5x1000