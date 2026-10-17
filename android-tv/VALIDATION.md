# Validación de la preview

Versión 0.1.1: pruebas de host y compilación completas; pendiente de comprobar
el resultado en el Vestel Cosmos / Android 11 del usuario.

## Comprobado en el entorno de desarrollo

- `:app:assembleDebug`: compilación e instalación potencial como APK firmada.
- `:app:protocolTest`: 40 comprobaciones; incluye diez fotogramas de una captura
  real de la Switch en `logs/m83-runI/stream-first10.sft`.
- Cabeceras little-endian, lecturas fragmentadas con timeouts intermedios,
  separación de vídeo/PCM, enteros sin signo y cancelación.
- Rechazo de cabeceras inválidas, versiones antiguas, tamaños fuera de límites,
  PCM desalineado y formatos no soportados antes de reservar el payload.
- Extracción de SPS/PPS con prefijos Annex-B de tres y cuatro bytes.
- Recuperación de referencias al iniciar, perder un paquete, cambiar de resolución,
  reiniciar sesión o saturarse/envejecer la cola. Se espera un IDR antes de decodificar
  fotogramas dependientes; se comprueba también el rollover de secuencia.
- `:app:lintDebug`: sin errores de Android Lint. Las advertencias de target SDK,
  orientación horizontal y traducción de textos son limitaciones de esta preview.
- Servidor de prueba: lecturas TCP fragmentadas, H.264 real, tono PCM estéreo
  y una segunda conexión. Se prueba en loopback sin anuncios a la LAN.
- APK: metadatos de API mínima 23, launcher de Android TV, USB opcional y firma
  verificada con las herramientas del SDK.
- `:app:playbackTest`: ejecuta los workers reales con dobles de drivers, que
  se compilan aparte y no se incluyen en la APK:
  - Primer buffer de entrada retrasado 60 ms: se configura una sola vez y aparece salida.
  - Salida retrasada 180 ms y PTS alterados: se presenta imagen en lugar de descartarla toda.
  - Hardware que acepta datos sin devolver imagen: cambia al decoder de software en un IDR.
  - Audio nativo que exige 160 ms para iniciar: reproduce y no entra en un ciclo de vaciados.

Estas comprobaciones **no prueban la reproducción física ni la latencia**.
No se han ejecutado el sysmodule ni los viewers de PC existentes.

## Primera prueba en tu TV

1. Instala la APK y comprueba que aparece en el launcher. Recorre los botones con
   las flechas del mando y verifica que el foco es visible. Comprueba **Cerrar panel**
   antes y después de recibir imagen; debe seguir cerrado hasta que lo abras con el mando.
2. Cierra el viewer de Windows. Pon Switch Frame Tap en Network y conecta la TV
   por IP manual primero. Confirma imagen, color, proporción y sonido.
3. Activa estadísticas y anota modelo de TV, versión Android, resolución, FPS,
   decoder, tiempos de cola/decodificación y resincronizaciones. Prueba un juego
   de 60 fps y otro de 30 fps.
4. Prueba 720p y 1080p, High y Medium en la consola. Comprueba que cambia de
   resolución sin imagen corrupta. Compara red cableada y Wi-Fi si están disponibles.
5. Vuelve al HOME de la Switch, cierra/reabre el juego, desconecta/reconecta la red
   y sal/vuelve a la app de TV. Debe recuperar imagen sin tener que reinstalar.
6. Cambia ambos lados a USB viewer. Conecta directamente la Switch en portátil,
   autoriza USB y comprueba vídeo/audio. Quita y vuelve a poner el cable.
7. Rechaza el permiso USB una vez: no debe entrar en un ciclo de diálogos; pulsa
   Conectar para solicitarlo otra vez. Prueba Silenciar y Audio estable.

Para comparar la latencia, graba en cámara lenta una animación o contador visible
simultáneamente en la pantalla de la Switch y en la TV. La diferencia entre ambas
imágenes aproxima el retardo añadido; las estadísticas internas no lo sustituyen.
No cambies la calidad o resolución entre las tomas que estés comparando.

Si falla, indica el paso, mensaje mostrado y foto de las estadísticas. Si tienes
ADB conectado a la TV, `adb logcat -s SFT-TV` recoge los errores de transporte,
audio y vídeo del viewer. El servidor de replay permite aislar la reproducción
Android del streamer de la consola.

Para el caso «conecta pero no hay imagen/sonido», pulsa **Cerrar panel** y mira:

- **Vídeo RX > 0, decodificado = 0:** el decoder no devuelve imágenes.
- **Decodificado > 0, mostrado > 0 pero negro:** revisar la salida Surface/driver;
  «mostrado» cuenta buffers entregados para renderizar, no una medición del panel físico.
- **Audio RX = 0:** no están llegando paquetes de sonido a la app.
- **Audio RX > 0, reproducido = 0:** la salida de audio no avanza; comprueba también
  «silenciado», «sin foco» y el buffer nativo.

Windows funcionando por red con la misma Switch confirma que existe un stream
utilizable, pero estos contadores son necesarios para localizar el punto de fallo
en el hardware Android. Los fallos corregidos se han reproducido con dobles de
drivers; no se ha confirmado aún cuál afectaba al televisor del usuario.
