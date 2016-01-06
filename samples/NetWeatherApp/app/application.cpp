// NetWeather
// Emil Mitev <quanttrom@gmail.com>

#include "configuration.h"
#include <SmingCore/SmingCore.h>
#include <Libraries/LiquidCrystal/LiquidCrystal.h>
#include <math.h>


#include "special_chars.h"

// Some Bug for now??
int __errno;

LiquidCrystal lcd(4,2,14,12,13,15);

Timer heartBeatTimer;
bool heartBeatState = true;

Timer stagingTimer;
Timer weatherTimer;
Timer secondsUpdaterTimer;

HttpClient httpWeather;

bool readyWeather = false;
bool readyTime = false;


void onGetWeather(HttpClient& client, bool successful);
void heartBeatBlink();
void WiFiConnected();
void WiFiFail();
void waitForData();
void showTime();
void secondsUpdater();
void getWeather();
void showWeather();
void refrest_data();
void reboot_system();
String capitalizeStartLetter(String s);
void onGotTime(NtpClient& client, time_t time);


// Update Time every hour
NtpClient ntpClient("pool.ntp.org", SECS_PER_HOUR, onGotTime);



struct display_data_struct {
	int weather_Temp;
	int weather_Humidity;
	String weather_Description;
	DateTime weather_SunRise;
	DateTime weather_SunSet;

} display_data;

enum tempUnit {
	Fahrenheit,
	Kelvin,
	Celcius,
};

enum fsm_states{
	state_refresh,
	state_waitingForData,
	state_readyToDisplay,
	state_showingTime,
	state_showingWeather,
	state_reboot
} current_state;


String capitalizeFirstLetter(String s) {
	// always capitalize first letter
	bool capitalize = true;

	for(int i=0; i<s.length() ;i++){
		if ( s[i] == ' ' ){
			capitalize = true;
		}else {
			if (capitalize) s[i]=toupper(s[i]);

			capitalize = false;

		}
	}
	return s;
}

DynamicJsonBuffer jsonConfigBuffer;
// Declare pointer in order to avoid initializing just yet
JsonObject* cfg= NULL;

void saveConfig(JsonObject& cfg_local);

JsonObject& getConfig(){

	if (fileExist(NETWEATHER_CONFIG_FILE))
	{
		int size = fileGetSize(NETWEATHER_CONFIG_FILE);
		char* jsonString = new char[size + 1];
		fileGetContent(NETWEATHER_CONFIG_FILE, jsonString, size + 1);

		JsonObject& config = jsonConfigBuffer.parseObject(jsonString);

		if (config.success())
		{

			return config;

		}

	}

	JsonObject& config=jsonConfigBuffer.createObject();

	JsonObject& network=jsonConfigBuffer.createObject();
	config["network"]=network;
	network["ssid"] = WIFI_SSID;
	network["password"] = WIFI_PWD;

	JsonObject& weather=jsonConfigBuffer.createObject();
	config["weather"]=weather;
	weather["city_id"]="6094817";
	weather["url"]="http://api.openweathermap.org/data/2.5/weather";
	weather["api_key"]="a5e4c3d55c24560c4327ca0a862897c5";
	weather["units"] = Celcius;
	weather["refresh_rate"]= 5*SECS_PER_MIN*1000;

	// Eastern Standard Time - EST
	config["time_zone"] = -5.0;

	config["display_time"] = 4*1000;


	saveConfig(config);

	return config;
}



void stateHandler(){
	// Localize the config
	JsonObject& cfg_local = *cfg;

	switch ( current_state ) {
		case state_refresh:
			// Request that we get data
			refrest_data();
			// Move onto next  in a recursive manner, probably not the best, but no timers involved
			current_state = state_waitingForData;
			stateHandler();
			break;
		case state_waitingForData:
			// Check every 0.5 second if we are ready to display data
			// waitForData will update to state showingTime when we have all the data needed
			stagingTimer.initializeMs(500, stateHandler).start();
			waitForData();
			break;
		case state_readyToDisplay:
			// Since this is our first state since we got all the needed data, change the display frequency
			stagingTimer.initializeMs(cfg_local["display_time"].as<int>(), stateHandler).start();
			// Move onto showingTime next
			current_state = state_showingTime;
			Serial.println("STATE: state_readyToDisplay");
		break;
		case state_showingTime:
			showTime();
			// Schedule an update of time every second so it looks like it's moving and repeat
			secondsUpdaterTimer.initializeMs(1000, secondsUpdater).start();
			// Set the next state to showingWeather
			current_state = state_showingWeather;
			Serial.println("STATE: state_showingTime");
			break;
		case state_showingWeather:
			// Since this is  after showingTime, stop the secondsUpdater
			secondsUpdaterTimer.stop();
			// Show the weather on the screen, set the next state and just sit tight for stagingTimer to call us again
			showWeather();
			current_state = state_showingTime;
			Serial.println("STATE: state_showingWeather");

			break;
		case state_reboot:
			reboot_system();
			break;
		default:
			// We shouldn't be here...
			current_state = state_reboot;
			break;
	}


	return;
}

void refrest_data(){
	// Localize the config
	JsonObject& cfg_local = *cfg;

	// getting Time is started by the NtpClient constructor

	// get the current weather, check why I need to do that by investigating NtpClient constructor and timegetter
	getWeather();
	// Setup how often we should get new weather info
	weatherTimer.initializeMs(cfg_local["weather"]["refresh_rate"], getWeather).start();
	return;
}

void reboot_system(){
	// save config
	// reboot system

}


void saveConfig(JsonObject& cfg_local){
	Serial.print("SAVING FOLLOWING JSON CONFIG TO FILE: ");
	cfg_local.printTo(Serial);
	Serial.println();


}

void init()
{
//	spiffs_mount(); // Mount file system, in order to work with files

	// Initialize local reference with configuration settings
	JsonObject& cfg_local=getConfig();
	// point global pointer to local reference
	cfg=&cfg_local;


	Serial.begin(SERIAL_BAUD_RATE); // 115200 by default
	Serial.systemDebugOutput(true); // Allow debug output to serial

	//Change CPU freq. to 160MHz
	System.setCpuFrequency(eCF_160MHz);
	Serial.print("CPU frequency: ");
	Serial.println((int)System.getCpuFrequency());

	// blink heartbeat LED so we know that board is running
	pinMode(HEARTBEAT_LED_PIN, OUTPUT);
	heartBeatTimer.initializeMs(1000, heartBeatBlink).start();

	// Setup Wifi AP
	WifiAccessPoint.enable(false);
	WifiStation.waitConnection(WiFiConnected, 30, WiFiFail);
  	WifiStation.config( cfg_local["network"]["ssid"].as<String>() , cfg_local["network"]["password"].as<String>() );
	WifiStation.enable(true);
	// The WiFiConnected function will start our FSM and request time + weather

	// set timezone -> difference from UTC
	SystemClock.setTimeZone(cfg_local["time_zone"].as<double>());

	// Add special LCD characters
	lcd.begin(16, 2);   // initialize the lcd for 16 chars 2 lines
	lcd.createChar(1, icon_termometer);
	lcd.createChar(2, icon_water);
	lcd.createChar(3, celsius);
	lcd.createChar(4, icon_clock);
	lcd.createChar(5, icon_wifi);

	//-------- Write characters on the display ------------------
	// NOTE: Cursor Position: (CHAR, LINE) start at 0
	lcd.clear();
	lcd.print("Connecting to");
	lcd.setCursor(15,0);
	lcd.print("\5");
	lcd.setCursor(0,1);
	lcd.print("WiFi ...");
	lcd.blink();




}

void onGotTime(NtpClient &client, time_t time){
	// We have this function so we do not show time displays until we have a time updated through NTP
	readyTime=true;
//	client.setAutoQuery(true);
	client.setAutoUpdateSystemClock(true);
	SystemClock.setTime(time,eTZ_UTC);

}

void WiFiConnected(){
	// Localize the config
	JsonObject& cfg_local = *cfg;

	lcd.clear();
	lcd.print("SSID:");
	lcd.print(WifiStation.getSSID());

	lcd.setCursor(0,1);
	lcd.print("IP:");
	lcd.print(WifiStation.getIP());
	lcd.noBlink();

	// Once we are connected to WiFi start the FSM
	current_state = state_refresh;
	stateHandler();
}

void WiFiFail(){
	lcd.clear();
	lcd.print("Still connecting");
	lcd.setCursor(0,1);
	lcd.print("to WiFi ...");
	lcd.blink();

	WifiStation.waitConnection(WiFiConnected, 10, WiFiFail); // Repeat and check again
}

void waitForData(){
	// Localize the config
	JsonObject& cfg_local = *cfg;
	lcd.clear();
	lcd.print("Updating Net");
	lcd.setCursor(0,1);
	lcd.print("Time & Weather...");
	lcd.blink();

	// Change to the next display when we have the time data
	if (readyWeather && readyTime ){
		current_state = state_readyToDisplay;
	} else {
		// Print a dot so we know we are waiting for data...
		Serial.print('.');
	}
}

void showTime(){
	// Localize the config
	JsonObject& cfg_local = *cfg;
	lcd.clear();
	lcd.noBlink();
	lcd.setCursor(2,0);
	lcd.print("\4 ");

	DateTime time_now = SystemClock.now();

	lcd.print(time_now.toShortTimeString(true));

	lcd.setCursor(0,1);
	lcd.print("R ");
	lcd.print(display_data.weather_SunRise.toShortTimeString());
	lcd.print(" S ");
	lcd.print(display_data.weather_SunSet.toShortTimeString());

}

void secondsUpdater(){

	lcd.setCursor(4,0);
	DateTime time_now = SystemClock.now();
	lcd.print(time_now.toShortTimeString(true));

}

void showWeather(){
	// Localize the config
	JsonObject& cfg_local = *cfg;
	lcd.clear();
	lcd.noBlink();
	lcd.print("\1 ");
	lcd.print(display_data.weather_Temp);
	lcd.print("\3");

	char temp_units;
	switch ( cfg_local["weather"]["units"].as<int>() ) {
		case Fahrenheit: temp_units = 'F'; break;
		case Kelvin: temp_units = 'K'; break;
		case Celcius: temp_units = 'C'; break;
		default: temp_units = 'C';
	}
	lcd.print(temp_units);
	lcd.setCursor(10,0);
	lcd.print("\2 ");
	lcd.print(display_data.weather_Humidity);
	lcd.print("%");
	lcd.setCursor(0,1);
	lcd.print(display_data.weather_Description);

}

void onGetWeather(HttpClient& client, bool successful) {
	// Localize the config
	JsonObject& cfg_local = *cfg;

	// TODO: Make sure this isn't a permanent change
	if (!successful ) {
		Serial.println("Couldn't get JSON. Resetting Timer for 10");
		weatherTimer.initializeMs(10000, getWeather).start();
	}

	String response = client.getResponseString();
	Serial.println("Server response: '" + response + "'");
	if (response.length() > 0)
	{
		DynamicJsonBuffer jsonBuffer;
		JsonObject& jsonWeather = jsonBuffer.parseObject(response);
		if (!jsonWeather.success()) { // JSON Parser failed
		      Serial.println("JSON Parser failed !!!");
		      return;
		  }


		display_data.weather_Temp = (int) lround ( jsonWeather["main"]["temp"].as<double>() );
		display_data.weather_Humidity = (int) jsonWeather["main"]["humidity"].as<int>();
		display_data.weather_Description = capitalizeFirstLetter( jsonWeather["weather"][0]["description"].as<String>() );

		// Adjust for timezone
		display_data.weather_SunRise = (long) jsonWeather["sys"]["sunrise"].as<long>() + (SECS_PER_HOUR * cfg_local["time_zone"].as<double>());
		display_data.weather_SunSet = (long) jsonWeather["sys"]["sunset"].as<long>() + (SECS_PER_HOUR * cfg_local["time_zone"].as<double>());

		Serial.println("===========");
		Serial.print("Temperature: ");
		Serial.println(display_data.weather_Temp);
		Serial.print("Humidity: ");
		Serial.println(display_data.weather_Humidity);
		Serial.print("Weather Description: ");
		Serial.println(display_data.weather_Description);
		Serial.print("Sunrise Time: ");
		Serial.println(display_data.weather_SunRise);
		Serial.print("Sunset Time: ");
		Serial.println(display_data.weather_SunSet);
		Serial.println("===========");
		Serial.print("HEAP Free size: ");
	    Serial.println(system_get_free_heap_size());
		Serial.println("===========");


		readyWeather = true;
	}
}



void getWeather(){


	// Localize the config
	JsonObject& cfg_local = *cfg;
	// Extract only weather related config
	JsonObject& cfg_weather = cfg_local["weather"];

	String temp_url_req;
	switch ( cfg_weather["units"].as<int>() ) {
		case Fahrenheit: temp_url_req = "&units=imperial"; break;
		case Kelvin: temp_url_req = ""; break;
		case Celcius: temp_url_req = "&units=metric"; break;
		default: temp_url_req = "&units=metric";
	}

	String url = cfg_weather["url"].as<String>() + "?id=" + cfg_weather["city_id"].as<String>() + temp_url_req  + "&appid=" + cfg_weather["api_key"].as<String>();
//	String url = "http://www.quanttrom.com/weather.json";


	httpWeather.downloadString( url ,onGetWeather);

}



void heartBeatBlink()
{

	digitalWrite(HEARTBEAT_LED_PIN, heartBeatState);
	heartBeatState = !heartBeatState;

}
