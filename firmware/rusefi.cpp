/**
 * @file	rusefi.cpp
 * @brief Initialization code and main status reporting look
 *
 * @date Dec 25, 2013
 * @author Andrey Belomutskiy, (c) 2012-2017
 */

/**
 * @mainpage
 *
 * @section sec_into
 *
 * rusEfi is implemented based on the idea that with modern 100+ MHz microprocessors the relatively
 * undemanding task of internal combustion engine control could be implemented in a high-level, processor-independent
 * (to some extent) manner. Thus the key concepts of rusEfi: dependency on high-level hardware abstraction layer, software-based PWM etc.
 *
 * @section sec_main Brief overview
 *
 * rusEfi runs on crank shaft or cam shaft ('trigger') position sensor events.
 * Once per crank shaft revolution we evaluate the amount of needed fuel and
 * the spark timing. Once we have decided on the parameters for this revolution
 * we schedule all the actions to be triggered by the closest trigger event.
 *
 * We also have some utility threads like idle control thread and communication threads.
 *
 *
 *
 * @section sec_trigger Trigger Decoding
 *
 * Our primary trigger decoder is based on the idea of synchronizing the primary shaft signal and simply counting events on
 * the secondary signal. A typical scenario would be when cam shaft positions sensor is the primary signal and crankshaft is secondary,
 * but sometimes there would be two signals generated by two cam shaft sensors.
 * Another scenario is when we only have crank shaft position sensor, this would make it the primary signal and there would be no secondary signal.
 *
 * There is no software filtering so the signals are expected to be valid. TODO: in reality we are still catching engine stop noise as unrealisticly high RPM.
 *
 * The decoder is configured to act either on the primary signal rise or on the primary signal fall. It then compares the duration
 * of time from the previous signal to the duration of time from the signal before previous, and if the ratio falls into the configurable
 * range between 'syncRatioFrom' and 'syncRatioTo' this is assumed to be the synchronizing event.
 *
 * For instance, for a 36/1 skipped tooth wheel the ratio range for synchronization is from 1.5 to 3
 *
 * Some triggers do not require synchronization, this case we just count signals.
 * A single tooth primary signal would be a typical example when synchronization is not needed.
 *
 *
 *
 *
 *
 * @section sec_scheduler Event Scheduler
 *
 * It is a general agreement to measure all angles in crank shaft angles. In a four stroke
 * engine, a full cycle consists of two revolutions of the crank shaft, so all the angles are
 * running between 0 and 720 degrees.
 *
 * Ignition timing is a great example of a process which highlights the need of a hybrid
 * approach to event scheduling.
 * The most important part of controlling ignition
 * is firing up the spark at the right moment - so, for this job we need 'angle-based' timing,
 * for example we would need to fire up the spark at 700 degrees. Before we can fire up the spark
 * at 700 degrees, we need to charge the ignition coil, for example this dwell time is 4ms - that
 * means we need to turn on the coil at '4 ms before 700 degrees'. Let's  assume that the engine is
 * current at 600 RPM - that means 360 degrees would take 100ms so 4ms is 14.4 degrees at current RPM which
 * means we need to start charting the coil at 685.6 degrees.
 *
 * The position sensors at our disposal are not providing us the current position at any moment of time -
 * all we've got is a set of events which are happening at the knows positions. For instance, let's assume that
 * our sensor sends as an event at 0 degrees, at 90 degrees, at 600 degrees and and 690 degrees.
 *
 * So, for this particular sensor the most precise scheduling would be possible if we schedule coil charting
 * as '85.6 degrees after the 600 degrees position sensor event', and spark firing as
 * '10 degrees after the 690 position sensor event'. Considering current RPM, we calculate that '10 degress after' is
 * 2.777ms, so we schedule spark firing at '2.777ms after the 690 position sensor event', thus combining trigger events
 * with time-based offset.
 *
 * @section config Persistent Configuration
 * engine_configuration_s structure is kept in the internal flash memory, it has all the settings. Currently rusefi.ini has a direct mapping of this structure.
 *
 * Please note that due to TunerStudio protocol it's important to have the total structure size in synch between the firmware and TS .ini file -
 * just to make sure that this is not forgotten the size of the structure is hard-coded as PAGE_0_SIZE constant. There is always some 'unused' fields added in advance so that
 * one can add some fields without the pain of increasing the total configuration page size.
 * <br>See flash_main.cpp
 *
 *
 * @section sec_fuel_injection Fuel Injection
 *
 *
 * @sectuion sec_misc Misc
 *
 * <BR>See main_trigger_callback.cpp for main trigger event handler
 * <BR>See fuel_math.cpp for details on fuel amount logic
 * <BR>See rpm_calculator.cpp for details on how getRpm() is calculated
 *
 */

#include "main.h"
#include "trigger_structure.h"
#include "hardware.h"
#include "engine_controller.h"
#include "efiGpio.h"

#include "global.h"
#include "rfi_perftest.h"
#include "rusefi.h"
#include "memstreams.h"

#include "eficonsole.h"
#include "status_loop.h"
#include "pin_repository.h"
#include "flash_main.h"
#include "algo.h"

#if EFI_HD44780_LCD
#include "lcd_HD44780.h"
#endif /* EFI_HD44780_LCD */

#if EFI_ENGINE_EMULATOR || defined(__DOXYGEN__)
#include "engine_emulator.h"
#endif /* EFI_ENGINE_EMULATOR */

LoggingWithStorage sharedLogger("main");

bool main_loop_started = false;

static char panicMessage[200];

extern bool hasFirmwareErrorFlag;
extern fatal_msg_t errorMessageBuffer;

static virtual_timer_t resetTimer;

EXTERN_ENGINE
;

// todo: move this into a hw-specific file
static void rebootNow(void) {
	NVIC_SystemReset();
}

/**
 * Some configuration changes require full firmware reset.
 * Once day we will write graceful shutdown, but that would be one day.
 */
static void scheduleReboot(void) {
	scheduleMsg(&sharedLogger, "Rebooting in 5 seconds...");
	lockAnyContext();
	chVTSetI(&resetTimer, MS2ST(5000), (vtfunc_t) rebootNow, NULL);
	unlockAnyContext();
}

void runRusEfi(void) {
	efiAssertVoid(getRemainingStack(chThdGetSelfX()) > 512, "init s");
	assertEngineReference(PASS_ENGINE_PARAMETER_F);
	initIntermediateLoggingBuffer();
	initErrorHandling();

#if EFI_SHAFT_POSITION_INPUT || defined(__DOXYGEN__)
	/**
	 * This is so early because we want to init logger
	 * which would be used while finding trigger sync index
	 * while reading configuration
	 */
	initTriggerDecoderLogger(&sharedLogger);
#endif /* EFI_SHAFT_POSITION_INPUT */

	/**
	 * we need to initialize table objects before default configuration can set values
	 */
	initDataStructures(PASS_ENGINE_PARAMETER_F);

	/**
	 * First thing is reading configuration from flash memory.
	 * In order to have complete flexibility configuration has to go before anything else.
	 */
	readConfiguration(&sharedLogger);
	prepareVoidConfiguration(&activeConfiguration);

	/**
	 * First data structure keeps track of which hardware I/O pins are used by whom
	 */
	initPinRepository();

	/**
	 * Next we should initialize serial port console, it's important to know what's going on
	 */
	initializeConsole(&sharedLogger);

	engine->setConfig(config);

	addConsoleAction("reboot", scheduleReboot);

	/**
	 * Initialize hardware drivers
	 */
	initHardware(&sharedLogger);

	initStatusLoop(engine);
	/**
	 * Now let's initialize actual engine control logic
	 * todo: should we initialize some? most? controllers before hardware?
	 */
	initEngineContoller(&sharedLogger PASS_ENGINE_PARAMETER_F);

#if EFI_PERF_METRICS || defined(__DOXYGEN__)
	initTimePerfActions(&sharedLogger);
#endif
        
#if EFI_ENGINE_EMULATOR || defined(__DOXYGEN__)
	initEngineEmulator(&sharedLogger, engine);
#endif
	startStatusThreads(engine);

	rememberCurrentConfiguration();

	print("Running main loop\r\n");
	main_loop_started = true;
	/**
	 * This loop is the closes we have to 'main loop' - but here we only publish the status. The main logic of engine
	 * control is around main_trigger_callback
	 */
	while (true) {
		efiAssertVoid(getRemainingStack(chThdGetSelfX()) > 128, "stack#1");

#if (EFI_CLI_SUPPORT && !EFI_UART_ECHO_TEST_MODE) || defined(__DOXYGEN__)
		// sensor state + all pending messages for our own dev console
		updateDevConsoleState(engine);
#endif /* EFI_CLI_SUPPORT */

		chThdSleepMilliseconds(boardConfiguration->consoleLoopPeriod);
	}
}

void chDbgStackOverflowPanic(thread_t *otp) {
        (void)otp;
	strcpy(panicMessage, "stack overflow: ");
#if defined(CH_USE_REGISTRY) || defined(__DOXYGEN__)
	int p_name_len = strlen(otp->p_name);
	if (p_name_len < sizeof(panicMessage) - 2)
		strcat(panicMessage, otp->p_name);
#endif
	chDbgPanic3(panicMessage, __FILE__, __LINE__);
}

int getRusEfiVersion(void) {
	return 20170318;
}
