# Registro de rendimiento

## 2026-10-29 — Sombras con cara trasera en W y niebla con toon o sombra en el RSX [host: planes y ensamblado; sin tiempos]

- Log previo de SM64DS (PS3, 2×): 290–302 s `sombras: cara trasera W sin paridad` 120/120 en siete ventanas y 342–372 s `niebla combinada con toon o sombra` 120/120 en quince; el costo 3D del RSX en el castillo cuando entraba era 4,0–5,9 ms. Son respaldos de paridad, no de costo.
- Banco del host `shadows_sweep`: 524 288 escenas de hasta cinco polígonos (una sombra 61 440, varias 135 168, ampliado 327 680) con y sin atributos de niebla, 0 diferencias contra `r3d_px.h`; los 3 717 120 estados de color y 5000 lotes siguen en 0. `rsx3d-wdepth-check.py`: 8 programas W, 848 casos iguales. `rsx3d-fog-check.py`: 15 856 casos.
- Sin medición en consola: dibujos por polígono de sombra iguales a la ronda anterior (misma secuencia) más la pasada de atributos cuando hay niebla; medir en SM64DS castillo y Bob-omb Battlefield, MKDS y Metroid según `build/rsx-sombras/informe.md`.

## 2026-10-28 — Tramo del planificador del JIT: 1024 → 512 ciclos del ARM9
- PS3 real, Sonic Rush Adventure (intro y título, sin tocar), emulación por cuadro (mediana por ventana de 120): 1024 ≈ 21 ms con 4–20 cuadros del motor B preparados tarde y parpadeo; 512 ≈ 21,6–22,2 ms (+3 %), 0 tarde, sin parpadeo; 128 ≈ 23,6 ms (+12 %), 0 tarde.
- Metroid Prime Hunters con 512, 2 min de menús y juego: 9–13,5 ms por cuadro, presentación a 16,7 ms casi siempre; sin parpadeo. Sin A/B estricto contra 1024 en la misma escena.
- Interruptor: `nucleo-jit-tramo` con el número de ciclos (16–20000) en el directorio de datos.

## 2026-10-28 — NSMB 1-1: melonDS como referencia y espejo interpolado de SOUNDCAP (host)

Grabaciones digitales de melonDS 1.1 (`tools/melonds-grabar.py`: controlador
«disk» de SDL, 48 kHz PCM16 estéreo, 105 s desde el arranque, interpolación 0
y 3) y del host (`salsads-cpurun`, 5000 cuadros, mismo guardado y botones). El
nivel empieza a los 62 s en el host y a los 74 s en melonDS (sus pulsaciones
duran más); se recorta cada archivo un segundo antes y se alinea por
envolventes de seis bandas (correlación 0,83–0,95 en los primeros 8 s; la
correlación de onda es baja, 0,03, porque los relojes de salida no coinciden).
Tramo medido: 22 s. Energía por banda en dB respecto a la potencia total,
media L/R; picos de |Δ²| > 0,05 FS separados ≥ 3 muestras, todo a 48 kHz
(`build/sonido/analisis-3.py`, `espectro-3.py`).

| Grabación | RMS L/R dBFS | picos/s L/R | 4–8 kHz | 8–12 kHz | 12–16 kHz |
|---|---:|---:|---:|---:|---:|
| host nativo (32 728 → 48 k) | −16,0/−18,3 | 323/517 | −24,87 | −30,22 | −31,49 |
| host FIR (escucha sin HD) | −16,0/−18,3 | 276/387 | −24,88 | −30,22 | −32,98 |
| host HD anterior (`audio-hd-sin-captura`) | −16,0/−18,3 | 218/444 | −25,02 | −31,38 | −34,54 |
| host HD con espejo de captura | −18,4/−19,6 | 42/292 | −25,23 | −32,27 | −37,60 |
| melonDS, interpolación ninguna | −17,8/−20,5 | 48/282 | −25,34 | −32,42 | −34,78 |
| melonDS, interpolación cúbica | −18,1/−20,7 | 47/265 | −25,31 | −32,27 | −34,52 |

Por tercios de octava (dB respecto al total): melonDS ninguna/cúbica difieren
menos de 0,3 dB en todos; frente a nuestro nativo están entre −0,5 (6,35 kHz)
y −2,1 dB (12,7 kHz). La aspereza del pasaje no es un defecto de temporización
ni de formato: la referencia la reproduce igual y su interpolador no la toca,
porque toda la salida de NSMB pasa por SOUNDCAP (SOUNDCNT `0xB97F`).

Espejo interpolado (`snd_state.hdcap`): mismas muestras nativas con y sin él
(`cmp` de los WAV nativos idéntico), RAM y estado fieles idénticos. Banco del
seno por captura (`hd-captura-3`, mismo método que la sección siguiente): SINAD
19,72 → 63,22 dB; imagen de 7498,65 Hz −23,89 → −64,44 dBc; la voz directa
sigue en −64,04 dBc. El RMS baja 2,4/1,3 dB (L/R) en NSMB: es la respuesta de
Hermite frente al escalón en la octava alta de cada voz, la misma que ya tiene
la ruta HD directa. Costo en el host (5000 cuadros de NSMB, dos pasadas, orden
relativo): HD anterior 17,11/16,80 s, HD con espejo 17,11/16,90 s, dentro del
ruido; el costo real se mide en la PS3 con `DATO audio`. Estado
+16 KB en el SPE (holgura previa 165 600 B). WAV y métricas en
`build/sonido/melonds/`, fuera del repositorio.

## 2026-10-28 — Hermite antes y después de SOUNDCAP (host LE/Docker)

Banco propio sin ROM sobre `4181d5c`, mismo `snd_run` del núcleo nuevo. Seno
PCM16 de 32 muestras (dos períodos), amplitud 16000, TMR de la voz 0 con período
2095: Fs = 7998,5637 Hz, tono = 499,9102 Hz. Dos recorridos: voz 0 directa o
mezcla L → SOUNDCAP0 → anillo PCM16 de 2048 B → voz 1, período 512. Salida L
de la voz 1, excluida del mezclador de captura; sin realimentación ni recorte.
Cada caso genera 240 trabajos de 547 ticks. Ajuste senoidal desde el segundo 1
hasta el final; SINAD = potencia del tono / residuo completo, incluida la imagen
Fs−f, sin confundirlo con una estimación de ruido blanco.

| Recorrido | HD | SINAD (dB) | Imagen a 7498,65 Hz (dBc) |
|---|---:|---:|---:|
| Directo | no | 19,6504 | −23,4969 |
| Directo | sí | 62,7597 | −64,0407 |
| Por captura | no | 19,6511 | −23,4971 |
| Por captura | sí | 19,7168 | −23,8929 |

Las 131280 muestras nativas estéreo de cada recorrido son idénticas con HD
encendido y apagado. La mejora de rechazo de imagen es 40,54 dB directa y
0,40 dB por captura. Se confirma un límite de la mejora de escucha: las voces
de origen se mezclan sin interpolar para SOUNDCAP; interpolar después las voces
1/3 a la frecuencia nativa conserva sus imágenes dentro de banda. El pasaje
host de NSMB usa precisamente esta salida, SOUNDCNT `0xB97F`, voces 1/3 PCM16
con TMR `0xFE00`. Esto no determina cómo suena el hardware ni melonDS.

Artefactos locales: `build/sonido/hd-captura-2.c`, `medir-seno-2.py`,
`seno-metricas-2.json` y `seno-captura*-hd*-2.f32/.pcm`. Compilación y ejecución
con `tools/build.sh --sh`, GCC `-O2`, sin cambiar fuentes del núcleo. No se
midió tiempo del host para inferir costo del Cell.

### WAV aportados de PS3 y equivalencia de pasajes

Los dos WAV PS3 nuevos duran 20 s; nativo 32728 Hz/PCM16 y salida 48000 Hz/float32.
El host del nivel dura 20,032 s. Remuestreo de diagnóstico con
`scipy.signal.resample_poly` a una tasa común, correlación mono y búsqueda
de ±10 s: PS3 nativo frente al tramo host, 0,00568; las señales no quedan
alineadas por un desfase fijo. No es una comparación válida de ruido residual.

La búsqueda por envolventes de seis bandas encuentra PS3 2–10 s frente a host
completo 3,34–11,34 s: correlación de envolventes 0,99356. A tasa nativa,
refinamiento de −81 muestras (−2,475 ms), correlación mono 0,89514 y diferencias
espectrales de `tools/audio-stage-compare.py` entre −0,099 y +0,048 dB en sus
cinco bandas. Esto identifica un pasaje común con el comienzo del WAV completo;
no acredita una pareja de grabaciones del nivel entero. La correlación mono
tampoco acredita paridad estéreo: el residuo conjunto L/R, compensando una sola
ganancia, tiene relación señal/residuo de 1,58 dB.

Máximos locales de `abs(Δ²)` > 0,05 de escala completa, separados al menos tres
muestras, contados en la tasa original de cada archivo:

| WAV | Máximos/s L / R | Cruces del umbral/s L / R |
|---|---:|---:|
| Host nivel, nuevo | 679,0 / 644,0 | 1584,3 / 1595,6 |
| Host nivel, anterior | 705,0 / 656,6 | 1635,4 / 1637,6 |
| PS3 nuevo, nativo | 609,2 / 757,4 | 1141,4 / 1381,0 |
| PS3 nuevo, HD | 233,4 / 283,4 | 340,3 / 395,8 |

Los cruces consecutivos pueden pertenecer al mismo escalón; estos conteos no
son equivalentes entre sí ni miden solo ruido. En el host nuevo, mediana de
distancias 11 muestras en L/R y 43,7/44,3 % entre 6 y 20 muestras. Los WAV PS3
tienen 13,53 % de pares exactamente cero (nativo), frente a 1,32 % en el tramo
host; el decil inferior sin excluir silencio daría una medida engañosa. Entre
los bloques activos de unos 10 ms, el decil inferior da RMS L/R −29,58/−28,32
dBFS host y −31,80/−33,27 dBFS PS3 nativo. Incluyen colas musicales: no son
una medición aislada de energía de ruido entre notas.

El log PS3 aportado acredita autoprueba nueva de ocho trabajos: 0 diferencias
en muestras, estado, memoria y salida; `audio-verificar` apagado en juego.
Banco del arranque, 16 voces/120 cuadros: fiel media/máximo 1512/1515 µs;
HD 4129/4133 µs. El log anterior da 966/968 y 3597/3601 µs. Son medidas de
esos arranques de consola, sin A/B controlado ni estimación de una futura ruta
de captura interpolada. El log confirma grabación sin pérdidas, silencio por
faltas ni bloques tarde; no identifica por sí solo el pasaje musical.

Referencia melonDS pendiente por falta de acceso gráfico en el entorno.
Conclusión limitada al recorrido HD propio; no se atribuye la aspereza al
hardware y no se implementa una corrección de emulación sin referencia.
Informe, métricas por banda/canal, alineaciones y scripts: `build/sonido/`,
sufijo `-2`. Todos los WAV y señales permanecen fuera del repositorio.

## 2026-10-28 — Sonido basado en DeSmuME: captura y voces (host)

NSMB, nivel 1-1, 5000 cuadros desde el mismo guardado y con las mismas pulsaciones.
Se comparan 655 607 pares nativos (20,032 s, desde el tick 2 079 000), antes de
cualquier etapa de salida. Se descarta de la traza el último trabajo de 548 ticks,
todavía no recogido por el receptor WAV. Corredor nativo del host, **no tiempos ni
escucha de la PS3**. Evidencia local y WAV de las 16 voces en `build/sonido/`;
extracción reproducible con `SND_TRACE` y `tools/audio-voice-report.py`.

| Medida | Núcleo anterior | Solo captura corregida | Núcleo nuevo completo |
|---|---:|---:|---:|
| Concentración pico/media de saltos por fase del cuadro, señal mono | 2,8961 | 1,4520 | 1,5429 |
| RMS de segunda diferencia L / R (unidades PCM16) | 737,26 / 848,48 | 708,59 / 823,15 | 707,39 / 825,69 |
| Energía 8 kHz–Nyquist / energía total L / R | 0,12484 % / 0,29038 % | 0,11669 % / 0,27698 % | 0,12215 % / 0,27470 % |
| RMS L / R (unidades PCM16) | 5587,31 / 4183,25 | 5546,23 / 4161,22 | 5449,95 / 4203,22 |

- Aislamiento: la variante local de captura conserva el decodificador, arranque y
  paneo anteriores; cambia FIFO, visibilidad de escrituras y reglas de captura.
  Correlación 0,999895, desfase 0. La reducción de concentración de saltos (49,9 %)
  identifica un defecto de realimentación dependiente de los bloques.
- En ese tramo SOUNDCNT es `0xB97F`: L sale exclusivamente de la voz 1 y R de la 3;
  ambas son PCM16 de SOUNDCAP, TMR `0xFE00`. Se capturan las mezclas L/R sin esas
  voces. Las otras voces usan ADPCM y, entre la 8 y la 13, también PSG; no hay
  ruido LFSR ni PCM8. SOUNDBIAS siempre 512. El mapa previo solo usaba ADPCM.
- Las tablas de pasos/índices ADPCM ya coincidían; cambian el redondeo de la
  diferencia, el extremo negativo y la recuperación del bucle. El temporizador
  de captura nominal y el sesgo constantes descartan esos dos factores como
  causa de este tramo. No se atribuye el ruido a todas las voces ADPCM.
- `tools/audio-stage-compare.py` (ambos argumentos son **host**, aunque sus claves
  se llamen `ps3`/`host`): núcleo completo, correlación 0,953850, desfase −1 muestra,
  diferencia de espectro por banda 0,06–0,15 dB. Su `snr_db=8,04` mide el residuo
  entre dos implementaciones, **no** una SNR contra audio limpio. No hay una
  referencia digital independiente de este juego y no se afirma que todo el
  siseo se haya eliminado. La banda alta cambia poco y contiene música legítima.
- El interruptor anterior reproduce exactamente los 2 734 607 pares del WAV
  previo. Las etapas HD/Hermite, FIR, estirado, limitador y ganancia siguen
  disponibles. Costo del SPE y escucha A/B con audífonos: pendientes de PS3.

## 2026-10-27 — Audio PPE/SPE y estiramiento fuerte (host/RPCS3)
- Autoprueba aislada en RPCS3, cuatro trabajos fiel/HD: antes, `hd_last` difería una ULP en voz 1/trabajo 2 y voz 0/trabajo 3; después, muestras 0, estado 0, memoria 0 y salida 0. Holgura del SPE de audio: 171 072 B (mínimo 8192).
- Banco host reproducible, producción 0,5: variante normal/larga, 664/332 uniones; flujo en empalmes 0,0102641→0,0079671 (×0,7762), CV medio de envolvente ×0,8455, nitidez de ataques ×0,9393. A producción 1,0: salida binariamente idéntica y 0 uniones en ambas.
- Costo host del estirador por bloque de 10 ms: 11,825→19,266 µs a producción 0,5; solo indica orden relativo. El tono sintético queda en 997±3 Hz y no se remuestrea dentro de WSOLA.
- Guion local `build/audio7/guion-ps3.txt`: PPE/SPE de NSMB y cinco aislamientos, más MKDS normal/largo. La identificación del ruido y todo costo del Cell quedan pendientes de esas capturas y mediciones reales.

## 2026-10-26 — Mezcla rota de NSMB y verificación SPE/PPE
- `grab-mezcla.wav` de PS3, 32728 Hz y 20 s: pico/media de segunda diferencia L/R 2,785/2,583, fase 330; H6 y H9 de 59,826 Hz sobresalen +15,72 y +13,40 dB. En el WAV host completo a 48 kHz: 1,845/1,649, fase 400; los puntos de captura y tasas son distintos.
- Alineación por la derivada de la envolvente RMS: el host queda 2,1764 s después; PS3 2–8 s contra host 4,1764–10,1764 s, remuestreado linealmente a 32728,498 Hz, da correlación mono 0,98748.
- En ese tramo de 6 s, pico/media L/R: PS3 19,990/30,620 y host 2,601/3,333; fases PS3 520/520 y host 70/350. La captura PS3 concentra discontinuidades unas 6–9 veces más, pero el host es posterior al FIR a 48 kHz: no es una comparación binaria ni de nivel.
- `audio-verificar` medirá en PS3, por trabajo, el costo de tres copias de unos 4,1 MiB, una mezcla PPE y la comparación/aplicación; también líneas vivas cambiadas y diferencias de muestras/estado/captura. No se extrapolan tiempos del host ni de RPCS3.
- Guion: NSMB nivel 1 durante 60 s con `audio-verificar` y, si hace falta, `audio-grabar`; conservar `nucleo.log` y la primera diferencia. Cero diferencias y sonido limpio con líneas cambiadas confirmaría la carrera; diferencias estables aislarían el campo del SPE.

## 2026-10-26 — Tic por cuadro y régimen nominal del audio (host)
- NSMB, 3300 cuadros, captura PCM16 previa al PPE: 3098 trabajos de 547 muestras, 200 de 548 y el primero parcial. Con la frontera antigua, pico/media de saltos L/R 1,961/2,527; conservando la línea SOUNDCAP de las voces 1/3, 1,240/1,196 (meta ≤ 1,5).
- El residuo antigua−corregida tiene los doce primeros múltiplos de 59,826 Hz entre 14,0 y 20,8 dB sobre sus vecinos. Aislamiento por voz: conservar solo la 1 aplana L 3,03→1,30; conservar solo la 3 aplana R 3,78→1,27.
- Simulación de 600 bloques de 480 pares con producción por cuadro de 799/800: salida exacta, 0 uniones, 0 pares insertados y 0 bloques WSOLA activos. Corrección de deriva del remuestreo ±0,25 %, suficiente para 59,94 frente a 59,826 Hz; falta confirmar contadores en PS3.
- El host solo acredita comportamiento y orden relativo, no tiempos del Cell. Guion A/B local en `build/audio5/guion-ps3.txt`; medir en NSMB faltas, uniones, producción y la captura nativa con `tools/audio-frame-metric.py`.

## 2026-10-26 — Audio HD opcional (audio-estirar)
- Banco propio `tools/audio-hd-quality.c/.py`, host LE: PCM16 periódico, seno Fs/16, amplitudes 0,5 y 0,01; Fs efectiva 7998,56/16004,77/21990,80 Hz; ajuste senoidal entre 1 y 3 s, sin WSOLA (velocidad completa), debajo de la rodilla. Evidencia en `build/audio3/`.
- Fiel de 16 bits → Hermite, Fs 8/16/22 kHz: SINAD 19,65/22,59/18,90 → 62,76/62,75/62,76 dB; mayor espurio superior −23,51/−29,53/−23,47 → −64,06/−64,06/−64,04 dBc. A 22 kHz domina una imagen plegada a 12,11 kHz; la imagen directa Fs−f ya era rechazada por el FIR fiel.
- SNR tras excluir continua, armónicos 2–10 e imagen Fs−f: 21,96/23,58/18,91 → 68,69/68,68/68,69 dB; residuo −31,05/−32,67/−28,00 → −77,72/−77,71/−77,72 dBFS RMS (incluye otros espurios, no solo ruido blanco). THD2–10 0,0330/0,2143/0,4968 → 0,00216 %.
- Silencio: cero exacto en las tres rutas, RMS −∞ dBFS; SNR no definida. Señal débil a 8 kHz: THD2–10 0,110 % (16 bits), 8,601 % (10 bits), 0,0978 % (HD). La cuantización a 10 bits exige `original = si`; no explica por sí sola el ruido de la salida habitual.
- 16 voces en host: fiel 0,0542 → HD 0,1328 ms/cuadro; no son tiempos del SPE. Banco de arranque `DATO audio-hd-banco`: 120 cuadros por ruta, PCM16 a 8/16/22 kHz, media/máximo del decrementador; referencia histórica en juego 0,31 ms/cuadro, nueva medida PS3 pendiente.
- `audio-hd` en el directorio de datos, apagado por omisión; host `--audio-hd`. Hermite causal retrasa dos muestras por voz, reloj racional, mezcla flotante y compensación de parada de 32 pares; limitador existente al final. Decodificación, SOUNDCAP y lectura del juego conservan la ruta fiel.
- Se conserva Hermite sin sinc de ocho puntos: el costo host ya aumenta y falta el margen SPE real. No elimina cuantización de origen PCM8/ADPCM ni todo alias; atenúa cerca de Nyquist por voz. No se justifica activarlo por omisión sin escucha y costo PS3.
- Guion único `build/audio3/guion-ps3.txt`: Metroid (intro/menú/voces) y MKDS (demostración/carrera), mismos tramos con `audio-hd.off` y `audio-hd`; oír siseo, aspereza y clics; comparar banco de 16 voces y `DATO audio`. Salir SELECT+START y O, conservar ambos logs; no borrar interruptores.

## 2026-10-28 — Presupuesto para preferir HD [PS3 rondas 4/5 recibidas; política en host]

Análisis reproducible en `build/niebla-final/diag-6/logs.py` y `logs.txt`.
`nsmb-niebla4.log` contiene `juegos-niebla4.log`: se analizan por separado,
sin sumar sus cuadros. Se separa cada juego al abrir su ROM.

| Consola / selección de ventanas | Ventanas | Servicio aproximado p25 / mediana / p75 (ms) |
|---|---:|---:|
| Ronda 4, NSMB, 120/120 RSX | 55 | 12,838 / 13,631 / 14,242 |
| Ronda 4, Metroid, 120/120 RSX | 18 | 13,914 / 22,466 / 27,405 |
| Ronda 5, NSMB, con 3D | 37 | 6,948 / 9,341 / 10,455 |
| Ronda 5, Metroid, con 3D | 63 | 7,901 / 14,966 / 19,044 |

Servicio aproximado = media del intervalo menos media de la espera del flip;
las medias pertenecen a la misma ventana, pero no a una captura emparejada
por cuadro. Las ventanas completas RSX de Metroid incluyen escenas ligeras
y pesadas: para el tramo desde 120 s siguen siendo válidas las 13 ventanas
del análisis anterior. No comparar sus medianas con las de otros tramos
como un A/B estricto.

NSMB ronda 5: 368 cuadros RSX y 3414 SPE (90,3 % SPE) en las ventanas con 3D.
El costo crudo menor del SPE pierde HD aun cuando ambos caben. Metroid:
1912 RSX y 4882 SPE (71,9 % SPE) en esas ventanas; conservar la posibilidad
de bajar en los tramos pesados. Los umbrales 16,0/16,667 ms dejan a NSMB
margen respecto de su p75 de 14,242 ms con la ruta anterior y distinguen
las ventanas RSX pesadas de Metroid. El margen de retorno es 0,667 ms;
la mejora mínima del cuadro es 5 % + 0,5 ms para filtrar diferencias pequeñas.
Son decisiones de política, pendientes de ajuste mediante A/B en consola.

La ronda 6 mide trabajo PPE por cuadro sin flip y usa como carga el máximo
de su media y el costo 3D de la ruta. HD se conserva si cabe; sobre 16,667 ms
solo se baja cuando el costo 3D RSX supera SPE ×1,10 + 0,5 ms **y** su carga
supera SPE ×1,05 + 0,5 ms. El retorno con margen y la ausencia de ganancia SPE
exigen dos ventanas de confirmación; la banda 16,0–16,667 ms conserva HD o
prolonga el sondeo. No se ejecutan ambas rutas en un mismo cuadro.

Síntesis en host con el código real (`diag-6/politica.c`), 10 000 decisiones:
NSMB 9440 HD / 560 SPE; Metroid pesado 632 HD / 9368 SPE; CPU saturada sin
mejora del cuadro 9440 HD / 560 SPE; raster GPU lento oculto al PPE 632 HD /
9368 SPE. Incluyen calibración y sondeos. No son tiempos ni fps de PS3.

Límites: las dos rutas se miden en momentos distintos; cambios de escena,
composición/presentación GPU y esperas retiradas junto al flip pueden alterar
el intervalo real respecto de la carga. El costo 3D evita esconder un raster
lento, pero no constituye una medición del plazo GPU completo. Validar con
`rutas`, `costo cambio`, `cuadro` y `HD costos` en Metroid y NSMB; el paquete 6
no tiene mediciones nuevas de consola.

## 2026-10-28 — Nativa desde HD y costo adaptativo [PS3 ronda 4; host corrección de ronda 5]

Datos recibidos de consola, sin sumarlos dos veces: `nsmb-niebla4.log` contiene
`juegos-niebla4.log`. Separar en la apertura de cada juego; para Metroid en
juego tomar las ventanas a partir de 120 s. Script y resumen reproducibles:
`build/niebla-final/diag-5/logs.py` y `logs.txt`.

- Metroid en juego, 13 ventanas completas RSX: p25/mediana/p75 del intervalo
  23,318 / 25,807 / 42,190 ms. No se incluyen los menús de 16,7 ms en esa cifra.
- NSMB: 67 ventanas con 3D, 55 con intervalo ≤17,5 ms, cuatro entre 17,5–20,
  ocho entre 20–25 y ninguna >25; 7383 cuadros RSX y cuatro SPE.
- Grupos de Metroid con niebla final ampliada (20 por escala, 1882 cuadros):
  medianas GPU nativa color 6,113 / auxiliar 5,099 / final 0,186 ms;
  HD 9,304 / 8,083 / 0,704 ms. Mediana del total de cada resolución:
  11,399 y 17,976 ms; máximos 23,245 y 33,187. Los grupos tienen su propia
  media y tamaño, no son muestras individuales ni un A/B nuevo. La reducción
  elimina el grupo nativo solo cuando no hay captura; no se deduce una ganancia
  de fps sumando medianas de ventanas distintas.
- NSMB, niebla sin ruta ampliada (58 grupos, 6862 cuadros): medianas del total
  nativo 1,950 y HD 2,823 ms. Se conserva el detalle de otros modos en el resumen.
- Se detectaron 947 presentaciones omitidas por captura a doble pantalla en
  Metroid. Eso no cuenta todas las capturas: el bit DISPCAPCNT de la instantánea
  decide la necesidad exacta, de forma conservadora incluso con banco no LCDC.

**Ronda 5, conteos previstos, sin tiempos nuevos de PS3:**

- Sin captura: dos resoluciones → una rasterización de escena. Con niebla,
  cuatro pasadas de geometría → dos (color/atributos HD); dos quads de niebla
  → uno. Sigue un único quad de exportación, ahora leyendo HD directamente.
  Banco de 256 placas: 514 → 257 dibujos a 2×; con `3d-rsx-pasadas-ronda3`,
  1024 → 512. La ruta exacta conserva los conteos de la ronda 4.
- Reducción por muestra `(x*s+s/2, y*s+s/2)` entera, filtro nearest,
  RGB6/A5 en una sola conversión. Para escalas pares se añade medio texel a
  las coordenadas del quad, evitando seleccionar por redondeo entre dos texeles.
  No es paridad universal con el raster 1×: la cobertura y la interpolación
  pueden cambiar. Por eso nunca alimenta una captura ni una comparación exacta.
- Selector: ventanas de ocho muestras. SPE empieza midiendo ocho trabajos;
  RSX entra tras dos ventanas con `RSX + 500 us <= SPE * 0,85`. Una ventana
  que no cumple durante el sondeo vuelve al SPE. En RSX, dos ventanas con
  `RSX > SPE * 1,10 + 500 us` fuerzan el SPE. SPE permanece 120 muestras y
  vuelve a sondear; RSX renueva su base SPE tras 120 muestras. Generaciones
  descartan hasta dos trabajos anteriores todavía en vuelo; marcas cero,
  invertidas o muestras >1 s no se utilizan.
- Tramo estable caro: 120 muestras SPE por ocho de sondeo RSX (93,75 % SPE).
  Tramo estable barato: 136 RSX por ocho SPE de renovación (94,44 % RSX).
  Son cuentas de la política, sin respaldos por funciones no admitidas y sin
  las muestras tardías del cambio. El sondeo no dibuja la escena por ambas rutas.
- Métrica RSX: suma de color/auxiliar/final en ambas resoluciones realmente
  emitidas, exportación y preparación PPE. Métrica SPE: mayor duración de
  ambos trabajos, leída del decrementador al retirarlos. No suma espera del
  PPE o emisión que pueda solaparse con GPU. No mide el costo total del cuadro
  ni garantiza tiempos de PS3; comprobar intervalo, composición y cambios HD/nativo.

Validación **host**, sin mediciones de velocidad de consola: 882 231
comprobaciones dirigidas finales por orden LE/SAN y BE/qemu, cero fallos;
entre ellas 32 comprobaciones del contrato de captura y secuencias de
10 000 muestras sintéticas para comprobar permanencia y recuperación.
La suite completa conserva únicamente los 34 fallos BE conocidos del anillo
JIT; ROMs, gráficos, sonido, 32 hashes y 16 lockstep correctos. Los 19 shaders
son idénticos a ronda 4. La ejecución gráfica nueva queda pendiente: RPCS3
aborta antes de arrancar incluso con `--help`.

Sin nuevas reservas GDDR3/XDR, shaders ni cambios de stencil en juego. Solo
la autoprueba reserva una superficie temporal para leer HD completa. A/B:
`3d-rsx-nativa-ronda4`, `3d-rsx-sin-costo`; ambos restauran las decisiones de
la ronda 4. La secuencia anterior de dibujos sigue en `3d-rsx-pasadas-ronda3`.

## 2026-10-28 — Pasadas de niebla [PS3 ronda 3; host solo corrección de ronda 4]

Log acumulado `build/niebla-final/ps3/mkds-niebla3.log`: Metroid antes de 290 s,
NSMB entre 290 y 900 s, MKDS después. Análisis reproducible en
`build/niebla-final/diag-4/logs.py`. No son escenas emparejadas para A/B.

| Juego / muestras PS3 | N | Mediana de polígonos | Dibujos totales | Raster nativo ms | Preparación ms |
|---|---:|---:|---:|---:|---:|
| Metroid, motivo niebla final | 25 | 280 | 4480 | 9,949 | 0,688 |
| Metroid, Z sin niebla | 17 | 169 | 1656 | 7,783 | 0,470 |
| NSMB, motivo niebla final | 28 | 316 | 1164 | 4,138 | 0,704 |
| NSMB, W sin niebla | 7 | 757 | 818 | 5,483 | 1,468 |

- Ventanas completas de Metroid 120/120 RSX (15): medianas intervalo 30,509,
  emulación 28,276, preparación 0,699, envío 8,592 y raster nativo 9,697 ms.
  NSMB 120/120 RSX (41): 17,922 / 14,421 / 0,739 / 1,962 / 3,938 ms.
  Preparar datos no explica por sí solo la caída; emitir/ejecutar dibujos domina
  en Metroid. El envío puede incluir presión del FIFO: no se suma al raster
  como si ambos fueran trabajos independientes.
- Metroid a 103,166 s: 705 polígonos, 12 420 dibujos, raster 25,620 ms y
  preparación 1,660 ms. El contador «dibujos 1005 -> 12420» compara una
  estimación simple de una sola rasterización con todos los dibujos reales;
  no mide el costo aislado de atributos. 1005 = 705 + 4×75 translúcidos.
  La ruta ampliada usa 3–4 dibujos por opaco y cuatro más por translúcido;
  3105 por rasterización × color/atributos × nativo/HD = 12 420.
  Los quads finales no se contabilizan ahí.
- hd-10 NSMB: 49 muestras Z, medianas 313 polígonos / 56 dibujos / 0,410 ms;
  dos muestras W, 751 / 804 / 5,406 ms. El tramo W de ~5,5 ms ya existía.
  Los ~0,4 ms son Z sin niebla, mientras la ronda 3 admite nuevos cuadros con
  niebla. Sin una reproducción idéntica no se atribuye toda la diferencia a
  regresión. Sí se identifica que Z plana por polígono fragmenta los lotes
  de color aunque ese shader no la consume.

Ronda 4, **reducción de operaciones, sin tiempos nuevos de PS3**:

- Opaco ampliado sin igualdad: 3–4 dibujos → 1–2. Se consulta la cara anterior
  solo cuando pudo existir un escritor trasero. La igualdad conserva sus dos
  límites inclusivos y la escritura posterior.
- Translúcido simple: cinco → uno con borrado ocupado, dos con borrado vacío
  y mezcla; añadir uno si la textura también puede producir alfa 31.
  Atributos: una pasada translúcida (más la opaca si procede), independientemente
  del alfa previo. En la ruta ampliada se conservan puerta y rechazo de ID;
  las tres pasadas de atributos se reducen a una.
- Lotes de color opaco con niebla: se comparan solo las constantes que lee
  `shade`/`shade_w`. Atributos conserva su Z plana y bandera por polígono.
  En el banco de 256 planos se esperan 1 dibujo de color y 256 de atributos
  por resolución, frente a 256+256; la consola debe confirmar el contador.
- Cifras anteriores son conteos de código. No se promete una reducción
  proporcional de ms ni que se haya alcanzado al SPE. GPU auxiliar y
  posproceso, envío y preparación se registran por separado en
  `DATO rsx3d pasadas`, incluyendo motivos poco frecuentes dentro de cada
  ventana de 120 etiquetas. Las marcas HD se leen solo tras su propia etiqueta;
  sin niebla se aprovechan las marcas existentes.
- Paridad host LE/SAN y BE/qemu: 3 717 120 estados por orden, sin diferencias;
  incluye descarte alfa, destino transparente, ID repetido, caras, TDEPTH,
  mezcla, atributos y ambos bordes de igualdad Z/W. El ensamblado de shaders
  no cambia. Falta ejecutar autoprueba gráfica 1×/2× y A/B con
  `3d-rsx-pasadas-ronda3`, midiendo las mismas escenas de Metroid y NSMB.

Se difieren las dos ampliaciones de cobertura. El límite de IDs de Metroid
afecta 21 ventanas/1580 cuadros; sin sombras, cuatro bits podrían representar
hasta 15 IDs, pero hay que comprobar el remapeo y la frontera de rechazo.
La cara trasera W de MKDS afecta 104 ventanas/12 386 cuadros; exige resolver
los empates de máscaras ya fallidos en dos variantes anteriores. Ningún límite
se abre sin paridad gráfica.

## 2026-10-28 — Profundidad exportada y bordes de niebla [PS3: diagnóstico; host: ensamblado]

- Paquete `salsads-niebla-final-2`, PS3 aislada 1×: 452/512 escenas finales aceptadas, 60 fallos y 1 474 560 píxeles fuera de tolerancia; pruebas antiguas 0/8/10 con 384/8832/7680 diferencias. Los 60 fallos corresponden exactamente a exportar W=0 en la pasada de atributos por colisión COLOR/DEPTH con R1.z. Los tres fallos antiguos coinciden con reconstruir Z−1 en bordes de LUT.
- Corrección comprobada en host: 13 656 casos fp40 con ambos alias de salida, descartes y bordes; las tres imágenes corregidas coinciden con los PPM de referencia (49 152 píxeles cada una). No son ejecuciones ni tiempos del RSX. Atributos Z añade cinco instrucciones; W usa un registro más para separar COLOR de DEPTH. Sin destinos nuevos ni lecturas durante el juego. Medir raster nativo/HD y cuadros >20 ms en consola, después de validar autopruebas 1×/2×; comparación con `3d-rsx-niebla-anterior` en la misma escena.

## 2026-10-28 — Salidas parciales de niebla [PS3: diagnóstico anterior; host: ensamblado]

- Consola, paquete de niebla final anterior, autoprueba aislada 1×: 512/512 escenas rechazadas, 18 874 368 píxeles fuera de tolerancia. Las 18 pruebas anteriores también fallan. PPM 5: distintos R/G/B = 46 080/0/0; PPM 16 = 47 424/46 464/42 816. Dos colisiones de salida con R0 explican rojo sobrescrito y profundidad Z mutilada. La comprobación del ensamblado corregido da 11 556 casos sin fallos en host; no mide rendimiento RSX.
- Sin nuevas mediciones de PS3 para el arreglo. La sonda y sus lecturas están limitadas a autoprueba y no sirven para medir tiempos de juego. Repetir 1×/2× antes de medir Metroid, NSMB y MKDS; comparar `3d-rsx-niebla-anterior` en la misma escena. Sin cambios de reservas GDDR3 ni de pasadas durante el juego.

## 2026-10-28 — Niebla final y diagnóstico del compositor [PS3 hd-10; host solo corrección]

- Niebla final nueva sin medir en PS3. Repite el raster para producir profundidad/bandera y añade una pasada de pantalla completa; ese costo está incluido en las marcas existentes de raster nativo/HD. Comparación con `3d-rsx-niebla-anterior`, misma escena y escala. No se deduce una ganancia con los tiempos del host.
- Relectura de logs hd-10, ventanas de 120 cuadros, medianas de etapas de A agrupadas por censo previo: NSMB con ≥100 RSX (40 ventanas), compone 10,33, capas 2,34, final 7,93 ms; con >60 SPE (6), 4,835 / 2,335 / 2,425 ms. MKDS con ≥100 RSX (46), 7,05 / 0,98 / 6,02 ms; con >60 SPE (9), 2,78 / 0,98 / 1,75 ms. Son grupos de escenas distintas, no una medición A/B por cuadro.
- NSMB, tramo 77–83 s: 77,454 s: 112 RSX/9 SPE, final 7,93; 79,456 s: 54/66, final 2,43; 81,459 s: 36/83, final 2,42; 83,461 s: 120/0, final 7,92. Capas 2,33–2,34 y espera 0,00. La mediana cambia de ruta al cambiar la mayoría de cuadros. B permanece en ~2,40 ms de final. `compose_graphics` llama a `hd_export` solo con `hd_ea`: la exportación adicional es la explicación compatible con estos datos, no una espera del raster dentro de «final». No se aísla todavía selección de capas, transposición o DMA del exportador; no se aplica arreglo al compositor.
- Host LE con ASan/UBSan: 512 escenas dirigidas, color calculado aparte y paridad referencia/bandas. No sustituye la comparación RSX de consola, pendiente por falta de acceso a RPCS3.

## 2026-10-27 — RSX ronda 15 [PS3 hd-9 sin aplazar; RPCS3 solo corrección]
- MKDS 2× sin aplazamiento (hd-9 + r3d-sin-aplazar): 869/7080 cuadros >20 ms (12 %), emulación mediana 13,6 ms; con aplazamiento la imagen HD no se renovaba (0/120). Niebla por fragmento y rescate PPE de texturas: medir en consola el censo RSX/SPE de Metroid y «RGBA8» por cuadro.

## 2026-10-26 — RSX ronda 14 [PS3 hd-8 analizada]
- MKDS 2× con hd-8: compositor A «(final)» 6,0–7,0 ms en carrera (antes 13,2; meta ≤3 no alcanzada), compositor A 8,7 ms; la espera del 3D se aplaza (líneas 88–160) y la pantalla A se congela: A/B en `build/rsx3d-hd-9/guion-ps3.txt` con `DATO rsx3d HD cambio`.

## 2026-10-26 — RSX ronda 13 [PS3 ronda 12 analizada; RPCS3 solo corrección]
- MKDS 2× (hd-7): cuadros >20 ms 26,7/120; RSX preparación 0,5–1,5 envío 0,6–1,0 etiqueta 0,004 raster 2–3 ms; compositor A 15,0 (carrera) y 30,0 ms (escena previa, 254 polígonos) con «(final)» 13,2/28,2 ms = exportación de metadatos HD; Metroid menú 40,9 ms. Nuevo exportador vectorial, meta ≤3 ms: medir en PS3 (`DATO spe-etapas` A «(final)»).

## 2026-10-26 — RSX ronda 12 parcial [RPCS3, no PS3]
- Sombras Z dirigidas: 24–25 dibujos/escena, mediana raster 0,096 ms (12 muestras); cuatro escenas con muchos OID Z/W: 61–62 dibujos, mediana 1,115 ms, incluido primer uso de W de 11,751 ms. Son pasadas adicionales, no una ganancia de tiempo acreditada.
- Igualdad: 8 dibujos opacos o 12 con mezcla; mediana raster 0,064 ms en 76 muestras tras excluir los primeros usos de cada grupo. A 2× se dibuja además el destino HD; tiempos de RPCS3 no extrapolables.
- Sondeo de texturas, 14 lotes: 14 vencen el plazo de 250 µs y luego coinciden al retirarlos; máximo observado en validación final 275 µs a 1× y 273 µs a 2×. Se comprueba también la ruta de lote ya completado; no acredita rescates en juego.
- Empates Z aislados: primera prueba falla 3/8; corregir la lectura de orientación no inicializada permite 8/8 exactos, sin modificar la conversión de vértices.
- Parada: TID ampliado deja 24576 diferencias tras ambas variantes; cara trasera W deja 24576 en cuatro dirigidas tras ambas variantes. Se mantienen en SPE; Z y remapeo OID admitidos solo tras paridad.
- 1×/2×/interruptores apagados: 0 fallos, 16 cuadros exactos y 177 mapas iguales. Evidencia y guion único en `build/rsx3d-hd-7/`; medir % RSX/HD, motivos y coste en PS3 real.

## 2026-10-26 — HD en juego, ronda 11
- PS3 real, `build/ps3-hd-diag/d-mkds-2x.log`: 6294 retiradas RSX, 901 SPE; 6294/8520 presentaciones HD. Hay cuadros sin 3D; el porcentaje total no mide pérdidas de rasters aceptados.
- 48 ventanas con ≥100 retiradas RSX: medianas intervalo 39,623, emulación 25,862, compositor A 39,305, B 7,472, etiqueta/reutilización 0,004 y recogida final 0,005 ms. Incluyen distintas escenas; no sustituyen el recorte de pista del autor.
- PS3, ventanas ≥100 RSX: eventos de línea 2,380 ms a 1× frente a 15,345 a 2× (≈12,965 ms extra); geometría 2,30 frente a 2,57. El compositor A termina antes de presentar porque `vid_wait` lo espera al reutilizar la instantánea dentro de `nds_run_frame`; la recogida final pequeña no incluye esa espera.
- Cambio dirigido: metadatos repetidos por ocho píxeles, conservando prioridades/ventanas/mezclas y ambas alternativas de alfa 3D. No hay tiempos nuevos de PS3; los del SPE bajo RPCS3 no predicen consola.
- Flip HD esperado después de emular y antes de tocar pantalla; A/B `3d-rsx-hd-flip-temprano`. Permite solapar composición RSX de N con emulación N+1; 1× conserva su orden.
- `3d-rsx-hd-metadatos-escalares` conserva el recorrido previo; nuevas esperas PPE por etapa y causas de caída en cada ventana. No sumar «instantánea» a SPE: es un subconjunto.
- RPCS3, juego propio Gouraud: 118/120 HD en la primera ventana (dos cuadros sin capa 3D), 240 cuadros y resultado correcto; alambre vuelve al SPE. No acredita velocidad ni ausencia de retención en PS3.
- Costos raster HD leídos tras su etiqueta, nunca tras la etiqueta nativa temprana: elimina restas de marcas aún no publicadas. Evidencia y guion en `build/rsx3d-hd-6/`.

## 2026-10-26 — Diagnóstico HD solapado, ronda 10
- RPCS3, secuencia original: descriptores originales 1 exacto/15 anteriores/0 otros; distintos 0/15/1 (primer cuadro). Protocolo sin fallos en ambos; 177 mapas iguales a hd-3.
- La variante cambia solo el destino nativo de la ranura 1 y su descriptor de exportación; la preparación DS no cambia. No se promueve a juego.
- Sin tiempos nuevos de PS3: mañana corresponde reproductor 2×, diagnóstico experimental, MKDS 1× y solo con exactitud original MKDS 2×, misma pista dos minutos.
- RPCS3, composición leída entre parejas: 16 exactos en ambas variantes; nativo 7/8/1 y 8/8/0. Esa espera altera la reproducción: no sustituye el banco original ni certifica el solapamiento en juego.
- Evidencia y guion en `build/rsx3d-hd-diag/`; los costos HD con etiqueta nativa temprana no certifican el tiempo total GPU.

## 2026-10-26 — Ronda 9 HD: localización del cuadro anterior (RPCS3)
- Base ronda 7: 16/16 solapados exactos a 2×. Parche ronda 8: 15/16 incorrectos, ambos tras compilación completa.
- Error en x=32..223, y=32..159: 192×128=24576 píxeles, repartidos 12288/12288 entre las mitades superior e inferior.
- Comparación contra los 16 cuadros: salida n igual a referencia n−1 para n=1..15, incluido fondo; no coincide con n+1. Ranura n%2, exportaciones XDR a 18 MiB + ranura×256 KiB.
- Descartada la banda única; causa de la retención completa pendiente. Se restaura el código; evidencia en `build/rsx3d-hd-5/`. Sin tiempos PS3 ni validación de juego HD.

## 2026-10-26 — Ronda 8 HD: exactitud bloqueante (RPCS3)
- Dos intentos con exportación nativa antes del raster HD: ambos fallan 15/16 cuadros solapados; cada fallo afecta 24576 píxeles. El segundo renueva también el sampler nativo, sin mejora.
- 177 mapas síncronos sin cambios; 32 contratos, línea cambiante y seis escenas SPE exactos. No se toman tiempos como resultado válido ni se extrapolan a PS3.
- Experimento revertido; logs y parche en `build/rsx3d-hd-4/`. No se midió MKDS en juego HD ni se certifican 60 fps.

## 2026-10-26 — Composición HD renovada (RPCS3, no PS3)
- Misma instantánea 3D; destino completo 512×384/768×576/1024×768. Metadatos 2D sintéticos prearmados; excluye emulación, generación SPE y flip.
- Copia/composición: media de ocho pasadas tras dos de calentamiento. Total GPU suma esas medias al último raster nativo+HD+exportación caliente; no es tiempo de cuadro de un juego.
- Orden MKDS/SM64DS/NSMB/pesado, ms composición / total GPU:
- 2×: 0,076/1,455 · 0,086/0,974 · 0,073/1,701 · 0,071/2,805.
- 3×: 0,082/1,642 · 0,072/1,039 · 0,080/1,952 · 0,072/2,845.
- 4×: 0,095/1,783 · 0,096/1,208 · 0,085/1,993 · 0,082/3,001.
- Publicación PPE 0,155–0,207 ms; 177 mapas nativos sin cambios por escala. Recarga activa: contratos y seis escenas SPE exactos; apagada retiene datos.
- No acredita 60 fps ni el presupuesto de PS3; consola pendiente. Logs y contraste de las cuatro hipótesis en `build/rsx3d-hd-3/`.

## 2026-10-26 — RSX ronda 6, diagnóstico de composición (RPCS3, no PS3)
- Hipótesis 1, entrada retenida: línea cambiante sin SPE, 722/768 píxeles distintos; patrón idéntico tras tres publicaciones.
- Hipótesis 2, lectura de destino ligado: desligado y borrado en otro destino permiten distinguir la sonda; 3/3 muestras RGBA del 3D exactas, 9/12 palabras de metadatos incorrectas.
- Resultado final de la mínima: 731 diferencias (722 píxeles + 9 sondas); seis escenas SPE siguen con 48200–49151 píxeles distintos y nativo/metadatos exactos. Parada por tope; causa última no demostrada.
- 1× conserva 177 mapas, autoprueba y solapamiento. No hay medición HD con composición ni conclusión de velocidad PS3; guion y volcados en `build/rsx3d-hd-2/`.

## 2026-10-25 — RSX fase 2, cuarta ronda
- **PS3 recibida, ronda 3:** MKDS misma pista, RSX 90 %; intervalo mediana 16,68 ms, 58/71 ventanas a 60; SPE 59/66. Emulación 11,36/10,37 ms y cuadros >20 ms 22,9/12,6 por 120.
- Preparación mediana/p90 1,13/2,73 ms, envío 0,59/1,0; pesado «vértices» 1,68 ms incluía estados; RGBA8 frío 34–41 ms PPE.
- **Cambio:** VMX conserva la división Z; perfil separa estados/vértices y autoprueba compara ambas rutas sobre 16 384 vértices. RGBA8 por lotes de 65 536 texeles en el SPE, tras el raster de respaldo; constantes y stencil solo si cambian.
- **Política fría:** ningún sondeo espera al decodificador; el cuadro completo vuelve al SPE hasta completar sus texturas. Instantánea y arena protegidas; el siguiente raster puede encontrar trabajo pendiente.
- **Validación host/RPCS3:** dirigidas LE/BE 871 923 por orden sin fallos; LE/SAN, ROMs/hashes/lockstep BE correctos, solo 34 fallos conocidos. Autoprueba 0 fallos, 16 cuadros exactos; 176 mapas ronda 3 y 177 A/B iguales. Sonda de texel aislado 512: mismas 24 090 diferencias DS con ambas rutas; no es regresión ni paridad global.
- Arena RSX 24 MiB, descriptores 64 KiB y metadatos adicionales ≈15 KiB; reserva local compartida, doble salida de 4 KiB, holgura 13 840 B. Banco RPCS3 16 384 vértices: escalar 301 µs/VMX 191 µs en una corrida; no acredita mejora en PS3.
- **Guion PS3:** autoprueba aislada; MKDS misma pista 2 min con `3d-rsx` y 2 min con `3d-spe`; salir antes de recoger logs. Resumidor y detalle en `build/rsx3d-fase2-4/`.
- **Decisión pendiente:** preparación+envío ≤0,3 ms mediana y emulación no peor que SPE; una vuelta medida por parte. Si no baja, conservar su interruptor anterior y seguir; no extrapolar RPCS3 ni declarar paridad global RSX/DS.

## 2026-10-25 — Juego RSX solapado (base PS3 recibida; nueva ruta sin medir)
- Base recibida, MKDS carrera: 6590 RSX/730 SPE, intervalo 33,3 ms; raster+exportación mediana 2,36, p90 3,06, máximo 4,49 ms. Aislado pesado 4,24 ms, autoprueba sin fallos.
- Relectura local: 53 muestras RSX, preparación mediana 2,232, copia 0,247, total PPE 5,485 ms; residual envío+espera 2,854. 72 ventanas: emulación 16,59 y frame_begin 14,295 ms. No sumar medianas como si fueran el mismo cuadro.
- Código anterior: la etiqueta 3D se espera dentro de emulación; frame_begin espera el flip. Inferencia: superar el plazo de presentación pierde un vsync; el log anterior no separa envío de etiqueta.
- Estimación caliente orientativa: 16,5 − 0,9 de identidad − 2,4 de espera raster/exportación ≈ 13,2 ms de emulación; compatible con intervalo 16,7, por debajo del tope 25 ms. No es medida del nuevo binario ni garantía en carrera.
- `DATO rsx3d cuadro`: medias por 120 cuadros, tiempos PPE incluidos en emulación; raster/exportación y compositor se solapan, no sumarlos al total. Retiradas RSX y envíos SPE pueden cruzar una ventana.
- A/B: MKDS carrera 2 min con `3d-rsx`, sin prueba aislada; luego 2 min sin RSX. Comparación adicional con `3d-rsx-sin-solapar` si hace falta; perfil de preparación optativo e idéntico en ambos lados.
- RPCS3: 16 cuadros con dos exportaciones pendientes y retirada alternada exactos; autoprueba aislada 0 fallos, 176 mapas iguales a la ronda previa. No se midió velocidad PS3.
- Exigir autoprueba sin fallos y paridad sin regresión; objetivo intervalo 16,7 ms y ~90 % RSX. Si sigue >25 ms, parar con el desglose. Persisten RGBA8 frío en PPE y diferencias RSX de cobertura/interpolación previamente documentadas.

## 2026-10-25 — RSX fase 2, ronda 2 (PS3 recibida; cambios pendientes de medir)
- PS3, base `bc84e66`: pesado caliente raster 4,987–4,993 + exportación 0,197–0,198 ms, 652 dibujos/estados; supera 4 ms. MKDS carrera admitido por RSX, 1,57 ms; Metroid conserva SPE por igualdad.
- Mapas PS3: 11 fallos de niebla, 384–8832 píxeles; rectángulos completos de 192 píxeles, sin diferencias de cobertura. Error RGB6 máximo 11 en prueba 8 y 1 en las restantes; PPM solo indica presencia, no magnitud del error alfa. Falta la referencia 0 entre los 157 mapas: contraste con la referencia previa y el conteo del log.
- Host: 38 rectángulos RGB admiten densidad un nivel menor, 45 admiten Z un entero menor antes de `>>9`; el restante pierde azul 63→62. Hay 39 rectángulos sin PA_FOG con error alfa. Son dos mecanismos compatibles con los mapas, no una lectura de los bits internos del interpolador.
- Densidad entera antes del interpolador y recuperación de constantes pequeñas antes de mezclar; 528384 mezclas host con perturbación de un ULP correctas. Z/W variable con niebla queda en SPE.
- Sombras: consumo inmediato permite lotes estables del mismo ID/estado sin textura ni descarte; 110592 secuencias host conservan stencil/profundidad/escrituras. RPCS3 pesado: 652→436 dibujos/estados (−33,1 %); 176 mapas iguales con lotes apagados/remapeo general, 158 comunes iguales a la ronda anterior. No hay tiempos nuevos de PS3 ni garantía del límite 4 ms.
- Guion local `build/rsx3d-fase2-2/guion-ps3.txt`: mismos cuadros y banderas, sin recargar `.rsx`; comparar con `3d-rsx-sombra-sin-lotes` solo si hace falta. No continuar a juego ni aprobar la ruta si pesado supera 4 ms.

## 2026-10-25 — RSX fase 2 parcial (host/RPCS3; nueva ruta sin medir en PS3)
- Base `c23c56e`, PS3 real aportada por el autor: raster+exportación MKDS 2,06 ms, SM64DS 2,24, NSMB 1,88, pesado 3,93; preparación caliente MKDS ~1,8/pesado ~2,5 ms, identidad ~0,9 ms y decodificación fría 34–37 ms. Es la base, no una medida de esta entrega.
- Censo host por trabajo 3D: MKDS carrera, ventana 5000:8000, 2938/3000 con máscara+sombra; resto de modos 0. Muestra anterior MKDS 800/800 con sombras; SM64DS título y NSMB, 800 cada uno, sin modos pendientes. Metroid en juego, ventana 14000:18000: niebla e igualdad 4000/4000, resto 0; referencia host idéntica y reproductor RPCS3 devuelve el original íntegro por igualdad. No extrapolar a otros niveles ni confundir título con juego.
- RPCS3, conteo de dibujos pesado: base filtrada sin sombras 403; original con sombras general 968, optimizado 652. MKDS carrera original: general 152 → optimizado 108. Los 158 mapas coinciden entre remapeos y con/sin lotes. No equivalen a tiempo PS3: comprobar raster+exportación ≤4 ms antes de continuar.
- RPCS3, mapa MKDS carrera completo contra referencia: diferencias cobertura/RGB/alfa 0/10289/0. En muestras antiguas, incluir sombras cambia 3 píxeles de referencia/3 RSX en MKDS y 25/44 en pesado; no atribuir todos los residuos a estos modos ni certificar paridad global.
- Niebla: primera hipótesis W falló 8 casos por normalización y precisión cerca de límites; segunda usa W normalizada constante explícita, 16/16 correctos. W variable y mezclas vuelven al SPE. Clear image carece de referencia implementada; bordes necesitan atributos vecinos, igualdad tiene margen DS y alambre exige cobertura de aristas originales: no se sustituyen por aproximaciones nativas.
- Datos, capturas solo locales y mapas en `build/rsx3d-fase2/`; interruptores SPE independientes para sombra/toon/niebla y `3d-rsx-sombra-general` para comparar. La LUT reserva 128 B en 20 MiB; sin tocar caché, RGBA8, identidad ni conversión de vértices.
- Validación: LE 8 740 377 comprobaciones sin fallos; unidad BE 8 744 107 con los 34 fallos conocidos del anillo. SAN correcto; compilación del ejecutor BE detenida tras dos caídas del gcc emulado, por lo que ROMs/hashes/lockstep BE no se validaron en esta corrida.

## 2026-10-25 — Identidad incremental del RSX (host/RPCS3)
- Base c23c56e: resultado comunicado de PS3 real raster+exportación MKDS/SM64DS/NSMB/pesado 2,06/2,24/1,88/3,93 ms; pesado aprobado bajo 4 ms.
- Host, 500 repeticiones calientes por cuadro grabado: preparación 75,750→57,090 / 27,854→7,846 / 99,290→80,390 / 124,556→107,330 µs. Flujos idénticos; 100 invalidaciones por escena contra decodificación completa.
- RPCS3, media de tres repeticiones, identidad completa→incremental: 327→2 / 332→2 / 337→2 / 322→3 µs. No son tiempos del PPE real.
- VMX, misma vuelta RPCS3, vértices escalar→vector: 180→272 / 19→29 / 263→333 / 321→253 µs; descartado, sin segunda ronda.
- Caché con generaciones únicas tras reiniciar, propagadas con la instantánea; páginas escritas conservan la comparación exacta de 1 KiB. Sin metadatos se usa la comparación completa.
- A/B PS3: mismos cuadros y banderas `3d-rsx` + `3d-rsx-prueba`, alternar `3d-rsx-identidad-completa`; leer `DATO rsx3d costo` frío y `DATO rsx3d caliente`. En juego, `3d-rsx-perfil` registra las etapas.
- SPE descartado tras una vuelta RPCS3: lotes de hasta 64 KiB en geometría libre; preparación PPE máxima MKDS/pesado 0,378/2,293 ms frente a 17,032/18,960, pero RGBA8 SPE 138,307/48,159 ms frente a 16,531/18,132 y lote frío máximo 67,096 ms. Incluye coste de ejecución/compilación del emulador; no justifica compartir geometría en consola. Paridad: 39 mapas iguales, geometría intercalada exacta y 710 940 comprobaciones ASan/UBSan.
- Detalle, guion y resultados crudos locales: `build/rsx3d-prep/` (SPE retirado en `prototipo/`); paquete de entrega `build/salsads-rsx3d-prep.pkg` (SalsaDS RSX, SLDS00030).

## 2026-10-25 — RSX ronda 3: costo por lotes (host/RPCS3, base PS3 aportada)
- PS3 anterior MKDS/SM64DS/NSMB/pesado, raster+exportación: 4,177/2,427/4,644/7,542 ms. Dibujos 596/55/972/1101 → 185/33/211/403; estados lógicos sin cambio: 185/31/211/403.
- Host nativo, 500 cuadros calientes sin GPU ni reloj de etapas: 120,318/26,376/197,012/312,858 → 76,824/26,932/101,088/121,936 µs; frío MKDS 2141→2468, pesado 2429→2776 µs. No son tiempos PPE.
- RPCS3 pesado, medianas de tres: raster 2,020→0,975 ms, preparación 2,823→1,657; MKDS preparación 1,066→2,823, SM64DS 0,363→0,527: variabilidad y ausencia de mejora uniforme; no convertir a tiempos PS3.
- Calibración prudente con NSMB PS3 sin textura: 4,420/964=4,585 µs/dibujo; conserva el residual. Estima raster+exportación 2,29/2,33/1,15/4,34 ms; con MKDS sin textura, pesado 3,17. Intervalo cruza 4 ms: parada.
- Preparación PS3 orientativa, sin garantía: caliente MKDS 1,6–2,5 ms, SM64DS 1,0–1,2, NSMB 1,8–3,7, pesado 1–3 (sin base caliente propia); frío MKDS/pesado 20–30. Pendiente medir, no promesa de ahorro.
- Reproductor: mismos cuatro `.rsx`, `3d-rsx` + `3d-rsx-prueba`, sin `3d-spe`; sale solo. Leer `DATO rsx3d costo/caliente` tras cerrar. La subida es visibilidad de XDR, no copia adicional de textura; las etapas no cubren toda la preparación.
- A/B por archivos: `3d-rsx-sin-lotes`, `3d-rsx-preparacion-anterior`, `3d-rsx-cache-gruesa`; renombrar a `.off` para desactivar. `3d-rsx-perfil` mide etapas en juego; prueba aislada siempre las mide.
- Guion completo, comparaciones y estimación en `build/rsx3d-ronda3/`; pesado omite 167 sombras, MKDS en juego sigue usando SPE por efectos. Caché llena conserva el respaldo; sin fase 2 ni envío a consola.

## 2026-10-25 — RSX 1×, ronda 2 (host/RPCS3; sin medida PS3)
- Etiqueta de fence previa 322420947/enviada 1: centro 00000000 antes de `finish`, 1f003f00 después. Inicializar etiquetas corrige la primera exportación; compilación síncrona no la corregía.
- Misma lista con Z/W: RGB MKDS 23523/23526, SM64DS 359/359; UV constantes en W dejan 499/57. NSMB: 214 diferencias de cobertura, 145 sin texturas/mezcla (56 comunes); no son categorías disjuntas.
- Modulación/decal y empaquetado constantes exactos; mezcla RGBA8 pierde un nivel incluso entre blancos iguales. Interpolación por líneas, aristas, cuantización y mezcla exacta siguen pendientes; no se añadió copia por polígono.
- Pesado: 1248 originales, 167 sombras omitidas, 1081 polígonos/1101 dibujos, 44 texturas/1015808 B en frío; área ponderada ≈88296 fragmentos. W usa 58 instrucciones y exportación 12.
- Sensibilidad sin calibrar: 3–6 millones de instrucciones/ms, 1000–3000 dibujos/ms y exportación 0,3–1 ms → 1,52–3,81 ms; preparación/copia PPE aparte. RPCS3 no valida ese presupuesto ni el cuadro completo.
- Diagnóstico PS3: `3d-rsx-prueba` y `3d-rsx`, instantáneas `rsx3d-{mkds,sm64ds,nsmb,pesado}.rsx` en el directorio de datos; cada comparación aparece 3 s (referencia izquierda). Sin `3d-rsx`, ambas mitades son referencia; sale solo.
- Juego: renombrar la prueba a `.off`; MKDS pesado 2 min apagado/encendido, misma demostración, salir SELECT+START y O. Mirar aristas, texeles, translucidez, profundidad y primer cuadro; `DATO rsx3d` da modo/motivo, pasadas, bytes y costes.
- Con sombras el juego vuelve al SPE: medir RSX en el reproductor y no confundir respaldo con mejora. Parar si raster+exportación supera 4 ms; atribución residual y paridad siguen sin aprobar. Detalle local: `build/rsx3d-ronda2/diagnostico.txt`.

## 2026-10-25 — Lectura local de planos por lote (cand-v004)
- Base PS3 aportada, MKDS pesado, 159 ventanas: armado ≈ 4,6 ms/cuadro; lectura 1,92, atributos 1,29, recorte 0,80, ensamblado 0,41 y salida 0,20; DMA ≈ 0,03. Meta ≤ 2,5 y espera PPE ≤ 1 ms, aún sin acreditar.
- El descarte fuera del volumen ya precedía a la copia de atributos; «atributos» incluye proyección/cara/profundidad. No se adelanta la cara al recorte entero: sus redondeos pueden alterar el área.
- Variante por vértice descartada sin ganancia clara en host; candidata por lote: cuatro cargas alineadas, trasposición y seis planos SIMD, sin extraer coordenadas escalares (verificado en el ensamblador SPU). No cambia orden, planos, contadores ni overflow.
- Host nativo, MKDS 2200–2499, código anterior/lote con la misma alineación a 16, tres parejas: medianas 0,152880 → 0,152610 ms/cuadro (−0,18 %, no concluyente). Listas completas antes/después idénticas byte a byte en MKDS/NSMB/SM64DS; referencia 1175/2196/2299 cuadros iguales. Evidencia local: `build/armado/`.
- Sin ejecutor SPU disponible; el decrementador ficticio existente cuenta lecturas, no trabajo. No hay ticks de rendimiento válidos ni tercera variante. Ahorro PS3 por etapa: lectura pendiente (techo absoluto 1,92 ms); resto sin ahorro directo previsto; la alineación puede encarecer copias del recorte.
- A/B: `geometria-lectura-escalar` en el directorio de datos (host: `--geometria-lectura-escalar`); conserva la caché de proyección. `geometria-sin-reuso` fuerza también lectura escalar; mantenerlo apagado para aislar este cambio.
- Guion PS3: MKDS pesado 2 min con `geometria-perfil`, repetir con lectura escalar; misma carga y opciones, comparar lectura-local, armado total, SPE y espera PPE por polígonos/cuadro. Renombrar interruptores a `.off`; salir con SELECT+START y O antes de recoger el log.


## 2026-10-25 — RSX 1×: estimación parcial, sin medición PS3
- Host, MKDS, máximo de polígonos en cuadros 2200–2499: cuadro 2384, 1248 polígonos; 300 cuadros idénticos a la referencia.
- Subconjunto Z: 167 polígonos no soportados excluidos, 1101 dibujos, 44 texturas, 1015808 B RGBA8 en frío; área proyectada ponderada por pasadas ≈88296 fragmentos.
- Shader: 53 instrucciones y una lectura; exportación: 12 instrucciones y 192 KiB. Ninguna copia de pantalla por polígono; una copia XDR final para el compositor.
- Estimación de sensibilidad del subconjunto: 1,4–3,7 ms si se sostienen 3–6 millones de instrucciones/ms, 1000–3000 dibujos/ms y exportación 0,3–1 ms. Son supuestos sin calibrar, no tiempos PS3 ni prueba del umbral de 4 ms para el cuadro completo.
- `DATO rsx3d`: polígonos/dibujos/texturas/bytes/aciertos, stamps raster/exportación y tiempos PPE de preparación/copia/total. La caché invalida toda la arena ante cualquier cambio de VRAM.
- Parada por primera exportación vacía repetida en RPCS3; siete escenas siguientes exactas. W, igualdad y efectos pendientes; los mapas clasifican cobertura, RGB en intersección y alfa, sin aislar profundidad de interpolación.
- Guion PS3 aplazado: tras resolver las pruebas, misma demostración MKDS 2 min apagado/encendido; exigir modo efectivo RSX y registrar raster+exportación ≤4 ms, preparación, copia y coste completo. Sin envío a consola.

## 2026-10-23 — Perfil del armado de geometría (pendiente PS3)
- Evidencia PS3 aportada: MKDS pesado, armado 6,93 ms/cuadro a los 60–64 s; host ≈ 0,17 ms no reproduce ese costo. No se optimiza.
- `geometria-perfil` activa `DATO geometria-armado` por ventana: ms/cuadro de lectura local y outcodes, ensamblado sin emisión, recorte, atributos (proyección/cara/profundidad), salida local (índices/límites y escritura).
- DMA: entrada del registro de órdenes (incluye vértices, matrices y demás órdenes) y salida de vértices/polígonos, ambos incluyen su espera mostrada aparte. No existe DMA de lectura de vértices individuales dentro del armado; la salida incluye el vaciado final del trabajo, fuera de `emit_poly`.
- Conteos/cuadro: entrada a emisión, enviados a recorte, supervivientes del recorte antes de descarte por cara/capacidad, salida final, referencias a vértices ya guardados y vértices resultantes del recorte. Un recorte produce cero o un polígono, sin triangularlo.
- Costo: restas y sumas de ticks de 32 bits por trabajo, acumulación de 64 bits en PPE; sin asignaciones ni DMA adicional. Lecturas del decrementador solo con el perfil activo; las sondas y sus ramas perturban el tiempo, costo real pendiente de A/B en PS3.
- Guion: MKDS carrera pesada 2 min, mismo tramo y opciones; repetir con `geometria-perfil` renombrado a `.off`, comparar ventanas de carga semejante y `DATO geometria-spe`. Salir con SELECT+START y luego O antes de recoger el log.

Cada optimización lleva medición antes/después **en la PS3 real** (RPCS3 y el
host no reproducen los tiempos del Cell). Una fila por medición, la más
reciente arriba. Formato: fecha, commit, prueba, entorno, antes, después,
notas.

## 2026-10-23 — Armado de polígonos: copias y proyección compartida (cand-v004)

**PS3 real, rc3, MKDS:** a los 62,832 s, `DATO geometria-spe` informa 11,32 ms/cuadro: vértices 8,49 (armado 6,93), luces 1,63; espera del PPE 4,55 ms. A los 66,834 s el armado llega a 9,89 y la espera a 6,44: la carga no es constante. La meta de armado ≤ 3 ms y espera ≈ 0 queda **pendiente de medir en consola**.

**Dos ideas, host nativo, aritmética `GX_VEC` + `GX_SPLIT_MUL`:** MKDS sin entrada, 2501 cuadros; reloj alrededor de `emit_poly_inner` en los cuadros 2199–2498 (índice desde cero), 669 277 llamadas en 300 cuadros. El reloj por polígono está incluido, sin descontar; estas cifras no son tiempos ni predicciones de PS3. Tres corridas por variante, en serie y alternando el orden:

| Variante | Armado, mediana (ms/cuadro) | Cambio |
|---|---:|---:|
| Original | 0,159977 | — |
| Copiar solo si hay recorte | 0,154913 | −3,2 % |
| Además reutilizar proyecciones en las tiras | 0,147600 | −7,7 % |

La caché incluye el VIEWPORT como clave; conserva las coordenadas enteras y fraccionarias de 64 bits, incluso antes del descarte por cara. Un intento de reducirla a dos coordenadas y reutilizar solo vértices ya guardados perdió la ventaja (0,159743 ms de mediana) y se descartó. No se modificaron las reglas de recorte, divisiones, atributos de salida, ventanas ni sincronización DMA.

**Árbol final con interruptor A/B**, tres parejas adicionales original/final: 0,174820/0,157973; 0,163567/0,157973; 0,163850/0,149507 ms. Medias 0,167412 → 0,155151 ms (−7,3 %); mejora por pareja 3,4–9,6 %. Ganancia modesta: no acredita el objetivo de consola. En todos los experimentos: 1131 cuadros de geometría, hash `ca8447637d3b18da`. Evidencia e instrumentación local en `build/rc4/`; no se incorporan datos de juegos al repositorio.

**Validación y parada:** LE/SAN 8 084 353 comprobaciones y unitarias BE 8 087 998, cero fallos; las 46 escenas 3D de SAN coinciden con referencia y golden. MKDS, NSMB y SM64DS: `--vs-ref3d` idéntico con ambos modos y hashes de geometría iguales (1174, 2195 y 2298 cuadros rasterizados por modo). Compilación completa y paquete correctos, todos los SPE por encima de 8192 B. La batería completa `test-host` quedó interrumpida: GCC ppc64 falló dos veces con `cc1: Segmentation fault` al compilar `salsads-gxparidad`; se detuvo el tercer intento automático. No se cambió el compilador ni la VM. Faltan las ROMs/JIT BE de esta corrida; no instalar la candidata hasta completar esa verificación. Log local: `build/rc4/build-ppc64-run-fallo.log`.

**A/B en PS3:** archivo `geometria-sin-reuso` en el directorio de datos; el arranque dice «geometría SPE: armado …». Desactiva la caché y vuelve a copiar por polígono, conservando la estructura interna ampliada: no es un ejecutable idéntico a rc3. Host: `--geometria-sin-reuso`, también con `--gx-defer`. Repetir la demostración y la misma carrera pesada de MKDS con/sin archivo, manteniendo iguales los demás interruptores; comparar armado, total SPE, espera del PPE y polígonos/cuadro. Renombrar el archivo a `.off` para desactivarlo. No atribuir una diferencia a la geometría si cambia la carga.

**Si persiste la espera (solo propuesta):** identificar primero qué barreras vienen del SWAP y cuáles de lecturas de resultados. Para las primeras, separar cuadro en construcción, cuadro pendiente y cuadro listo, retener registro y destino DMA hasta su finalización, y lanzar el raster al comprobar la secuencia completada; el PPE podría continuar trabajo independiente antes de esa barrera. Las lecturas de matrices, BOX/POS/VEC y RAM_COUNT deben conservar la sincronización. Hace falta comprobar orden, reutilización de memoria y latencia de presentación; no se implementó ni se promete eliminar toda espera.

**NSMB, diagnóstico sin arreglo:** entre 121 y 275,3 s hay 12 ventanas con «línea» entre 17,07 y 45,81 ms. El archivo rotulado como Metroid muestra a los 232,607 s eventos 46,79 ms, de ellos «línea» 45,81 y «gx» 0,28. Cerca, raster3D: SPE0/1 60,24/60,31 ms, espera del PPE 32,38 ms de mediana (53,90 p95), texturas 82,00 ms sumados y 42 922,1 DMA XDR/cuadro. La espera aplazada sigue ejecutándose dentro del evento de línea (`nds_video_submit_pending`); el ARM9 casi a cero puede reflejar el muestreo de cuadros alternos (`frame_no & 1`), no demuestra que no ejecute instrucciones. Causa probable del atasco: tráfico de celdas XDR (forzadas) en el rasterizador. Confirmar con XDR apagada en la misma escena; no sumar ni igualar medianas con promedios de ventanas diferentes.

## 2026-10-23 — Reserva local reducida con respaldo XDR (host)
- MKDS, demostración sin entrada, dos unidades, cuadros 2200–2499; cifras por cuadro, suma de ambos SPE. DMA de 128 B; total = lecturas + escrituras.
| Celdas | Decodificadas | Lecturas XDR | DMA total XDR | Aciertos locales | Holgura SPE (B) |
|---|---:|---:|---:|---:|---:|
| 448 | 840,5 | 11 193,3 | 12 033,7 | 53 719,5 (82,76 %) | 17 856 |
| 384 | 841,7 | 11 287,0 | 12 128,7 | 53 625,8 (82,61 %) | 22 464 |
| 256 | 843,4 | 11 485,0 | 12 328,5 | 53 427,7 (82,31 %) | 31 808 |
- Elegidas 448: menor tráfico entre las tres; DMA total +0,8 % frente a las 512 del prototipo. Referencia idéntica en los 300 cuadros de cada tamaño; mínimo 8 192 B intacto. Sin XDR: 512 celdas y bloques intactos, programa separado, holgura 8 432 B.
- Las líneas de VRAM/paletas conservan su tamaño; solo se reducen celdas y etiquetas. No son tiempos de PS3: falta confirmar coste de DMA y regresiones con XDR en los otros juegos.

## 2026-10-23 — Caché XDR: prototipo, enlace del SPE bloqueado
- Host nativo, MKDS demostración sin entrada, dos unidades, celdas forzadas, ventana 2200–2499: base 11 101,8 decodificaciones/cuadro; XDR 839,2 (−92,4 %), 10 262,7 aciertos y 11 941,0 DMA de 128 B (≈1,46 MiB/cuadro).
- `--vs-ref3d`: 0 diferencias en los 300 cuadros contabilizados. Base previa: mediana 1,589 ms; prototipo actual: una corrida 1,789 ms; solo orden relativo, no tiempo de PS3.
- Techo por conteo: sustituir 92,4 % de decodificaciones por consulta/DMA; no elimina buscar y leer datos/paleta para validar contenido. No equivale a ahorrar 92,4 % del tiempo de texturas.
- Primer prototipo: 819,9 decodificaciones y 2,609 ms; hash por palabras en el actual reduce coste de consulta; las colisiones se resuelven por comparación exacta.
- Dos enlaces fallidos: holguras 5 248 y 5 952 B frente a 8 192 B exigidos. Trabajo detenido, sin paquete ni medición de consola; NSMB, SM64DS y Metroid pendientes.

| Fecha | Commit | Prueba | Entorno | Antes | Después | Notas |
|---|---|---|---|---|---|---|
| 2026-10-19 | r3d-reparto | **Render 3D: reparto de bandas con historia** (carga del SPE más cargado por cuadro pesado, dos unidades; costos por banda del host con los coeficientes de la PS3: preparación 0,71 µs, línea 0,70 µs, píxel 0,062 µs, texturas calibradas) | **host + modelo**, no la PS3 | `r3d_balance` por estimación: MKDS carrera 10 949 µs (ideal 9 891), NSMB nivel 6 334 (6 100), SM64DS 5 205 (5 166); alterno: 10 599 / 6 552 / 5 205 | historia (costos medidos de la banda dos cuadros antes): **10 041** / **6 218** / 5 205 (oráculo 10 002 / 6 196) | La estimación queda 10 % sobre el ideal en MKDS, peor que el reparto alterno; la historia a 1,5 %. Ganancia **[est]** ≈ 0,9 ms en el SPE más lento de MKDS pesado, 0,1 ms en NSMB. En la PS3 el desequilibrio de los logs es de 6–9 % en media (máximo 29–31 %). **Sin medir en la PS3** (el SPE escribe los ticks por banda; en el host no hay historia) |
| 2026-10-19 | r3d-reparto | **Render 3D: caché de texturas por celdas de 4×4 con líneas de 1 KB** (Mario Kart DS en carrera, cuadros 2500–3000, dos unidades; contadores por cuadro, suma de las unidades) | **host (contadores) + modelo de la traza**, no la PS3 | PS3 (ps3-gxrapido): texturas **15,0 / 17,2 / 20,0 ms** de 26,5 / 29,2 / 32,6 de dibujo (suma de los dos SPE; SPE0/SPE1 15,2/12,3, 16,0/13,6, 17,4/15,6 ms), 2 460–3 272 bloques decodificados y 2 332–3 144 desalojos por cuadro (621 bloques distintos), 4 890–6 174 líneas de 128 B. Host: 3 278 bloques, 3 150 desalojos, 6 442 líneas | Host: 10 460 celdas decodificadas (8 600 distintas por unidad), 9 500 desalojos, **1 245 líneas de 1 KB**; mismas imágenes (REF3D igual en 1 674 cuadros). Modelo de la traza calibrado con el 17 ms de la PS3: texturas **4,4–5,9 ms** [est] (3,5–5,9 con otras calibraciones) | **Sin medir en la PS3.** La caché automática deja SM64DS (59 bloques, 117 líneas) y NSMB nivel (336 bloques) en bloques, como antes. Estimación **[est]** por SPE en MKDS pesado: 12–17 ms → 8,5–9 ms |
| 2026-10-19 | r3d-reparto | **Render 3D: espera del 3D aplazada a la línea L (≤ 160) del cuadro siguiente, pantalla B antes** | **host** (seguridad) y razonamiento, no la PS3 | espera en la línea 262 del mismo cuadro: el 3D tenía un cuadro de emulación (≈ 14 ms con MKDS pesado); espera media 0,05–2 ms, p95 1,0–4,8 ms | hasta la línea 160 del cuadro siguiente (≈ 1,6 cuadros); 0 imágenes distintas en 1 674 (MKDS), 2 798 (SM64DS) y 3 811 (NSMB nivel) trabajos con dos en vuelo y L = 100, 160 y 190 | Sin latencia agregada: el 2D se dibuja al final de la pasada siguiente a la que lo entrega. La composición de la A (3,6 ms en MKDS) fija L; la B (11,3 ms en MKDS) sale en la 262. **Sin medir en la PS3** |
| 2026-10-19 | gx-rapido | **FIFO de geometría: ráfaga de la DMA de modo 7 y escrituras de la CPU por el bus corto** (`salsads-gxparidad --bench`: 2,8 M palabras de DMA con órdenes baratas y 400 000 de la CPU) | **host**, no la PS3: Mac (clang -O2, intérprete; las órdenes COLOR/NOP casi no cuestan, así que mide lo que cuesta llevar cada palabra a la FIFO) | forma anterior (`--lento`): DMA 23 ns por palabra, CPU 21 ns | rutas cortas: DMA **15 ns**, CPU **17 ns** (−35 % y −19 %) | **Sin medir en la PS3.** Resumen del estado idéntico en NSMB, SM64DS y MKDS (3000 cuadros). En MKDS (cuadros 2500–3000) la DMA mueve ~15 000 palabras por cuadro en 33 arranques y la CPU ~1 000–1 800: en la PS3 las escrituras a la E/S costaban 2,0–2,2 µs de promedio (2,4–3,5 ms por cuadro) porque el CNT de la DMA ejecuta la transferencia. Estimación **[est]** de la ganancia en el ARM9 de MKDS pesado: 2,5–4 ms por cuadro (DMA 1,5–2,5; GXFIFO de la CPU 0,5–0,9; LDM/STM 0,3–0,6); lo que queda es el trabajo propio del motor (`gx_advance_run`, ~8 000–12 000 órdenes por cuadro). Interruptor `gx-lento` para el A/B |
| 2026-10-19 | 2252dfa | **Recompiladores del ARM9 (tercera ronda) y del ARM7**: emulación de un cuadro, mediana de las ventanas de 120 cuadros (New Super Mario Bros. en un nivel, Mario Kart DS en carrera, Super Mario 64 DS) | **PS3 real** con HEN | solo el ARM7 en el recompilador (bde2c16): NSMB 13,8 ms (ARM9 10,3), MKDS 15,0 ms (ARM9 9,4); cuadros por encima de 16,7 ms: 30 % y 25 % | NSMB **8,6 ms** (ARM9 4,8, ARM7 1,11, eventos 2,4), MKDS **12,2 ms** (ARM9 6,9, ARM7 1,15, eventos 3,6), SM64DS 8,4 ms (ARM9 5,1); por encima de 16,7 ms: 2 %, 7 % y 9 % | El pico del ARM9 en las cargas baja de 22–28 ms a ~14 ms. Quedan por la ruta lenta ~350 escrituras a GXFIFO y ~500 accesos a E/S por cuadro en MKDS |
| 2026-10-19 | arm9-perf | **Recompilador, tercera ronda**: banderas entre bloques en el CR, pistas estáticas, búsqueda común de destinos, ventanas caliente/fría, bucles de espera, alineación de LDR en el índice, MUL/LDM/CMP #0/MOV más cortos, datos condicionales con S traducidos (MKDS carrera, cuadros 2500–3000; NSMB mapa 3D, 3800–4400) | **qemu-ppc64 + modelo de cachés** (`tools/jit-perfil.c`), no la PS3 | MKDS: 202,6 k instr., 1,93 M palabras calientes (9,52 por instr.), huella 1202 KB, fallos L1/L2 41 300 / 10 200, modelo 2,35 ms. NSMB: 297,6 k, 3,00 M (10,09), 612 KB, 46 800 / 5400, 2,18 ms | MKDS: 183,9 k, 1,44 M (7,82), 780 KB, 24 200 / 6800, 1,57 ms (−33 %). NSMB: 283,5 k, 2,32 M (8,19), 420 KB, 26 800 / 4000, 1,54 ms (−29 %) | **Sin medir en la PS3.** Antes = las mismas mejoras apagadas (`--jit-opts 63`). El modelo explica ~2,4 de los ~8,5 ms que la consola mide en la carrera: los milisegundos los dará `DATO jit-tiempo`. Cada mejora tiene su interruptor para el A/B (docs/fase4.md, «Tercera ronda») |
| 2026-10-19 | 245c15e | **Cargas: el sondeo de ROMCTRL salta hasta el final del tramo** (Mario Kart DS 700 cuadros, New Super Mario Bros. 1300, Super Mario 64 DS 1500; tramo de 1024) | **host**, no la PS3: Mac (clang -O2, intérprete) | ARM9 141 M / 263 M / 283 M instrucciones; MKDS 2,8 ms por cuadro de corredor (cargas: 5,4 ms, 91 000 lecturas de ROMCTRL por cuadro) | 50 M / 144 M / 153 M; MKDS 1,4 ms (cargas: 1,6 ms) | Misma imagen en MKDS y NSMB; SM64DS difiere en un destello. En la PS3 las cargas costaban ~24 ms de ARM9 por cuadro (~70 sondeos por tramo): **sin medir** |
| 2026-10-19 | arm7-jit | **ARM7 con el recompilador (ARMv4T)**: tiempo del ARM7 por cuadro (1500 cuadros, tramo de 1024, modo rápido) | **host (qemu-ppc64)**, no la PS3: el código PowerPC lo traduce qemu, así que el tiempo no vale; sirven los contadores | PS3 con el intérprete: ARM7 3–4 ms por cuadro (NSMB nivel 3,8, MKDS carrera 3,2). qemu con el intérprete: NSMB 1958 µs, SM64DS 1536, MKDS 3243 | qemu con el recompilador: NSMB 770 µs, SM64DS 750, MKDS 990 (×2,5, ×2,0, ×3,3). Contadores por cuadro: NSMB 192 entradas y 258 llamadas al intérprete para 27,5 k instrucciones; SM64DS 150 y 430 para 21,6 k; MKDS 281 y 1048 para 53,4 k | **Sin medir en la PS3.** Tiempos de qemu solo como orden relativo. La entrada al código generado costaba ≈ 9 µs por tramo en el ARM9 (consola): con 150–280 entradas el ARM7 quedaría en 1,4–2,5 ms **[est]**; meta < 1 ms. El paquete escribe `DATO jit7` y el ARM7 en `DATO desglose` |
| 2026-10-18 | fase4 | **Recompilador del ARM9 (Fase 4)**: código emitido y salidas en juegos (1500 cuadros, modo rápido) | **qemu-ppc64**: no vale para tiempos | intérprete: ARM9 44–49 ms por cuadro en la PS3 (fase33) | 15,1–15,6 palabras PPC por instrucción guest (caliente + fría); 2057–4509 entradas, 774–2969 llamadas al intérprete y 4900–9700 accesos lentos por cuadro (NSMB, MKDS, SM64DS) | **Sin medir en la PS3**: paquete SLDS00009 con `DATO jit` (docs/fase4.md) |
| 2026-10-18 | 52a398f | **Render 3D: bucle de la línea sin divisiones enteras ni 64 bits** (mismos bits) | **host**, no la PS3: Mac (clang -O2), una unidad | SM64DS «Touch to start» 2,43 ms por cuadro, MKDS demostración 3,62 | **1,52** y **3,30**; NSMB (menú a 1-1) 0,95 | **Sin medir en la PS3**: `DATO raster3d` del paquete fase32. RPCS3 (no mide): escena 9 de gx3d 1,6 ms por SPE con 31 000 píxeles |
| 2026-10-17 | d7f059a | **Cuadro ocioso del planificador** (`ocio`: las dos CPUs en IntrWait; y la demostración, ≈ 4,7 k instrucciones por cuadro) | **host**, no la PS3: Mac (clang -O2) | 177 µs por cuadro ocioso (8753 tramos de 128 ciclos por cuadro); demostración 220 µs | **24 µs** (13,5 tramos, 525 saltos ociosos y 526 eventos por cuadro) y **67 µs**; `carga` (ocupadas las dos CPUs) 146–148 → 145–146 MIPS | ×7,3 y ×3,3 en el host. En la PS3 (paquete de 400ca88) la demostración costaba 2,73 ms por cuadro y la recta de sus ventanas da ≈ 2,1 ms fijos + 0,16 µs por instrucción: el costo fijo era el planificador. **Sin medir el después en la PS3**: el paquete 9f2a177 escribe `DATO desglose`. Quedan ~525 eventos de video por cuadro |
| 2026-10-17 | 9f2a177 | **ROM de carga: MIPS del intérprete con las dos CPUs ocupadas** (`carga`, 120 cuadros, ≈ 300 000 instrucciones por iteración) | **host**, no la PS3: Mac (clang -O2) y contenedor arm64 (gcc -O2) | 6–7 MIPS en total en la PS3 con New Super Mario Bros. y la demostración (paquete de 400ca88: ARM9 4,7, ARM7 2,1) | Mac 145–146 MIPS; contenedor 134–139 | **Sin medir en la PS3.** El paquete 9f2a177 embebe la ROM y escribe `DATO MIPS` (dentro de cada CPU con el timebase y sobre el tiempo total) |
| 2026-10-17 | 1541de7 | **Máquina de la DS (src/core + src/cpu): MIPS con las dos CPUs, el planificador, IRQ, DMA y E/S** (ROMs completas y tres respaldos del autor, 600 cuadros) | **host**, no la PS3: Mac (clang -O2, tramo de 128 ciclos) y contenedor arm64 (gcc -O2) | intérpretes solos (1.1, ROM con `ndsmin`): 105–110 MIPS | `cpuarm` 99–106 (Mac) / 114–126 (contenedor); `cpu9` 106 / 117–123; `irq`, `dma`, `ipc`, `periph`, `bios` 79–85; Mario Kart DS 123,5, New Super Mario Bros. 116,2, Super Mario 64 DS 101,6 | **Sin medir en la PS3.** El HALT no cuesta (la CPU en HALT adelanta su reloj). Coste del planificador sobre `cpuarm`: tramo 1 → 27,8; 16 → 77,2; 128 → 99,5; 1000 → 108,5; 20 000 → 103,7 MIPS (≈ 6 % de 128 frente a no partir). El paquete «SalsaDS núcleo» escribe `DATO nucleo` (ms de emulación por cuadro e instrucciones por cuadro) para medirlo en la consola |
| 2026-10-17 | 2f5693e | **Intérprete de CPU (src/cpu): instrucciones por segundo** (`armbench`: los 4 bucles de la 0.7, un núcleo ARM y uno Thumb) | **host**, no la PS3: Mac (clang -O2) y contenedor arm64 (gcc -O2) | intérprete mínimo de la 0.3 en la misma Mac: 244–277 MIPS (`mezcla`); en la PS3 dio 18,1 | Mac: mezcla 182,0, suma64 135,5, ramas 133,6, bytes 183,5, arm-mix 134,4, thumb-mix 186,0; contenedor: 192,3 / 125,5 / 124,9 / 197,7 / 144,3 / 125,5; ROMs completas (ambas CPUs): 105–110 | **Sin medir en la PS3.** El núcleo completo es ≈ 1,5× más lento que el mínimo de la 0.3 en el host; por escala caben ~12 MIPS en el PPE **[est]**. Sirve para arrancar y depurar; el recompilador sigue siendo obligatorio |
| 2026-10-17 | df3153a | **jit-arm: MHz de ARM9 del código traducido** (4 bucles × 3 formas de banderas) | PS3 real con HEN (paquete «SalsaDS JIT», SLDS00007; log en `docs/datos/jit-arm-df3153a-ps3.log`) | intérprete de 0.3: 18,1 MIPS; referencia 20–22 | JIT perezosas 508–793, **CR 535–840**, fusionadas 506–866 MIPS; peor bucle 534,8 = 8,0× los 67 | Meta de 67 MHz cumplida con 8× de holgura. ×43,9 contra el de 0.3 en `mezcla`. CR/perezosas +5 a +10 %; fusionadas −36 % en `bytes` (sin explicar). Techo: subconjunto ARM, memoria plana de 16 KB, sin E/S |
| 2026-10-17 | df3153a | **jit-arm: el ARM7 en el hilo 1** (ARM9 `suma64` JIT: 526,9 MIPS) | PS3 real con HEN | sin hilo del ARM7 | girar −31,9 %; prioridad baja −28,3 %; `sysUsleep(50)` −6,0 % (despertar 29 µs); semáforo −0,7 % (1 ms) / −8,9 % (100 µs) / −25,6 % (33 µs), despertar ~9 µs, p99 36–72; cola de eventos −1,0 % / −9,6 % | Decisión: dormir en un semáforo de lv2, despertares ≥ 100 µs. Avisar cuesta 3,4–4,6 µs en el hilo del ARM9 |
| 2026-10-16 | 01d267d | **dual-screen: salida de dos pantallas** (720p/1080p × 4 distribuciones × escala entera/ajustada) | PS3 real (paquete «SalsaDS pantallas», SLDS00004) | — | 16/16 combinaciones a 60 Hz estables: intervalo mediana 16,67 ms, p95 ≤ 16,84, 0 vblanks saltados, PPE 1,05–1,07 ms por cuadro | Criterio cumplido. Distribución comprobada por píxeles en RPCS3: 0 fallos |
| 2026-10-16 | 26368ff | **spe-gpu2d: composición 2D de las dos pantallas en un SPE** (escenas tipica, pesada, extrema) | PS3 real (paquete «SalsaDS 2D», SLDS00003) | estimación [est] 1–3 ms | típica **4,31 ms** (A 2,19 / B 2,12), pesada **5,59** (2,77 / 2,81), extrema **8,46** (4,32 / 4,14); referencia en el PPE 22,7 / 34,9 / 38,2 ms | Criterio < 4 ms **NO cumplido** con 2 pantallas en 1 SPE; con 1 pantalla por SPE ~2,2 / 2,8 / 4,3 ms. 73–192 esperas por el mapa por pantalla. Imagen idéntica a la referencia (24 semillas). LS ≈ 199,7 KB |
| 2026-10-16 | 255b0d2 | **spe-raster3d: caché de texturas desbordada** (N = 2048, 24 texturas = 176 KB decodificadas en 112 KB) | PS3 real con HEN (paquete «SalsaDS raster», SLDS00002; log en `docs/datos/spe-raster3d-255b0d2-ps3.log`) | con 12 texturas: 1× 4,19 ms, 2× 8,38 ms (2 SPEs alternas) | con 24: 1× **22,65**, 2× **52,36** (1 SPE: 42,22 y 101,84) | ×5,4 y ×6,2. 1178 fallos y 2913 KB de VRAM por cuadro a 1×; 35,4 ms de 42,2 son texturas. Primera decisión de la Fase 3 |
| 2026-10-16 | 255b0d2 | **spe-raster3d: 4× de resolución interna**, N = 2048 | PS3 real con HEN | estimado (2 SPEs): ~14 ms banda, ~13 tile | 2 SPEs: banda **22,20** / tile **19,61**; 1 SPE: 42,82 / 37,64 | No cabe en 2 SPEs a 60 Hz (16,7 ms). El tile gana 12 % a la banda de 4 líneas |
| 2026-10-16 | 255b0d2 | **spe-raster3d: 2× de resolución interna**, N = 2048 | PS3 real con HEN | estimado (2 SPEs): ~4,5 ms | 2 SPEs: banda **8,38** / tile **7,96**; 1 SPE: 15,17 / 14,33 | Al límite de 8 ms; la estimación se quedó corta ×1,8 |
| 2026-10-16 | 255b0d2 | **spe-raster3d: 1× (criterio de la 0.4)**, N = 500 / 1000 / 2048 | PS3 real con HEN | estimado N = 2048: ~3,5 ms (1 SPE), ~2 ms (2 SPEs); el PPE no se estimó | 1 SPE: 3,63 / 4,64 / 6,76; 2 SPEs alternas: **2,15 / 2,80 / 4,19** (mitades 2,20 / 2,82 / 4,25); PPE escalar (N = 2048): 50,46 | **Cumple (< 8 ms).** Medida ×1,6–2 sobre lo estimado: 152 ciclos por grupo de 4 píxeles y ~1950 por triángulo de setup (supuestos ~80 y 500); el setup duplicado es 29 % del tiempo a 1× |
| 2026-10-16 | 08b7ef0 | **ppe-bench: memoria LE** (`lwbrx`/`stwbrx`/`lhbrx`/`sthbrx` contra normal y contra rotaciones) | PS3 real con HEN (paquete «SalsaDS bench», SLDS00001; log en `docs/datos/ppe-bench-08b7ef0-ps3.log`) | carga normal: 7,16 ciclos de latencia (32 bits), 2,56 de throughput | `lwbrx`: 7,17 y 2,56 (igual); carga + rotaciones: 13,36 y 4,46 | `lhbrx` 7,19 contra 7,20; rotaciones +4–6 de latencia. Cruce de sector de 32 B: 50–57 ciclos (microcódigo); no alineado dentro del sector: gratis. Decisión: memoria LE con `*brx` |
| 2026-10-16 | 08b7ef0 | **ppe-bench: banderas NZCV** en un bucle ARM de 7,55 instr./vuelta | PS3 real con HEN | cota sin banderas: 3,33 ciclos por instr. ARM | (a) completas 16,70; **(b) perezosas 6,01**; (c) N/Z `cr0`, C `cr1`, V `cr2` 7,60; (d) `add.`/`subfc.` + XER.CA 6,55 | Decisión: perezosas (b) + fusión flags/salto y banderas muertas en el recompilador. Reevaluar (c)/(d) con trazas reales (Fase 4) |
| 2026-10-16 | 08b7ef0 | **ppe-bench: costo de instrucciones** (throughput, bruto) | PS3 real con HEN | `add` 1,54 ciclos | `add.` 1,67, `subf.` 1,55, `neg.` 1,67, `addc`/`subfc` 1,54, `adde`/`subfe`/`addze` 2,05 (nativas); `and.`/`andi.`/`cntlzw.`/`rlwinm.` 14,5; `slw`/`sraw` 16,6; `mullw` 11,5; `mfcr` 27,0; `mtcrf 0xff` 20,7; `mtocrf` 1,55 | Los netos 0,00 son artefacto de un bucle de referencia más lento (nops de 2,06 ciclos) que las instrucciones: valen los brutos. Corrige la guía de Salsa64: la aritmética con punto y el acarreo no son microcódigo. `mtctr`+`bctr`: sin penalización con 0 instrucciones en medio (destino fijo) |
| 2026-10-16 | 08b7ef0 | **ppe-bench: despacho de un intérprete ARM mínimo** (bucle de 9 instr. por elemento) | PS3 real con HEN | `switch`: 16,53 MIPS guest (193,6 ciclos/instr.) | tabla de punteros 16,52 MIPS; `goto` calculado 17,88 MIPS (179,0) | Lejos de los ~100 MIPS de la DS: el recompilador es obligatorio. El despacho cambia ≤ 8 % |
| 2026-10-17 | fase2 | **Compositor 2D de la Fase 2.1: una pantalla** (escenas tipica, pesada, extrema, afin, efectos) | **host**, no la PS3: Mac (clang -O2, núcleos escalares); RPCS3 solo como prueba de arranque | referencia de la 0.5 en la PS3: típica 2,19 / pesada 2,77 / extrema 4,32 ms por pantalla | host 0,67 / 0,61 / 0,69 / 1,59 / 1,04 ms; RPCS3 (sin valor) 1,9 / 2,0 / 2,7 / 18,5 / 13 | **Sin medir en la PS3** con el compositor completo (afines, ventanas, efectos). «SalsaDS núcleo» escribe `DATO spe` con el tiempo de cada SPE por pantalla y cuadro |
| 2026-10-17 | fase2 | **Compositor 2D de la 2.2 con New Super Mario Bros. (menú)**: pantalla A / B por cuadro | RPCS3 (cuenta instrucciones: no mide ni saltos ni rutinas de software); el viejo en la PS3 real | PS3 real con el compositor de la 2.1: **A 28,1–28,9 ms, B 7,8–9,1** | RPCS3 2.1: A 14,8, B 1,3; RPCS3 2.2: **A 4,7** (plan 1,4: afines 0,9; compone 3,4: capas 1,6, color final 1,8), **B 1,2** | Estimación PS3 2.2 (cómputo ÷ 3 + ≈ 2 ms de DMA): A 2,5–4, B 1,5–2,5 **[est]**; falta medir. Causa: fondos afines con `%` y productos de 64 bits por píxel, y un color final escalar con saltos por píxel (el SPE no tiene unidad escalar ni predice saltos) |
| 2026-10-17 | fase2 | **Mezclador de sonido en un SPE** (547 muestras de 32,7 kHz y 813 de 48 kHz por cuadro) | RPCS3 (SPE; sin valor de rendimiento) | — | NSMB: 0,27–0,57 ms por cuadro de trabajo | Sin medir en la PS3 |
| 2026-10-18 | fase34 | **Render 3D en el SPE: línea vectorial, texturas por tramos y aristas en 32 bits** (SM64DS, NSMB nivel, MKDS) | antes: PS3 real (paquete 9ce969d, logs de la Fase 3.3); después: host (Mac, clang -O2, una unidad) y RPCS3 (no mide) | meta ≤ 8 ms por SPE en SM64DS y NSMB; estimación [est] 3,5–6 ms | PS3 antes: SM64DS **36,3 / 37,0** (SPE0 / SPE1; 575 polígonos, 180 000 píxeles, 483 bloques), NSMB nivel **26,7 / 27,2** (137 000 píxeles), MKDS 6,6–15; ajuste: 210 ns por píxel, 73 µs por bloque, 3,7 µs por polígono·banda. Host antes → después: SM64DS 1,63 → 0,83 ms, MKDS 2,44 → 1,94, NSMB 0,40 → 0,40 | **Sin medir en la PS3 con el código nuevo.** El `DATO raster3d` trae ahora `polígonos / aristas / píxeles`. Mismos bits que la referencia (392 000 líneas al azar, 40 escenas × 4 repartos LE y BE, los tres juegos) |

## 2026-10-28 — HeartGold: diagnóstico funcional del guardado (host)

Sin medición de rendimiento PS3 ni optimización del camino caliente. Comparación con la misma copia de 512 KiB,
3300 cuadros con `--wifi-sin-inicio` y 5500 activos (estos últimos incluyen CONTINUE y llegada al mapa):

| Caso (host nativo, intérprete) | Lecturas AUXSPI | Bytes leídos | Distintos del archivo | RDSR |
|---|---:|---:|---:|---|
| Inicialización local desactivada | 1660 | 424249 | 0 | 3320 respuestas 00h |
| Activa, hasta el mapa | 2199 | 561777 | 0 | 4398 respuestas 00h |

Las primeras 4980 conversaciones son idénticas byte a byte entre ambas rutas. El aviso ocurre después: SPI del firmware
03h 00h 00h 2Ch devuelve longitud 0000h con la ruta antigua y 0138h con la nueva. Al corregirla se alcanza el diagnóstico de
registros/RAM Wi-Fi; antes no existía estado local que leyera de vuelta los patrones. Con el dispositivo local pasa a CONTINUE.
No hay RDID ni escrituras al guardado durante estas corridas; después de continuar aparecen dos consultas IR 08h 00h que
siguen devolviendo FFh FFh. No impiden cargar el mapa; IR/Pokéwalker no están implementados y no se cambian aquí.

Evidencia local en `build/hg/`: trazas completas, conversaciones resumidas, capturas y comprobación de todos los bytes.
Carga de la partida original, guardado desde el menú del juego y reapertura en otro proceso comprobados. Pendiente medir
en la consola el paquete exacto, repetir partida nueva y verificar otros juegos que inicialicen el dispositivo local.

## 2026-10-26 — Audio con solape y rodilla suave (host; pendiente PS3)
- Señales sintéticas con producción 25/50/75/100 % y transición 100→50→0→100: sin faltas; senoide de 997 Hz medida 997–999,33 Hz, salto máximo 0,06554 frente a 0,06521 original; copia bit a bit a 100 %. Barrido 200–13 400 Hz, ruido y estéreo en cuadratura incluidos; detalles locales en `build/audio/unit-final.log`.
- Métrica de unión: energía de la segunda diferencia (filtro de altas frecuencias, módulo `4 sin²(ω/2)`) en los ocho primeros pares del salto y ±8 del final del solape, dividida por la del interior. No mide audibilidad: senoide ≈0,99–1,00; ruido ≤1,13; sin impulso adicional en las uniones.
- Limitador, senoide de 1 kHz a 0 dBFS: THD (armónicos 2–15) 0→3,12228 %; H3 2,863 %, H5 1,226 %. Con amplitud 1,4 y recorte duro de referencia: THD 13,16084→12,64385 %, H5 2,784→1,559 %. Identidad binaria comprobada bajo −3,01 dBFS; techo 0,98 y monotonía comprobados. No elimina la distorsión previa ni garantiza menos THD para cualquier pico.
- Metroid, 900 cuadros en host: 721 073 pares, 15,02 s, pico previo al WAV +0,5 dBFS. Reproducción del WAV con ráfagas de 800 pares a 30 Hz: 718 560 añadidos, cero faltas, energía relativa de unión 1,10478; los silencios propios del juego se conservan. A 60 Hz: cero añadidos y comparación binaria exacta.
- Host, solo núcleo de extensión en esa reproducción: 15,58 µs/grano al 50 % y 0,53 µs al 100 %; no son tiempos PS3. Consumidor real con servicios simulados y ASan/UBSan: 256 000 pares exactos, nivel del control de razón idéntico, vueltas del anillo, déficit, pausa y reapertura correctos (`build/audio/platform-check.log`).
- Guion PS3 (una ronda): instalar «SalsaDS Audio» aparte; Metroid intro 30 s, menú 30 s y juego 60 s; MKDS título/demostración 60 s y carrera 60 s. Repetir con `audio-sin-estirar`; comparar los mismos pasajes, tono, clics y ritmo. Renombrar a `.off` y reiniciar la aplicación al cambiar interruptores; no borrar archivos.
- En pasajes fuertes, repetir con solo `audio-sin-limitador`: escuchar aspereza de transitorios y volumen, sin atribuir los huecos al limitador. Exigir faltas nuevas 0 durante juego, sin bloques tarde, sin audio residual en selector; comparar diferencias de µs acumulados/tiempo reproducido y máximo/grano, fps y mezcla SPE frente a la ejecución anterior. Salir con SELECT+START y luego O antes de recoger el log.
- Riesgos pendientes de escucha/medición real: repetición de ataques/voz en déficit extremo, coloración del solape y de la rodilla, costo del hilo PPE y latencia del grano (hasta 10 ms de datos pendientes). El SPE no cambia; holguras del paquete: 9056/178384/8432/17856/144032 B, todas ≥8192. No se envió nada a la consola.

## 2026-10-26 — Audio musical continuo, segunda ronda (audio-estirar)
- PS3, primera ronda (`build/ps3-audio/p1.log`): 0 faltas/silencio, pico −0,38 dBFS, 36 % activo, 2 409 120 pares añadidos y 2 338 194 µs de extensión; mejor sonido, pero coloración en acordes. Esta segunda ronda todavía no está medida en consola.
- WSOLA en el hilo de audio: grano 40 ms, salto 30 ms, solape 10 ms; `STRETCH2_HOP=960` permite 30/20/10 ms (ambos probados). Ventanas complementarias `w=3u²−2u³`, suma unidad; el audio conserva sus muestras, sin interpolación ni cambio del límite de deriva ±2 %.
- Producción estimada en ventanas de 100 ms, polo `r←r+0,4(p−r)` y realimentación de reserva; salto de análisis fraccionario. Búsqueda hasta 20 ms, dos polos de ≈750 Hz cada uno, decimación 8 y refinamiento a una muestra; error estéreo `Σ(dLP²+0,08d²)`. Reserva suficiente para alinear graves; ataques detectados también en la búsqueda se copian localmente 1:1 para no duplicarlos.
- Banco propio de acordes con armónicos y percusión (`tools/audio-quality.c/.py`), extensión exacta 25/50 %: flujo en empalmes −42,7/−24,6 %, CV medio por bandas −26,4/−21,7 %, nitidez +3,8/+16,5 %; ancho de ataque 7,5→7,33/7,20 ms. Empalmes en 20 s: 788→663 y 1189→664; salida al 100 % idéntica, sin faltas.
- Métricas: Hann 2048, paso 120; flujo = norma cuadrática de la diferencia de espectros normalizados en empalmes registrados, excluyendo ataques; CV de envolventes RMS en 80–250/250–500/500–1000/1000–4000 Hz. Ataques: Hann 512, banda >6 kHz, ancho a media potencia y pico/energía en ±50 ms. Son indicadores, no audibilidad; guardas ≥10 % de mejora musical, ancho ≤+10 %, nitidez ≥90 % de v1. Pasan: no se incorpora vocoder en esta ronda.
- Host, núcleo de extensión por 10 ms: v1→v2 7,56→10,94 µs (+25 % de duración), 11,72→10,99 µs (+50 %) y 0,90→0,56 µs nominal; no son tiempos PS3. Estéreo en cuadratura, senoides 73,416/997 Hz, barrido, ruido, transiciones y sanitizadores: 14 309 451 comprobaciones por configuración, 0 fallos; limitador intacto.
- Copia nominal y control cerrado del anillo: 3 067 904 pares comprobados sin extensión ni diferencias; pausa/reapertura y vueltas del anillo correctas. Reproducción de la captura local de Metroid al 50 % sin faltas; los silencios propios de la captura se conservan. SPE sin cambios: holguras 9056/178384/8432/17856/144032 B; mínimo 8192.
- PS3: mismo Metroid (intro 30 s, menú 30 s, juego 60 s) y MKDS (demo/carrera 60 s cada uno), repetir con `audio-estirar-v1`, reiniciando y renombrando a `.off`; conservar `audio-sin-estirar` y `audio-sin-limitador`. Escuchar agua/peine, tono grave, ataques dobles; exigir faltas/silencio 0, comparar uniones/s, µs, bloques tarde y fps. Guion: `build/audio2/guion-ps3.txt`; salir SELECT+START y O antes del log.
- Riesgos: coloración residual, percusión densa y déficit extremo; mayor reserva (precarga 85 frente a 43 ms) y ≈105 KiB adicionales en el PPE. `% tiempo estirando` puede subir por el control continuo: no equivale a muestras añadidas. La mejora perceptiva y el costo real requieren el A/B de consola; no se envió nada.

## 2026-10-26 — HD: diagnóstico detenido (host/RPCS3)
- Base PS3 recibida bcd37c1: raster pesado ≈4 ms y preparación PPE mediana 0,89 ms; no hay medición nueva de consola.
- RPCS3 ejecuta raster nativo y HD a 2×; sus tiempos por volcado están en `build/rsx3d-hd/`. No acreditan rendimiento PS3 ni p95 ≤16,7 ms.
- SPE→RSX falla dos veces con seis escenas; nativo y metadatos exactos, composición final distinta. No se miden ni promueven 3×/4×; HD bloqueado en juego.
- Sensibilidad teórica con raster nativo R≈4 ms: a 2×, doble pasada ≈8–20 ms según fracción de llenado, antes de composición/presentación. No permite afirmar 60 fps.
- Publicación experimental: copia PPE de 768 KiB de metadatos, sin devolver HD. No corrigió la discrepancia; coste real pendiente y ninguna ganancia atribuida.
