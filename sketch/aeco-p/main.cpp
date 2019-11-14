/*
 *  Copyright (c) 2019 - 2029 MaiKe Labs
 *
 *	This program is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, either version 3 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
*/

#include "softspi.h"
#include "sx1272.h"
#include "vbat.h"
#include "softi2c.h"
#include "pc10.h"
//#include "U8g2lib.h"
#include "rtcdriver.h"

/* Timer used for bringing the system back to EM0. */
RTCDRV_TimerID_t xTimerForWakeUp;

/* 30s */
static uint32_t sensor_period = 10;

//#define ENABLE_CAD			1

#define node_addr		106

#define DEST_ADDR		1

#define USE_SX1278		1

//#define ENABLE_SSD1306		1
//#define LOW_POWER

#define MAX_DBM			20
#define TXRX_CH			CH_00_470

///////////////////////////////////////////////////////////////////
//#define WITH_EEPROM
//#define WITH_APPKEY
//#define WITH_ACK
///////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////
// CHANGE HERE THE LORA MODE
#define LORAMODE		11	// BW=125KHz, SF=12, CR=4/5, sync=0x34
//////////////////////////////////////////////////////////////////

#ifdef WITH_APPKEY
// CHANGE HERE THE APPKEY, BUT IF GW CHECKS FOR APPKEY, MUST BE
// IN THE APPKEY LIST MAINTAINED BY GW.
uint8_t my_appKey[4] = { 5, 6, 8, 8 };
#endif

///////////////////////////////////////////////////////////////////
// IF YOU SEND A LONG STRING, INCREASE THE SIZE OF MESSAGE
uint8_t message[50];
///////////////////////////////////////////////////////////////////

#define INFO_S(fmt,param)			Serial.print(F(param))
#define INFO(fmt,param)				Serial.print(param)
#define INFOLN(fmt,param)			Serial.println(param)
#define FLUSHOUTPUT					Serial.flush();

#ifdef WITH_EEPROM
#include <EEPROM.h>
#endif

#ifdef WITH_ACK
#define	NB_RETRIES			2
#endif

#ifdef WITH_EEPROM
struct sx1272config {

	uint8_t flag1;
	uint8_t flag2;
	uint8_t seq;
	// can add other fields such as LoRa mode,...
};

sx1272config my_sx1272config;
#endif

void push_data(float pres);

#ifdef ENABLE_SSD1306
U8G2_SSD1306_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
//U8G2_SSD1306_128X64_NONAME_1_SW_I2C u8g2(U8G2_R0, SCL, SDA, U8X8_PIN_NONE);
#endif

char *ftoa(char *a, double f, int precision)
{
	long p[] =
	    { 0, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000 };

	char *ret = a;
	long heiltal = (long)f;
	itoa(heiltal, a, 10);
	while (*a != '\0')
		a++;
	*a++ = '.';
	long desimal = abs((long)((f - heiltal) * p[precision]));
	if (desimal < p[precision - 1]) {
		*a++ = '0';
	}
	itoa(desimal, a, 10);
	return ret;
}

void power_on_dev()
{
	digitalWrite(10, HIGH);
}

void power_off_dev()
{
	digitalWrite(10, LOW);
}

#ifdef ENABLE_SSD1306
void draw_press(int32_t p)
{
	u8g2.setPowerSave(0);

	//u8g2.clearBuffer();		// clear the internal memory

	u8g2.setFont(u8g2_font_logisoso24_tf);	// choose a suitable font

	u8g2.firstPage();

	do {
		u8g2.drawStr(98, 48, "Pa");		// write something to the internal memory
		u8g2.setCursor(16, 48);
		u8g2.print(p);
	} while (u8g2.nextPage());

	delay(2000);

	u8g2.setPowerSave(1);

	//u8g2.sendBuffer();		// transfer internal memory to the display
}
#endif

void sensor_check()
{
	static float old_pres = 0.0;
	float pres = 0.0;
	Ecode_t e;

	INFOLN("%s", "Checking the sensor data...");

	power_on_dev();		// turn on device power
	pressure_init();	// initialization of the sensor

	pres = get_pressure();

	power_off_dev();

	if (abs(pres - old_pres) > 10.0) {

		push_data(pres);

		old_pres = pres;

	} else {

		//digitalWrite(SX1272_RST, LOW);
		spi_end();
		wire_end();
	}

	RTCDRV_StartTimer(xTimerForWakeUp, rtcdrvTimerTypeOneshot, sensor_period * 1000, sensor_check, NULL);
	//EMU_EnterEM2(true);
}


void setup()
{
	Ecode_t e;

	// dev power ctrl
	pinMode(10, OUTPUT);

	Serial.setRouteLoc(1);
	Serial.begin(115200);

#ifdef ENABLE_SSD1306
	float pres = get_pressure();

	u8g2.begin();

	draw_press((int32_t) pres);
#endif

	/* Initialize RTC timer. */
	RTCDRV_Init();
	RTCDRV_AllocateTimer(&xTimerForWakeUp);

	//INFO("%s", "Clock Freq = ");
	//INFOLN("%d", CMU_ClockFreqGet(cmuClock_CORE));

	//sensor_check();

	RTCDRV_StartTimer(xTimerForWakeUp, rtcdrvTimerTypeOneshot, sensor_period * 1000, sensor_check, NULL);
}

void qsetup()
{
	vbat_adc_init();

	power_on_dev();		// turn on device power

	//pressure_init();	// initialization of the sensor

	sx1272.sx1278_qsetup(TXRX_CH, MAX_DBM);
	sx1272.setNodeAddress(node_addr);

#ifdef ENABLE_CAD
	sx1272._enableCarrierSense = true;
#endif

#ifdef ENABLE_SSD1306
	u8g2.begin();
#endif
}

void push_data(float pres)
{
	long startSend;
	long endSend;

	float vbat = 0.0;

	uint8_t app_key_offset = 0;
	int e;

	qsetup();

	vbat = get_vbat();
	// pres = get_pressure();

#ifdef WITH_APPKEY
	app_key_offset = sizeof(my_appKey);
	// set the app key in the payload
	memcpy(message, my_appKey, app_key_offset);
#endif

	uint8_t r_size;

	// the recommended format if now \!TC/22.5
	char vbat_s[10], pres_s[10];
	ftoa(vbat_s, vbat, 2);
	ftoa(pres_s, pres, 2);

	r_size = sprintf((char *)message + app_key_offset, "\\!U/%s/P/%s", vbat_s, pres_s);

	INFO_S("%s", "Sending ");
	INFOLN("%s", (char *)(message + app_key_offset));

	INFO_S("%s", "Real payload size is ");
	INFOLN("%d", r_size);

	int pl = r_size + app_key_offset;

#ifdef ENABLE_CAD
	sx1272.CarrierSense();
#endif

	startSend = millis();

#ifdef WITH_APPKEY
	// indicate that we have an appkey
	sx1272.setPacketType(PKT_TYPE_DATA | PKT_FLAG_DATA_WAPPKEY);
#else
	// just a simple data packet
	sx1272.setPacketType(PKT_TYPE_DATA);
#endif

	// Send message to the gateway and print the result
	// with the app key if this feature is enabled
#ifdef WITH_ACK
	int n_retry = NB_RETRIES;

	do {
		e = sx1272.sendPacketTimeoutACK(DEST_ADDR,
						message, pl);

		if (e == 3)
			INFO_S("%s", "No ACK");

		n_retry--;

		if (n_retry)
			INFO_S("%s", "Retry");
		else
			INFO_S("%s", "Abort");

	} while (e && n_retry);
#else
	e = sx1272.sendPacketTimeout(DEST_ADDR, message, pl);
#endif
	endSend = millis();

#ifdef WITH_EEPROM
	// save packet number for next packet in case of reboot
	my_sx1272config.seq = sx1272._packetNumber;
	EEPROM.put(0, my_sx1272config);
#endif

	INFO_S("%s", "LoRa pkt size ");
	INFOLN("%d", pl);

	INFO_S("%s", "LoRa pkt seq ");
	INFOLN("%d", sx1272.packet_sent.packnum);

	INFO_S("%s", "LoRa Sent in ");
	INFOLN("%ld", endSend - startSend);

	INFO_S("%s", "LoRa Sent w/CAD in ");
	INFOLN("%ld", endSend - sx1272._startDoCad);

	INFO_S("%s", "Packet sent, state ");
	INFOLN("%d", e);

	INFO_S("%s", "Remaining ToA is ");
	INFOLN("%d", sx1272.getRemainingToA());

	e = sx1272.setSleepMode();
	if (!e)
		INFO_S("%s", "Successfully switch LoRa into sleep mode\n");
	else
		INFO_S("%s", "Could not switch LoRa into sleep mode\n");

	digitalWrite(SX1272_RST, LOW);

	spi_end();

	power_off_dev();

	wire_end();
}

void loop()
{
	//INFO("%s", "Clock Freq = ");
	//INFOLN("%d", CMU_ClockFreqGet(cmuClock_CORE));

	INFOLN("%s", "Something is wrong, you can not go here...");

	delay(5000);
}