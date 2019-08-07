/*
 *  Copyright (c) 2019 - 2029 MaiKe Labs
 *
 *  Based on the Arduino_LoRa_Demo_Sensor sketch
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

#include <SPI.h>
#include <Wire.h>
#include "sx1272.h"
#include "vbat.h"
#include "gps.h"
#include "sht2x.h"

//#define USE_SI2301				1
//#define node_addr				249
#define node_addr				250

#define ENABLE_GPS			1
//#define ENABLE_SHT2X		1
//#define DISABLE_SX1278		1

#define ENABLE_CAD			1

#define DEST_ADDR				2

//#define LOW_POWER				1

#define MAX_DBM			20
#define TXRX_CH			CH_00_441

///////////////////////////////////////////////////////////////////
//#define WITH_EEPROM
//#define WITH_APPKEY
//#define WITH_ACK
//#define LOW_POWER_TEST
///////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////
// CHANGE HERE THE LORA MODE
#define LORAMODE		11	// BW=125KHz, SF=12, CR=4/5, sync=0x34
//////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////
// CHANGE HERE THE TIME IN SECONDS BETWEEN 2 READING & TRANSMISSION
unsigned int idlePeriod = 6;	// 6 seconds
///////////////////////////////////////////////////////////////////

#ifdef WITH_APPKEY
// CHANGE HERE THE APPKEY, BUT IF GW CHECKS FOR APPKEY, MUST BE
// IN THE APPKEY LIST MAINTAINED BY GW.
uint8_t my_appKey[4] = { 5, 6, 8, 8 };
#endif

///////////////////////////////////////////////////////////////////
// IF YOU SEND A LONG STRING, INCREASE THE SIZE OF MESSAGE
uint8_t message[64];
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

#ifdef LOW_POWER
#define	LOW_POWER_PERIOD	8
// you need the LowPower library from RocketScream
// https://github.com/rocketscream/Low-Power
#include "LowPower.h"
unsigned int nCycle = idlePeriod / LOW_POWER_PERIOD;
#endif

unsigned long next_tx = 0L;

#ifdef WITH_EEPROM
struct sx1272config {

	uint8_t flag1;
	uint8_t flag2;
	uint8_t seq;
	// can add other fields such as LoRa mode,...
};

sx1272config my_sx1272config;
#endif

/* only support .0001 */
char *ftoa(char *a, float f, int preci)
{
	long p[] =
	    {0, 10, 100, 1000, 10000};

	char *ret = a;

	long ipart = (long)f;

	//INFOLN("%d", ipart);

	itoa(ipart, a, 10);		//int16, -32,768 ~ 32,767 

	while (*a != '\0')
		a++;

	*a++ = '.';

	long fpart = abs(f * p[preci] - ipart * p[preci]);

	//INFOLN("%d", fpart);

	if (fpart > 0) {
		if (fpart < p[preci]/10) {
			*a++ = '0';
		}
		if (fpart < p[preci]/100) {
			*a++ = '0';
		}
		if (fpart < p[preci]/1000) {
			*a++ = '0';
		}
	}

	itoa(fpart, a, 10);
	return ret;
}

void power_on_dev()
{
#ifndef USE_SI2301
	digitalWrite(6, HIGH);
#else
	digitalWrite(7, LOW);
#endif
}

void power_off_dev()
{
#ifndef USE_SI2301
	digitalWrite(6, LOW);
#else
	digitalWrite(7, HIGH);
#endif
}

void setup()
{
	int e;

#ifndef USE_SI2301
	pinMode(6, OUTPUT);
#else
	pinMode(7, OUTPUT);
#endif

	// Open serial communications and wait for port to open:
#ifdef ENABLE_GPS
	Serial.begin(9600);
#else
	Serial.begin(115200);
#endif

	// Print a start message
	INFO_S("%s", "Noduino Quark LoRa Node\n");

// See http://www.nongnu.org/avr-libc/user-manual/using_tools.html
// for the list of define from the AVR compiler
#ifdef __AVR_ATmega328P__
	INFO_S("%s", "ATmega328P detected\n");
#endif

	power_on_dev();		// turn on device power

#ifdef ENABLE_SHT2X
	sht2x_init();		// initialization of the sensor
#endif

#ifndef DISABLE_SX1278
	sx1272.sx1278_qsetup(TXRX_CH, MAX_DBM);

	sx1272.setNodeAddress(node_addr);

#ifdef ENABLE_CAD
	sx1272._enableCarrierSense = true;
#endif

#endif

#ifdef ENABLE_GPS
	gps_setup();
#endif
}

void qsetup()
{
#ifdef ENABLE_SHT2X
	sht2x_init();	// initialization of the sensor
#endif

#if 0
#ifndef DISABLE_SX1278
	sx1272.ON();		// power on the module

	// BW=125KHz, SF=12, CR=4/5, sync=0x34
	sx1272.setMode(LORAMODE);

	// Select frequency channel
	sx1272.setChannel(DEFAULT_CHANNEL);

#ifdef PABOOST
	// Select amplifier line; PABOOST or RFO
	sx1272._needPABOOST = true;
#endif

	sx1272.setPowerDBM((uint8_t) MAX_DBM);

	// Set the node address and print the result
	sx1272.setNodeAddress(node_addr);

#ifdef ENABLE_CAD
	// enable carrier sense
	sx1272._enableCarrierSense = true;
#endif
#endif
#endif

#ifndef DISABLE_SX1278
	sx1272.sx1278_qsetup(CH_00_433, 20);
	sx1272.setNodeAddress(node_addr);

#ifdef ENABLE_CAD
	sx1272._enableCarrierSense = true;
#endif
#endif

#ifdef ENABLE_GPS
	gps_setup();
#endif
}

void get_pos()
{
	// Get a valid position from the GPS
	int valid_pos = 0;

	uint32_t timeout = millis();
	do {
		if (Serial.available())
		valid_pos = gps_decode(Serial.read());
	} while ((millis() - timeout < 2000) && ! valid_pos) ;

	if (valid_pos) {
	}
}

void loop(void)
{
	long startSend;
	long endSend;
	uint8_t app_key_offset = 0;
	int e;
	float temp = 0, humi = 0, vbat;

#ifndef LOW_POWER
	if (millis() > next_tx) {
#endif

#ifdef ENABLE_SHT2X
		temp = sht2x_get_temp();
		humi = sht2x_get_humi();
#endif
		vbat = get_vbat();

#ifdef ENABLE_GPS
		get_pos();
#endif

#ifdef WITH_APPKEY
		app_key_offset = sizeof(my_appKey);
		// set the app key in the payload
		memcpy(message, my_appKey, app_key_offset);
#endif

		uint8_t r_size;

		// the recommended format if now \!TC/22.5
		char vbat_s[10], temp_s[10], humi_s[10];

		ftoa(vbat_s, vbat, 2);
		ftoa(temp_s, temp, 2);
		ftoa(humi_s, humi, 0);

#ifdef ENABLE_GPS
		char lat_s[12], lon_s[12], alt_s[10];

		ftoa(lat_s, gps_lat, 4);
		ftoa(lon_s, gps_lon, 4);
		ftoa(alt_s, gps_altitude, 0);

		// this is for testing, uncomment if you just want to test, without a real pressure sensor plugged
		//strcpy(vbat_s, "noduino");
		r_size = sprintf((char *)message + app_key_offset, "\\!U/%s/T/%s/H/%s/lat/%s/lon/%s/alt/%s",
					vbat_s, temp_s, humi_s, lat_s, lon_s, alt_s);
#else
		r_size = sprintf((char *)message + app_key_offset, "\\!U/%s/T/%s/H/%s",
					vbat_s, temp_s, humi_s);
#endif

		INFO_S("%s", "Sending ");
		INFOLN("%s", (char *)(message + app_key_offset));

		INFO_S("%s", "Real payload size is ");
		INFOLN("%d", r_size);

		int pl = r_size + app_key_offset;

#if 0
		float a=39.8822, b=-24.3334, c=119.62, d=12.0045, e=12.035, f=119.0001;

		INFOLN("%f", a);

		ftoa(lat_s, a, 3);
		INFO_S("%s", "lat = ");
		INFOLN("%s", lat_s);

		ftoa(lat_s, b, 3);
		INFO_S("%s", "lat = ");
		INFOLN("%s", lat_s);

		ftoa(lat_s, c, 4);
		INFO_S("%s", "lat = ");
		INFOLN("%s", lat_s);

		ftoa(lat_s, d, 4);
		INFO_S("%s", "lat = ");
		INFOLN("%s", lat_s);

		ftoa(lat_s, e, 2);
		INFO_S("%s", "lat = ");
		INFOLN("%s", lat_s);

		ftoa(lat_s, f, 4);
		INFO_S("%s", "lat = ");
		INFOLN("%s", lat_s);
#endif

#ifdef ENABLE_CAD
		sx1272.CarrierSense();
#endif

		startSend = millis();

#ifndef DISABLE_SX1278
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
#endif

#ifdef LOW_POWER
		INFO_S("%s", "Switch to power saving mode\n");

#ifndef DISABLE_SX1278
		e = sx1272.setSleepMode();
		if (!e)
			INFO_S("%s", "Successfully switch LoRa into sleep mode\n");
		else
			INFO_S("%s", "Could not switch LoRa into sleep mode\n");
#endif

		//sx1272.reset();
		//sx1272.OFF();

		digitalWrite(SX1272_RST, LOW);

		SPI.end();
		digitalWrite(10, LOW);
		digitalWrite(11, LOW);
		digitalWrite(12, LOW);
		digitalWrite(13, LOW);

		FLUSHOUTPUT
		delay(50);

		Wire.end();
		digitalWrite(A4, LOW);	// SDA
		digitalWrite(A5, LOW);	// SCL

		power_off_dev();

		for (int i = 0; i < nCycle; i++) {

			// ATmega328P, ATmega168, ATmega32U4
			LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);

			//INFO_S("%s", ".");
			FLUSHOUTPUT delay(10);
		}

		delay(50);

		power_on_dev();
		delay(100);
		qsetup();
#else
		INFOLN("%ld", next_tx);
		INFO_S("%s", "Will send next value at\n");

		next_tx = millis() + (unsigned long)idlePeriod * 1000;

		INFOLN("%ld", next_tx);
	}
#endif
}
