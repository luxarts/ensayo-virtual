# Chat de voz, transporte simplificado y vúmetros por pista

Ampliación de `2026-11-13-grabacion-multipista-design.md` a partir de la primera prueba de uso. Todo lo no mencionado acá sigue igual.

## 1. Transporte

- Un solo botón **Play/Pausa** (barra espaciadora). Se eliminan los botones **Stop** y **Grabar**.
- Pausa deja el cursor donde está; Play continúa desde ahí.
- Con una pista propia armada, **Play graba**: aplica la cuenta previa si está activa, y la toma empieza en el cursor. Pausa (o la barra espaciadora) termina la toma y deja de reproducir.
- Atajos (solo con el transporte libre, nunca durante una toma, para no romper la alineación):
  - **W**: el cursor vuelve al inicio (0).
  - **←/→**: mueven el cursor 1 s.
  - **Shift+←/→**: mueven el cursor un compás (`beatsPerBar` pulsos al `bpm` del proyecto).
- Los atajos existentes (S cortar, Supr borrar) siguen. Se elimina R.

## 2. Pistas

- Cada encabezado de pista tiene un botón **×** para borrar la pista (`delete_track`). Pide confirmación: "¿Borrar la pista «<nombre>»? Sus regiones desaparecen del proyecto." Lo pueden usar el autor de la pista y el host; para los demás está deshabilitado con tooltip. No se puede borrar una pista mientras se graba en ella.
- **Vúmetro por pista**, vertical y angosto, junto al encabezado:
  - en la pista armada propia muestra la entrada en vivo (post ganancia de envío);
  - en las demás muestra el nivel de la pista en la reproducción (post volumen, pan y mute, pre master).
- Muestra el pico como número en dBFS (retención de 1,5 s; "-∞" en silencio).
- **Testigo de clip**: el segmento superior se prende en rojo cuando una muestra llega a ≥ 0 dBFS (|x| ≥ 1,0) y queda prendido hasta que el usuario hace clic sobre el vúmetro. El testigo es local de cada usuario.

## 3. Monitoreo y volúmenes propios

- El monitor de las entradas propias suena **solo si hay una pista propia armada**, y solo la entrada que corresponde a ese tipo de pista: micrófono (mono, centrado) para voz, salida del VST del instrumento (estéreo) para instrumento. Sin pista armada no hay monitor.
- Panel **"Mi audio"** en la vista de sesión, con cuatro faders en dB (-60 a +6 dB, doble clic = 0 dB) y su valor:
  - **Voz – monitor** y **Instrumento – monitor**: lo que escuchás de vos mismo.
  - **Voz – envío** y **Instrumento – envío**: ganancia aplicada a la entrada antes de grabar y antes del chat de voz. Esta ganancia queda impresa en la toma.
- Los cuatro valores se guardan en la configuración local.

## 4. Chat de voz siempre activo

- Se elimina el check "Escuchar en vivo".
- Mientras estás en una sesión, el micrófono propio (post ganancia de envío de voz) se codifica continuamente en Opus (frames de 128 muestras, mono) y se envía por el relay UDP del host. Así todos se escuchan en todo momento, como un chat de voz.
- Mientras grabás una pista de instrumento, se envía además el instrumento como un segundo stream (estéreo) del mismo emisor, para que los demás escuchen la toma en vivo. Al grabar una pista de voz alcanza con el stream de voz. El antiguo stream de preview de la toma desaparece.
- Botón **"Silenciar mi micrófono"** (toggle) en "Mi audio": deja de enviar la voz al chat. No afecta la grabación ni el monitor.
- **Recepción por emisor**: cada cliente mantiene un jitter buffer y decodificadores propios por emisor y por stream (voz, instrumento). La identidad del emisor sale del `clientId` del paquete, mapeado al usuario por la presencia que envía el host. Los streams se mezclan en la salida con fundido de entrada/salida para no producir clics al empezar, al cortarse o al ocultar pérdidas.
- **Mixer "Participantes"** (personal, no pasa por el server): una fila por integrante en línea, salvo vos, con nombre, fader de volumen (-60 a +12 dB) y mute. Se aplica al chat de voz y al instrumento en vivo de ese integrante. Se guarda por id de usuario en la configuración local. No afecta la mezcla del proyecto.
- El chat no se graba ni se guarda. La grabación sigue igual: segmentos FLAC sin pérdida, con alineación exacta.
- Se asume el uso de auriculares: sin ellos, la voz de los demás se cuela en el micrófono durante una toma.

## 5. Protocolo

- El paquete de audio UDP indica el tipo de stream (voz o instrumento) con los bits `voice`/`instrument` de `flags` del encabezado existente. El host lo reenvía sin cambios a todos los demás conectados que tengan una dirección UDP conocida, sin la condición previa de "escuchando" (el mensaje `listen` se acepta y se ignora).
- Cada conexión recibe en su propio `welcome` un secreto UDP privado (`udpSecret`, 8 bytes en 16 caracteres hex). El keepalive pasa a tener 13 bytes: `0x00`, `clientId` u32 little-endian y el secreto. El host aprende o actualiza la dirección UDP de una conexión solo con un keepalive que trae su secreto, y reenvía un paquete de audio solo si llega desde esa dirección. Como el `clientId` es público (va en la presencia), sin el secreto nadie puede desviar ni suplantar el audio de otro.
- La presencia que difunde el host incluye el `clientId` de cada conexión en línea para que los clientes mapeen emisor → usuario.

## 6. Pruebas

- Unitarias: per-track peaks y testigo de clip; monitor solo con pista armada; ganancias de envío aplicadas a la toma; recepción de dos emisores simultáneos sin mezclar sus secuencias; volumen y mute por participante; sin escalones al empezar o cortar un stream.
- Transporte: Play con pista armada graba, Pausa termina la toma; W y flechas mueven el cursor y no hacen nada durante una toma; borrar pista con permisos.
- Integración: dos clientes headless intercambian voz continuamente sin grabar (cada uno recibe el stream del otro); los scripts existentes siguen pasando.
- Capturas: `expected-controls.json` actualizado (Play/Pausa, vúmetros, ×, "Mi audio", "Participantes"; sin Stop, Grabar ni "Escuchar en vivo").
