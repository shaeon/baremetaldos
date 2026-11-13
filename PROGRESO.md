# Progreso del trabajo de Shaeon en BM-DOS

Fichero de trabajo para retomar el trabajo si una sesión se corta. Se
actualiza al empezar y al terminar cada paso, y se lee al empezar cada sesión.
(Estuvo en docs/PROGRESO.md; docs/ está en el .gitignore del proyecto.)

**Regla al empezar una sesión:** `git status` y `git log -3`.  Si hay cambios sin
guardar o commits que no aparecen aquí, son de un intento interrumpido: se
revisan como código ajeno antes de usarlos, y se anotan aquí.

Repositorio: fork `github.com/shaeon/baremetaldos` (de `mmoroca/baremetaldos`),
rama `main`.  Entrega: bundle `git bundle create ... <base>..shaeon-main`, que el
usuario aplica con `git fetch <bundle> shaeon-main && git merge --ff-only FETCH_HEAD`.

## Hecho (en `main` del fork)

1. MIDI en el núcleo 1 (Munt + FluidSynth), ganancia, diagnóstico en el panel.
2. Núcleos: gestor único (`bare_cores.cpp`), WFE para los libres.
3. Vídeo en el núcleo 2: triple búfer, doble página, vsync, proxy del mailbox,
   log de Circle fuera de la pantalla, escalador rápido, escalado nítido.
4. Audio: filtro de continua, limitador suave, contador de huecos.
5. CI en GitHub Actions: Pi 3, 4 y 5 + tarjeta completa (`BM-DOS-card.zip`).

## Hecho después del menú, paso 1

En GitHub hasta `9cbffd3` (elegir red WiFi).  Los dos últimos de la tabla
están en local y en el bundle, pendientes de push.

| Commit | Paso |
|---|---|
| 5a138f6 | Menú: ajustes que se guardan, pestaña Vídeo, diálogos |
| 3ea6b07 | Menú: Máquina |
| 80f4a2e | Menú: Sonido (salida, volúmenes, tarjetas, MIDI, pruebas) |
| 3836354 | Menú: Unidades (C:/E: carpetas, A:/D: imágenes en caliente, explorador) |
| 5a7270e | Menú: Entrada (KEYB, ratón, joystick) |
| a9d7eb8 | Menú: Rendimiento; el governor respeta `mode` |
| 289cfc8 | Menú: Arranque; SUPR/F2 al encender |
| a121ca7 | Red 1: Ethernet/WiFi, DHCP, NTP, zona horaria |
| 2d78fd9 | Red 2: módem y null-modem (capa SDL_net sobre sockets de Circle) |
| 357770d | Red 3: IPX sobre UDP |
| 9cbffd3 | Menú: elegir red WiFi (--wrap de ReceiveScanResult, campo de texto, teclado ES/US) |
| 533baea | **Encontrado sin conocer su origen** (intento interrumpido): WiFi encendido por defecto; espera NTP solo si hay red configurada. Revisado: correcto. **Confirmado por el usuario: WiFi siempre encendido.** |
| (el de "net: the card's web server") | Red 4: servidor web de la tarjeta (encontrado a medias de un intento interrumpido, revisado, corregido el escapado en la página, terminado) |
| (el de "fix: firmware calls from two cores, stdout off the screen...") | Fallos vistos en placa: carrera en el búfer del firmware de Circle (parche), stdout fuera de la pantalla, MIDI que perdía eventos sin consumo de audio |
| (el de "boot: BIOS-style logo, no rainbow splash...") | Logo de arranque original, sin arcoíris (disable_splash), boot_delay=0, NTP 5 s máx., tiempo de arranque en pantalla |
| (el de "web: password from the page, rename, move, free space") | Servidor web al estilo ZXMetalCircle: contraseña desde la web, renombrar, mover, espacio libre |
| (el de "fix: the boot stopped after the \"fb:\" lines") | Cuelgue al arrancar (foto del usuario): segunda llamada a CGlueStdioInit (assert) del commit 6ca96f0; quitada.  PROGRESO.md pasa a la raíz |
| (el de "dos: Ctrl+Alt+Del as on a PC - the BIOS asks...") | Ctrl+Alt+Supr como en un PC: la BIOS emulada pide el reinicio (parche), reinicio de DOS sin reiniciar la placa, CPU y caché del dynarec limpias, MIDI aparcado; R del menú = reiniciar DOS.  **Probar en placa:** 10-20 reinicios seguidos (memoria libre estable), sonido/ratón/CD después, reiniciar tras DOOM (modo protegido), Windows 3.x se queda la combinación. |
| (el de "fix: clock and temperature read only with the panel on") | Temperatura y reloj a 0: BareSysInfoRefresh solo corría con el panel PetSis visible; ahora siempre (y en el menú).  El veto por temperatura del governor no actuaba con el panel oculto.  Corrige la explicación anterior (no era la carrera del búfer del firmware) |

## En curso

**Punto 2 (governor por fotograma emulado + ver qué hace) - HECHO, pendiente de
probar en placa:** en la escena del 12-nov, "over" debe dejar de crecer y el
reloj bajar si la carga media queda por debajo del 75 %.
Pasos: [x] trabajo por fotograma (kernel: dividir por las fronteras de la
llamada)  [x] contadores en bare_sysinfo (SetLevel con motivo; subidas,
bajadas, tiempo por nivel, carga media)  [x] mostrarlos en Rendimiento
[x] pruebas PC  [x] compilar  [x] commit + bundle ("governor: work per emulated frame...").

**Punto 3 (governor sube antes: huecos de audio + calentamiento al arrancar un
programa) - HECHO, pendiente de probar en placa.**  Pasos: [x] ver detección del prompt
(no la hay: "CLI" es "sin imagen 1 s") y contador de huecos  [x] diseño:
PSP de DOS (bare_dosfiles.cpp) para el arranque de programas; 10 s arriba;
hueco -> +1 nivel (cada 0,5 s), 15 s sin bajar; 2 s de gracia tras menú /
pausa / reinicio; temperatura manda  [x] política + 11 pruebas (check-governor)
[x] kernel  [x] compilar  [x] commit + bundle ("governor: rise early...").
Limitación conocida: si el autoexec lanza un juego al arrancar, el primer PSP
visto puede ser el del juego (no hay calentamiento ese primer arranque).

**Punto 5 (perfilador) - HECHO, pendiente de leerlo en placa.**  (Encontrado a medias de
una sesión cortada; su aprobación no está en la conversación visible, pero el
usuario pidió "sigue por donde te has quedado".)  Objetivo del usuario:
decidir si se mueve el sonido al núcleo libre (3).  Pasos: [x] dónde mezcla
DOSBox (MixerChannel::Mix)  [x] diseño (ventanas de 1 s, CNTPCT_EL0)
[x] patches/dosbox-core/mixer-profile.patch  [x] port: bare_profile.cpp +
profile_core.h (check-profile); vídeo, audio, entrada (dentro de la llamada),
llamada y espera  [x] Rendimiento: "Dónde se va el tiempo"; páginas largas:
el cursor recorre las líneas informativas (menu_model.h, prueba nueva)
[x] pruebas PC  [x] compilar  [x] commit + bundle ("profile: where core 0's time goes...").

**Captura del perfilador en placa (12-nov):** "Teclado y ratón 49 %, CPU x86 0 %,
Esperando 51 %".  Error de medida: BareInputPoll termina con Yield(), que
cede la CPU a la tarea del emulador; medirlo desde fuera cargaba toda la
emulación al teclado.  Arreglado (se mide antes del Yield).  Lectura real:
emulación ~49 %, esperando 51 %: holgura de sobra (Carga 51 %, "comf").
El usuario dice que el teclado "está como lento": PREGUNTA pendiente de qué
exactamente (letras que tardan / repetición al mantener / menú).  Con el
arreglo de teclas repetidas, mantener una tecla ya repite como un PC (DOSBox:
500 ms de espera, luego ~30/s) y no a ráfagas como antes.

**WiFi muy lento en coger IP (usuario, 12-nov).**  Causa: la red, el WLAN,
wpa_supplicant y el DHCP son tareas del planificador y solo corren cuando
alguien cede la CPU; el metrónomo esperaba girando (SimpleusDelay) - la
mitad de cada segundo en un juego holgado - y el menú y la pausa también
(SimpleMsDelay).  Arreglo: esperas que ceden la CPU (Yield / MsSleep).
Además: la versión mostrada ("2026-11-10.78") es el sello manual del
proyecto, igual en todos los kernels; ahora se añade el commit
(build/bmdos_version.h, generado por el Makefile).  Hecho y entregado
("net: wait by yielding...").  Falta: probar en placa cuánto tarda la IP.

**Tres fallos del usuario (12-nov, tras "net: wait by yielding"):**
1. Hora: DOS siempre en 1970 (el WiFi tarda más de los 5 s de espera).  Hecho:
   patches/dosbox-core/bios-time-sync.patch (BIOS_SyncHostTime) y el bucle
   pone el reloj de DOS en cuanto hay hora de red; el arranque ya no espera.
2. IP que no llega con el menú (BIOS) abierto: con el kernel anterior el menú
   esperaba girando; c8751c6 ya lo cambió a MsSleep.  PROBAR con el kernel nuevo.
3. Reinicio de DOS: "Reiniciando DOS..." y pantalla negra muerta.  Causa: el
   DOSBox nuevo falla al arrancar (E_Exit) -> pide SHUTDOWN -> el port APAGABA
   la placa.  Hecho: tras un reinicio, ese SHUTDOWN se trata como fallo: se
   muestra el error de DOSBox, se guarda en SD:/bmdos-error.txt y se reinicia
   la placa.  CAUSA RAÍZ PROBABLE ENCONTRADA: la tabla de dispositivos de DOS
   (10 huecos) no se vacía al desmontar: cada arranque añade CON/NUL/LPT1 y
   nunca se quitan -> "DOS:Too many devices added" al 2º-3er reinicio.
   Arreglo: BareDosForgetDevices (bare_dosfiles.cpp) en el reinicio.  Si aún
   falla, el mensaje en pantalla / SD:/bmdos-error.txt dirá qué.
Compilado; commit "dos: the restart survives..."; falta probar en placa.

**Web (usuario, 12-nov): acciones arriba, y quitar el cambio de contraseña
desde la web (quizá más adelante en una página de configuración web).**  Hecho
("web: the actions on top..."): barra arriba (Subir ficheros, Subir carpeta,
Nueva carpeta, zona de arrastre); fuera la sección de contraseña, su JS, la
ruta /password y SavePassword.  La contraseña se pone en el menú o boot.conf.
IDEA PARA MÁS ADELANTE: página de configuración web.

**Punto 20 (panel de estado desde el menú) + rehacer la pestaña Salir -
HECHO, pendiente de probar en placa.**  El usuario: Salir "queda un poco raro"; quiere
"guardar y seguir en la BIOS" y "guardar y reiniciar la placa" (para aplicar
los cambios).  Pasos: [x] leer Salir actual  [x] diseño  [x] panel (boot.conf
+ Vídeo)  [x] Salir  [x] pruebas PC  [x] compilar  [x] commit + bundle ("menu: Salir rebuilt...").
  Hecho: boot.conf panel (+2 casos), "Panel de estado" en Vídeo (al momento);
  Salir en tres grupos (Guardar / Sin guardar / Reiniciar y apagar), Guardar y
  seguir, Guardar y reiniciar la placa, Deshacer los cambios; reiniciar DOS,
  la placa o apagar preguntan si hay cambios; el título recuerda "reinicia la
  placa/DOS para aplicar" (s_nApplyPending); las acciones sin valor usan toda
  la fila para el nombre (CMenu::LabelWidth).

**Punto 24 (Makefile con GNU Make 4.3) - HECHO ("make: the toolchain by its full path...").**  Causa:
Make < 4.4 no pasa el PATH exportado a $(shell), y Circle (Rules.mk) busca
libgcc.a, libstdc++.a, crtbegin.o... con $(shell $(CPP) -print-file-name).
Arreglo: override PREFIX/PREFIX64 con la ruta completa de opt/ (Makefile).
Comprobado: compila entero sin el toolchain en el PATH, kernel del mismo
tamaño (6930312) que con él.

**Punto 21: actualizador desde GitHub, importado de ZXMetalCircle (pedido
13-nov) - DISEÑO PROPUESTO, esperando respuesta del usuario.**
Origen (github.com/shaeon/ZXMetalCircle, src/): updater.cpp/.h (reglas:
CRC de todo antes de tocar, solo lo que cambia, .new + rename, kernels al
final, update/EN_CURSO, configuración como .nuevo), zipfile + inflate,
updatefs.cpp (FatFs), download/httpsclient + circleglue (Mbed TLS propio en
third_party/mbedtls con zxmc_config.h), kernelupdate.cpp (pantalla: NO se
trae, se rehace en nuestro menú).  Nuestro lado: circle-stdlib trae fuentes de
mbedtls sin compilar; el workflow YA publica release en etiqueta v*
(BM-DOS-card.zip + kernels).  Etiquetas: shaeon v0.66/v0.67/v0.78; mmoroca
hasta v0.80 (sus releases no llevan nuestro trabajo).
DECIDIDO (13-nov): numeración PROPIA (no la de mmoroca); repositorio por
defecto shaeon/baremetaldos (configurable); protegidos boot.conf, dosbox.conf,
cmdline.txt, config.txt, wpa_supplicant.conf (el nuevo como .nuevo, con opción
de usar los nuevos y guardar los del usuario como .bak) y dos/ entera; sección
"Actualización" en Sistema.  APROBADO: F1 -> F4.
También pedido: valores por defecto con GUS, MIDI y MPU-401 inteligente (HECHO,
commit "defaults: Gravis Ultrasound...").
F1 HECHA: src/update/ (updater, updatefs, zipfile, inflate, importados de
ZXMetalCircle 61494ca con su origen en la cabecera; cambios en updater.h:
lista de configuración BM-DOS, dos/ nunca se toca en lugar de esxdos_i, sin
paquete anterior, BM-DOS-card.zip / update/BM-DOS-<v>-card.zip).  make
check-update: 35 casos (escenarios de ZXMC reescritos + dos/, wpa_supplicant,
kernels de BM-DOS, deflate real con zipfile de Python y CRC), limpio con
ASan/UBSan.  En APP_SRCS (aún sin usar desde el kernel).
F2 APROBADA (13-nov), EN CURSO: HTTPS.  Hecho: MBEDTLS_REV v3.6.2 (la copia de
ZXMC es idéntica a la oficial) descargada por make fetch; config
src/update/bmdos_mbedtls_config.h; httpsclient + circleglue en src/update
(red como el resto del port); sd/certs/cacert.pem = Mozilla actual (118) + 3
raíces DigiCert retiradas por antigüedad que GitHub ha usado (NO las retiradas
por desconfianza), copiado por make-sd.sh a certs/.  ARREGLO de la F1: en la
tarjeta es dosbox/dosbox.conf (no la raíz) y dosbox/saves/ (partidas) tampoco
se toca.  check-update-live (red, fuera de check-host; CERTS=- con proxy): API,
plan B, descarga con redirección y un zip real de 67 entradas, todo bien.
Tu repo NO tiene releases publicadas todavía (solo etiquetas).
Kernel compilado con Mbed TLS (108 ficheros, +190 KB); check-host desde cero bien.
F3 APROBADA (13-nov), EN CURSO.  Antes, pedido: paquete como ZXMC - versión
en un fichero (BMDOS_VERSION, empieza en 0.80, +0.01 cada release), artefacto
subido como CARPETA (no zip dentro de zip), release BORRADOR con un único
BM-DOS-<v>-sd.zip (vale para el actualizador y para descomprimir en la SD);
el actualizador vuelve a buscar -sd.zip; el workflow para si la etiqueta no
coincide con BMDOS_VERSION.
Preguntas hechas: repositorio por defecto (propuesto shaeon/baremetaldos,
configurable), numeración de nuestras etiquetas, ficheros protegidos (5 de
configuración + dos/ entero), sitio en el menú (Sistema), fases F1-F4.

### Revisión completa del proyecto (pedida por el usuario, 12-nov)

"Ahora que funciona y estamos afinando: revísalo y ve arreglando lo que veas;
si hay dudas, pregunta antes."  Orden de trabajo; marcar cada casilla al
terminarla y anotar debajo lo encontrado:

**Punto 1 de la lista (P1) - HECHO, pendiente de probar en placa:** en el
panel, `bd` debe quedar en ~70 (no 77), `drop` cerca de 0 y `HDMI` hacia la
mitad de la cola.  (El 2, governor por frame, no aprobado aún.)
Pasos: [x] stash pop (conflicto del Makefile resuelto: check-keys y check-pace)
[x] revisar pace_policy.h + prueba (correctos)  [x] revisar bare_thread /
kernel / bare_audio  [x] quitar lo del punto 2 (división por fotograma)
[x] arreglo propio: dos marcapasos peleaban (el del bucle frenaba al
metrónomo cuando el ajuste pedía acelerar); el del bucle ya no espera y sus
cuentas salen del metrónomo  [x] pruebas PC  [x] compilar  [x] commit + bundle ("pace: one metronome...").

- [x] R1. **Teclado en el menú** - hecho, compilado y en el commit "input: ...":
      - "muchas letras": bare_input.cpp entregaba "pulsada" toda tecla presente
        en cada informe USB (no solo las nuevas) -> key_report.h + check-keys.
        Afectaba también a DOS.
      - Mayús izquierdo: el manejador entrega los 8 modificadores en cada
        cambio (izquierdos primero); el menú tenía un solo flag de Mayús y el
        "derecho: suelto" borraba el "izquierdo: pulsado" -> un flag por tecla.
      - tecla ç (ISO, uso USB 0x32) era RETROK_UNKNOWN: no hacía nada ni en DOS
        -> RETROK_BACKSLASH (scancode 0x2B, lo que manda un PC).
      - Bloq Mayús en el menú (BareInputCapsLock); Retroceso con repetición.
      - distribución del menú: de cmdline.txt keymap= (teclado real), si no KEYB.
      - la contraseña se VE al escribir (Tab la oculta) y una línea dice con
        qué distribución se leen las teclas.
      - WiFi: se rechazaban comillas y barras invertidas; wpa_supplicant lee
        hasta la ÚLTIMA comilla (config.c os_strrchr) -> permitidas.
- [x] R2. Menú (punto 11 de la lista).  NOTA: encontrado a medias de una
      sesión cortada; su petición ("revisa todo el texto...") no está en la
      conversación visible, pero entra en la revisión aprobada.  Revisado: la
      prueba nueva da 14 fallos con el menú anterior y 0 con el nuevo.
      Pedido (según la sesión cortada) 12-nov junto con:
      "revisa todo el texto, hay partes que se cortan o no caben").
      Plan: [x] prueba en el PC que construye todas las pestañas con el
      bare_menu.cpp real (make check-menu-text, en check-host): 15 textos no
      cabían -> 0  [x] corregidos (valores cortos + texto completo en la
      ayuda; nombres largos con "~" al estilo DOS; Rendimiento por niveles;
      Red cabe sin desplazar)  [x] releído LoadSettings / Discard / Defaults:
      el País del WiFi nunca se leía (caía en la rama de boot.conf) y buscaba
      country= también en comentarios -> arreglado y con prueba
      [x] compilar, commit, bundle ("menu: every text fits...").
      Antes:
      Hecho: releído SaveSettings.  Encontrado y corregido: "País del WiFi"
      cambiado sin elegir red se marcaba guardado y no se escribía (ahora
      actualiza la línea country= de wpa_supplicant.conf).
      FALTA: releer LoadSettings (valores a medida, nDefault), "Fijar la IP
      actual", Discard/LoadDefaults con los ajustes en vivo, y probar en placa
      cada pestaña (cambiar, F10, mirar el fichero en el PC).
**R3-R6 APROBADOS por el usuario (12-nov): revisar y arreglar, en orden.**
- [x] R3. Arranque: orden, esperas, logo, SUPR, reinicio de DOS.  Encontrado y
      corregido: (a) "arranque: X s" se medía antes de cargar DOSBox (lo más
      lento); ahora después de "core: load ok"; (b) en el menú del arranque
      (SUPR, antes de que exista DOSBox) guardar opciones R pedía reiniciar
      DOS y ese reinicio corría justo tras arrancar: s_bBeforeDos; (c) tras
      reiniciar DOS el menú seguía mostrando las imágenes de A:/D:
      (BareMenuOnDosRestart).  Revisado sin cambios: orden de init, NTP solo
      si hay red configurada, doble cierre del MIDI aparcado (inofensivo).
- [x] R4. Audio y MIDI: releído BareAudioQueue (mezcla MIDI, filtro de
      continua, limitador, volumen, descartes, huecos) y el cierre aparcado
      del MIDI: sin fallos.  Sigue pendiente la saturación MIDI en DOOM
      (necesita una grabación del usuario).
- [x] R5. Vídeo: el mensaje "Reiniciando DOS..." se presenta ANTES de
      desmontar DOSBox, así que la última imagen guardada nunca apunta a un
      búfer liberado; menú encima y restaurar, bien.  Sin cambios.
- [x] R6. Red: el servidor web espera la red en su tarea (bien).  Encontrado:
      FatFs sin bloqueo (FF_FS_LOCK 0): borrar/mover/renombrar desde la web
      un fichero que DOS tiene abierto puede corromper el volumen.  Hecho lo
      seguro: la web rechaza tocar una imagen metida en A:/D: (o su carpeta)
      con BareDiskInUse.  **PREGUNTA AL USUARIO pendiente:** activar
      FF_FS_LOCK (parche a Circle) para cubrir los ficheros abiertos por los
      juegos; coste: límite de ficheros abiertos a la vez (p. ej. 64).
      **DECIDIDO (usuario, 12-nov): NO el bloqueo de FatFs (impediría a un
      programa abrir dos veces para escribir el mismo fichero), SÍ consultar
      la tabla de ficheros abiertos de DOSBox (Files[]) desde la web.**  Hecho en
      el código: bare_dosfiles.cpp (BareDosFileOpen, con las banderas de DOSBox),
      la web lo consulta junto a BareDiskInUse.  Compilado; commit "web: ...open files".
      Probar en placa: con un juego guardando partida, intentar borrar su
      fichero desde la web -> debe negarse con el motivo.
      Compilado y en el commit "review R3-R6: ...".  Pregunta respondida (ver arriba).
- [x] R7. Governor y ritmo: hecho con los puntos 1 (metrónomo) y 2 (governor
      por fotograma).
- [ ] R8. CI, make-sd, parches, README.

## Pendiente

- Previstos en el menú: asignar botones del mando, lanzador de juegos,
  corrección de aspecto, panel de estado, actualizar desde GitHub.
- Arreglo del Makefile con GNU Make 4.3 (PATH en `$(shell)`).
- Probar en placa todo lo del menú y la red (el usuario aún no lo ha probado),
  y el reinicio de DOS (ver la fila de Ctrl+Alt+Supr).
- Llevar el trabajo a Miguel Ángel (mmoroca) como PR.

## Entrega con parches (desde el 12-nov, a petición del usuario)

En vez de bundle: `git format-patch --binary --stdout <su origin/main>..HEAD >
baremetaldos-<hash>.patch`; el usuario: `git am --3way fichero.patch` y `git
push origin main`.  Conserva autor, fecha y mensaje, pero los HASHES CAMBIAN
(el committer es él).  Por eso, al empezar cada entrega: `git fetch origin`
(en /tmp/chk) y, si su árbol es idéntico al mío, alinear mi main con el suyo
(`git reset --hard origin/main` tras comprobar `git diff` vacío) para que los
hashes de PROGRESO, del sello BMDOS_COMMIT y de las bases coincidan.  El sello
del kernel que entrego lleva MI hash; el de GitHub Actions, el suyo.

## Hoja de ruta de rendimiento (12-nov, pendiente de que el usuario elija)

A. Governor: A1 ver qué hace (datos en Rendimiento), A2 subir antes (huecos de
   audio como señal, calentamiento al arrancar un programa), A4 margen de
   temperatura.  (A3 overclock: DESCARTADO por ahora, decisión del usuario
   12-nov: "nada de overclocking de momento".)
B. Perfilador por partes del frame en el núcleo 0.
C. Compilación: -O3 y LTO en DOSBox-core (medir).
D. Ajustes de DOSBox en el menú: oplemu, frecuencia del mezclador, ciclos
   automáticos.
E. No copiar el frame: DESCARTADO (12-nov), a petición del usuario se midió:
   el perfilador da "Vídeo 0,4 %" del núcleo 0 (BareVideoPresent entero, copia
   incluida; ~3 % como mucho con la pantalla entera cambiando a 70 fps), y
   dosbox-core no admite GET_CURRENT_SOFTWARE_FRAMEBUFFER: evitar la copia
   obligaría a tocar el render de DOSBox o a que el núcleo 2 lea el búfer de
   DOSBox mientras se escribe (desgarros).  No compensa.
F. OPL en el núcleo 3 (solo si B lo justifica).

Lectura del panel del usuario (12-nov, escena de créditos, Pi 4):
`v2 39/s ov0 12.6ms vs | 1500 MHz 43C | L1/2 max over3215 | pace 38/70 w0% bd77
dup 0 + | fr 25673us | HDMI 4734/5292 | drop 9512 gap 0`.  Núcleo 2 bien; la
temperatura ya se lee.  bd77 > 70: la máquina avanza ~10 % más rápido que el
tiempo real sin que el bucle la frene (w0%, "+"), el audio llega de más y se
tira (drop 9512 muestras/s = ~11 % de 88200): posible origen de chasquidos.
El governor recibe el tiempo de toda la llamada (fr 25.7 ms, con ~2 fronteras
dentro) contra el presupuesto de UN frame (14.3 ms) -> ve sobrecarga
(over3215) que no es real y se queda en el máximo.  Propuesta: P1 frenar a la
máquina cuando va por delante (prioritario), P2 governor midiendo por
frontera (A1/A2).

## Otros emuladores (valorado el 12-nov a petición del usuario; sin decisión)

BM-DOS es un frontend libretro: cambiar de núcleo cuesta poco solo si el otro
también es un core libretro.  DOSBox Pure lo es (Voodoo, Windows 9x, ZIP);
DOSBox Staging, DOSBox-X y 86Box son aplicaciones SDL/Qt: port nuevo.  86Box
además es emulación de bajo nivel: pide BIOS, DOS instalado en imagen de disco
y mucha más CPU (su FAQ: ~700 puntos mono-hilo para un 486DX2/66).
Recomendación dada: seguir con dosbox-core.

IDEA PARA EL FUTURO (usuario, 12-nov): un proyecto aparte, el mismo frontend
(Circle + libretro) con DOSBox Pure como núcleo, para probar cómo va.  Si se
empezara de cero, sería la elección: un core libretro que lo trae casi todo
dentro (sin el SDL falso, SDL_net, FluidSynth ni Munt externos), con Voodoo por
software, juegos desde ZIP, lanzador y apoyo para Windows 9x, y la misma
velocidad que DOSBox.  Sin probar en bare-metal: lo incierto es su dynarec
ARM64 y sus hilos en Circle (lo que más costó con dosbox-core), y su menú
propio, que habría que apagar o rodear.  ESTE proyecto sigue con dosbox-core.  Dato útil: el código de Voodoo
YA está en third_party/dosbox-core/src/hardware/voodoo*.cpp, excluido en el
Makefile (WITH_VOODOO=0, "OpenGL"): mirar si su ruta por software compila
(punto 23).

## Upstream (mmoroca) desde nuestra base v0.78 (mirado el 13-nov)

Dos commits: 444e70d (quita un script local) y 4301875 = v0.80
("2026-11-11.80"): MIDI General MIDI con TinySoundFont (tsf.h, solo cabecera)
dentro de midi_stub.cpp; lector de SoundFont rápido (búfer propio de 64 KB
alineado a 64, _IONBF, reintento de bloques con EIO: 9863 ms -> 3836 ms con
31 MB); guarda de memoria antes de cargar (Circle hace panic sin memoria, no
devuelve NULL); cabecera del arranque en una caja con glifos CP437 y columna
que se desplaza (bare_video.cpp); setup.sh genera circle/memory.h; READMEs y
dosbox.conf documentan [midi]; pruebas check-sf2reader y status_test.
Choca con lo nuestro: nuestro MIDI es FluidSynth + Munt en el núcleo 1
(midi_stub.cpp, bare_midi.cpp), y bare_video.cpp, Makefile, READMEs,
dosbox.conf, bare_env.h y bare_sysinfo.cpp también los hemos cambiado.
Aprovechable: su forma de leer el SoundFont rápido y con reintentos, y la
guarda de memoria (nuestro FluidSynth carga con fopen/fread de newlib).
HECHO (13-nov, pedido): lector rápido en las cinco funciones de fichero que
usa FluidSynth (bare_midi.cpp: bloque propio de 64 KB alineado a 64, _IONBF,
reintento en trozos de 4 KB), y la guarda de memoria con HEAP_LOW (el montón
de malloc; upstream usa HEAP_ANY, que en Pi 4/5 suma memoria que malloc no
usa).  Probado en el PC: lectura exacta byte a byte (32 MB, 358 lecturas
variadas, saltos, final), guarda que rechaza con 4 MB, carga y suena.  Sin
probar: el camino de reintento (no se puede provocar un EIO en el PC).

## Notas técnicas

- Compilar: `export PATH=$PWD/opt/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf/bin:$PATH; make -j2 RASPPI=4`.
  Con cambios en `bare_env.h` o en el Makefile se recompila casi todo (~15 min).
- Pruebas en el PC: `make check-host` (o las sueltas: check-menu, check-conf,
  check-bootconf, check-platform, check-kernel...).
- `bare_net.cpp`, `bare_sdlnet.cpp`, `bare_web.cpp` no compilan con los
  sustitutos del PC (cabeceras de red de Circle): se listan en check-symbols
  como definidos en otra parte; solo los comprueba la compilación cruzada.
