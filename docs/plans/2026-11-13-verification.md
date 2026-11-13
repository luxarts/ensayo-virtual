# Grabación multipista: verificación (2026-11-13)

Rama `feat/grabacion-multipista`. Máquina: Windows 11 Pro, VS 2022, interfaz **Arturia MiniFuse 2**
("MiniFuse ASIO Driver", 8 entradas / 8 salidas a 48 kHz; las salidas 2/3 "LOOPBACK" vuelven como
entradas 2/3). Red real (Winsock, loopback 127.0.0.1). Binarios Release para la integración, Debug
para las unitarias.

## Pruebas unitarias (Debug)

`.\build\EnsayoTests_artefacts\Debug\EnsayoTests.exe` → **`TOTAL: 231 results, 0 failures`**, exit 0.

230 venían de las tareas 1–10; la nueva es `HeadlessClient / runHeadless prints cached <take> <k>
while the segments of a take still being recorded arrive` (el runner informa 8 checks: 7 `expect`
del test más el de `parse`; puerto 22260): un ProjectClient graba una toma de 4 segmentos (uno cada
400 ms) mientras `runHeadless` observa; antes de `take_complete` aparecen al menos 3 cantidades
distintas de `cached <take> <k>`, y la línea `cached <take> 4` de la toma completa aparece
*después* de `take_complete` (no alcanza con una parcial con el mismo número). Además el test de parseo cubre `--import-url` (URL válida, URL que no es de
YouTube, falta de valor, sin sesión).

## Integración (Release, `tests\integration\run-all.ps1`)

Procesos reales: `EnsayoServer.exe --project <dir>` hostea un proyecto y cada cliente es
`Ensayo Virtual.exe --headless` con su propio `--user` y `--cache`. El host se conecta como el
usuario host (`host_user`) desde 127.0.0.1. Salida de la última corrida completa (exit 0):

| Script | Puerto | Resultado | Tiempo | Medidas |
|---|---|---|---|---|
| screenshots | – | PASS | 3.9 s | 23 controles del lobby y 79 de la sesión presentes, visibles y con tamaño; ventana real `--demo` capturada (1296×839) |
| record-sync | 22240 | PASS | 42.9 s | A recibió los segmentos k = 1..7 de la toma de B antes del `take_complete` de B; hash igual en A y B; región de B {start 48000, offset 464}; **23 clicks, error máximo 0 muestras** |
| edit-sync | 22241 | PASS | 31 s | 6 ediciones de B aplicadas, 1 rechazada (`not allowed to edit this track`); A, B y C con el mismo `project_hash` |
| reconnect | 22242 | PASS | 5.9 s | B muerto con 3 segmentos en su caché; `--resume` completa 144000 muestras; en el host segmentos `0,1,2`, toma completa, región {0, 0, 144000} |
| reopen | 22243 | PASS | 3.9 s | mismo id de sesión y mismo host tras reiniciar el servidor; B obtiene el mismo hash; un cliente nuevo baja la toma y tiene las mismas pistas y tomas |
| import-export | 22244 | PASS | 5.9 s | WAV 44.1 kHz → 144000 muestras exactas, clicks exportados en k·12000 (error 0); YouTube ("Me at the zoo", 19.1 s, 915052 muestras); exportaciones 24 bits / 48 kHz / estéreo con largo = fin del proyecto (144000 y 915052) |
| asio-calibration | 22245 | PASS | 16.6 s | MiniFuse: latencia reportada in 328 + out 408; **calibración 44 muestras**; 15 clicks grabados por el loopback, **error máximo 0 muestras** (límite 48) |

Las salidas quedan en `tests/integration/out/<script>/` (ignoradas por git).

### Cómo se mide la alineación

- **record-sync** (dispositivo falso en modo loopback, latencias ocultas 200 + 264, bloques de 256):
  B graba su propia reproducción de la pista de clicks de A. A exporta dos veces el mismo proyecto
  (`--ops` con `set_mix muted`): sólo la pista de clicks y sólo la pista de B. En cada WAV, el
  "onset" de un click es la primera muestra del canal izquierdo que llega a la mitad del pico del
  archivo después de 2000 muestras de silencio. Cada onset de la pista de B dentro de su región
  (fuera de los 144 de fundido de los bordes) se compara con el onset de referencia más cercano:
  los 23 coinciden en la misma muestra (error 0; el plan admite ±1). Se elige la exportación y no
  los archivos de caché porque recorre el render real (región, offset, fundidos, mute).
- **asio-calibration**: lo mismo, con el hardware: la pista de clicks sale por LOOPBACK (salidas
  2/3) y vuelve por la entrada 2. Con la calibración medida en esa misma apertura del dispositivo,
  el offset de la región es 328 + 408 + 44 = 780 y los 15 clicks caen en la misma muestra que la
  referencia. Sin la calibración habrían quedado corridos 44 muestras (0.9 ms).
- La calibración varía entre aperturas del dispositivo: 44 muestras en dos corridas, 37 en otra (con
  otro proceso usando la MiniFuse a la vez). En las tres la toma quedó con error 0, porque se calibra
  y se graba en la misma apertura.

### Recepción progresiva

En record-sync, A imprime `cached <toma de B> <k>` cada vez que crece la cantidad de segmentos de esa
toma en su caché. El script registra cuándo aparece cada línea y cuándo imprime B `take_complete`:
A tenía k = 1, 2, 3, 4, 5, 6, 7 antes de que la toma se completara (se pide ≥ 3).

### Corrida con la MiniFuse ocupada

El script se salta (SKIP) sólo si el driver no está instalado, o si el paso 1 (`--calibrate`) no
puede abrir el dispositivo con uno de los errores de apertura de JUCE para un dispositivo ausente u
ocupado, tal como los imprime el cliente headless: `error ASIO device: No Driver`, `Driver failed
to initialise`, `No such device`, `Can't start device`, `Device didn't start correctly`, o `error
ASIO device did not open`. Cualquier otro error, y cualquier fallo después de que el paso 1 abrió el
dispositivo (la grabación, las exportaciones), es FAIL. Un driver que devuelve su propio texto de
error al inicializar también da FAIL.

**La rama de SKIP por dispositivo ocupado no está probada**: el driver de la MiniFuse admite varios
programas a la vez. Con un proceso headless que tenía la MiniFuse abierta, el script abrió el
dispositivo igual y dio PASS (calibración 37, error 0).

## Capturas revisadas

- `out/screenshots/shots/lobby.png` (1100×760): Nombre, Rol, Crear sesión y Abrir proyecto…, Link
  de sesión y Unirse; Dispositivo ASIO, Frecuencia fija en 48000 Hz, Buffer 256, canales, Panel ASIO,
  Latencia, Calibrar latencia con "Calibración: sin calibrar"; VST; barra de estado. Sin solapamientos.
- `out/screenshots/shots/session.png` (1100×760): proyecto de demostración con 4 pistas: Guitarra
  rítmica y Voz guía (mías, con ●R y formas de onda), Bajo de "Martín" con "Bloq." y la región roja
  "● grabando (Martín)", y Batería (YouTube) gris "descargando 64 %". Transporte (Grabar, 0:05.000 ·
  2.4.3, BPM 92, 4/4, Click, Cuenta), Cortar (S), Borrar (Supr), Exportar mezcla…; barra de estado
  "Sincronizado (v 128) · Ping: 12 ms · Subidas: 0 · Descargas: 1 · Demo · 48 kHz · 256 · CPU: 3 %".
- `out/screenshots/shots/demo-window.png` (1296×839): la ventana real de `--demo`, igual al render
  (con el nombre de usuario de `settings.json`).

`expected-controls.json` suma los contenedores `track.1`–`track.4` (la sesión pasa de 75 a 79
nombres).

## Cambios en el producto que hicieron falta

1. **`--headless` no mostraba los segmentos que llegan durante la grabación.** `cached <take> <n>` sólo
   se imprimía para tomas completas, así que no había forma de comprobar desde afuera que A recibe la
   toma de B mientras se graba. Ahora, para una toma todavía incompleta, se imprime
   `cached <take> <k>` cada vez que crece la cantidad de segmentos en la caché local (la línea de la
   toma completa no cambia). Test unitario nuevo (arriba).
2. **`--headless --import-url <url>`**: baja una URL de YouTube con `tools\yt-dlp.exe` (+ deno) por
   el mismo `YouTubeImporter` de "Importar de YouTube", imprime `downloaded <archivo>` y lo importa
   como `--import`. Un fallo de la bajada imprime `error youtube: <motivo>` (con las últimas líneas
   de yt-dlp). El script lo toma como SKIP de esa parte sólo si el motivo es de red (resolver el
   nombre, conexión, error HTTP, timeout); por ejemplo un video que no existe ("This video is
   unavailable", probado a mano) es FAIL. Tests de parseo.
3. `setHeadlessOutputCopyForTest` (sólo para tests): copia cada línea que imprime el modo headless.

No apareció ningún bug de alineación, sincronización, permisos, reanudación ni reapertura.

## Observaciones

- `--wait-takes N` termina cuando N tomas están completas y en caché; la región de una toma grabada
  (`add_region`) llega una versión después. Un cliente que sale con `--wait-takes` puede imprimir un
  hash una versión atrasado. Por eso los hashes se comparan así: el cliente que graba sale recién
  cuando la región de su toma está en su réplica (`ProjectClient` suelta la toma pendiente sólo
  entonces), así que su hash es el final; los que observan (A en record-sync, A y C en edit-sync)
  se quedan un tiempo fijo y el script exige que su última versión `applied` sea igual a la del que
  grabó/editó antes de comparar hashes (si no, FAIL con "raise --duration"). En reopen, el hash de
  antes del reinicio viene del que grabó, y el script verifica que su dump ya tiene la región.
- Un usuario nuevo que se une agrega `add_user` al proyecto y cambia el hash: en reopen, el cliente
  nuevo se compara por pistas y tomas, y el que ya era miembro por hash.
- En reconnect se pierde el segundo que todavía no se había cortado en segmento cuando se mató el
  proceso (la toma queda en 3 s exactos): es lo esperado para un cierre abrupto; el WAV local
  (`recordings\<take>.wav`) puede tener algo más, pero la recuperación usa lo que hay en la cola de
  subida.

## Lo que el spec §9 pide y no cubre un script

- **Corte de red a mitad de grabación**: reconnect mata el proceso (peor caso). El corte de red sin
  matar el proceso está en las unitarias (`ProjectClient` reconexión con `dropConnectionForTest`,
  re-anuncio de la toma y sus picos; `Ui` sólo lectura y Reintentar).
- **Subida limitada que se encola y se vacía**: no hay forma de limitar el ancho de banda desde los
  scripts; la cola persistente de subidas está cubierta por las unitarias de `ProjectClient`
  (pendientes tras reinicio, `--resume`).
- **Automatización de la ventana real** (crear pista, grabar, cortar, mover): cubierta por las
  unitarias `Ui` y `DawView` sobre los componentes reales; el script sólo captura la ventana `--demo`.

---

## Adenda: chat de voz, transporte y vúmetros (2026-11-13, tarea 4)

Misma máquina y misma rama, sobre el plan `docs/plans/2026-11-13-chat-de-voz-y-transporte.md`. Esta vez las
unitarias corrieron en **Release** (`build\EnsayoTests_artefacts\Release\EnsayoTests.exe`), y después, no a la
vez, `run-all.ps1`.

### Unitarias

**`TOTAL: 271 results, 0 failures`**, exit 0. Lo nuevo o cambiado en esta tarea:

- `StudioEngine / received streams: start, end and concealment are faded` y `... a lost packet is concealed
  without a click`: el contador de recepción (`chatReceivedFrames`) cuenta sólo cuadros reales: 300 de 300
  enviados, y 299 de 299 con un paquete perdido (el cuadro ocultado y los 18 de ocultamiento del final no
  cuentan).
- `HeadlessClient / runHeadless sends and receives the voice chat and prints chat_rx per received stream at exit`
  (puerto 22234): otro integrante en el mismo proceso (motor con seno en el micrófono, unido con
  `SessionController`) y `runHeadless --fake-audio sine --duration 3`. El headless imprimió
  `chat_rx <clientId de Rita> voice 1126` y Rita recibió 1130 cuadros de voz del headless (3 s ≈ 1125).
- `DawView / the delete button ...`: durante mi propia toma no se puede borrar ninguna pista (tooltip "No se
  pueden borrar pistas mientras grabás"), igual que el resto de la edición; también para el host.
- `Ui / a guest joins a link; a lost connection is read-only (Play with an armed track only plays) ...`: en
  "Sólo lectura" con una pista propia armada, Play reproduce: no intenta grabar ni muestra un error.
- `UiTests` usaba el puerto 22246; pasó a 22233 para que 22240–22246 quede sólo para la integración.

### Integración

| Script | Puerto | Resultado | Tiempo | Medidas |
|---|---|---|---|---|
| screenshots | – | PASS | 4.2 s | controles OK; ventana `--demo` capturada |
| record-sync | 22240 | PASS | 42.6 s | A tenía k = 1..6 de la toma de B antes de su `take_complete`; hashes iguales; offset 464; **23 clicks, error máximo 0** |
| edit-sync | 22241 | PASS | 30.8 s | 6 ediciones aplicadas, 1 rechazada; A, B y C con el mismo hash |
| reconnect | 22242 | PASS | 5.5 s | 3 segmentos; `--resume` completa 144000 muestras |
| reopen | 22243 | PASS | 3.5 s | misma sesión y host; mismo hash; C baja la toma |
| import-export | 22244 | PASS | 6.5 s | WAV 144000 muestras, clicks ±0; YouTube 19.1 s, 915052 muestras |
| **voice-chat** | 22246 | PASS | 12.7 s | 6 s de solapamiento, 2250 cuadros esperados (mínimo 1800): **A recibió 2258 de B, B recibió 2253 de A** (100 %) |
| asio-calibration | 22245 | PASS | 16 s | calibración 38; 15 clicks, error máximo 0 (límite 48) |

- **voice-chat** (nuevo): A hostea con `--host`, B se une; los dos con `--fake-audio sine`, sin grabar. Cada uno
  imprime al salir `chat_rx <clientId> voice|instrument <cuadros>` (cuadros Opus de 128 muestras decodificados,
  sin contar los ocultados). Se exige exactamente un stream de voz en cada lado, ninguno de instrumento, y al
  menos el 80 % de 375 × 6. El conteo pasa apenas de 2250 porque B manda desde que se conecta (antes de su
  `welcome`) hasta que cierra, un poco más que sus 6 s.
- **record-sync con el chat siempre activo**: B graba en loopback su propia salida, y en esa salida también
  suena el chat de A. A queda en `--fake-audio silence` (el valor por defecto), así que su chat es silencio y
  la toma de B sigue con error 0. No hizo falta silenciar el chat de B. Ningún otro script usaba la escucha en
  vivo ni la semántica vieja de Stop/Grabar (graban por `--record`, que usa `startTake`/`stopTake`).

### Capturas revisadas

- `session.png` y `demo-window.png`: transporte con **|<** y **▶ Play** (sin Stop ni Grabar ni Escuchar en
  vivo); cada pista con **×** (deshabilitada en el Bajo de Martín, que no es mía) y vúmetro con número en dB
  (el del Bajo, en +0.7, con el testigo de clip rojo); panel **Mi audio** con los cuatro faders y "Silenciar
  mi micrófono"; panel **Participantes** con Martín (-4.5 dB) y Sofía (silenciada, **M** rojo). Sin
  solapamientos.
- `lobby.png`: sin cambios.

### Cambios en el producto de esta tarea

1. `StudioEngine::chatReceivedFrames()`: cuadros reales recibidos por stream (`clientId`, tipo), atómicos que el
   callback sólo incrementa; `--headless` los imprime como `chat_rx` al salir.
2. Play en "Sólo lectura" con una pista armada reproduce en vez de intentar grabar.
3. Durante mi propia toma se bloquea borrar cualquier pista.
