#include "xtmrctr.h"

XTmrCtr_Config XTmrCtr_ConfigTable[] __attribute__ ((section (".drvcfg_sec"))) = {

	{
		"xlnx,axi-timer-2.0", /* compatible */
		0x42800000, /* reg */
		0x5f5e100, /* clock-frequency */
		0x2000, /* interrupts */
		0x41800001 /* interrupt-parent */
	},
	 {
		 NULL
	}
};