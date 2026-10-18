/* SalsaDS — ROM snd: direcciones compartidas (datos de origen y resultados). */
#ifndef SND_H
#define SND_H

#define SND_OUT 0x02300000u   /* capturas: un caso cada 0x400 bytes */
#define SND_D16C 0x02310000u  /* PCM16 constante 0x4000 */
#define SND_D8C 0x02310400u   /* PCM8 constante 0x40 */
#define SND_D16R 0x02310800u  /* PCM16 rampa */
#define SND_D8R 0x02311000u   /* PCM8 rampa */
#define SND_D16M 0x02311400u  /* PCM16 constante 0x7FFF */
#define SND_D16N 0x02311800u  /* PCM16 constante -0x4000 */
#define SND_D16S 0x02311C00u  /* PCM16 constante 0x1235 */
#define SND_D16I 0x02312400u  /* PCM16: 8 * índice */
#define SND_SCRATCH 0x02313000u /* capturas de la pasada de calentamiento (512 bytes) */
#define SND_ZERO 0x02312800u  /* PCM16 de silencio */
#define SND_ADPCM2 0x02313400u /* IMA-ADPCM con valor inicial distinto de cero */
#define SND_ADPCM 0x02312000u /* IMA-ADPCM de prueba */

#endif
