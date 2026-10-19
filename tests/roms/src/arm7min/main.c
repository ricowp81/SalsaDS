/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ARM7 mínimo de las ROMs de prueba: no hace nada. Evita enlazar
 * el núcleo ARM7 por omisión de BlocksDS (que trae DSWiFi, Maxmod y Mbed TLS,
 * con sus avisos de licencia) en ROMs que solo ejercitan el ARM9.
 */
int main(void)
{
	for (;;)
		;
}
