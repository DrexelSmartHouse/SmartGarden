/**************************************************************
 * Sleep an ESP8266 module and wake it up once a day to water
 * your garden. The module wakes up at the time specified by
 * SLEEP_COUNTS_FOR_ACTION definition
 * 
 * By Tim Lechman
 * 
 * Last edited: 3/15/2018
 **************************************************************/

#include <ESP8266WiFi.h>
ADC_MODE(ADC_VCC); //vcc read-mode

// Include API-Headers
extern "C" {
#include "ets_sys.h"
#include "os_type.h"
#include "osapi.h"
#include "mem.h"
#include "user_interface.h"
#include "cont.h"
}

#include <ArduinoJson.h>
#include <string.h>
#include <WiFiUdp.h>

// state definitions
#define STATE_COLDSTART 0
#define STATE_SLEEP_WAKE 1
#define STATE_WAKEUP_ACTION 4

#define SLEEP_TIME 60*60*1000000                        // sleep intervals in us
#define SLEEP_COUNTS_FOR_ACTION 9 //time that the device will connect to wifi every day
//#define SLEEP_COUNTS_FOR_BATT_CHECK 2 //2*24
//#define BATT_WARNING_VOLTAGE 2.4
#define WIFI_CONNECT_TIMEOUT_S 20

// RTC-MEM Adresses
#define RTC_BASE 65
#define RTC_STATE 66
#define RTC_WAKE_COUNT 67

#define VCC_ADJ 1.096

#define SERIAL_DEBUG

//WiFi credentials
//char SSID[] = "DRXL-SMARTHOUSE-2.4"; char PASS[] = "3kq8b3kq8b";
//char SSID[] = "Mr. Wifi"; char PASS[] = "joyoussocks146";
char SSID[] = "BG-Guest"; char PASS[] = "!!Guest$*";

// global variables
byte buf[10];
byte state;   // state variable
byte event = 0;
uint32_t sleepCount;
uint32_t time1, time2;
uint currentHour;
uint wateringTime = 19;

// Temporary buffer
uint32_t b = 0;

int i;

// Use your own API key by signing up for a free developer account.
// http://www.wunderground.com/weather/api/
#define WU_API_KEY "5829c7aa493fd1a2"

// US ZIP code
#define WU_LOCATION "19107"
#define WUNDERGROUND "api.wunderground.com"

// HTTP request
const char WUNDERGROUND_REQ[] =
    "GET /api/" WU_API_KEY "/conditions/q/" WU_LOCATION ".json HTTP/1.1\r\n"
    "User-Agent: ESP8266/0.1\r\n"
    "Accept: */*\r\n"
    "Host: " WUNDERGROUND "\r\n"
    "Connection: close\r\n"
    "\r\n";

// json buffer area
static char respBuf[4096];

unsigned int localPort = 2390;      // local port to listen for UDP packets

//Don't hardwire the IP address or we won't get the benefits of the pool. Lookup the IP address for the host name instead 
//IPAddress timeServer(129, 6, 15, 28); // time.nist.gov NTP server
IPAddress timeServerIP; // time.nist.gov NTP server address
const char* ntpServerName = "time.nist.gov";

const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message

byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets

// A UDP instance to let us send and receive packets over UDP
WiFiUDP udp;

WiFiClient client;

// prototypes
void determineWakeupState();
void connectToWiFi();
void goToSleep(byte STATE);
void getWeather() ;
bool parseWeather(char *json);
uint getTime();
void sendNTPpacket(IPAddress& address);

/**************************************************************
 * Function: setup
 * ------------------------------------------------------------ 
 * summary: Firmware runs directly from setup. Device wakes 
 * up, checks its state, then performs actions based on its 
 * current state.
 * parameters: void
 * return: void
 **************************************************************/
void setup() {
	WiFi.forceSleepBegin();  // send wifi to sleep to reduce power consumption
	yield();
	system_rtc_mem_read(RTC_BASE, buf, 2); // read 2 bytes from RTC-MEMORY

	// if serial is not initialized all following calls to serial end dead.
	#ifdef SERIAL_DEBUG
		Serial.begin(115200);
		delay(10);
		Serial.println();
		Serial.println();
		Serial.println(F("Started from reset"));
	#endif

	determineWakeupState();

	switch (state)  {
	//first run after power on
	case STATE_COLDSTART:
		#ifdef SERIAL_DEBUG
			Serial.println("Cold start state!!") ;
		#endif
		sleepCount = 0;
		goToSleep(STATE_WAKEUP_ACTION);
		break;

	//checks to see if it is time for the wakeup action and then goes back to sleep
	case STATE_SLEEP_WAKE:
		system_rtc_mem_read(RTC_WAKE_COUNT, &sleepCount, 4); // read counter
		sleepCount++;
		if (sleepCount % 24 == SLEEP_COUNTS_FOR_ACTION) {
			buf[0] = STATE_WAKEUP_ACTION;  // one more sleep required to wake with wifi on
			system_rtc_mem_write(RTC_STATE, buf, 1); // set state for next wakeup
			ESP.deepSleep(10, WAKE_RFCAL);
			yield();
		}
		//// check battery
		//if (sleepCount % SLEEP_COUNTS_FOR_BATT_CHECK == 0) {
		//	if ((float)ESP.getVcc()* VCC_ADJ < BATT_WARNING_VOLTAGE) {
		//		goToSleep(STATE_WAKEUP_ACTION);
		//	}
		//}
		//else {
		//	#ifdef SERIAL_DEBUG
		//		Serial.println("Battery OKAY.") ;
		//	#endif
		//}

		// no special event, go to sleep again
		goToSleep(STATE_SLEEP_WAKE);
		break;
	
	//connects to wifi, sets sleepCount to the current hour, and checks weather if it is time for the action
    case STATE_WAKEUP_ACTION:
		WiFi.forceSleepWake();
		delay(500);
		wifi_set_sleep_type(MODEM_SLEEP_T);
		WiFi.mode(WIFI_STA);
		yield();      
		connectToWiFi();

		// time to perform actions while wifi is on
		sleepCount = getTime();
		if (sleepCount == SLEEP_COUNTS_FOR_ACTION) {
			getWeather();
			parseWeather(respBuf);
		}

		// now re-initialize
		goToSleep(STATE_SLEEP_WAKE);
		break;

	}
	delay(1000);
}

/**************************************************************
 * Function: loop
 * ------------------------------------------------------------ 
 * summary: Dead loop function.
 * parameters: void
 * return: void
 **************************************************************/
void loop() {
	delay(10);
}

/**************************************************************
 * Function: determineWakeupState
 * ------------------------------------------------------------ 
 * summary: determines if this is the first time the device is 
 * waking up or if reset is due to sleep-wake
 * parameters: void
 * return: void
 **************************************************************/
void determineWakeupState() {
	if ((buf[0] != 0x55) || (buf[1] != 0xaa)) {  // cold start, magic number is not present
		state = STATE_COLDSTART;
		buf[0] = 0x55; buf[1] = 0xaa;
		system_rtc_mem_write(RTC_BASE, buf, 2);
	}
	else { // reset was due to sleep-wake
		system_rtc_mem_read(RTC_STATE, buf, 1);
		state = buf[0];
	}

	// now the restart cause is clear, handle the different states
	#ifdef SERIAL_DEBUG
		Serial.printf("State: %d\r\n", state);
	#endif
}

/**************************************************************
 * Function: connectToWiFi
 * ------------------------------------------------------------ 
 * summary: Basic WiFi connecting function. It stops if it 
 * times out.
 * parameters: void
 * return: void
 **************************************************************/
void connectToWiFi() {
	time1 = system_get_time();
	// Connect to WiFi network
	#ifdef SERIAL_DEBUG
		Serial.println();
		Serial.println();
		Serial.print("Connecting to ");
		Serial.println("wifi");
	#endif
	WiFi.begin(SSID, PASS);
	while (WiFi.status() != WL_CONNECTED) {
		delay(500);
		#ifdef SERIAL_DEBUG
			Serial.print(".");
		#endif
		time2 = system_get_time();
		if (((time2 - time1) / 1000000) > WIFI_CONNECT_TIMEOUT_S) { // wifi connection lasts too long, retry
			ESP.deepSleep(10, WAKE_RFCAL);
			yield();
		}
	}
	#ifdef SERIAL_DEBUG
		Serial.println("");
		Serial.println("WiFi connected");
	#endif
}

/**************************************************************
 * Function: goToSleep
 * ------------------------------------------------------------ 
 * summary: Writes to the sleep counter, sets the state for
 * next wakeup, and passes control back to background processes
 * parameters: byte nextState
 * return: void
 **************************************************************/
void goToSleep(byte nextState) {
	system_rtc_mem_write(RTC_WAKE_COUNT, &sleepCount, 4); // write counter
	buf[0] = nextState;
	system_rtc_mem_write(RTC_STATE, buf, 1);            // set state for next wakeup
	ESP.deepSleep(SLEEP_TIME, WAKE_RF_DISABLED);
	yield();                                            // pass control back to background processes to prepare sleep
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
		return;
	}
	// Terminate the C string
	respBuf[respLen++] = '\0';
	#ifdef SERIAL_DEBUG
		Serial.print(F("respLen "));
		Serial.println(respLen);
		Serial.println(respBuf);
	#endif
}

/**************************************************************
 * Function: parseWeather
 * ------------------------------------------------------------ 
 * summary: parses the json and sends requested data over 
 * serial
 * parameters: char *json
 * return: bool
 **************************************************************/
bool parseWeather(char *json) {
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
	const char *observation_time = current["observation_time_rfc822"];
	#ifdef SERIAL_DEBUG
		Serial.println(precip_today_in);
		Serial.println(observation_time);
	#endif

	if (precip_today_in.toFloat() > 0.01) {
		#ifdef SERIAL_DEBUG
			Serial.println("It will rain today. Bring an umbrella!!") ;
		#endif
	}
	return true;
}

/**************************************************************
* Function: getTime
* ------------------------------------------------------------
* summary: gets the current time from the internet
* parameters: void
* return: uint currentHour
**************************************************************/
uint getTime() {
	currentHour = 25;
	Serial.println("Starting UDP");
	udp.begin(localPort);
	Serial.print("Local port: ");
	Serial.println(udp.localPort());

	//get a random server from the pool
	WiFi.hostByName(ntpServerName, timeServerIP);

	sendNTPpacket(timeServerIP); // send an NTP packet to a time server
								 // wait to see if a reply is available
	delay(1000);

	int cb = udp.parsePacket();
	if (!cb) {
		Serial.println("no packet yet");
	}
	else {
		Serial.print("packet received, length=");
		Serial.println(cb);
		// We've received a packet, read the data from it
		udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer

		//the timestamp starts at byte 40 of the received packet and is four bytes,
		// or two words, long. First, esxtract the two words:

		unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
		unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
		// combine the four bytes (two words) into a long integer
		// this is NTP time (seconds since Jan 1 1900):
		unsigned long secsSince1900 = highWord << 16 | lowWord;
		Serial.print("Seconds since Jan 1 1900 = ");
		Serial.println(secsSince1900);

		// now convert NTP time into everyday time:
		Serial.print("Unix time = ");
		// Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
		const unsigned long seventyYears = 2208988800UL;
		// subtract seventy years:
		unsigned long epoch = secsSince1900 - seventyYears;
		// print Unix time:
		Serial.println(epoch);


		// print the hour, minute and second:
		Serial.print("The time is ");
		uint currentHour = (((epoch % 86400L) / 3600) + 20) % 24;
		Serial.print(currentHour); // print the hour (86400 equals secs per day)
		Serial.print(':');
		if (((epoch % 3600) / 60) < 10) {
			// In the first 10 minutes of each hour, we'll want a leading '0'
			Serial.print('0');
		}
		Serial.print((epoch % 3600) / 60); // print the minute (3600 equals secs per minute)
		Serial.print(':');
		if ((epoch % 60) < 10) {
			// In the first 10 seconds of each minute, we'll want a leading '0'
			Serial.print('0');
		}
		Serial.println(epoch % 60); // print the second
		Serial.println(currentHour);
		return currentHour;
	}
	
}

/**************************************************************
* Function: sendNTPpacket
* ------------------------------------------------------------
* summary: send an NTP request to the time server at the given
* address
* parameters: IPAddress& address
* return: void
**************************************************************/
void sendNTPpacket(IPAddress& address) {
	Serial.println("sending NTP packet...");
	// set all bytes in the buffer to 0
	memset(packetBuffer, 0, NTP_PACKET_SIZE);
	// Initialize values needed to form NTP request
	// (see URL above for details on the packets)
	packetBuffer[0] = 0b11100011;   // LI, Version, Mode
	packetBuffer[1] = 0;     // Stratum, or type of clock
	packetBuffer[2] = 6;     // Polling Interval
	packetBuffer[3] = 0xEC;  // Peer Clock Precision
							 // 8 bytes of zero for Root Delay & Root Dispersion
	packetBuffer[12] = 49;
	packetBuffer[13] = 0x4E;
	packetBuffer[14] = 49;
	packetBuffer[15] = 52;

	// all NTP fields have been given values, now
	// you can send a packet requesting a timestamp:
	udp.beginPacket(address, 123); //NTP requests are to port 123
	udp.write(packetBuffer, NTP_PACKET_SIZE);
	udp.endPacket();
}