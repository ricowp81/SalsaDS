/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — ROM ipc: órdenes del ARM9 al ARM7 por el buzón. */
#ifndef IPC_H
#define IPC_H

#define C_SYNC_SET 1   /* arg0: dato que el ARM7 pone en sus bits 8-11 */
#define C_SYNC_GET 2   /* res0: el IPCSYNC del ARM7 */
#define C_SYNC_PULSE 3 /* arg0: dato; el ARM7 manda una IRQ de sincronía */
#define C_SYNC_ARM 4   /* el ARM7 habilita la IRQ de sincronía y cuenta */
#define C_SYNC_COUNT 5 /* res0: cuántas IRQ de sincronía recibió, res1: su IPCSYNC */
#define C_FIFO_CNT 6   /* arg0: valor para IPCFIFOCNT del ARM7 */
#define C_FIFO_RECV 7  /* arg0: cuántas lecturas hace; res0 válidas, res1 la última, res2 IPCFIFOCNT, res3 orden */
#define C_FIFO_SEND 8  /* arg0: cuántas palabras, arg1: la primera (las demás suman 1) */

#endif
