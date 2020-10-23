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
#include "pc10.h"

#include "rtcdriver.h"
#include "math.h"
#include "em_wdog.h"

//#define	DEBUG					1

#define FW_VER						"Ver 1.1"

//#define CONFIG_2MIN					1

#define ENABLE_OLED					1
#define ENABLE_CAD					1

#define ENABLE_P_TEST			1
//#define ENABLE_RTP_TEST			1

#ifdef ENABLE_P_TEST
#define DELTA_P					0.1
#else
#define DELTA_P					0.05
#endif

#ifdef ENABLE_RTP_TEST
#define DELTA_RT_P				0.08
#endif

#define ENABLE_CRYPTO				1

#define	PAYLOAD_LEN					30		/* 26+2+4 = 32B */

#ifdef ENABLE_CRYPTO
#include "crypto.h"
#endif

#ifdef ENABLE_P_TEST
static uint32_t cnt_01 = 0;
#endif

#ifdef ENABLE_RTP_TEST
static uint32_t cnt_rt_01 = 0;
#endif

/* Timer used for bringing the system back to EM0. */
RTCDRV_TimerID_t xTimerForWakeUp;

#ifndef CONFIG_2MIN
static uint32_t sample_period = 2;			/* 20s */
static uint32_t cnt_20s = 0;
#define		HEARTBEAT_TIME			6600	/* 120min */
static float old_pres = 0.0;
#else
//static uint32_t sample_period = 110;		/* 120s */
static uint32_t sample_period = 18;			/* 20s */
#endif

static float cur_pres = 0.0;

static float max_pres = 0.0;
static float min_pres = 0.0;

static float cur_vbat = 0.0;

static uint32_t cnt_vbat_3v3 = 0;
static uint32_t cnt_vbat_low = 0;
static bool vbat_low = false;

static uint32_t oled_on_time = 30;			/* s */
static uint32_t oled_refresh_time = 500;	/* ms */
static int oled_i;

#ifdef MONITOR_CURRENT
static float cur_curr = 0.0;
#endif

static uint32_t need_push = 0;

#define	PWR_CTRL_PIN			8		/* PIN17_PC14_D8 */
#define	KEY_PIN					0		/* PIN01_PA00_D0 */

#define SCL_PIN					11		/* PIN14_PD7 */
#define SDA_PIN					10		/* PIN13_PD6 */

#define	TX_TIME					1800		// 1800ms
#define DEST_ADDR				1

#ifdef CONFIG_V0
#define TXRX_CH				CH_01_472
#define LORA_MODE			12

#define	RESET_TX			0
#define	DELTA_TX			1
#define	TIMER_TX			2
#define	KEY_TX				3
#define	EL_TX				4
#define	WL_TX				5

#else
#define node_addr				110

#define TXRX_CH				CH_00_470
#define LORA_MODE			11
#endif

#define MAX_DBM					20

uint8_t message[PAYLOAD_LEN+6] __attribute__((aligned(4)));

#ifdef CONFIG_V0
uint32_t tx_cause = RESET_TX;
uint16_t tx_count __attribute__((aligned(4))) = 0;
uint32_t tx_ok_cnt = 0;
#endif

/*
 * Show Mode:
 *
 *   0x0: show pressure
 *   0x1: show max and min
 *   0x2: show send and version
*/
#define	MODE_P			0
#define	MODE_MAX		1
#define	MODE_VER		2

int mode = MODE_P;
int old_mode = MODE_P;
uint32_t key_count = 0;

#ifdef DEBUG

#define INFO_S(param)			Serial.print(F(param))
#define INFO_HEX(param)			Serial.print(param,HEX)
#define INFO(param)				Serial.print(param)
#define INFOLN(param)			Serial.println(param)
#define FLUSHOUTPUT					Serial.flush();

#else

#define INFO_S(param)
#define INFO(param)
#define INFOLN(param)
#define FLUSHOUTPUT

#endif

#ifdef WITH_ACK
#define	NB_RETRIES			2
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

#ifdef ENABLE_OLED
#include "U8g2lib.h"

/*
 * PIN24_PE13_D13 - SH1107_SCL_PIN10
 * PIN23_PE12_D12 - SH1107_SDA_PIN9
 * PIN21_PF02_D16 - SH1107_RST_PIN14
 *
 * GND - SH1107_A0_PIN13 (I2C_ADDR = 0x78)
 */
#define SH1107_SDA					12
#define SH1107_SCL					13
#define SH1107_RESET				16

U8G2_SH1107_SEEED_128X128_1_HW_I2C u8g2(U8G2_R0, SH1107_RESET);

#ifdef EFM32HG

#include "logo.h"

void show_logo()
{
	u8g2.setPowerSave(0);

	u8g2.firstPage();

	do {
		u8g2.drawXBM(12, 34, logo_width, logo_height, logo_xbm);
		//u8g2.drawXBM(0, 0, ptest_width, ptest_height, ptest);
	} while (u8g2.nextPage());
}

void show_low_bat()
{
	u8g2.setPowerSave(0);

	u8g2.firstPage();

	do {
		u8g2.setFont(u8g2_font_freedoomr10_mu);	// choose a suitable font
		u8g2.setCursor(16, 64);
		u8g2.print(" LOW BATTERY ");
	} while (u8g2.nextPage());
}

char dev_id[24];

uint64_t get_devid();

char *uint64_to_str(uint64_t n)
{
	char *dest = dev_id;

	dest += 20;
	*dest-- = 0;
	while (n) {
		*dest-- = (n % 10) + '0';
		n /= 10;
	}

	strcpy(dev_id, dest+1);

	return dest + 1;
}

#endif

void show_press(float p, float vb, bool show_bat)
{
	char dev_vbat[6] = "00000";
	char press[6];

	ftoa(press, p, 2);

	int len = strlen(press);		/* 5 or 4 */

	ftoa(dev_vbat, vb, 2);
	dev_vbat[4] = 'V';

	u8g2.setPowerSave(0);

	u8g2.firstPage();

	uint64_to_str(get_devid());

	do {
		if (vbat_low == true) {

			if (show_bat)
				u8g2.drawXBM(59, 8, low_battery_width, low_battery_height, low_battery_icon);

		} else {
			u8g2.drawXBM(59, 8, battery_width, battery_height, battery_icon);
		}

		u8g2.setFont(Futura_Medium_16px);
		u8g2.setCursor(8, 18);
		u8g2.print(dev_vbat);

		u8g2.setFont(Futura_Heavy_20px);
		u8g2.setCursor(98, 20);
		u8g2.print("  M");

		u8g2.setFont(Futura_Medium_19px);
		u8g2.setCursor(16, 120);
		u8g2.print(dev_id);

		if (p == -1.0) {
			// no sensor connected
			u8g2.drawXBM(27, 36, no_sensor_width, no_sensor_height, no_sensor_icon);

		} else if (p == -2.0) {
			// out of low range

			u8g2.drawXBM(30, 34, x_low_width, x_low_height, x_low_icon);

		} else if (p == -3.0) {
			// out of high range
			u8g2.drawXBM(30, 34, x_high_width, x_high_height, x_high_icon);
		} else {
			u8g2.setFont(Futura_Medium_55px);
			if (len <= 4) {

				u8g2.setCursor(24, 80);

			} else if (len == 5) {

				u8g2.setCursor(8, 80);
			}

			u8g2.print(press);
		}

	} while (u8g2.nextPage());
}

void show_maxmin(float max, float min)
{
	char pres_s[6];

	u8g2.setPowerSave(0);

	u8g2.firstPage();

	do {
		u8g2.setFont(Futura_Heavy_20px);
		u8g2.setCursor(98, 20);
		u8g2.print("  M");

		u8g2.setCursor(10, 55);
		u8g2.print("Max ");

		u8g2.setCursor(10, 92);
		u8g2.print("Min ");

		u8g2.setFont(Futura_Medium_32px);
		u8g2.setCursor(50, 58);
		ftoa(pres_s, max, 2);
		u8g2.print(pres_s);


		u8g2.setCursor(48, 95);
		ftoa(pres_s, min, 2);
		u8g2.print(pres_s);

	} while (u8g2.nextPage());
}

void show_ver(int txc)
{
	char txc_s[6];

	u8g2.setPowerSave(0);

	u8g2.firstPage();

	do {
		u8g2.setFont(Futura_Heavy_20px);
		u8g2.setCursor(50, 23);
		u8g2.print(FW_VER);
		u8g2.print(".");
		u8g2.print(sample_period);

		u8g2.setCursor(58, 116);
		u8g2.print("3.0 M");

		if(txc < 9999) {
			u8g2.setCursor(10, 78);
		} else {
			u8g2.setCursor(6, 78);
		}
		u8g2.print("Send");

		//ftoa(txc_s, txc, 0);
		u8g2.setFont(Futura_Medium_32px);

		if(txc < 9999) {
			u8g2.setCursor(60, 78);
		} else {
			u8g2.setCursor(45, 78);
		}

		u8g2.print(txc);

	#if 0
		u8g2.setFont(Futura_Heavy_20px);
		u8g2.setCursor(10, 116);
		u8g2.print(sample_period);
		u8g2.print(" S");
	#endif

	} while (u8g2.nextPage());
}
#endif

void push_data();

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

#ifdef MONITOR_CURRENT
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
#endif

float fetch_vbat()
{
	float vbat = 0;

	for (int i = 0; i < 5; i++) {
		vbat += adc.readVbat();
	}

	return vbat/5.0;
}

void check_sensor(RTCDRV_TimerID_t id, void *user)
{
	(void)id;
	(void)user;

	WDOG_Feed();

	RTCDRV_StopTimer(xTimerForWakeUp);

	cur_vbat = fetch_vbat();

	if (cur_vbat >= 3.27 && cur_vbat < 3.34) {

		cnt_vbat_3v3++;

		if (cnt_vbat_3v3 >= 3) {
			// 3v3 power

			sample_period = 2;

			cnt_vbat_3v3 = 0;
		}

		cnt_vbat_low = 0;
		vbat_low = false;

	} else if (cur_vbat < 3.0) {

		cnt_vbat_low++;

		if (cnt_vbat_low >= 3) {

			sample_period = 28;

			vbat_low = true;
			cnt_vbat_low = 0;
		}

		cnt_vbat_3v3 = 0;

	} else if (cur_vbat >= 3.34) {

		// battery supply
		cnt_vbat_3v3 = 0;
		sample_period = 18;

		cnt_vbat_low = 0;
		vbat_low = false;
	}

#ifndef CONFIG_2MIN
	cnt_20s++;

	if (cnt_20s >= HEARTBEAT_TIME/20) {
		need_push = 0x5a;
		tx_cause = TIMER_TX;
		cnt_20s = 0;
	}
#endif

	pressure_init(SCL_PIN, SDA_PIN);	// initialization of the sensor

	/*
	 * Return value:
	 *
	 *   unit is 'm'
	 *
	 *   183.673469 m: Max valid value (16383)
	 *  -1.53061224 m: Min valid value (1501)
	 *  -1 m: Bus error, no sensor connected, not wakeup
	 *  -2 m: Out of low range
	 *  -3 m: Out of high range
	*/
	cur_pres = get_water_h();

#ifndef CONFIG_2MIN
	float dp = fabsf(cur_pres - old_pres);

	if (dp >= DELTA_P) {

		need_push = 0x5a;
		tx_cause = DELTA_TX;

	#ifdef ENABLE_P_TEST
		cnt_01 = 0;
	#endif

		return;
	}

	#ifdef ENABLE_P_TEST
	if (dp >= DELTA_P/2 && dp < DELTA_P) {

		cnt_01++;

		if (cnt_01 >= 3) {
			need_push = 0x5a;
			tx_cause = DELTA_TX;

			cnt_01 = 0;
		}

	} else if (dp < DELTA_P/2) {

		cnt_01 = 0;
	}
	#endif
#else
	// 2min fixed interval
	need_push = 0x5a;
	tx_cause = TIMER_TX;
#endif
}

void trig_check_sensor()
{
	need_push = 0x5a;
#ifdef CONFIG_V0
	tx_cause = KEY_TX;
#endif

	key_count++;

	mode++;
	mode %= 3;

	if (2 == sample_period || 18 == sample_period) {
		// usb power, reset the oled_i
		oled_i = 0;
	}
}

void setup()
{
	Ecode_t e;

	WDOG_Init_TypeDef wInit = WDOG_INIT_DEFAULT;

	/* Watchdog setup - Use defaults, excepts for these : */
	wInit.em2Run = true;
	wInit.em3Run = true;

#ifdef ENABLE_CRYPTO
	crypto_init();
#endif

#ifdef CONFIG_2MIN
	//wInit.perSel = wdogPeriod_128k;	/* 128k 1kHz periods should give 128 seconds */
	wInit.perSel = wdogPeriod_32k;	/* 32k 1kHz periods should give 32 seconds */
#else
	wInit.perSel = wdogPeriod_32k;	/* 32k 1kHz periods should give 32 seconds */
#endif

	cur_vbat = fetch_vbat();

	// init dev power ctrl pin
	pinMode(PWR_CTRL_PIN, OUTPUT);

	power_on_dev();

	pinMode(KEY_PIN, INPUT);
	attachInterrupt(KEY_PIN, trig_check_sensor, FALLING);

	/* Initialize RTC timer. */
	RTCDRV_Init();
	RTCDRV_AllocateTimer(&xTimerForWakeUp);

#ifdef DEBUG
	Serial.setRouteLoc(1);
	Serial.begin(9600);
#endif

	/* Start watchdog */
	WDOG_Init(&wInit);

#ifdef ENABLE_OLED
	u8g2.begin();

	#ifdef EFM32HG
	pressure_init(SCL_PIN, SDA_PIN);	// initialization of the sensor
	cur_pres = get_water_h();

	min_pres = cur_pres;

	delay(2);
	show_logo();
	delay(800);

	if (vbat_low) {
		show_low_bat();
		delay(2700);
	}

	show_press(cur_pres, cur_vbat, true);
	delay(1800);

#if 0
	show_press(-2.0, cur_vbat, true);
	delay(1800);

	show_press(-3.0, cur_vbat, true);
	delay(1800);
#endif

	#endif

	u8g2.setPowerSave(1);
#endif

	/* bootup tx */
	tx_cause = RESET_TX;
	need_push = 0x5a;

	mode = MODE_VER;
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
	long start;
	long end;
	int e;

	WDOG_Feed();

#ifdef CONFIG_V0
	uint8_t *pkt = message;

	memset(pkt, 0, PAYLOAD_LEN+6);

	pkt[0] = 0x47; pkt[1] = 0x4F; pkt[2] = 0x33;
#endif

	////////////////////////////////
#ifdef MONITOR_CURRENT
	cur_curr = fetch_current();

	noInterrupts();

	if (cur_curr > 1.9)
		pkt[15] = EL_TX;
	else
		pkt[15] = tx_cause;

	interrupts();
#else
	pkt[15] = tx_cause;
#endif
	////////////////////////////////

	power_on_dev();		// turn on device power

	if (KEY_TX == tx_cause || RESET_TX == tx_cause) {
		pressure_init(SCL_PIN, SDA_PIN);
		cur_pres = get_water_h();		// hPa (mbar)
	}


#ifdef CONFIG_V0
	// set devid
	uint64_t devid = get_devid();
	uint8_t *p = (uint8_t *) &devid;
	for(int i = 0; i < 8; i++) {
		pkt[3+i] = p[7-i];
	}

	/* water_h is m, x100 is cm */
	int16_t ui16 = (int16_t)(cur_pres * 100);
	p = (uint8_t *) &ui16;

	pkt[11] = p[1]; pkt[12] = p[0];

	ui16 = cur_vbat * 1000;
	pkt[13] = p[1]; pkt[14] = p[0];

	p = (uint8_t *) &tx_count;
	//pkt[16] = p[1]; pkt[17] = p[0];
	pkt[PAYLOAD_LEN-2] = p[1]; pkt[PAYLOAD_LEN-1] = p[0];
	tx_count++;

	float chip_temp = fetch_mcu_temp();

	// Humidity Sensor data	or Water Leak Sensor data
	pkt[20] = 0;

	// Internal Temperature of the chip
	pkt[21] = (int8_t)roundf(chip_temp);

	// Internal humidity to detect water leak of the shell
	pkt[22] = 255;

	// Internal current consumption
#ifdef MONITOR_CURRENT
	pkt[23] = (int8_t)roundf(cur_curr);
#else
	pkt[23] = 0;
#endif

	/////////////////////////////////////////////////////////
	/*
	 * 2. crc
	 * 3. set mic
	*/
	ui16 = get_crc(pkt, PAYLOAD_LEN);
	p = (uint8_t *) &ui16;

	pkt[PAYLOAD_LEN] = p[1]; pkt[PAYLOAD_LEN+1] = p[0];

#ifdef ENABLE_CRYPTO
	set_pkt_mic(pkt, PAYLOAD_LEN+6);
#endif
	/////////////////////////////////////////////////////////
#else
	uint8_t r_size;

	char vbat_s[10], pres_s[10];
	ftoa(vbat_s, vbat, 2);
	ftoa(pres_s, cur_pres, 2);

	r_size = sprintf((char *)message, "\\!U/%s/P/%s", vbat_s, pres_s);

	INFO("Sending ");
	INFOLN((char *)message);

	INFO("Real payload size is ");
	INFOLN(r_size);
#endif

	qsetup();

#ifdef ENABLE_CAD
	sx1272.CarrierSense();
#endif

#ifdef DEBUG
	start = millis();
#endif

#ifdef CONFIG_V0
	e = sx1272.sendPacketTimeout(DEST_ADDR, message, PAYLOAD_LEN+6, TX_TIME);
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
		// send message succesful, update the old_pres
	#ifndef CONFIG_2MIN
		old_pres = cur_pres;
	#endif
		tx_ok_cnt++;
	}

#ifdef DEBUG
	end = millis();

	INFO("LoRa Sent in ");
	INFOLN(end - start);

	INFO("LoRa Sent w/CAD in ");
	INFOLN(end - sx1272._startDoCad);

	INFO("Packet sent, state ");
	INFOLN(e);
#endif

	sx1272.setSleepMode();
	digitalWrite(SX1272_RST, LOW);

	spi_end();

	// dev power off
	power_off_dev();
}

void task_oled()
{
#ifdef ENABLE_OLED
	char pres_s[6];

	// reset the key count
	key_count = 0;

	float vbat = cur_vbat;

	float cur_p = 0.0;
#ifdef ENABLE_RTP_TEST
	float old_p = 0.0;
#endif

	if (2 == sample_period) {
		// usb power
		oled_on_time = 60;
		oled_refresh_time = 200;

	} else if (28 == sample_period) {

		// low battery
		oled_on_time = 15;
		oled_refresh_time = 1500;

	} else if (18 == sample_period) {

		// battery supply
		oled_on_time = 30;
		oled_refresh_time = 500;
	}

	pressure_init(SCL_PIN, SDA_PIN);

	for (oled_i=0; oled_i<oled_on_time*1000/oled_refresh_time; oled_i++) {

		WDOG_Feed();

		cur_p = get_water_h();

		#ifdef ENABLE_RTP_TEST
		float dp = fabsf(cur_p - old_p);

		if (dp >= DELTA_RT_P) {

			//need_show = 0x5a;
			old_p = cur_p;

			cnt_rt_01 = 0;

		} else if (dp >= DELTA_RT_P/2 && dp < DELTA_RT_P) {

			cnt_rt_01++;

			if (cnt_rt_01 >= 3) {

				//need_show = 0x5a;
				old_p = cur_p;

				cnt_rt_01 = 0;

			} else {

				cur_p = old_p;
			}

		} else if (dp < DELTA_RT_P/2) {

			cnt_rt_01 = 0;

			cur_p = old_p;
		}
		#endif

		if (key_count == 2) {

			// reset the min & max
			min_pres = cur_p;
			max_pres = cur_p;
			key_count = 0;

			mode = MODE_MAX;

		} else if (key_count >= 3) {

			key_count = 0;
			u8g2.setPowerSave(1);

			// reset the mode
			mode = MODE_VER;

			return ;

		} else {
			key_count = 0;
		}

		if (cur_p > max_pres) {
			max_pres = cur_p;
		}

		if (cur_p < min_pres) {
			min_pres = cur_p;
		}

		if (vbat > 3.0) {
			vbat_low = false;
		}

		switch(mode) {
			case MODE_P:
				show_press(cur_p, vbat, oled_i%2);
				break;

			case MODE_MAX:
				show_maxmin(max_pres, min_pres);
				//show_maxmin(16.88, -2.0);
				break;

			case MODE_VER:
				show_ver(tx_ok_cnt);
				break;
		}

		delay(oled_refresh_time);
	}

	u8g2.setPowerSave(1);
#endif

	// reset the mode
	mode = MODE_VER;
}

void loop()
{
	if (key_count >= 1) {

		power_on_dev();
		u8g2.begin();
		task_oled();

	}

	if (0x5a == need_push) {
		push_data();

		need_push = 0;
	}

#ifdef ENABLE_OLED
	u8g2.setPowerSave(1);
#endif

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