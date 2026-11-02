# Estado de SalsaDS

Bitácora de lo hecho, lo que quedó a medias y lo decidido. Lo más reciente
arriba. Las mediciones van en `perf-log.md`; los resultados de la Fase 0, en
`fase0.md`.

## 2026-11-02 — Ritmo con el 3D en el SPE: el flip anterior se espera después de emular (rama ritmo-spe)
- En la PS3 (rc2), con el 3D en el SPE, Sonic Rush Adventure se ve a 30 fps exactos (intervalo 33,3 ms con 16,3–18,1 ms de emulación) y Metroid pierde cuadros (p95 33,4 ms ya con 14–16 ms de emulación); con el 3D en el RSX y HD, el mismo Sonic va a unos 50 (19,4 ms).
- Causa: no es el 3D sino dónde espera el PPE el flip. `render_frame` llamaba a `rsxkit_frame_begin` (espera el flip del cuadro anterior) antes de emular. El flip ocurre en un VBlank, así que cada vuelta arrancaba justo después de uno; si el trabajo de la vuelta (emulación, recogida, dibujo, el sondeo de la espera) no cabía en el refresco, el flip salía en el VBlank siguiente al siguiente y el PPE esperaba el resto: `frame_begin` 13,8–16,1 ms en `DATO desglose`, emulación + `frame_begin` = 33,3. La HD del RSX espera el flip después de emular desde la ronda 11 (`8faacea`, `late_flip`): `frame_begin` 0,00 y el intervalo sigue a la emulación, también en las ventanas en que el selector mandó 112 de 120 cuadros al SPE (19,8 ms). La app RSX sin archivos (rc2), con `3d-rsx-1x` o con `3d-spe` tomaba la forma vieja.
- Arreglo: la espera del flip va después de emular en todas las rutas, como la HD. El PPE emula el cuadro N+1 mientras el flip de N espera su VBlank: un cuadro que se pasa sale en el refresco siguiente a terminarlo, y uno suelto de hasta casi dos refrescos no se pierde si los siguientes tienen holgura. Costo: con cuadros que caben, la imagen sale un refresco más tarde que con la forma anterior (como ya pasa con la HD). Antes de la espera nada se dibuja en la pantalla; la emulación, la entrega del 2D y el intercambio de pantallas no cambian.
- Interruptor `flip-temprano`: vuelve la forma anterior (antes de emular; la HD conserva su espera después de emular y `3d-rsx-hd-flip-temprano` la sigue adelantando).
- `DATO ritmo` por ventana: cuántos cuadros esperaron el flip después de emular; el intervalo en pantalla entre flips con el tiempo del sistema (`gcmGetLastFlipTime`, primera vez que se usa: si la consola diera otra unidad sale «fuera de rango»); y los VBlanks entre flips programados (1, 2, 3 o más, media y Hz de la tele). El «intervalo del RSX» de `DATO nucleo` es la vuelta del programa: con la espera después de emular puede ser 19 ms aunque la tele muestre cada cuadro 16,7 o 33,3.
- Paquetes «SalsaDS ritmo» (SLDS00074) y «SalsaDS ritmo RSX» (SLDS00075) en `build/ritmo-spe/`, pendientes de la PS3.

## 2026-11-02 — rc3: la app RSX dibuja el 3D en el RSX y en HD 2× por omisión
- Hasta la rc2, la app RSX solo usaba el RSX con el archivo `3d-rsx` y el HD con `3d-rsx-2x` y `3d-rsx-hd-juego`; sin ellos dibujaba el 3D en el SPE, igual que la normal. En la PS3, sin archivos, Sonic Rush Adventure va a 30 fps (ritmo 33,3 ms) y con el RSX a unos 50 (19,4 ms); Metroid pierde cuadros (p95 33,4 ms) sin el RSX y no con él (17,3 ms).
- Ahora, en la app RSX (`NUCLEO_RSX`), el 3D va al RSX y en HD 2× por omisión: `3d-spe` lo devuelve al SPE, `3d-rsx-1x` lo deja nativo y `3d-rsx-3x`/`3d-rsx-4x` lo suben. La app normal no cambia. El selector sigue mandando al SPE cada escena que no cabe o que el RSX no dibuja igual.
- Para la v0.0.5 la app RSX se publica como «SalsaDS» (SLDS00009) y la de solo SPE como «SalsaDS SPE» (SLDS00010).

## 2026-11-02 — rc2: bordes del RSX apagados por omisión
- En la PS3, los bordes del RSX (rama `rsx-bordes`) llevan HeartGold al RSX, pero en HD faltan contornos y la segunda ronda (`5a68129`) dibuja una diagonal en el techo de la casa de Pueblo Primavera; la preparación del PPE sube a 6,6 ms (ronda 1) y 12,2 ms (ronda 2) por cuadro y el RSX tarda 26–27 ms contra 14,6–16,7 del SPE.
- Por omisión, los cuadros con marcado de bordes van al SPE, como en la rc1; el archivo `3d-rsx-bordes` los lleva al RSX (sustituye a `3d-rsx-bordes-spe`). La rc2 lleva la ronda 1; la ronda 2 queda en su rama.
- Siguiente: comparar en la consola el mismo cuadro por el RSX y por el SPE y guardar las diferencias, y una pasada final en la GPU con el ID y la profundidad, sin preparación por dibujo en el PPE.

## 2026-11-02 — Unión de capacidad y bordes del RSX (rsx-lotes)

- Se conservan la capacidad ampliada y el marcado de bordes. Los códigos de OID/TID remapeados quedan en stencil; el alfa auxiliar de bordes mantiene el OID original y la arista geométrica. Las limpiezas por reciclado preceden a los lotes y se repiten en color, bordes, niebla y cada escala. Los interruptores `3d-rsx-capacidad-anterior` y `3d-rsx-bordes-spe` siguen siendo independientes.
- El censo del host combina ambos grupos de columnas sin duplicados y usa la capacidad nueva también con bordes y espaldas. Se conservan las autopruebas y degradaciones de ambas ramas; el banco portátil añade secuencias de OID compartido en stencil, reciclado y conservación de metadatos.
- Validación Docker: `test-host` completo con salida 0, 610 veredictos correctos; unitarias LE/ASan/UBSan 24713819 y BE/qemu 24717585 comprobaciones, cero fallos (sin los 34 históricos de `test_arec.c`). Incluye 224 escenas de capacidad, 168 de bordes, 32 hashes y 16 lockstep. Censo sintético: 792 escenas y 10288 comprobaciones correctas. Verificadores fp40: bordes 7272, W 1060 y niebla 15856 casos, sin fallos. Paquetes normal y RSX compilados para comprobación, sin entrega ni envío; informe en `build/rsx-lotes/fusion.md`. La paridad gráfica y los tiempos físicos del RSX siguen pendientes de consola.

## 2026-10-31 — Pantallas cruzadas un cuadro: el intercambio se lee del cuadro entregado (rama pantallas-swap)
- En la PS3 (v0.0.5-rc1, versión normal; el código es el mismo en la del RSX): cuadros sueltos con la pantalla de arriba abajo en la intro y el menú de Metroid Prime Hunters y en Sonic Rush Adventure (la intro y la aparición de «Press START»).
- Causa: con el 3D encendido el núcleo entrega el 2D del cuadro N en la línea 262 (o en la línea aplazada del N+1) y el programa lo presenta al terminar la pasada siguiente. Para entonces el cuadro N+2 ya cerró su línea 192 en la misma instantánea (son dos; `vid_wait` la reclama en la línea 0) y el programa leía ahí `powcnt1`: imagen del N con el intercambio del N+2. Mientras el juego alterna POWCNT1.15 cada cuadro (captura a doble pantalla) coinciden; cuando rompe la alternancia (cambio de escena, cuadro que el juego pierde, entrar o salir de un menú) salen cruzadas uno o dos cuadros.
- Evidencia: `salsads-cpurun --presentacion` sigue el orden de la consola con las llamadas del núcleo y guarda la imagen de cada cuadro. Intérprete: Metroid (intro, título y menús, 5200 cuadros) 41 cuadros cruzados con la lectura al presentar y 0 con el bit de la entrega; Sonic Rush Adventure (4000) 10 y 0. Recompilador en qemu con tramos de 512 (el de la consola): Sonic 15 en 3100 cuadros (el juego pierde más capturas que con el intérprete: cuadros 943, 1063, 1223 y 1680 contra 1149) y 0. melonDS (como programa, `tools/melonds-frames.py`) rompe la alternancia en el mismo cambio de escena de la intro de Metroid (0,0,0,0 y luego 1,0,1,1): es del juego. Log de la consola: menú de Metroid estable con 120 SWAP_BUFFERS de 120 y transiciones con 33–79.
- Arreglo: el programa guarda POWCNT1.15 al entregar (`sub.swap`; adaptado de `2942626` de la rama captura-doble, que no aplica limpio). Interruptor `pantallas-swap-anterior` (vuelve a leerlo al presentar). `DATO pantallas` cuenta por ventana los cuadros que la forma anterior habría cruzado. `test-host` comprueba el modelo con la demostración (Select intercambia).
- De captura-doble no entran las retenciones (`9b70d10`, `097d1b6`): atacan otro síntoma (una imagen vieja cuando el juego pierde una captura), y con el bit de la entrega la consola presenta exactamente la salida de cada cuadro de la DS; tampoco `f214c40` (costo de la captura). Sí `9dd89c5` (ciclos en macOS) y `tools/melonds-frames.py` de `c64ba48`. Paquetes «SalsaDS swap» (SLDS00061) y «SalsaDS swap RSX» (SLDS00062); falta verlo en la PS3.

## 2026-10-31 — Páginas físicas del JIT alineadas: Plants vs. Zombies ya no ejecuta un overlay viejo (rama arm9-rescate)
- Revisión de lo que la rama arm9-fps tenía fuera de main tras unir arm9-es. La DMA inmediata a GXFIFO, VRAMCNT_C/D sin vaciar el ARM7 y la E/S matemática directa ya están en main con otra forma; lo que les falta (una llamada menos por palabra a GXFIFO, la raíz perezosa, la caché del ARM7 de 2 MB con invalidación por página) queda por debajo del 3 % del cuadro según los logs de la PS3 de arm9-es (`0DC` ≈ 0,14 µs por palabra; el ARM7 con 2 vaciados en toda la sesión y 0 ms traduciendo). No se traen.
- Sí se trae la alineación: la ITCM sin alinear dejaba 112 bytes de relleno antes de la RAM (también con el compilador de la PS3), y el final de cada página de 16 KB de la RAM, la WRAM y la VRAM caía en la página física siguiente: una escritura por la ruta rápida no invalidaba el código. PvZ, tras Start Adventure: `jit-sin-codigo` PC=e59d600c y salto a 0xffff0000 (PS3 y qemu). Con la ITCM alineada a 128 y la comprobación al compilar llega a Level 1-1 (qemu, 6000 cuadros, 0 pasos sin código); `test_jit_overlay` pasa de 22 fallos a 0. Sin interruptor (es la disposición de `struct nds`): en la PS3 se compara con la v0.0.5-rc1.
- `test_arec`: los 34 fallos BE conocidos (149/152) eran la expectativa del episodio dirigido aplicada a otros programas; acotada a ese episodio, BE queda sin fallos.
- `test-host` en 0 (LE 24 233 605 y BE 24 237 371 comprobaciones, sin fallos); lockstep en qemu sin diferencias: NSMB, SM64DS, MKDS y Metroid 1500 cuadros (arranques) y PvZ 5200 cuadros hasta Level 1-1. Paquete «SalsaDS ARM9» (SLDS00045) pendiente de la PS3.

## 2026-10-31 — Líneas claras en la pista de Mario Kart DS: atributos de las aristas x-mayores de abajo con el tamaño completo (rama r3d-aristas-mkds)
- En la PS3 (v0.0.5-rc1) la recta de salida de MKDS mostraba líneas horizontales claras de 1 píxel sobre el asfalto; con `r3d-sin-completo` desaparecían. Causa, en el host (cuadro 4300 de la carrera, 914 polígonos, DISP3DCNT 0x001D: antialiasing sin edge marking, W-buffer): todos los polígonos van a tamaño completo y la izquierda x-mayor que va hacia la derecha (la arista de abajo) dibujaba el tramo de su línea con los atributos del final del tramo. El polígono 163, de (113,45)-(113,47)-(124,48)-(124,45), pintaba la línea 47 de x = 113 a 123 con un solo texel (el de x = 124) y w = 33289 (la del extremo cercano) en lugar de 37464: le ganaba la prueba W al 70, que comparte esa arista, y salía una fila uniforme (brillo 149 contra 84 sin el tamaño completo). Igual el 164/75 (línea 48), el 168/65 (línea 53, x 184-200) y el 34/35 (línea 70, x 11-23). La derecha x-mayor hacia la izquierda tenía el mismo defecto, y la regla dejaba además píxeles de más: +1 también en la derecha x-mayor, la derecha hacia adentro un píxel más larga y la última línea de una izquierda x-mayor con la derecha vertical.
- Regla corregida, medida contra melonDS ejecutándolo con las escenas 46 a 49 de `gx3d` (los polígonos de esa recta con una textura que codifica s y t; edge marking con id 0, antialiasing, polígonos chicos y ortográfica): las aristas x-mayores que van hacia adentro dibujan su tramo de la línea, la izquierda desde redondeo(X(y)) y la derecha hasta redondeo(X(y)) exclusivo (la mitad exacta hacia abajo), y el extremo nuevo lleva los atributos de la arista en esa x (con perspectiva: s, t y color por la x, w y z en la línea y); se aplica antes de juntar los tramos gordos. La derecha y-mayor inclinada dibuja su píxel salvo que la línea ya termine más a la derecha (tramo gordo de la izquierda, línea vacía); la x-mayor no crece. En la ortográfica no se aplica encima la regla 3.7 (la línea ya acaba en redondeo(X(y))). Condición del tamaño completo (GBATEK, «Polygon Size») sin cambios.
- Contra melonDS: escena 46 con la misma cobertura (la rc1 tenía 38 píxeles de más; sin el tamaño completo faltaban 511) y 797 píxeles distintos (rc1 1775), casi todos los de la textura en perspectiva que ya difieren con polígonos chicos (escena 48: 850); quedan 245 en los tramos compartidos donde melonDS deja ganar al otro polígono por la W (empate casi exacto, sin explicar). Escena 49 (ortográfica): 0 (rc1 867). MKDS en la misma escena (capa 3D de melonDS capturada en la recta de salida): 16081 → 12384 píxeles distintos y 34 → 10 tiras claras de 6 píxeles o más, las 10 también sin el tamaño completo (karts en otro lugar). Sin antialiasing en SalsaDS la fila compartida muestra entero al polígono que gana; en la DS se mezcla por cobertura.
- HeartGold sin regresión: intro sin botones, cuadros 2150-2355, huecos 10596 sin el tamaño completo → 2 (como la rc1); mapa de Pueblo Primavera (continuar la partida y caminar, 12001 cuadros) 217859 → 0 (rc1 0). El guion de 29 900 cuadros de la entrada del 26 ya no llega al mapa en main (la partida va al tutorial de partida nueva, que pide tocar la pantalla).
- Interruptor: archivo `r3d-aristas-rc1` en el directorio de datos (host `--r3d-aristas-rc1`, `R3D_F_FULLSTRETCH`) vuelve a la regla de la v0.0.5-rc1; con él `gx3d.golden` 0-45 sale idéntico. `r3d-sin-completo` sigue igual. Cambian 5 imágenes de `gx3d.golden` (28, 29, 30, 37 y 40, translúcidos con mezcla), todas más cerca de melonDS (408 → 126, 288 → 183, 21 → 0, 393 → 373, 3464 → 3266 píxeles); escenas 46 a 49 nuevas. Pruebas dirigidas en `test_r3d_vrow.c`; cambia el hash de la semilla 7 de `test_render3d.c`. La ruta del RSX no usa estas reglas (sus autopruebas son cuadriláteros alineados a los ejes).
- Local store del SPE: el código nuevo dejaba la pila de `raster3d` con 6736 B de holgura (mínimo 8192). Para recuperarla, sin cambiar resultados: `r3d_end_wz` toma la w y la z de `r3d_end_at` (las mismas cuentas) y, solo en el SPE, `r3d_lin` y `r3d_zlerp` van fuera de línea (`R3D_LS`). Cuesta unas llamadas y cinco interpolaciones más por línea de polígono: medir en la PS3 los ms del render 3D contra la rc1 en la misma escena.
- `test-host`: LE 0 fallos; BE solo los 34 conocidos de `test_arec.c`; `gx3d` 0-49 iguales al dorado en LE, BE y SAN; `--vs-ref3d` igual en HeartGold, MKDS, NSMB y SM64DS. Paquete «SalsaDS aristas» (`SLDS00083`, versión normal) con holgura de pila de `raster3d` 8400 B; la versión RSX también enlaza. Falta la PS3: líneas de MKDS en la recta de salida, HeartGold en Pueblo Primavera y la Torre Quemada, con y sin `r3d-aristas-rc1`, y el tiempo del render 3D.

## 2026-10-31 — Capacidad de IDs del RSX por cuadro y por intervalo (rsx-capacidad)

- Los IDs ya se remapeaban por cuadro; el tope procedía del stencil de 8 bits, no del alfa de color RGB6/A5. En el reparto ampliado, puerta, cara, máscara y ocupado dejaban cuatro bits compartidos entre OID y TID. El veto previo del contador translúcido (>8), con sombras, impedía incluso intentar ese reparto.
- Se conservan los repartos que ya cabían. Si no caben, se recupera el bit de máscara cuando no hay sombras y el de ocupado cuando el borrado tiene alfa. Si aún faltan códigos, `rsx_capacity.h` calcula los intervalos de primer/último uso en el orden efectivo del raster: un código solo se reutiliza después del último uso del ID anterior, precedido de una limpieza de stencil a pantalla completa. Conserva color, Z, OID, cara y máscara; se repite en atributos y HD. Los IDs simultáneos que no caben siguen en SPE con motivo propio.
- `3d-rsx-capacidad-anterior` en `/dev_hdd0/salsads/` restaura los topes rc1. Las pasadas anteriores y la niebla por polígono también conservan esos topes. La autoprueba incorpora 224 escenas Z/W, niebla, toon, sombras y transparencias, con controles rc1 y comparación RGB6/A5 exacta; cualquier diferencia nueva degrada la ampliación al SPE.
- Censo reproducible del host: MKDS, salida y una vuelta, 13 → 0 rechazos de capacidad en 4573 cuadros con 3D; SM64DS, recorrido hasta Bob-omb, 2332 → 0 en 29397 (interior del castillo 24 → 0; Bob-omb con niebla 256 → 0). No se equiparan a los 970 históricos, cuyo log no permite atribuir IDs por cuadro. Conteos y límites en `perf-log.md`.
- Validación: 14 553 comprobaciones dirigidas del plan sin fallos; unitarias LE/SAN 24 236 520 sin fallos y BE 24 240 255 con solo los 34 conocidos de `test_arec.c`. Todos los bloques de ROMs pasan: LE 189, SAN 165 y BE 253, incluidos 32 hashes y 16 lockstep. Tras dos fallos del compilador amd64 emulado se detuvo esa vía; el mismo GCC/opciones en la imagen local i386 completó BE, con método y logs en el informe. Paquete limpio «SalsaDS capacidad», `SLDS00071`, sin envío a consola.
- Sin cambios en shaders, raster SPE, CPU ni bibliotecas MIT. El rectángulo de limpieza publica sus vértices con `dcbf`/`sync` antes de que los lea el RSX. Para unir con cambios de bordes: conservar su admisión de modos y combinar únicamente el bloque de capacidad, los campos de comando y la limpieza previa al dibujo; no sustituir el archivo completo. La validación gráfica y el costo real quedan pendientes de PS3; informe, conteos y paquete de revisión en `build/rsx-capacidad/`.

## 2026-10-31 — Marcado de bordes en el RSX (rsx-bordes; acreditación gráfica pendiente)
- Nueva pasada de cuatro vecinos con OID real y profundidad de 24 bits. El stencil sigue resolviendo sombras y TID; el alfa RGB6/A5 del color se conserva. Un destino auxiliar guarda RGB=Z24 y A=OID6/arista del último opaco. Los translúcidos y las sombras conservan esos metadatos y solo reemplazan la profundidad con PA_TDEPTH. Una tabla de aristas por polígono/fila reproduce AT_EDGE de `r3d`: no se contornean agujeros interiores de textura ni intersecciones interiores.
- La pasada sustituye RGB con `EDGE_COLOR[OID>>3]` antes de la niebla; los vecinos exteriores usan CLEAR_COLOR/CLEAR_DEPTH, incluido OID 63. A 2×–4× se consultan vecinos separados un píxel nativo. La niebla conserva su raster de atributos independiente: color + bordes requieren dos rasterizados y un quad; con niebla son tres y dos. Sin cambios en las bibliotecas MIT ni en el rasterizador SPE.
- `3d-rsx-bordes-spe` restaura el respaldo. La autoprueba compara exactamente RGB6/A5 y degrada durante ese arranque solo el grupo que falle (Z/W, opacos/translúcidos, con/sin niebla), con motivo explícito en el log; incluye reducción HD. Se ejecuta con `autotest` o `3d-rsx-prueba`. Se conserva la limitación del AA del SPE: afecta cobertura, aún no mezcla alfa de aristas como el hardware DS.
- Host Docker: intro HeartGold [0,3000), 1043 cuadros con capa 3D, rechazos por modos 1031 → 0. Todos los cuadros activos son W+translúcidos+AA, sin niebla. Por escala: 296310 dibujos de color, 259581 de atributos y 1031 quads de bordes, más exportación. No incluye costo, caché de texturas ni degradación de consola. Pueblo Primavera no acreditado: la partida de solo lectura mostró un aviso de borrado; queda para la PS3.
- Unitarias Docker: LE/ASan/UBSan 24 709 612 comprobaciones, 0 fallos; BE/qemu 24 713 347, solo los 34 conocidos de `test_arec.c:149/152`. La primera BE alcanzó el límite de 600 s; repetida con 1800 s termina completa. Las 168 escenas y los bancos de bordes coinciden en ambos órdenes. Resto de `test-host`: ROMs LE/SAN/BE, gráficos, sonido, 32 hashes y 16 lockstep correctos. Verificadores fp40: bordes 7272 casos, W 1060 y niebla 15856, sin fallos.
- Resultados completos de la batería, paquete, SHA-256 y guion en `build/rsx-bordes/informe.md`. La paridad gráfica y el beneficio en PS3 siguen pendientes: no se ha enviado ni ejecutado el paquete en consola.

## 2026-10-29 — Niebla con toon en el RSX por omisión, acreditada en la PS3 (rsx-sombras)
- Autoprueba en la PS3 con el arreglo de TEXUNIT1: 1× y 2× sin interruptores «OK, 0 fallos, 0 grupos degradados» (niebla+toon como respaldo esperado); con `3d-rsx-niebla-toon`, 1× y 2× con niebla+toon y niebla+resaltado, Z y W, en el RSX: 0 distintos, 0 fuera de tolerancia. Sombras 38/39/42/43 iguales a la referencia.
- SM64DS (castillo, Bob-omb y título) a 2×: cuadros en HD 34,5 % → 54,5 % y cuadros del RSX 43 % → 62 % contra la ronda anterior; desaparecen las caídas «niebla combinada con toon o sombra» y «cara trasera W sin paridad». Quedan «3D ampliado: capacidad translúcida sin paridad», «sombras: capacidad de IDs excedida» y las de costo.
- Por eso la niebla con toon o resaltado va al RSX por omisión; `3d-rsx-niebla-toon-spe` la manda al SPE (el archivo `3d-rsx-niebla-toon` ya no se lee). La autoprueba sigue degradando al SPE solo el grupo que falle.

## 2026-10-29 — Niebla con toon o resaltado: la tabla equivocada en TEXUNIT1; al SPE por omisión con interruptor (rsx-sombras)
- Autoprueba en la PS3 (paquete «SalsaDS sombras», 2×): pasa todo salvo las cuatro de niebla+toon/resaltado (Z 27 840 y 27 744 píxeles fuera de tolerancia, W 48 960 y 48 768, peor 255/63), y con eso la autoprueba apagaba todo el 3D del RSX. Las imágenes y el log dan la causa sin hipótesis: en Z difieren justo los cuadrados **sin** bandera de niebla (los que la llevan caían en la entrada 31 de la tabla, niebla total, y tapaban el color de debajo); en W difieren todos (la profundidad quedaba por debajo del desplazamiento: sin niebla). El color de toon del RSX era (0,0,0) donde la referencia da (0,15,0) y (6,0,0) donde da (2,43,43): exactamente lo que produce `toon.fcg` si la tabla que muestrea es la de **niebla** (densidades en R, G = B = 0) en lugar de la de toon. En `rsx3d.c` el bloque de niebla sobrescribía `tex[1]` después del de toon, para todos los polígonos.
- Arreglo: `rsx_color_table()` (`rsx_modes.h`, portable, con prueba en `test_rsx_passes`) decide qué tabla ocupa TEXUNIT1 en el dibujo de color: la de toon para el modo 2, la de niebla solo en la ruta por fragmento anterior; la pasada final la lee en TEXUNIT2 y la de atributos no la lee.
- Por omisión la niebla con toon o resaltado vuelve al SPE (motivo «niebla con toon o resaltado (SPE por omisión)») hasta acreditar el arreglo en consola; `3d-rsx-niebla-toon` la lleva al RSX. La niebla con sombras sigue en el RSX (48/48 en consola). La autoprueba degrada al SPE solo el grupo que falle (niebla con sombras o con toon, `rsx3d_fog_degrade`) y el veredicto dice «OK con degradación», en vez de apagar todo el 3D del RSX.
- Autoprueba más honesta: niebla+toon con paso de tabla 1024 (densidad parcial en Z y W), profundidades 4000–32570 (antes desbordaban la Z de 24 bits a partir del cuadrado 42 de cada 48: los bloques cian del diff) y la misma z>>9 en W; guarda `rsx3d-niebla-toon-N-{rsx,ref,diff}.ppm` y anota los primeros cuatro cuadrados que fallan (RSX y referencia en RGB6/A5, z>>9, bandera, índice de toon y texel). Niebla+sombra también con densidad parcial en Z (índice 16) y en W (W ×1280; antes en Z era total y en W nula).
- Pendiente de consola: autoprueba 1× y 2× sin interruptores (debe dar 0 fallos y 0 degradaciones) y con `3d-rsx-niebla-toon`; SM64DS castillo y Bob-omb (niebla con sombras) y el título (toon) con el interruptor.

## 2026-10-29 — Sombras con cara trasera en W y niebla junto con toon o sombra en el RSX (rsx-sombras)
- Causa de que SM64DS cayera al SPE a resolución nativa (log de la ronda anterior, tramos 290–302 s y desde 342 s con `RSX 0 SPE 120`): dos respaldos de paridad, no el costo. (1) «sombras: cara trasera W sin paridad»: el juego usa W-buffer, sombras de stencil y opacos de espalda; la DS resuelve el empate frente/espalda con «menor o igual», y la W por fragmento del RSX (`floor(1/pos.w)·q.y`) no daba el mismo valor en dos programas distintos para la misma W: `1/pos.w` llega como w−ε (RCP) y `q.y` interpolada lleva su propio ε, así que un polígono caía a w−1 y otro no. (2) «niebla combinada con toon o sombra»: la tabla de niebla y la de toon compartían los mismos 128 bytes y la pasada de atributos solo sustituía `shade` por `fog_attr`, no `shadow` ni `toon`.
- W por fragmento en `wdepth.cgi`, la misma expresión en los ocho programas que la escriben: `1/pos.w` redondeado al entero y multiplicado por `2^wshift` entero (`q.w`), sin interpolantes con ε; `tools/rsx3d-wdepth-check.py` ejecuta el ensamblado fp40 de los ocho y exige el mismo valor (848 casos). Con eso se admiten las sombras con cara trasera en W (reparto ampliado, bit 6 = cara). `3d-rsx-w-truncada` vuelve a la W anterior (y conserva el respaldo); `3d-rsx-sombra-trasera-w-spe` solo el respaldo.
- Niebla final con toon, resaltado y sombras: tabla de niebla en su propia ranura (`FOG_OFF`), programas `fog_attr_shadow`/`fog_attr_shadow_w` (siempre translúcidos, también con alfa 31) y `fog_attr` para toon con `k.z = 0`. `3d-rsx-niebla-combinada-spe` restaura el respaldo; con `3d-rsx-niebla-anterior` sigue rechazándose (comparten TEXUNIT1).
- Las pasadas de sombras pasan a `rsx_passes.h` (`rsx_mask_passes`, `rsx_shadow_passes`, `rsx_shadow_color_passes`, reparto `rsx_shadow_layout_*`), misma secuencia que el PPE mandaba a mano; `draw_shadow_extended` queda solo para `3d-rsx-pasadas-ronda3`. Banco del host `test_rsx_passes`: 524 288 escenas de hasta cinco polígonos (opaco de frente o espalda, máscara, sombra con uno o dos IDs, translúcido del mismo ID, alfa 31, PA_TDEPTH, bandera de niebla, borrado con o sin alfa, mezcla, prueba alfa; tres repartos) contra las reglas escalares de `r3d_px.h`, con y sin atributos de niebla, 0 diferencias.
- Autoprueba de la PS3: las sombras 36–51 (cara trasera con W) ya se comparan con la referencia en vez de esperar el respaldo; 48 escenas nuevas de niebla+sombra (Z/W, cara trasera, una y varias sombras, PA_TDEPTH, alfa 31, ID repetido, borrado con niebla) y 4 de niebla+toon/resaltado (512 cuadrados texturados, Z y W); respaldos 5 y 8 exigen la imagen exacta. Pendiente de ejecutar en consola: aquí no hay RSX.
- El programa `tests/phase4/rsx3d` no compilaba desde la unión del sonido (faltaban `stretch.c`, `wavrec.c` y las aristas de pila del SPE de audio): Makefile al día. Informe, interruptores y guion en `build/rsx-sombras/informe.md`.

## 2026-10-29 — rsxkit unificada con Salsa64 (rama rsxkit-unificar)
- `lib/rsxkit` vuelve a ser la misma biblioteca que la de Salsa64, byte a byte (las copias habían divergido desde el subtree aece830). Entra lo de Salsa64 (fuente del sistema, pasadas con malla, `rsxkit_depth_load`, `rsxkit_unmap_main`, lectura 1:1 exacta, tijeras recortadas, seguro ante un RSX que no avanza, video con los valores del XMB) y se conserva lo nuestro (stencil, programa del usuario con etiqueta de uso, COLOR0 auxiliar, constantes incrementales, mezclas MAX/MIN, `RSXKIT_ARGB8888`, `out_target`). Donde chocaban queda una sola forma: destinos con alfa por `rsxkit_target_create_ex` (`rsxkit_target_create_rgba` es un inline), alfa del dibujo por `dst_alpha`, `RSXKIT_CLEAR_ALPHA` o destino con alfa en `rsxkit_clear`, blit con un solo fragment program.
- Adaptación: `rsx3d.c` pide `dst_alpha = RSXKIT_DSTA_FRAG` en cada dibujo (sin eso rsxkit dejaría alfa 1 y se perderían los IDs y el RGB6/A5 del alfa). El Makefile de `tests/phase4/rsx3d` se pone al día con el del núcleo (`stretch.c`, `wavrec.c`, llamadas indirectas del SPE de sonido): el paquete con RSX no compilaba desde la ronda de sonido.
- Lo que sí cambia de comportamiento sin tocar código: la lectura 1:1 de un destino (`rsxkit_target_read` de los bloques 8×8 y 1×1 de HD) va con el filtro más cercano, no lineal; esperas al RSX con tope; tijeras vacías sin mandar.
- Verificado: `test-host` LE 23 781 491 comprobaciones, 0 fallos; BE solo los 34 conocidos de `test_arec.c`; paquete `build/salsads-rsxkit/salsads-rsxkit.pkg`, «SalsaDS rsxkit», `SLDS00044`, SHA-256 `c3b783289397e1124ed73f07e89b8336791db9aabb47622029601ffbe2b8449f`; holguras SPE sobre el mínimo (la menor 8368). Falta la PS3: autoprueba RSX, Metroid/MKDS a 2× y botón PS; guion e informe en el árbol de Salsa64 (`build/rsxkit-unificar/`). Sin envío a consola.

## 2026-10-29 — E/S lenta del ARM9: DMA por bloques, E/S directa, matemáticas por palabra, VRAMCNT sin vaciar el ARM7 (rama arm9-es)
- Desglose de la ruta lenta con los logs de la PS3 (Metroid de la intro a la zona pesada, SM64DS; `DATO jit-lentos-es`, 259 ventanas): en la zona pesada de Metroid la ruta lenta cuesta 6,4–8,1 ms de los 20–24 ms del cuadro impar; DMA3CNT (`0DC`) 3,8–5,9 ms (245–395 arranques × 13–15 µs: casi todos DMA en modo inmediato hacia GXFIFO de 72 palabras, más rellenos y copias de 1 KB a OAM/VRAM), matemáticas (`280`–`2BC`) 1,3–1,6 ms, ROMCTRL hasta 1 ms en los menús, VRAMCNT 161 µs por escritura (0,5–0,9 ms por cuadro en la intro: cada cambio de C/D vaciaba la caché del recompilador del ARM7, 5 662 veces en 126 s). SM64DS tramo lento: `0DC` 3,0–3,4 ms (133–152 × 22 µs), matemáticas 0,6–0,8, IME 0,1–0,5. Tabla y techos en `fase4.md`, «Quinta ronda».
- Cambios, cada uno con interruptor (archivo en el directorio de datos; en el host la opción del mismo nombre): **DMA por bloques** (`sched.c` `dma_block`, `mem.c` `nds_dma_span`): tramos contiguos del host con `memcpy`/relleno para RAM, WRAM, VRAM de un banco, LCDC, paleta y OAM (con su registro de cambios por media palabra), `DMAxFILL` como relleno, GXFIFO en modo inmediato palabra a palabra desde el puntero (`nds_gx_fifo_write_n`); avisos de código por líneas de 64 B; lo que no cumple sigue unidad a unidad (`dma-lento`). **VRAMCNT_C/D** vacía el ARM7 solo si el banco entra o sale de su mapa (`vram7-vaciar-siempre`). **Matemáticas** por imagen de 8 bytes y división perezosa, **ROMCTRL** entero directo, **E/S directa del recompilador** (`arec_cfg.io_rd/io_wr`: sin `mem_rd32`/bus/`nds_bus_read` para 0x04000000–0x04000FFF fuera de las TCM) (`es-lenta`). Traza `--dbg 256` (NDS_DBG_DMA) y `DATO dma` / línea «DMA» de `--stats`.
- Exactitud: `--state-hash` igual con y sin las banderas en Metroid, SM64DS, NSMB, MKDS, HeartGold, White 2 y PvZ (2 400 cuadros); prueba `test_dma` (34 transferencias, 4 000 juegos de operandos de matemáticas, ROMCTRL) y el vaciado del ARM7 en `test_vram7`; `--lockstep` deja la referencia con las formas anteriores. Techo con lo medido: cuadro pesado de Metroid −1 a −2,5 ms, SM64DS lento −1,5 a −2 ms; lo que quede en `0DC` es `gx_fifo_write` (geometría), no E/S. Falta medir en la PS3 (paquete «SalsaDS ARM9», SLDS00042).
- Arreglado: la marca de seguridad del recompilador (`nucleo-jit.marker`) quedaba puesta al salir de la aplicación sin haber abierto ningún juego (desde el selector, o la autoprueba al terminar), y el siguiente arranque de esa versión usaba el intérprete; la salida ordenada la quita siempre.
- En la PS3 (paquete «SalsaDS ARM9»): Metroid, cuadros con reloj de perfil, ARM9 7,04 → 4,98 ms, ARM7 1,52 → 0,85 ms y accesos lentos 3,63 → 2,76 ms (peor ventana 8,1 → 5,2); SM64DS con 5 ventanas de más de 17 ms contra 10 y accesos lentos 2,83 → 2,13 ms; NSMB estable a 16,7 ms. El intervalo de Metroid no mejoró en esa corrida (escenas más pesadas; en las ventanas pesadas los cuadros sin reloj de perfil tardan más que los medidos, posible alternancia de cuadros pesados y ligeros): pendiente de medir la misma escena.


## 2026-10-28 — Referencia melonDS y espejo interpolado de SOUNDCAP (sonido-desmume)
- Referencia digital obtenida: `tools/melonds-grabar.py` graba lo que melonDS manda a la tarjeta con el controlador de audio «disk» de su SDL (`SDL_AUDIODRIVER=disk`, 48 kHz PCM16, ritmo real) y pulsa botones sin teclado sustituyendo por el stub de GDB la `ldrh` que lee KEYINPUT (`mvn rd, #teclas`); en NSMB son dos lecturas, en `0x02005B40` y `0x02044664` (tras descomprimir el binario ARM9). Configuración de melonDS respaldada y restaurada.
- NSMB 1-1, mismo guardado y misma secuencia de botones, tramo de 22 s de música alineado por envolventes: melonDS con interpolación **ninguna** y **cúbica** son iguales entre sí (±0,3 dB en todas las bandas): su interpolación tampoco actúa sobre lo que pasa por SOUNDCAP. Frente a nuestro nativo, melonDS queda a −0,5…−2,2 dB en 4–12,7 kHz y −3,3 dB en 12–16 kHz, con picos de |Δ²| mucho más escasos (48/282 por segundo frente a 276/387 de nuestra salida FIR). Conclusión: la aspereza no es un error de temporización ni de formato nuestro; es el comportamiento de la mezcla sin interpolar grabada en la captura, que la referencia reproduce igual. Datos en `perf-log.md`.
- Mitigación: espejo interpolado de la captura (`snd_state.hdcap`, 16 KB). Por cada muestra PCM que SOUNDCAP escribe, se guarda la mezcla de las voces HD en ese instante; las voces 1/3 que reproducen ese anillo (misma dirección, tamaño y formato) la oyen en lugar del PCM. Captura, RAM, muestras nativas y estado fieles siguen idénticos (comprobado en NSMB y en la prueba unitaria `test_hd_capture_mirror`). Banco del seno por captura: imagen −23,89 → −64,44 dBc, igual que la voz directa. En NSMB: 8–12 kHz −0,9 dB y 12–16 kHz −3,1 dB respecto al HD anterior, picos de |Δ²| 218/444 → 42/292 por segundo; RMS −2,4/−1,3 dB (L/R) por la respuesta del interpolador frente al escalón. Interruptor de vuelta atrás `/dev_hdd0/salsads/audio-hd-sin-captura` (host `--audio-hd-sin-captura`). Falta oírlo y medir su costo en la PS3 (paquete «SalsaDS sonido», `salsads-sonido-3`).

## 2026-10-28 — Límite de HD al reproducir SOUNDCAP (sonido-desmume)
- Banco propio, host LE en Docker, sin ROM: PCM16 a 7998,56 Hz, seno de 499,91 Hz. HD reduce la imagen de 7498,65 Hz de −23,50 a −64,04 dBc en salida directa, pero solo de −23,50 a −23,89 dBc al reproducir la captura por la voz 1. Las muestras nativas son idénticas con/sin HD en ambos recorridos. Hermite interpola la captura ya mezclada; no elimina las imágenes que entraron desde sus voces de origen.
- No se acredita equivalencia completa de los WAV aportados de PS3 y el tramo host del nivel: correlación mono 0,0057 al buscar un desfase fijo; sí hay un tramo común de unos 8 s con el comienzo del WAV host completo (0,8951; bandas a menos de 0,10 dB). No usar el residuo de pasajes sin alinear como medida de ruido. Método y límites en `perf-log.md`; informe local `build/sonido/informe-2.md`.
- El log PS3 aportado del núcleo nuevo acredita la autoprueba de ocho trabajos con 0 diferencias de muestras, estado, memoria y salida. `audio-verificar` estaba apagado durante el juego. Su banco de 16 voces da 1,512/4,129 ms por trabajo fiel/HD; son datos de ese arranque, no el costo de una mitigación nueva.
- Referencia digital melonDS pendiente: el ejecutable no pudo conectar con los servicios gráficos del entorno; el paquete no ofrece plataforma sin ventana. No se obtuvo audio de referencia ni se consultó su código. No se concluye que la aspereza sea del hardware; tampoco se cambia el núcleo ni se genera otro paquete sin esa comparación.
- `test-host=1` solo por los 34 fallos BE conocidos de `test_arec.c:149/152`: LE con sanitizadores 22 852 825 comprobaciones sin fallos; BE 22 856 555. ROMs, escenas e imágenes LE/SAN/BE, 32 hashes y lockstep correctos. Texto publicable y formato correctos.

## 2026-10-28 — Voces y captura a partir de DeSmuME (sonido-desmume)
- Núcleo portable nuevo, con origen y avisos GPL-2.0-or-later: avance PCM/ADPCM, predictor e índice de bucle, PSG/ruido, redondeo de volumen/paneo, maestro antes del recorte y captura con temporizador/FIFO. Las escrituras SOUNDCAP pendientes se reenvían a las voces; ya no cambian la realimentación al partir el trabajo.
- NSMB 1-1, host, 20,032 s: concentración de saltos por cuadro 2,896→1,543; corrigiendo solo captura, 1,452 (correlación 0,999895). SOUNDCNT `0xB97F`: toda la salida pasa por las voces PCM16 1/3 de captura. Se identifica un defecto periódico; la energía en banda alta varía poco y no queda demostrado que desaparezca todo el ruido audible. Datos y límites en `perf-log.md`.
- `/dev_hdd0/salsads/audio-nucleo-anterior` devuelve íntegro el núcleo anterior: 2 734 607 pares de NSMB idénticos al WAV previo. Se conservan HD/Hermite, FIR, estirado, limitador, ganancia y sus interruptores.
- Pruebas dirigidas: 71 179 comprobaciones LE con sanitizadores y BE, 0 fallos; incluyen bucles ADPCM, PCM8/16, ocho duties, LFSR, captura a tres períodos, recarga durante captura, fuentes 0+1/2+3 y eco independiente de la partición. Autopruebas PPE/SPE ampliadas a ocho trabajos y ambos núcleos.
- `test-host=1` únicamente por los 34 fallos BE conocidos de `test_arec.c:149/152`: LE 22 852 817 comprobaciones sin fallos; BE 22 856 547; ROMs LE/SAN/BE, imágenes, hashes y lockstep correctos. La recarga final de captura se comprobó después con las 71 179 dirigidas LE/SAN y BE y conserva exactamente el WAV nativo de NSMB medido. Texto publicable, SPDX y formato correctos.
- Paquete limpio `build/salsads-sonido-final/salsads-sonido-final.pkg`, «SalsaDS sonido», `SLDS00033`; SHA-256 `9ee15eb17c614d82b1d5f213071f2b6551b536b5bb8d5a6572e345513da565b3`. Holgura SPE audio 165 600 B, mínimo del conjunto 8432 B; presupuesto actualizado. Git no permite crear `index.lock`: entrega en `build/sonido/cambios.patch`, mensaje en `commit.txt` e informe en `informe.md`.
- La ejecución de RPCS3 queda pendiente: el entorno de pruebas no permite abrir para escritura su candado compartido. No se declara paridad SPE/PPE ejecutada en esta revisión. La consola debe registrar 0 diferencias antes de comparar escucha y costo; no se ha enviado ningún archivo.

## 2026-10-27 — Paridad del audio y diagnóstico de la salida (audio-estirar)
- Las dos diferencias de la autoprueba eran `hd_last`: voz 1 en el trabajo 2 y voz 0 en el 3, separadas por una ULP debido a la contracción FMA PPE/SPU. El ancla persistente del antichasquido se guarda como muestra entera sin cuantizar la salida; la prueba aislada queda en 0 diferencias de muestras, estado, memoria y salida.
- La fase del FIR pasa a Q0.32 y el ajuste de deriva sigue limitado a ±0,25 %. La autoprueba registra, si falla, voz, posición/fracción, ADPCM, ruido, línea, SOUNDCAP, fases, historia y envolvente; `audio-mezcla-ppe` permite la pasada A/B sin desactivar la comprobación.
- `audio-sin-hd`, `audio-sin-remuestrear` (lineal), `audio-sin-estirar`, `audio-sin-limitador` y `audio-sin-ganancia` aíslan la cadena. `tools/audio-stage-compare.py` alinea WAV por correlación y mide saltos por fase, modulación y espectro; falta la captura PS3 homogénea para atribuir el ruido restante.
- A velocidad nominal y con al menos 20 ms sanos, WSOLA copia exactamente y no une. `audio-estirar-largo` ofrece para producción ≈0,5 grano de 80 ms, salto de 60 ms y búsqueda de 40 ms; queda apagado por omisión.
- Banco host a producción 0,5: 664→332 uniones, flujo espectral ×0,7762, modulación de envolvente ×0,8455 y nitidez ×0,9393; al 100 %, salida idéntica y 0 uniones. Son comparaciones relativas del host, no tiempos de la PS3.
- `test-host`: LE 22 816 259 (0 fallos); BE 22 819 989 (solo los 34 conocidos); SAN, ROMs, escenas, hashes y lockstep correctos. Holgura del SPE de audio 171 072 B; medición PS3 y atribución de la etapa ruidosa pendientes.


## 2026-10-26 — Verificación en juego de la mezcla SPE/PPE (audio-estirar)
- `audio-verificar` hace que cada trabajo mezcle en el SPE y en el PPE la misma instantánea de RAM y el mismo estado inicial; compara muestras nativas, salida flotante, las 16 voces, las dos capturas y la memoria escrita por SOUNDCAP.
- El SPE contrasta además cada línea de 128 B que lee con la RAM viva y cuenta las que cambiaron durante el trabajo. La captura se hace en memoria privada y se aplica al recoger el trabajo, solo en sus bytes modificados; así se separan la carrera de lectura y el RMW alineado de SOUNDCAP.
- La primera diferencia y después una cada 120 trabajos distintos registra cuadro, voz/formato, muestra SPE/PPE, estado antes/después, capturas y cambio de RAM. Cada ventana da diferencias, lecturas, desbordes y costo de copia/PPE/comparación; apagado no añade copias ni trabajo de mezcla.
- Captura PS3 nativa: pico/media de segunda diferencia L/R 2,785/2,583, con H6/H9 a +15,72/+13,40 dB sobre vecinos. Alineada por envolvente, PS3 2–8 s contra host 4,1764–10,1764 s: correlación 0,98748; pico/media 19,990/30,620 frente a 2,601/3,333. No hay paridad directa porque el host está después del FIR a 48 kHz.
- `test-host`: LE 22 430 253 (0 fallos); BE 22 433 983 (solo los 34 conocidos de `test_arec.c`); SAN, ROMs, imágenes, hashes y lockstep correctos. Holgura mínima 8432 B; audio 171936 B; verificadores de publicación/SPDX en 0.
- RPCS3 llegó una vez a la autoprueba ampliada: muestras, memoria capturada y salida iguales; la fase flotante requería tolerancia de 0,0005 muestras. Dos corridas terminaron antes en una instrucción ilegal de `r3d-1`; validación final del estado y medición de costo quedan para la PS3.
- Paquete `build/salsads-audio-6.pkg`, «SalsaDS Audio», `SLDS00033`; SHA-256 `9e64653bbb163d6df93fe28d1fe121f46176fbfc5961e275b631ea9c5d900263`. Sin envío a consola.

## 2026-10-26 — Frontera del audio y velocidad nominal (audio-estirar)
- `b073287`: el tic por cuadro era la línea de 128 B de las voces 1/3 que reproducen SOUNDCAP: se descartaba al acabar cada trabajo y saltaba a la instantánea de RAM siguiente. Ahora se conserva hasta cambiar SAD, arrancar o volver al bucle; `audio-frontera-antigua` restaura el fallo para A/B.
- `--wav-native` reproduce la partición PS3 (547/548 muestras) y `tools/audio-frame-metric.py` mide segunda diferencia, fase y armónicos. En NSMB host: pico/media L/R 1,961/2,527 → 1,240/1,196; el residuo antiguo contiene H1–H12 de 59,826 Hz entre 14,0 y 20,8 dB sobre los vecinos.
- A velocidad completa, una reserva real de 50 ms ya no entra en WSOLA por la oscilación de las entregas; 600 bloques con ráfagas 799/800 dieron identidad, 0 uniones y 1000/1000. La deriva de relojes pasa al remuestreo, limitada a ±0,25 %.
- Las 20 muestras recortadas de la captura PS3 son fieles: la DS suma y limita a 16 bits, aplica volumen maestro y recorta su DAC de 10 bits; no se redujo la ganancia.
- `test-host`: LE 22 430 253 (0 fallos); BE 22 433 983 (solo los 34 conocidos de `test_arec.c`); SAN, ROMs, imágenes, hashes y lockstep correctos.
- Autoprueba RPCS3 interpretada: audio SPE/PPE, 4 trabajos fiel/HD, 0 diferencias de muestras, estado, captura y salida flotante; el resultado global falla después en composición 2D, fuera de este cambio. Paquete limpio `build/salsads-audio-5.pkg`, «SalsaDS Audio», `SLDS00033`, holgura mínima 8432 B; SHA-256 `7fcc5f3301162f74683cf906d8681d19fca22fb2990b3d7d8305f0bf739d714a`; verificadores en 0. Sin envío a consola.

## 2026-10-26 — Grabación digital del audio, ganancia y métricas del limitador
- Para el ruido de la PS3 que el micrófono no permite aislar: con el archivo vacío `/dev_hdd0/salsads/audio-grabar` al abrir un juego se graban 20 s a partir de los 5 s de juego en dos puntos.
  `grab-mezcla.wav`: la mezcla nativa que devuelve el SPE (`nat`: enteros de 16 bits, 2 canales, 32 728 Hz), antes del FIR/Hermite, el estiramiento, el remuestreo, la ganancia y el limitador; 2,6 MB.
  `grab-salida.wav`: lo último que se escribe en los bloques del puerto (flotante de 32 bits, 2 canales, 48 kHz; el puerto es `AUDIO_PORT_2CH`), después de estiramiento, ganancia y limitador; 7,7 MB.
- Memoria reservada al abrir el juego (10,3 MB, liberada al terminar); el hilo de audio solo copia bloques; los archivos los escribe el hilo de emulación a trozos de 32 KB por cuadro (o de golpe al cerrar el juego). Los WAV se codifican en little-endian aunque la PS3 sea big-endian (`src/audio/wavrec.c`, prueba en `tests/unit/test_wavrec.c`). Una línea en `nucleo.log` da rutas, segundos, muestras perdidas (anillo), de silencio, faltas y bloques tarde.
- `/dev_hdd0/salsads/audio-ganancia`: un número en dB (por ejemplo `-6`), aplicado antes del limitador; sin archivo, 0 dB. La línea `DATO audio` suma ganancia, porcentaje de muestras donde el limitador actúa (sobre la rodilla de −3,01 dBFS) y su reducción máxima y media en dB.
- `test-host`: LE 22 429 045 (0 fallos); BE solo los 34 fallos conocidos de `test_arec.c`; ROMs BE, hashes y lockstep correctos (el GCC ppc64 cayó dos veces con `cpurun` y compiló solo a la tercera, igual que antes). Holgura SPE mínima 8432 B; verificadores en 0. Paquete limpio del código `b62e4ef`: `build/salsads-audio-4.pkg`, «SalsaDS Audio», `SLDS00033`; SHA-256 `61356da8c53f2898344759032bfffabea8729c0b6a4509c611a8e8913c9d5305`. Sin envío a consola.
- Nivel de la mezcla contra la DS (GBATEK, leído en el código de `snd_mix.c`): cada canal sale como PCM de 16 bits × volumen/128 ÷ divisor (1, 2, 4, 16), con balance L×(128−pan)/128 y R×pan/128 (centrado, −6 dB por lado); el mezclador suma y limita a 16 bits, el volumen maestro multiplica por mv/128 y el DAC toma 10 bits: (L>>6) + 0x200 recortado a 0–0x3FF. Un 16 bits a escala completa es ±512 sobre el sesgo, o sea exactamente ±1,0 (0 dBFS): por diseño la DS tiene 0 dB de margen y recorta en el DAC; un canal centrado a todo volumen queda en −6 dBFS. La mezcla nativa nunca pasa de 0 dBFS, así que los +2/+3 dBFS de v0.0.4 (sin limitador) vendrían del FIR/remuestreo sobre una mezcla recortada o de la ruta HD, que no recorta; hipótesis a confirmar con `grab-mezcla.wav` contra `grab-salida.wav`.

## 2026-10-26 — Audio HD opcional (audio-estirar)
- `9948780`: Hermite causal por voz PCM/ADPCM a 48 kHz, reloj racional, mezcla flotante y rampas de 32 pares; PSG/ruido sin interpolar, limitador existente al final. `audio-hd` apagado por omisión; SOUNDCAP y lectura del juego siguen fieles.
- PCM16, Fs 8/16/22 kHz: SINAD 19,65/22,59/18,90 → 62,76/62,75/62,76 dB; espurio superior −23,51/−29,53/−23,47 → ≈−64 dBc; THD2–10 0,033/0,214/0,497 → 0,00216 %. Silencio cero exacto; método, SNR y cuantización en `perf-log.md`.
- Apagado idéntico a `f1d353b`: 3 465 666 pares (16/10 bits); captura HD LE/BE idéntica al hash de referencia. Dirigidas SAN: 34 610 comprobaciones, 0 fallos; división de bloques, deriva ±2 %, bucles, reinicios y paradas.
- `test-host=1` únicamente por los 34 fallos BE conocidos (`test_arec.c:149/152`); LE/SAN, ROMs, hashes y lockstep correctos. Unitarias finales LE 22 429 009 (0 fallos), BE 22 432 739 (solo esos 34); verificadores de publicación/SPDX en 0. GCC cruzado cayó una vez; único reintento correcto.
- Build y paquete limpios; holgura SPE mínima 8432 B, audio 173392 B. `build/salsads-audio-3.pkg`, «SalsaDS Audio», `SLDS00033`; SHA-256 `b9e15d513ea61c2518430a66713b77264d0c7b7c515aee3b12400a6ce50a25ea`.
- Host, 16 voces: 0,0542 → 0,1328 ms/cuadro; costo SPE pendiente (banco de 120 cuadros al arrancar, referencia histórica en juego 0,31 ms). Guion Metroid/MKDS en `build/audio3/guion-ps3.txt`; riesgos: coloración cerca de Nyquist y ruido de origen. Sin envío a consola.

## 2026-10-26 — Audio musical, segunda ronda (audio-estirar)
- `402ced7`: WSOLA continuo de 40 ms, alineación filtrada en graves y protección de ataques; ajustable a 30 ms. `audio-estirar-v1` recupera la primera ronda; se conservan `audio-sin-estirar` y `audio-sin-limitador`.
- Host, acordes y percusión estirados 25/50 %: flujo en empalmes −42,7/−24,6 %, modulación por bandas −26,4/−21,7 %, nitidez +3,8/+16,5 %. Copia nominal exacta; no hizo falta vocoder para superar las guardas del banco.
- `test-host`: LE/SAN, ROMs, hashes y lockstep correctos; solo 34 fallos BE conocidos de `test_arec.c:149/152`. Unitarias finales: LE 22 394 443 (0 fallos), BE 22 398 173 (solo esos 34); audio 30/40 ms, sanitizadores, control cerrado y Metroid al 50 % sin faltas.
- Paquete limpio del código `402ced7`: `build/salsads-audio-2.pkg`, «SalsaDS Audio», `SLDS00033` verificado en PARAM.SFO; SHA-256 `f5d36761919685e01a5cc379b3ef1ed1a8f33f54a4ae7fbe933781d2ff222086`. Holgura SPE mínima 8432 B (audio 178384); verificadores de publicación/SPDX en 0.
- Pendiente PS3: mismos pasajes de Metroid y MKDS con/sin `audio-estirar-v1`; guion en `build/audio2/guion-ps3.txt`, métricas y método en `perf-log.md`. Riesgos: coloración residual, percusión densa y mayor reserva/latencia; costo y escucha reales pendientes. Sin envío a consola.

## 2026-10-26 — Audio continuo sin bajar el tono (audio-estirar)
- `75c81df`: WSOLA en el hilo de audio: 15 ms, solape de 5 ms y búsqueda estéreo; copia exacta sin déficit, deriva ±2 % intacta. Interruptores `audio-sin-estirar` y `audio-sin-limitador`; contadores de extensión, faltas y costo.
- Rodilla tras el FIR a −3,01 dBFS, techo 0,98; identidad bajo el umbral, sin envolvente. THD a 0 dBFS 3,12 %; con amplitud 1,4 baja 13,16→12,64 % frente al recorte duro. Mezcla/captura nativa sin cambios.
- Dirigidas LE/SAN y BE: 4 473 003 comprobaciones, 0 fallos; consumidor con servicios simulados: 256 000 pares exactos, control de razón, vueltas del anillo y reapertura correctos. Metroid host al 50 %: cero faltas; al 100 %: identidad binaria.
- `test-host=1` solo por los 34 fallos BE conocidos de `test_arec.c:149/152`; LE/SAN, ROMs BE, hashes y lockstep correctos. Compilación general y paquete limpio en 0; holgura mínima SPE 8432 B, audio 178384 B.
- Paquete `build/salsads-audio.pkg` del código `75c81df`, «SalsaDS Audio», `SLDS00033` verificado en PARAM.SFO; SHA-256 `0508e6595c4db32991170f3474c5cbc6b849a7f0516ecf03547e4e931c479e0f`. Verificadores de publicación y SPDX en 0; evidencia y guion en `build/audio/`.
- Pendiente PS3: Metroid intro/menú/juego y MKDS, comparar tono, clics, aspereza, costo y fps. Riesgos: repetición/coloración en déficit extremo y armónicos de la rodilla; sin envío a consola.

## 2026-10-28 — HeartGold: inicialización local Wi-Fi antes de CONTINUE (guardado-v005)
- Reproducido el aviso de comunicación con la partida nueva de 512 KiB. Las primeras 4980 conversaciones AUXSPI son idénticas con y sin el arreglo; todas las lecturas coinciden con el archivo. RDSR=00h, sin RDID ni escrituras antes del aviso.
- Causa: el firmware generado devuelve longitud Wi-Fi cero en 002Ch; al corregirla, el diagnóstico local encuentra que las escrituras a registros/RAM Wi-Fi no se conservan. Bloque sintético de 0138h bytes con CRC y dispositivo local de 8 KiB de RAM, registros con máscaras y BB permiten pasar a CONTINUE y al mapa.
- Sin cambios a AUXSPI, Flash ni ID. Las tablas BB/RF son ceros propios; sin firmware de una consola ni código copiado de otro emulador. No se implementan conexión inalámbrica, temporizadores MAC ni Pokéwalker. Después de continuar hay dos sondeos IR 08h sin emular que no impiden esta carga.
- A/B: archivo `wifi-sin-inicio` en el directorio de datos, leído al arrancar; host `--wifi-sin-inicio`. Desactiva bloque de firmware y dispositivo local, y reproduce el aviso con la misma partida. Trazas `--dbg 128` para Wi-Fi, 1 para SPI y 8 para cartucho/AUXSPI.
- Host: partida recibida → CONTINUE → mapa → guardar desde el menú → cerrar proceso → reabrir → mapa comprobados. El original queda intacto. Evidencia local en `build/hg/`; pruebas dirigidas con ASan/UBSan y en qemu BE: 16787 comprobaciones, 0 fallos.
- Compilación completa Docker correcta; LE completo: 8101713 comprobaciones sin fallos. Unitarias BE: 8105443, solo los 34 fallos conocidos de `test_arec.c:149/152`; `test-host` termina en 1 solo por esos 34 fallos BE: 608 veredictos OK, ROMs LE/BE/SAN, gráficos, hashes y lockstep correctos. Parche sobre `54ae2a5` (el índice Git no admite escritura); paquete `build/salsads-guardado-2/salsads-guardado-2.pkg`, «SalsaDS Guardado», `SLDS00032`, holgura mínima SPE 8400 B. SHA-256 `a996a003e40f2d5e329307839acd35f9fb640c97714730e0cc3e33458b856b2f`. Falta validar el paquete exacto en la PS3.

## 2026-10-26 — Comparación del guardado de HeartGold (pokemon-w2-guardado)
- Host, copia de la partida: v0.0.4 y `007d384` muestran el mismo aviso de corrupción; no se reproduce una regresión exclusiva del prefijo.
- Base: 12 sondeos `00 05 00` fallidos y acceso directo; rama: acceso con prefijo. Después, conversaciones normalizadas idénticas: 1121 lecturas, 286721 bytes, todos iguales al archivo.
- Sin RDID ni otras órdenes IR; sin escrituras. Original y copias conservan SHA-256 `bb92a224b6c3def07e2b527689ed8b3add9acf1afa5f15839a3af5307548a3a0`.
- Trazas completas, capturas del aviso y comparación en `build/w2/regresion/`; espera inicial de unos 28 minutos por turno de desarrollo.
- Sin cambios al protocolo ni paquete nuevo: falta una partida comprobada con la base o aclarar la diferencia entre host y consola antes de corregir y validar.

## 2026-10-26 — Prefijo de guardado infrarrojo (pokemon-w2-guardado)
- `007d384`: `00 05 00` recibía `FF FF FF`; se admite el prefijo para códigos I. JEDEC, latencias y acceso directo sin cambios.
- White 2, host: título, partida nueva, guardado manual de 524288 bytes y reinicio con «CONTINUE» verificados; vuelve al personaje y ubicación guardados. Capturas y trazas en `build/w2/`.
- Regresión host: MKDS escribe y recarga 262144 bytes, conserva el perfil y entra al selector de kart. HeartGold no está disponible; pendiente en consola.
- LE/SAN completos sin fallos; unitarias LE 8085059 comprobaciones. Reintento BE `-j1`: 8088789 comprobaciones, solo los 34 fallos conocidos de `test_arec.c:149/152`.
- Corredor BE: GCC volvió a caer por segmentación de cc1 con `-j1`; sin repetir. ROMs BE y lockstep NSMB/SM64DS/MKDS/Metroid de 1500 cuadros pendientes; validación global termina en 1.
- Paquete del código `007d384`: `build/salsads-w2.pkg`, «SalsaDS Guardado», `SLDS00032` libre en los PARAM.SFO locales; holgura mínima SPE 8432 B. Verificadores 0; sin envío ni prueba PS3.
- SHA-256 `a937cc642b3a6ccffe3570c048a6c0a566251773cafd0a5ac75b06d2115cc370`; guion PS3 en `build/w2/guion-ps3.txt`: White 2 nueva partida/guardar/salir/continuar, HeartGold guardar/cargar y regresión NSMB.

## 2026-10-28 — Tramo del planificador del JIT en 512 ciclos del ARM9
- Sonic Rush Adventure parpadeaba en la pantalla de abajo con el JIT y no con el intérprete. Con tramos de 1024 ciclos el juego prepara el motor B (DISPCNT, BG2, VRAMCNT) en las primeras líneas del cuadro siguiente: 4–20 cuadros «tarde» por ventana de 120. Con 512 o 128 (el del intérprete) no queda ninguno y no parpadea.
- PS3 real, intro de SRA (`/dev_hdd0/salsads/nucleo-jit-tramo`): 1024 ≈ 21 ms de emulación por cuadro, 512 ≈ 21,6–22,2 ms (+3 %), 128 ≈ 23,6 ms (+12 %). Metroid Prime Hunters con 512: sin parpadeo ni congelaciones, 9–13,5 ms por cuadro en juego.
- `JIT_QUANTUM` pasa de 1024 a 512; `nucleo-jit-tramo` sigue sirviendo para cambiarlo. La retención y el remapeo de la rama `captura-doble` atacaban el síntoma y no se integran.

## 2026-10-26 (2) — Torre Quemada de la intro: aristas x-mayores hacia adentro (rama heartgold-huecos)
- La intro de HeartGold (3 pueblos): con el arreglo anterior las dos primeras escenas quedaron limpias; en la Torre Quemada (cuadros ~2260 a 2350 desde el arranque) quedaban filas de 3 a 6 píxeles sobre aristas casi horizontales (el polígono 156, arista inferior de (23,124) a (83,129): la línea 125 empezaba en x = 47 en lugar de 35).
- Arreglo: con el tamaño completo, la izquierda x-mayor que va hacia la derecha y la derecha x-mayor que va hacia la izquierda también dibujan su tramo de la línea (la interpolación se estira para no extrapolar). Mismo interruptor `r3d-sin-completo`.
- También se dibuja la última línea de un polígono con izquierda x-mayor (la regla anterior la saltaba: era la fila entera bajo el polígono 31). Huecos (píxel transparente con vecinos opacos) en los cuadros 2150–2355: 11 611 con la regla anterior → 2. El edge marking solo pinta donde el id del vecino difiere y es más lejano; en estas escenas todos los polígonos tienen id 0 y el borde del plano de fondo (id 63) marca los píxeles junto a un hueco real, como en la DS.
- `HOLES_LOG=1|2` y `HOLES_COVER=1` (con `--frame-dump`) en `salsads-cpurun` listan huecos por cuadro y qué polígonos cubren cada píxel.

## 2026-10-26 — Rombos negros de HeartGold: tamaño completo de los polígonos (rama heartgold-huecos)
- Causa: en el mapa (cuadro 29800, Pueblo Primavera) polígonos vecinos con aristas que no coinciden exacto (el 62, derecha de (149,0) a (151,78); el 43, izquierda de (149,4) a (152,89)) dejaban una columna de 1 píxel sin cubrir (x = 150, líneas 61 a 77); con DISP3DCNT = 0x3039 (antialiasing y edge marking). GBATEK «Polygon Size»: solo los opacos sin edge marking ni antialiasing pierden el borde derecho; con cualquiera de los dos (o translúcidos con mezcla) la arista derecha inclinada dibuja su píxel. El rasterizador compartido lo tenía siempre exclusivo; la referencia del host tenía los mismos huecos (no era del SPE).
- Arreglo: `p->full` en `r3d_poly_setup` y un píxel más en `r3d_row_at` (derecha no vertical); no cambia la interpolación. Interruptor: archivo `r3d-sin-completo` en `/dev_hdd0/salsads/` (`--r3d-sin-completo` en el host). Prueba dirigida en `test_r3d_vrow.c`.
- Medido en el host (mismo guion de entrada, 29 900 cuadros): píxeles de hueco 187 282 en 5 690 cuadros → 132 en 22 (los que quedan son un tapete del interior, no del mapa). `--vs-ref3d` igual en HeartGold, MKDS, NSMB y SM64DS. Cambian el hash de la semilla 7 y 6 imágenes de `gx3d.golden` (28, 29, 30, 34, 37, 40), iguales en LE, BE y SAN. Falta confirmar en la PS3.

## 2026-10-28 — Metroid: 15 IDs translúcidos en 3D ampliado y recalibración del selector

- Medido en PS3 con los lotes nuevos (Metroid pesado 2×): 343 polígonos → 242 + 242 dibujos; GPU 7,7 ms (antes 15,5); costo 3D RSX 8,2–8,7 ms. Pero de 123 a 140 s todos los cuadros caían al SPE por «3D ampliado: capacidad translúcida sin paridad» (tope de 8 IDs antes de intentar el reparto ampliado, que sin sombras deja 4 bits de TID), y en 140 s el selector bajó al SPE comparando con una referencia SPE de 107 s (escena ligera, 3,1 ms): los cuadros sin 3D no la renovaban.
- Sin sombras se admiten hasta 15 IDs translúcidos (`3d-rsx-ids-translucidos-8` restaura el tope). El selector cuenta la edad de la referencia SPE en cuadros (con o sin 3D) y, si decidiría bajar con una de más de 120, recalibra primero (motivo «referencia SPE de otra escena: recalibrar», contador `recalibrar` en `rutas`; `3d-rsx-referencia-vieja` = ronda 6). Prueba en `test_rsx_cost`.
- La carga del cuadro RSX (18,8–20,9 ms) es trabajo del PPE: emulación ~16–21 ms (cuadros impares de Metroid) + presentación HD 2,07 (1,0 del `memcpy` de metadatos) + espera de textura 0,58 + preparación y envío 1,6; las esperas a la GPU son nulas. Pendiente: evitar el `memcpy` con un tercer juego de metadatos.

## 2026-10-28 — Menos dibujos RSX en Metroid: lotes en 3D ampliado, puerta compacta y atributos por vértice

- Causa medida en PS3 (ronda 6, Metroid pesado a 2×): 333 polígonos → 688 dibujos de color + 688 de atributos por cuadro; GPU color 8,1 + auxiliar 6,7 + posproceso 0,7 ms y envío PPE 3,7 ms. Las ventanas a 1× dan unos 6–8 µs de GPU por dibujo (sin relleno), así que ~9 de los 15,5 ms de GPU son dibujos, no píxeles. Todas esas escenas tienen polígonos con igualdad de profundidad: el 3D ampliado apagaba los lotes (cada opaco frontal con espalda previa son dos dibujos) y cada translúcido costaba 7 dibujos de color y 5 de atributos (borrado de puerta, puerta, rechazo de ID y color).
- Plan de pasadas compacto (`rsx_passes.h`, `rsx_color_passes_mode`): la puerta del bit 7 queda decidida en su propia pasada (1 al pasar, 0 al fallar profundidad o stencil, con `stencil_sfail`), sobra la pasada de borrado y el rechazo del mismo ID translúcido entra en la misma prueba si el polígono no tiene texeles opacos; el empate frontal se resuelve con una pasada LEQUAL más una `RSXKIT_DEPTH_EQUAL` sobre destinos frontales. Translúcido ampliado: 7 → 3 dibujos de color (2 sin espalda previa) y 5 → 3 de atributos. Los opacos frontales con espalda previa emiten primero la pasada de espaldas (LEQUAL) y luego la de frentes (LESS): así un lote de varios polígonos equivale al orden polígono a polígono (prueba `batches` en `test_rsx_passes`).
- Lotes con clave portable (`rsx_batch.h`): textura, k, pruebas, IDs y, en 3D ampliado, la cara. La pasada de atributos ya no lleva la Z plana ni la bandera de niebla por constantes: van en la palabra `pad` de cada vértice (segundo color, `rsxkit_vertices_color_aux`) y el shader las lee de `fc` (Z>>9 en dos bytes, plano, niebla; mismo formato por constantes). Así los atributos forman los mismos lotes que el color.
- rsxkit: un programa propio se carga solo al cambiar y su etiqueta de uso (barrera del back end) se escribe al cambiar de programa, al salir del 3D o antes de destruirlo, en vez de en cada dibujo. `stencil_sfail`, `RSXKIT_DEPTH_EQUAL`, `rsxkit_vertices_color_aux`, `rsxkit_3d_program_sync`.
- Interruptores (A/B, `/dev_hdd0/salsads/`): `3d-rsx-atributos-por-poligono` (constantes y un dibujo por polígono en atributos), `3d-rsx-ampliado-sin-lotes`, `3d-rsx-puerta-ronda6`, `3d-rsx-programa-por-dibujo`. `rsx3d lotes:` en el arranque dice qué hay activo.
- Conteos en el host (`salsads-cpurun --rsx-lotes reproducción.rsx`, misma clave y plan que el PPE): la reproducción de Metroid en juego (522 polígonos, 223 translúcidos, 25 con igualdad) pasa de 1640 + 1640 = 3280 dibujos a 1012 + 1012 = 2024 (−38 %); NSMB y SM64DS sin cambio (no son ampliados). Para la zona pesada del log (688 por pasada con pocos translúcidos) la estimación es ~265 por pasada. Los tiempos los dice la consola.
- Diagnóstico para la Parte B: `DATO jit-lentos-es` lista los ocho registros de E/S que más tiempo pagan por la ruta lenta del recompilador (Metroid pesado: 4313 escrituras × 1,17 µs = 5,0 ms por cuadro sin saber a qué registro).
- Validación: unidad LE 8 996 681 comprobaciones sin fallos; `niebla fp40` 13 656 casos con el nuevo formato de `fc`; `test-host` y paquete `salsads-rend-fable` (SLDS00040) en `build/rend/informe.md`. Pendiente medir en PS3: autoprueba 1×/2×, Metroid pesado, NSMB y MKDS con cada interruptor.

## 2026-10-28 — HD preferida cuando cabe en el cuadro

- Ronda 5 recibida de PS3: autopruebas 1×/2× correctas. El selector anterior manda al SPE 3414 de 3782 cuadros con 3D de NSMB (90,3 %), aunque el RSX cabe. En ronda 4, las 55 ventanas completas RSX de NSMB tienen servicio aproximado (intervalo menos flip) p25/mediana/p75 12,838 / 13,631 / 14,242 ms; Metroid, 18 ventanas completas RSX: 13,914 / 22,466 / 27,405 ms. Los archivos acumulados de ronda 4 no se suman entre sí.
- La política mide ahora el trabajo del cuadro en el PPE, desde su comienzo hasta encolar la presentación, excluyendo `frame_begin`/flip. Incluye la emulación y sus esperas, recogida 2D y presentación. La carga por ruta es el máximo de ese trabajo y su costo 3D previo (GPU + preparación RSX, máximo de trabajadores SPE), para no ocultar un raster lento tras el flip. No suma costos que se solapan ni ejecuta las dos rutas para el mismo cuadro.
- Mantener HD hasta 16,667 ms; confirmar el retorno con ≤16,0 ms en dos ventanas de ocho. Solo bajar si excede 16,667 ms y el SPE mejora tanto el raster (+10 % y 0,5 ms) como la carga del cuadro (+5 % y 0,5 ms). Si no hay mejora suficiente, conservar HD aunque la CPU impida 60 Hz. Se mantienen dos ventanas para salir del RSX estable, un sondeo caro de ocho, permanencia/renovación desde 120 muestras y `3d-rsx-sin-costo`.
- Las muestras de cuadro y raster tienen ventanas independientes de al menos ocho, misma ruta y generación. Solo se toma un cuadro con exactamente un envío admitido; se descartan los respaldos de paridad y cuadros que cruzan un cambio de fase. `DATO rsx3d rutas` añade cargas, presupuesto, margen de retorno, motivo y conteos de decisiones; `costo cambio` conserva la transición con sus medidas.
- Pruebas sintéticas de 10 000 decisiones con la política real: NSMB 9440 HD / 560 SPE de calibración; Metroid pesado 632 HD de sondeo / 9368 SPE. Son cuentas del host, sin promesa de fps en consola. Casos de saturación de CPU, GPU oculta, fronteras, picos, cambios de escena, pausas y dos retiros en vuelo cubiertos. El raster, sus shaders y tolerancias no cambian. Informe y evidencia en `build/niebla-final/informe-6.md`; validación de juegos en PS3 pendiente.
- Validación: unidad LE/SAN 8 984 172 comprobaciones sin fallos; BE/qemu 8 987 906 con solo los 34 conocidos de `test_arec.c:149/152`. Dirigidas finales LE/SAN y BE: 892 259 por orden, cero fallos, incluidos cuatro escenarios con dos retiros en vuelo. Paquete 6 desde directorio nuevo, sin avisos; 13 656 casos fp40 correctos, 19 shaders idénticos a ronda 5 y holgura SPE mínima 8368 B. `test-host` termina con salida 1 solo por esos fallos conocidos: ROMs LE/BE/SAN, gráficos y sonido correctos, 32 hashes de estado y 16 lockstep coincidentes. El ejecutor BE se compiló tras un único reintento de GCC. Git deniega crear el índice; se entregan parche verificado y mensaje de commit.

## 2026-10-28 — Exportación nativa desde HD y selección por costo

- Ronda 4 confirmada en PS3: autopruebas 1×/2× correctas. Metroid en juego, 13 ventanas 120/120 RSX: intervalo p25 23,318 / mediana 25,807 / p75 42,190 ms. NSMB: 55/67 ventanas cerca de 16,7 ms, ninguna >25 ms y cuatro cuadros SPE. El raster nativo y HD duplican color/atributos; reducir dibujos ya no basta en los tramos pesados.
- En juego HD sin captura, `pack` exporta directamente una muestra central por píxel de la imagen HD; omite el raster nativo, incluidas sus pasadas auxiliar y final de niebla. La imagen conserva RGB6/A5 sin filtro de alfa. DISPCAPCNT activo en la instantánea inicial, verificación y API de reproducción conservan el raster 1× independiente. Las escrituras posteriores no cambian la instantánea que captura el núcleo; una prueba dirigida cubre esa frontera. A/B: `/dev_hdd0/salsads/3d-rsx-nativa-ronda4`.
- Selector para juego HD: ocho muestras SPE iniciales; dos ventanas RSX de ocho con margen de 15 % y 0,5 ms para entrar; dos ventanas más caras que SPE en 10 % y 0,5 ms para salir. Permanencia SPE y renovación de referencia desde RSX cada 120 muestras válidas. Solo se admiten tiempos de etiquetas completas, sin nuevas esperas; las generaciones descartan retiros tardíos. Cada juego reinicia la historia. A/B: `3d-rsx-sin-costo`; `DATO rsx3d rutas` separa reducida/exacta y SPE por costo/calibración.
- La comparación usa raster nativo + HD + exportación + preparación PPE frente al máximo de ambos trabajadores SPE; no equivale al intervalo total. Los sondeos, cambios de escena y la composición HD impiden prometer que cada cuadro sea más rápido. Medir la misma zona con el selector activo/desactivado y con `3d-rsx-pasadas-ronda3`.
- Validación host: suite LE 8 974 116 comprobaciones sin fallos; BE/qemu 8 977 846 con solo los 34 conocidos de `test_arec.c:149/152`. ROMs LE/BE/SAN, gráficos y sonido correctos; 32 hashes y 16 lockstep sin diferencias. Dirigidas finales LE/SAN y BE: 882 231 por orden, cero fallos, incluidas 32 comprobaciones de captura añadidas después de la parte unitaria de la suite. Paquete desde directorio nuevo sin avisos, holgura SPE mínima 8368 B y 19 shaders idénticos a ronda 4. Git deniega el índice: se entregan parche comprobado y mensaje de commit.
- Autoprueba ampliada: otras 18 pruebas y 512 escenas por la ruta reducida, 16 cuadros solapados con retiro alternado y ocho imágenes HD completas como referencia de muestreo en texturas, pendientes y transparencias. Las pruebas originales y los respaldos 6/7 no cambian de tolerancia. Sin nueva ejecución gráfica: RPCS3 aborta antes de arrancar en este entorno. Informe y paquete de revisión en `build/niebla-final/informe-5.md`; validación PS3 pendiente.

## 2026-10-28 — Pasadas de niebla RSX: costo y reducción

- La ronda 3 pasa en PS3 a 1×/2×: 512 escenas finales, 18 de niebla y respaldos 6/7 sin fallos. En juego, Metroid tiene mediana de 30,509 ms de intervalo en las ventanas 120/120 RSX, con 9,697 ms de raster nativo y 8,592 ms de envío PPE. La muestra de 705 polígonos llega a 12 420 dibujos: color y atributos, ambos a 1×/2×, más puerta de profundidad por cada polígono al haber igualdad. No son 17 dibujos de posproceso; los dos quads finales ni entran en ese contador.
- Plan de pasadas portable: opacos normales sin puerta temporal (una o dos pruebas según la cara previa); translúcidos simples con rechazo directo del ID y reemplazo de vacíos antes de mezclar; atributos translúcidos sin distinguir destino ocupado/vacío, pues siempre hacen AND y solo escriben profundidad con TDEPTH. No cambia el orden de polígonos, shaders, tolerancias ni reparto de stencil. Los lotes de color ignoran Z plana y bandera de niebla, que solo consume la pasada auxiliar.
- A/B: `/dev_hdd0/salsads/3d-rsx-pasadas-ronda3` conserva las pasadas anteriores. `3d-rsx-niebla-anterior` conserva además la ruta hd-10. `DATO rsx3d pasadas` separa dibujos, GPU y envío PPE de color/atributos/posproceso por escala y por motivo, cada 120 etiquetas completas. Sin esperas nuevas, lecturas de imagen ni reservas por cuadro.
- Host LE/SAN y BE/qemu: 3 717 120 estados por orden contra las reglas escalares, sin diferencias; siguen pasando las 512 escenas referencia/bandas. Unitarias: LE 8 963 846 sin fallos; BE 8 967 576 con los 34 conocidos de `test_arec.c:149/152`. `test-host` termina con salida 1 solo por esos fallos; ROMs, gráficos y sonido correctos, 32 hashes y 16 comparaciones lockstep sin diferencias. Shaders sin cambios. La autoprueba gráfica y la mejora en consola quedan pendientes: RPCS3 aborta antes de arrancar en este entorno.
- No se amplían IDs ni sombras W: Metroid conserva el respaldo de capacidad (21 ventanas/1580 cuadros en el log) y MKDS el de cara trasera W (104/12 386). Prioridad a validar las pasadas nuevas. Informe, guion y paquete de revisión de ronda 4 en `build/niebla-final/informe-4.md`.

## 2026-10-28 — Profundidad W y bordes Z de niebla RSX

- La segunda autoprueba PS3 1× deja 60/512 escenas finales fuera de tolerancia: todas con W y bandera inicial de niebla. En el ensamblado, `DEPTH` escribe R1.z y el temporal elegido para COLOR vuelve a escribir R1.z=0. Se rechazan los translúcidos posteriores al primer escritor de profundidad; queda una bandera o profundidad anterior. Reproducir esa secuencia predice exactamente los 60 índices del log, incluidos los seis sin opaco y los 24 con ID repetido. La sonda de 17/19 confirma atributos 48:0 y bandera 255 donde el AND debe apagarla.
- El corrector reserva R1 cuando existe DEPTH. La comprobación fp40 simula su alias con R1.z, compara la profundidad exportada y cubre descartes opacos/translúcidos, W desplazada y bordes Z con bandera de plano encendida/apagada: 13 656 casos sin fallos. Reintroducir cada shader anterior provoca el fallo esperado.
- Los 384/8832/7680 píxeles erróneos de las pruebas antiguas 0/8/10 coinciden exactamente con Z−1 en un borde de LUT (incluido alfa mediante el PPM de diferencias). Los polígonos de Z constante pasan ahora Z>>9 entera a los atributos; los inclinados mantienen WPOS. El ensamblado corregido reproduce los 49 152 píxeles de referencia de cada prueba y alfa exacta en sus 256 planos. No se cambia la tolerancia ni la profundidad usada para visibilidad.
- `test-host` termina con salida 1 sin fallos nuevos: LE 8 963 840 comprobaciones sin fallos; BE 8 967 570 con los 34 conocidos de `test_arec.c:149/152`. ROMs LE/BE/SAN, gráficos, sonido, 32 hashes y 16 comparaciones lockstep correctos. Las 512 escenas de referencia/bandas pasan en ambos órdenes.
- Paquete `salsads-niebla-final-3` compilado desde directorio vacío, sin avisos, con holgura SPE mínima 8368 B. Validación gráfica 1×/2× y rendimiento del arreglo pendientes de PS3; informe y evidencias en `build/niebla-final/informe-3.md`. Sin commit por denegación de escritura del índice; se entrega parche y mensaje.

## 2026-10-28 — Corrección de las salidas parciales de niebla RSX

- Los PPM de consola aíslan dos colisiones de `COLOR` con el temporal `R0` de cgcomp. En `niebla-5`, verde y azul son exactos en 49 152 píxeles y rojo satura en toda la imagen (46 080 distintos). La pasada final escribe RGB y después pisa rojo al calcular alfa. En atributos Z, escribir G pisa el byte alto de profundidad antes de escribir R: `niebla-16` termina leyendo LUT[0] en vez del tramo con offset 7000. El intérprete del ensamblado anterior reproduce exactamente los PPM 5 y 16; en el 0 reproduce rojo completo, pero quedan diferencias de intensidad sin validar en consola.
- Se aplica el corrector de salida ya existente a `fog_attr` y `fog_final`: acumula el resultado en otro registro y hace una sola escritura a `COLOR` al terminar. W ya lo usaba. La compilación interpreta el ensamblado fp40 corregido con el alias real de `COLOR`: 11 556 casos de atributos Z/W, LUT, alfa y sonda; cero fallos. Reintroducir por separado cualquiera de los dos shaders anteriores falla como se esperaba.
- La autoprueba registra diferencias y máximos R/G/B/A, primer píxel, referencia antes/después, FOG_COLOR y LUT del PPE. Una sonda del mismo cálculo lee en la GPU color previo/final RGBA8, atributos, FOG_COLOR, offset/paso y t0/t1/densidad/índice, a 1× y HD. Solo en autoprueba síncrona; sin lecturas nuevas durante el juego.
- «Sin recursos» al final era la desactivación por autoprueba fallida, después de inicializar y dibujar todas las escenas. Ahora se distingue del fallo de inicialización, se liberan los recursos al rechazar la autoprueba y se informa la reserva auxiliar (384 KiB a 1×). No hay evidencia de falta de GDDR3 en ese log.
- Los logs hd-10 aportados contienen partidas, sin comparación RGB6 de reproducciones: no permiten cuantificar una regresión de sus variantes. MKDS y SM64DS se reproducen con `DISP3DCNT=001d` (sin niebla); los programas de color, W y empaquetado son idénticos byte por byte a los recompilados desde hd-10. No se atribuyen sus errores de 49–63 a esta corrección.
- `test-host` termina con salida 1 solo por los 34 fallos BE conocidos de `test_arec.c:149/152`: LE 8 963 840 comprobaciones sin fallos; BE 8 967 570 con esos 34. ROMs LE/BE/SAN, gráficos, 32 hashes y 16 comparaciones lockstep correctos. Sin fallos nuevos.
- Paquete de revisión «SalsaDS niebla»/SLDS00030: `salsads-niebla-final-2`. Compilación y validación del ensamblado correctas; pendiente repetir las autopruebas 1×/2× y medir en PS3. Informe y evidencias en `build/niebla-final/informe-2.md`. Sin commit por denegación de escritura del índice; parche completo y mensaje en esa carpeta.


## 2026-10-28 — Niebla final del RSX (validación gráfica pendiente)

- El color se rasteriza sin niebla. Una segunda rasterización repite las mismas pruebas de alfa, profundidad e ID sobre atributos auxiliares: profundidad Z/W desplazada nueve bits en dos canales y bandera de niebla en alfa. Los opacos reemplazan ambos; los translúcidos hacen AND de bandera y solo reemplazan profundidad con `PA_TDEPTH`. El posproceso aplica la LUT interpolada, color y modo solo alfa al color ya mezclado, incluido el borrado. Ruta nativa y HD, sin lecturas al PPE ni asignaciones por cuadro.
- `3d-rsx-niebla-anterior` restaura hd-10, incluido el rechazo de translúcidos; la aproximación `3d-rsx-niebla-translucidos` solo tiene efecto con ese interruptor. Se conservan `3d-rsx-niebla-spe`, `3d-rsx-niebla-constante` y los respaldos por modos/capacidad sin paridad. Metroid puede descubrir después el límite previo de más de ocho IDs con igualdad: no se amplía el stencil.
- 512 escenas compartidas de host y autoprueba cubren Z/W, profundidad independiente en dos translúcidos, ocho combinaciones de bandera, solo alfa, ID repetido y borrado transparente. Host LE con sanitizadores y BE en qemu: 3499 comprobaciones dirigidas, 0 fallos por orden; la autoprueba gráfica exige RGB6 ±2 y alfa exacta contra la referencia. Compilación PS3 correcta; RPCS3 no pudo arrancar en el entorno restringido, por lo que no se acredita todavía paridad RSX ni mejora en consola. `test-host` termina con los 34 fallos BE ya conocidos del diagnóstico del JIT (sin cambios en CPU); LE, ROMs LE/BE/SAN, gráficos, hashes y lockstep pasan.
- Diagnóstico de hd-10: el costo alterno coincide con los metadatos HD. En NSMB (77–83 s), capas A 2,33–2,34 ms; final A 2,42–2,43 con mayoría SPE y 7,92–7,93 con mayoría RSX. Las cifras de B (~3,3 ms total / 2,4 final) no son la ruta rápida de A. No se cambia el compositor; detalle en `perf-log.md`.
- Paquete de prueba «SalsaDS niebla»/SLDS00030: `build/salsads-niebla-final/salsads-niebla-final.pkg`, compilado desde directorio nuevo. Informe, SHA-256, parche y guion de consola en `build/niebla-final/`. Cambios pendientes de commit por denegación de escritura del índice de Git.

## 2026-10-27 — RSX ronda 15: todo al RSX
- Congelación de hd-8/hd-9 confirmada en consola: la causa es el aplazamiento de la espera del 3D con HD (con `r3d-sin-aplazar` la imagen HD cambia en 116–120/120 presentaciones; con aplazamiento, 0/120). Sin encontrar el mecanismo (el orden de envío, copia y presentación es correcto en el código), la HD apaga el aplazamiento sola; `3d-rsx-hd-aplazar` lo conserva para seguir buscando. Costo medido por el autor: sin aplazar 869/7080 cuadros >20 ms (12 %) y emulación mediana 13,6 ms, contra 27 % en hd-7.
- Niebla por fragmento: el fragment program emula la tabla de 32 entradas con desplazamiento, bits de fracción e interpolación entre entradas sobre la Z del fragmento (modo Z) o la W por fragmento desplazada (modo W); tabla en TEXUNIT1. La profundidad del RSX no reproduce el entero de la DS: tolerancia documentada ±1 LSB de 5 bits por canal (y ±1 de 7 bits de densidad) en los cuadros con niebla; `3d-rsx-niebla-constante` restaura la regla anterior. Sigue en SPE «niebla con translúcidos» (pasada final de la DS sobre la profundidad final: no se reproduce por polígono).
- Texturas frías: en vez de caer al SPE, el PPE decodifica la textura (o el resto de una a medias) hasta 16384 texeles por cuadro; `3d-rsx-texturas-sin-rescate` lo apaga; el costo sale en «RGBA8» de `DATO rsx3d etapas` y el conteo en `DATO rsx3d frio`.
- No hechos: IDs translúcidos de más (nt > 8, ~23/120 en escenas de MKDS) y sombras de caras traseras W (~3/120): el stencil de 8 bits ya reparte puerta Z, cara, máscara, ocupado y 4 bits de ID; ampliarlos exige otro reparto y las dos hipótesis de la ronda 12 fallaron. Quedan en SPE con sus interruptores.
- RPCS3: las 18 pruebas de niebla dan 0 distintos con la niebla por fragmento (la W por fragmento se redondea: 1/pos.w llega como w−ε en polígonos planos); las pruebas de respaldo 6/7 pasan a exigir la tolerancia contra la referencia. El volcado de Metroid en juego sigue en SPE: tras la niebla cae por «niebla con translúcidos» y, con `3d-rsx-niebla-translucidos` (aproximación opcional, sin paridad), por «3D ampliado: capacidad translúcida» (más de 8 IDs translúcidos). Rescate PPE de texturas: 19 y 115 texturas por ventana en las reproducciones.
- Paquete «SalsaDS RSX»/SLDS00030: `build/salsads-rsx3d-hd-10.pkg`; guion y logs en `build/rsx3d-hd-10/`.

## 2026-10-26 — RSX ronda 14: regresión de hd-8 (pantalla de arriba congelada en MKDS 2×)
- PS3 con hd-8: la pantalla A se queda fija con el juego corriendo (log sin errores, HD 120/120). Diferencias con hd-7 en el log: la detección de «captura a doble pantalla» se activó en MKDS (8–120 cuadros por ventana) porque miraba el bit 31 de DISPCAPCNT, que MKDS deja puesto sin capturar (banco fuera de LCDC: 0 capturas); y, con el compositor A más rápido (8 ms), la espera del 3D pasó a aplazarse (líneas 88–160; en hd-7, 0), primera vez con HD en consola.
- Arreglos y diagnóstico: la detección usa solo capturas realizadas en dos cuadros seguidos (`nds_vset.captured`); `DATO rsx3d HD cambio` cuenta por 120 presentaciones cuántas veces los metadatos y un bloque 8×8 central de la imagen HD (leído con `rsxkit_target_read` tras la copia) repiten el cuadro anterior. Interruptores separados por novedad: `3d-rsx-hd-captura` (sin presentación nativa por captura), `3d-rsx-hd-metadatos-ppe` (presenta con el exportador escalar del PPE en lugar del vectorial del SPE: el SPE no puede llevar los dos por pila y código), `3d-rsx-hd-verificar-metadatos` (solo compara, `DATO rsx3d HD verificación`), `r3d-sin-aplazar` (ya existía).
- RPCS3: autoprueba 2× 0 fallos; MKDS no reproduce la congelación en el host ni en RPCS3 (la demo con 3D llega tarde y RPCS3 no reproduce los tiempos). Decide la PS3 con el guion A/B de `build/rsx3d-hd-9/guion-ps3.txt`. Paquete `build/salsads-rsx3d-hd-9.pkg`.

## 2026-10-26 — RSX ronda 13: tirones a 2× y captura a doble pantalla
- PS3 (ronda 12, MKDS 2×): los cuadros >20 ms no salen del RSX (preparación 0,5–1,5 ms, envío ≤1, etiqueta 0,004, raster 2–3) sino del compositor A con HD: la exportación de metadatos («(final)» en `DATO spe-etapas`) cuesta 13 ms en carrera y 28 ms en la escena previa (960 de 3100 cuadros lentos, intervalo 30 ms); en carrera el PPE la espera 2,5–4 ms en «línea». Metroid menú: 40,9 ms.
- Exportador vectorial de metadatos en el SPE (8 píxeles por vector, transposición por shuffles); el escalar por píxel queda en PPE/host como referencia y en la autoprueba (`paridad bytes`). Host: 8641 comprobaciones, 0 fallos (vector = escalar byte a byte). Holgura de pila del gpu2d 8368 B. Techo: compositor A de 13–28 ms a ~2–4 ms; MKDS 2× debería quedar como 1× (≤1 cuadro lento por ventana en carrera).
- Metroid intro «estroboscópica» (ronda 12): el juego captura el motor A entero a C/D alternando, el motor B muestra ese banco (BG3 o OBJ de mapa de bits) y POWCNT1 intercambia pantallas cada cuadro (traza host `salsads-cpurun --video-trace`, cuadros 90–2075: DISPCAPCNT 8032/8033xxxx, DISPCNT B 0835/1035, POWCNT1 820f/020f); con el 95 % de los cuadros en el RSX cada pantalla alterna HD (A) y nativo (B) a 30 Hz. Arreglo: `hd_capture_pair` presenta A en nativo mientras dure el patrón (`DATO rsx3d HD caída: captura a doble pantalla`); A/B `3d-rsx-hd-captura`.
- Metroid en juego (log completo): 2444/4680 caen al SPE por niebla interpolada, pero el ARM9 ya tarda 17–22 ms de mediana en esas ventanas (10–15 en las ligeras): la niebla en RSX solo quita la espera del raster (p95 12–20 ms, mediana ≤2); no da 60 fps. Sin implementar; propuesta como interruptor opcional con tolerancia documentada.
- Otras causas de tirones medidas: el recompilador del ARM7 traduce 3,5–5 ms por cuadro con cientos de vaciados de su caché de 1 MB en las ventanas de carga de MKDS y en la intro de Metroid (fuera de esta rama); sombras traseras W (3/120) e IDs translúcidos (23/120) quedan en SPE tras dos hipótesis fallidas en la ronda 12: techo pequeño.
- RPCS3: autoprueba 1×/2×/interruptores apagados 0 fallos, 16 cuadros solapados exactos, seis casos HD con metadatos 0 y paridad bytes 0; Metroid 700 cuadros a 2×: la intro cae a nativo por «captura a doble pantalla» 120/120 y con `3d-rsx-hd-captura` vuelve a HD 118/120. Paquete «SalsaDS RSX»/SLDS00030: `build/salsads-rsx3d-hd-8.pkg`; SHA-256, logs y guion en `build/rsx3d-hd-8/`. Sin envío.

## 2026-10-26 — RSX ronda 12: ampliación parcial con respaldo exacto
- Stencil por cara en Z, leyendo la orientación del polígono de entrada, con puerta temporal de profundidad y ocupación; OID remapeados solo cuando una sombra puede consultarlos. Se conserva la ruta anterior para los cuadros ya admitidos.
- Igualdad inclusiva Z ±512/W ±255 con dos límites y escritura posterior; 81 dirigidas exactas, incluidas Z inclinada, destino transparente y niebla constante. Estados genéricos de profundidad propios en `rsxkit` (MIT).
- Tope aplicado: sombras traseras W y exceso de TID siguen en SPE tras dos hipótesis fallidas. El volcado de Metroid sigue en SPE por niebla interpolada; no se acredita cobertura casi total ni mejora de velocidad en juego.
- Texturas ya encoladas: sondeo con plazo de 250 µs, sin publicar lotes incompletos; coste/listos/vencidos por 120 intentos de cuadro. No cambia RGBA8, identidad ni vértices.
- A/B: `3d-rsx-sombra-trasera-spe`, `3d-rsx-sombra-ids-spe`, `3d-rsx-igualdad-spe`, `3d-rsx-texturas-sin-espera`; límites sin paridad siempre conservan el respaldo.
- RPCS3 1×/2× y A/B apagado: 0 fallos, 16 cuadros solapados exactos, 177 mapas previos iguales; 64 escenarios de sombras incluyen ocho empates aislados y rechazos sin modificar salida. Holgura SPE mínima 8336 B.
- LE/SAN, ROMs, hashes y lockstep correctos; unitarias BE conservan los 34 fallos conocidos del anillo JIT, fuera de alcance.
- Paquete «SalsaDS RSX»/SLDS00030: `build/salsads-rsx3d-hd-7.pkg`; SHA-256, pruebas y guion en `build/rsx3d-hd-7/`. Medir autoprueba, MKDS y Metroid dos minutos a 2× en PS3; sin envío.

## 2026-10-26 — RSX ronda 11: espera del compositor y metadatos HD
- PS3 de 6121383: 6294 retiradas RSX y 6294 composiciones HD; «raster ausente» incluye respaldo SPE y ausencia de capa 3D, sin pérdida global acreditada por emparejamiento.
- Ventanas con ≥100 RSX: mediana compositor A 39,305 ms, emulación 25,862; etiqueta/reutilización 0,004 ms. Eventos de línea: 2,380→15,345 ms; la reutilización de instantáneas espera al SPE dentro de la emulación.
- Metadatos de presentación por bloques de ocho con capas/ventanas idénticas; contrato escalar intacto. A/B `3d-rsx-hd-metadatos-escalares`; flip aplazado hasta presentar: la composición RSX de N puede correr durante la emulación de N+1 (`3d-rsx-hd-flip-temprano` restaura el orden previo).
- Log por ventana: esperas SPE A/B, instantánea (incluida en SPE), textura, adquisición HD, envío/cierre; motivos concretos de caída. Raster HD se mide solo tras su etiqueta, sin espera adicional.
- Texturas DS, identidad y vértices intactos. No se reutiliza HD de otro cuadro para ocultar un respaldo SPE ni se cambia su política de admisión.
- RPCS3: 1×/2× aislados, 0 fallos, 16 cuadros exactos y 177 mapas iguales; seis escenas SPE con metadatos escalares/por bloques idénticos. La secuencia experimental mantiene la limitación propia de RPCS3.
- Build correcto, holgura mínima 8336 B; LE/SAN, ROMs/hashes/lockstep pasan; solo 34 fallos BE conocidos. Metadatos con DMA de 16 cambios: 8641 comprobaciones, 0 fallos.
- Juego propio en RPCS3: 240 cuadros, resultado correcto; Gouraud 118/120 HD tras dos cuadros sin 3D, alambre cae al SPE con motivo. Paquete `build/salsads-rsx3d-hd-6.pkg`, SLDS00030; SHA-256 y guion en `build/rsx3d-hd-6/`. Rendimiento nuevo en PS3 pendiente; sin envío.

## 2026-10-26 — RSX ronda 10: paquete de diagnóstico HD para consola
- `3d-rsx-hd-juego` + escala HD activa el orden nativo→HD de ronda 8; sin el archivo se conserva la ruta de 883bf2a. Preparación de texturas, identidad y vértices intacta.
- Autoprueba informativa: 16 cuadros, exacto/anterior/otro por cuadro; repetición con otro destino nativo para cambiar el descriptor de exportación por ranura. No es un arreglo ni se activa esa variante en juego.
- Composición leída en otro banco de 16 cuadros por variante: la espera entre parejas altera la sincronía y no sustituye la secuencia original.
- Juego: porcentaje HD y caídas cada 120 presentaciones; sin esperas PPE nuevas. El atraso no aborta la autoprueba: consultar los resúmenes diagnósticos además del resultado global.
- RPCS3: 1× y reproductor 2×, autoprueba 0 fallos, 16 exactos y 177 mapas iguales a hd-3; experimento original 1 exacto/15 anteriores, variante 0/15/1. No demuestra el comportamiento de PS3.
- Build completo correcto; holgura mínima 8432 B. LE/SAN, ROMs, hashes y lockstep pasan; `test-host=1` solo por los 34 fallos BE conocidos del anillo JIT, fuera de alcance. Verificadores en 0.
- Paquete `build/salsads-rsx3d-hd-diag.pkg`, «SalsaDS RSX»/SLDS00030; SHA-256, evidencia y guion único en `build/rsx3d-hd-diag/`. PS3 pendiente; sin envío ni publicación.

## 2026-10-26 — RSX ronda 9: descartada la banda de 96 líneas
- RPCS3 2×, base restaurada de ronda 7: 16 cuadros solapados exactos; el parche de ronda 8 reproduce 15/16 fallos.
- Los 24576 píxeles forman el rectángulo de prueba 192×128: x=32..223, y=32..159; 12288 diferencias en cada mitad.
- Cada cuadro n=1..15 coincide íntegramente con n−1, nunca n+1; destino n%2, contenido previo de la ranura opuesta, también retirando en orden inverso.
- No se acredita un fallo por bandas ni su causa interna; parada al tope solicitado. Código e instrumentación revertidos, 1× y reproductor intactos.
- Dos builds correctos; evidencia y parche diagnóstico en `build/rsx3d-hd-5/`. Sin nueva ejecución LE/BE/SAN: solo documentación conservada.
- Sin habilitar HD en juego ni generar paquete hd-4. El paquete hd-3 previo sigue limitado al reproductor; no medir MKDS 2× como juego HD.

## 2026-10-26 — RSX ronda 8: parada por solapamiento HD
- Exportar nativo antes de HD, con etiquetas separadas, falla en RPCS3 2×: 15/16 cuadros solapados devuelven colores anteriores, 24576 píxeles por cuadro.
- Renovar además el sampler de exportación repite exactamente el fallo; parada tras dos intentos, sin tercer cambio.
- Los 32 contratos HD, línea cambiante y seis escenas SPE pasan; 177 mapas nativos síncronos iguales a ronda 7. Eso no acredita el solapamiento.
- Build completo correcto en ambos intentos; holguras 2D 8720 B, mínima global 8432 B. Evidencia y parche descartado en `build/rsx3d-hd-4/`.
- Código restaurado a ronda 7; HD en juego sigue bloqueado, sin paquete hd-4 ni nueva validación LE/BE/SAN. Mañana corresponde el reproductor hd-3, sin acreditar juego HD ni tiempos PS3.

## 2026-10-26 — RSX ronda 7: metadatos HD renovados
- RPCS3 retenía el sampler con descriptor idéntico: invalidación/etiqueta, vaciado XDR y escritura PPE directa no bastan; cambiar el offset sí actualiza. Mapeos y emparejamiento correctos.
- Recarga optativa del sampler en `rsxkit` (MIT), activada solo para metadatos HD; se conserva la invalidación previa. Sin cambios al shader, texturas DS, identidad ni vértices.
- A/B `3d-rsx-hd-sin-recarga`: desactivada reproduce 473 fallos en la línea; activa: 1024 píxeles/20 sondas en ranuras 0,0,1,0, 32 contratos y seis escenas SPE exactos a 2×/3×/4×.
- 1× intacto: autoprueba y 16 cuadros solapados exactos; 177 mapas nativos iguales a la ronda 6 en las cuatro escalas.
- Banco aislado con composición a resolución completa, 2D sintético, sin SPE/flip; cifras RPCS3 en perf-log.md, sin extrapolarlas a PS3. HD sigue limitado al reproductor.
- LE/SAN y ROMs/hashes/lockstep correctos; unitarias BE mantienen los 34 fallos conocidos del anillo, fuera de alcance.
- Paquete «SalsaDS RSX»/SLDS00030: `build/salsads-rsx3d-hd-3.pkg`; evidencia, SHA-256 y guion en `build/rsx3d-hd-3/`. Sin envío; pendiente confirmar en PS3 real.

## 2026-10-26 — RSX ronda 6: composición HD, parada tras dos hipótesis
- Línea mínima sin SPE: 722/768 píxeles difieren; tres publicaciones conservan el patrón inicial. Sonda independiente de las cuatro palabras de metadatos y del RGBA 3D, por canal.
- Desligar el destino antes de leer corrige la retención de la primera lectura; la muestra 3D es exacta, pero 9/12 sondas de metadatos siguen antiguas. Se conserva este cierre solo en autopruebas HD.
- Seis escenas: nativo y metadatos exactos; shader 49149/48527/48200/49151/49008/49126 diferencias. No se demostró fallo del traductor; shader de composición sin cambios.
- Ensamblador, binario, GLSL, las dos hipótesis y guion en `build/rsx3d-hd-2/`; 19 instrucciones condicionales señaladas por el verificador, sin atribuirles este fallo.
- RPCS3 1×: autoprueba 0 fallos, 16 cuadros solapados exactos y 177 mapas sin cambios. Captura y preparación intactas.
- LE y SAN correctas; unitarias BE conservan los 34 fallos conocidos del anillo.
- HD sigue bloqueado en juego; sin medir 2×/3×/4× con composición. Paquete diagnóstico `build/salsads-rsx3d-hd-2.pkg`; probar primero autoprueba HD en PS3 real.

## 2026-10-26 — RSX fase 3: HD detenido por paridad
- Prototipo 2×: raster HD separado, exportación nativa intacta, metadatos 2D con dos capas elegibles, ventanas, alfa y brillo; B nearest.
- `3d-rsx-2x`/`3d-rsx-3x`/`3d-rsx-4x` quedan limitados a `3d-rsx-prueba`; en juego se fuerza 1× con motivo en log. No se habilita HD ni se certifican 60 fps.
- RPCS3 1×: autoprueba 0 fallos, 16 cuadros solapados exactos y 177 mapas iguales a bcd37c1. 2× conserva esos mapas; contrato del shader: 32 × 49152 casos exactos.
- Bloqueo: seis escenas SPE producen imagen nativa y metadatos exactos, pero el shader difiere en 48200–49151 píxeles. Repetido con publicación PPE separada: parada, sin probar 3×/4×.
- Se corrigieron el alfa de copias RGBA y el remapeo de metadatos; terminar el envío con la exportación nativa recupera el solapamiento. Esos arreglos no resuelven el bloqueo SPE/RSX.
- LE/SAN y metadatos con bloques DMA de 64/256 cambios correctos; BE conserva 34 fallos conocidos. Holguras: 2D 8720 B, mínima global 8432 B. Presupuesto en memory-budget.md.
- Evidencia, guion y límites en `build/rsx3d-hd/`; paquete de diagnóstico `build/salsads-rsx3d-hd.pkg`, sin envío ni validación para medir HD en consola.

## 2026-10-25 — RSX fase 2, cuarta ronda: preparación y envío
- Base PS3 `abddb45`, MKDS misma pista: preparación 1,13 ms y envío 0,59; emulación RSX 11,36 frente a SPE 10,37; cuadros >20 ms 22,9 frente a 12,6/120.
- Vértices VMX con división Z escalar exacta; armado separado en el perfil y sin limpiar campos de aristas que RSX no consume.
- RGBA8 en el segundo SPE del raster: doble búfer, lotes de hasta 65 536 texeles después del respaldo; instantánea inmutable, publicación solo al completar y sin espera PPE en juego.
- Constantes y stencil incrementales en `rsxkit` (MIT); formato DS y decodificador en `src/` (GPL-2.0-or-later).
- Interruptores: `3d-rsx-vertices-ppe`, `3d-rsx-texturas-ppe`, `3d-rsx-envio-anterior`; se conserva `3d-rsx-sin-solapar`.
- Validación: LE/SAN, ROMs BE, hashes y lockstep correctos; solo 34 fallos BE conocidos. Dirigidas LE/BE 871 923 por orden, 0 fallos; build completo correcto.
- RPCS3: autoprueba 0 fallos, 16 cuadros solapados exactos, 176 mapas iguales a ronda 3 y 177 iguales A/B. RGBA8 14 casos, alternancia raster y publicación e invalidación entre lotes correctas; holgura nueva 13 840 B, mínima global 8432 B.
- Entrega «SalsaDS RSX»/SLDS00030: `build/salsads-rsx3d-fase2-4.pkg`; SHA-256, guion y evidencia en `build/rsx3d-fase2-4/`. Verificadores en 0; sin envío ni publicación.
- Riesgo: el trabajador de RGBA8 usa 448 celdas al rasterizar; su lote puede retrasar el respaldo siguiente. La meta preparación+envío ≤0,3 ms y la ausencia de tirones requieren PS3 real.

## 2026-10-25 — RSX fase 2, tercera ronda: solapamiento con emulación
- Integrada la identidad incremental de `6a03335`, conservando pruebas y documentación de ambas ramas.
- Cola común RSX/SPE: envío sin esperar, dos ranuras de vértices/tabla toon/exportación/estadísticas y etiquetas por trabajo; captura DISPCAPCNT retira el resultado antes de leerlo.
- La caché compartida espera su última etiqueta antes de modificar texeles; no retiene listas ni VRAM del juego. Arena XDR sin aumento, 24 MiB.
- `3d-rsx-sin-solapar` restaura la espera inmediata. `DATO rsx3d cuadro` separa emulación, preparación, envío, etiqueta, reutilización, copia, raster/exportación, compositor y flip.
- Log PS3 anterior: raster+exportación 2,36 ms, preparación 2,23, emulación 16,59 y espera de flip 14,30; compatible con perder un vsync. La recuperación de 16,7 ms requiere medición en consola.
- Validación: autoprueba aislada RPCS3 0 fallos, 16 cuadros solapados exactos y 176 mapas iguales a la ronda anterior; dirigidas LE/BE 655413 comprobaciones por orden sin fallos. `test-host`: LE/SAN, ROMs, hashes y lockstep correctos; solo los 34 fallos BE conocidos del anillo.
- Build completo correcto, holgura SPE mínima 8432 B, verificadores en 0. Paquete `build/salsads-rsx3d-fase2-3.pkg`, «SalsaDS RSX»/SLDS00030; SHA-256 y guion locales en `build/rsx3d-fase2-3/`. Sin envío ni publicación; velocidad PS3 pendiente.

## 2026-10-25 — RSX fase 2, segunda ronda: niebla constante y lotes de sombras
- PS3 recibida: 11/16 dirigidas de niebla fallan; pesado 652 dibujos, 5,19 ms raster+exportación. Los mapas de consola sustituyen la aprobación previa de RPCS3.
- Diagnóstico host: 83/84 rectángulos RGB compatibles con densidad/borde Z anterior; el restante pierde un canal saturado. Hay 39 rectángulos sin niebla con error alfa; recuperar enteros antes de mezclar evita el truncado de constantes.
- Densidad DS entera por polígono Z/W constante; mezcla RGB6/A5 en RSX con constantes redondeadas. Z/W variable con niebla vuelve al SPE; `3d-rsx-niebla-spe` conserva prioridad. No se certifica niebla variable ni paridad global.
- Sombras consecutivas del mismo ID/estado, sin textura ni descarte alfa: consumir máscara incluso al fallar Z; omitir destino vacío cuando el borrado tiene alfa. `3d-rsx-sombra-sin-lotes` recupera las pasadas anteriores; stencil genérico MIT separado.
- Reproductor RPCS3: 18 dirigidas de niebla, 24 de sombras y 8 respaldos correctos; 176 mapas idénticos con lotes apagados/remapeo general y 158 comunes sin cambio respecto a la ronda anterior. Pesado: 652→436 dibujos/estados; no acredita tiempo PS3.
- Host: 110592 secuencias de stencil y 528384 mezclas perturbadas correctas. Build completo y LE/SAN correctos; BE solo 34 fallos conocidos, ROMs/hashes/lockstep correctos. Holgura mínima SPE 8432 B; verificadores en 0. Paquete limpio `build/salsads-rsx3d-fase2-2.pkg`, código `ba64c4d`, título «SalsaDS RSX»/SLDS00030; SHA-256 local en `build/rsx3d-fase2-2/SHA256.txt`.
- Preparación de texturas, identidad y conversión de vértices intactas. Guion con las mismas banderas/cuadros y evidencia local en `build/rsx3d-fase2-2/`; confirmar niebla y pesado ≤4 ms en consola antes de integrar.

## 2026-10-25 — RSX fase 2 parcial: sombras, toon y niebla opaca
- `19eb7d8`/`19662be`: stencil genérico MIT; sombras por ID, toon/resaltado y niebla por fragmento en DS. Preparación de texturas, identidad y vértices intacta; reservar LUT de 128 B en el desplazamiento 20 MiB del área RSX al integrar.
- Sombras: remapeo OID/TID con respaldo por capacidad o profundidad trasera; niebla sin mezcla/toon/sombras y W constante. Clear image, bordes efectivos, igualdad y alambre siguen en SPE; límites y guion en `build/rsx3d-fase2/`.
- RPCS3: 20 casos de sombras, 16 de niebla, 6 de respaldo y 4096 combinaciones toon; 158 mapas idénticos con/sin lotes y sombras general/optimizada. Persisten diferencias globales de cobertura/interpolación/mezcla; no hay paridad global aprobada.
- Host MKDS carrera: sombras 2938/3000; captura completa admitida por RSX. Metroid en juego: niebla+igualdad 4000/4000, respaldo SPE íntegro verificado. SM64DS solo título; faltan su captura de juego y bordes efectivos.
- Base `c23c56e` medida en PS3 por el autor: pesado 3,93 ms; esta ruta añade pasadas y NO tiene medida PS3 ni aprobación del límite 4 ms. No se extrapola RPCS3.
- Interruptores por cuadro: `3d-rsx-sombra-spe`, `3d-rsx-toon-spe`, `3d-rsx-niebla-spe`; censo de uso RSX/SPE cada 120 cuadros. Validación de 2 min en consola pendiente.
- Build completo/paquete y verificadores correctos. `test-host=1`: LE/SAN pasan, unidad BE con 34 fallos conocidos; compilador emulado cae dos veces al construir el ejecutor BE, sin tercer intento ni validación ROM/JIT BE. Holgura SPE mínima 8432 B.
- Paquete diagnóstico «SalsaDS RSX», SLDS00030: `build/salsads-rsx3d-fase2.pkg`; SHA-256, mapas, reproductor y guion locales en `build/rsx3d-fase2/`. Sin envío a consola ni publicación.

## 2026-10-25 — Preparación RSX: identidad incremental de VRAM
- Sellos por página física de 16 KiB, compartidos entre instantáneas; solo se comparan bytes de páginas escritas. `3d-rsx-identidad-completa` restaura la ruta previa.
- Host caliente MKDS/SM64DS/NSMB/pesado: 75,8/27,9/99,3/124,6 → 57,1/7,8/80,4/107,3 µs; 400 cambios y flujos de triángulos idénticos, ASan/UBSan correcto.
- RPCS3: identidad caliente ~322–337 → 2–3 µs; 16 dirigidas y 39 mapas iguales a c23c56e. No acredita tiempos de PS3.
- VMX descartado tras una vuelta: vértices empeoran en tres escenas. Se conserva la conversión escalar.
- SPE descartado tras una vuelta: RGBA8 RPCS3 MKDS/pesado 138/48 ms frente a 16,5/18,1 en PPE; un lote frío llega a 67 ms y podría retrasar geometría. Se conserva PPE y sigue pendiente el tirón frío.
- Validación dirigida final LE/BE: 655 390 comprobaciones por orden, 0 fallos; build completo y revisión SPDX correctos. Unitarias BE conservan los 34 fallos conocidos del anillo.

## 2026-10-25 — RSX ronda 3: lotes; parada por presupuesto conservador
- `66e1651`: un búfer por cuadro, opacos consecutivos idénticos agrupados, radix estable y conversión por vértice; translúcidos, IDs y empates Z/W conservan su orden.
- Dibujos MKDS/SM64DS/NSMB/pesado: 596/55/972/1101 → 185/33/211/403; estados lógicos 185/31/211/403 sin cambio. Menos recargas de programa y etiquetas por dibujo.
- Caché por identidad exacta de páginas VRAM/paleta; perfil de identidad, vértices, búsqueda, RGBA8 y visibilidad XDR. La decodificación permanece en PPE.
- RPCS3: 16 dirigidas, aritmética, W, seis cambios de identidad y repeticiones calientes correctos; 39 mapas idénticos a la ronda 2 con optimizaciones activas/inactivas. Host ASan/UBSan: flujos de triángulos idénticos en cinco combinaciones.
- Host caliente MKDS 120→77 µs, pesado 313→122; frío empeora ~15 %. RPCS3 no muestra mejora uniforme de preparación; no acredita ahorro PPE real.
- Estimación conservadora PS3 raster+exportación pesado 4,34 ms (sensibilidad 3,17–4,34): supera el tope; parada, sin fase 2 ni decodificación SPE. Se conservan errores de paridad previos.
- `test-host=1` solo por los 34 fallos BE conocidos del anillo; LE/SAN, ROMs BE, hashes y lockstep correctos. Build completo y paquete limpios en 0; holgura mínima SPE 8432 B; verificadores en 0.
- Paquete diagnóstico «SalsaDS RSX», SLDS00030: `build/salsads-rsx3d-3.pkg`; reproductor, guion, SHA-256 y detalle en `build/rsx3d-ronda3/`. No enviado ni aprobado para sustituir el SPE.

## 2026-10-25 — RSX 1×, ronda 2: exportación corregida; paridad aún no aprobada
- `8475715`: etiquetas FINISH/FENCE inicializadas; antes, la primera espera aceptaba 322420947 como si fuera 1 y copiaba ceros. La comparación síncrona descartó compilación tardía de shaders.
- `51326bd`: W por fragmento con `wshift`, sin sustituirla por Z; prueba de perspectiva 0/4/8 correcta. Se conserva la aproximación triangular y el respaldo por modos no soportados.
- Modulación/decal opacos y RGB6/A5: 4096 combinaciones constantes exactas; mezcla RGBA8 no exacta (blanco sobre blanco puede perder un nivel). IDs repetidos cubiertos por pruebas dirigidas.
- Mapas W/Z en `build/rsx3d-ronda2/`: cobertura/color/alfa MKDS 0/23526/0 (65 sombras excluidas), SM64DS 0/359/0 (original W), NSMB 214/864/227 (original Z). UV constantes reducen RGB a 499/57/537; los residuos no están atribuidos por completo.
- `3d-rsx-prueba` reproduce instantáneas sin CPUs/SPE, también en consola: izquierda referencia/derecha RSX; apagado usa referencia en ambas. Registra costes y exporta mapas; no se rodeó ni se certificó el fallo SPU de RPCS3.
- `test-host=1` solo por los 34 fallos BE conocidos del anillo; LE/SAN, ROMs/JIT BE, hashes/lockstep correctos. Build completo y RPCS3 aislado encendido/apagado/manual correctos; 13 referencias LE=PPE BE, holgura mínima 8432 B, verificadores en 0.
- Paquete de diagnóstico `build/salsads-rsx3d-2.pkg`, «SalsaDS RSX», `SLDS00030` (PARAM.SFO verificado), SHA-256 `0185c3fa4684caa11d961100aa73ad059fb72f96894e58518c2268374e62f786`; sin aprobar para reemplazar el rasterizador habitual. Geometría SIMD y VRAM ARM7 intactas.
- Sensibilidad del subconjunto MKDS pesado: 1,52–3,81 ms raster+exportación, sin calibrar; 1101 dibujos, 44 texturas/1015808 B en frío. No acredita el cuadro con sombras ni tiempos PS3.

## 2026-10-25 — RSX 1×: fase 0 detenida, sin aprobar (rsx3d-fase0)
- `3d-rsx` optativo, `3d-spe` prevalece; Z nativa, W y efectos no soportados vuelven al SPE. Geometría sin cambios.
- RGBA8 por identidad exacta de VRAM/mapas; stencil para IDs; exportación RGB6/A5 a XDR, sin copia por polígono. Preparación aún en PPE.
- RPCS3: primera exportación vacía en dos corridas; parada. En la segunda, las otras siete escenas dirigidas coinciden bit a bit (incluyen IDs repetidos).
- Aplicación completa apagada: RPCS3 falla en su recompilador SPU (`r3d-1`, instrucción ilegal); reproductor aislado apagado correcto.
- Mapas locales: cobertura (alfa≠0)/color/alfa MKDS 0/23523/0, SM64DS 0/359/0 (subconjuntos en Z), NSMB 214/864/227 (original Z); no son paridad DS.
- Host: MKDS 1174, SM64DS 2298, NSMB 2195 cuadros iguales; LE/SAN completos y dirigida LE/BE correctos, unitarias BE solo 34 fallos conocidos. ROMs/JIT BE pendientes: `cc1` cayó dos veces en `salsads-ciclos`; parada.
- Build y paquete correctos; holgura SPE mínima 8432 B; verificadores de publicación/SPDX en 0. Paquete de diagnóstico `build/salsads-rsx3d.pkg`, título «SalsaDS RSX», `SLDS00030`, SHA-256 `449fa21d0703c4a2b49e31566ef93d25783bd2cdf4865296a85b68ae463c3a59`; no instalar ni medir hasta resolver exportación y completar RPCS3.

## 2026-10-25 — Clasificación de planos por lote en el SPE (cand-v004)
- `0371c56`: cuatro posiciones alineadas, trasposición y planos SIMD; conserva recorte, cara, contadores y orden. A/B independiente: `geometria-lectura-escalar`; se mantiene `geometria-perfil`.
- Listas de vértices/polígonos antes/después idénticas byte a byte en MKDS/NSMB/SM64DS; `--vs-ref3d`: 1175/2196/2299 cuadros iguales. Dirigidas con sanitizadores: 3247 comprobaciones, 0 fallos.
- `test-host=1` solo por los 34 fallos BE conocidos del anillo (`test_arec.c:149/152`, exige destinos de otros episodios); LE/SAN, ROMs BE, hashes y lockstep correctos. Unitarias LE: 8 084 992; BE: 8 088 722 comprobaciones.
- Build y paquete limpio en 0; holgura mínima SPE 8432 B, geometría 144 032 B. GCC ppc64 cayó una vez en `gxparidad`; el único reintento con `-j1` pasó. Verificadores de publicación y SPDX en 0.
- Host: armado 0,152880 → 0,152610 ms/cuadro, sin ganancia concluyente; no hay ticks SPU válidos. Metas PS3 ≤ 2,5 ms de armado y ≤ 1 ms de espera pendientes; guion de 2 min y riesgos en `perf-log.md`.
- Paquete del código `0371c56`: `build/salsads-armado.pkg`, SHA-256 `9b381b0ffd2226e47499848286fe870da95428619c3bc1ffb740272d6d3f0964`; sin probar en la consola.

## 2026-10-24 — Protección y código de VRAM del ARM7 (cand-v004)
- Captura: faltaba exigir LCDC al destino; la prueba reproduce `80008000` pisando bancos del ARM7. Ahora los rechaza (GBATEK, «DS Video Capture»; DeSmuME `WillDisplayCapture`, solo consulta).
- C/D, MST=2, OFS 0/1, espejos de 256 KiB, VRAMSTAT, solapamiento y DMA LCDC→ARM7: pruebas correctas; el mapeo no necesitó cambios.
- MKDS host: D=`82`, VRAMSTAT=`02`, código ARM válido desde `0600068c`; trazas con `--dbg 64` y 120 cuadros lockstep sin diferencias (9 643 659 comparaciones). Tercera carrera no reproducida; captura como causa de ese episodio aún por confirmar.
- JIT de páginas C/D sin solapamiento, invalidación CPU/DMA y vaciado al reasignar; `arm7-vram-nojit` permite comparar. Se conserva `jit-sin-codigo`.
- LE/SAN completos y dirigida BE: 661 comprobaciones, 0 fallos; unitarias BE: solo 34 fallos conocidos del anillo (`test_arec.c:149/152`, exige destinos de un bucle en otros episodios); ROMs, hashes y lockstep correctos; `test-host` sale en 1 únicamente por el anillo.
- `--vs-ref3d` host: MKDS 1174, NSMB 2195, SM64DS 2298 cuadros iguales; build y paquete limpio correctos. `build/salsads-vram7.pkg`, SHA-256 `ce6ee3a73098dd8196fd7d53b914bc98e95a1f5583e5ee52d8e1e5580dd36abc`; falta otro Grand Prix completo en consola.

## 2026-10-23 — Perfil optativo del armado en el SPE (cand-v004)
- Solo instrumentación: archivo `geometria-perfil`, línea `DATO geometria-armado` por ventana; etapas, conteos y guion en `perf-log.md`. Sin cambios al algoritmo ni envío a consola.
- Compilación completa y paquete limpio en 0; holguras: geometría 141 232 B, 3D 8432 B, 3D XDR 17 856 B, 2D 9056 B y sonido 178 384 B; mínimo 8192 exigido en el paquete.
- LE: 8 084 353 comprobaciones, 0 fallos; sondas activas con reloj simulado y ASan/UBSan: 3355 comprobaciones, 0 fallos. Autoprueba de consola alterna perfil apagado/activo contra PPE.
- `--vs-ref3d` con sondas activas en host: MKDS 1174, NSMB 2195 y SM64DS 2298 cuadros idénticos; hashes de geometría iguales a la entrega anterior. No mide el costo PS3.
- `test-host` incompleto: GCC ppc64 cayó en `cart.c`; memoria de desarrollo disponible ≈ 6,6 GiB, sin presión aparente. Único reintento con `-j1`: volvió a caer en `math.c`; parada reglamentaria, BE/SAN completo pendientes.
- Paquete `build/salsads-geometria-perfil.pkg`, SHA-256 `7a6a6604be3c78f52b96f26c79d2b6600c140adfa1386f9a4dca522560e13aaf`; no validado para instalar hasta completar pruebas. Logs locales en `build/geometria-perfil/`.
- Verificadores de publicación y SPDX en 0. Costo de las sondas pendiente de A/B real; sin promesa de ganancia.


## 2026-10-23 — Armado con proyección compartida (cand-v004, rc4)
- `d2608d7`: evita copias sin recorte y reutiliza la proyección de las tiras con VIEWPORT como clave; reglas de recorte, salida y DMA intactas.
- MKDS host, armado: medias de tres parejas 0,167412 → 0,155151 ms/cuadro (−7,3 %; detalle y variación en `perf-log.md`). No acredita ≤ 3 ms ni espera ≈ 0 en la PS3.
- A/B: `geometria-sin-reuso` en el directorio de datos; host `--geometria-sin-reuso` (también diferido). La ruta apagada conserva la estructura ampliada; comparar además con rc3.
- Referencia idéntica y hashes iguales con/sin reutilización: MKDS 1174, NSMB 2195 y SM64DS 2298 cuadros rasterizados por modo. Pruebas dirigidas: viewport a mitad de tira, recorte, descarte y ventanas de salida.
- Holguras SPE: geometría 144 272 B, 3D 8432 B, 3D XDR 17 856 B, 2D 9056 B y sonido 178 384 B; enlace con mínimo 8192 en todos. Compilación completa y paquete limpios; LE/SAN y unitarias BE verdes. `test-host` incompleto: GCC ppc64 cayó dos veces con `cc1: Segmentation fault` al compilar `salsads-gxparidad`; parada reglamentaria, tercer intento automático detenido.
- Paquete `build/salsads-v0.0.4-rc4.pkg`, SHA-256 `b8145eb12420c14df162739e1530dc1d29c781e198102e1541f2f24788676d02`; evidencia local en `build/rc4/`. Verificadores de publicación y SPDX en 0; validación BE de ROMs/JIT pendiente antes de instalar.
- NSMB: «línea» 45,81 ms a los 232,607 s, espera del raster 3D con mucho DMA XDR; causa probable, sin arreglo. Propuesta no bloqueante del SWAP y guion A/B de MKDS en `perf-log.md`; sin envío a consola.

## 2026-10-23 — Caché XDR con reserva local reducida (cand-v004)
- Cambio `77afc33`: 448 celdas, cuatro vías; programa SPE elegido al arrancar con `texturas-xdr`. Apagado conserva bloques y 512 celdas; encendido fuerza celdas. La unión de cachés impedía liberar memoria reduciendo solo el array.
- Host MKDS, cuadros 2200–2499: 840,5 decodificadas, 11 193,3 lecturas XDR y 53 719,5 aciertos locales/cuadro; menor tráfico que 384 y 256 celdas. Tabla en `perf-log.md`; sin extrapolar tiempos a PS3.
- Holguras: 3D XDR 17 856 B, habitual 8 432 B (mínimo 8 192 intacto); 2D 9 056 B, sonido 178 384 B, geometría 146 256 B. Presupuesto actualizado en la cabecera del SPE.
- Referencia idéntica con XDR activa/inactiva en MKDS (1 174 cuadros), NSMB (2 195) y SM64DS (2 298); MKDS apagado conserva los contadores del ejecutable anterior.
- Metroid: tres instantáneas (menú, campaña, nivel) idénticas en bloques/celdas/XDR; falta el respaldo para completar `--vs-ref3d`. Clang UBSan avisa de la resta de punteros de la cabecera, ya presente antes de este cambio.
- `tools/build.sh test-host` terminó en 0 (LE, BE, SAN, hashes y lockstep); variante reducida con ASan/UBSan también en 0. Compilación completa y paquete limpios; verificadores de publicación y SPDX en 0.
- Paquete `build/salsads-v0.0.4-rc3.pkg`, SHA-256 `66145fe496a34e924464fb22385432ad4a82fae7202f6b45e1ae4dfcad3191a3`; evidencia local en `build/rc3-local/`.
- Pendiente PS3: MKDS carrera pesada con/sin `texturas-xdr`; NSMB, SM64DS y Metroid con XDR para detectar regresiones por DMA. Sin envío ni publicación.

## 2026-10-23 — Prototipo XDR detenido por holgura del SPE (cand-v004)
- Segunda caché de celdas: 8 MiB estáticos, dos particiones exclusivas por SPE; clave de contenido comparada completa, sin falsos aciertos por colisiones.
- MKDS, host, cuadros 2200–2499: 11 101,8 → 839,2 decodificadas/cuadro (−92,4 %), 10 262,7 aciertos XDR; referencia idéntica. Detalle en `perf-log.md`.
- Activación optativa: `texturas-xdr` en el directorio de datos; host `--texturas-xdr`; log de aciertos, DMA y decodificadas.
- Dos enlaces rechazados por pila: holgura 5 248 y 5 952 B, mínimo 8 192 B. Parada reglamentaria; no se rebajó el mínimo ni se produjo rc3.
- Prototipo pendiente de reducir código/pila y completar validación del árbol final; no instalar ni medir todavía en la consola.
- Evidencia local en `build/rc3/`; no se acredita la meta de 58 campos/s ni ausencia de regresión en otros juegos.

## 2026-10-23 — MKDS: límite de la caché de celdas (cand-v004)
- Host, demostración del título sin entrada, cuadros 2200–2499, dos unidades y celdas forzadas: 11 101,8 decodificados, 10 077,8 desalojos y 82,90 % de aciertos por cuadro; mediana de tres corridas: 1,589 ms.
- Dispersión multiplicativa: 11 108,0 / 10 084,0, 82,89 %, 1,595 ms (+0,4 %); ocho vías: 11 096,3 / 10 072,3, 82,91 %, 1,638 ms (+3,1 %). Ambas descartadas: dos ideas sin ganancia clara; búsqueda detenida.
- Traza de los 300 cuadros: 8 724 celdas distintas de media (5 173–10 363), suma de ambos SPE, frente a 1 024 plazas. El 78,6 % de los decodificados son primeras visitas; ampliar la caché vaciada por cuadro no alcanza para reducirlos a la mitad en esta escena.
- Único cambio conservado: el reloj de `--r3d-win` acumula solo la ventana seleccionada, igual que sus contadores; antes dividía el tiempo de toda la corrida entre los cuadros de la ventana.
- Sin cambios en el rasterizador, DMA, presupuesto del SPE ni interruptores; sin medir en la PS3 y sin acreditar la meta de 58 campos/s. La pantalla B no se aborda por la condición de parada.
- Cifras, traza, capturas y logs locales en `build/rc2/`; validaciones y paquete en el informe de entrega.
## 2026-10-21 — Metroid Hunters: chip Flash y conservación de escrituras parciales

- Se reproduce el error físico de lectura al seleccionar archivo. AMHP estaba cargando una EEPROM de 512 B creada por una falsa detección del opcode 0Ah;
  las conversaciones reales usan direcciones de tres bytes hasta 03DF00h. `AMH` pasa a la tabla como Flash de 256 KiB, con fuentes en `THIRD_PARTY.md`.
  0Ah/0Bh compartidos por Flash y EEPROM dejan de decidir el tipo por sí solos; se conserva la detección de EEPROM por lectura corta y por tabla/configuración.
- Añadir solo el chip correcto permitía crear A, pero no conservarlo al reiniciar: PAGE WRITE 0Ah borraba toda la página antes de modificar unos bytes.
  El datasheet M45PE20 exige conservar los no dirigidos; se retira ese borrado externo. PAGE PROGRAM 02h sigue bajando bits y los borrados DBh/D8h no cambian.
  Una mutación temporal que reintroduce el borrado falla en seis comprobaciones nuevas de vecinos; la versión corregida pasa las 193 comprobaciones dirigidas
  con ASan/UBSan. No se considera suficiente verificar solo que una escritura se lee inmediatamente.
- Los archivos AMH de 512 B seleccionados por tabla se rechazan sin modificar el chip ni importar contenido (`NDS_SAVE_INCOMPATIBLE`).
  La aplicación conserva el original y usa `<código>.flash256k.sav` por separado; bloquea escrituras si la alternativa existente es ilegible o tiene tamaño incorrecto.
  El corredor del host sale con 2 antes de escribir un archivo incompatible. No se afirma recuperar partidas perdidas ni se sobrepone la tabla a una configuración explícita.
- Host nativo: se crea el archivo A y se escribe una Flash de 262144 bytes; se reinicia el proceso, se vuelve a cargar A y el archivo conserva su SHA-256
  `3294fc6c55eab5bbad2eaf3b5ee1e4c41457e8bf5658809f5a909c6e7ad3ce57`. Desde esa partida se entra al briefing, se confirma aterrizaje en Celestial Archives
  y se alcanza la vista de primera persona con HUD a los 8500 cuadros. Se conservan las entradas y capturas como evidencia local, no se incorpora material del respaldo.
  Esto demuestra acceso inicial y persistencia del archivo A, no guardado de progreso avanzado, campaña completa ni rendimiento de la PS3.
- Prueba adicional `tests/guardado-archivos`: ejecuta la sección real de guardado de la aplicación con archivos sintéticos; original intacto, alternativa nueva,
  reapertura correcta y bloqueo ante archivo alternativo inválido. Pasa nativa y Docker LE con sanitizadores, y en qemu big-endian con código compilado para Cell.
  La prueba rechaza archivos preexistentes antes de crear su fixture. La aplicación pasa comprobación de sintaxis con el compilador PPE;
  se usan encabezados generados existentes solo para resolver sus declaraciones, sin enlazar ni empaquetar programas ajenos.
- `test-host` con ambas correcciones termina con salida 0: 609 veredictos OK, ninguno FALLO, unitarias LE/SAN 8076266 y BE 8079911 comprobaciones sin fallos,
  32 comparaciones de estado iguales al intérprete y lockstep aprobado. La corrida intermedia se detuvo para no atribuir al código definitivo sus resultados anteriores.
  Fuente del arreglo en `349f164`, evidencia y capturas locales en `build/guardado-metroid-evidencia/`; revisión de privacidad sin hallazgos y respaldo original intacto.
  Sin PKG nuevo, FTP, cambios de banderas ni guardados físicos. Falta integrar sobre la candidata de captura segura y validar el paquete exacto en RPCS3 antes de otra prueba física.

## 2026-10-19 — Render 3D: texturas por celdas, reparto con historia y espera aplazada (rama r3d-reparto)

- **Qué mostró la medición** (logs de la PS3, MKDS carrera pesada): los dos SPE del 3D tardaban 12–17 ms por cuadro porque el 56–61 % del dibujo eran
  texturas: la caché de 64 bloques de 256 texeles decodificaba 2 500–3 300 bloques por cuadro (5 veces los 621 distintos que el cuadro toca) y cada uno se pide
  en ~20 de sus texeles; el reparto entre los SPE estaba a 6–9 % de desequilibrio y la espera del PPE al 3D, que se hacía en la línea 262, de 0,05 a 2 ms con picos de 6.
  El detalle (traza de texeles, caches simuladas, coeficientes de la PS3) está en `docs/fase3.md`, 3.9.
- **Hecho** (commits de la rama `r3d-reparto`): caché de **celdas de 4×4** (512 celdas, líneas de VRAM de 1 KB) con elección automática entre celdas y bloques
  (`r3d_texpol_feed`); reparto de bandas con **historia** (los SPE escriben los ticks por banda); **espera del 3D aplazada** a la línea ≤ 160 del cuadro siguiente con la pantalla
  B entregada antes (sin agregar latencia a la imagen: se explica en 3.9); caminos lentos de la textura compartidos y sonda 3 de 32 bits para que la pila conserve su holgura.
- **Interruptores en /dev_hdd0/salsads**: `texturas-bloques`, `texturas-celdas`, `r3d-sin-aplazar`, `reparto-sin-historia` (y el ya existente `reparto-alterno`). El arranque
  dice qué modo queda; `DATO raster3d-latencia` agrega la línea de espera, la composición de la A y el modo de texturas vigente.
- **Paridad**: igual a la referencia en las 40 escenas × 6 repartos × los dos modos (LE con ASan/UBSan, BE con qemu), `--vs-ref3d` en los tres respaldos del autor
  (MKDS, SM64DS y NSMB —también un nivel—), bloques y celdas contra `r3d_decode` texel a texel, `--r3d-aplazar` (el aplazamiento no cambia la imagen), RPCS3 con la autoprueba
  (celdas y bloques alternados) con la config del intérprete (con la normal el SPE del 3D muere por «Illegal instruction»/«Segfault», también en la versión anterior; con
  el intérprete, a veces —no es de esta rama: el mismo `r3d_row_at` de antes—, se repite y pasa).
- **Estimación (sin PS3)**: texturas 15–20 ms → 4–6 ms (suma de los dos SPE), SPE más lento de MKDS pesado 12–17 → ≈ 9 ms; reparto con historia a 1–2 % del ideal; SM64DS y NSMB
  siguen en bloques (sin cambio). Pendiente medir en la consola: MKDS carrera pesada, NSMB en un nivel y SM64DS, y comparar con los archivos de arriba.
- **Paquete** «SalsaDS núcleo JIT» (SLDS00009) del commit `df76468` (`build/salsads-r3dreparto.pkg`), SHA-256 `52a1e36aec21633d0e611b8888ea4fa35208470ddbe9745ab749bf0531c3ac15`;
  holgura de pila del SPE del 3D 9 360 bytes. Sin probar en la PS3.
- **Hallazgos que no son de esta rama**: (1) Metroid Prime Hunters (30 fps, 140 polígonos): los 19 ms de «línea» no son la espera del 3D (≤ 4,5 ms) sino la captura de pantalla compuesta
  **en el PPE** (`capture_frame` → `gpu2d_compose_engine`); ver 3.9. (2) La composición 2D de la pantalla B en MKDS carrera tarda 11,3 ms por cuadro (la A, 3,6): 68 % de un cuadro de un
  SPE. (3) «gx» (2,2–3,3 ms de eventos en MKDS pesado) es el PPE esperando a la geometría en su SPE (8–9 ms de SPE).

## 2026-10-19 — Chip de guardado por juego (rama guardado)

- **Problema** (PS3 real): todo juego sin `.sav` recibía EEPROM de 64 KB; Pokémon Blanco 2 (`IRD`) dio un error del propio juego sobre la
  memoria de guardado y HeartGold (`IPK`) arrancó con el chip equivocado: usan Flash de 512 KB.
- **Hecho** (detalle en `docs/guardado.md`): (1) tabla por código en `src/core/save.c` (Pokémon DS de 4.ª y 5.ª generación = Flash
  512 KB, más los de dirección de un byte de la lista blanca de DeSmuME; fuente en `docs/THIRD_PARTY.md`); (2) detección como
  respaldo por las conversaciones SPI del juego (RDID/borrados = Flash, órdenes con A8 = EEPROM 512 B, páginas de 130/131–259/>259 bytes
  = EEPROM 64 KB/128 KB/FRAM), con repetición de la conversación sobre el chip nuevo; (3) `guardado.cfg` (`CODIGO=flash512k`); (4) el log
  dice `guardado: chip <tipo> de <N> bytes (tabla|detectado|cfg|.sav|defecto)`. Un `.sav` de 64 KB de un juego que la tabla da como
  Flash se migra al chip correcto. `salsads-cpurun --save-cfg`.
- **Cobertura honesta**: la tabla es corta a propósito (~20 códigos). DeSmuME no trae una tabla de tamaños por juego (solo esa lista blanca
  y una base externa por CRC); lo que no está se deduce. No se inventaron entradas dudosas (Mystery Dungeon, Ranger, Layton, Dragon Quest, etc.).
  Una EEPROM de 8 KB funciona igual como de 64 KB, así que importa sobre todo para Flash, dirección de un byte y tamaños mayores.
- **Pruebas**: `tests/unit/test_save.c` en `test-host`; NSMB, MKDS y SM64 DS con los respaldos del autor: mismo `.sav` y mismo chip que `main`.
- **Paquete** (commit `b926885`): `build/salsads-guardado.pkg`, SHA-256 `a664cc8f5fbda838970beec9e433479399dbf621e897734ab92d4b02a169ffa3`.
- **Sin probar en la PS3**: Blanco 2 y HeartGold (sin ROMs aquí). Si algún Flash pide otro ID, `guardado.cfg` o la tabla. Los Flash de 2 MB no caben
  en `NDS_SAVE_MAX` (1 MB).

## 2026-10-19 — La FIFO de geometría en la ruta rápida (rama gx-rapido)

- **Qué dominaba** (log de la PS3 con `jit-tiempos` + perfil del host): no eran los `STR` a GXFIFO sino la **DMA de modo 7**: ~15 000
  palabras por cuadro en 33 arranques de canal en la carrera de MKDS, cada una por la ruta completa (bus de lectura, bus de escritura,
  reparto de la E/S, recorrido de los canales de DMA). El «2 µs por escritura a la E/S» del log es el CNT de la DMA, que ejecuta la
  transferencia; las escrituras de la CPU a GXFIFO eran 1 000–1 800 por cuadro.
- **Hecho** (detalle en `docs/fase4.md`, «Cuarta ronda»): ráfaga de la DMA de modo 7 dentro de `gx.c` (`gx_dma_burst`), escrituras de
  la CPU del ARM9 a 0x04000400–0x040005FF por el bus corto (`b9_w32` → `nds_gx_cpu_write`), ruta corta en `h_wr` del recompilador
  (`arec_cfg.gx_wr`) que ya no cuenta como acceso lento, y la caché `dma9_gxw` que evita recorrer los cuatro canales en cada escritura.
  Mismo tiempo emulado: ciclos, cola del motor, eventos e IRQ idénticos.
- **Interruptor**: `/dev_hdd0/salsads/gx-lento` (la forma anterior); el arranque lo dice y `jit: opciones del recompilador` lo lista.
  `DATO jit-lentos` agrega «ya no van por la ruta lenta». Host: `salsads-cpurun --gx-lento`.
- **Pruebas**: `salsads-gxparidad` (nuevo; dos máquinas con y sin las rutas cortas, 600 semillas, comparadas paso a paso al bit; en
  `test-host` LE, SAN y BE), resumen del estado con y sin `--gx-lento` en las ROMs de geometría y DMA, y los tres respaldos del autor.
- **Sin medir en la PS3.** Qué mirar (mismas escenas, NSMB nivel y MKDS carrera pesada): `DATO desglose` (ARM9 y eventos), `DATO
  jit-lentos` (promedio de µs por escritura a la E/S, y «ya no van por la ruta lenta»), `DATO jit-tiempo` con `jit-tiempos`; A/B con
  `gx-lento` en la misma escena.

## 2026-10-19 — Rendimiento del ARM9 en el recompilador (rama arm9-perf)

- **Perfil del host**: `salsads-cpurun --jit-prof` (traza de bloques en qemu-ppc64) y `tools/jit-perfil.c` (palabras por clase,
  huella de código por cuadro, modelo de L1/L2/ERAT, bloques más caros). Mostró: ~1,2 MB de código caliente distinto por cuadro en
  la carrera de MKDS (más que la L2), datos con S a 11 palabras por instrucción (banderas a memoria en cada salida), BX a 17,5, las
  secciones frías intercaladas con las calientes, un sondeo de memoria como bloque más corrido en MKDS y NSMB, y en las cargas
  91 000 lecturas de ROMCTRL por cuadro (el salto de la fase46 casi nunca actuaba).
- **Hecho** (detalle y tabla en `docs/fase4.md`, «Tercera ronda»): sondeo de ROMCTRL/SPICNT hasta el final del tramo
  (`nds_poll_skip`); banderas entre bloques en el CR; pistas estáticas en los caminos raros; búsqueda común de destinos
  indirectos; ventanas de 32 KB caliente/fría; bucles de espera que consumen el tramo; desalineación de LDR en el índice de la
  tabla; menos palabras en MUL, LDM/STM, CMP #0 y MOV; datos condicionales con S traducidos.
- **Host (qemu, modelo)**: MKDS carrera −25 % de palabras calientes, −35 % de huella, modelo 2,35 → 1,57 ms; NSMB −23 %, −31 %,
  2,18 → 1,54 ms; cargas de MKDS con el intérprete: un tercio de las instrucciones del ARM9.
- **Interruptores en /dev_hdd0/salsads** (cada uno vuelve a la forma anterior de una mejora): `jit-banderas-memoria`,
  `jit-sin-pistas`, `jit-salidas-copiadas`, `jit-sin-ventanas`, `jit-sin-esperas`; `jit-tiempos` mide dónde se va el tiempo del
  ARM9 (`DATO jit-tiempo`, `DATO jit-lentos`).
- **Paridad**: `tools/build.sh test-host` verde; NSMB, SM64DS y MKDS (1500 cuadros): `--lockstep` sin diferencias y el mismo resumen
  del estado que el intérprete en modo estricto.
- **Sin medir en la PS3.** Qué mirar: `DATO desglose` en las mismas escenas que arm7-jit; con `jit-tiempos`, el reparto del ARM9; el
  A/B de cada interruptor. Paquete «SalsaDS JIT núcleo» (SLDS00009): `build/salsads-arm9perf.pkg`, SHA-256 en el informe de la rama.

## 2026-10-19 — El ARM7 en el recompilador

- **Hecho**: el ARM7 (ARMv4T) usa el mismo recompilador que el ARM9, como instancia aparte (`n->jit7`: caché de código propia, tablas propias, mapa de
  RAM principal, WRAM compartida y WRAM del ARM7, BIOS 7 en HLE). Lo que cambia de ARMv5 a ARMv4T está en el decodificador y el emisor (`c->v5`):
  CLZ, QADD, BLX, LDRD/STRD, CP15 y BKPT son indefinidas (van al intérprete), la condición 1111 es «nunca», LDR/LDM/POP a pc no cambian de estado,
  LDRH/LDRSH impares, STM con la base en la lista y la máscara de MSR. Detalle, memoria y seguridad en `docs/fase4.md` («El ARM7 en el recompilador»).
- **Memoria**: la zona de código del texto pasa de 4 a 5 MB (4 del ARM9, 1 del ARM7); el heap suma ≈ 5,6 MB (las dos tablas de punteros de 2 MB y
  tablas acotadas; `docs/memory-budget.md`).
- **Seguridad**: `nucleo-jit.marker` protege a las dos CPUs (si la aplicación muere con código traducido de cualquiera, el siguiente arranque usa el
  intérprete en las dos); `/dev_hdd0/salsads/arm7-nojit` deja solo al ARM7 con el intérprete. La línea `jit:` del arranque dice cómo va el ARM7
  y `DATO jit7` trae sus contadores por cuadro.
- **Pruebas** (qemu-ppc64; los tiempos no valen): `tools/build.sh test-host` verde en LE, BE y SAN (BE: 8 077 525 comprobaciones de las unitarias, 0 fallos;
  `test_arec` corre ahora los perfiles ARM9 y ARM7), ROMs con `--jit`, `--jit-no9` y `--jit-no7`, `--lockstep` de las dos CPUs contra el intérprete, `ciclos --jit`,
  y el resumen del estado de la máquina (`--state-hash`) de `cpuarm`, `irq`, `periph`, `demo` y `carga` igual al del intérprete en modo estricto. Con respaldos del autor,
  1500 cuadros de New Super Mario Bros., Super Mario 64 DS y Mario Kart DS: mismo resumen (registros y contadores de las dos CPUs, RAM, WRAM, VRAM,
  paleta, OAM y las dos imágenes) con el intérprete, con los dos recompiladores y con el del ARM7 solo, todos en modo estricto.
- **Contadores del ARM7 en modo rápido** (host, tramo de 1024, 1500 cuadros; por cuadro): NSMB 192 entradas, 258 llamadas al intérprete, 893 lecturas y
  232 escrituras lentas, 27,5 k instrucciones, código 499 KB; SM64DS 150 entradas, 430 llamadas, 489 / 551, 21,6 k, 438 KB; MKDS 281 entradas, 1048
  llamadas (602 SWI de Thumb), 1755 / 580, 53,4 k, 491 KB; sin pasos del despachador (MKDS: 1437 en total) y 2 vaciados en toda la corrida.
- **Sin medir en la PS3.** Qué mirar: `DATO desglose` (ARM7 en ms por cuadro; meta < 1 ms, con el intérprete 3–4), `DATO jit7`, y la línea `jit:`. El riesgo es la
  entrada y salida: el ARM9 cuesta ≈ 9 µs por tramo en la consola (`docs/fase4.md`, segunda ronda); el ARM7 entra 150–280 veces por cuadro (≈ 143 instrucciones
  por entrada en NSMB), así que si cada entrada cuesta lo mismo serían 1,4–2,5 ms y no se llegaría a la meta **[est]**. Comparar en la misma escena con y sin `arm7-nojit`.
- Paquete «SalsaDS JIT núcleo» (SLDS00009): `build/salsads-arm7jit.pkg`, SHA-256 en el informe de la rama.

## 2026-10-18 — ROM por fragmentos (fase48)

- `src/core/cart.c`: `nds_load_rom_paged` lee la ROM por páginas (256 KB en la PS3, caché LRU de 32 MB, lectura adelantada síncrona de la página siguiente en acceso secuencial); cabecera, ARM9/ARM7 se leen al inicio sin caché. `nds_load_rom` (entera) no cambia. El host y `tests/phase1/nucleo/main.c` eligen fragmentos si la ROM pasa de 64 MB (`ROM_WHOLE_MAX_MB`) o con `/dev_hdd0/salsads/rom-paged`; host: `--rom-paged PAGE_KB,CACHE_KB`, `--rom-whole-max MB`. Presupuesto en `memory-budget.md`.
- Prueba: NSMB, SM64DS y MKDS, 600 cuadros, páginas de 4 KB con caché de 64 KB y de 256 KB con 2 MB: misma imagen (md5) y mismo estado de las CPUs que con la ROM entera. Fallos por cuadro (1800 cuadros, 256 KB, 2 MB): NSMB 0,017, SM64DS 0,010, MKDS 0,014 (10–30 páginas en total, la mayoría de la carga inicial).
- Sin probar en la PS3: ninguna ROM de más de 64 MB (no hay respaldo a mano); la lectura por adelantado es síncrona (hilo 1 / asíncrona queda abierta). `test-host` verde (LE, BE y SAN).

## 2026-10-18 — Pantallas de carga: sondeo del cartucho (fase46)

- **Medido en el host** (NSMB, partida del autor): una carga de ~60 cuadros (título, cuadros 183–245) y las de los cuadros 1233–1300 y 1600–1700
  leen 512 bytes por comando B7 con la CPU (no por DMA), palabra a palabra, y el juego gira en `ldr ROMCTRL / tst bit 23` (60–70 % de las
  instrucciones del ARM9 en esos cuadros); el resto es trabajo del juego (rellenos y descompresión). El tiempo emulado es el de la DS real
  (~6,7 MB/s): ~9 000 bloques en 1 700 cuadros. Host: 4,6 ms por cuadro de carga contra 1,4 ms ociosos.
- **Hecho**: al leer ROMCTRL con el bloque esperando palabra o fin, el reloj de esa CPU avanza hasta el siguiente evento (tope `ev_min`) en vez de
  ejecutar el bucle; mismo tiempo para el juego, ARM9 −9 % de instrucciones en NSMB (1 700 cuadros), −21 % de tiempo host en la carga del título.
  `test-host` verde (la ROM `periph` y los tamaños de tramo sin cambios).
- **Paquete «SalsaDS núcleo» con el JIT (SLDS00009)** del commit `80f3332`: `build/salsads-nucleo-jit-fase46.pkg`, SHA-256 `1582a43f383eee6a1dded47dafe54438f0cca5beff2e5d7a75eff9aa13c06eed`. Sin probar en la PS3: medir la carga del título de NSMB.
- No se tocó la latencia emulada del cartucho: acortarla sería adelantar la carga respecto a la DS real (opción posterior, con interruptor).

## 2026-10-18 — Tallos inclinados de NSMB (3.7)

- **Tallo inclinado, dientes de los segmentos de arriba: arreglado** (escenas 38 y 39 de `gx3d`, nuevas; la 38 pasa de 234 píxeles distintos
  con melonDS a 0). La partida del autor lleva el host al 1-3 con el guion de 3.6 y, desde el cuadro 2780, la plataforma se inclina (volcado
  con `--frame-dump`). Causa: en las líneas donde la arista derecha es x-mayor y va hacia la izquierda el denominador de la interpolación contaba solo hasta el final del tramo
  dibujado; melonDS cuenta hasta el extremo de afuera del tramo (`r3d_row_at`, solo ortográfica). De paso: escena 1: 828 → 10, 14: 616 → 399,
  20: 299 → 42, 21: 3629 → 2144; las demás igual (`docs/fase3.md`, 3.7).
- **Mapa con cámara en movimiento (rayas verdes): sin arreglar.** Reproducido (guion sin entrar al nivel: izquierda desde el 1400 y derecha desde el
  1700; rayas en el cuadro 1755) y trasladado entero (23 polígonos en orden) a la escena 40 de `gx3d`: melonDS esconde el parche donde el suelo
  es casi coplanar y la DS real no (foto), así que melonDS no sirve de referencia ahí; no se tocó la profundidad. Siguiente: volcar profundidad por píxel en
  la PS3 o usar otro emulador de la DS real.
- **Paquete «SalsaDS núcleo» con el JIT (SLDS00009)** del commit `97d0337` (más este renglón): `build/salsads-nucleo-jit-fase37.pkg`, SHA-256
  `e3c5bc7cc2a9cc0ea42e3ba847d459a9f083883b5a6c0f2f2360d9598073c250`; holgura de pila del SPE del 3D 12272 bytes. Sin probar en la PS3: mirar el tallo inclinado del 1-3.
- Pruebas: `gx3d.golden` con 41 escenas (1, 14, 20 y 21 cambiadas; 38, 39 y 40 nuevas), ROM `gx3d.nds` reconstruida.

## 2026-10-18 — NSMB 1-3: tallos y nubes (3.6)

- **La partida del autor** (`A2DE.sav`, la de las 20:13; la de las 13:42 estaba recién formateada y no servía) lleva el host al 1-3:
  `salsads-cpurun --save COPIA --press 950:0:120,56 --press 960:0 --press 1100:0:90,54 --press 1110:0 --press 1500:1 --press 1510:0`
  (Mario Game, Archivo 1, A en el nodo), y desde el 2600 con `--press 2600:12` (derecha y B) salen los hongos que se inclinan.
- **Tallos «resorte»: arreglado** (contra melonDS, escenas 35 y 36 de `gx3d`, y se ve liso en el host): tres reglas de `r3d_px.h` para las
  aristas compartidas x-mayores (final exclusivo de la derecha y-mayor que va a la izquierda en su primera línea, sin píxel de «línea vacía»
  en la última línea de un polígono con izquierda x-mayor, y atributos en la línea siguiente para la izquierda x-mayor que va a la derecha
  con la derecha y-mayor). Detalle y números en `docs/fase3.md`, 3.6. De paso mejoran las escenas 0, 13, 14, 20, 22 (146 → 0), 23, 24, 25 y
  las de profundidad; ninguna empeora salvo la 34 (+1 píxel).
- **Nubes del 1-3: arreglado, no era del 3D**: era el fondo BG3 (extendido de 16 bits con paleta extendida). El juego escribe `BG3X` en cada
  línea y la recarga reponía también la Y interna: una sola fila de mapa repetida, una columna plana pálida. Escribir `BGxX` recarga solo
  la X interna y `BGxY` solo la Y (como melonDS), en el compositor y en su referencia.
- **Mapa del mundo 1 con la cámara en movimiento: sin arreglar** (reproducido en el host, cuadros 1840–1858 del recorrido 1-3 → hacia el
  nodo 1-1 y de vuelta; el parche de tierra con rayas verdes de 10–30 píxeles en su borde de arriba). La escena 37 de `gx3d` lo toma del
  cuadro; melonDS pierde casi todo el segundo triángulo del parche y SalsaDS no (337 píxeles); en la DS real el parche sale completo, así
  que falta fidelidad en la escena. Siguiente paso: volcar el cuadro completo (todos los polígonos de la zona, también los translúcidos
  y su orden) en vez de solo suelo y parche, o medir la profundidad por píxel del suelo en melonDS.
- **Captura de pantalla (DISPCAPCNT)** en `src/core/video.c` (ver `docs/fase3.md`): fuentes A (compuesta o 3D) y B (VRAM o FIFO), mezcla, tamaños y bancos, un solo cuadro. `test-host` verde. Paquete con el JIT (SLDS00009): `build/salsads-nucleo-jit-fase25.pkg`, SHA-256 `9613367fe9a8a6f44a013e4323d8495823babb06daa7d12adc5e209261894d59`. Sin probar en la PS3: Mario Kart DS, presentación del circuito.
- **Paquete «SalsaDS núcleo» con el JIT (SLDS00009)** del commit `577e5f8` (más este renglón): `build/salsads-nucleo-jit-fase44.pkg`, SHA-256
  `f0720e18b2fce0c8f415faff7de463bf5b272049760b25ffa856fd94b0a79157`. Sin probar en la PS3: mirar los tallos y las nubes del 1-3 y el mapa
  con la cámara en movimiento (las rayas del parche deberían seguir).
- Pruebas: `test-host` verde (LE y BE, 8 075 418 y 8 076 520 comprobaciones), `--vs-ref3d` y `--vs-ref` iguales con NSMB (1-3 y el mapa), MKDS y SM64DS; `gx3d.golden` con 38
  escenas (15 cambiadas) y el hash de la semilla 7 de `test_render3d` actualizado.

## 2026-10-18 — Geometría: segunda vuelta (3.5b)

- **Medido en la PS3**: SPE 35,3 ms por cuadro en NSMB (26 363 órdenes, 1227 polígonos), PPE 13,1 ms anotando + 6 ms esperando; con la
  geometría directa en el PPE la cinemática de NSMB cuesta 22,4 ms. Ver `docs/fase3.md`, 3.5b.
- **Hecho**: el cuello era el motor de tiempo (`gx_advance` ≈ 4 veces por palabra de FIFO recalculando la siguiente orden): caché `nxt`,
  salida inmediata, `nds_gx_schedule` sin trabajo con una mirada pendiente, tablas de órdenes estáticas (host: emulación completa de NSMB
  −13 %). Armado con una división por coordenada. En el SPE: matrices, vértices y normales en vectores de cuatro, productos de 32 × 32 → 64
  con multiplicaciones de 16 bits, cocientes con coma flotante y corrección, sin `__muldi3`/`__divti3`; ventanas y trabajos más grandes;
  medición por etapa (`DATO geometria-spe`). Mismos bits (unitarias con la versión del SPE en el host, hash de cuadros igual).
- **Paquete «SalsaDS núcleo» con el JIT (SLDS00009)** del commit `9b4f0f3`: `build/salsads-nucleo-jit-fase35b.pkg`, SHA-256
  `0fdf76ac421e880d59ad90fd607ecbcfa35ea1af78cb523e1445869a91a28a34`. Geometría en el PPE por omisión; con el archivo
  `/dev_hdd0/salsads/geometria-spe` va al SPE y el log trae el desglose por etapa. En RPCS3: `RESULTADO nucleo: OK`; autoprueba del SPE igual.
- Pendiente: medir en la PS3 (con y sin `geometria-spe`); decidir el valor por omisión con ese número.

## 2026-10-18 — Eventos y geometría en un SPE (Fase 3.5)

- **Medido en la PS3 con el JIT** (SM64DS en juego): emulación 26,6 ms = ARM9 13,3 + ARM7 2,3 + eventos 10,4 + planificador 0,6; en otras
  escenas ARM9 17,4 y eventos 5,4 (misma suma). La geometría costaba 7,0 ms por cuadro en el PPE (≈ 670 polígonos). El reparto
  ARM9/eventos se movía porque el motor de geometría es perezoso y lo ejecuta quien lo mira primero (la CPU o el evento `EV_GX`).
- **Eventos**: de 4925 por cuadro, 4363 eran miradas de `EV_GX` cada 256 ciclos. Ahora solo se mira cuando el nivel de la FIFO
  puede haber cambiado (o toca repetir la IRQ 21), y el evento de HBlank solo se arma si alguien lo usa. Por cuadro en el host: SM64DS
  5039 → 1497, NSMB 5031 → 2304, MKDS 3183 → 1437. Contadores por tipo de evento (`DATO eventos`, `cpurun --stats --gx-clock`). Las
  ROMs `irq`, `dma`, `ipc`, `periph`, `gx`, `geom` siguen en OK con todos los tamaños de tramo; cambia el intercalado fino de las
  CPUs (fronteras de tramo) y la IRQ 21 por nivel ya no sale antes de que se cumpla el nivel (detalle en `docs/fase3.md`, 3.5).
- **Geometría**: motor de tiempo (PPE) y ejecución separados (`gx.c` / `gx_exec.c`); en modo diferido las órdenes se anotan en un
  registro y un SPE (el sexto, antes libre) las ejecuta por lotes con el mismo código; el PPE espera solo si la CPU lee un resultado de
  la ejecución o en el SWAP_BUFFERS. Mismos bits que el modo directo: pruebas unitarias nuevas (`test_gx_defer`, `test_gx_job`), ROMs
  `gx`/`geom` con `--gx-defer` en LE y BE, hash de cuadros (`--gx-hash`) igual en directo y diferido en SM64DS/NSMB/MKDS (4000 cuadros) y
  `--vs-ref3d` igual; autoprueba del SPE al arrancar (3 registros al azar, vértices/polígonos/resultados iguales). `test-host` verde
  (LE, BE, sanitizadores, recompilador, lockstep).
- **Paquete «SalsaDS núcleo» con el JIT (SLDS00009)** del commit `ef37479`: `build/salsads-nucleo-jit-fase35.pkg` (copia de
  `build/salsads-nucleo-jit/salsads-nucleo-jit.pkg`), SHA-256 `c1e0aa503a86885e8827b814ce9d1474921bd36276f78c65be8481b537ebf7bc`. En
  RPCS3 (SM64DS, 400 cuadros): autoprueba de geometría igual y `RESULTADO nucleo: OK`. Líneas nuevas del log: `DATO eventos`,
  `DATO geometria-spe` (trabajos, órdenes y subidas DMA por cuadro, ms del SPE, ms y número de esperas del PPE); `DATO geometria` ahora
  mide lo que el PPE gasta anotando y publicando. **Sin medir en la PS3**: expectativa PPE ≈ 7 → ≈ 1 ms por cuadro; falta ver los ms del
  SPE (código escalar de 64 bits, `__divti3` en el recorte) y cuánto espera el PPE en el VBlank.
- Pendiente: medirlo en la consola; si el SPE pasa de ≈ 8 ms por cuadro, partir el trabajo (transformación/iluminación aparte del
  recorte) o vectorizar la transformación de vértices; el tiempo de `frame_begin` (6 ms por cuadro en el desglose de la PS3) es otra
  historia por mirar; `gx_t` creció 128 KB (registro de órdenes).

## 2026-10-18 — Recompilador del ARM9, segunda ronda (Fase 4.1)

- **Medido en la PS3 con la primera versión** (SM64DS en juego): ARM9 13,3 ms por cuadro de trabajo (17–22 en escenas
  pesadas), 1716–4170 entradas por cuadro, 600–1985 salidas por destino indirecto no encontrado, 1091–2174 por presupuesto,
  ~1180 llamadas al intérprete. El tiempo del ARM9 sigue a los tramos (~9 µs por tramo), no a las instrucciones.
- **Hecho** (`docs/fase4.md`, «Segunda ronda»): en línea MCR p15 c7, MRC p15 c1/c9, MRS, MSR CPSR_c sin cambio de modo y MSR
  SPSR, SWP/SWPB y LDRD/STRD; la tabla de destinos indirectos se rellena después de un fallo; LDR relativo al pc leído al
  traducir; tramo de 1024 ciclos en el paquete (`nucleo-jit-tramo` lo cambia); línea `DATO jit-llamadas`.
- **En qemu, 1500 cuadros, por cuadro**: llamadas al intérprete 1819 → 411 (NSMB), 2968 → 248 (MKDS), 773 → 242 (SM64DS);
  salidas por destino no encontrado 1054 → 30, 337 → 3, 177 → 17; entradas 4508 → 2024, 2181 → 378, 2057 → 1142; palabras PPC
  por instrucción 15,6 → 14,0, 15,1 → 13,5, 15,2 → 13,7. Misma imagen final que antes con el mismo tramo; `--lockstep` sin
  diferencias en los tres juegos con 128 y con 1024; `tools/build.sh test-host` verde.
- **Precisión del tramo de 1024**: las CPUs se ven hasta ~1000 ciclos tarde; los bucles de sondeo dan más vueltas (lecturas
  lentas ×1,5–3, ARM9 +2–10 % de instrucciones). En NSMB y SM64DS los tramos los cortan los eventos (~5100 por cuadro, del motor
  de geometría): solo bajan a la mitad. El siguiente paso para eso es del planificador (no se tocó `src/core/sched.c`).
- **Paquete «SalsaDS JIT núcleo» (SLDS00009)**: `build/salsads-nucleo-jit/salsads-nucleo-jit.pkg` del commit 5f2f3ce, SHA-256
  `a5788b65e1b2e252c7face21f40180e25b420448da24196139064b88970b85ae`.
- Pendiente: medir en la consola (mismas escenas, y `nucleo-jit-tramo` con 128 para comparar); LDM/STM a la E/S, CMP
  condicionales, entrada y salida más baratas.

## 2026-10-18 — Dos fallos de New Super Mario Bros. vistos en la PS3 (rama fase24)

- **Mapa del mundo 1, líneas verdes sobre el camino de tierra: arreglado** (no era del 2D; era el 3D). El parche de tierra (cuadrilátero
  translúcido, W-buffer) y el suelo (triángulo opaco con aristas x-mayores) están casi en el mismo plano; la w de las aristas
  x-mayores se interpolaba por la x del píxel entero y saltaba entre líneas, de modo que en unas líneas la profundidad de los dos
  empataba y el parche perdía. Ahora la w y la z de esas aristas siguen la y (línea siguiente), medido contra melonDS; y los
  empates de profundidad con «menor» los gana un polígono frontal sobre uno de espalda, como melonDS. Detalle, escenas nuevas de
  `gx3d` (26 a 34) y lo que queda abierto: `docs/fase3.md`, 3.5.
- **Nivel 1-3, nubes del fondo como un bloque claro con bordes rectos: sin reproducir.** El host llega al mapa con un guion de
  teclas (`--press`, ahora hasta 512 entradas; el nivel 1-1 corre, con saltos y carrera), pero para llegar al 1-3 hay que pasar el 1-1
  y el 1-2 jugando y el guion ciego muere antes. Hace falta la partida del autor (`/dev_hdd0/salsads/saves/` en la PS3, 8 KB):
  con ella `salsads-cpurun --save PARTIDA --press ...` y `tools/melonds-shot.py` llevan a la escena. Lo único probable sin la
  escena: el bloque es del 3D (las plataformas en forma de hongo del nivel son 3D) y la regla de empates nueva (frontal sobre
  espalda) podría arreglarlo; **sin confirmar**. Si no, las pistas del autor (nube solo arriba y el resto cielo) apuntan a una
  capa que se repite en vertical o a una ventana: revisar con `tools/gfx-bisect.py` sobre el cuadro de esa escena.
- Herramientas: `--press` admite 512 entradas (antes 64). Escenas 26 a 34 de la ROM `gx3d`.
- Pruebas: `tools/build.sh test-host` verde (LE, BE y sanitizadores); `gx3d.golden` con 35 escenas; MKDS, SM64DS y NSMB con `--vs-ref3d`
  y `--vs-ref` iguales. SPE del 3D: texto 58 296 B, pila con 16 464 B libres (holgura 9 696 B; `r3d_end_wz` va fuera de línea: en
  línea la holgura bajaba a 7 392 y el enlace lo rechazaba).
- **Paquete «SalsaDS JIT núcleo» (SLDS00009)** con el recompilador, del commit e13daba: `build/salsads-nucleo-jit-fase24.pkg`
  (copia de `build/salsads-nucleo-jit/salsads-nucleo-jit.pkg`), SHA-256 `cbf36d479d932a3afb647bd4e859a7f856ee107248536016c747178a6a0b8ae1`.
  **Sin probar en la PS3** (ni en RPCS3): falta confirmar que el mapa del mundo 1 ya no tiene líneas y mirar el nivel 1-3.

## 2026-10-18 — Recompilador del ARM9 (Fase 4)

- **El ARM9 corre con un recompilador ARMv5TE (ARM y Thumb) → PowerPC64** propio (`src/jit/arec*`, conectado a la máquina en
  `src/core/jit.c`): bloques por dirección y estado, enlace directo, tabla de destinos indirectos, presupuesto de ciclos en el
  prólogo, banderas NZCV como comparaciones en cr2–cr4 (en memoria solo donde las lee una llamada al intérprete o una salida,
  con vista adelante del código de destino), ruta rápida por una tabla de páginas de 16 KB (RAM, WRAM, DTCM, ITCM) y lenta por
  el bus, invalidación por escrituras a código (bus, ITCM, DMA, HLE), CP15/MSR/SWI/LDRD/SWP… por el intérprete desde el bloque.
  El ARM7 sigue con el intérprete. Diseño, pruebas y números en `docs/fase4.md`.
- **Correcto contra el intérprete en qemu-ppc64**: 800 programas al azar de ARM y Thumb y casos dirigidos (`test_arec`); las 12
  ROMs propias con `--jit` y con `--lockstep` (otra máquina con el intérprete, comparadas después de cada bloque); New Super
  Mario Bros., Mario Kart DS y Super Mario 64 DS 1500 cuadros en `--lockstep` **sin diferencias** (99, 53 y 56 M comparaciones).
- Modo rápido (el de la PS3): un bloque puede pasarse del final del tramo; para que eso no rompa las esperas por tiempo, los
  desbordes de temporizadores vencidos se atienden al leer o escribir un temporizador (la ROM `irq` fallaba sin eso; con el
  intérprete ningún resultado cambia).
- **Paquete «SalsaDS JIT núcleo» (SLDS00009)**: `build/salsads-nucleo-jit/salsads-nucleo-jit.pkg` (`tests/phase4/nucleo-jit`) del commit
  7a90a6a, SHA-256 `400704fe1253af680131e890c02fd81e69264aa9f6fc17909e7919ac3ebe726a`,
  con la línea `DATO jit` y el ARM9 en `DATO desglose`. En RPCS3 corre el respaldo (intérprete) y da `RESULTADO nucleo: OK`.
  **Sin medir en la PS3**: la meta es el ARM9 bajo ~8 ms por cuadro (44–49 ms con el intérprete).
- Pendiente: medir en la consola; el ARM7 en el recompilador; en línea las llamadas al intérprete frecuentes (CP15 de caché,
  SWP, MRS/MSR); caché de código más grande o con desalojo parcial (SM64DS usa 2,8 de 4 MB en 1500 cuadros).

## 2026-10-18 — Rendimiento del 3D y juntas entre triángulos (Fase 3.4)

- **Medido en la PS3 antes** (`DATO raster3d`, 178 muestras): New Super Mario Bros. (nivel) 26,7 ms por SPE, Super Mario 64 DS
  36–37 ms (el PPE esperaba 29–32 ms al 3D), Mario Kart DS 6,6–15 ms. Un ajuste de las columnas da **210 ns por píxel** (≈ 670
  ciclos, el bucle de la línea escalar con ~10 saltos sin predicción por píxel), **73 µs por bloque de texturas decodificado** y 3,7 µs
  por polígono·banda; los fijos eran ≈ 1 ms. No era un costo por banda: esas escenas dibujan 130–180 mil píxeles por cuadro.
- **Hecho**: línea del 3D en vectores de 4 píxeles (`src/gpu3d/r3d_vrow.h`, mismos bits que `r3d_draw_row`, que sigue de referencia),
  decodificación de bloques de texturas por tramos con la paleta convertida en el local store (el formato 4x4 por tile), z de las
  aristas sin aritmética de 64 bits y cadena de aristas sin módulo, borrado con vectores, y el `DATO raster3d` con líneas de polígono
  y el reparto `polígonos / aristas / píxeles`. Local store: texto 56 032 B, pila con 18 768 B libres (holgura 12 048 B). **Esperado en la
  PS3: 3,5–6 ms por SPE en SM64DS y NSMB (meta ≤ 8); sin medir.** En el host SM64DS 1,63 → 0,83 ms, MKDS 2,44 → 1,94 ms.
- **Juntas entre triángulos**: la causa era el **recorte** (`gx.c`), no las reglas de relleno: el vértice nuevo se interpolaba en el
  sentido en que cada polígono recorría la arista (dos polígonos que comparten una arista recortada sacaban vértices a un píxel de
  distancia) y no quedaba exactamente sobre el plano. Ahora se interpola de adentro a afuera y queda en ±w. Mario Kart DS: de 34 908
  píxeles con agujeros en 1 283 cuadros a 19 en 3; escena nueva 25 de `gx3d` (malla en perspectiva recortada) contra melonDS de 458
  píxeles distintos (358 sin dibujar) a 27 (ninguno sin dibujar). Las reglas de relleno no dan juntas: 17 millones de píxeles de
  1 200 mallas al azar, 0 sin cubrir. De paso: las aristas de 45° hacia la izquierda son x-mayores (escena 14 de 930 a 647 píxeles
  distintos con melonDS). Detalle en `docs/fase3.md`, 3.4. **Falta confirmar en la PS3 que SM64DS ya no muestra juntas** (en el host
  no se llega a una zona de juego sin entrada).
- **Pruebas**: `test-host` (LE con sanitizadores y BE; la línea vectorial contra la escalar en 392 000 líneas, mallas sin juntas, recorte
  con aristas compartidas, bloques de texturas contra `r3d_decode` texel a texel, ROM `gx3d` con 26 semillas); juegos con `--vs-ref3d`
  iguales (SM64DS, MKDS, NSMB) y 0 diferencias de bloques; RPCS3: autoprueba 3D igual y `RESULTADO nucleo: OK`. Cambió el hash de la
  semilla 7 de `test_render3d.c`, `gx3d.golden` (escenas 0, 5, 6, 13, 14, 20 y las nuevas 23 a 25) y la ROM `gx3d`.
- Herramientas: `gfx-compare.py --mel-cache`, `CPURUN_OUT`/`CPURUN_CFLAGS` en `cpurun.sh`, `salsads-cpurun --holes/--frame-dump`,
  `tools/ppm-png.py`.
- **Paquete** «SalsaDS núcleo» (SLDS00008) `build/salsads-nucleo-fase34.pkg` del commit `2945dba`, SHA-256
  `73e6e3783dd1816c85645aa0bb70b518e23d2af98c73fdae50aaa58d09b566fe`. En RPCS3: autoprueba 3D igual (sondas y 4 escenas) y
  `RESULTADO nucleo: OK` con SM64DS, MKDS y NSMB.
- **Queda**: medir en la PS3; si el número de `píxeles` o `aristas` sigue alto, desplegar dos grupos de píxeles por vuelta (la cadena
  de factor → textura → color es larga) o calcular las aristas una vez por polígono·banda; el borde y la niebla (`posproceso`) siguen
  escalares (≈ 0,4 ms por SPE con edge marking).

## 2026-10-18 — Fidelidad del 3D (Fase 3.3)

- **La estrella de «Touch to start» de Super Mario 64 DS**: dos errores del motor de geometría (`gx.c`). VTX_DIFF sumaba la
  diferencia por 8 (se suma tal cual: los ojos eran ocho veces más grandes) y las coordenadas de textura generadas (la textura de
  entorno de la estrella) no seguían el hardware: el modo 2 se calcula al llegar NORMAL con `>> 21` y sin traslación (antes `>> 17`
  con traslación), el 3 con `>> 24` en cada vértice, el 1 en TEXCOORD sin multiplicar la traslación por 16. Hallado volcando las
  órdenes del cuadro y comparando con melonDS (escena 15 de `gx3d`, idéntica). Pruebas en `test_gx.c`.
- **Regla de interpolación del color** (medida con las escenas 16 a 22 de `gx3d`, que aíslan línea, aristas, perspectiva y aristas
  inclinadas): color con 3 bits más y fracción llena salvo en el 0; lineal exacto con las `w` iguales; con `w` distinta, P = 8 en la
  línea; un píxel más de denominador con la arista derecha inclinada y y-mayor; aristas x-mayores por la y en el extremo de afuera;
  decal con alfa 0 y 31. Contra melonDS: Gouraud de 14 001 a 828 píxeles distintos, toon y highlight de ~4 000 a **0**, escenas
  15 a 18 idénticas (detalle y lo que queda en `docs/fase3.md`, 3.3).
- **Queda**: el color de los triángulos con aristas x-mayores (±1 en ≈ 40 % de los canales de la escena 21), niebla (escena 9),
  bordes de un píxel, antialiasing, imagen de fondo y la captura de pantalla DISPCAPCNT (no hecha; plan en `docs/fase3.md`).
- **Pruebas**: `test-host` verde (LE con sanitizadores y BE; ROM `gx3d` con 23 semillas e imágenes de referencia nuevas); juegos sin
  regresión (`--vs-ref3d` igual en SM64DS, MKDS y NSMB). Cambió la ROM `gx3d` (escenas 15 a 22), no `geom`.
- **PS3**: el SPE del 3D usa las mismas reglas (texto 47,1 KB, pila libre 23,9 KB; `r3d_end_at` y `r3d_dda_init` salieron de línea para
  no inflar el bucle). Paquete «SalsaDS núcleo» (SLDS00008) `build/salsads-nucleo-fase33.pkg` del commit 9ce969d, SHA-256
  `8cb4badd2f5921f5dae7af47b0d9d4eb111ac15a91aa2429e6c461455ac6ea2b`. En RPCS3: autoprueba 3D igual y MKDS 400 cuadros `RESULTADO nucleo: OK`. **Sin medir en la PS3** (el bucle sigue escalar).
- Aparte: `tests/unit/core` (volcado de memoria de qemu, 1,8 MB) está commiteado por error en `main`; lo regeneran las corridas BE.

## 2026-10-18 — Motor de render 3D (Fase 3.2)

- **Rasterizador de la DS** sobre `gx_frame` (`src/gpu3d/r3d_*`, código propio desde GBATEK y melonDS ejecutándolo): orden de
  opacos y translúcidos, aristas en punto fijo ajustadas a melonDS, interpolación con corrección por w, Z/W-buffer, prueba
  menor/igual, translúcidos con id, alpha test, modulate/decal/toon/highlight/sombras, los 7 formatos de textura con
  repetir/espejo/recorte, edge marking y niebla. **Referencia** (cuadro entero) y **código del SPE** (bandas de 16 líneas, DMA por
  listas, caché de bloques de texeles decodificados con LRU) comparten las reglas en enteros de 32 bits y dan **los mismos bits**
  en el host LE, qemu BE, el PPE y el SPE. Pendientes: antialiasing, imagen de fondo, polígonos de un punto.
- **Capa 3D** como BG0 del motor A en el compositor 2D (SPE y referencia): BG0HOFS y la mezcla del 3D con el objetivo 2 con su
  propio alfa. Se dibuja en la línea 0 del cuadro siguiente al SWAP_BUFFERS y se espera en la línea 192 (detalle en `docs/fase3.md`).
- **Pruebas**: unitarias (40 escenas × 4 repartos, LE y BE), ROM `gx3d` (15 escenas, en `test-host` con imagen de referencia) y
  contra melonDS: 5 escenas idénticas (formatos de textura, translúcidos, sombras, 3D sobre 2D, prueba «igual»); el resto difiere
  en bordes (≈ 5 % de las líneas, un píxel) y en el color de Gouraud (±1 de 5 bits en ≈ 30 % de los canales).
- **Juegos** en el host: Mario sale dibujado en New Super Mario Bros. 1-1 y la carrera de Mario Kart DS se ve; la estrella de Super
  Mario 64 DS sale rota, pero sus vértices ya vienen mal del motor de geometría (pendiente de la 3.1). Host: 0,95 / 3,30 / 1,52 ms
  por cuadro.
- **PS3**: dos SPE para el 3D (2D A, 2D B, sonido, 3D × 2: queda un SPE libre), autoprueba al arrancar (sondas de aritmética y 4
  escenas contra el PPE) y `DATO raster3d`. Paquete «SalsaDS núcleo» (SLDS00008) del commit `211d01e`:
  `build/salsads-nucleo-fase32.pkg`, SHA-256 `77fa46e6972291609050993e2996ffea5ab3f4768fcc67832ec220bc9d1fa046`. En RPCS3:
  autoprueba igual, verificación durante la prueba igual, `RESULTADO nucleo: OK`. **Sin medir en la PS3**: el bucle de la línea es
  escalar y puede ser lento en el SPE; si pasa de ≈ 6 ms por SPE en MKDS, vectorizarlo.
- Arreglos de paso: desplazamientos de valores con signo en `gx.c` (UBSan). Herramientas: `tools/r3d-fit.py`,
  `tools/melonds-shot.py`, `rpcs3-run.py --rom/--frames`, `salsads-cpurun --ref3d/--vs-ref3d/--r3d-stats/--img3d`.

## 2026-10-17 — Motor de geometría (Fase 3.1)

- **El motor de geometría real** (`src/gpu3d`, C portable y sin dependencias; pegamento en `src/core/geom.c`) sustituye a la
  FIFO «siempre vacía»: GXFIFO empaquetada y puertos de órdenes, FIFO de 256 entradas con tubería de 4 y GXSTAT con niveles,
  ocupado e IRQ 21 por nivel; DMA de modo 7 que respeta la mitad de la FIFO; matrices (modos 0 a 3, pilas con error), vértices,
  BEGIN/END con los cuatro tipos de primitiva, iluminación, coordenadas de textura, recorte contra los seis planos, viewport,
  BOX/POS/VEC_TEST, RAM_COUNT y SWAP_BUFFERS en el VBlank. Deja por cuadro una lista de polígonos en pantalla (`gx_frame`, doble
  búfer, 32 B por vértice y 48 por polígono, sin punteros) lista para los SPE de la 3.2. Detalle, formatos y redondeos en
  `docs/fase3.md`.
- **Contrastado con melonDS** (sólo ejecutándolo): la ROM `geom` (2878 comprobaciones) da lo mismo en los dos salvo el tiempo de la
  FIFO, las lecturas de 32 bits de VEC_RESULT y la pila de proyección tras errores repetidos. Matrices (≈ 2000 comprobaciones
  aleatorias), pilas, recorte, caras y la cuenta de vértices de las tiras (se comparten) coinciden. Pruebas unitarias del motor en
  LE y BE.
- **Juegos**: New Super Mario Bros. (menú, archivo, mapa y 1-1), Mario Kart DS y Super Mario 64 DS siguen llegando adonde
  llegaban; NSMB manda ≈ 480 polígonos por cuadro en 1-1 (máximo 1895) y la geometría cuesta ≈ 0,64 ms por cuadro en la Mac.
- Paquete «SalsaDS núcleo» (SLDS00008) con `DATO geometria` (tiempo del motor en el PPE con el reloj de perfil): `build/salsads-nucleo-fase31.pkg`
  (commit `9e1c73f`), SHA-256 `1dc3567fe7eeca57721f82bdb6da9d9e0db48ce31005322f43dbba8e156d6e81`; compila y enlaza, **no probado en RPCS3 ni medido
  en la PS3**; el PPE es varias veces más lento que la Mac y la decisión PPE/SPE del motor depende de ese número.
- Pendiente: la 3.2 (rasterizador en SPE sobre `gx_frame`, texturas y paletas, orden de translúcidos, SWAP_BUFFERS con rasterizador
  asíncrono); medir el motor en la PS3; el modelo de iluminación y el recorte están sin contrastar con una DS real (correr `geom` ahí).

## 2026-10-17 — FIFO de geometría, sonido y compositor rápido (Fase 2.2)

- **New Super Mario Bros. ya entra a un nivel en el host.** El bucle de 620 883 instrucciones por cuadro no era el sonido: el
  ARM9 esperaba a `GXSTAT` (FIFO de comandos con menos de la mitad) y a la IRQ 21 para mandar su lista de geometría por DMA. Ahora
  la FIFO de geometría existe como «siempre vacía» (se traga las órdenes; IRQ 21 por nivel como en melonDS): título, menú,
  selección de archivo (táctil), intro, mapa y 1-1 con el reloj corriendo, a 91–94 MIPS. Sin 3D no se ve a Mario (Fase 3).
- **SWI 1Ah–1Ch** (tablas de seno, tono y volumen) generadas por fórmula y comprobadas contra las 1556 entradas de melonDS.
- **Sonido**: los 16 canales (PCM8/16, IMA-ADPCM, PSG, ruido), SOUNDCNT/SOUNDBIAS y las capturas, en `src/audio` (portable) con
  un SPE de mezcla (`src/spu/snd`), remuestreo lineal a 48 kHz en flotante (con el punto marcado para un FIR polifásico) y salida de
  audio de la PS3 (`src/platform/ps3/audio_out.c`). La ROM `snd` captura lo que sale del mezclador por hardware y
  `tools/snd-compare.py` lo compara con melonDS muestra a muestra: iguales, con dos divergencias documentadas (`docs/fase2.md`).
  Autoprueba del SPE contra el PPE al arrancar (0 diferencias en RPCS3).
- **2D en el SPE**: los 28 ms de la pantalla A y 8 de la B eran código escalar (multiplicación de 64 bits y división por
  software en los fondos afines; color final de un píxel por vuelta con una docena de saltos): vectorizado, y una vía rápida para
  los fondos que solo se desplazan. En RPCS3 (que cuenta instrucciones) con NSMB: A 14,8 → 4,7 ms, B 1,3 → 1,2; la PS3 lo dirá
  (`DATO spe-etapas`). Instrumentación por etapa en el SPE y contadores de DMA en el host.
- Herramientas: `--press`/`--shot` (entrada y capturas por guion), `--wav`, `--g2d-stats`, `--trace-from`, `--dump-cpu`, TCM visibles.
- Paquete «SalsaDS núcleo» (SLDS00008) con todo lo anterior: `build/salsads-nucleo-fase22.pkg` (commit `c4a738b`), SHA-256 `cce7721a908a362ee3aed51d673348361a34bb10a5fd9a33b20f67de0e37a601`; en RPCS3 OK.
- Pendiente: medir en la PS3 (2D por etapa, audio con faltas, 10 bits), 3D (Fase 3), FIR del remuestreo, HOLD y volumen general
  contra una DS real.

## 2026-10-17 — Motores 2D A y B (Fase 2.1)

El compositor de la 0.5 es ahora el renderizador de las dos pantallas: registro de cambios por línea en el núcleo
(`src/core/video.c`), `src/gpu2d/gpu2d_compose.c` (SPE y host, mismos píxeles), referencia independiente y un SPE por
pantalla en la PS3. Fondos de texto, afines y extendidos, OBJ de todos los tipos, ventanas, mezcla, brillo, mosaico,
paletas extendidas y modos de pantalla 0/1/2/3. Detalle, pruebas y divergencias con melonDS en `docs/fase2.md`.
Paquete «SalsaDS núcleo» (SLDS00008) con los SPE; New Super Mario Bros. llega al menú en el host. Pendiente: medirlo en la
PS3 real, la captura de pantalla, la capa 3D (Fase 3) y el sonido (2.2).

## 2026-10-17 — Cierre de la Fase 1 (1.3)

- **Cuadro ocioso**: el planificador cortaba el tiempo en 8753 tramos por cuadro aunque las dos CPUs estuvieran en HALT; eso eran los
  ≈ 2,1 ms fijos por cuadro medidos en la PS3 (2,73 ms de emulación con la demostración, 8,4 con 28–39 k instrucciones por cuadro:
  2,1 ms + 0,16 µs por instrucción). Ahora el tiempo salta al siguiente evento: en la Mac un cuadro ocioso baja de 177 a 24 µs
  (×7,3) y la demostración de 220 a 67 µs; la carga (ocupadas las dos CPUs) no cambia. Quedan ~525 eventos de video por cuadro.
  El paquete escribe `DATO desglose` y `DATO MIPS` en `nucleo.log` para repartirlo en la consola.
- **New Super Mario Bros. y Mario Kart DS**: no era la CPU ni el IPC. NSMB se apagaba porque no había chip de guardado (el ARM7 lee su estado
  por AUXSPI cada 5 ms y contestábamos FFh para siempre); MKDS repetía sin fin una escritura de la configuración de Wi-Fi en el firmware
  porque el firmware ignoraba las escrituras. Con las dos cosas, los dos configuran el video **igual que melonDS** (DISPCNT de los dos motores y
  VRAMCNT idénticos, y el guardado de NSMB, 8 KB, byte a byte igual al de melonDS). Super Mario 64 DS llega al mismo punto que melonDS.
- **Guardado del cartucho (AUXSPI)**: EEPROM de 0,5 KB, 8 KB, 64 KB y 128 KB, FRAM de 32 KB y Flash de 256 KB a 1 MB; el tipo sale del
  tamaño del archivo de guardado, de una tabla de tres códigos de juego o, si no, EEPROM de 64 KB. ROM propia `save` (siete tamaños,
  little y big endian y sanitizadores; contra melonDS con divergencias anotadas). El firmware de la consola también se puede escribir.
- **ROM de carga** (`carga`) y **ROM ociosa** (`ocio`): dos nuevas, propias, en `tests/roms/homebrew/`. `carga` mantiene las dos CPUs
  ocupadas con trabajo variado y comprueba sus resultados (145 MIPS en la Mac).
- Paquete **«SalsaDS núcleo» (SLDS00008)** del commit 9f2a177: `build/salsads-nucleo/salsads-nucleo.pkg`, SHA-256
  `8da0871ecb4e16e97569e3250055f0f4aef3d7ca8722e568dc627662408f92a4`. Lleva la ROM de carga embebida (o `rom.nds`), guardado por
  juego en `/dev_hdd0/salsads/saves/` y el desglose por cuadro. En RPCS3: OK. **Falta medirlo en la PS3.**
- Herramientas de diagnóstico nuevas en el host: trazas por `--dbg`, `--break7/9` con anillo y pila, `--watch7/9`, `--dump`,
  `tools/melonds-dump.py` (memoria y registros de melonDS por GDB). `tools/melonds-run.py` corre sobre una copia temporal.
- Higiene: cinco volcados de memoria de qemu (`*.core`, ≈ 40 MB) se habían colado en un commit anterior; se quitaron del árbol y se ignoran,
  pero siguen en la historia (si el repositorio se publica, conviene reescribirla antes).
- Pendiente (detalle en `docs/fase1.md`): medir en la PS3, SWI 1Ah–1Ch (sonido), FIFO de geometría (Fase 3), detección del chip de guardado
  sin archivo ni tabla, eventos de video perezosos si el desglose lo pide.

## 2026-10-17 — Máquina de la DS y primera imagen (Fase 1.2)

- `src/core` + `src/bios`: mapa de memoria de las dos CPUs (RAM, WRAM con WRAMCNT, VRAM por bancos con VRAMCNT,
  paleta, OAM), planificador de eventos con el ARM7 a la mitad del reloj del ARM9, IRQ por CPU, DISPSTAT/VCOUNT y
  VBlank/HBlank, temporizadores, DMA, IPC (SYNC y FIFO), teclas, cartucho (ROM desencriptada), SPI con firmware
  generado, táctil, energía y RTC, división y raíz por hardware, BIOS en HLE (bloques de BIOS propios + SWI) y
  arranque directo. Sustituye a `ndsmin`. Detalle, frontera para el ARM7 en otro hilo, tablas y divergencias en
  `docs/fase1.md`.
- **Primera imagen**: modo de visualización directa de VRAM del motor A y color de fondo del motor B. `hola` y `mem`
  dan en melonDS la misma imagen píxel a píxel y los mismos registros. `salsads-cpurun --screens` la vuelca.
- ROMs propias por paso (`mem`, `irq`, `dma`, `ipc`, `bios`, `periph`, `demo`): **todas OK** en SalsaDS (little y
  big-endian, y con tramos del planificador de 1 a 20 000 ciclos) y en melonDS 1.1; sus divergencias quedaron anotadas.
  Hallazgos de melonDS: su BIOS libre destruye r0 y r2 en el Halt del ARM7, da el resto de Div en valor absoluto, Sqrt
  0 con entradas grandes y no tiene Huffman; el DMA de HBlank y el de «inicio de pantalla» difieren de GBATEK.
- Paquete de PS3 **«SalsaDS núcleo» (SLDS00008)**: corre la máquina con la demostración propia embebida (o
  `/dev_hdd0/salsads/rom.nds`) y las dos pantallas de la 0.6. En RPCS3: OK. **Falta la PS3 real.**
- Respaldos del autor (a mano, sin capturas en el repo): Super Mario 64 DS arranca más lejos (configura video y
  VRAM y sigue con su lógica; usa la FIFO de geometría); New Super Mario Bros. configura video y se queda con los
  dos procesadores esperándose; Mario Kart DS no llega a configurar el video (el ARM7 lee el firmware sin parar).
- Decisión pendiente del autor: ninguna nueva. Pendiente técnico (guardados, SWI 1Ah–1Ch, trazas de los juegos, GX
  FIFO): lista en `docs/fase1.md`, «Qué falta para cerrar la Fase 1».

## 2026-10-17 — Intérpretes de CPU (Fase 1.1)

- `src/cpu/`: un núcleo (`struct acore`) para el **ARM9 (ARMv5TE)** y el **ARM7 (ARMv4T)**,
  ARM y Thumb, modos con banco, excepciones, HALT, líneas de IRQ/FIQ, memoria por
  callbacks con direcciones alineadas, un gancho de SWI para la HLE de la BIOS y, en la
  ARM9, CP15 mínimo con ITCM/DTCM (base, tamaño, espejo, modo de carga). Detalle,
  reglas por versión y plan de la 1.2 en `docs/fase1.md`.
- Pruebas: ≈ 8,0 M de comprobaciones unitarias en LE y BE (con mutaciones que las ponen a
  prueba) y las ROMs `hola`, `cpu9` y la nueva `cpuarm` (ARM9 y ARM7, ARM y Thumb,
  excepciones y TCM) corridas en los intérpretes (LE y BE) y en **melonDS 1.1: OK**.
  `tools/build.sh test-host` lo corre todo; `tools/cpurun.sh` corre una ROM en la Mac
  sin Docker; `tools/melonds-run.py --arm7` muestra también el bloque del ARM7, el
  registro de fallos y las divergencias.
- melonDS **diverge de la documentación** en varios casos de borde (LDRH/LDRSH impares del
  ARM7, listas vacías de LDM/STM, STM con la base en la lista, bit Q del ARM7, BKPT, modo de
  carga de las TCM): la ROM los anota aparte (no son fallos) y los intérpretes siguen
  GBATEK; ninguno está contrastado en una DS real (lista en `docs/fase1.md`).
- Rendimiento del intérprete en el **host**: 125–198 MIPS (Mac y contenedor); ≈ 1,5×
  más lento que el mínimo de la 0.3. En la PS3 sin medir (≈ 12 MIPS por escala [est]).
- Corredor `salsads-cpurun` + `src/host/ndsmin.c`: arranque directo con una máquina
  mínima (no es el mapa de la 1.2) que llega al `main` de las ROMs de BlocksDS con su
  `crt0` y libnds reales.
- Marco de pruebas de las ROMs: registro de los primeros 16 fallos, registro de
  divergencias, marcador de avance y variante para el ARM7 (`sds7.c`). `hola` y `cpu9`
  se reconstruyeron (hashes nuevos en `SHA256SUMS`; la construcción es reproducible).
- Pendiente para la 1.2: mapa de memoria con VRAM por bancos, planificador, IRQ,
  temporizadores, DMA, IPC FIFO, cartucho, SPI/firmware, BIOS en HLE, arranque directo y
  la primera imagen. Y probar el intérprete en el PPE real.

## 2026-10-17 — Prueba mínima del JIT ARM → PPC (Fase 0.7) y cierre de la Fase 0

- `tests/phase0/jit-arm` («SalsaDS JIT», SLDS00007) + `src/jit/`: traductor de un
  subconjunto de ARM (datos con S, ADC, LDR/STR/LDRB/STRB, B<cond>) a PowerPC64 con
  tres formas de banderas (perezosas, CR por instrucción, cmp+salto fusionados), un
  intérprete de referencia y cuatro bucles (el de la 0.3, suma64 con acarreo,
  ramas, bytes). **qemu-ppc64 ejecuta el código traducido** y da lo mismo que la
  referencia (registros, memoria, instrucciones).
- **Medido en la PS3 real (2026-10-17): el ARM9 cabe.** El peor bucle da 534,8 MIPS
  con CR por instrucción (8× los 67 MHz; intérprete de 0.3: 18,1 MIPS). ARM7 en el
  hilo 1: girar −31,9 %, dormir en un semáforo de lv2 −0,7 % a −9 %. **Fase 0
  cerrada**: arquitectura confirmada en `fase0.md` («Cierre de la Fase 0»).
- Caché de código: cola NOBITS de 4 MB en el texto (`mk/rec-cache.ld`), hilo de
  prueba con plazo de 2 s y marca en disco; si falla o cuelga (CFW), respaldo a los
  intérpretes. En RPCS3 el JIT no se intenta (solo lectura).
- Mide también el ARM7 en el hilo 1 del PPE: pérdida de MIPS del ARM9 y latencia de
  despertar con giro, giro de prioridad baja, `sysUsleep`, semáforo y cola de
  eventos de lv2.

## 2026-10-16 — Rasterizador 3D en SPEs (Fase 0.4)

- `tests/phase0/spe-raster3d` («SalsaDS raster», SLDS00002): escena sintética
  de 500, 1000 y 2048 triángulos con textura de la VRAM (por DMA) y color de
  vértice, z-buffer de 24 bits, dibujada por 1 o 2 SPEs a 1×, 2× y 4×, por
  bandas de líneas o tiles. Diseño, verificación y qué mirar en
  `docs/fase0.md` §0.4.
- Verificado: el mismo núcleo en el host (LE y BE) contra una referencia en C
  (modo id idéntico; color 99,96 % idéntico; todos los repartos con los mismos
  bits) y en RPCS3 (`RESULTADO … OK`).
- **Medido en la PS3 real (paquete de `255b0d2`): criterio cumplido.** 1× N =
  2048 en 2 SPEs: 4,19 ms (< 8); 2×: 7,96 ms; 4×: 19,61 ms (no cabe en 2
  SPEs). Medida ×1,6–2 sobre la estimación. Caché de texturas desbordada: ×5,4
  más lenta, primera decisión de la Fase 3. Detalle en `docs/fase0.md` §0.4,
  `perf-log.md` y `docs/datos/`. **Fase 0.4 cerrada.**
- Encontrado: `spu_re` de spu-gcc ya incluye el `fi`; uno más arruina el
  recíproco. El programa ahora autoprueba en cada SPE las funciones con
  intrínsecos contra el PPE.
- Queda como riesgo para la Fase 3: texturas que no caben decodificadas en el
  local store (con 24 texturas, la caché se vacía en cada unidad).
## 2026-10-16 — Composición 2D en un SPE y salida de dos pantallas (Fases 0.5 y 0.6)

- **0.5** (`tests/phase0/spe-gpu2d`, «SalsaDS 2D», SLDS00003): compositor 2D
  por líneas en un SPE (4 fondos de texto, 128 OBJ, VRAM por DMA, registro de
  cambios por línea) y una referencia píxel por píxel. Imágenes idénticas en el
  host (LE y BE) y en RPCS3 (SPE de verdad); el **tiempo (< 4 ms) falta medirlo
  en la PS3 real**. Detalle, límites del modelo y la estimación en `fase0.md` 0.5.
- **0.6** (`tests/phase0/dual-screen`, «SalsaDS pantallas», SLDS00004): dos
  texturas por cuadro con rsxkit en cuatro distribuciones a 720p y 1080p con
  escala entera y ajustada (distribución comprobada por píxeles en RPCS3), DS3 →
  botones de la DS y el puntero táctil (botón de toque mapeable, stick derecho o
  inclinación del Sixaxis). **Pendiente en la PS3 real**: los 60 Hz, 1080p, y
  cómo se sienten los dos modos del puntero (precisión, temblor, latencia); los
  ejes del Sixaxis son una suposición que se corrige en `controles.cfg`.
- Código nuevo reutilizable: `src/gpu2d/` (referencia, compositor, escenas),
  `src/frontend/{dspad,layout}.c` (entrada y geometría, sin PSL1GHT),
  `tools/raw-png.py` (volcados a PNG), `tools/rpcs3/salsads-1080.yml` (RPCS3 a
  1080p). El logger comprueba ahora que `/dev_hdd0/salsads` existe y lo dice.
- **APPID duplicado**: `tests/phase0/udplog` ya usaba SLDS00003. Los paquetes de
  0.5 y 0.6 siguen la asignación pedida (SLDS00003 y SLDS00004); si udplog y
  spe-gpu2d se instalan en la misma consola, el segundo reemplaza al primero.
  La Fase 0.4 (spe-raster3d) usa SLDS00002, que también tiene `empty`; el siguiente libre es SLDS00005 (pantallas giro usó SLDS00006).

## 2026-10-16 — Microbenchmarks del PPE (Fase 0.3)

- `tests/phase0/ppe-bench` (SLDS00001 «SalsaDS bench»): memoria LE con
  `lwbrx`/`stwbrx`/`lhbrx`/`sthbrx`, tres estrategias de banderas NZCV más una
  cota, y tres despachos de un intérprete ARM mínimo. Verificado contra
  referencias en C bajo qemu-ppc64 (`tools/build.sh test-host`) y arranca en
  RPCS3.
- **Medido en la PS3 real** (paquete de `08b7ef0`, subido por quien
  tiene el permiso del FTP): resultados, decisiones y log completo en
  `docs/fase0.md` §0.3, `perf-log.md` y `docs/datos/`. Decididos: memoria LE
  con `lwbrx` y compañía, banderas perezosas, y recompilador obligatorio
  (intérprete 16,5–17,9 MIPS contra ~100 que pide la DS). **Fase 0.3 cerrada.**
- Descubierto al escribirlo: en PowerPC64, el CA de `addc`/`subfc` es el
  acarreo del bit 63, no del 32; el C de ARM sale del bit 32 del resultado de
  64 bits con los operandos extendidos con ceros.

## 2026-10-16 — ROMs de prueba (Fase 0.2)

- `tests/roms`: marco de pruebas `sds` (resultado en RAM `0x02380000` y en
  pantalla), ROMs `hola` y `cpu9` (65 187 comprobaciones del ARM9), construidas
  con BlocksDS en Docker (`tools/build-roms.sh`) y validadas en melonDS 1.1
  con su stub de GDB (`tools/melonds-run.py`).
- ARM7 de las ROMs: uno propio vacío, para no enlazar DSWiFi/Maxmod/Mbed TLS.
- Falta: pruebas del ARM7 (ARMv4T), DMA, timers, IPC, VRAM por bancos; y
  copiar el aviso íntegro de picolibc antes de redistribuir los `.nds`.

## 2026-10-16 — Arranque: esqueleto (Fase 0.1)

Hecho:
- `lib/rsxkit` y `lib/spukit` entran por `git subtree --squash` desde sus
  repositorios (MIT). Cada uno en su propio commit y sin tocar otra carpeta.
- Build en Docker: `tools/build.sh` usa la imagen `salsads-toolchain:dev`, que
  se construye en segundos (`tools/docker-image.sh`) a partir de la imagen del
  toolchain de Salsa64 (ps3toolchain, PSL1GHT y cgcomp: más de una hora de
  compilación que no se repite). `docker/Dockerfile` explica cómo.
- `mk/ps3.mk` (reglas de PSL1GHT, SPU embebido, `.pkg`) y
  `src/platform/ps3/` (logger TTY + archivo + UDP) adaptados de Salsa64.
- Logger UDP en el **puerto 18195** (Salsa64 usa 18194): `tools/udplog.py`.
  Un receptor por puerto. Archivos de la consola en `/dev_hdd0/salsads/`.
- Pruebas del host en los dos órdenes de bytes: `tools/build.sh test-host`
  corre `tests/unit` con el gcc nativo del contenedor (little-endian, con
  ASan/UBSan) y con `qemu-ppc64` (big-endian, CPU `power9`; ver
  `docs/cell-desde-salsa64.md` §6.2). Cada corrida imprime `host: LE|BE` y el
  script falla si no coincide.
- `tools/rpcs3-run.py` y `tools/rpcs3/*.yml`: RPCS3 sin interfaz. Usa el
  **mismo candado** (`~/Library/Caches/rpcs3/salsa64-rpcs3.lock`) que Salsa64,
  porque el RPCS3, su `dev_hdd0` y su `TTY.log` son uno solo.
- `tools/deploy-ps3.sh` (FTP de webMAN MOD o `ps3load`; la IP va en
  `tools/ps3.conf`, ignorado por git).

APPIDs de SalsaDS: rango `SLDS`, distinto del `SLSA` de Salsa64.

| APPID | Programa |
|---|---|
| SLDS00001 | `tests/phase0/ppe-bench` («SalsaDS bench») |
| SLDS00002 | `tests/phase0/spe-raster3d` («SalsaDS raster»); también `tests/phase0/empty`, que solo se corre en RPCS3 |
| SLDS00003 | `tests/phase0/udplog` (y también `tests/phase0/spe-gpu2d`, «SalsaDS 2D»: ver el aviso de arriba) |
| SLDS00004 | `tests/phase0/dual-screen` («SalsaDS pantallas») |
| SLDS00006 | `dual-screen` con otro nombre («SalsaDS pantallas giro», modo GIRO) |
| SLDS00007 | `tests/phase0/jit-arm` (0.7) |
| SLDS00008 | `tests/phase1/nucleo` («SalsaDS núcleo», Fase 1.2) |

Pendiente (preguntas abiertas del plan, de la decisión del autor):
licencia del proyecto, juegos objetivo, aplicación independiente o núcleo
libretro, nombre del ejecutable.

## Qué existe hoy de DS en la PS3

No encontré ningún emulador de DS usable en PS3 (búsqueda del 2026-10-16;
detalle y fuentes en `docs/fase0.md`, 0.2).

## Fase 4.5: dónde se van los 25,5 ms de Mario Kart DS (medido en la PS3, geometría en el SPE)

- `frame_begin` (7,8 ms) no es trabajo: es `rsxkit_frame_begin`, la espera del flip anterior. Con la emulación en 25,5 ms el
  intervalo es de 33,3 ms (30 Hz) y esa espera es el relleno hasta el siguiente VSync. Desaparece sola al bajar de 16,7 ms.
- Los «eventos» (13,5 ms) son casi todos el evento de línea (9,9 ms), y de esos ~9,2 ms son la espera del render 3D en la
  línea 192 (`wait3d` en `nds_gx_vblank`). El trabajo real de la CPU por cuadro es ~16,3 ms (ARM9 6,8 + ARM7 4,5 + gx 3,1 + resto):
  ya está en el borde de los 16,7 ms, así que para 60 fps hace falta además esconder la espera del 3D.
- Cambios de esta rama: la geometría va al SPE por omisión (`/dev_hdd0/salsads/geometria-ppe` la devuelve al PPE); en
  `nds_gx_vblank` el motor avanza (`gx_advance`, que solo escribe el cuadro en construcción) antes de esperar al render 3D;
  nueva línea `DATO raster3d-latencia` (envío en la línea 0 → llegada del PPE al VBlank → fin de cada SPE).
- Pendiente (diseño): el render dura 10,8 / 13,5 ms por SPE pero tarda ~21 ms desde el envío; la línea nueva dirá si son
  esperas de arranque o contención. Para esconder la espera: tercer `gx_frame` (el cuadro en vuelo sigue vivo tras el
  SWAP_BUFFERS), esperar el render en la línea 262 y enviar el 2D (`vid_submit`) allí, con el reparto `prev/sub` del bucle
  principal desplazado un cuadro; y repartir mejor las bandas (SPE1 trabaja 25 % más que SPE0). ARM7 en el hilo 1: sin
  hacer (ARM7 4,5 ms; exige sincronía por tramos con el ARM9 y la ruta lenta del JIT compartida).
- Rama fase47 (hecho, sin medir en la PS3): tercer `gx_frame` (`gx.c`: `bld = (bld + 1) % 3`); `nds_gx_vblank` ya no espera el 3D; la espera
  (`wait3d`) y la entrega del 2D (`submit`) pasan a la línea 262 (`nds_video_submit_pending`); una captura DISPCAPCNT con 3D visible sigue
  esperando en la 192. La imagen es la misma (la latencia del 3D no cambió); el bucle principal toma el 2D entregado en esta pasada
  (`prev = sub` al principio) y dibuja el mismo cuadro de antes. Bandas del 3D repartidas por costo estimado (`r3d_balance`, `r3d_job.bandsel`);
  archivo `reparto-alterno` para comparar con el reparto alterno. Esperado: la espera de ~9,2 ms baja a ~4–5 ms (el render pasa de 12 a 16,3 ms de
  margen) y el SPE1 deja de ir un 25 % por encima del SPE0; medir con `DATO raster3d` / `raster3d-latencia` y con `reparto-alterno` para el A/B.
  Paquete con el JIT (SLDS00009) del commit `1e712e3`: `build/salsads-nucleo-jit-fase47.pkg`, SHA-256
  `fa1e9cab105e444b1d3eaf9c5443adbbd920dd6481024cca490e3dc0dccbbbe6`. Sin probar en la PS3.

## Rama fase50: rendimiento del 2D y tallos inclinados a la derecha (2026-10-18)

- **Regresión 60 -> 30 fps de la fase47 (NSMB)**: la fase47 recogía el 2D entregado en la línea 262 en la misma pasada (`prev = sub` antes de
  `vid_collect`), así que el PPE esperaba la composición entera (5 ms). Se restauró el orden anterior: se recoge el 2D de la pasada
  anterior (los SPE lo componen mientras se emula el cuadro siguiente) y `prev = sub` va al final. Se conservan la espera del 3D en la línea
  262 y el tercer `gx_frame`. Las dos instantáneas `set` bastan: el 2D en vuelo (pasada N) y el de la pasada N+1 usan conjuntos distintos y
  el de N-1 ya se recogió. **Medir en la PS3**: `DATO spe` «el PPE esperando» ~0 ms e intervalo del RSX 16,7 ms.
- **Tallos inclinados a la derecha (NSMB)**: medidos contra melonDS ejecutándolo con escenas nuevas 41 a 45 de `gx3d` (tallo espejado texturado, sin
  textura, desplazado, y dos sondas con textura directa cuyo color dice el texel por píxel). Causa: izquierda x-mayor que va hacia la derecha
  (ortográfica): la interpolación a lo largo de la línea arranca en la x de esta línea de la arista (redondeada), a la izquierda del primer
  píxel dibujado, y sus atributos van en la y de la línea (no la siguiente). Nuevo campo `r3d_row.xo` (origen de la interpolación; `k = x - xo`)
  en `r3d_px.h` y `r3d_vrow.h`. Diferencias con melonDS: 41 y 43: 175 -> 0; 44 y 45: 0; 21: 2144 -> 56; 36: 962 -> 25; 14: 399 -> 123; 0: 127 -> 82;
  ninguna escena empeora. Queda 1 píxel en la 42 (cobertura de la costura, ya estaba). `gx3d.golden` actualizado (0, 14, 21, 36 y 41 a 45).
- **Audio**: revertida la unión f966cc3 (control de tasa amplio y limitador); queda el control de tasa anterior a la fase27 y el FIR de la fase26.
- Rama fase52 (hecho, sin probar en la PS3): **caché del recompilador pedida a PS3MAPI**. En la PS3 real con HEN 4.91 (prueba
  «Salsa64 JIT test» v2 del 2026-10-18) `PROC_PAGE_ALLOCATE` de la syscall 8 de Cobra/Mamba/webMAN da páginas ejecutables de 1 MB
  (probadas hasta 32 MB, en 0x70000000) que se escriben directo y se ejecutan; limpiar 64 KB cuesta lo mismo que en el texto. Al
  arrancar, `salsads_rec_cache_mapi` (tests/phase0/jit-arm/rec_cache.c) pide 16 MB y los prueba como la caché del texto (hilo con
  plazo de 2 s, escribir/ejecutar/reescribir al principio y al final; marca propia `nucleo-jit-ps3mapi.marker`); si no hay PS3MAPI o
  falla, sigue la cola NOBITS de 4 MB del texto. La zona está lejos del texto: el código generado llama a C por `mtctr`/`bctrl`
  (`far_calls`, sin intentar `bl`). Las tablas de bloques y enlaces se duplican desde 8 MB de caché (32768 bloques, ~8 MB del heap).
  Las páginas se devuelven al salir. Archivo `jit-sin-ps3mapi` para volver a la caché del texto (A/B). Qué medir: `DATO jit`
  (vaciados de la caché y bloques traducidos por cuadro) y el tiempo del ARM9, con y sin `jit-sin-ps3mapi`.
- fase52 en la PS3 (2026-10-18, HEN 3.3.0, PS3MAPI 0x124): las páginas de `PROC_PAGE_ALLOCATE` **siguen contando como libres**
  para el proceso: la siguiente reserva normal (el heap que crecía con las tablas del recompilador) recibió la misma memoria
  física (una marca escrita en 0x70080000 apareció en el heap en 0x55180000) y llenar la tabla `wr_phys` con 0xFF borraba las
  rutinas comunes: el primer bloque saltaba a 0xFFFFFFFF (`fnmadd.`, válida) y giraba sin volver (juego en negro). Arreglo:
  justo después se pide la misma cantidad con `sysMemoryAllocate` y no se toca (absorbe esas páginas), comprobado página por
  página con una marca; si no quedan todas apartadas, no se usa PS3MAPI. Con eso NSMB corre a 60 fps con 8 MB de caché, pero
  **no gana nada**: siguen 6 vaciados (no eran por falta de espacio; se usan ~3 MB) y el ARM9 pasa de 5,73 a 6,09 ms por las
  llamadas a C por CTR. Queda **experimental**, apagada: archivo `jit-ps3mapi` para probarla. Si un juego llena los 4 MB, lo
  simple es agrandar la cola NOBITS del texto (Salsa64 usa 32 MB así). Diagnóstico que lo encontró: memoria del proceso leída en
  vivo con webMAN (getmem de PS3MAPI), vigía del JIT y fotos de la primera palabra.

## Fase 49: selector de ROM y abrir/cerrar juegos sin fugas de memoria (rama fase49)

- **Selector** (`tests/phase1/nucleo/main.c`, el mismo programa de «SalsaDS núcleo» y «SalsaDS JIT núcleo»): al abrir la aplicación
  lista los `*.nds` de `/dev_hdd0/salsads/` por nombre (con `opendir` de newlib, nada de libsysfs). Cruceta o stick izquierdo
  para moverse (repite al mantener), L1/R1 o izquierda/derecha saltan una página, **X abre**, **O sale de SalsaDS**. Texto sobre
  fondo liso con «SalsaDS» y la versión; la combinación de salida está escrita en la propia pantalla. La última ROM elegida se
  guarda en `/dev_hdd0/salsads/ultima-rom.txt` y el cursor empieza en ella. Si no hay ningún `.nds`, ofrece la ROM de carga
  integrada. Los botones que ya estaban apretados al entrar al selector (la B del juego, por ejemplo) no cuentan.
- **Salir del juego al selector: SELECT + START del DS3 mantenidos 1 s.** Aparece un medidor pasados los primeros 250 ms. No choca
  con los controles de la DS (la DS reinicia con L+R+START+SELECT; L2 cambia la distribución, R2 es el toque, L3 recentra,
  L3+R3 muestra la línea de estado). Al soltar antes de 1 s no pasa nada (el juego sí recibió Select y Start).
- **Memoria** (detalle en `docs/memory-budget.md`): se reserva una vez al arrancar y se reutiliza entre juegos la estructura
  `nds` (≈ 10,2 MB, `nds_init` la deja en blanco), las texturas del RSX, la interfaz del RSX, los seis SPE (quedan en espera de
  trabajos; sus autopruebas corren una sola vez), la salida de audio y el recompilador del ARM9 (se prueba y se crea una vez; `nds_init`
  lo recrea con los mismos tamaños, y al cerrar se vacía su caché). Lo que depende de la ROM sale de **páginas de 1 MB del
  sistema** (`sysMemoryAllocate`), fuera del heap: la ROM entera y la caché de páginas del modo por fragmentos (nuevo
  `nds_rom_set_allocator` en `src/core/cart.c`); se devuelven en `game_close`. El cierre espera lo que haya en vuelo (sonido,
  geometría, render 3D, composición 2D, fences del RSX), escribe el guardado y suelta la ROM.
- **Informe en el log al cerrar cada juego**: reservado por subsistema; cambio frente al cierre anterior (heap en uso, libres,
  bloques, arena, tope del heap, memoria de usuario libre del sistema por la syscall 352); y desde el tercer cierre el acumulado
  frente al segundo con veredicto «FUGA»/«sin fuga» (tolerancia 4096 bytes). Líneas «memoria al arrancar / tras cerrar el juego /
  al salir».
- **Pruebas**: `src/host/ciclos.c` (`salsads-ciclos`) abre y cierra 12 veces la ROM de carga, entera y por fragmentos, con
  `nds_init` sobre la misma estructura, y exige que el heap en uso y la arena no crezcan; corre en LE, en BE (qemu) y en BE con el
  recompilador (`--jit`), dentro de `tools/build.sh test-host`. En RPCS3 (config del intérprete del PPU, `salsads-interp.yml`; con la
  config normal el SPE r3d-1 muere por «Illegal instruction» también en la versión anterior): `tools/rpcs3-run.py nucleo-jit --rom
  tests/roms/homebrew/carga.nds --frames 40 --ciclos 5 --config tools/rpcs3/salsads-interp.yml` → 5 aperturas, RESULTADO OK, heap
  y tope sin cambio entre cierres. RPCS3 no implementa la syscall 352: allí «usuario libre» sale en 0.
- Sin probar en la PS3 real: el selector con el mando, el medidor de salida, que `sysMemoryAllocate` de 32 MB/64 MB devuelva todo
  (la memoria de usuario libre debe quedar igual tras cada cierre) y el recompilador recreado en `nds_init` entre juegos.
- Paquete con el JIT (SLDS00009) del commit `8057fbc`: `build/salsads-nucleo-jit-fase49.pkg`, SHA-256
  `0fea0e710ff1ef6420f2e0d0b5e855eb89f31cd641d95e61e5b628f10a678e3a`. Sin probar en la PS3.

## Candidata v0.0.4 (2026-10-22)
- Une las ramas de Codex ya integradas en `texturas-metroid`, sin su andamiaje de medición, y descarta `r3d-aristas` (el enlace del SPE queda sin holgura de pila: 7312 B < 8192 B).
- **Captura de pantalla (DISPCAPCNT) en el PPE sin pasar por la ruta general:** compone solo el motor A y solo las capas que la captura usa (`gpu2d_compose_engine_ppe`, capa 3D única fusionada). Medido en la PS3: Metroid, composición ~20,7 → ~1,7 ms y captura total ~21,3 → ~2,3 ms. La captura por SPE queda en el código pero sin ser usada por el frontend.
- **Texturas 3D por celdas:** paletas indexadas reutilizadas y celdas de 16 colores decodificadas por parejas. Host: −25 % del raster en Metroid; PS3 (sin A/B estricto): texturas sumadas ~17 → ~8,6 ms.
- **Guardado de Metroid:** `AMH` = Flash de 256 KB; 0Ah/0Bh ya no deciden por sí solos un chip; PAGE WRITE (0Ah) no borra el resto de la página (a contrastar con la hoja del M45PE20); un `.sav` antiguo de 512 B no se toca y la recuperación va a `<juego>.flash256k.sav`.
- Pendiente en la PS3: A/B de MKDS (`texturas-bloques` + `r3d-sin-aplazar`), Metroid (fps y guardado), y holgura de pila de los SPE con `tools/spu-stack.py`.
