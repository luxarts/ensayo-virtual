# Ensayo Virtual

Grabador multipista colaborativo para bandas, escrito en C++20 / JUCE 8. Cada músico graba en su
casa, con su interfaz **ASIO**, sobre lo que ya grabaron los demás; las tomas llegan a todos
mientras se graban y cualquiera puede editar sus pistas (cortar, mover, recortar, volumen, paneo,
mute). Al final se exporta la mezcla a WAV.

No es un ensayo en vivo: tocar en tiempo real por internet chocaba con la latencia y el jitter de
la red. Acá cada toma se graba localmente sin cortes y por la red viajan sólo archivos (FLAC) y
operaciones de edición, por TCP, así que nada de lo grabado se pierde ni se desfasa. Para hablar
mientras tanto hay un **chat de voz** siempre abierto (Opus por UDP, con un poco de demora), que
no toca lo grabado.

```
músico A (host)                                         músico B (invitado)
┌───────────────────────────────┐   TCP control (ops)   ┌──────────────────────────────┐
│ proyecto: project.json + tomas│◄─────────────────────►│ réplica del proyecto         │
│ servidor embebido (ProjectHost│   TCP datos (FLAC)    │ caché local de tomas         │
│ puerto 22140)                 │◄─────────────────────►│ graba en su PC, sube 1 s a   │
│ graba, edita, reproduce       │   UDP chat de voz     │ la vez mientras graba        │
│ reenvía el chat a todos       │◄ ─ (y la toma en ─ ─ ►│                              │
│                               │    curso, en vivo)    │                              │
└───────────────────────────────┘                       └──────────────────────────────┘
```

- **El proyecto vive en la PC del host** (`%APPDATA%\EnsayoVirtual\projects\<id>\`). Cada invitado
  guarda una copia en caché (`%LOCALAPPDATA%\EnsayoVirtual\cache\`) y puede seguir escuchando sin
  conexión; para editar o grabar hace falta que el host esté conectado.
- **Tomas progresivas**: mientras grabás, cada segundo de audio se codifica a FLAC 24 bits y se
  sube; los demás lo ven crecer en rojo y lo reciben al instante. Al parar no hay que esperar.
- **Alineación exacta**: la toma se ubica en la línea de tiempo compensando la latencia de entrada y
  salida que reporta el driver más el desvío medido con **Calibrar latencia**.
- **Permisos**: cada uno edita sus pistas; el host puede editar todo. Las pistas de otros muestran
  un candado ("Bloq.").

El diseño y el plan están en
[`docs/specs/2026-11-13-grabacion-multipista-design.md`](docs/specs/2026-11-13-grabacion-multipista-design.md)
y [`docs/plans/2026-11-13-grabacion-multipista.md`](docs/plans/2026-11-13-grabacion-multipista.md);
el chat de voz, el transporte y los vúmetros, en
[`docs/specs/2026-11-13-chat-de-voz-y-transporte-design.md`](docs/specs/2026-11-13-chat-de-voz-y-transporte-design.md);
la verificación con números, en [`docs/plans/2026-11-13-verification.md`](docs/plans/2026-11-13-verification.md).

> Solo Windows 10/11 x64. Audio a 48 kHz.

## Compilar

Requisitos: Visual Studio 2022 (o Build Tools) con "Desarrollo de escritorio con C++", CMake ≥ 3.22,
Git. JUCE (`8.0.17-security`) y libopus (`v1.6.1`) se descargan solos en la primera configuración
(o se usan desde `.deps/` si ya están clonados ahí).

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release -- /m
```

Binarios:

```
build\EnsayoVirtual_artefacts\Release\Ensayo Virtual.exe      (+ tools\yt-dlp.exe y tools\deno.exe)
build\EnsayoServer_artefacts\Release\EnsayoServer.exe
```

### Pruebas

```powershell
cmake --build build --config Debug --target EnsayoTests -- /m
.\build\EnsayoTests_artefacts\Debug\EnsayoTests.exe            # unitarias; exit 0 = todo OK
powershell -ExecutionPolicy Bypass -File tests\integration\run-all.ps1   # procesos reales (Release)
```

`run-all.ps1` levanta `EnsayoServer.exe` con un proyecto y varios clientes `--headless` con un
dispositivo de audio falso: uno graba sobre la reproducción de otro (y se verifica que los clicks
grabados caigan en la misma muestra que los originales), ediciones de tres clientes que terminan en
el mismo proyecto, un cliente que se cae a mitad de una toma y la retoma, el host que se reinicia,
importar un WAV y un video de YouTube y exportar la mezcla, el chat de voz entre dos clientes
(cada uno recibe al menos el 80 % de los paquetes de voz del otro) y las capturas de la interfaz.
`asio-calibration.ps1` usa hardware real (el loopback del Arturia MiniFuse) y se salta (SKIP) si
el driver no está o no se puede abrir. Detalle en [`tests/integration/README.md`](tests/integration/README.md).

## Usar

### Antes de empezar: audio y calibración

1. Abrir **Ensayo Virtual** y poner tu **Nombre**.
2. En **Audio** elegir el driver ASIO, buffer (256 por defecto; 128 o 64 también andan), canal de
   micrófono, canal de instrumento y par de salida. La frecuencia es siempre 48000 Hz. **Panel
   ASIO** abre el panel del driver.
3. **Calibrar latencia** (una vez por interfaz y tamaño de buffer): conectá la salida elegida a la
   entrada del micrófono con un cable (o usá el loopback de la interfaz, si tiene) y apretá el
   botón. La app emite 8 clicks, los busca en la entrada y guarda el desvío respecto de lo que
   reporta el driver ("Calibración: N muestras"). Sin calibrar también se graba alineado según el
   driver; calibrar corrige a las placas que reportan mal su latencia.
4. Opcional: en **VST (instrumento)** elegir un plugin `.vst3` (amp sim, efectos) para el
   instrumento.

### Crear o abrir un proyecto (host)

- **Crear sesión** crea un proyecto nuevo ("Proyecto dd-mm-aaaa HH.MM"), lo hostea en el puerto
  22140 y te conecta. **Abrir proyecto…** lista los proyectos guardados (o busca un `project.json`
  en otra carpeta) y lo hostea igual.
- Arriba aparece el link `ensayo://<tu IP>:22140/XXXXXX`: **Copiar link** y mandarlo a la banda.
  El link es el mismo cada vez que abrís ese proyecto.

### Unirse (invitado)

Pegar el link en **Link de sesión** y **Unirse**, o hacer clic en el link (la app registra el
esquema `ensayo://` en el primer arranque; si ya está abierta, se une en la misma ventana). La app
baja las tomas que falten; la barra de estado muestra "Subidas: n · Descargas: m".

### Reproducir y moverse

Un solo botón **▶ Play / ❚❚ Pausa** (o la barra **Espacio**). Pausa deja el cursor donde está.

| Tecla | Qué hace |
|---|---|
| **Espacio** | Play / Pausa (con una pista propia armada, Play graba; Pausa termina la toma) |
| **W** | Vuelve al comienzo (igual que **\|<**) |
| **←** / **→** | Mueve el cursor 1 segundo |
| **Shift + ←** / **Shift + →** | Mueve el cursor un compás |
| **S** | Corta la región seleccionada en el cursor |
| **Supr** | Borra la región seleccionada |

W y las flechas no hacen nada mientras grabás. Ninguna tecla actúa mientras escribís en un campo de
texto (nombre de pista, URL de YouTube, BPM).

### Grabar

1. **+ Pista de voz** o **+ Pista de instrumento** (la voz graba el canal de micrófono en mono; el
   instrumento, el canal de instrumento después del VST).
2. Armar la pista con **●R**, poner el cursor donde querés entrar (clic en la regla, **|<**, **W**
   o las flechas). Con la pista armada te escuchás por los auriculares (sólo esa entrada; sin nada
   armado no hay monitoreo).
3. **▶ Play** (o **Espacio**): con una pista propia armada, Play graba. Con **Cuenta** activada
   suena un compás antes; con **Click**, el metrónomo (BPM y compás arriba; los cambia el host).
   Mientras tanto suena el resto del proyecto. Sin pista armada, Play sólo reproduce; en "Sólo
   lectura" (host desconectado) también reproduce aunque haya una pista armada.
4. **❚❚ Pausa** (o **Espacio** de nuevo). La toma queda como una región en tu pista y se termina de
   subir en segundo plano.

Los demás ven tu toma crecer en rojo ("● grabando (tu nombre)") y, si es una pista de instrumento,
la oyen en vivo por el chat con un poco de demora (Opus por UDP); eso es sólo para escuchar: lo que
queda en el proyecto es tu toma original.

### Vúmetros

Cada pista tiene un vúmetro con el pico en dBFS (se sostiene 1,5 s). En tu pista armada muestra lo
que entra (después de la ganancia de envío); en las demás, lo que suena (con volumen, paneo y mute).
Si una señal llega a 0 dBFS se prende el **testigo de clip** en rojo y queda prendido hasta que
hacés clic en el vúmetro.

### Mi audio

Panel de la derecha, sólo para vos (se guarda en tu configuración):

- **Voz – monitor** / **Instrumento – monitor**: cuánto te escuchás (sólo con una pista propia de
  ese tipo armada).
- **Voz – envío** / **Instrumento – envío**: ganancia de tu micrófono y de tu instrumento antes de
  todo: lo que grabás (queda en la toma) y lo que escuchan los demás.
- Doble clic en un fader lo vuelve a 0 dB.

### Chat de voz y Participantes

Mientras estás en una sesión, tu micrófono se escucha siempre en el chat de voz de los demás (y el
de ellos en el tuyo), aunque nadie esté grabando. **Silenciar mi micrófono** (en "Mi audio") deja
de enviar tu voz; no cambia lo que grabás ni tu monitoreo. El chat nunca entra en las tomas de los
demás: cada uno graba sólo sus propias entradas.

El panel **Participantes** tiene una fila por integrante conectado: un fader con su nombre (su voz y
su instrumento en vivo, de -60 a +12 dB) y **M** para silenciarlo. Es sólo para vos y se recuerda
por integrante.

> **Usá auriculares.** Con parlantes, el micrófono capta la reproducción y el chat de los demás:
> eso se cuela en tu toma y los demás se escuchan a sí mismos con eco.

Si se corta la conexión mientras grabás, la toma sigue grabándose en tu PC y se sube al
reconectar. Si la app se cierra de golpe, la toma se recupera la próxima vez que abras ese proyecto.

### Editar

- **Mover**: arrastrar la región. **Recortar**: arrastrar su borde izquierdo o derecho.
- **Cortar (S)**: corta la región seleccionada en el cursor. **Borrar (Supr)**: borra la región.
- Por pista: **M** (mute, para todos), **S** (solo, sólo en tu PC), volumen, paneo y nombre
  (doble clic). Ctrl + rueda hace zoom; la rueda desplaza.
- **Borrar una pista**: la **×** de su encabezado (pide confirmación; sus regiones desaparecen del
  proyecto). Pueden hacerlo su autor y el host; no mientras alguien graba en ella.
- Cada edición se manda al host y le llega igual a todos. Si el host la rechaza (pista ajena, sin
  conexión), la barra de estado dice por qué.
- Mientras grabás, la edición queda bloqueada (también borrar pistas).

### Importar y exportar

- **Importar archivo…** (WAV, FLAC, OGG, MP3, M4A/MP4/AAC; cualquier frecuencia, se convierte a
  48 kHz) crea una pista nueva con el nombre del archivo (de tipo "Importada", sin grabación) con el
  audio desde el comienzo, y se sube a todos.
- **URL de YouTube** + Enter: baja el audio con `yt-dlp` y lo importa igual (la pista se llama como
  el video).
- **Exportar mezcla…** guarda la mezcla (lo que se oye, con mute/solo/volumen/paneo) en un WAV
  24 bits, 48 kHz, estéreo, desde el comienzo hasta el final de la última región.

### Salir y volver

**Salir** vuelve al lobby. Si quedaron segmentos por subir, se envían la próxima vez que abras ese
proyecto. Si el host se desconecta, los invitados quedan en "Sólo lectura: host desconectado":
pueden seguir escuchando lo que tienen en caché y **Reintentar** cuando el host vuelve.

## Hostear: puertos, red y Tailscale

El host usa el puerto **22140**: TCP para el proyecto y las tomas (obligatorio) y UDP para el chat
de voz y el instrumento en vivo (sin UDP se graba y edita igual, pero no se oye a nadie). El host
reenvía el audio de cada uno a todos los demás. Opciones para que la banda llegue a tu PC:

- **Misma red**: alcanza con el link (IP local). Permitir Ensayo Virtual en el firewall de Windows
  (redes privadas) la primera vez que lo pregunta.
- **Tailscale** (recomendado por internet, sin tocar el router): todos instalan Tailscale y entran
  a la misma red; el host comparte el link con su IP de Tailscale (`ensayo://100.x.y.z:22140/ID`).
- **Abrir el puerto**: redirigir 22140 TCP y UDP (el UDP, para el chat de voz) en el router hacia
  la PC del host y compartir el link con la IP pública.

El puerto se cambia con `hostPort` en `settings.json`. `EnsayoServer.exe` hostea un proyecto sin
interfaz (por ejemplo en otra PC que queda prendida):

```
EnsayoServer.exe --project <carpeta> [--port 22140] [--name <nombre>] [--user <id>] [--user-name <nombre>] [--log]
```

Imprime `host_user <id>` y `listening port=N session=ID`; el link es `ensayo://<ip>:N/ID`. Sale con
Ctrl+C o al cerrarse su entrada estándar. El usuario host (`--user`, por defecto el del
`project.json` o uno nuevo) sólo puede conectarse desde la misma PC.

## Modos de línea de comandos

```
"Ensayo Virtual.exe" ensayo://host:puerto/ID            unirse directo
"Ensayo Virtual.exe" --demo                             ventana con un proyecto de demostración (4 pistas)
"Ensayo Virtual.exe" --screenshot <dir>                 lobby.png, session.png y controls.json; sale
"Ensayo Virtual.exe" --headless ...                     cliente sin interfaz (pruebas):
    --host <carpeta> [--port N] | --join <link>          hostear un proyecto o unirse
    --name N  --user <id>  --cache <dir>                 identidad y caché
    --fake-audio silence|sine|click|loopback --block N --latency-in N --latency-out N
    --asio "<driver>" --mic N --inst N --out P --mute-monitor
    --calibrate                                          imprime "calibration <muestras>"
    --add-track voice|instrument <nombre>  --import <archivo>  --import-url <url de YouTube>
    --record <pista> --at <s> --for <s>  --ops <ops.jsonl>  --wait-takes N  --resume
    --export <mezcla.wav>  --dump-project <archivo>  --duration S
EnsayoServer.exe --project <carpeta> [--port N] [--name N] [--user <id>] [--user-name N] [--log]
```

Las líneas que imprime `--headless` (`welcome`, `applied`, `rejected`, `take_complete`, `cached`,
`project_hash`, `chat_rx`, …) están documentadas en `src/client/HeadlessClient.h`. En una sesión
`--headless` el chat de voz anda como en la app: con `--fake-audio sine` el micrófono es un tono de
440 Hz, y al salir imprime `chat_rx <clientId> voice|instrument <cuadros>` por cada audio recibido
de otro integrante (cuadros Opus de 128 muestras decodificados, 375 por segundo).

## Configuración y archivos

| Qué | Dónde |
|---|---|
| Configuración (nombre, id de usuario, dispositivo, canales, buffer, plugin, calibraciones, puerto, ruta de yt-dlp, último proyecto) | `%APPDATA%\EnsayoVirtual\settings.json` |
| Proyectos hosteados (`project.json`, `session.txt`, `takes\<toma>\<n>.flac`) | `%APPDATA%\EnsayoVirtual\projects\<id>\` |
| Caché de tomas, grabaciones locales (`recordings\<toma>.wav`) y subidas pendientes | `%LOCALAPPDATA%\EnsayoVirtual\cache\<proyecto>\` |

Si `settings.json` está dañado se aparta como `settings.json.bad` y se usan valores por defecto.

## YouTube

No hay nada que instalar: el zip de la app trae en `tools\` [`yt-dlp.exe`](https://github.com/yt-dlp/yt-dlp)
y [`deno.exe`](https://deno.com) (el runtime de JavaScript que yt-dlp necesita para YouTube). No
hace falta ffmpeg: el audio se baja tal cual (`m4a`/AAC) y se decodifica con Media Foundation. La
app busca `yt-dlp.exe` en `ytDlpPath` de `settings.json`, en `tools\`, junto al ejecutable y en el
`PATH`. Si compilás vos, la compilación copia `tools\` junto al exe; bajá los binarios con
`.\tools\fetch-tools.ps1`. Descargar contenido de YouTube puede ir contra sus términos de uso: es
responsabilidad de quien lo hace.

## Problemas frecuentes

| Mensaje / síntoma | Qué hacer |
|---|---|
| `No se encontraron drivers ASIO` | Instalar el driver ASIO de la interfaz. |
| `Error de audio: ...` / `No hay un dispositivo de audio funcionando` | Otro programa (DAW) tiene el driver tomado, o se desconectó la interfaz. Cerrarlo, elegir el dispositivo en el lobby y **Reintentar**. |
| `No se detectaron los clicks en la entrada` (calibración) | Revisar el cable o el loopback, el par de salida y el canal de micrófono, y que el volumen de salida no esté en cero. |
| La toma suena corrida respecto de los demás | **Calibrar latencia** con el mismo buffer con el que grabás (la calibración se guarda por interfaz y buffer). |
| `Armá una pista propia (●R) para grabar` | Armar una pista tuya (no importada) antes de **▶ Play**. |
| `La toma es más corta que la cuenta y la latencia; no se guardó` | Se paró antes de que empezara a grabar de verdad: grabar un poco más. |
| `Sólo lectura: host desconectado` | El host cerró la app o se cortó la red. Se puede escuchar lo que ya está en caché; **Reintentar** cuando vuelva. |
| `Operación rechazada: not allowed to edit this track` | Es una pista de otro integrante; sólo el autor y el host pueden editarla. |
| `este proyecto pertenece a otro anfitrión` | Ese proyecto lo creó otra instalación (otro id de usuario); abrilo desde la PC de su host o unite con su link. |
| Nadie puede unirse desde afuera | Usar Tailscale o abrir el puerto 22140 TCP en el router del host (ver "Hostear"). |
| No se oye el chat de voz | Llega por UDP 22140: permitirlo en el firewall/antivirus del host y de cada integrante (el resto anda igual por TCP). Revisar también que nadie tenga **Silenciar mi micrófono** activado ni esté en **M** en tus "Participantes". |
| Eco en el chat, o el chat se oye en mi toma | Usar auriculares: con parlantes el micrófono capta lo que suena. |
| `Descargas` no baja de 1 | Las tomas se bajan del host: si está desconectado, esperan a que vuelva. |
| `Falta tools\yt-dlp.exe` | Falta la carpeta `tools` junto al exe (está en el zip del release; o correr `tools\fetch-tools.ps1`). |
| `yt-dlp falló` con "JS challenge" / pocos formatos | Falta `tools\deno.exe` o yt-dlp está viejo: bajar los dos de nuevo con `tools\fetch-tools.ps1`. |
| `La configuración estaba dañada…` | Se guardó una copia `settings.json.bad`; revisar el dispositivo y la calibración en el lobby. |

## Licencia

AGPL-3.0 (JUCE se usa bajo su licencia AGPLv3). Incluye el SDK ASIO de Steinberg distribuido con
JUCE (GPLv3), el SDK VST3 (MIT), libopus (BSD) y FLAC (BSD). ASIO y VST son marcas de Steinberg
Media Technologies GmbH.
