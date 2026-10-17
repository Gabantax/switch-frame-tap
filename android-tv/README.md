# Switch Frame Tap TV — preview 0.1.1

Viewer para **Android TV / Google TV desde Android 6.0 (API 23)**. Recibe vídeo
H.264 y audio PCM de Switch Frame Tap directamente de la consola, por red local
o USB. Solo reproduce: el mando de juego sigue conectado a la Switch.

Es una primera versión para pruebas. Se ha compilado y comprobado el protocolo
con una captura real del repositorio. **Todavía no se ha validado la reproducción
en un televisor físico, ni medido la latencia real en Android.**

## Instalar y conectar

1. Copia `SwitchFrameTapTV-0.1.1-preview.apk` al televisor o TV Box y ábrela
   desde un gestor de archivos que permita instalar APK. Autoriza la instalación
   desde ese gestor cuando Android lo solicite. Aparece como **Switch Frame Tap TV**
   en el launcher de TV. La APK de prueba está firmada con una clave de desarrollo.
2. En la Switch, usa el manager de Switch Frame Tap para elegir la conexión.
   Reinicia la consola si lo solicita. Mantén el streamer instalado y activo.
3. **Red:** elige Network en la Switch y Red en la TV. Ambos equipos deben estar
   en la misma red. Deja la IP vacía para buscar por el anuncio UDP y la última
   dirección utilizada, o introduce la IP que muestra el manager de la consola.
   Pulsa **Conectar**. El stream usa TCP 9950 y el descubrimiento UDP 9951.
   Si el router filtra anuncios, usa la IP manual; la app no escanea la subred.
4. **USB:** elige USB viewer en la Switch y USB en la TV. Conecta el USB-C de la
   Switch directamente a un puerto USB host del televisor con un cable de datos.
   Pulsa **Conectar** y autoriza el acceso al USB. No uses el modo webcam/UVC.
   La Switch debe estar en modo portátil para dejar libre su USB-C.
5. Abre un juego. La app pasa a pantalla completa cuando recibe imagen reciente.
   Cierra el viewer de Windows antes: el streamer admite un único viewer.

En red la Switch puede permanecer en el dock. Cambiar entre red y USB en la APK
no cambia el modo elegido en la consola; configura también la Switch.

## Mando de la TV

- Flechas: navegar por los controles; OK: seleccionar.
- Durante el vídeo, OK o Menú abre los controles. Atrás vuelve al vídeo.
- **Cerrar panel**, arriba a la derecha, oculta los controles sin desconectar.
  Funciona también cuando no hay imagen; el panel no vuelve a abrirse solo hasta
  que pulses OK, Menú o Atrás. Así puedes ver las estadísticas completas.
- En la pantalla sin conexión, Atrás sale de la app.
- **Silenciar audio** afecta al viewer, no a la consola.
- **Audio estable** aumenta el buffer solicitado de 20 a 60 ms, sujeto al mínimo
  del dispositivo. Déjalo desactivado para priorizar menor retardo. Activarlo
  reinicia la conexión y puede ayudar si el sonido se corta.
- **Mostrar estadísticas** muestra FPS enviados a la pantalla, bitrate,
  FPS recibidos y decodificados, decodificador, descartes, audio recibido,
  avance del audio reproducido, buffer real de la TV y estado del foco de audio.

La app detiene la conexión y libera audio/USB al salir a otra aplicación o
suspenderse. Al volver, reanuda la conexión si estaba activa.

## Calidad y retardo

La app no recodifica el vídeo ni reduce su resolución. Recibe la calidad elegida
en la Switch, que puede adaptarla automáticamente a la capacidad de la red.
Para empezar, elige High y 1080p en la consola si tu configuración lo permite.
Si aparecen pausas o resincronizaciones, prueba Medium o 720p en la consola.
1080p60 depende también del juego y de la capacidad de captura de la Switch;
la app no convierte un juego de 30 fps en 60 fps.

La reproducción da prioridad a los decodificadores H.264 de hardware. En Android
11 o posterior solicita baja latencia si el decoder declara soporte. Si las
opciones optimizadas fallan, intenta una configuración básica y otros decoders,
incluido software como último recurso; puede aumentar el retardo.

No se acumula una cola ilimitada para mantener la imagen fluida. Se muestra la
salida decodificada más reciente y se recupera desde un IDR si se pierden referencias o se
satura la cola de entrada. Esto conserva los píxeles del vídeo mostrado, pero
en un dispositivo o enlace insuficiente puede haber saltos o pausas hasta el
siguiente fotograma clave. No se promete simultáneamente calidad máxima y una
latencia fija en cualquier televisor.

La preview 0.1.1 corrige un reinicio repetido del decoder cuando su primer buffer
tarda en estar disponible. Usa PTS relativos y admite drivers que modifiquen
las marcas de tiempo. Elimina el descarte fijo de todas las salidas que superen
120 ms, que podía mantener la pantalla negra en un pipeline lento. Si un decoder
acepta fotogramas pero no entrega salida, intenta otro desde un IDR.

El audio ya no se vacía al resincronizar el vídeo ni al superar un límite fijo
de 80 ms: algunos televisores necesitan un buffer nativo mayor para empezar a
reproducir. La cola del sistema queda limitada por AudioTrack, manteniendo pequeño
el buffer de la aplicación. El buffer real y el foco de audio se muestran en las
estadísticas. En Android 12 o posterior se solicita también un umbral de inicio
bajo cuando el driver lo permite.

**Las estadísticas no miden la latencia total:** «Cola + decodificación» va desde
la recepción de la cabecera hasta entregar el buffer al sistema de pantalla.
No incluye el retardo desconocido de red anterior a la llegada ni el procesamiento
del panel. «Consola» es la edad enviada por el streamer. Son medidas distintas.
El protocolo no proporciona un reloj de presentación común para audio y vídeo;
se controla la acumulación de ambos con buffers pequeños.

## Compatibilidad

Un mismo APK contiene código Java y recursos, sin bibliotecas nativas por CPU:
sirve para ARM de 32/64 bits y otros dispositivos que cumplan las APIs requeridas.
Se ha diseñado para Android TV, Google TV y Sony con Android TV, **desde Android
6**, sin una lista de modelos comprobados todavía. Sony con otro sistema operativo
o Android anterior a 6 no es compatible con esta versión.

USB es opcional y se detecta en ejecución: el firmware del televisor debe ofrecer
USB host y permitir abrir la interfaz bulk del dispositivo `1209:5f1e`, interfaz 0,
endpoint `0x81`. Un puerto para memorias USB no confirma por sí solo que esto funcione.
La conexión por red funciona independientemente de esa capacidad.

## Construir

Requisitos: JDK 17, Android SDK Platform 34 y Build Tools 34. Configura `JAVA_HOME`
y `ANDROID_HOME`, o el SDK mediante `local.properties` (no se distribuye). El wrapper
usa Gradle 8.5 y el plugin Android 8.3.0. La primera ejecución descarga dependencias.
No se usan dependencias de UI ni FFmpeg/SDL/libusb en la app Android.

Desde `android-tv/`:

```powershell
.\gradlew.bat :app:assembleDebug :app:protocolTest :app:playbackTest :app:lintDebug
python tools/package.py
```

En Linux/macOS, usa `bash gradlew` en lugar de `gradlew.bat`.
El APK generado está en `app/build/outputs/apk/debug/app-debug.apk`; el empaquetador
lo copia a `../dist/` junto con el código fuente y checksums SHA-256.

El target SDK 34 se usa para esta preview de instalación manual. No se presenta
como una publicación lista para Google Play; publicar requerirá actualizar el
target y revisar los permisos de red local de las versiones nuevas de Android.

## Probar sin la Switch

Desde la raíz del repositorio, en un PC en la misma LAN que la TV:

```powershell
python android-tv/tools/replay_server.py logs/m83-runI/stream-first10.sft --tone
```

Introduce en la TV la IP que imprime el servidor y pulsa Conectar. El servidor
repite la captura H.264 real e inserta un tono estéreo de 440 Hz. Permite comprobar
decodificación, sonido y reconexión TCP sin tocar la consola. La captura antigua
es IDR-only, por lo que no sustituye una prueba con el stream actual de IDR/P.
No mide el USB ni la latencia real de la Switch. El fixture pertenece al repositorio
original; no se incluye en el ZIP del código de Android.

Consulta [VALIDATION.md](VALIDATION.md) para lo comprobado y la prueba en TV pendiente.

## Fuentes y licencia

Derivado del protocolo y del viewer de **Tomas Uribe**,
[switch-frame-tap](https://github.com/TomasUribe/switch-frame-tap).
`LICENSE` contiene GPL-2.0 y `UPSTREAM-NOTICE` conserva los créditos originales.
El ZIP acompaña al APK con el código fuente correspondiente del viewer Android.

APIs contrastadas con documentación oficial:
[MediaCodec](https://developer.android.com/reference/android/media/MediaCodec),
[baja latencia](https://source.android.com/docs/core/media/low-latency-media),
[AudioTrack](https://developer.android.com/reference/android/media/AudioTrack),
[USB](https://developer.android.com/reference/android/hardware/usb/UsbDeviceConnection),
[Android TV](https://developer.android.com/training/tv/get-started/create).
