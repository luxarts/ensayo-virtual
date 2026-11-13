# Grabación multipista: verificación (2026-11-13)

Rama `feat/grabacion-multipista`. Máquina: Windows 11 Pro, VS 2022, interfaz **Arturia MiniFuse 2**
("MiniFuse ASIO Driver", 8 entradas / 8 salidas a 48 kHz; las salidas 2/3 "LOOPBACK" vuelven como
entradas 2/3). Red real (Winsock, loopback 127.0.0.1). Binarios Release para la integración, Debug
para las unitarias.

## Pruebas unitarias (Debug)

`.\build\EnsayoTests_artefacts\Debug\EnsayoTests.exe` → **`TOTAL: 231 results, 0 failures`**, exit 0.

230 venían de las tareas 1–10; la nueva es `HeadlessClient / runHeadless prints cached <take> <k>
while the segments of a take still being recorded arrive` (8 checks, puerto 22260): un ProjectClient
graba una toma de 4 segmentos (uno cada 400 ms) mientras `runHeadless` observa; antes de
`take_complete` aparecen al menos 3 cantidades distintas de `cached <take> <k>`, y después la línea
de la toma completa. Además el test de parseo cubre `--import-url` (URL válida, URL que no es de
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

El script se salta (SKIP) si el driver no está instalado o no se puede abrir. Para probar el caso
"otro programa tiene el driver" se dejó un proceso headless con la MiniFuse abierta y se corrió el
script: el driver de la MiniFuse admite varios clientes, así que no falló al abrir y el script dio
PASS (calibración 37, error 0). La rama de SKIP por dispositivo tomado no se puede ejercitar con
esta interfaz.

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
   como `--import`. Un fallo de la bajada imprime `error youtube: <motivo>` (el script lo toma como
   SKIP de esa parte). Tests de parseo.
3. `setHeadlessOutputCopyForTest` (sólo para tests): copia cada línea que imprime el modo headless.

No apareció ningún bug de alineación, sincronización, permisos, reanudación ni reapertura.

## Observaciones

- `--wait-takes N` termina cuando N tomas están completas y en caché; la región de una toma grabada
  (`add_region`) llega una versión después. Un cliente que sale con `--wait-takes` puede imprimir un
  hash una versión atrasado. record-sync usa `--duration 40` en A para comparar hashes.
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
