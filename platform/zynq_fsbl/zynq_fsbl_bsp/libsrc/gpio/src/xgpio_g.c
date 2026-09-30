#include "xgpio.h"

XGpio_Config XGpio_ConfigTable[] __attribute__ ((section (".drvcfg_sec"))) = {

	{
		"xlnx,axi-gpio-2.0", /* compatible */
		0x41200000, /* reg */
		0x1, /* xlnx,interrupt-present */
		0x1, /* xlnx,is-dual */
		0x2002, /* interrupts */
		0x41800001, /* interrupt-parent */
		0x4 /* xlnx,gpio-width */
	},
	{
		"xlnx,axi-gpio-2.0", /* compatible */
		0x41210000, /* reg */
		0x1, /* xlnx,interrupt-present */
		0x1, /* xlnx,is-dual */
		0x2003, /* interrupts */
		0x41800001, /* interrupt-parent */
		0x2 /* xlnx,gpio-width */
	},
	 {
		 NULL
	}
};