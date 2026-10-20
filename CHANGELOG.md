# Cambios

## 1.2.0

### Novedades

- **Escalar imágenes** (rueda de herramientas de imagen, botón **ESCALAR**). Abre el editor en la nueva pestaña
  **Tamaño**, que agranda o reduce la imagen a mano arrastrando los tiradores del marco: las esquinas escalan
  manteniendo la proporción y los lados estiran o encogen solo el ancho o el alto. También tiene un deslizador
  (10 % a 400 %) y porcentajes fijos (25, 50, 75, 150, 200 y 400 %), y muestra el tamaño final en píxeles. Se combina con el resto del editor: si hay recorte,
  se escala la zona recortada.
- **Convertir SVG a imagen.** Los archivos `.svg` se tratan como imágenes: se pueden pasar a JPG, PNG, WEBP, HEIC,
  TIFF, AVIF, BMP, PDF y DOCX y usar con las herramientas de imagen. Se dibujan con Direct2D a 2048 px por el lado
  largo (hasta 8192 si el SVG es mayor). El texto (`<text>`/`<tspan>`), que Direct2D descarta, se dibuja aparte
  con DirectWrite respetando posición, transformaciones, color, fuente y alineación; las fuentes de Linux habituales
  (Liberation, DejaVu) se sustituyen por sus equivalentes de Windows.

## 1.1.0

### Novedades

- **Enviar a otro dispositivo** (rueda de herramientas, cualquier archivo o carpeta). Abre una ventana que comparte
  los archivos mientras está abierta:
  - **Enlace y código QR:** FileTools levanta un pequeño servidor web en el PC. Escanea el QR con la cámara del
    móvil o abre el enlace en cualquier dispositivo de la misma red (Wi-Fi o cable) y descarga los archivos desde
    una página con vista previa de las imágenes. El enlace lleva una clave aleatoria y deja de funcionar al pulsar
    **Detener**. Las carpetas se envían como ZIP, se ve el progreso de cada descarga y **Cambiar de red** elige la
    dirección si el PC tiene varias.
  - **Bluetooth…** abre el asistente de Windows con los archivos ya elegidos.
  - **Compartir…** abre el panel Compartir de Windows (Uso compartido cercano).
- **Ventana de ajustes.** Se abre con **Ctrl+Alt+S**, con un clic en el icono de la bandeja o volviendo a abrir
  `FileTools.exe`. Reúne dónde guardar los resultados, la calidad, iniciar con Windows, los avisos, el estado de
  ffmpeg y ExifTool (con **Elegir…** para indicar `exiftool.exe`), los atajos de teclado, **Cerrar tarjetas
  flotantes** y **Salir de FileTools**. Los cambios se guardan al momento.
- **Atajos de teclado configurables.** El de la rueda (Ctrl+Alt+Espacio) y el de los ajustes (Ctrl+Alt+S) se pueden
  cambiar: clic en el atajo y pulsar la nueva combinación. Si otra aplicación ya la usa, se mantiene la anterior, y
  **Restablecer** vuelve al atajo por defecto.
- **Calidad 100 %, ahora por defecto.** Al convertir, WEBP y AVIF se guardan sin pérdida (píxeles idénticos) y JPG y
  HEIC a calidad máxima (JPG sin submuestreo de color). Quien ya tenía otra calidad elegida la conserva.
- **Avisos opcionales:** se puede desactivar la notificación al terminar una tarea en segundo plano (los errores se
  siguen avisando).
- Línea de comandos: `--send <archivos…>` abre la ventana de enviar y `--settings` abre los ajustes.

### Mejoras

- Texto más suave y legible, letra algo más grande, bordes nítidos y curvas más finas en las ventanas de ajustes,
  enviar e información.
- Las píldoras azules (opción elegida, botones principales) ya no muestran una línea en el medio.
- El menú de la bandeja empieza por **Ajustes…** y muestra el atajo elegido; el clic izquierdo en el icono abre los
  ajustes.

### Correcciones

- Al abrir un AVIF con transparencia (por ejemplo, para convertirlo a PNG) se perdía el fondo transparente.

## 1.0.0

Primera versión.
