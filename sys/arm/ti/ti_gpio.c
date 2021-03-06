/*-
 * Copyright (c) 2011 Ben Gray <ben.r.gray@gmail.com>.
 * Copyright (c) 2014 Luiz Otavio O Souza <loos@FreeBSD.org>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/**
 * Beware that the OMAP4 datasheet(s) lists GPIO banks 1-6, whereas the code
 * here uses 0-5.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>

#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/gpio.h>
#include <sys/interrupt.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <arm/ti/ti_cpuid.h>
#include <arm/ti/ti_gpio.h>
#include <arm/ti/ti_scm.h>
#include <arm/ti/ti_prcm.h>

#include <dev/fdt/fdt_common.h>
#include <dev/gpio/gpiobusvar.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include "gpio_if.h"
#include "ti_gpio_if.h"

#if !defined(SOC_OMAP4) && !defined(SOC_TI_AM335X)
#error "Unknown SoC"
#endif

/* Register definitions */
#define	TI_GPIO_REVISION		0x0000
#define	TI_GPIO_SYSCONFIG		0x0010
#define	TI_GPIO_IRQSTATUS_RAW_0		0x0024
#define	TI_GPIO_IRQSTATUS_RAW_1		0x0028
#define	TI_GPIO_IRQSTATUS_0		0x002C
#define	TI_GPIO_IRQSTATUS_1		0x0030
#define	TI_GPIO_IRQSTATUS_SET_0		0x0034
#define	TI_GPIO_IRQSTATUS_SET_1		0x0038
#define	TI_GPIO_IRQSTATUS_CLR_0		0x003C
#define	TI_GPIO_IRQSTATUS_CLR_1		0x0040
#define	TI_GPIO_IRQWAKEN_0		0x0044
#define	TI_GPIO_IRQWAKEN_1		0x0048
#define	TI_GPIO_SYSSTATUS		0x0114
#define	TI_GPIO_IRQSTATUS1		0x0118
#define	TI_GPIO_IRQENABLE1		0x011C
#define	TI_GPIO_WAKEUPENABLE		0x0120
#define	TI_GPIO_IRQSTATUS2		0x0128
#define	TI_GPIO_IRQENABLE2		0x012C
#define	TI_GPIO_CTRL			0x0130
#define	TI_GPIO_OE			0x0134
#define	TI_GPIO_DATAIN			0x0138
#define	TI_GPIO_DATAOUT			0x013C
#define	TI_GPIO_LEVELDETECT0		0x0140
#define	TI_GPIO_LEVELDETECT1		0x0144
#define	TI_GPIO_RISINGDETECT		0x0148
#define	TI_GPIO_FALLINGDETECT		0x014C
#define	TI_GPIO_DEBOUNCENABLE		0x0150
#define	TI_GPIO_DEBOUNCINGTIME		0x0154
#define	TI_GPIO_CLEARWKUPENA		0x0180
#define	TI_GPIO_SETWKUENA		0x0184
#define	TI_GPIO_CLEARDATAOUT		0x0190
#define	TI_GPIO_SETDATAOUT		0x0194

/* Other SoC Specific definitions */
#define	OMAP4_MAX_GPIO_BANKS		6
#define	OMAP4_FIRST_GPIO_BANK		1
#define	OMAP4_INTR_PER_BANK		1
#define	OMAP4_GPIO_REV			0x50600801
#define	AM335X_MAX_GPIO_BANKS		4
#define	AM335X_FIRST_GPIO_BANK		0
#define	AM335X_INTR_PER_BANK		2
#define	AM335X_GPIO_REV			0x50600801
#define	PINS_PER_BANK			32
#define	TI_GPIO_BANK(p)			((p) / PINS_PER_BANK)
#define	TI_GPIO_MASK(p)			(1U << ((p) % PINS_PER_BANK))

static struct ti_gpio_softc *ti_gpio_sc = NULL;
static int ti_gpio_detach(device_t);

static u_int
ti_max_gpio_banks(void)
{
	switch(ti_chip()) {
#ifdef SOC_OMAP4
	case CHIP_OMAP_4:
		return (OMAP4_MAX_GPIO_BANKS);
#endif
#ifdef SOC_TI_AM335X
	case CHIP_AM335X:
		return (AM335X_MAX_GPIO_BANKS);
#endif
	}
	return (0);
}

static u_int
ti_max_gpio_intrs(void)
{
	switch(ti_chip()) {
#ifdef SOC_OMAP4
	case CHIP_OMAP_4:
		return (OMAP4_MAX_GPIO_BANKS * OMAP4_INTR_PER_BANK);
#endif
#ifdef SOC_TI_AM335X
	case CHIP_AM335X:
		return (AM335X_MAX_GPIO_BANKS * AM335X_INTR_PER_BANK);
#endif
	}
	return (0);
}

static u_int
ti_first_gpio_bank(void)
{
	switch(ti_chip()) {
#ifdef SOC_OMAP4
	case CHIP_OMAP_4:
		return (OMAP4_FIRST_GPIO_BANK);
#endif
#ifdef SOC_TI_AM335X
	case CHIP_AM335X:
		return (AM335X_FIRST_GPIO_BANK);
#endif
	}
	return (0);
}

static uint32_t
ti_gpio_rev(void)
{
	switch(ti_chip()) {
#ifdef SOC_OMAP4
	case CHIP_OMAP_4:
		return (OMAP4_GPIO_REV);
#endif
#ifdef SOC_TI_AM335X
	case CHIP_AM335X:
		return (AM335X_GPIO_REV);
#endif
	}
	return (0);
}

/**
 *	ti_gpio_mem_spec - Resource specification used when allocating resources
 *	ti_gpio_irq_spec - Resource specification used when allocating resources
 *
 *	This driver module can have up to six independent memory regions, each
 *	region typically controls 32 GPIO pins.
 *
 *	On OMAP3 and OMAP4 there is only one physical interrupt line per bank,
 *	but there are two set of registers which control the interrupt delivery
 *	to internal subsystems.  The first set of registers control the
 *	interrupts delivery to the MPU and the second set control the
 *	interrupts delivery to the DSP.
 *
 *	On AM335x there are two physical interrupt lines for each GPIO module.
 *	Each interrupt line is controlled by a set of registers.
 */
static struct resource_spec ti_gpio_mem_spec[] = {
	{ SYS_RES_MEMORY,   0,  RF_ACTIVE },
	{ SYS_RES_MEMORY,   1,  RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_MEMORY,   2,  RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_MEMORY,   3,  RF_ACTIVE | RF_OPTIONAL },
#if !defined(SOC_TI_AM335X)
	{ SYS_RES_MEMORY,   4,  RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_MEMORY,   5,  RF_ACTIVE | RF_OPTIONAL },
#endif
	{ -1,               0,  0 }
};
static struct resource_spec ti_gpio_irq_spec[] = {
	{ SYS_RES_IRQ,      0,  RF_ACTIVE },
	{ SYS_RES_IRQ,      1,  RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,      2,  RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,      3,  RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,      4,  RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,      5,  RF_ACTIVE | RF_OPTIONAL },
#if defined(SOC_TI_AM335X)
	{ SYS_RES_IRQ,      6,  RF_ACTIVE | RF_OPTIONAL },
	{ SYS_RES_IRQ,      7,  RF_ACTIVE | RF_OPTIONAL },
#endif
	{ -1,               0,  0 }
};

/**
 *	Macros for driver mutex locking
 */
#define	TI_GPIO_LOCK(_sc)		mtx_lock_spin(&(_sc)->sc_mtx)
#define	TI_GPIO_UNLOCK(_sc)		mtx_unlock_spin(&(_sc)->sc_mtx)
#define	TI_GPIO_LOCK_INIT(_sc)		\
	mtx_init(&_sc->sc_mtx, device_get_nameunit((_sc)->sc_dev), \
	    "ti_gpio", MTX_SPIN)
#define	TI_GPIO_LOCK_DESTROY(_sc)	mtx_destroy(&(_sc)->sc_mtx)
#define	TI_GPIO_ASSERT_LOCKED(_sc)	mtx_assert(&(_sc)->sc_mtx, MA_OWNED)
#define	TI_GPIO_ASSERT_UNLOCKED(_sc)	mtx_assert(&(_sc)->sc_mtx, MA_NOTOWNED)

/**
 *	ti_gpio_read_4 - reads a 32-bit value from one of the GPIO registers
 *	@sc: GPIO device context
 *	@bank: The bank to read from
 *	@off: The offset of a register from the GPIO register address range
 *
 *
 *	RETURNS:
 *	32-bit value read from the register.
 */
static inline uint32_t
ti_gpio_read_4(struct ti_gpio_softc *sc, unsigned int bank, bus_size_t off)
{
	return (bus_read_4(sc->sc_mem_res[bank], off));
}

/**
 *	ti_gpio_write_4 - writes a 32-bit value to one of the GPIO registers
 *	@sc: GPIO device context
 *	@bank: The bank to write to
 *	@off: The offset of a register from the GPIO register address range
 *	@val: The value to write into the register
 *
 *	RETURNS:
 *	nothing
 */
static inline void
ti_gpio_write_4(struct ti_gpio_softc *sc, unsigned int bank, bus_size_t off,
                 uint32_t val)
{
	bus_write_4(sc->sc_mem_res[bank], off, val);
}

static inline void
ti_gpio_intr_clr(struct ti_gpio_softc *sc, unsigned int bank, uint32_t mask)
{

	/* We clear both set of registers. */
	ti_gpio_write_4(sc, bank, TI_GPIO_IRQSTATUS_CLR_0, mask);
	ti_gpio_write_4(sc, bank, TI_GPIO_IRQSTATUS_CLR_1, mask);
}

static inline void
ti_gpio_intr_set(struct ti_gpio_softc *sc, unsigned int bank, uint32_t mask)
{

	/*
	 * On OMAP4 we unmask only the MPU interrupt and on AM335x we
	 * also activate only the first interrupt.
	 */
	ti_gpio_write_4(sc, bank, TI_GPIO_IRQSTATUS_SET_0, mask);
}

static inline void
ti_gpio_intr_ack(struct ti_gpio_softc *sc, unsigned int bank, uint32_t mask)
{

	/*
	 * Acknowledge the interrupt on both registers even if we use only
	 * the first one.
	 */
	ti_gpio_write_4(sc, bank, TI_GPIO_IRQSTATUS_0, mask);
	ti_gpio_write_4(sc, bank, TI_GPIO_IRQSTATUS_1, mask);
}

static inline uint32_t
ti_gpio_intr_status(struct ti_gpio_softc *sc, unsigned int bank)
{
	uint32_t reg;

	/* Get the status from both registers. */
	reg = ti_gpio_read_4(sc, bank, TI_GPIO_IRQSTATUS_0);
	reg |= ti_gpio_read_4(sc, bank, TI_GPIO_IRQSTATUS_1);

	return (reg);
}

static device_t
ti_gpio_get_bus(device_t dev)
{
	struct ti_gpio_softc *sc;

	sc = device_get_softc(dev);

	return (sc->sc_busdev);
}

/**
 *	ti_gpio_pin_max - Returns the maximum number of GPIO pins
 *	@dev: gpio device handle
 *	@maxpin: pointer to a value that upon return will contain the maximum number
 *	         of pins in the device.
 *
 *
 *	LOCKING:
 *	No locking required, returns static data.
 *
 *	RETURNS:
 *	Returns 0 on success otherwise an error code
 */
static int
ti_gpio_pin_max(device_t dev, int *maxpin)
{

	*maxpin = ti_max_gpio_banks() * PINS_PER_BANK - 1;

	return (0);
}

static int
ti_gpio_valid_pin(struct ti_gpio_softc *sc, int pin)
{

	if (pin >= sc->sc_maxpin ||
	    TI_GPIO_BANK(pin) >= ti_max_gpio_banks() ||
	    sc->sc_mem_res[TI_GPIO_BANK(pin)] == NULL) {
		return (EINVAL);
	}

	return (0);
}

/**
 *	ti_gpio_pin_getcaps - Gets the capabilties of a given pin
 *	@dev: gpio device handle
 *	@pin: the number of the pin
 *	@caps: pointer to a value that upon return will contain the capabilities
 *
 *	Currently all pins have the same capability, notably:
 *	  - GPIO_PIN_INPUT
 *	  - GPIO_PIN_OUTPUT
 *	  - GPIO_PIN_PULLUP
 *	  - GPIO_PIN_PULLDOWN
 *
 *	LOCKING:
 *	No locking required, returns static data.
 *
 *	RETURNS:
 *	Returns 0 on success otherwise an error code
 */
static int
ti_gpio_pin_getcaps(device_t dev, uint32_t pin, uint32_t *caps)
{
	struct ti_gpio_softc *sc;

	sc = device_get_softc(dev);
	if (ti_gpio_valid_pin(sc, pin) != 0)
		return (EINVAL);

	*caps = (GPIO_PIN_INPUT | GPIO_PIN_OUTPUT | GPIO_PIN_PULLUP |
	    GPIO_PIN_PULLDOWN);

	return (0);
}

/**
 *	ti_gpio_pin_getflags - Gets the current flags of a given pin
 *	@dev: gpio device handle
 *	@pin: the number of the pin
 *	@flags: upon return will contain the current flags of the pin
 *
 *	Reads the current flags of a given pin, here we actually read the H/W
 *	registers to determine the flags, rather than storing the value in the
 *	setflags call.
 *
 *	LOCKING:
 *	Internally locks the context
 *
 *	RETURNS:
 *	Returns 0 on success otherwise an error code
 */
static int
ti_gpio_pin_getflags(device_t dev, uint32_t pin, uint32_t *flags)
{
	struct ti_gpio_softc *sc;

	sc = device_get_softc(dev);
	if (ti_gpio_valid_pin(sc, pin) != 0)
		return (EINVAL);

	/* Get the current pin state */
	TI_GPIO_LOCK(sc);
	TI_GPIO_GET_FLAGS(dev, pin, flags);
	TI_GPIO_UNLOCK(sc);

	return (0);
}

/**
 *	ti_gpio_pin_getname - Gets the name of a given pin
 *	@dev: gpio device handle
 *	@pin: the number of the pin
 *	@name: buffer to put the name in
 *
 *	The driver simply calls the pins gpio_n, where 'n' is obviously the number
 *	of the pin.
 *
 *	LOCKING:
 *	No locking required, returns static data.
 *
 *	RETURNS:
 *	Returns 0 on success otherwise an error code
 */
static int
ti_gpio_pin_getname(device_t dev, uint32_t pin, char *name)
{
	struct ti_gpio_softc *sc;

	sc = device_get_softc(dev);
	if (ti_gpio_valid_pin(sc, pin) != 0)
		return (EINVAL);

	/* Set a very simple name */
	snprintf(name, GPIOMAXNAME, "gpio_%u", pin);
	name[GPIOMAXNAME - 1] = '\0';

	return (0);
}

/**
 *	ti_gpio_pin_setflags - Sets the flags for a given pin
 *	@dev: gpio device handle
 *	@pin: the number of the pin
 *	@flags: the flags to set
 *
 *	The flags of the pin correspond to things like input/output mode, pull-ups,
 *	pull-downs, etc.  This driver doesn't support all flags, only the following:
 *	  - GPIO_PIN_INPUT
 *	  - GPIO_PIN_OUTPUT
 *	  - GPIO_PIN_PULLUP
 *	  - GPIO_PIN_PULLDOWN
 *
 *	LOCKING:
 *	Internally locks the context
 *
 *	RETURNS:
 *	Returns 0 on success otherwise an error code
 */
static int
ti_gpio_pin_setflags(device_t dev, uint32_t pin, uint32_t flags)
{
	struct ti_gpio_softc *sc;
	uint32_t oe;

	sc = device_get_softc(dev);
	if (ti_gpio_valid_pin(sc, pin) != 0)
		return (EINVAL);

	/* Set the GPIO mode and state */
	TI_GPIO_LOCK(sc);
	if (TI_GPIO_SET_FLAGS(dev, pin, flags) != 0) {
		TI_GPIO_UNLOCK(sc);
		return (EINVAL);
	}

	/* If configuring as an output set the "output enable" bit */
	oe = ti_gpio_read_4(sc, TI_GPIO_BANK(pin), TI_GPIO_OE);
	if (flags & GPIO_PIN_INPUT)
		oe |= TI_GPIO_MASK(pin);
	else
		oe &= ~TI_GPIO_MASK(pin);
	ti_gpio_write_4(sc, TI_GPIO_BANK(pin), TI_GPIO_OE, oe);
	TI_GPIO_UNLOCK(sc);
	
	return (0);
}

/**
 *	ti_gpio_pin_set - Sets the current level on a GPIO pin
 *	@dev: gpio device handle
 *	@pin: the number of the pin
 *	@value: non-zero value will drive the pin high, otherwise the pin is
 *	        driven low.
 *
 *
 *	LOCKING:
 *	Internally locks the context
 *
 *	RETURNS:
 *	Returns 0 on success otherwise a error code
 */
static int
ti_gpio_pin_set(device_t dev, uint32_t pin, unsigned int value)
{
	struct ti_gpio_softc *sc;
	uint32_t reg;

	sc = device_get_softc(dev);
	if (ti_gpio_valid_pin(sc, pin) != 0)
		return (EINVAL);

	TI_GPIO_LOCK(sc);
	if (value == GPIO_PIN_LOW)
		reg = TI_GPIO_CLEARDATAOUT;
	else
		reg = TI_GPIO_SETDATAOUT;
	ti_gpio_write_4(sc, TI_GPIO_BANK(pin), reg, TI_GPIO_MASK(pin));
	TI_GPIO_UNLOCK(sc);

	return (0);
}

/**
 *	ti_gpio_pin_get - Gets the current level on a GPIO pin
 *	@dev: gpio device handle
 *	@pin: the number of the pin
 *	@value: pointer to a value that upond return will contain the pin value
 *
 *	The pin must be configured as an input pin beforehand, otherwise this
 *	function will fail.
 *
 *	LOCKING:
 *	Internally locks the context
 *
 *	RETURNS:
 *	Returns 0 on success otherwise a error code
 */
static int
ti_gpio_pin_get(device_t dev, uint32_t pin, unsigned int *value)
{
	struct ti_gpio_softc *sc;
	uint32_t oe, reg, val;

	sc = device_get_softc(dev);
	if (ti_gpio_valid_pin(sc, pin) != 0)
		return (EINVAL);

	/*
	 * Return data from output latch when set as output and from the 
	 * input register otherwise.
	 */
	TI_GPIO_LOCK(sc);
	oe = ti_gpio_read_4(sc, TI_GPIO_BANK(pin), TI_GPIO_OE);
	if (oe & TI_GPIO_MASK(pin))
		reg = TI_GPIO_DATAIN;
	else
		reg = TI_GPIO_DATAOUT;
	val = ti_gpio_read_4(sc, TI_GPIO_BANK(pin), reg);
	*value = (val & TI_GPIO_MASK(pin)) ? 1 : 0;
	TI_GPIO_UNLOCK(sc);

	return (0);
}

/**
 *	ti_gpio_pin_toggle - Toggles a given GPIO pin
 *	@dev: gpio device handle
 *	@pin: the number of the pin
 *
 *
 *	LOCKING:
 *	Internally locks the context
 *
 *	RETURNS:
 *	Returns 0 on success otherwise a error code
 */
static int
ti_gpio_pin_toggle(device_t dev, uint32_t pin)
{
	struct ti_gpio_softc *sc;
	uint32_t reg, val;

	sc = device_get_softc(dev);
	if (ti_gpio_valid_pin(sc, pin) != 0)
		return (EINVAL);

	/* Toggle the pin */
	TI_GPIO_LOCK(sc);
	val = ti_gpio_read_4(sc, TI_GPIO_BANK(pin), TI_GPIO_DATAOUT);
	if (val & TI_GPIO_MASK(pin))
		reg = TI_GPIO_CLEARDATAOUT;
	else
		reg = TI_GPIO_SETDATAOUT;
	ti_gpio_write_4(sc, TI_GPIO_BANK(pin), reg, TI_GPIO_MASK(pin));
	TI_GPIO_UNLOCK(sc);

	return (0);
}

/**
 *	ti_gpio_intr - ISR for all GPIO modules
 *	@arg: the soft context pointer
 *
 *	LOCKING:
 *	Internally locks the context
 *
 */
static int
ti_gpio_intr(void *arg)
{
	int bank_last, irq;
	struct intr_event *event;
	struct ti_gpio_softc *sc;
	uint32_t reg;

	sc = (struct ti_gpio_softc *)arg;
	bank_last = -1;
	reg = 0; /* squelch bogus gcc warning */
	for (irq = 0; irq < sc->sc_maxpin; irq++) {

		/* Read interrupt status only once for each bank. */
		if (TI_GPIO_BANK(irq) != bank_last) {
			reg = ti_gpio_intr_status(sc, TI_GPIO_BANK(irq));
			bank_last = TI_GPIO_BANK(irq);
		}
		if ((reg & TI_GPIO_MASK(irq)) == 0)
			continue;
		event = sc->sc_events[irq];
		if (event != NULL && !TAILQ_EMPTY(&event->ie_handlers))
			intr_event_handle(event, NULL);
		else
			device_printf(sc->sc_dev, "Stray IRQ %d\n", irq);
		/* Ack the IRQ Status bit. */
		ti_gpio_intr_ack(sc, TI_GPIO_BANK(irq), TI_GPIO_MASK(irq));
	}

	return (FILTER_HANDLED);
}

static int
ti_gpio_attach_intr(device_t dev)
{
	int i;
	struct ti_gpio_softc *sc;

	sc = device_get_softc(dev);
	for (i = 0; i < ti_max_gpio_intrs(); i++) {
		if (sc->sc_irq_res[i] == NULL)
			break;

		/*
		 * Register our interrupt filter for each of the IRQ resources.
		 */
		if (bus_setup_intr(dev, sc->sc_irq_res[i],
		    INTR_TYPE_MISC | INTR_MPSAFE, ti_gpio_intr, NULL, sc,
		    &sc->sc_irq_hdl[i]) != 0) {
			device_printf(dev,
			    "WARNING: unable to register interrupt filter\n");
			return (-1);
		}
	}

	return (0);
}

static int
ti_gpio_detach_intr(device_t dev)
{
	int i;
	struct ti_gpio_softc *sc;

	/* Teardown our interrupt filters. */
	sc = device_get_softc(dev);
	for (i = 0; i < ti_max_gpio_intrs(); i++) {
		if (sc->sc_irq_res[i] == NULL)
			break;

		if (sc->sc_irq_hdl[i]) {
			bus_teardown_intr(dev, sc->sc_irq_res[i],
			    sc->sc_irq_hdl[i]);
		}
	}

	return (0);
}

static int
ti_gpio_bank_init(device_t dev, int bank)
{
	int pin;
	struct ti_gpio_softc *sc;
	uint32_t flags, reg_oe, rev;

	sc = device_get_softc(dev);

	/* Enable the interface and functional clocks for the module. */
	ti_prcm_clk_enable(GPIO0_CLK + ti_first_gpio_bank() + bank);

	/*
	 * Read the revision number of the module.  TI don't publish the
	 * actual revision numbers, so instead the values have been
	 * determined by experimentation.
	 */
	rev = ti_gpio_read_4(sc, bank, TI_GPIO_REVISION);

	/* Check the revision. */
	if (rev != ti_gpio_rev()) {
		device_printf(dev, "Warning: could not determine the revision "
		    "of GPIO module %d (revision:0x%08x)\n", bank, rev);
		return (EINVAL);
	}

	/* Disable interrupts for all pins. */
	ti_gpio_intr_clr(sc, bank, 0xffffffff);

	/* Init OE register based on pads configuration. */
	reg_oe = 0xffffffff;
	for (pin = 0; pin < PINS_PER_BANK; pin++) {
		TI_GPIO_GET_FLAGS(dev, PINS_PER_BANK * bank + pin, &flags);
		if (flags & GPIO_PIN_OUTPUT)
			reg_oe &= ~(1UL << pin);
	}
	ti_gpio_write_4(sc, bank, TI_GPIO_OE, reg_oe);

	return (0);
}

/**
 *	ti_gpio_attach - attach function for the driver
 *	@dev: gpio device handle
 *
 *	Allocates and sets up the driver context for all GPIO banks.  This function
 *	expects the memory ranges and IRQs to already be allocated to the driver.
 *
 *	LOCKING:
 *	None
 *
 *	RETURNS:
 *	Always returns 0
 */
static int
ti_gpio_attach(device_t dev)
{
	struct ti_gpio_softc *sc;
	unsigned int i;
	int err;

	if (ti_gpio_sc != NULL)
		return (ENXIO);

	ti_gpio_sc = sc = device_get_softc(dev);
	sc->sc_dev = dev;
	TI_GPIO_LOCK_INIT(sc);
	ti_gpio_pin_max(dev, &sc->sc_maxpin);
	sc->sc_maxpin++;

	/* There are up to 6 different GPIO register sets located in different
	 * memory areas on the chip.  The memory range should have been set for
	 * the driver when it was added as a child.
	 */
	if (bus_alloc_resources(dev, ti_gpio_mem_spec, sc->sc_mem_res) != 0) {
		device_printf(dev, "Error: could not allocate mem resources\n");
		ti_gpio_detach(dev);
		return (ENXIO);
	}

	/* Request the IRQ resources */
	if (bus_alloc_resources(dev, ti_gpio_irq_spec, sc->sc_irq_res) != 0) {
		device_printf(dev, "Error: could not allocate irq resources\n");
		ti_gpio_detach(dev);
		return (ENXIO);
	}

	/* Setup the IRQ resources */
	if (ti_gpio_attach_intr(dev) != 0) {
		device_printf(dev, "Error: could not setup irq handlers\n");
		ti_gpio_detach(dev);
		return (ENXIO);
	}

	/*
	 * Initialize the interrupt settings.  The default is active-low
	 * interrupts.
	 */
	sc->sc_irq_trigger = malloc(
	    sizeof(*sc->sc_irq_trigger) * sc->sc_maxpin,
	    M_DEVBUF, M_WAITOK | M_ZERO);
	sc->sc_irq_polarity = malloc(
	    sizeof(*sc->sc_irq_polarity) * sc->sc_maxpin,
	    M_DEVBUF, M_WAITOK | M_ZERO);
	for (i = 0; i < sc->sc_maxpin; i++) {
		sc->sc_irq_trigger[i] = INTR_TRIGGER_LEVEL;
		sc->sc_irq_polarity[i] = INTR_POLARITY_LOW;
	}

	sc->sc_events = malloc(sizeof(struct intr_event *) * sc->sc_maxpin,
	    M_DEVBUF, M_WAITOK | M_ZERO);

	/* We need to go through each block and ensure the clocks are running and
	 * the module is enabled.  It might be better to do this only when the
	 * pins are configured which would result in less power used if the GPIO
	 * pins weren't used ... 
	 */
	for (i = 0; i < ti_max_gpio_banks(); i++) {
		if (sc->sc_mem_res[i] != NULL) {
			/* Initialize the GPIO module. */
			err = ti_gpio_bank_init(dev, i);
			if (err != 0) {
				ti_gpio_detach(dev);
				return (err);
			}
		}
	}
	sc->sc_busdev = gpiobus_attach_bus(dev);
	if (sc->sc_busdev == NULL) {
		ti_gpio_detach(dev);
		return (ENXIO);
	}

	return (0);
}

/**
 *	ti_gpio_detach - detach function for the driver
 *	@dev: scm device handle
 *
 *	Allocates and sets up the driver context, this simply entails creating a
 *	bus mappings for the SCM register set.
 *
 *	LOCKING:
 *	None
 *
 *	RETURNS:
 *	Always returns 0
 */
static int
ti_gpio_detach(device_t dev)
{
	struct ti_gpio_softc *sc = device_get_softc(dev);
	unsigned int i;

	KASSERT(mtx_initialized(&sc->sc_mtx), ("gpio mutex not initialized"));

	/* Disable all interrupts */
	for (i = 0; i < ti_max_gpio_banks(); i++) {
		if (sc->sc_mem_res[i] != NULL)
			ti_gpio_intr_clr(sc, i, 0xffffffff);
	}
	gpiobus_detach_bus(dev);
	if (sc->sc_events)
		free(sc->sc_events, M_DEVBUF);
	if (sc->sc_irq_polarity)
		free(sc->sc_irq_polarity, M_DEVBUF);
	if (sc->sc_irq_trigger)
		free(sc->sc_irq_trigger, M_DEVBUF);
	/* Release the memory and IRQ resources. */
	ti_gpio_detach_intr(dev);
	bus_release_resources(dev, ti_gpio_irq_spec, sc->sc_irq_res);
	bus_release_resources(dev, ti_gpio_mem_spec, sc->sc_mem_res);
	TI_GPIO_LOCK_DESTROY(sc);

	return (0);
}

static uint32_t
ti_gpio_intr_reg(struct ti_gpio_softc *sc, int irq)
{

	if (ti_gpio_valid_pin(sc, irq) != 0)
		return (0);

	if (sc->sc_irq_trigger[irq] == INTR_TRIGGER_LEVEL) {
		if (sc->sc_irq_polarity[irq] == INTR_POLARITY_LOW)
			return (TI_GPIO_LEVELDETECT0);
		else if (sc->sc_irq_polarity[irq] == INTR_POLARITY_HIGH)
			return (TI_GPIO_LEVELDETECT1);
	} else if (sc->sc_irq_trigger[irq] == INTR_TRIGGER_EDGE) {
		if (sc->sc_irq_polarity[irq] == INTR_POLARITY_LOW)
			return (TI_GPIO_FALLINGDETECT);
		else if (sc->sc_irq_polarity[irq] == INTR_POLARITY_HIGH)
			return (TI_GPIO_RISINGDETECT);
	}

	return (0);
}

static void
ti_gpio_mask_irq(void *source)
{
	int irq;
	uint32_t reg, val;

	irq = (int)source;
	if (ti_gpio_valid_pin(ti_gpio_sc, irq) != 0)
		return;

	TI_GPIO_LOCK(ti_gpio_sc);
	ti_gpio_intr_clr(ti_gpio_sc, TI_GPIO_BANK(irq), TI_GPIO_MASK(irq));
	reg = ti_gpio_intr_reg(ti_gpio_sc, irq);
	if (reg != 0) {
		val = ti_gpio_read_4(ti_gpio_sc, TI_GPIO_BANK(irq), reg);
		val &= ~TI_GPIO_MASK(irq);
		ti_gpio_write_4(ti_gpio_sc, TI_GPIO_BANK(irq), reg, val);
	}
	TI_GPIO_UNLOCK(ti_gpio_sc);
}

static void
ti_gpio_unmask_irq(void *source)
{
	int irq;
	uint32_t reg, val;

	irq = (int)source;
	if (ti_gpio_valid_pin(ti_gpio_sc, irq) != 0)
		return;

	TI_GPIO_LOCK(ti_gpio_sc);
	reg = ti_gpio_intr_reg(ti_gpio_sc, irq);
	if (reg != 0) {
		val = ti_gpio_read_4(ti_gpio_sc, TI_GPIO_BANK(irq), reg);
		val |= TI_GPIO_MASK(irq);
		ti_gpio_write_4(ti_gpio_sc, TI_GPIO_BANK(irq), reg, val);
		ti_gpio_intr_set(ti_gpio_sc, TI_GPIO_BANK(irq),
		    TI_GPIO_MASK(irq));
	}
	TI_GPIO_UNLOCK(ti_gpio_sc);
}

static int
ti_gpio_activate_resource(device_t dev, device_t child, int type, int rid,
	struct resource *res)
{
	int pin;

	if (type != SYS_RES_IRQ)
		return (ENXIO);

	/* Unmask the interrupt. */
	pin = rman_get_start(res);
	ti_gpio_unmask_irq((void *)(uintptr_t)pin);

	return (0);
}

static int
ti_gpio_deactivate_resource(device_t dev, device_t child, int type, int rid,
	struct resource *res)
{
	int pin;

	if (type != SYS_RES_IRQ)
		return (ENXIO);

	/* Mask the interrupt. */
	pin = rman_get_start(res);
	ti_gpio_mask_irq((void *)(uintptr_t)pin);

	return (0);
}

static int
ti_gpio_config_intr(device_t dev, int irq, enum intr_trigger trig,
	enum intr_polarity pol)
{
	struct ti_gpio_softc *sc;
	uint32_t oldreg, reg, val;

	sc = device_get_softc(dev);
	if (ti_gpio_valid_pin(sc, irq) != 0)
		return (EINVAL);

	/* There is no standard trigger or polarity. */
	if (trig == INTR_TRIGGER_CONFORM || pol == INTR_POLARITY_CONFORM)
		return (EINVAL);

	TI_GPIO_LOCK(sc);
	/*
	 * TRM recommends add the new event before remove the old one to
	 * avoid losing interrupts.
	 */
	oldreg = ti_gpio_intr_reg(sc, irq);
	sc->sc_irq_trigger[irq] = trig;
	sc->sc_irq_polarity[irq] = pol;
	reg = ti_gpio_intr_reg(sc, irq);
	if (reg != 0) {
		/* Apply the new settings. */
		val = ti_gpio_read_4(sc, TI_GPIO_BANK(irq), reg);
		val |= TI_GPIO_MASK(irq);
		ti_gpio_write_4(sc, TI_GPIO_BANK(irq), reg, val);
	}
	if (reg != oldreg && oldreg != 0) {
		/* Remove the old settings. */
		val = ti_gpio_read_4(sc, TI_GPIO_BANK(irq), oldreg);
		val &= ~TI_GPIO_MASK(irq);
		ti_gpio_write_4(sc, TI_GPIO_BANK(irq), oldreg, val);
	}
	TI_GPIO_UNLOCK(sc);

	return (0);
}

static int
ti_gpio_setup_intr(device_t dev, device_t child, struct resource *ires,
	int flags, driver_filter_t *filt, driver_intr_t *handler,
	void *arg, void **cookiep)
{
	struct ti_gpio_softc *sc;
	struct intr_event *event;
	int pin, error;

	sc = device_get_softc(dev);
	pin = rman_get_start(ires);
	if (ti_gpio_valid_pin(sc, pin) != 0)
		panic("%s: bad pin %d", __func__, pin);

	event = sc->sc_events[pin];
	if (event == NULL) {
		error = intr_event_create(&event, (void *)(uintptr_t)pin, 0,
		    pin, ti_gpio_mask_irq, ti_gpio_unmask_irq, NULL, NULL,
		    "gpio%d pin%d:", device_get_unit(dev), pin);
		if (error != 0)
			return (error);
		sc->sc_events[pin] = event;
	}
	intr_event_add_handler(event, device_get_nameunit(child), filt,
	    handler, arg, intr_priority(flags), flags, cookiep);

	return (0);
}

static int
ti_gpio_teardown_intr(device_t dev, device_t child, struct resource *ires,
	void *cookie)
{
	struct ti_gpio_softc *sc;
	int pin, err;

	sc = device_get_softc(dev);
	pin = rman_get_start(ires);
	if (ti_gpio_valid_pin(sc, pin) != 0)
		panic("%s: bad pin %d", __func__, pin);
	if (sc->sc_events[pin] == NULL)
		panic("Trying to teardown unoccupied IRQ");
	err = intr_event_remove_handler(cookie);
	if (!err)
		sc->sc_events[pin] = NULL;

	return (err);
}

static phandle_t
ti_gpio_get_node(device_t bus, device_t dev)
{

	/* We only have one child, the GPIO bus, which needs our own node. */
	return (ofw_bus_get_node(bus));
}

static device_method_t ti_gpio_methods[] = {
	DEVMETHOD(device_attach, ti_gpio_attach),
	DEVMETHOD(device_detach, ti_gpio_detach),

	/* GPIO protocol */
	DEVMETHOD(gpio_get_bus, ti_gpio_get_bus),
	DEVMETHOD(gpio_pin_max, ti_gpio_pin_max),
	DEVMETHOD(gpio_pin_getname, ti_gpio_pin_getname),
	DEVMETHOD(gpio_pin_getflags, ti_gpio_pin_getflags),
	DEVMETHOD(gpio_pin_getcaps, ti_gpio_pin_getcaps),
	DEVMETHOD(gpio_pin_setflags, ti_gpio_pin_setflags),
	DEVMETHOD(gpio_pin_get, ti_gpio_pin_get),
	DEVMETHOD(gpio_pin_set, ti_gpio_pin_set),
	DEVMETHOD(gpio_pin_toggle, ti_gpio_pin_toggle),

	/* Bus interface */
	DEVMETHOD(bus_activate_resource, ti_gpio_activate_resource),
	DEVMETHOD(bus_deactivate_resource, ti_gpio_deactivate_resource),
	DEVMETHOD(bus_config_intr, ti_gpio_config_intr),
	DEVMETHOD(bus_setup_intr, ti_gpio_setup_intr),
	DEVMETHOD(bus_teardown_intr, ti_gpio_teardown_intr),

	/* ofw_bus interface */
	DEVMETHOD(ofw_bus_get_node, ti_gpio_get_node),

	{0, 0},
};

driver_t ti_gpio_driver = {
	"gpio",
	ti_gpio_methods,
	sizeof(struct ti_gpio_softc),
};
