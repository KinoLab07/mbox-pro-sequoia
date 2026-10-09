// MboxProHAL.c - AudioServerPlugin que publica el Avid Mbox Pro ante CoreAudio.
//
// Corre DENTRO de coreaudiod, que esta en caja de arena y no puede abrir
// FireWire (ver mboxshm.h). Asi que esta pieza no toca el hardware: lee y
// escribe los dos anillos de memoria compartida que mantiene el ayudante.
//
// Si el ayudante no esta corriendo el aparato aparece igual, con la entrada en
// silencio y la salida descartada; eso permite probar la mitad de CoreAudio por
// separado de la mitad de FireWire.
//
// Compilar: ./construir.sh

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stddef.h>
#include <os/log.h>
#include <errno.h>
#include "mboxshm.h"

// os_log_error se conserva siempre; el nivel normal se descarta y por eso
// el registro salia vacio aunque el plugin si estuviera cargado.
#define REG(...) os_log_error(OS_LOG_DEFAULT, "MboxProHAL: " __VA_ARGS__)

enum { kObjCaja = kAudioObjectPlugInObject, kObjAparato = 2, kObjEntrada = 3, kObjSalida = 4 };

static pthread_mutex_t gCerrojo = PTHREAD_MUTEX_INITIALIZER;
static UInt32   gCuentaRef    = 0;
static AudioServerPlugInHostRef gAnfitrion = NULL;
static UInt32   gCuentaES     = 0;        // clientes con la ES arrancada
static Float64  gTasa         = MBOX_TASA;

// --- memoria compartida con el ayudante --------------------------------------
static MboxCab *gCab = NULL;
static float   *gAnCaptura = NULL, *gAnRepro = NULL;
static double   gTicksPorMarco = 0;
static UInt64   gTicksPeriodo  = 0;

// Bloque propio del plugin: se crea aunque el del ayudante no exista, y asi se
// puede saber desde fuera si este codigo llega a ejecutarse.
static volatile uint64_t *gDiag = NULL;
static void AbrirDiag(void) {
    if (gDiag) return;
    static int primera = 1;
    if (primera) { primera = 0; shm_unlink(MBOX_SHM_DIAG); }   // instancia nueva, cuentas nuevas
    int fd = shm_open(MBOX_SHM_DIAG, O_CREAT | O_RDWR, 0666);
    if (fd < 0) return;
    ftruncate(fd, 4096);          // si ya existia falla, y da igual: ya tiene tamano
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p != MAP_FAILED) gDiag = (volatile uint64_t *)p;
    close(fd);
}
#define DIAG(i) do { if (gDiag) gDiag[i]++; } while (0)
// 0 Initialize  1 StartIO  2 GetZeroTimeStamp  3 WriteMix  4 ReadInput
// 5 intentos de abrir la del ayudante  6 fallos  7 errno del ultimo fallo

static void AbrirCompartida(void) {
    if (gCab) return;
    AbrirDiag(); DIAG(5);
    int fd = shm_open(MBOX_SHM_NOMBRE, O_RDWR, 0666);
    if (fd < 0) { DIAG(6); if (gDiag) gDiag[7] = (uint64_t)errno; REG("shm_open fallo"); return; }
    void *p = mmap(NULL, MBOX_BYTES_TOTAL, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { REG("mmap de la compartida fallo"); return; }
    MboxCab *c = (MboxCab *)p;
    if (c->magia != MBOX_MAGIA || c->version != MBOX_VERSION) {
        REG("la compartida no cuadra (magia 0x%X version %u)", c->magia, c->version);
        munmap(p, MBOX_BYTES_TOTAL); return;
    }
    gCab = c;
    gAnCaptura = (float *)((char *)p + MBOX_DESPL_CAPTURA);
    gAnRepro   = (float *)((char *)p + MBOX_DESPL_REPRO);
    REG("ayudante encontrado: %u canales a %u Hz", c->canales, c->tasa);
}

static UInt64 gOrigen = 0;      // marcos del ayudante que valen "muestra 0"
static UInt64 gSemilla = 1;
static int    gAnclado = 0;

static void CerrarCompartida(void) {
    if (!gCab) return;
    munmap((void *)gCab, MBOX_BYTES_TOTAL);
    gCab = NULL; gAnCaptura = NULL; gAnRepro = NULL;
    // La linea de tiempo se corta: que CoreAudio lo sepa al reconectar.
    if (gAnclado) { gAnclado = 0; ++gSemilla; }
}

// El ayudante casi siempre arranca DESPUES que coreaudiod, y si la Mbox es la
// salida de sistema la ES no se para nunca, asi que StartIO no vuelve a pasar.
// Hay que reintentar por nuestra cuenta. Y si el ayudante se reinicia, crea otro
// objeto de memoria: la magia a cero avisa y hay que volver a mapear.
static void ReintentarCompartida(void) {
    static UInt64 ultimo = 0;
    UInt64 ahora = mach_absolute_time();
    if (gCab && gCab->magia != MBOX_MAGIA) { REG("el ayudante se fue"); CerrarCompartida(); }
    // Si el ayudante lleva 2 s sin dar senales de vida, soltar el segmento y
    // volver a buscar: puede haberse relanzado con uno nuevo y este estar
    // muerto aunque su magia siga valida.
    {
        static UInt64 vivoUltimo = 0;
        if (gCab) {
            UInt64 a = gCab->anclaHost;
            if (a && ahora > a && (double)(ahora - a) < 0.5 * MBOX_TASA * gTicksPorMarco) vivoUltimo = ahora;
            else if (!vivoUltimo) vivoUltimo = ahora;
            else if ((double)(ahora - vivoUltimo) > 2.0 * MBOX_TASA * gTicksPorMarco) {
                REG("el ayudante no da senales; vuelvo a buscarlo"); CerrarCompartida(); vivoUltimo = 0;
            }
        }
    }
    if (gCab) return;
    if (ultimo && (double)(ahora - ultimo) < 1.0 * MBOX_TASA * gTicksPorMarco) return;
    ultimo = ahora;
    AbrirCompartida();
}

// El ayudante esta vivo si su ancla de reloj es reciente.
static int AyudanteVivo(void) {
    if (!gCab) return 0;
    UInt64 a = gCab->anclaHost;
    if (!a) return 0;
    UInt64 ahora = mach_absolute_time();
    return (ahora > a) && ((double)(ahora - a) < 0.5 * MBOX_TASA * gTicksPorMarco);
}

// --- reloj --------------------------------------------------------------------
static UInt64  gAnclaHost = 0;
static Float64 gAnclaMuestra = 0;

// CoreAudio exige una linea temporal CONTINUA, y avisa de los cortes con la
// semilla: si cambia, vuelve a tomar referencias; si no cambia, da por hecho que
// todo sigue igual. El contador del ayudante lleva corriendo desde que arranco,
// asi que hay que restarle un origen para empezar cerca de cero, y hay que subir
// la semilla cada vez que ese origen se mueve (al enganchar, al reiniciarse el
// ayudante, al pasar del reloj propio al suyo).
// (gOrigen, gSemilla y gAnclado estan declarados mas arriba)

static void PrepararTiempo(void) {
    mach_timebase_info_data_t t; mach_timebase_info(&t);
    // ticks de host por marco a 48 kHz
    gTicksPorMarco = (1.0e9 / (double)MBOX_TASA) * (double)t.denom / (double)t.numer;
    gTicksPeriodo  = (UInt64)(gTicksPorMarco * MBOX_PERIODO);
}

// --- formato ------------------------------------------------------------------
static void LlenarFormato(AudioStreamBasicDescription *f) {
    memset(f, 0, sizeof *f);
    f->mSampleRate       = gTasa;
    f->mFormatID         = kAudioFormatLinearPCM;
    f->mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f->mBytesPerPacket   = 4 * MBOX_CANALES;
    f->mFramesPerPacket  = 1;
    f->mBytesPerFrame    = 4 * MBOX_CANALES;
    f->mChannelsPerFrame = MBOX_CANALES;
    f->mBitsPerChannel   = 32;
}

// --- IUnknown -----------------------------------------------------------------
static AudioServerPlugInDriverInterface gInterfaz;
static AudioServerPlugInDriverInterface *gInterfazPtr = &gInterfaz;
static AudioServerPlugInDriverRef gDriver = &gInterfazPtr;

static HRESULT Consultar(void *d, REFIID iid, LPVOID *sal) {
    (void)d;
    CFUUIDRef u = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(u, IUnknownUUID) || CFEqual(u, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gCerrojo); ++gCuentaRef; pthread_mutex_unlock(&gCerrojo);
        *sal = gDriver; r = S_OK;
    } else *sal = NULL;
    CFRelease(u);
    return r;
}
static ULONG Retener(void *d) { (void)d;
    pthread_mutex_lock(&gCerrojo); ULONG n = ++gCuentaRef; pthread_mutex_unlock(&gCerrojo); return n; }
static ULONG Soltar(void *d) { (void)d;
    pthread_mutex_lock(&gCerrojo); ULONG n = gCuentaRef ? --gCuentaRef : 0; pthread_mutex_unlock(&gCerrojo); return n; }

// --- basicos ------------------------------------------------------------------
static OSStatus Iniciar(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef h) {
    (void)d; gAnfitrion = h;
    PrepararTiempo();
    AbrirDiag(); DIAG(0);
    AbrirCompartida();
    REG("iniciado; ayudante %s", AyudanteVivo() ? "vivo" : "ausente");
    return 0;
}
static OSStatus CrearAparato(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
                             const AudioServerPlugInClientInfo *ci, AudioObjectID *sal) {
    (void)d; (void)desc; (void)ci; (void)sal; return kAudioHardwareUnsupportedOperationError; }
static OSStatus DestruirAparato(AudioServerPlugInDriverRef d, AudioObjectID o) {
    (void)d; (void)o; return kAudioHardwareUnsupportedOperationError; }
static OSStatus AnadirCliente(AudioServerPlugInDriverRef d, AudioObjectID o,
                              const AudioServerPlugInClientInfo *ci) { (void)d; (void)o; (void)ci; return 0; }
static OSStatus QuitarCliente(AudioServerPlugInDriverRef d, AudioObjectID o,
                              const AudioServerPlugInClientInfo *ci) { (void)d; (void)o; (void)ci; return 0; }
static OSStatus CambioConfig(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 c, void *i) {
    (void)d; (void)o; (void)c; (void)i; return 0; }
static OSStatus AbortarCambio(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 c, void *i) {
    (void)d; (void)o; (void)c; (void)i; return 0; }

// --- propiedades --------------------------------------------------------------
static Boolean TieneProp(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t c,
                         const AudioObjectPropertyAddress *a) {
    (void)d; (void)c;
    UInt32 t = 0; OSStatus e;
    extern OSStatus MboxGetPropSize(AudioObjectID, const AudioObjectPropertyAddress *, UInt32 *);
    e = MboxGetPropSize(o, a, &t);
    return e == 0;
}
static OSStatus EsAjustable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t c,
                            const AudioObjectPropertyAddress *a, Boolean *sal) {
    (void)d; (void)c; (void)o; (void)a; *sal = false; return 0; }

OSStatus MboxGetPropSize(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 *sal) {
    switch (o) {
    case kObjCaja:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:                     *sal = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyManufacturer:
        case kAudioPlugInPropertyResourceBundle:            *sal = sizeof(CFStringRef); return 0;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:                *sal = sizeof(AudioObjectID); return 0;
        case kAudioPlugInPropertyTranslateUIDToDevice:      *sal = sizeof(AudioObjectID); return 0;
        }
        break;
    case kObjAparato:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *sal = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner:                     *sal = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyName: case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID: case kAudioDevicePropertyModelUID: *sal = sizeof(CFStringRef); return 0;
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioObjectPropertyIdentify:
        case kAudioDevicePropertyIsHidden:                  *sal = sizeof(UInt32); return 0;
        case kAudioDevicePropertyNominalSampleRate:         *sal = sizeof(Float64); return 0;
        case kAudioDevicePropertyAvailableNominalSampleRates: *sal = sizeof(AudioValueRange); return 0;
        case kAudioDevicePropertyPreferredChannelsForStereo: *sal = 2 * sizeof(UInt32); return 0;
        case kAudioDevicePropertyPreferredChannelLayout:
            *sal = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions)
                            + MBOX_CANALES * sizeof(AudioChannelDescription)); return 0;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
            if (a->mScope == kAudioObjectPropertyScopeInput || a->mScope == kAudioObjectPropertyScopeOutput)
                *sal = sizeof(AudioObjectID);
            else *sal = 2 * sizeof(AudioObjectID);
            return 0;
        case kAudioObjectPropertyControlList:               *sal = 0; return 0;
        case kAudioDevicePropertyRelatedDevices:            *sal = sizeof(AudioObjectID); return 0;
        }
        break;
    case kObjEntrada: case kObjSalida:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: case kAudioObjectPropertyClass: *sal = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner:                     *sal = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyOwnedObjects:              *sal = 0; return 0;
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:                   *sal = sizeof(UInt32); return 0;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:            *sal = sizeof(AudioStreamBasicDescription); return 0;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:  *sal = sizeof(AudioStreamRangedDescription); return 0;
        }
        break;
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus TamanoProp(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t c,
                           const AudioObjectPropertyAddress *a, UInt32 nq, const void *q, UInt32 *sal) {
    (void)d; (void)c; (void)nq; (void)q;
    return MboxGetPropSize(o, a, sal);
}

static OSStatus LeerProp(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t c,
                         const AudioObjectPropertyAddress *a, UInt32 nq, const void *q,
                         UInt32 cap, UInt32 *usado, void *dat) {
    (void)d; (void)c; (void)nq;
    #define PONE(tipo, valor) do { if (cap < sizeof(tipo)) return kAudioHardwareBadPropertySizeError; \
        *(tipo *)dat = (valor); *usado = sizeof(tipo); return 0; } while (0)

    switch (o) {
    case kObjCaja:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PONE(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:     PONE(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner:     PONE(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: PONE(CFStringRef, CFSTR("Avid"));
        case kAudioPlugInPropertyResourceBundle: PONE(CFStringRef, CFSTR(""));
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: PONE(AudioObjectID, kObjAparato);
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef u = q ? *(CFStringRef *)q : NULL;
            PONE(AudioObjectID, (u && CFEqual(u, CFSTR("MboxProFireWire:0"))) ? kObjAparato : kAudioObjectUnknown);
        }
        }
        break;
    case kObjAparato:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PONE(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:     PONE(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner:     PONE(AudioObjectID, kObjCaja);
        case kAudioObjectPropertyName:      PONE(CFStringRef, CFSTR("Mbox Pro (FireWire)"));
        case kAudioObjectPropertyManufacturer: PONE(CFStringRef, CFSTR("Avid"));
        case kAudioDevicePropertyDeviceUID: PONE(CFStringRef, CFSTR("MboxProFireWire:0"));
        case kAudioDevicePropertyModelUID:  PONE(CFStringRef, CFSTR("MboxPro3rdGen"));
        case kAudioDevicePropertyTransportType: PONE(UInt32, kAudioDeviceTransportTypeFireWire);
        case kAudioDevicePropertyClockDomain:   PONE(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: PONE(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: PONE(UInt32, gCuentaES > 0 ? 1 : 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: PONE(UInt32, 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PONE(UInt32, 1);
        case kAudioDevicePropertyLatency: {
            // Latencia de verdad, medida por el ayudante (antes se decia 0 y
            // REAPER no podia alinear lo grabado).
            int entrada = (a->mScope == kAudioObjectPropertyScopeInput);
            UInt32 l = entrada ? 64 : 512;
            if (gCab) l = entrada ? gCab->latEntrada : gCab->latSalida;
            PONE(UInt32, l);
        }
        case kAudioDevicePropertySafetyOffset:  PONE(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: PONE(UInt32, MBOX_PERIODO);
        case kAudioDevicePropertyIsHidden:      PONE(UInt32, 0);
        case kAudioObjectPropertyIdentify:      PONE(UInt32, 0);
        case kAudioDevicePropertyNominalSampleRate: PONE(Float64, gTasa);
        case kAudioDevicePropertyRelatedDevices: PONE(AudioObjectID, kObjAparato);
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            if (cap < sizeof(AudioValueRange)) return kAudioHardwareBadPropertySizeError;
            AudioValueRange r = { MBOX_TASA, MBOX_TASA };
            *(AudioValueRange *)dat = r; *usado = sizeof r; return 0;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (cap < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)dat)[0] = 1; ((UInt32 *)dat)[1] = 2; *usado = 2 * sizeof(UInt32); return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            UInt32 hace = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions)
                                   + MBOX_CANALES * sizeof(AudioChannelDescription));
            if (cap < hace) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *l = (AudioChannelLayout *)dat;
            memset(l, 0, hace);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = MBOX_CANALES;
            for (UInt32 i = 0; i < MBOX_CANALES; i++) {
                l->mChannelDescriptions[i].mChannelLabel =
                    (i == 0) ? kAudioChannelLabel_Left :
                    (i == 1) ? kAudioChannelLabel_Right :
                               (kAudioChannelLabel_Discrete_0 + i);
            }
            *usado = hace; return 0;
        }
        case kAudioObjectPropertyControlList: *usado = 0; return 0;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams: {
            AudioObjectID *v = (AudioObjectID *)dat; UInt32 n = 0;
            if (a->mScope == kAudioObjectPropertyScopeInput) {
                if (cap >= sizeof(AudioObjectID)) v[n++] = kObjEntrada;
            } else if (a->mScope == kAudioObjectPropertyScopeOutput) {
                if (cap >= sizeof(AudioObjectID)) v[n++] = kObjSalida;
            } else {
                if (cap >= sizeof(AudioObjectID)) v[n++] = kObjEntrada;
                if (cap >= 2 * sizeof(AudioObjectID)) v[n++] = kObjSalida;
            }
            *usado = n * sizeof(AudioObjectID); return 0;
        }
        }
        break;
    case kObjEntrada: case kObjSalida:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PONE(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:     PONE(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner:     PONE(AudioObjectID, kObjAparato);
        case kAudioObjectPropertyOwnedObjects: *usado = 0; return 0;
        case kAudioStreamPropertyIsActive:  PONE(UInt32, 1);
        case kAudioStreamPropertyDirection: PONE(UInt32, o == kObjEntrada ? 1 : 0);
        case kAudioStreamPropertyTerminalType:
            PONE(UInt32, o == kObjEntrada ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker);
        case kAudioStreamPropertyStartingChannel: PONE(UInt32, 1);
        case kAudioStreamPropertyLatency:   PONE(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            if (cap < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
            LlenarFormato((AudioStreamBasicDescription *)dat);
            *usado = sizeof(AudioStreamBasicDescription); return 0;
        }
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            if (cap < sizeof(AudioStreamRangedDescription)) return kAudioHardwareBadPropertySizeError;
            AudioStreamRangedDescription *r = (AudioStreamRangedDescription *)dat;
            LlenarFormato(&r->mFormat);
            r->mSampleRateRange.mMinimum = MBOX_TASA;
            r->mSampleRateRange.mMaximum = MBOX_TASA;
            *usado = sizeof(AudioStreamRangedDescription); return 0;
        }
        }
        break;
    }
    #undef PONE
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus EscribirProp(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t c,
                             const AudioObjectPropertyAddress *a, UInt32 nq, const void *q,
                             UInt32 n, const void *dat) {
    (void)d; (void)o; (void)c; (void)a; (void)nq; (void)q; (void)n; (void)dat;
    return kAudioHardwareUnsupportedOperationError;
}

// --- entrada/salida -----------------------------------------------------------
static OSStatus ArrancarES(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c) {
    (void)d; (void)o; (void)c;
    AbrirDiag(); DIAG(1);
    pthread_mutex_lock(&gCerrojo);
    if (gCuentaES == 0) {
        AbrirCompartida();
        gAnclaHost = mach_absolute_time();
        gAnclaMuestra = 0;
        gAnclado = 0; ++gSemilla;
        REG("ES arrancada; ayudante %s", AyudanteVivo() ? "vivo" : "ausente");
    }
    ++gCuentaES;
    pthread_mutex_unlock(&gCerrojo);
    return 0;
}
static OSStatus PararES(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c) {
    (void)d; (void)o; (void)c;
    pthread_mutex_lock(&gCerrojo);
    if (gCuentaES) --gCuentaES;
    pthread_mutex_unlock(&gCerrojo);
    return 0;
}

static OSStatus MarcaCero(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c,
                          Float64 *tm, UInt64 *th, UInt64 *semilla) {
    (void)d; (void)o; (void)c;
    AbrirDiag(); DIAG(2);
    if (gCab) gCab->pluginLatido++;
    ReintentarCompartida();
    if (AyudanteVivo()) {
        // Esclavizado al reloj del APARATO, que es el que manda: el ayudante
        // publica cuantos marcos lleva capturados y en que instante.
        UInt64 mc = gCab->marcosCapturados, ah = gCab->anclaHost;
        if (!gAnclado || mc < gOrigen) {     // primer enganche, o el ayudante reinicio
            gOrigen = mc; gAnclado = 1; ++gSemilla;
        }
        UInt64 rel = mc - gOrigen;
        UInt64 resto = rel % MBOX_PERIODO;
        *tm = (Float64)(rel - resto);
        *th = ah - (UInt64)((double)resto * gTicksPorMarco);
        *semilla = gSemilla;
        return 0;
    }
    // Sin ayudante: reloj propio, para poder probar la mitad de CoreAudio sola.
    if (gAnclado) { gAnclado = 0; ++gSemilla; }   // se fue: la linea se corta
    UInt64 ahora = mach_absolute_time();
    if (ahora >= gAnclaHost + gTicksPeriodo) { gAnclaMuestra += MBOX_PERIODO; gAnclaHost += gTicksPeriodo; }
    *tm = gAnclaMuestra; *th = gAnclaHost;
    *semilla = gSemilla;
    return 0;
}

static OSStatus HaraES(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op,
                       Boolean *hara, Boolean *enSitio) {
    (void)d; (void)o; (void)c;
    DIAG(8);
    if (gDiag) gDiag[9] = op;        // la ultima operacion por la que pregunta
    Boolean s = (op == kAudioServerPlugInIOOperationReadInput ||
                 op == kAudioServerPlugInIOOperationWriteMix);
    if (hara) *hara = s;
    if (enSitio) *enSitio = true;
    return 0;
}
static OSStatus EmpiezaES(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op,
                          UInt32 n, const AudioServerPlugInIOCycleInfo *ci) {
    (void)d; (void)o; (void)c; (void)op; (void)n; (void)ci; return 0; }
static OSStatus TerminaES(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 c, UInt32 op,
                          UInt32 n, const AudioServerPlugInIOCycleInfo *ci) {
    (void)d; (void)o; (void)c; (void)op; (void)n; (void)ci; return 0; }

// OJO con la firma: DoIOOperation lleva NUEVE parametros y el tercero es el
// identificador del FLUJO, no el del cliente. Escribirla sin el corre todos los
// argumentos un puesto, y entonces lo que se lee como operacion es en realidad
// el identificador de cliente: no coincide nunca con 'read' ni con 'rite' y las
// dos ramas se saltan siempre, en silencio.
static OSStatus HaceES(AudioServerPlugInDriverRef d, AudioObjectID aparato, AudioObjectID flujo,
                       UInt32 cliente, UInt32 op, UInt32 nMarcos,
                       const AudioServerPlugInIOCycleInfo *ci, void *buf, void *buf2) {
    (void)d; (void)aparato; (void)flujo; (void)cliente; (void)buf2;
    DIAG(10);
    if (gDiag) gDiag[11] = op;       // la ultima operacion que de verdad llega
    if (op == kAudioServerPlugInIOOperationReadInput) {
        DIAG(4); if (gCab) gCab->pluginLecturas++;
        float *sal = (float *)buf;
        if (!AyudanteVivo()) { memset(sal, 0, (size_t)nMarcos * MBOX_CANALES * sizeof(float)); return 0; }
        UInt64 m = gOrigen + (UInt64)ci->mInputTime.mSampleTime;
        for (UInt32 i = 0; i < nMarcos; i++) {
            const float *o_ = gAnCaptura + ((size_t)((m + i) & MBOX_MASCARA) * MBOX_CANALES);
            memcpy(sal + (size_t)i * MBOX_CANALES, o_, MBOX_CANALES * sizeof(float));
        }
        return 0;
    }
    if (op == kAudioServerPlugInIOOperationWriteMix) {
        DIAG(3); if (gCab) gCab->pluginEscrituras++;
        if (!AyudanteVivo()) return 0;
        const float *ent = (const float *)buf;
        UInt64 m = gOrigen + (UInt64)ci->mOutputTime.mSampleTime;
        for (UInt32 i = 0; i < nMarcos; i++) {
            float *o_ = gAnRepro + ((size_t)((m + i) & MBOX_MASCARA) * MBOX_CANALES);
            memcpy(o_, ent + (size_t)i * MBOX_CANALES, MBOX_CANALES * sizeof(float));
        }
        return 0;
    }
    return 0;
}

// --- la tabla, en el orden exacto del encabezado ------------------------------
static AudioServerPlugInDriverInterface gInterfaz = {
    NULL,
    Consultar, Retener, Soltar,
    Iniciar, CrearAparato, DestruirAparato,
    AnadirCliente, QuitarCliente,
    CambioConfig, AbortarCambio,
    TieneProp, EsAjustable, TamanoProp, LeerProp, EscribirProp,
    ArrancarES, PararES, MarcaCero,
    HaraES, EmpiezaES, HaceES, TerminaES
};

// Punto de entrada que nombra el Info.plist.
void *MboxProCrear(CFAllocatorRef alloc, CFUUIDRef tipo);
void *MboxProCrear(CFAllocatorRef alloc, CFUUIDRef tipo) {
    (void)alloc;
    if (!CFEqual(tipo, kAudioServerPlugInTypeUUID)) return NULL;
    return gDriver;
}
