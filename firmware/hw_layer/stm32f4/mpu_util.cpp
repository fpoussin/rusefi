/**
 * @file	mpu_util.cpp
 *
 * @date Jul 27, 2014
 * @author Andrey Belomutskiy, (c) 2012-2016
 */

#include "main.h"
#include "mpu_util.h"
#include "error_handling.h"
#include "engine.h"
#include "pin_repository.h"

EXTERN_ENGINE;

extern "C" {
int getRemainingStack(thread_t *otp);
void prvGetRegistersFromStack(uint32_t *pulFaultStackAddress);
}

extern uint32_t __main_stack_base__;

#define GET_CFSR() (*((volatile uint32_t *) (0xE000ED28)))

#if defined __GNUC__
// GCC version

int getRemainingStack(thread_t *otp) {

#if CH_DBG_ENABLE_STACK_CHECK
	register struct intctx *r13 asm ("r13");
	otp->activeStack = r13;

	int remainingStack;
	if (dbg_isr_cnt > 0) {
		// ISR context
		remainingStack = (int)(r13 - 1) - (int)&__main_stack_base__;
	} else {
		remainingStack = (int)(r13 - 1) - (int)otp->p_stklimit;
	}
	otp->remainingStack = remainingStack;
	return remainingStack;
#else
	return 99999;
#endif /* CH_DBG_ENABLE_STACK_CHECK */
}

#else /* __GNUC__ */

extern uint32_t CSTACK$$Base; /* symbol created by the IAR linker */
extern uint32_t IRQSTACK$$Base; /* symbol created by the IAR linker */

int getRemainingStack(thread_t *otp) {
#if CH_DBG_ENABLE_STACK_CHECK || defined(__DOXYGEN__)
	int remainingStack;
	if (dbg_isr_cnt > 0) {
		remainingStack = (__get_SP() - sizeof(struct intctx)) - (int)&IRQSTACK$$Base;
	} else {
		remainingStack = (__get_SP() - sizeof(struct intctx)) - (int)otp->p_stklimit;
	}
	otp->remainingStack = remainingStack;
	return remainingStack;
#else
	return 999999;
#endif  
}

// IAR version

#endif

void baseHardwareInit(void) {
	// looks like this holds a random value on start? Let's set a nice clean zero
	DWT_CYCCNT = 0;
}

void DebugMonitorVector(void) {

	chDbgPanic3("DebugMonitorVector", __FILE__, __LINE__);

	while (TRUE)
		;
}

void UsageFaultVector(void) {

	chDbgPanic3("UsageFaultVector", __FILE__, __LINE__);

	while (TRUE)
		;
}

void BusFaultVector(void) {

	chDbgPanic3("BusFaultVector", __FILE__, __LINE__);

	while (TRUE) {
	}
}

/**
 + * @brief   Register values for postmortem debugging.
 + */
volatile uint32_t postmortem_r0;
volatile uint32_t postmortem_r1;
volatile uint32_t postmortem_r2;
volatile uint32_t postmortem_r3;
volatile uint32_t postmortem_r12;
volatile uint32_t postmortem_lr; /* Link register. */
volatile uint32_t postmortem_pc; /* Program counter. */
volatile uint32_t postmortem_psr;/* Program status register. */
volatile uint32_t postmortem_CFSR;
volatile uint32_t postmortem_HFSR;
volatile uint32_t postmortem_DFSR;
volatile uint32_t postmortem_AFSR;
volatile uint32_t postmortem_BFAR;
volatile uint32_t postmortem_MMAR;
volatile uint32_t postmortem_SCB_SHCSR;

/**
 * @brief   Evaluates to TRUE if system runs under debugger control.
 * @note    This bit resets only by power reset.
 */
#define is_under_debugger() (((CoreDebug)->DHCSR) & \
                            CoreDebug_DHCSR_C_DEBUGEN_Msk)

/**
 *
 */
void prvGetRegistersFromStack(uint32_t *pulFaultStackAddress) {

	postmortem_r0 = pulFaultStackAddress[0];
	postmortem_r1 = pulFaultStackAddress[1];
	postmortem_r2 = pulFaultStackAddress[2];
	postmortem_r3 = pulFaultStackAddress[3];
	postmortem_r12 = pulFaultStackAddress[4];
	postmortem_lr = pulFaultStackAddress[5];
	postmortem_pc = pulFaultStackAddress[6];
	postmortem_psr = pulFaultStackAddress[7];

	/* Configurable Fault Status Register. Consists of MMSR, BFSR and UFSR */
	postmortem_CFSR = GET_CFSR();

	/* Hard Fault Status Register */
	postmortem_HFSR = (*((volatile uint32_t *) (0xE000ED2C)));

	/* Debug Fault Status Register */
	postmortem_DFSR = (*((volatile uint32_t *) (0xE000ED30)));

	/* Auxiliary Fault Status Register */
	postmortem_AFSR = (*((volatile uint32_t *) (0xE000ED3C)));

	/* Read the Fault Address Registers. These may not contain valid values.
	 Check BFARVALID/MMARVALID to see if they are valid values
	 MemManage Fault Address Register */
	postmortem_MMAR = (*((volatile uint32_t *) (0xE000ED34)));
	/* Bus Fault Address Register */
	postmortem_BFAR = (*((volatile uint32_t *) (0xE000ED38)));

	postmortem_SCB_SHCSR = SCB->SHCSR;

	if (is_under_debugger()) {
		__asm("BKPT #0\n");
		// Break into the debugger
	}

	/* harmless infinite loop */
	while (1) {
		;
	}
}

void HardFaultVector(void) {
#if 0 && defined __GNUC__
	__asm volatile (
			" tst lr, #4                                                \n"
			" ite eq                                                    \n"
			" mrseq r0, msp                                             \n"
			" mrsne r0, psp                                             \n"
			" ldr r1, [r0, #24]                                         \n"
			" ldr r2, handler2_address_const                            \n"
			" bx r2                                                     \n"
			" handler2_address_const: .word prvGetRegistersFromStack    \n"
	);

#else
#endif        

	int cfsr = GET_CFSR();
	if (cfsr & 0x1) {
		chDbgPanic3("H IACCVIOL", __FILE__, __LINE__);
	} else if (cfsr & 0x100) {
		chDbgPanic3("H IBUSERR", __FILE__, __LINE__);
	} else if (cfsr & 0x20000) {
		chDbgPanic3("H INVSTATE", __FILE__, __LINE__);
	} else {
		chDbgPanic3("HardFaultVector", __FILE__, __LINE__);
	}

	while (TRUE) {
	}
}

#if HAL_USE_SPI || defined(__DOXYGEN__)
static bool isSpiInitialized[5] = { false, false, false, false, false };

static int getSpiAf(SPIDriver *driver) {
#if STM32_SPI_USE_SPI1
	if (driver == &SPID1) {
		return EFI_SPI1_AF;
	}
#endif
#if STM32_SPI_USE_SPI2
	if (driver == &SPID2) {
		return EFI_SPI2_AF;
	}
#endif
#if STM32_SPI_USE_SPI3
	if (driver == &SPID3) {
		return EFI_SPI3_AF;
	}
#endif
	return -1;
}

brain_pin_e getMisoPin(spi_device_e device) {
	switch(device) {
	case SPI_DEVICE_1:
		return boardConfiguration->spi1misoPin;
	case SPI_DEVICE_2:
		return boardConfiguration->spi2misoPin;
	case SPI_DEVICE_3:
		return boardConfiguration->spi3misoPin;
	default:
		break;
	}
	return GPIO_UNASSIGNED;
}

brain_pin_e getMosiPin(spi_device_e device) {
	switch(device) {
	case SPI_DEVICE_1:
		return boardConfiguration->spi1mosiPin;
	case SPI_DEVICE_2:
		return boardConfiguration->spi2mosiPin;
	case SPI_DEVICE_3:
		return boardConfiguration->spi3mosiPin;
	default:
		break;
	}
	return GPIO_UNASSIGNED;
}

brain_pin_e getSckPin(spi_device_e device) {
	switch(device) {
	case SPI_DEVICE_1:
		return boardConfiguration->spi1sckPin;
	case SPI_DEVICE_2:
		return boardConfiguration->spi2sckPin;
	case SPI_DEVICE_3:
		return boardConfiguration->spi3sckPin;
	default:
		break;
	}
	return GPIO_UNASSIGNED;
}

void turnOnSpi(spi_device_e device) {
	if (isSpiInitialized[device])
		return; // already initialized
	isSpiInitialized[device] = true;
	if (device == SPI_DEVICE_1) {
// todo: introduce a nice structure with all fields for same SPI
#if STM32_SPI_USE_SPI1
//	scheduleMsg(&logging, "Turning on SPI1 pins");
		initSpiModule(&SPID1, getSckPin(device),
				getMisoPin(device),
				getMosiPin(device),
				0,
				0,
				0);
#endif /* STM32_SPI_USE_SPI1 */
	}
	if (device == SPI_DEVICE_2) {
#if STM32_SPI_USE_SPI2
//	scheduleMsg(&logging, "Turning on SPI2 pins");
		initSpiModule(&SPID2, getSckPin(device),
				getMisoPin(device),
				getMosiPin(device),
				engineConfiguration->spi2SckMode,
				engineConfiguration->spi2MosiMode,
				engineConfiguration->spi2MisoMode);
#endif /* STM32_SPI_USE_SPI2 */
	}
	if (device == SPI_DEVICE_3) {
#if STM32_SPI_USE_SPI3
//	scheduleMsg(&logging, "Turning on SPI3 pins");
		initSpiModule(&SPID3, getSckPin(device),
				getMisoPin(device),
				getMosiPin(device),
				0,
				0,
				0);
#endif /* STM32_SPI_USE_SPI3 */
	}
}

void initSpiModule(SPIDriver *driver, brain_pin_e sck, brain_pin_e miso,
		brain_pin_e mosi,
		int sckMode,
		int mosiMode,
		int misoMode) {

	mySetPadMode2("SPI clock", sck,	PAL_MODE_ALTERNATE(getSpiAf(driver)) + sckMode);

	mySetPadMode2("SPI master out", mosi, PAL_MODE_ALTERNATE(getSpiAf(driver)) + mosiMode);
	mySetPadMode2("SPI master in ", miso, PAL_MODE_ALTERNATE(getSpiAf(driver)) + misoMode);
}

void initSpiCs(SPIConfig *spiConfig, brain_pin_e csPin) {
	spiConfig->end_cb = NULL;
	ioportid_t port = getHwPort(csPin);
	ioportmask_t pin = getHwPin(csPin);
	spiConfig->ssport = port;
	spiConfig->sspad = pin;
	mySetPadMode2("chip select", csPin, PAL_STM32_MODE_OUTPUT);
}

#endif
