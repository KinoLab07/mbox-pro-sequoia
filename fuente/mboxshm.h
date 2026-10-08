// mboxshm.h - el contrato entre las dos piezas.
//
// El plugin del HAL vive DENTRO de coreaudiod, que corre como el usuario
// _coreaudiod y esta en una caja de arena. Su perfil,
// /System/Library/Sandbox/Profiles/com.apple.audio.coreaudiod.sb, permite abrir
// solo cinco clases de IOKit y NINGUNA es de FireWire:
//
//   (allow iokit-open
//       (iokit-user-client-class "IOAudioControlUserClient")
//       (iokit-user-client-class "IOAudioEngineUserClient")
//       (iokit-user-client-class "IOAudio2DeviceUserClient")
//       (iokit-user-client-class "RootDomainUserClient")
//       (iokit-user-client-class "IOUserUserClient"))
//
// asi que el plugin NO puede hablar con la Mbox por mucho que se firme. Pero el
// mismo perfil trae `(allow ipc-posix-shm)` sin ninguna restriccion.
//
// De ahi el reparto: un proceso ayudante que corre como el usuario normal posee
// el FireWire y hace el isocrono, y este bloque de memoria compartida es el
// unico punto de contacto.

#ifndef MBOXSHM_H
#define MBOXSHM_H

#include <stdint.h>

#define MBOX_SHM_NOMBRE   "/mbox-pro-v1"
#define MBOX_SHM_DIAG     "/mbox-hal-diag"   // lo crea el plugin, no el ayudante
#define MBOX_MAGIA        0x4D424F58u      // 'MBOX'
#define MBOX_VERSION      3u   // 3: anade las latencias
#define MBOX_CANALES      8
#define MBOX_TASA         48000
#define MBOX_ANILLO       32768            // marcos, potencia de dos (0,68 s)
#define MBOX_MASCARA      (MBOX_ANILLO - 1)
#define MBOX_PERIODO      2048             // marcos por marca de tiempo del HAL

typedef struct {
    uint32_t magia, version, canales, tasa, marcosAnillo, periodo;

    // Latido del ayudante: sube sin parar mientras transmite y recibe. El
    // plugin lo mira para saber si hay alguien al otro lado.
    volatile uint64_t latido;

    // Ancla de reloj. El ayudante esta esclavizado al reloj del aparato, que es
    // el que manda, asi que publica cuantos marcos ha capturado y en que
    // instante de mach_absolute_time. El HAL construye su reloj con esto.
    volatile uint64_t marcosCapturados;
    volatile uint64_t anclaHost;

    // Cuantos marcos ha consumido el ayudante del anillo de reproduccion.
    volatile uint64_t marcosReproducidos;

    // Contadores de diagnostico.
    volatile uint64_t vacios;      // veces que el anillo de reproduccion estaba vacio
    volatile uint64_t perdidos;    // marcos de captura pisados sin leer

    uint32_t desplCaptura;         // desplazamiento en bytes de cada anillo
    uint32_t desplRepro;

    // Los escribe SOLO el plugin. Sirven para ver desde fuera si corre y que
    // partes de su ciclo de vida se ejecutan, sin depender del registro del
    // sistema, que dentro de coreaudiod no ha dado la cara.
    volatile uint64_t pluginLatido;      // sube en GetZeroTimeStamp
    volatile uint64_t pluginArranques;   // llamadas a StartIO
    volatile uint64_t pluginEscrituras;  // llamadas a WriteMix
    volatile uint64_t pluginLecturas;    // llamadas a ReadInput

    // Latencias en marcos, las publica el ayudante y el plugin se las dice a
    // CoreAudio para que programas como REAPER alineen lo que graban.
    volatile uint32_t latSalida;   // de la linea de tiempo a la salida analogica
    volatile uint32_t latEntrada;  // de la entrada analogica a la linea de tiempo
} MboxCab;

// Los dos anillos son float32 entrelazados, MBOX_CANALES por marco.
#define MBOX_BYTES_ANILLO ((size_t)MBOX_ANILLO * MBOX_CANALES * sizeof(float))
#define MBOX_DESPL_CAPTURA ((uint32_t)((sizeof(MboxCab) + 4095) / 4096 * 4096))
#define MBOX_DESPL_REPRO   ((uint32_t)(MBOX_DESPL_CAPTURA + MBOX_BYTES_ANILLO))
#define MBOX_BYTES_TOTAL   ((size_t)MBOX_DESPL_REPRO + MBOX_BYTES_ANILLO)

#endif
