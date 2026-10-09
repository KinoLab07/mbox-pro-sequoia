# Avid Mbox Pro en macOS Sequoia

Driver libre para la Mbox Pro de 3.ª generación (FireWire) en macOS hasta 15
Sequoia, **sin desactivar SIP y sin cuenta de desarrollador**. La guía completa,
para cualquier usuario, está en `doc/Mbox-Pro-Sequoia-Guia.pdf`.

## En tres líneas

```
./instalar.sh       # compila aquí e instala (pide la contraseña una vez)
./estado.sh         # diagnóstico, solo lee
./desinstalar.sh    # lo quita todo
```

**La rueda de monitor es el volumen general; si no suena, comprueba que no esté abajo del todo.**

## Qué hay

| Ruta | Qué es |
|---|---|
| `fuente/MboxProHAL.c` | plugin AudioServerPlugIn que corre dentro de `coreaudiod` |
| `fuente/mboxd.c` | ayudante: FireWire isócrono vía `IOFireWireLib`, router y mezclador DICE |
| `fuente/mboxshm.h` | contrato de memoria compartida entre los dos (versión 3) |
| `doc/guia.html` | fuente de la guía; el PDF se regenera con Brave/Chrome sin ventana |

Para regenerar el PDF:

```
"/Applications/Brave Browser.app/Contents/MacOS/Brave Browser" --headless=new \
  --no-pdf-header-footer --print-to-pdf="$PWD/doc/Mbox-Pro-Sequoia-Guia.pdf" "file://$PWD/doc/guia.html"
```

## Variables de diagnóstico de `mboxd`

Solo para depurar; el agente no usa ninguna salvo `MBOX_QUIETO=1`.

| Variable | Efecto |
|---|---|
| `MBOX_RETORNO=2` | devuelve la mezcla de monitor a la captura 1-2 (medir sin oídos) |
| `MBOX_MEDIDOR=1` | lee la sección de picos y la de aplicación (frena el bucle) |
| `MBOX_RUEDA=1/2` | vigila cambios en registros / la rueda |
| `MBOX_APP=modo,uitarget,input,led` | fuerza la sección de aplicación |
| `SIGUSR1` | vuelca 3 s de lo enviado a `/tmp/mbox-enviado.raw` |

La historia técnica (qué se probó, qué falló y por qué) está en
`~/Desktop/MBOX/3-DOCUMENTACION/SEQUOIA-ESPACIO-USUARIO.md`.
