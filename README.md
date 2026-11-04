# Ensayo Virtual

Ensayo de banda online con baja latencia, escrito desde cero en C++20 / JUCE 8, al estilo de
Jamulus: cada músico entra con nombre y rol, usa su interfaz **ASIO** con dos entradas
(micrófono e instrumento), puede poner un **VST3** sobre el instrumento, controla el volumen de
cada integrante (voz e instrumento por separado) y de su propio monitoreo, y la banda comparte un
**backing track** (MP3/WAV/FLAC/OGG o el audio de un video de YouTube) que suena sincronizado para
todos.

```
ASIO in mic ─────► voz ─────────────┬─► monitoreo local (0 ms) ─────┐
ASIO in instr ───► VST3 ─► instr L/R ┤                             ├─► ASIO out
                                    └─► Opus (64/128 muestras) ─► UDP ─► servidor ─► mezcla por cliente ─► UDP ─┘
                                                      servidor: voces + instrumentos de los demás + backing track
```

- Un paquete UDP por bloque de audio (64 o 128 muestras a 48 kHz), Opus en modo custom, CBR.
- El servidor arma una mezcla distinta para cada cliente (con los faders de ese cliente) y lo
  excluye de su propia mezcla: uno se escucha a sí mismo localmente sin latencia.
- El backing track lo reproduce el servidor dentro de la mezcla, así todos lo escuchan alineado
  con la banda.
- Latencia medida en esta máquina entre dos procesos (bloques de 128): **≈ 25 ms** ida.

El diseño, las alternativas evaluadas (P2P, Opus estándar, resampling adaptativo, etc.) y el
plan están en [`docs/specs/2026-11-04-ensayo-virtual-design.md`](docs/specs/2026-11-04-ensayo-virtual-design.md)
y [`docs/plans/2026-11-04-ensayo-virtual.md`](docs/plans/2026-11-04-ensayo-virtual.md).

> Solo Windows 10/11 x64.

## Compilar

Requisitos: Visual Studio 2022 (o Build Tools) con "Desarrollo de escritorio con C++", CMake ≥ 3.22,
Git. JUCE (`8.0.17-security`) y libopus (`v1.6.1`) se descargan solos en la primera configuración
(o se usan desde `.deps/` si ya están clonados ahí).

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target EnsayoVirtual EnsayoServer -- /m
```

Binarios:

```
build\EnsayoVirtual_artefacts\Release\Ensayo Virtual.exe
build\EnsayoServer_artefacts\Release\EnsayoServer.exe
```

### Pruebas

```powershell
cmake --build build --config Debug --target EnsayoTests -- /m
.\build\EnsayoTests_artefacts\Debug\EnsayoTests.exe            # unitarias; exit 0 = todo OK
.\tests\integration\run-all.ps1                                # dos instancias reales, latencia, backing track, capturas
```

`run-all.ps1` levanta `EnsayoServer.exe` y dos clientes headless con dispositivo de audio falso,
mide la latencia ida con clicks (emitidos en A, detectados en la salida de B), prueba el servidor
embebido, la subida y reproducción del backing track, la pérdida de paquetes simulada, y genera
capturas de la UI (`tests/integration/out/<run>/shots/`) verificando que cada control exista.
`asio-loopback.ps1` usa hardware real: abre el driver ASIO (`-Driver`, por defecto el MiniFuse) en
una instancia cuyas salidas LOOPBACK vuelven a sus entradas y mide la ida y vuelta completa
(cliente falso → servidor → ASIO salida → loopback → ASIO entrada → servidor → cliente falso); se
salta (SKIP) si el driver no está instalado.

## Usar

### Crear una sesión

1. Abrir **Ensayo Virtual**, poner **Nombre** y **Rol** (Voz / Instrumento / Instrumento + Voz).
2. En **Audio** elegir el driver ASIO, frecuencia 48000 Hz, buffer 128 (o 64), canal de micrófono,
   canal de instrumento y par de salida. **Panel ASIO** abre el panel del driver.
3. Opcional: en **VST (instrumento)** elegir un plugin `.vst3` (amp sim, efectos), **Abrir editor**.
4. **Crear sesión**. La app levanta el servidor embebido (puerto 22140 TCP y UDP) y muestra el link
   `ensayo://<tu IP>:22140/XXXXXX`. **Copiar link** y mandarlo a la banda.
   - Para gente fuera de tu red hay que abrir/redirigir el puerto 22140 (TCP y UDP) en el router,
     y usar tu IP pública en el link.
   - Alternativa: correr `EnsayoServer.exe --port 22140` en un VPS cercano a todos y que cada uno
     se una a `ensayo://<ip del vps>:22140/NEW` (el primero crea la sesión; el link real aparece en
     la ventana).

### Unirse

Pegar el link en **Link de sesión** y **Unirse**, o hacer clic en el link (la app registra el
esquema `ensayo://` en el primer arranque; si ya está abierta, se une en la misma ventana).

### Durante el ensayo

- **Yo (monitoreo)**: faders de tu voz y tu instrumento como los escuchás vos (no afectan a los
  demás).
- Por cada integrante: fader de **Voz** y de **Instrumento** + Mute (solo los que su rol envía).
- **Backing track**: **Cargar archivo…** (WAV/MP3/FLAC/OGG) o pegar una **URL de YouTube** y
  **Descargar**. El archivo se sube al servidor y el transporte (**Play/Pausa**, **Stop**, **Loop**,
  posición) es compartido: todos escuchan lo mismo en el mismo momento. **Volumen** y **Mute** del
  track son propios.
- Barra de estado: ping, bloques del jitter buffer (cliente/servidor), latencia estimada total,
  pérdida de paquetes, CPU. **Reintentar** reabre el dispositivo de audio tras un error.

### YouTube

La app no incluye ningún descargador. Para usar la URL de un video hace falta tener instalados
[`yt-dlp`](https://github.com/yt-dlp/yt-dlp) (más [`deno`](https://deno.com) y
[`ffmpeg`](https://ffmpeg.org), que yt-dlp necesita) en el `PATH`, junto al ejecutable, o en la ruta
configurada en `settings.json` (`ytDlpPath`). La descarga se hace en tu máquina con
`yt-dlp -x --audio-format mp3 --audio-quality 0 --no-playlist <url>` y el MP3 resultante se sube al
servidor. Descargar contenido de YouTube puede ir contra sus términos de uso: es responsabilidad
de quien lo hace.

## Modos de línea de comandos

```
"Ensayo Virtual.exe" ensayo://host:puerto/ID            unirse directo
"Ensayo Virtual.exe" --demo                             ventana con sesión de demostración
"Ensayo Virtual.exe" --screenshot <dir>                 lobby.png, session.png y controls.json; sale
"Ensayo Virtual.exe" --headless --host|--join <link> [--name N --role R --fake-audio click|sine|silence
                     --block 64|128|256 --emit-log a.csv --probe b.csv --compare a.csv --track f.wav --play
                     --simulate-loss N --duration S --stats s.jsonl
                     --asio "<driver>" --mic N --inst N --out P --mute-monitor]   # driver real en vez del falso
EnsayoServer.exe [--port N] [--udp-port N] [--frame 64|128] [--log]
```

## Configuración

`%APPDATA%\EnsayoVirtual\settings.json` (dispositivo, canales, plugin y su estado, faders,
puerto, ruta de yt-dlp). Borrarlo para volver a los valores por defecto. Los archivos subidos al
servidor quedan en `%TEMP%\EnsayoVirtual\`.

## Problemas frecuentes

| Mensaje / síntoma | Qué hacer |
|---|---|
| `No se encontraron drivers ASIO` | Instalar el driver ASIO de la interfaz. |
| `Error de audio: ...` | Otro programa (DAW) usa el driver, o se desconectó la interfaz. Cerrarlo y **Reintentar**. |
| `Sin audio del servidor` | No llegan paquetes UDP: revisar firewall/puerto 22140 UDP en el host. |
| Nadie puede unirse desde afuera | Abrir el puerto 22140 TCP y UDP en el router del host, o usar `EnsayoServer.exe` en un VPS. |
| Latencia alta / cortes | Bajar el buffer ASIO a 64 o 128; usar cable, no Wi-Fi; elegir un host cercano. |
| `yt-dlp` no encontrado | Instalar yt-dlp, deno y ffmpeg y ponerlos en el PATH. |

## Licencia

AGPL-3.0 (JUCE se usa bajo su licencia AGPLv3). Incluye el SDK ASIO de Steinberg distribuido con
JUCE (GPLv3), el SDK VST3 (MIT) y libopus (BSD). ASIO y VST son marcas de Steinberg Media
Technologies GmbH.
