
#include "Arduino.h"

//#define	DEBUG				1

#ifdef DEBUG
#define	INFO(x)				Serial.println(x)
#else
#define	INFO(x)
#endif

#define TIME_OUTT 1000

static uint8_t I2C_ADDRESS = 0x78;

/*
 * BMI160: I2C_ADDR = 0x68 / 0x69, set i2c_addr = 0xd0 (0x68 << 1)
 *
*/

void i2c_init(uint8_t addr)
{
	I2C_Init_TypeDef init_I2C = 
	{
		.enable = true,
		.clhr = i2cClockHLRStandard,
		.freq = I2C_FREQ_STANDARD_MAX,
		.master = true,
		.refFreq = 0
	};

	I2C_ADDRESS = addr;
	
	/* Enabling clock to the I2C, GPIO*/
	CMU_ClockEnable(cmuClock_HFPER, true);
	CMU_ClockEnable(cmuClock_I2C0, true);
	CMU_ClockEnable(cmuClock_GPIO, true);
	
	/* Starting LFXO and waiting until it is stable */
	CMU_OscillatorEnable(cmuOsc_LFRCO, true, true);

	/* Routing the LFXO clock to the RTC */
	CMU_ClockSelectSet(cmuClock_LFA, cmuSelect_LFRCO);

	/* PE12 - SDA, PE13 - SCL */
	GPIO_PinModeSet(gpioPortD, 6, gpioModeWiredAndPullUpFilter, 1);
	GPIO_PinModeSet(gpioPortD, 7, gpioModeWiredAndPullUpFilter, 1);

	GPIO_PinModeSet(gpioPortE, GPIO_PIN_12, gpioModeWiredAndPullUpFilter, 1);
	GPIO_PinModeSet(gpioPortE, GPIO_PIN_13, gpioModeWiredAndPullUpFilter, 1);

	/* Configure interrupt pin*/
	//GPIO_PinModeSet(gpioPortC, 4, gpioModeInput, 0);
	
	//I2C0->ROUTE |= I2C_ROUTE_LOCATION_LOC2 + I2C_ROUTE_SCLPEN + I2C_ROUTE_SDAPEN;
	I2C0->ROUTE = I2C_ROUTE_SDAPEN
					| I2C_ROUTE_SCLPEN
					| (0 << _I2C_ROUTE_LOCATION_SHIFT);

	I2C_Init(I2C0, &init_I2C);

	I2C0->CTRL |= I2C_CTRL_AUTOSN;
	//I2C0->CTRL |= I2C_CTRL_AUTOACK | I2C_CTRL_AUTOSN;

	//NVIC_EnableIRQ(I2C0_IRQn);
}

I2C_TransferReturn_TypeDef i2c_write(uint8_t *txbuf, int8_t txlen, uint8_t *rxbuf, uint8_t rxlen)
{
	uint32_t loop = TIME_OUTT;

	I2C_TransferReturn_TypeDef st;

	I2C_TransferSeq_TypeDef tx_init;

	tx_init.addr = I2C_ADDRESS;
	tx_init.buf[0].data = txbuf;
	tx_init.buf[0].len = txlen;
	tx_init.buf[1].data = rxbuf;
	tx_init.buf[1].len = rxlen;
	tx_init.flags = I2C_FLAG_WRITE;
	
	st = I2C_TransferInit(I2C0, &tx_init);

	while ((st == i2cTransferInProgress) && loop--)
	{
		st = I2C_Transfer(I2C0);
	}	

	if (loop == TIME_OUTT) {
		//Serial.println("i2c_wirte loop faild...");
		st = -5;
	}

	return st;
}
 
I2C_TransferReturn_TypeDef i2c_read(uint8_t raddr, uint8_t *rxData, uint8_t readLen)
{
	uint32_t loop = TIME_OUTT;

	I2C_TransferReturn_TypeDef st;

	I2C_TransferSeq_TypeDef rx_Init = 
	{
		.addr = I2C_ADDRESS,
		.buf[0].data = &raddr,
		.buf[0].len = 1,
		.buf[1].data = rxData,
		.buf[1].len = readLen,
		.flags = I2C_FLAG_WRITE_READ,
	};

	/* Do a polled transfer */
	st = I2C_TransferInit(I2C0, &rx_Init);

	while ((st == i2cTransferInProgress) && (loop--))
	{
		/* Enter EM1 while waiting for I2C interrupt */
		st = I2C_Transfer(I2C0);
		//EMU_EnterEM1();
		/* Could do a timeout function here. */
	}

	if (loop == TIME_OUTT) {
		//Serial.println("i2c_read loop fail...");
		st = -5;
	}

	return(st);
}
 
/* Interrupts */
#if 0
void enableI2cSlaveInterrupts(void)
{
	I2C_IntClear(I2C0, I2C_IFC_ADDR | I2C_IF_RXDATAV | I2C_IFC_SSTOP);
	I2C_IntEnable(I2C0, I2C_IEN_ADDR | I2C_IEN_RXDATAV | I2C_IEN_SSTOP);
	NVIC_EnableIRQ(I2C0_IRQn);
}

void disableI2cInterrupts(void)
{
	NVIC_DisableIRQ(I2C0_IRQn);
	I2C_IntDisable(I2C0, I2C_IEN_ADDR | I2C_IEN_RXDATAV | I2C_IEN_SSTOP);
	I2C_IntClear(I2C0, I2C_IFC_ADDR | I2C_IF_RXDATAV | I2C_IFC_SSTOP);
}

void I2C0_IRQHandler(void)
{
	int status;

	status = I2C0->IF;

	if (status & I2C_IF_ADDR) {
		// Address Match
		// Indicating that reception is started
		i2c_rxInProgress = true;
		I2C0->RXDATA;
		i2c_rxBufferIndex = 0;

		I2C_IntClear(I2C0, I2C_IFC_ADDR);

	} else if (status & I2C_IF_RXDATAV) {
		// Data received
		i2c_rxBuffer[i2c_rxBufferIndex] = I2C0->RXDATA;
		i2c_rxBufferIndex++;
	}

	if (status & I2C_IEN_SSTOP) {
		// Stop received, reception is ended
		I2C_IntClear(I2C0, I2C_IFC_SSTOP);
		i2c_rxInProgress = false;
		i2c_rxBufferIndex = 0;
	}
}

void GPIO_EVEN_IRQHandler(void)
{
	// Clear pending
	uint32_t interruptMask = GPIO_IntGet();
	GPIO_IntClear(interruptMask);

	// If RX is not in progress, a new transfer is started
	if (!i2c_rxInProgress) {
		disableI2cInterrupts();
		i2c_startTx = true;
	}
}

void GPIO_ODD_IRQHandler(void)
{
	// Clear pending
	uint32_t interruptMask = GPIO_IntGet();
	GPIO_IntClear(interruptMask);

	// If RX is not in progress, a new transfer is started
	if (!i2c_rxInProgress) {
		disableI2cInterrupts();
		i2c_startTx = true;
	}
}
#endif