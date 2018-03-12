/**************************************************************
 * Proof of functionality for the system:
 * Tries to connect to last internet connection. If it doesn't
 * work, it creates an access point to allow the user to enter
 * credentials for a network. Once connected, it chcecks the
 * weather on WU and sends a message over serial if it will 
 * rain
 **************************************************************/

#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino

#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>         //https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h>
#include <string.h>


// Use your own API key by signing up for a free developer account.
// http://www.wunderground.com/weather/api/
#define WU_API_KEY "5829c7aa493fd1a2"

// US ZIP code
#define WU_LOCATION "19107"

// 5 minutes between update checks. The free developer account has a limit
// on the  number of calls so don't go wild.
#define DELAY_NORMAL    (5*60*1000)
// 20 minute delay between updates after an error
#define DELAY_ERROR     (20*60*1000)

#define WUNDERGROUND "api.wunderground.com"

#define SERIAL_DEBUG          // This line toggles serial debugging
#define PRINT_JSON            // This line toggles the printing of the full json


// HTTP request
const char WUNDERGROUND_REQ[] =
    "GET /api/" WU_API_KEY "/conditions/q/" WU_LOCATION ".json HTTP/1.1\r\n"
    "User-Agent: ESP8266/0.1\r\n"
    "Accept: */*\r\n"
    "Host: " WUNDERGROUND "\r\n"
    "Connection: close\r\n"
    "\r\n";


// json bufferr area
static char respBuf[4096];


// protos
void getWeather() ;
bool showWeather(char *json);

/**************************************************************
 * Function: setup
 * ------------------------------------------------------------ 
 * summary: Uses wifiManager to create a wifi connection using 
 * past credentials and if they don't work an access point is 
 * opened to add new credentials
 * parameters: void
 * return: void
 **************************************************************/
void setup() {

    #ifdef SERIAL_DEBUG
    Serial.begin(115200);
    #endif

    //WiFiManager
    //Local intialization. Once its business is done, there is no need to keep it around
    WiFiManager wifiManager;
    #ifndef SERIAL_DEBUG
    wifiManager.setDebugOutput(false);
    #endif

    //reset saved settings
    //wifiManager.resetSettings();
    
    //set custom ip for portal
    wifiManager.setAPStaticIPConfig(IPAddress(10,0,1,1), IPAddress(10,0,1,1), IPAddress(255,255,255,0));

    //fetches ssid and pass from eeprom and tries to connect
    //if it does not connect it starts an access point with the specified name
    //and goes into a blocking loop awaiting configuration
    wifiManager.autoConnect("AutoConnectAP");
    
    //if you get here you have connected to the WiFi
    #ifdef SERIAL_DEBUG
    Serial.println("connected...yeey :)");
    #endif
}

/**************************************************************
 * Function: loop
 * ------------------------------------------------------------ 
 * summary: calls getWeather and showWeather
 * parameters: void
 * return: void
 **************************************************************/
void loop() {

  getWeather() ;

  if (showWeather(respBuf)) {
    delay(DELAY_NORMAL);
  }
  else {
    delay(DELAY_ERROR);
  }
}

/**************************************************************
 * Function: getWeather
 * ------------------------------------------------------------ 
 * summary: opens socket to WU and requests weather data, when
 * data is received it stores it in respBuf
 * parameters: void
 * return: void
 **************************************************************/
void getWeather() {
  // Use WiFiClient class to create TCP connections
  // Open socket to WU server port 80
  #ifdef SERIAL_DEBUG
  Serial.print(F("Connecting to "));
  Serial.println(WUNDERGROUND);
  #endif
  WiFiClient httpclient;
  const int httpPort = 80;
  if (!httpclient.connect(WUNDERGROUND, httpPort)) {
    #ifdef SERIAL_DEBUG
    Serial.println(F("connection failed"));
    #endif
    delay(DELAY_ERROR);
    return;
  }

  // This will send the http request to the server
  #ifdef SERIAL_DEBUG
  Serial.print(WUNDERGROUND_REQ);
  #endif
  httpclient.print(WUNDERGROUND_REQ);
  httpclient.flush();

  // Collect http response headers and content from Weather Underground
  // HTTP headers are discarded.
  // The content is formatted in JSON and is left in respBuf.
  int respLen = 0;
  bool skip_headers = true;
  while (httpclient.connected() || httpclient.available()) {
    if (skip_headers) {
      String aLine = httpclient.readStringUntil('\n');
      //Serial.println(aLine);
      // Blank line denotes end of headers
      if (aLine.length() <= 1) {
        skip_headers = false;
      }
    }
    else {
      int bytesIn;
      bytesIn = httpclient.read((uint8_t *)&respBuf[respLen], sizeof(respBuf) - respLen);
      #ifdef SERIAL_DEBUG
      Serial.print(F("bytesIn ")); Serial.println(bytesIn);
      #endif
      if (bytesIn > 0) {
        respLen += bytesIn;
        if (respLen > sizeof(respBuf)) respLen = sizeof(respBuf);
      }
      else if (bytesIn < 0) {
        #ifdef SERIAL_DEBUG
        Serial.print(F("read error "));
        Serial.println(bytesIn);
        #endif
      }
    }
    delay(1);
  }
  httpclient.stop();

  if (respLen >= sizeof(respBuf)) {
    #ifdef SERIAL_DEBUG
    Serial.print(F("respBuf overflow "));
    Serial.println(respLen);
    #endif
    delay(DELAY_ERROR);
    return;
  }
  // Terminate the C string
  respBuf[respLen++] = '\0';
  #ifdef SERIAL_DEBUG
  Serial.print(F("respLen "));
  Serial.println(respLen);
  #ifdef PRINT_JSON
  Serial.println(respBuf);
  #endif
  #endif
}

/**************************************************************
 * Function: showWeather
 * ------------------------------------------------------------ 
 * summary: parses the json and sends requested data over 
 * serial
 * parameters: char *json
 * return: bool
 **************************************************************/
bool showWeather(char *json)
{
  StaticJsonBuffer<3*1024> jsonBuffer;

  // Skip characters until first '{' found
  // Ignore chunked length, if present
  char *jsonstart = strchr(json, '{');
  //Serial.print(F("jsonstart ")); Serial.println(jsonstart);
  if (jsonstart == NULL) {
    #ifdef SERIAL_DEBUG
    Serial.println(F("JSON data missing"));
    #endif
    return false;
  }
  json = jsonstart;

  // Parse JSON
  JsonObject& root = jsonBuffer.parseObject(json);
  if (!root.success()) {
    #ifdef SERIAL_DEBUG
    Serial.println(F("jsonBuffer.parseObject() failed"));
    #endif
    return false;
  }

  // Extract weather info from parsed JSON
  JsonObject& current = root["current_observation"];
  String precip_today_in = current["precip_today_in"];
  String observation_time = current["observation_time_rfc822"];
  String local_epoch = current["local_epoch"];
  #ifdef SERIAL_DEBUG
  Serial.println(precip_today_in);
  Serial.println(observation_time);
  Serial.println(local_epoch);
  #endif

  if (precip_today_in.toFloat() > 0.02) {
    #ifdef SERIAL_DEBUG
    Serial.println("It will rain today. Bring an umbrella!!") ;
    #endif
  }
  return true;
}
