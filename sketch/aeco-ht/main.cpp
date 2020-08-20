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
#include "softi2c.h"
#include "sht2x.h"
#include "sht3x.h"
#include "rtcdriver.h"
#include "math.h"
#include "em_wdog.h"

#define CONFIG_PROTO_V34		1

/* Timer used for bringing the system back to EM0. */
RTCDRV_TimerID_t xTimerForWakeUp;

static uint32_t sample_period = 18;		/* 20s */

static uint32_t sample_count = 0;
#define		HEARTBEAT_TIME			6600

static float old_temp = 0.0;
static float cur_temp = 0.0;
static float old_humi = 0.0;
static float cur_humi = 0.0;

static float cur_curr = 0.0;

//#define	TX_TESTING				1
//#define	DEBUG					1

//#define ENABLE_SHT2X			1
#define ENABLE_SHT3X			1

#ifdef ENABLE_SHT2X
#define	DELTA_HUMI				3
#elif ENABLE_SHT3X
#define	DELTA_HUMI				2
#endif

static uint32_t need_push = 0;

#define	PWR_CTRL_PIN			8		/* PIN17_PC14_D8 */
#define	KEY_PIN					0		/* PIN01_PA00_D0 */

#if 0
#define SDA_PIN					11		/* PIN14_PD7 */
#define SCL_PIN					16		/* PIN21_PF2 */
#else
#define SDA_PIN					12		/* PIN23_PE12 */
#define SCL_PIN					13		/* PIN24_PE13 */
#endif

#define ENABLE_CAD				1

#define	TX_TIME					1800
#define DEST_ADDR				1

#define	RESET_TX			0
#define	DELTA_TX			1
#define	TIMER_TX			2
#define	KEY_TX				3
#define	EL_TX				4
#define	WL_TX				5

uint8_t tx_cause = RESET_TX;

#ifdef CONFIG_V0
#define TXRX_CH				CH_01_472
#define LORA_MODE			12

#else
#define node_addr			107

#define TXRX_CH				CH_00_470
#define LORA_MODE			11
#endif

#define MAX_DBM				20

//#define WITH_ACK

#ifdef CONFIG_V0

#ifdef CONFIG_PROTO_V33
uint8_t message[32] = { 0x47, 0x4F, 0x33 };
#define TX_LEN			24
#elif CONFIG_PROTO_V34
uint8_t message[38] = { 0x47, 0x4F, 0x34 };
#define TX_LEN			38
#endif

uint16_t tx_count = 0;
#else
uint8_t message[32];
#endif

#ifdef DEBUG

#define INFO_S(param)			Serial.print(F(param))
#define INFOHEX(param)			Serial.print(param,HEX)
#define INFO(param)				Serial.print(param)
#define INFOLN(param)			Serial.println(param)
#define FLUSHOUTPUT				Serial.flush();

#else

#define INFO_S(param)
#define INFO(param)
#define INFOLN(param)
#define INFOHEX(param)
#define FLUSHOUTPUT

#endif

#ifdef WITH_ACK
#define	NB_RETRIES			2
#endif

void push_data();

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
	digitalWrite(PWR_CTRL_PIN, HIGH);
}

void power_off_dev()
{
	digitalWrite(PWR_CTRL_PIN, LOW);
}

float fetch_mcu_temp()
{
	float temp = 0.0;
	for(int i=0; i<3; i++) {
		temp += adc.temperatureCelsius();
	}
	temp /= 3.0;

	return temp;
}

float fetch_current()
{
	adc.reference(adcRef1V25);

	int ad = 0;

	for (int i = 0; i < 5; i++) {
		ad += adc.read(A6, A7);
	}

	cur_curr = 1250.0*ad/2.0/2048.0/0.7 / 5;

	INFO("ADC differential ch6 ch7 read:");
	INFOLN(ad);

	INFO("The consumption current (mA): ");

	return cur_curr;
}

void check_sensor(RTCDRV_TimerID_t id, void *user)
{
	(void)id;
	(void)user;

	WDOG_Feed();

	RTCDRV_StopTimer(xTimerForWakeUp);

	sample_count++;

	if (sample_count >= HEARTBEAT_TIME/sample_period) {

		need_push = 0x5a;
		tx_cause = TIMER_TX;

		sample_count = 0;

		return;
	}

#ifdef ENABLE_SHT2X
	sht2x_init(SCL_PIN, SDA_PIN);		// initialization of the sensor
	cur_temp = sht2x_get_temp();
	cur_humi = sht2x_get_humi();
#endif
#ifdef ENABLE_SHT3X
	sht3x_init(SCL_PIN, SDA_PIN);		// initialization of the sensor
	//sht3x_read_sensor(&cur_temp, &cur_humi);
	cur_temp = sht3x_get_temp();
	cur_humi = sht3x_get_humi();
#endif

#ifdef TX_TESTING
	need_push = 0x5a;
	tx_cause = TIMER_TX;
#else
	//if (fabsf(cur_temp - old_temp) > 0.5 || fabsf(cur_humi - old_humi) > 3) {
	if (fabsf(cur_humi - old_humi) > DELTA_HUMI) {

		need_push = 0x5a;

		tx_cause = DELTA_TX;

	}
#endif
}

void trig_check_sensor()
{
	need_push = 0x5a;

	tx_cause = KEY_TX;
}

void setup()
{
	Ecode_t e;

	WDOG_Init_TypeDef wInit = WDOG_INIT_DEFAULT;

	/* Watchdog setup - Use defaults, excepts for these : */
	wInit.em2Run = true;
	wInit.em3Run = true;
	wInit.perSel = wdogPeriod_32k;	/* 32k 1kHz periods should give 32 seconds */

	// init dev power ctrl pin
	pinMode(PWR_CTRL_PIN, OUTPUT);
	power_off_dev();

	pinMode(KEY_PIN, INPUT);
	attachInterrupt(KEY_PIN, trig_check_sensor, FALLING);

	/* Initialize RTC timer. */
	RTCDRV_Init();
	RTCDRV_AllocateTimer(&xTimerForWakeUp);

#ifdef DEBUG
	Serial.setRouteLoc(1);
	Serial.begin(115200);
#endif

	/* Start watchdog */
	WDOG_Init(&wInit);

	/* bootup tx */
	tx_cause = RESET_TX;
	need_push = 0x5a;
}

void qsetup()
{
#ifdef CONFIG_V0
	sx1272.setup_v0(TXRX_CH, MAX_DBM);
#else
	sx1272.sx1278_qsetup(TXRX_CH, MAX_DBM);
	sx1272._nodeAddress = node_addr;
#endif

#ifdef ENABLE_CAD
	sx1272._enableCarrierSense = true;
#endif

}

#ifdef CONFIG_V0
uint64_t get_devid()
{
	uint64_t *p;

	p = (uint64_t *)0x0FE00008;

	return *p;
}

uint16_t get_crc(uint8_t *pp, int len)
{
	int i;
	uint16_t hh = 0;

	for (i = 0; i < len; i++) {
		hh += pp[i];
	}
	return hh;
}
#endif

void push_data()
{
	long startSend;
	long endSend;

	float vbat = 0.0;

	int e;

#ifdef CONFIG_V0
	uint8_t *pkt = message;
#endif

	////////////////////////////////
	cur_curr = fetch_current();

	noInterrupts();

	if (cur_curr > 1.9)
		pkt[15] = EL_TX;
	else
		pkt[15] = tx_cause;

	interrupts();
	////////////////////////////////

	vbat = adc.readVbat();

	if (KEY_TX == tx_cause || RESET_TX == tx_cause) {
	#ifdef ENABLE_SHT2X
		sht2x_init(SCL_PIN, SDA_PIN);		// initialization of the sensor
		cur_temp = sht2x_get_temp();
		cur_humi = sht2x_get_humi();
	#endif
	#ifdef ENABLE_SHT3X
		sht3x_init(SCL_PIN, SDA_PIN);		// initialization of the sensor
		//sht3x_read_sensor(&cur_temp, &cur_humi);
		cur_temp = sht3x_get_temp();
		cur_humi = sht3x_get_humi();
	#endif
	}

	power_on_dev();

	INFO("Temp: ");
	INFOLN(cur_temp);

	INFO("Humi: ");
	INFOLN(cur_humi);

#ifdef CONFIG_V0
	uint64_t devid = get_devid();

	uint8_t *p = (uint8_t *) &devid;

	// set devid
	int i = 0;
	for(i = 0; i < 8; i++) {
		pkt[3+i] = p[7-i];
	}

	int16_t ui16 = (int16_t)(cur_humi * 10);
	p = (uint8_t *) &ui16;

	pkt[11] = p[1]; pkt[12] = p[0];

	ui16 = vbat * 1000;
	pkt[13] = p[1]; pkt[14] = p[0];

	// pkt[15] <--- tx_cause

	#ifdef CONFIG_PROTO_V33
	pkt[2] = 0x33;

	p = (uint8_t *) &tx_count;
	pkt[16] = p[1]; pkt[17] = p[0];

	tx_count++;

	ui16 = get_crc(pkt, 18);
	p = (uint8_t *) &ui16;
	pkt[18] = p[1]; pkt[19] = p[0];

	float chip_temp = fetch_mcu_temp();

	// Humidity Sensor data	or Water Leak Sensor data
	pkt[20] = (int8_t)roundf(cur_temp);

	// Internal Temperature of the chip
	pkt[21] = (int8_t)roundf(chip_temp);

	// Internal humidity to detect water leak of the shell
	pkt[22] = 255;

	// Internal current consumption
	pkt[23] = (int8_t)roundf(cur_curr);

	#elif CONFIG_PROTO_V34

	pkt[2] = 0x34;

	pkt[16] = 8;			// dev_type

	pkt[17] = 0;			// dev_type

	/* DTF: 0 00 101 10, realtime data, float, 4bytes */
	pkt[18] = 0x16;

	/* DUF: % */
	pkt[19] = 0x15;

	p = (uint8_t *) &cur_humi;
	pkt[20] = p[1];
	pkt[21] = p[0];
	pkt[22] = p[3];
	pkt[23] = p[2];


	/* DTF: 0 00 101 10, realtime data, float, 4bytes */
	pkt[24] = 0x16;

	/* DUF: 'C */
	pkt[25] = 0xB;

	p = (uint8_t *) &cur_temp;
	pkt[26] = p[1];
	pkt[27] = p[0];
	pkt[28] = p[3];
	pkt[29] = p[2];

	/* frame number */
	p = (uint8_t *) &tx_count;
	pkt[30] = p[1]; pkt[31] = p[0];
	tx_count++;

	ui16 = get_crc(pkt, 32);
	p = (uint8_t *) &ui16;
	pkt[32] = p[1]; pkt[33] = p[0];

	pkt[34] = 0; pkt[35] = 0; pkt[36] = 0; pkt[37] = 0;
	#endif
#else
	uint8_t r_size;

	char vbat_s[10], temp_s[10], humi_s[10];

	ftoa(vbat_s, vbat, 2);
	ftoa(temp_s, cur_temp, 2);
	ftoa(humi_s, cur_humi, 0);

	r_size = sprintf((char *)message, "\\!U/%s/T/%s/H/%s",
				vbat_s, temp_s, humi_s);

	INFO("Sending ");
	INFOLN((char *)(message));

	INFO("Real payload size is ");
	INFOLN(r_size);
#endif

	qsetup();

#ifdef ENABLE_CAD
	sx1272.CarrierSense();
#endif

#ifdef DEBUG
	startSend = millis();
#endif

#ifdef CONFIG_V0
	e = sx1272.sendPacketTimeout(DEST_ADDR, message, TX_LEN, TX_TIME);
#else
	// just a simple data packet
	sx1272.setPacketType(PKT_TYPE_DATA);

	// Send message to the gateway and print the result
	// with the app key if this feature is enabled
#ifdef WITH_ACK
	int n_retry = NB_RETRIES;

	do {
		e = sx1272.sendPacketTimeoutACK(DEST_ADDR,
						message, r_size);

		if (e == 3)
			INFO("No ACK");

		n_retry--;

		if (n_retry)
			INFO("Retry");
		else
			INFO("Abort");

	} while (e && n_retry);
#else
	// 10ms max tx time
	e = sx1272.sendPacketTimeout(DEST_ADDR, message, r_size, TX_TIME);
#endif

	INFO("LoRa pkt size ");
	INFOLN(r_size);
#endif

	if (!e) {
		// send message succesful, update the old data
		old_temp = cur_temp;
		old_humi = cur_humi;
	}

#ifdef DEBUG
	endSend = millis();

	INFO("LoRa Sent in ");
	INFOLN(endSend - startSend);

	INFO("LoRa Sent w/CAD in ");
	INFOLN(endSend - sx1272._startDoCad);

	INFO("Packet sent, state ");
	INFOLN(e);
#endif

	e = sx1272.setSleepMode();
	if (!e)
		INFO("Successfully switch into sleep mode");
	else
		INFO("Could not switch into sleep mode");

	digitalWrite(SX1272_RST, LOW);

	spi_end();

	power_off_dev();
}

void loop()
{
	//INFO("Clock Freq = ");
	//INFOLN(CMU_ClockFreqGet(cmuClock_CORE));

	//INFO("need_push = ");
	//INFOHEX(need_push);

	if (0x5a == need_push) {
		push_data();

		need_push = 0;
	}

	power_off_dev();
	digitalWrite(SX1272_RST, LOW);

	spi_end();

	/*
	 * Enable rtc timer before enter deep sleep
	 * Stop rtc timer after enter check_sensor_data()
	 */
	RTCDRV_StartTimer(xTimerForWakeUp, rtcdrvTimerTypeOneshot, sample_period * 1000, check_sensor, NULL);

	wire_end();

	EMU_EnterEM2(true);
}