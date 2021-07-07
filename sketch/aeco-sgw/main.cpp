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

#include "m5311.h"

extern "C"{
#include "timex.h"
}

#include "rtcdriver.h"
#include "math.h"
#include "em_wdog.h"

#include "sx126x.h"

#include "circ_buf.h"

#define DEBUG							1
#define DEBUG_HEX_PKT					1
//#define ENABLE_RTC						1
//#define DEBUG_RTC						1

#define ENABLE_LORA						1
#define ENABLE_RX_IRQ					1

#define	FW_VER						"V1.1"

#ifdef ENABLE_RTC
#include "softi2c.h"
#include "pcf8563.h"
#endif

/* Timer used for bringing the system back to EM0. */
RTCDRV_TimerID_t xTimerForWakeUp;

static uint32_t check_period = 18;		/* 20s = 0.33min */
static uint32_t sample_period = 90;		/* 20s * 90 = 1800s, 30min */

#define WDOG_PERIOD				wdogPeriod_32k			/* 32k 1kHz periods should give 32 seconds */

#define SAMPLE_PERIOD			90		/* check_period x SAMPLE_PERIOD = 1800s (30min) */
#define PUSH_PERIOD				1080	/* check_period x PUSH_PERIOD = 360min, 6h */
//#define	MY_RPT_MSG			"{\"ts\":%d`\"gid\":\"%s\"`\"B\":%s`\"T\":%s`\"iT\":%d`\"tp\":%d`\"L\":%d`\"sid\":\"%s\"}"

#define INIT_TS						1614665566UL
#define MAX_TS						2000000000UL

static uint32_t sample_count = 0;

static float cur_temp = 0.0;
static float old_temp = 0.0;

#define	PWR_CTRL_PIN			8		/* PIN17_PC14_D8 */
//#define MODEM_ON_PIN			2		/* PIN6_PB8_D2 */
//#define MODEM_ON_PIN			14		/* PIN19_PF00_D14 */
//#define MODEM_ON_PIN			9		/* PIN18_PC15_D9 */
#define MODEM_ON_PIN			0		/* PIN01_PA00_D0 */

#define	KEY_PIN					15		/* PIN20_PF01_D15 */

#define	RF_INT_PIN				3		/* PIN8_PB11_D3 */
#define	RTC_INT_PIN				16		/* PIN21_PF02_D16 */

float cur_vbat __attribute__((aligned(4))) = 0.0;
bool vbat_low __attribute__((aligned(4))) = false;
int cnt_vbat_low __attribute__((aligned(4))) = 0;
int cnt_vbat_ok __attribute__((aligned(4))) = 0;

static bool need_sleep __attribute__((aligned(4))) = false;
uint32_t cnt_sleep __attribute__((aligned(4))) = 0;

uint16_t tx_count = 0;

#define	MQTT_MSG		"{\"ts\":%d`\"gwid\":\"%s\"`\"gid\":\"%s\"`\"B\":%s`\"tp\":%d`\"sgi\":%d`\"%s}"

#ifdef DEBUG
#define INFO_S(param)			Serial.print(F(param))
#define INFO_HEX(param)			Serial.print(param,HEX)
#define INFO(param)				Serial.print(param)
#define INFOLN(param)			Serial.println(param)
#define FLUSHOUTPUT				Serial.flush();
#else
#define INFO_S(param)
#define INFO(param)
#define INFOLN(param)
#define FLUSHOUTPUT
#endif

#define	RESET_TX			0
#define	DELTA_TX			1
#define	TIMER_TX			2
#define	KEY_TX				3
#define	EL_TX				4
#define	WL_TX				5

struct circ_buf g_cbuf __attribute__((aligned(4)));

int tx_cause = RESET_TX;

M5311 modem;

char iccid[24] __attribute__((aligned(4)));
int g_rssi = 0;

///////////////////////////////////////////////////////////////
#include "ccx.h"
#include "crypto.h"
#include "tx_ctrl.h"

#define TXRX_CH					472500000
#define TX_PWR					22

uint8_t pbuf[48] __attribute__((aligned(4)));;
struct ctrl_fifo g_cfifo __attribute__((aligned(4)));

SX126x lora(2,		// Pin: SPI CS,PIN06-PB08-D2
	    9,				// Pin: RESET, PIN18-PC15-D9
	    5,				// PIN: Busy,  PIN11-PB14-D5, The RX pin
	    3				// Pin: DIO1,  PIN08-PB11-D3
    );

inline void setup_lora()
{
	lora.reset();
	lora.init();
	lora.setup_v0(TXRX_CH, TX_PWR);
}

#ifdef ENABLE_RX_IRQ
void rx_irq_handler()
{
	NVIC_DisableIRQ(GPIO_ODD_IRQn);
	NVIC_DisableIRQ(GPIO_EVEN_IRQn);

	int plen = 0, rssi = 0;

	WDOG_Feed();

	//INFO("rx: ");

	plen = lora.get_rx_pkt(pbuf, 48);
	rssi = lora.get_pkt_rssi();

	//INFOLN(plen);

	if (plen > PKT_LEN || plen < 0) goto rx_irq_out;

	if ((true == is_our_pkt(pbuf, plen))
		&& (false == is_pkt_in_ctrl(&g_cfifo, pbuf, plen, seconds()))) {

		push_point(&g_cbuf, pbuf, rssi, plen, seconds());

		/* push the pkt data into tx_ctrl structure */
		check_ctrl_fno(&g_cfifo, pbuf, plen);
	}

rx_irq_out:
	NVIC_ClearPendingIRQ(GPIO_ODD_IRQn);
	NVIC_ClearPendingIRQ(GPIO_EVEN_IRQn);
	NVIC_EnableIRQ(GPIO_ODD_IRQn);
	NVIC_EnableIRQ(GPIO_EVEN_IRQn);
}
#endif
///////////////////////////////////////////////////////////////

void push_data();
int8_t fetch_mcu_temp();

uint64_t get_devid()
{
	uint64_t *p;

	p = (uint64_t *)0x0FE00008;

	return *p;
}

void power_on_dev()
{
	digitalWrite(PWR_CTRL_PIN, HIGH);
}

void power_off_dev()
{
	digitalWrite(PWR_CTRL_PIN, LOW);
}

void power_on_modem()
{
	digitalWrite(PWR_CTRL_PIN, HIGH);

#if 1
	digitalWrite(MODEM_ON_PIN, HIGH);
	delay(2200);
	digitalWrite(MODEM_ON_PIN, LOW);
	delay(200);
#endif
}

void power_off_modem()
{
	digitalWrite(MODEM_ON_PIN, HIGH);
	delay(8300);
	digitalWrite(MODEM_ON_PIN, LOW);

	digitalWrite(PWR_CTRL_PIN, LOW);
}

void wakeup_modem()
{
	digitalWrite(MODEM_ON_PIN, HIGH);
	delay(100);
	digitalWrite(MODEM_ON_PIN, LOW);
	delay(200);
}

void check_sensor(RTCDRV_TimerID_t id, void *user)
{
	int ret = 0;

	WDOG_Feed();

	RTCDRV_StopTimer(xTimerForWakeUp);

	fix_seconds(check_period + check_period/9);

	//INFOLN(seconds());

	sample_count++;

	if (sample_count % sample_period == 0) {
		/* 20s x 90 = 1200s, 30min, sample a point */

		#if 0
		pcf8563_init(SCL_PIN, SDA_PIN);
		ret = push_point(&g_cbuf, pcf8563_now(), (cur_temp * 10), fetch_mcu_temp(), cur_water);
		ret = push_point(&g_cbuf, seconds(), (cur_temp * 10), fetch_mcu_temp(), cur_water);
		#endif

		if (ret == 0) {
			/*
			 * point is saved ok
			 * reset sample period to 30min
			*/
			sample_period = 90;

		} else if (ret == 1) {
			/*
			 * cbuf is full
			 * change the sample period to 60min
			*/
			sample_period = 180;
		}
	}

	if (sample_count >= PUSH_PERIOD) {

		/* 4min * 15 =  60min */
		need_sleep = false;
		tx_cause = TIMER_TX;
		sample_count = 0;
	}
}

void trig_check_sensor()
{
	noInterrupts();

	need_sleep = false;
	tx_cause = KEY_TX;
	INFOLN("key");

	interrupts();
}

int setup_nb()
{
	bool network_ok = false;
	int start_cnt = 0;

	power_on_dev();		// turn on device power

	INFOLN("....");
	SerialUSART1.setRouteLoc(3);
	INFOLN("....");
	SerialUSART1.begin(115200);
	INFOLN("xxxx");

	modem.init(&SerialUSART1);

qsetup_start:

	INFOLN(__LINE__);
	WDOG_Feed();
	INFOLN(__LINE__);
	power_on_modem();
	INFOLN(__LINE__);

	delay(1200);
	modem.init_modem();

	memset(iccid, 0, 24);
	modem.get_iccid().toCharArray(iccid, 24);

	int ret = 0;
	ret = modem.check_boot();
	start_cnt++;

	if (ret == 1) {

		network_ok = true;

	} else if ((ret == 2 || ret == 0) && start_cnt < 3) {
		INFOLN(__LINE__);
		//power_off_dev();
		INFOLN(__LINE__);
		delay(1000);
		INFOLN(__LINE__);
		goto qsetup_start;
		INFOLN(__LINE__);
	} else {

		for (int i = 0; i < 15; i++) {

			WDOG_Feed();
			ret = modem.check_network();

			if (ret == 1) {
				network_ok = true;
				break;

			} else {

				INFOLN("Try to wakeup modem");
				wakeup_modem();
			}

			delay(1000);

			INFOLN("network check");

		#if 0
			power_off_dev();
			delay(1000);
			power_on_dev();

			power_on_modem();
		#else
			//reset_modem();
		#endif

			//modem.init_modem();
		}

	}

	if (network_ok) {

		//modem.disable_deepsleep();

		//INFOLN("IMEI = " + modem.get_imei());
		//INFOLN("IMSI = " + modem.get_imsi());
		//INFOLN(modem.check_ipaddr());

		extern char str[BUF_LEN];
		memset(str, 0, BUF_LEN);

		WDOG_Feed();

		g_rssi = modem.get_csq();
		INFOLN(g_rssi);

		//char strtest[] = "21/02/26,06:22:38+32";
		modem.get_net_time().toCharArray(str, BUF_LEN);
		INFOLN(str);

		uint32_t sec = str2seconds(str);

		if (sec > INIT_TS && sec < MAX_TS) {
			update_seconds(sec);

		#ifdef ENABLE_RTC
			pcf8563_init(SCL_PIN, SDA_PIN);
			pcf8563_set_from_seconds(sec);
		#endif
		}

		INFO("epoch = ");
		INFOLN(seconds());
		#ifdef ENABLE_RTC
		INFOLN(pcf8563_now());
		#endif
	} else {
		/* attach network timeout */
		power_on_modem();
		delay(100);
		modem.clean_net_cache();
		modem.enter_deepsleep();
		delay(2000);
	}

	return network_ok;
}

int8_t fetch_mcu_temp()
{
	float temp = 0.0;
	for(int i=0; i<3; i++) {
		temp += adc.temperatureCelsius();
	}
	temp /= 3.0;

	return (int8_t)roundf(temp);
}

void setup()
{
	Ecode_t e;

	WDOG_Init_TypeDef wInit = WDOG_INIT_DEFAULT;

	/* Watchdog setup - Use defaults, excepts for these : */
	wInit.em2Run = true;
	wInit.em3Run = true;
	wInit.perSel = WDOG_PERIOD;

	// dev power ctrl
	pinMode(PWR_CTRL_PIN, OUTPUT);
	power_off_dev();

	#if 1
	pinMode(MODEM_ON_PIN, OUTPUT);
	#else
	GPIO_PinModeSet(g_Pin2PortMapArray[MODEM_ON_PIN].GPIOx_Port,
					g_Pin2PortMapArray[MODEM_ON_PIN].Pin_abstraction,
					gpioModePushPullDrive, 0);
	GPIO_DriveModeSet(g_Pin2PortMapArray[MODEM_ON_PIN].GPIOx_Port,
					gpioDriveModeHigh);
	#endif
	digitalWrite(MODEM_ON_PIN, LOW);

	//pinMode(KEY_PIN, INPUT);
	//attachInterrupt(KEY_PIN, trig_check_sensor, FALLING);

#ifdef ENABLE_LORA
#ifdef ENABLE_RX_IRQ
	// RF Interrupt pin
	pinMode(RF_INT_PIN, INPUT);
	attachInterrupt(RF_INT_PIN, rx_irq_handler, RISING);
#endif
#endif

	/* Initialize RTC timer. */
	RTCDRV_Init();
	RTCDRV_AllocateTimer(&xTimerForWakeUp);

#ifdef DEBUG
	Serial.setRouteLoc(1);
	Serial.begin(115200);
#endif

	/* Start watchdog */
	WDOG_Init(&wInit);

	/* reset epoch */
	update_seconds(160015579);
	reset_ctrl_ts(&g_cfifo, seconds());

	/* boot tx */
	tx_cause = RESET_TX;

	INFOLN("\r\n\r\nWelcom to AE-SGW!");
	INFO("Firmware: ");
	INFOLN(FW_VER);
	INFO("DevID: ");
	INFOLN(uint64_to_str(myid_buf, get_devid()));

	INFO("epoch = ");
	INFOLN(seconds());

#ifdef ENABLE_RTC
	pcf8563_init(SCL_PIN, SDA_PIN);

	delay(1000);

	#ifdef DEBUG_RTC
	int ctrl = pcf8563_get_ctrl2();
	INFO("RTC ctrl2: ");
	INFO_HEX(ctrl);
	INFOLN("");

	if (ctrl == 0xFF) {
		/* Incorrect state of pcf8563 */
		NVIC_SystemReset();
	}
	#endif

	pcf8563_clear_timer();
#endif

#if 0
	push_point(&g_cbuf, seconds(), (cur_temp * 10), fetch_mcu_temp());
#endif

#ifdef ENABLE_LORA
	power_on_dev();
	setup_lora();
	lora.enter_rx();
#endif
}

void push_data()
{
	long cnt_fail = 0;

	struct pkt d;

	WDOG_Feed();

	float vbat = adc.readVbat();

	int ret = get_1st_point(&g_cbuf, &d);

	if (ret == 1) {
		INFOLN("No point in buf");
		return;
	}

	ret = setup_nb();

	if (ret == 1) {

		modem.mqtt_end();
		delay(100);

		char *my_devid = uint64_to_str(myid_buf, get_devid());

		WDOG_Feed();
		wakeup_modem();
		modem.mqtt_begin("mqtt.autoeco.net", 1883, my_devid);

		WDOG_Feed();
		wakeup_modem();

		if (modem.mqtt_connect()) {

			WDOG_Feed();

			while (get_1st_point(&g_cbuf, &d) == 0 && cnt_fail < 15) {

				WDOG_Feed();
				wakeup_modem();

				if (d.ts < INIT_TS || d.ts > MAX_TS) {
					INFOLN("Invalid ts, use current ts");
					d.ts = seconds();
					INFOLN(seconds());
				}

				extern char modem_said[MODEM_LEN];
				memset(modem_said, 0, MODEM_LEN);

				#if 0
				if (tx_cause == 0 || gmtime(&(d.ts))->tm_hour == 12) {

					sprintf(modem_said, MY_RPT_MSG,
							d.ts,
							my_devid,
							decode_vbat(vbat),
							decode_sensor_data(d.data / 10.0),
							d.iT,
							tx_cause,
							iccid
					);
				} else {
				}
				#endif

				sprintf(modem_said, MQTT_MSG,
						d.ts,
						my_devid,
						decode_devid(d.data, devid_buf),
						decode_vbat(d.data),
						decode_cmd(d.data),
						d.rssi,
						decode_sensor_data(d.data)
				);

				if (modem.mqtt_pub("dev/gw", modem_said)) {

					INFOLN("Pub OK, pop point");
					noInterrupts();
					pop_point(&g_cbuf, &d);
					interrupts();

				} else {

					cnt_fail++;
					INFOLN("Pub failed");
				}
			}
			INFOLN("no point");
		}

		WDOG_Feed();
		modem.mqtt_end();
	}

	WDOG_Feed();
}

void deep_sleep()
{
	power_off_modem();

	// dev power off
	power_off_dev();
}

struct pkt d;

void lora_rx_worker()
{
	noInterrupts();
	int ret = pop_point(&g_cbuf, &d);
	interrupts();

	if (ret != 0) {
		// no pkt
		return;
	}

	uint8_t *p = d.data;
	int p_len = d.plen;

#ifdef DEBUG_HEX_PKT
	int a = 0, b = 0;

	for (; a < p_len; a++, b++) {

		if ((uint8_t) p[a] < 16)
			INFO_S("0");

		INFO_HEX((uint8_t) p[a]);
		INFO_S(" ");
	}

	INFO_S("/");
	INFO(d.rssi);
	INFO_S("/");
	INFOLN(p_len);
#endif
}

void loop()
{
	if (need_sleep == false && 1 == digitalRead(KEY_PIN) && vbat_low == false) {
		/* storage mode */
		lora_rx_worker();
	}

	#if 0
	if (need_sleep == true || 0 == digitalRead(KEY_PIN)
		|| vbat_low == true) {

		INFOLN("deep sleep..");

		WDOG_Feed();

		deep_sleep();

		RTCDRV_StartTimer(xTimerForWakeUp, rtcdrvTimerTypeOneshot, check_period * 1000, check_sensor, NULL);

		EMU_EnterEM2(true);

		if (need_sleep == false && vbat_low == false) {
			power_on_dev();
			setup_lora();
		}
	}
	#endif
}