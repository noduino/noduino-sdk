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

#include "softi2c.h"
#include "pcf8563.h"

#include "pt1000.h"

#include "rtcdriver.h"
#include "math.h"
#include "em_wdog.h"

//#define	DEBUG					1

#ifdef CONFIG_V0
#include "softspi.h"
#include "sx1272.h"
#elif USE_SX126X
#include "sx126x.h"
#define SX126X_RST				9
SX126x sx126x(2,					// Pin: SPI CS,PIN06-PB08-D2
				SX126X_RST,			// Pin: RESET, PIN18-PC15-D9
				5,					// PIN: Busy,  PIN11-PB14-D5, The RX pin
				3					// Pin: DIO1,  PIN08-PB11-D3
);
#endif

#define ENABLE_CRYPTO				1
#define ENABLE_CAD					1

#ifdef ENABLE_CRYPTO
#include "crypto.h"
#endif

#define FW_VER					"Ver 1.2"

static float cur_curr = 0.0;

#define LEVEL_UNKNOWN				-2
#define LEVEL_LOW					-1
#define LEVEL_MEDIAN				0
#define LEVEL_HIGH					1

static float cur_temp = 0.0;
static int8_t cur_water = LEVEL_UNKNOWN;

#define	T_FIX					0

static uint32_t need_push = 0;

#define	PWR_CTRL_PIN			8		/* PIN17_PC14_D8 */
#define	KEY_PIN					0		/* PIN01_PA00_D0 */

#define	RTC_INT_PIN				16		/* PIN21_PF02_D16 */

#define SDA_PIN					12		/* PIN23_PE12 */
#define SCL_PIN					13		/* PIN24_PE13 */

#define	RESET_TX			0
#define	DELTA_TX			1
#define	TIMER_TX			2
#define	KEY_TX				3
#define	EL_TX				4
#define	WL_TX				5

uint8_t tx_cause = RESET_TX;


#ifdef CONFIG_V0
#define TXRX_CH					CH_01_472
#define MAX_DBM					20
#define	TX_TIME					1800
#elif USE_SX126X
#define TXRX_CH					472500000
#define MAX_DBM					14
#endif

#define	PAYLOAD_LEN					30		/* 30+2+4 = 36B */
uint8_t message[PAYLOAD_LEN+6] __attribute__((aligned(4)));

uint16_t tx_count = 0;

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


#define RTC_PERIOD					30
#define TX_PERIOD					120
uint32_t cnt_rtc_min __attribute__((aligned(4))) = 0;
//uint32_t rtc_period __attribute__((aligned(4))) = RTC_PERIOD;
//uint32_t tx_period __attribute__((aligned(4))) = 600;

#define LOW_BAT_THRESHOLD			3.4
static float cur_vbat = 0.0;
bool vbat_low __attribute__((aligned(4))) = false;
int cnt_vbat_low __attribute__((aligned(4))) = 0;
int cnt_vbat_ok __attribute__((aligned(4))) = 0;


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

#ifdef USE_PT1K_X1
int get_water()
{
	int ret = 0;
	uint32_t rt = 0;

	power_on_dev();
	pt1000_init();
	rt = pt1000_get_rt();
	power_off_dev();

	/* 1 - LOW, 2 - Median, 0 - High */
	ret = rt / 10000;

	switch (ret) {
		case 1:
			ret = LEVEL_LOW;
			break;
		case 2:
			ret = LEVEL_MEDIAN;
			break;
		case 0:
			ret = LEVEL_HIGH;
			break;
		default:
			ret = LEVEL_UNKNOWN;
	}

	/* (pt1K + 1.1K) para. 1.1K, [7223.35, 7624.92] */
	/* (pt1k + 0.5K) para. 0.5K, [3752.43, 3951.83] */
	/* 3pt1k, [6692.67, 9234] */
	if (ret == LEVEL_HIGH && (rt > 7625 || rt < 7223)) {

		ret = LEVEL_UNKNOWN;
	}

	/* 1 x pt1K+ 1.1K, (21039, 24851] */
	/* 1 x pt1k+ 0.5K, (15039, 18851] */
	/* 3pt1k, [20078, 27702 */
	if (ret == LEVEL_MEDIAN && (rt > 24851 || rt < 21039)) {

		ret = LEVEL_UNKNOWN;
	}

	/* 1 x pt1K, (10039, 13851] */
	if (ret == LEVEL_LOW && (rt > 13851 || rt < 10039)) {

		ret = LEVEL_UNKNOWN;
	}

	return ret;
}

float get_temp()
{
	int n = 0;
	uint32_t rt = 0;

	pt1000_init();
	rt = pt1000_get_rt();

	n = rt / 10000;

	if (2 == n) {
		/* median */

		rt -= 11000;

	} else if (0 == n) {
		/* high */

		//rt = 1.0 / (1.0/(double)rt - 1.0/11000.0) - 11000;
		rt = 11000.0*(double)rt / (double)(11000.0 - rt) - 11000;

	} // n >= 3, rt = rt

	// n == 1, rt = rt

	if (n != 3 && (rt > 13851 || rt <= 10039)) {

		// n = 0, 1, 2, 4...

		return -2.0;
	}

	// n = 3 is the 300'C, no sensor connected

	return cal_temp(rt);
}
#else
int get_water()
{
	int ret = 0;
	uint32_t rt = 0;

	power_on_dev();
	pt1000_init();
	rt = pt1000_get_rt();
	power_off_dev();

	/* 1 - LOW, 2 - Median, 0 - High */
	ret = rt / 10000;

	switch (ret) {
		case 1:
			ret = LEVEL_LOW;
			break;
		case 2:
			ret = LEVEL_MEDIAN;
			break;
		case 0:
			ret = LEVEL_HIGH;
			break;
		default:
			ret = LEVEL_UNKNOWN;
	}

	/* 2/3 x pt1000, (6693, 9234], (10039.5, 13851] */
	if (ret == LEVEL_HIGH && (rt > 9234 || rt <= 6693)) {

		ret = LEVEL_UNKNOWN;
	}

	/* 2 x pt1000, (20078, 27702], (10039, 13851] */
	if (ret == LEVEL_MEDIAN && (rt > 27702 || rt <= 20078)) {

		ret = LEVEL_UNKNOWN;
	}

	/* 1 x pt1000, (10039, 13851] */
	if (ret == LEVEL_LOW && (rt > 13851 || rt <= 10039)) {

		ret = LEVEL_UNKNOWN;
	}

	return ret;
}

float get_temp()
{
	int n = 0;
	uint32_t rt = 0;

	pt1000_init();
	rt = pt1000_get_rt();

	n = rt / 10000;

	if (2 == n) {

		rt /= 2.0;	

	} else if (0 == n) {

		rt *= 1.5;

	} // n >= 3, rt = rt

	// n == 1, rt = rt

	if (n != 3 && (rt > 13851 || rt <= 10039)) {

		// n = 0, 1, 2, 4...

		return -2.0;
	}

	// n = 3 is the 300'C, no sensor connected

	return cal_temp(rt);
}
#endif

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

void trig_check_sensor()
{
	need_push = 0x5a;
	tx_cause = KEY_TX;
}

void rtc_irq_handler()
{
	NVIC_DisableIRQ(GPIO_ODD_IRQn);
	NVIC_DisableIRQ(GPIO_EVEN_IRQn);

	WDOG_Feed();

	cnt_rtc_min++;

	pcf8563_init(SCL_PIN, SDA_PIN);
	//i2c_delay(I2C_1MS);
	pcf8563_reset_timer();

	INFO("ctrl2: ");
	INFOHEX(pcf8563_get_ctrl2());
	INFOLN("");

	if (cnt_rtc_min % (TX_PERIOD/RTC_PERIOD) == 0) {
		/* 10 min */
		need_push = 0x5a;
		tx_cause = TIMER_TX;
	}

	NVIC_ClearPendingIRQ(GPIO_ODD_IRQn);
	NVIC_ClearPendingIRQ(GPIO_EVEN_IRQn);
	NVIC_EnableIRQ(GPIO_ODD_IRQn);
	NVIC_EnableIRQ(GPIO_EVEN_IRQn);
}

void setup()
{
	Ecode_t e;

	WDOG_Init_TypeDef wInit = WDOG_INIT_DEFAULT;

	/* Watchdog setup - Use defaults, excepts for these : */
	wInit.em2Run = true;
	wInit.em3Run = true;
	wInit.perSel = wdogPeriod_64k;	/* 32k 1kHz periods should give 4 seconds */

	/* Start watchdog */
	WDOG_Init(&wInit);

#ifdef ENABLE_CRYPTO
	crypto_init();
#endif

	// init dev power ctrl pin
	pinMode(PWR_CTRL_PIN, OUTPUT);
	power_off_dev();

	pinMode(KEY_PIN, INPUT);
	attachInterrupt(KEY_PIN, trig_check_sensor, FALLING);

#ifdef DEBUG
	Serial.setRouteLoc(1);
	Serial.begin(115200);
#endif

	power_on_dev();		/* To consume the current ? */
	pcf8563_init(SCL_PIN, SDA_PIN);

	int ctrl = pcf8563_get_ctrl2();
	INFO("RTC ctrl2: ");
	INFOHEX(ctrl);
	INFOLN("");

	if (ctrl == 0xFF) {
		while(true) {
		}
	}

	pcf8563_set_from_int(2021, 4, 7, 16, 36, 0);

	//pcf8563_set_timer(1);
	pcf8563_set_timer_s(30);

	INFO("RTC ctrl2: ");
	INFOHEX(pcf8563_get_ctrl2());
	INFOLN("");
	INFOLN(pcf8563_now());
	INFO("RTC timer: ");
	INFOHEX(pcf8563_get_timer());
	INFOLN("");
	power_off_dev();

	pinMode(RTC_INT_PIN, INPUT_PULLUP);
	attachInterrupt(RTC_INT_PIN, rtc_irq_handler, FALLING);

	/* bootup tx */
	tx_cause = RESET_TX;
	need_push = 0x5a;
}

void qsetup()
{
#ifdef CONFIG_V0
	sx1272.setup_v0(TXRX_CH, MAX_DBM);
	#ifdef ENABLE_CAD
	sx1272._enableCarrierSense = true;
	#endif

#elif USE_SX126X
	sx126x.wakeup();
	sx126x.init();
	sx126x.setup_v0(TXRX_CH, MAX_DBM);
#endif
}

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

void push_data()
{
	long startSend;
	long endSend;

	float vbat = 0.0;

	int e;

	uint8_t *pkt = message;

	memset(pkt, 0, PAYLOAD_LEN+6);
	pkt[0] = 0x47; pkt[1] = 0x4F;

	////////////////////////////////
#ifdef MONITOR_CURRENT
	cur_curr = fetch_current();

	noInterrupts();

	if (KEY_TX == tx_cause && cur_curr > 1.9)
		pkt[15] = EL_TX;
	else
		pkt[15] = tx_cause;

	interrupts();
#else
	pkt[15] = tx_cause;
#endif
	////////////////////////////////

	vbat = adc.readVbat();

	power_on_dev();
	cur_water = get_water();
	cur_temp = get_temp() + T_FIX;

	uint64_t devid = get_devid();

	uint8_t *p = (uint8_t *) &devid;

	// set devid
	int i = 0;
	for(i = 0; i < 8; i++) {
		pkt[3+i] = p[7-i];
	}

	int16_t i16 = (int16_t)(cur_temp * 10);

	p = (uint8_t *) &i16;
	pkt[11] = p[1]; pkt[12] = p[0];

	i16 = cur_vbat * 1000;
	pkt[13] = p[1]; pkt[14] = p[0];

	pkt[15] = tx_cause;

	pkt[2] = 0x33;

	float chip_temp = fetch_mcu_temp();

	// Humidity Sensor data	or Water Leak Sensor data
	pkt[20] = cur_water;

	// Internal Temperature of the chip
	pkt[21] = (int8_t)roundf(chip_temp);

	// Internal humidity to detect water leak of the shell
	pkt[22] = cur_water;

	// Internal current consumption
#ifdef MONITOR_CURRENT
	pkt[23] = (int8_t)roundf(cur_curr);
#else
	pkt[23] = cur_water;
#endif

	// pkt[27] = 0b100, set bit 2, HTimer rpt pkt
	pkt[PAYLOAD_LEN-3] = 0x4;

	uint32_t ep = pcf8563_now();

	if (ep < 1617783965) {
		ep = 0;
	}

	p = (uint8_t *) &ep;
	pkt[18] = p[3];
	pkt[19] = p[2];
	pkt[24] = p[1];
	pkt[25] = p[0];

	p = (uint8_t *) &tx_count;
	pkt[PAYLOAD_LEN-2] = p[1]; pkt[PAYLOAD_LEN-1] = p[0];
	tx_count++;

	/////////////////////////////////////////////////////////
	/*
	 * 2. crc
	 * 3. set mic
	*/
	#if 0
	uint16_t ui16 = get_crc(pkt, PAYLOAD_LEN);
	p = (uint8_t *) &ui16;
	pkt[PAYLOAD_LEN] = p[1]; pkt[PAYLOAD_LEN+1] = p[0];
	#else
	pkt[PAYLOAD_LEN] = 0; pkt[PAYLOAD_LEN+1] = 0;
	#endif

#ifdef ENABLE_CRYPTO
	set_pkt_mic(pkt, PAYLOAD_LEN+6);
#endif
	/////////////////////////////////////////////////////////

	qsetup();

#ifdef DEBUG
	startSend = millis();
#endif

#ifdef CONFIG_V0

	#ifdef ENABLE_CAD
	sx1272.CarrierSense();
	#endif

	e = sx1272.sendPacketTimeout(1, message, PAYLOAD_LEN+6, TX_TIME);

	sx1272.setSleepMode();
	digitalWrite(SX1272_RST, LOW);
	spi_end();
#elif USE_SX126X

	#ifdef ENABLE_CAD
	sx126x.enable_cad();
	#endif
	e = sx126x.send(message, PAYLOAD_LEN+6, SX126x_TXMODE_SYNC);
	sx126x.set_sleep();

	power_off_dev();
	//digitalWrite(SX126X_RST, LOW);
#endif

	if (!e) {
		// send message succesful, update the old data
	}

#ifdef DEBUG
	endSend = millis();

	//INFO("LoRa Sent in ");
	//INFOLN(endSend - startSend);
#endif

	INFO("TX state: ");
	INFOLN(e);
}

void loop()
{
	if (0x5a == need_push) {

		WDOG_Feed();

		push_data();

		need_push = 0;
	}

#if defined(CONFIG_V0)
	digitalWrite(SX1272_RST, LOW);
	spi_end();
#endif

	//wire_end();
	//digitalWrite(SCL_PIN, HIGH);
	//digitalWrite(SDA_PIN, HIGH);

	power_off_dev();

	EMU_EnterEM2(true);
}