/*
 * netConfig.c --
 *
 *	Machine-type dependent initialization of the network interfaces.
 *
 * Copyright (C) 1987 Regents of the University of California
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 *
 *
 * $Header: /sprite/src/kernel/net/sun4.md/RCS/netConfig.c,v 9.1 90/10/19 15:47:55 jhh Exp $ SPRITE (Berkeley)
 */

#include <sprite.h>
#include <net.h>
#include <netInt.h>
#include <netIEInt.h>
#include <netUltraInt.h>

Net_Interface netConfigInterfaces[] = {
    {"IE", 0, (Address) NET_IE_CONTROL_REG_ADDR, TRUE, 6, NetIEInit},
    {"ULTRA", 0, (Address) NET_ULTRA_CONTROL_REG_ADDR, FALSE, 220, NetUltraInit}
};
int netNumConfigInterfaces = 
	    sizeof(netConfigInterfaces) / sizeof(Net_Interface);

