/*
 * Caché de código del recompilador: cola NOBITS al final del segmento de
 * texto de lv2 (hdr_text de lv2.ld). Es el único lugar escribible y
 * ejecutable con HEN (docs/cell-desde-salsa64.md §2). Al ir detrás de todo lo
 * que ocupa archivo, el segmento queda con memsz > filesz: el cargador la
 * rellena con ceros sin que ocupe espacio en el EBOOT. Se agrega a lv2.ld con
 * INSERT: pasar `-T rec-cache.ld -T lv2.ld`, en ese orden (con -T, gcc ya no
 * agrega lv2.ld solo). La sección la define tests/phase0/jit-arm/rec_cache.c.
 */
SECTIONS
{
	/* Alineada a 1 MB por si lv2 da páginas grandes al segmento; el ajuste va
	 * dentro de la sección (ALIGN(1M) en la sección mueve el inicio del
	 * segmento de texto a 0). */
	.salsads_rec_cache (NOLOAD) : ALIGN(64K) { . = ALIGN(1M); KEEP (*(.salsads_rec_cache)) } : hdr_text
}
INSERT AFTER .sys_proc_prx_param;
