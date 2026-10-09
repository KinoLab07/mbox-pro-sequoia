// mboxd - el ayudante. Posee el FireWire y hace de puente con el plugin del HAL.
//
// Corre como el usuario normal, sin privilegios y con SIP activado. Mantiene los
// dos streams isocronos contra la Mbox Pro y publica dos anillos en memoria
// compartida (ver mboxshm.h), que es lo unico que coreaudiod puede tocar.
//
// El motor es el de mboxfases.c, ya verificado: modo bloqueante de 8 bloques,
// SetDCLUserHeaderPtr obligatorio en cada DCL de envio (sin el hay panic del
// kernel), anillo de recepcion de 1024 ranuras y el router reprogramado despues
// del relock.
//
// Lo nuevo es el relleno continuo del anillo de envio. Ya no es un tono fijo: hay
// que escribir por delante de donde va leyendo el DMA, y para saberlo se usa el
// reloj del bus. GetCycleTime da el ciclo actual, y como el DMA manda el paquete
// i en el ciclo C0+i, basta anotar C0 justo despues de arrancar.
//
// Compilar: clang -O2 -o mboxd mboxd.c -framework IOKit -framework CoreFoundation
// Ejecutar: ./mboxd            (Ctrl-C para parar; deja el aparato en reposo)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <CoreFoundation/CoreFoundation.h>
#include "mboxshm.h"

#define PRIV      0xFFFFE0000000ULL
#define EXT       0xFFFFE0200000ULL
#define OPCODE    0x80070001u

#define NPKT_S    256
#define PORGRUPO  8
#define CONDATOS  6
#define BLOQUES   8
#define DBS       9
#define PKT_S     (8 + BLOQUES * DBS * 4)
#define DATOS_S   (NPKT_S * PKT_S)
#define HDROFF    (((DATOS_S + 4095) / 4096) * 4096)
#define TOTAL_S   (HDROFF + 4096)
#define RETARDO   (3 * 3072)
#define MARCOS_VUELTA (CONDATOS * (NPKT_S / PORGRUPO) * BLOQUES)   // 1536

#define NPKT_R    1024
#define PKT_R     512
#define HDRBYTES  4

#define ADELANTO  64        // paquetes que vamos por delante del DMA (8 ms)
// DESFASE: que muestra del anillo se lee respecto de lo capturado. ERA +1536,
// puesto a ojo, y era el origen de la distorsion: CoreAudio solo tiene escrito
// hasta "ahora + un bufer" (~512), asi que se leian muestras AUN NO ESCRITAS,
// restos de hace 0,68 s, y la lectura cruzaba esa frontera adelante y atras.
// Medido: 75 saltos por segundo en multiplos de 8 muestras, con el anillo que
// escribe CoreAudio perfectamente limpio. Ahora se lee POR DETRAS de lo escrito
// y se ajusta solo si aun asi encuentra muestras sin escribir.
static long long gDesfase = -1024;   // punto de partida; luego lo gobierna el margen medido
// Las lecturas de registros son sincronas y frenan el bucle que rellena el
// audio: con el medidor de picos dentro hubo 60 resincronizaciones por segundo.
// Solo con MBOX_MEDIDOR=1, para diagnosticar.
static int gMedidor = 0, gRueda = 0;
// MBOX_RUEDA_SOFT=1: la rueda la aplica el ayudante solo a las lineas 1-2 (la
// primera version). Por defecto la aplica el firmware a las seis salidas.
static int gRuedaSoft = 0;
// Grabadora de lo ENVIADO (canal 0), para separar envio de captura.
#define GRAB_N (48000 * 3)
static float gGrab[GRAB_N]; static unsigned gGrabK = GRAB_N; static volatile sig_atomic_t gVolcar = 0;
static void alVolcar(int s) { (void)s; gVolcar = 1; }
static long long gSytPrev = -1; static unsigned long long gSytTicks = 0, gSytMarcos = 0;

// Router de trabajo: captura desde las entradas analogicas, reproduccion a las
// salidas por el mezclador. Sin retorno host->host, que degrada el aparato.
static unsigned short kRouter[] = {
    0x40b0,0x41b1,0x42b2,0x43b3,0x44b4,0x45b5, 0x00b6,0x01b7,
    0x4020,0x4121,0x4222,0x4323,0x4424,0x4525, 0x0026,0x0127,
    0xb028,0xb129,0xb22a,0xb32b,0xb42c,0xb52d,0xb62e,0xb72f,
    0x5030,0x5131,
    // Configuracion normal de interfaz: lineas 1-2 = mezcla de monitor (salidas
    // 0-1 del mezclador), lineas 3-6 = canales 3-6 del ordenador por separado
    // (salidas 2-5 del mezclador, pases directos). La tabla de Avid mandaba la
    // mezcla de monitor tambien a 3-4 y aparcaba las salidas 2-3.
    0x2040,0x2141,0x2242,0x2343,0x2444,0x2545,
    0x2600,0x2701,
    0x2850,0x2951,0x2a52,0x2b53,0x2c54,0x2d55,
    0x2ef0,0x2ff1 };
#define NROUTER (sizeof(kRouter)/sizeof(kRouter[0]))
static const unsigned short kMixIdx[] = {
    8,9,10,11,12,13,14,15,16, 26,27,28,29,30,31,32,33,34,
    46,65,84,103,122,141, 152,171,190,209 };
static const unsigned short kMixVal[] = {
    0x0abb,0x12b7,0x12b7,0x12b7,0x12b7,0x12b7,0x10d4,0x10d4,0x19f2,
    0x0abb,0x12b7,0x12b7,0x12b7,0x12b7,0x12b7,0x10d4,0x10d4,0x19f2,
    0x4000,0x4000,0x4000,0x4000,0x4000,0x4000, 0x4000,0x4000,0x4000,0x4000 };
#define NMIX (sizeof(kMixIdx)/sizeof(kMixIdx[0]))

static IOFireWireLibDeviceRef D = NULL;
static UInt64 G = 0, T = 0, X = 0;
static volatile sig_atomic_t gParar = 0;
static double tpmGlobal = 0;   // ticks de host por marco a 48 kHz
static void alParar(int s) { (void)s; gParar = 1; }
// Codigo de salida. 0 = parado a peticion (Ctrl-C, launchctl). Distinto de 0 =
// algo fallo (Mbox apagada, reinicio del bus, el Mac se durmio...): launchd
// relanza el ayudante, que espera a la Mbox y la vuelve a configurar entera.
static volatile int gCodigoSalida = 0;
// La memoria compartida se marca como invalida en CUALQUIER salida. Antes solo
// se hacia en la salida normal: un arranque fallido dejaba un segmento con la
// magia valida, el plugin se enganchaba a el durante sus 5 s de vida y se
// quedaba ahi para siempre (medido el 9 oct: apagar la Mbox la dejaba muda).
static MboxCab *gCabGlobal = NULL;
static void InvalidarCompartida(void) { if (gCabGlobal) { gCabGlobal->magia = 0; gCabGlobal->anclaHost = 0; } }
static void Fallo(int codigo, const char *por) {
    if (!gCodigoSalida) gCodigoSalida = codigo;
    fprintf(stderr, "\nmboxd: %s; salgo para que me relancen\n", por);
    gParar = 1;
}

static void A(UInt64 a, FWAddress *o) {
    o->nodeID = 0; o->addressHi = (UInt16)((a >> 32) & 0xFFFF); o->addressLo = (UInt32)(a & 0xFFFFFFFF); }
// CUIDADO: la version anterior devolvia 0 cuando la transaccion FALLABA, que es
// indistinguible de "el registro vale cero". Con la pila FireWire colgada —cosa
// que pasa— eso convierte cualquier medicion en ficcion: el programa informa
// ceros con aire de dato. Ahora se cuentan los fallos y quien mida puede saber
// si el aparato seguia contestando.
static unsigned long gFallosLectura = 0, gLecturas = 0;
static UInt32 R(UInt64 a) {
    FWAddress f; A(a, &f); UInt32 v = 0;
    gLecturas++;
    if ((*D)->ReadQuadlet(D, (*D)->GetDevice(D), &f, &v, false, 0) != kIOReturnSuccess) {
        gFallosLectura++; return 0;
    }
    return CFSwapInt32BigToHost(v); }
static unsigned long gFallosEscritura = 0;
static int W(UInt64 a, UInt32 v) {
    FWAddress f; A(a, &f);
    int ok = (*D)->WriteQuadlet(D, (*D)->GetDevice(D), &f, CFSwapInt32HostToBig(v), false, 0) == kIOReturnSuccess;
    if (!ok) gFallosEscritura++;
    return ok;
}
static int W_viejo(UInt64 a, UInt32 v) {
    FWAddress f; A(a, &f);
    return (*D)->WriteQuadlet(D, (*D)->GetDevice(D), &f, CFSwapInt32HostToBig(v), false, 0) == kIOReturnSuccess; }
static UInt64 Sec(int i) { return EXT + (UInt64)R(EXT + (UInt64)i * 8) * 4; }
static int EsperarComando(UInt64 c) {
    for (int i = 0; i < 200; i++) { if (!(R(c) & 0x80000000u)) return i; usleep(10000); } return -1; }

static IOReturn hSup(IOFireWireLibIsochPortRef p, IOFWSpeed *s, UInt64 *c) {
    (void)p; *s = kFWSpeed400MBit; *c = ~0ULL; return kIOReturnSuccess; }
// Como ASFireWire: RX lleva tambien seq_start = 0 y TX la velocidad S400.
static IOReturn hAllocRx(IOFireWireLibIsochPortRef p, IOFWSpeed s, UInt32 ch) {
    (void)p; (void)s; printf("  canal %u -> RX del aparato\n", ch); W(X + 8, ch); W(X + 0x0C, 0); return kIOReturnSuccess; }
static IOReturn hAllocTx(IOFireWireLibIsochPortRef p, IOFWSpeed s, UInt32 ch) {
    (void)p; (void)s; printf("  canal %u -> TX del aparato\n", ch); W(T + 8, ch); W(T + 0x14, 2); return kIOReturnSuccess; }
static IOReturn hRelRx(IOFireWireLibIsochPortRef p) { (void)p; W(X + 8, 0xFFFFFFFF); return kIOReturnSuccess; }
static IOReturn hRelTx(IOFireWireLibIsochPortRef p) { (void)p; W(T + 8, 0xFFFFFFFF); return kIOReturnSuccess; }
static IOReturn hStart(IOFireWireLibIsochPortRef p) { (void)p; W(G + 0x50, 1); return kIOReturnSuccess; }
static IOReturn hStopP(IOFireWireLibIsochPortRef p) { (void)p; return kIOReturnSuccess; }
static void hParo(IOFireWireLibIsochChannelRef c, UInt32 n) { (void)c;
    fprintf(stderr, "\n!! un canal se paro, condicion 0x%08X\n", n);
    Fallo(3, "el sistema paro un canal isocrono (reinicio del bus o Mbox apagada)"); }

static IOFireWireLibDeviceRef Open(void) {
    CFMutableDictionaryRef m = IOServiceMatching("IOFireWireUnit");
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMasterPortDefault, m, &it) != kIOReturnSuccess) return NULL;
    io_object_t o; IOFireWireLibDeviceRef f = NULL;
    while ((o = IOIteratorNext(it))) {
        CFTypeRef vr = IORegistryEntrySearchCFProperty(o, kIOServicePlane, CFSTR("Vendor_ID"),
            kCFAllocatorDefault, kIORegistryIterateRecursively | kIORegistryIterateParents);
        int av = 0;
        if (vr) { int v = 0; CFNumberGetValue((CFNumberRef)vr, kCFNumberIntType, &v); av = (v == 0x00A07E); CFRelease(vr); }
        if (!av) { IOObjectRelease(o); continue; }
        IOCFPlugInInterface **p = NULL; SInt32 sc = 0;
        if (IOCreatePlugInInterfaceForService(o, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &p, &sc) == kIOReturnSuccess && p) {
            IOFireWireLibDeviceRef d = NULL;
            if ((*p)->QueryInterface(p, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID), (void **)&d) == S_OK && d) {
                if ((*d)->OpenWithSessionRef(d, (*d)->GetSessionRef(d)) == kIOReturnSuccess || (*d)->Open(d) == kIOReturnSuccess) f = d;
                else (*d)->Release(d);
            }
            IODestroyPlugInInterface(p);
        }
        IOObjectRelease(o);
        if (f) break;
    }
    IOObjectRelease(it);
    return f;
}
static void be32(UInt8 *p, UInt32 v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }

// El reloj del bus: 7 bits de segundos y 13 de ciclo, 8000 ciclos por segundo.
// Si la lectura falla se repite la anterior: devolver 0 hacia saltar la cuenta
// acumulada 128 segundos de golpe y forzaba una resincronizacion (un salto).
static unsigned CicloAhora(void) {
    static unsigned ultimo = 0;
    UInt32 ct = 0;
    if ((*D)->GetCycleTime(D, &ct) != kIOReturnSuccess) return ultimo;
    return ultimo = ((ct >> 25) & 0x7F) * 8000u + ((ct >> 12) & 0x1FFF);
}

// --- RUEDA DE MONITOR ---------------------------------------------------------
// La rueda de la Mbox se lee en aplicacion +0x08: posicion absoluta 0..255, en
// pasos de 3 (medido el 7 de octubre girandola despacio). El firmware no la
// aplica a nada con UITarget=0, asi que la aplica el ayudante: coeficientes del
// mezclador de las salidas 0-1 (la mezcla de monitor, lineas 1-2). Curva de
// monitor: 0 = silencio, maximo = 0 dB, en medio ~ -14 dB.
static UInt16 GananciaRueda(UInt32 v) {
    if (v == 0) return 0;
    if (v > 255) v = 255;
    // 60 dB lineales era demasiado: "se empieza a escuchar casi a mitad de
    // giro". 40 dB con curva de potencia 1,5: -14 dB a media rueda.
    double db = -40.0 * pow(1.0 - (double)v / 255.0, 1.5);
    double g = 16384.0 * pow(10.0, db / 20.0);
    return (UInt16)(g < 1 ? 1 : g);
}
static void AplicarRueda(UInt64 mix, UInt32 v) {
    UInt16 g = GananciaRueda(v);
    W(mix + 4 + (UInt64)(0 * 18 + 8) * 4, g);   // salida 0 <- canal 1
    W(mix + 4 + (UInt64)(1 * 18 + 9) * 4, g);   // salida 1 <- canal 2
}

// --- DUENO DEL APARATO -------------------------------------------------------
//
// Todo driver de DICE (Avid, Linux, ASFireWire) lo primero que hace es
// apuntarse como dueno en GLOBAL+0x00 con un compare-swap de 64 bits, dando la
// direccion donde quiere recibir las notificaciones. Yo no lo hacia nunca: el
// aparato creia que no habia ningun ordenador al mando. Aqui la direccion es un
// espacio de direcciones propio que acepta las escrituras y no hace nada.
static IOFireWireLibPseudoAddressSpaceRef gBuzon = NULL;
static UInt32 gBuzonMem[4];
static UInt64 gDueno = 0;
static int CompararCambiar64(UInt64 a, UInt64 esperado, UInt64 nuevo, UInt64 *viejo) {
    FWAddress f; A(a, &f);
    UInt32 e[2] = { CFSwapInt32HostToBig((UInt32)(esperado >> 32)), CFSwapInt32HostToBig((UInt32)esperado) };
    UInt32 n[2] = { CFSwapInt32HostToBig((UInt32)(nuevo >> 32)),    CFSwapInt32HostToBig((UInt32)nuevo) };
    UInt32 o[2] = { 0, 0 };
    IOReturn r = (*D)->CompareSwap64(D, (*D)->GetDevice(D), &f, e, n, o, 8, false, 0);
    if (viejo) *viejo = ((UInt64)CFSwapInt32BigToHost(o[0]) << 32) | CFSwapInt32BigToHost(o[1]);
    return r == kIOReturnSuccess;
}
static UInt64 LeerDueno(void) { return ((UInt64)R(G + 0x00) << 32) | R(G + 0x04); }
static void ReclamarDueno(UInt16 nodo) {
    const UInt64 NADIE = 0xFFFF000000000000ULL;
    gBuzon = (*D)->CreatePseudoAddressSpace(D, sizeof gBuzonMem, NULL, 1024, gBuzonMem,
                 kFWAddressSpaceAutoWriteReply | kFWAddressSpaceAutoReadReply,
                 CFUUIDGetUUIDBytes(kIOFireWirePseudoAddressSpaceInterfaceID));
    if (!gBuzon) { printf("mboxd: no pude crear el buzon de notificaciones; sigo sin dueno\n"); return; }
    FWAddress fa; (*gBuzon)->GetFWAddress(gBuzon, &fa);
    gDueno = ((UInt64)(0xFFC0u | (nodo & 0x3F)) << 48) | ((UInt64)fa.addressHi << 32) | fa.addressLo;
    UInt64 antes = LeerDueno(), viejo = 0;
    // Si quedo apuntado un mboxd anterior que murio sin soltarlo, se le quita.
    UInt64 esperado = (antes != NADIE && (antes >> 48) == (gDueno >> 48)) ? antes : NADIE;
    CompararCambiar64(G + 0x00, esperado, gDueno, &viejo);
    UInt64 ahora = LeerDueno();
    printf("mboxd: dueno antes 0x%016llX, ahora 0x%016llX %s\n", (unsigned long long)antes,
           (unsigned long long)ahora, ahora == gDueno ? "(SOY YO)" : "(NO SE PUDO RECLAMAR)");
    if (ahora != gDueno) gDueno = 0;
}
static void SoltarDueno(void) {
    if (!gDueno) return;
    UInt64 viejo = 0;
    CompararCambiar64(G + 0x00, gDueno, 0xFFFF000000000000ULL, &viejo);
    printf("mboxd: dueno soltado (queda 0x%016llX)\n", (unsigned long long)LeerDueno());
}

// Deja el reloj enganchado y el router puesto. Se usa al arrancar y al rearmar.
//
// REVISION DEL 6 DE OCTUBRE, contra el driver ASFireWire que si funciona:
//
//  - Antes se reescribia CLOCK_SELECT en CADA arranque para forzar un
//    reenganche. ASFireWire lo prohibe explicitamente: reescribirlo con el valor
//    que ya tiene relanza el PLL y "atasca los flujos del aparato". Encaja con el
//    estado mudo que solo arreglaba apagar la Mbox. Ahora solo se escribe si el
//    aparato NO esta ya a 48 kHz enganchado.
//  - La tabla de Avid de 44 rutas (la de ASFireWire) es la que sono. La
//    configuracion de fabrica es la de modo autonomo, con el mezclador en manos
//    del firmware y de la rueda; pelear con ella fue un error. Se vuelve a la de
//    Avid, escrita UNA vez.
static const unsigned short *gTabla = NULL; static size_t gNTabla = 0;
static int gSinRouter = 0, gAbrirMezcla = 0, gYaRearmado = 0, gTap = 0;
static int PrepararAparato(UInt64 cmd, UInt64 mix, UInt64 rtr, UInt64 cur) {
    const unsigned short *T = gTabla ? gTabla : kRouter;
    size_t N = gTabla ? gNTabla : NROUTER;

    UInt32 clk = R(G + 0x4C), tasa = R(G + 0x5C), st = R(G + 0x54);
    int yaEsta = (clk == 0x020C) && tasa == 48000 && (st & 1) && ((st >> 8) & 7) == 2;
    printf("mboxd: reloj CLOCK_SELECT=0x%X tasa=%u STATUS=0x%X -> %s\n", clk, tasa, st,
           yaEsta ? "ya esta a 48 kHz, NO se toca" : "hay que ponerlo a 48 kHz");
    if (!yaEsta) {
        W(G + 0x4C, 0x020C);
        int ok = 0;
        for (int i = 0; i < 40; i++) { usleep(100000); UInt32 s = R(G + 0x54);
            if ((s & 1) && ((s >> 8) & 7) == 2 && R(G + 0x5C) == 48000) { ok = 1; break; } }
        if (!ok) return -1;
    }
    if (gSinRouter) {
        if (gAbrirMezcla) {
            W(mix + 4 + (UInt64)(0*18 + 0)*4, 0x2000);
            W(mix + 4 + (UInt64)(1*18 + 1)*4, 0x2000);
            W(mix + 4 + (UInt64)(0*18 + 6)*4, 0x2000);
            W(mix + 4 + (UInt64)(1*18 + 7)*4, 0x2000);
        }
        return 0;
    }
    // Router: solo si el vigente no es ya el nuestro.
    int igual = (R(cur) == N);
    for (size_t i = 0; igual && i < N; i++) if ((R(cur + 4 + i * 4) & 0xFFFF) != T[i]) igual = 0;
    if (!igual) {
        W(rtr, (UInt32)N);
        for (size_t i = 0; i < N; i++) W(rtr + 4 + i * 4, T[i]);
        W(cmd, OPCODE); EsperarComando(cmd);
        printf("mboxd: router de Avid escrito (%zu rutas)\n", N);
    } else printf("mboxd: el router de Avid ya estaba puesto\n");
    // Coeficientes de Avid, y la mezcla de monitor en estereo limpio: la salida
    // 0 del mezclador (lineas 1 y 3) solo con el canal izquierdo del ordenador a
    // unidad, la 1 (lineas 2 y 4) solo con el derecho. Los de la captura de
    // Avid mezclaban los dos canales en los dos lados a -10/-15 dB.
    // MEZCLADOR COMPLETO, en cada arranque. Escribir solo algunos coeficientes
    // dejaba vivos los de pruebas anteriores (el 7 de octubre, "salida 2 <-
    // canal 1" de una prueba seguia ahi y YouTube salia tambien por 3-4).
    // Entradas del mezclador 8-15 = canales 1-8 del ordenador.
    {
        static UInt16 m[16][18];
        memset(m, 0, sizeof m);
        if (gRuedaSoft) { UInt16 g = GananciaRueda(R(Sec(8) + 0x08));   // rueda aplicada por el ayudante
            m[0][8] = g;  m[1][9] = g; }
        else { m[0][8] = 0x4000; m[1][9] = 0x4000; }
        for (int k = 2; k < 8; k++) m[k][8 + k] = 0x4000;   // 2-5 -> lineas 3-6, 6-7 -> S/PDIF
        m[8][8] = 0x4000;  m[9][9] = 0x4000;    // auricular A = canales 1-2
        m[10][8] = 0x4000; m[11][9] = 0x4000;   // auricular B = canales 1-2 (los dos son monitor)
        for (int o = 0; o < 16; o++)
            for (int e = 0; e < 18; e++)
                W(mix + 4 + (UInt64)(o * 18 + e) * 4, m[o][e]);
        int mal = 0;
        for (int o = 0; o < 16; o++)
            for (int e = 0; e < 18; e++)
                if ((R(mix + 4 + (UInt64)(o * 18 + e) * 4) & 0xFFFF) != m[o][e]) mal++;
        printf("mboxd: mezclador completo escrito (288 coeficientes, %d distintos al releer)\n", mal);
    }
    // EL LED DE LOS PARLANTES. Al cargar un router, el firmware pone la
    // seleccion de monitor a 0: LED verde apagado y monitores sin senal. El
    // driver de Avid (y ASFireWire) vuelve a escribir UILEDState, seccion de
    // aplicacion +0x14, con la seleccion 1: 0x100 | bit0 | (1 << 5) = 0x121.
    {
        // MODO ORDENADOR. Los siete volcados de Mojave con el driver de Avid
        // funcionando tienen SIEMPRE: StandaloneMode=2, UITarget=0x3F,
        // InputControl=0x10, UILEDState=0x101. Recien encendida la Mbox esta en
        // StandaloneMode=1 (autonoma) y UITarget=3, y asi las lineas 1-2 no
        // sacan la reproduccion aunque el mezclador la lleve (medido).
        UInt64 app = Sec(8);
        UInt32 m0 = R(app), t0 = R(app + 0x0C), i0 = R(app + 0x24), l0 = R(app + 0x14);
        // StandaloneMode=2 dejo TODAS las salidas mudas (7 oct): en modo
        // ordenador la Mbox espera algo mas del driver. Se queda en 1 y se
        // prueba cada registro por separado: MBOX_APP=modo,uitarget,input,led
        {
            // UITarget = 0x3F, como el driver original: las seis salidas siguen
            // la rueda y la aplica el FIRMWARE, con su curva. (Con 0x3F "no
            // sonaba nada" porque la rueda arranca en 0 al encender y no se giro.)
            unsigned vm = 1, vt = gRuedaSoft ? 0 : 0x3F, vi = m0 ? i0 : 1, vl = 0x121;   // UITarget=0: con 3 las lineas 1-2 seguian un volumen de monitor a 0 (mudas)
            const char *e = getenv("MBOX_APP");
            if (e) sscanf(e, "%x,%x,%x,%x", &vm, &vt, &vi, &vl);
            W(app + 0x00, vm); W(app + 0x0C, vt); W(app + 0x24, vi); W(app + 0x14, vl);
        }
        // VOLUMEN DE MONITOR: aplicacion +0x08. UITarget dice que salidas lo
        // siguen (bit por salida: 3 = lineas 1 y 2). Recien encendida vale 0 y
        // las lineas 1-2 quedan mudas. En los volcados de Avid: 0x48..0xA8.
        {
            UInt32 v8 = R(app + 0x08);
            const char *e = getenv("MBOX_VOL");
            if (e) { W(app + 0x08, (UInt32)strtoul(e, NULL, 16)); }
            printf("mboxd: volumen de monitor (+0x08) 0x%X -> 0x%X\n", v8, R(app + 0x08));
        }
        printf("mboxd: modo %X->%X  UITarget %X->%X  InputControl %X->%X  LED %X->%X\n",
               m0, R(app), t0, R(app + 0x0C), i0, R(app + 0x24), l0, R(app + 0x14));
    }
    int mal = 0;
    if (R(cur) != N) mal = 1;
    for (size_t i = 0; !mal && i < N; i++) if ((R(cur + 4 + i * 4) & 0xFFFF) != T[i]) mal++;
    return mal ? -2 : 0;
}

int main(void) {
    signal(SIGINT, alParar); signal(SIGTERM, alParar);
    { mach_timebase_info_data_t tb; mach_timebase_info(&tb);
      tpmGlobal = (1.0e9 / 48000.0) * (double)tb.denom / (double)tb.numer; }
    // MBOX_SOLO_RX=1 arranca sin el envio, para separar las dos mitades.
    const char *eSolo = getenv("MBOX_SOLO_RX"); int soloRx = eSolo && atoi(eSolo);
    // MBOX_TONO=1: ignorar el anillo y escribir un tono fijo en TODOS los
    // paquetes, como la prueba que esta manana sono por los parlantes. Separa
    // "el camino de envio funciona" de "mi relleno continuo esta mal de fase".
    const char *eTono = getenv("MBOX_TONO"); int tonoFijo = eTono && atoi(eTono);
    // MBOX_RETORNO=1: las dos primeras rutas de captura pasan de las entradas
    // analogicas a la salida del mezclador, para leer nuestra propia
    // reproduccion y saber si el aparato la esta consumiendo, sin depender de
    // la etapa analogica ni de nadie escuchando.
    // MBOX_ESCOPETA=1: en vez de suponer que bloque del router alimenta los
    // monitores, mandar la salida 8 del mezclador a TODOS los destinos a la vez.
    // Si con esto no sale sonido, el problema no esta en el encaminamiento.
    int escopeta = getenv("MBOX_ESCOPETA") && atoi(getenv("MBOX_ESCOPETA"));
    static unsigned short kEscopeta[41];
    if (escopeta) {
        int k = 0;
        kEscopeta[k++] = 0xb028;                       // host:0 -> entrada 8 del mezclador
        const int bloques[] = { 0, 1, 3, 4, 5 };
        for (unsigned b = 0; b < 5; b++)
            for (unsigned c = 0; c < 8; c++)
                kEscopeta[k++] = (unsigned short)((0x28 << 8) | (bloques[b] << 4) | c);
        printf("mboxd: ESCOPETA, %d rutas a todos los destinos\n", k);
    }

    // MBOX_RETORNO=N: que salida del mezclador se devuelve a la captura.
    //   1 -> salidas 8 y 9   (las que van a las salidas de linea)
    //   2 -> salidas 0 y 1   (la mezcla de monitor y auriculares)
    //   3 -> salidas 2 y 3
    {
        const char *e = getenv("MBOX_RETORNO");
        int q = e ? atoi(e) : 0;
        if (q) {
            unsigned base = (q == 1) ? 8 : (q == 2) ? 0 : 2;
            kRouter[0] = (unsigned short)(((0x20 + base) << 8) | 0xb0);
            kRouter[1] = (unsigned short)(((0x20 + base + 1) << 8) | 0xb1);
            printf("mboxd: RETORNO de las salidas %u y %u del mezclador (0x%04X 0x%04X)\n",
                   base, base + 1, kRouter[0], kRouter[1]);
        }
    }
    if (getenv("MBOX_DIRECTO12")) {   // prueba: ordenador 1-2 directo a lineas 1-2
        for (size_t i = 0; i < NROUTER; i++) {
            if (kRouter[i] == 0x2040) kRouter[i] = 0xB040;
            if (kRouter[i] == 0x2141) kRouter[i] = 0xB141;
        }
        printf("mboxd: PRUEBA lineas 1-2 directas desde el ordenador (0xB040 0xB141)\n");
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    { time_t t = time(NULL); char b[64]; strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", localtime(&t));
      fprintf(stderr, "\n===== mboxd arranca %s =====\n", b); }
    gMedidor = getenv("MBOX_MEDIDOR") && atoi(getenv("MBOX_MEDIDOR"));
    gRueda = getenv("MBOX_RUEDA") ? atoi(getenv("MBOX_RUEDA")) : 0;
    gRuedaSoft = getenv("MBOX_RUEDA_SOFT") ? atoi(getenv("MBOX_RUEDA_SOFT")) : 0;
    {   // Hilo de tiempo real, como cualquier motor de audio: sin esto el
        // planificador puede dormirlo mas de lo que el relleno aguanta.
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        double t = (double)tb.denom / tb.numer * 1e6;   // ticks por ms
        thread_time_constraint_policy_data_t pol = { (uint32_t)(1 * t), (uint32_t)(0.3 * t), (uint32_t)(1 * t), 0 };
        pol.preemptible = 1;
        kern_return_t kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                                             (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        printf("mboxd: hilo de tiempo real %s\n", kr == KERN_SUCCESS ? "si" : "NO");
    }

    D = Open();
    if (!D) {
        // Sin Mbox: esperar a que aparezca en vez de salir, para no hacer girar
        // a launchd. Se comprueba cada 2 s.
        fprintf(stderr, "mboxd: no encuentro la Mbox Pro; espero a que se encienda o se conecte\n");
        while (!D && !gParar) { sleep(2); D = Open(); }
        if (!D) return 0;
        fprintf(stderr, "mboxd: Mbox Pro encontrada\n");
        sleep(1);   // que termine de asentarse tras encenderse
    }
    G = PRIV + (UInt64)R(PRIV +  0) * 4;
    T = PRIV + (UInt64)R(PRIV +  8) * 4;
    X = PRIV + (UInt64)R(PRIV + 16) * 4;
    UInt32 gen = 0; UInt16 nodo = 0;
    (*D)->GetBusGeneration(D, &gen);
    (*D)->GetLocalNodeIDWithGeneration(D, gen, &nodo);
    unsigned sid = nodo & 0x3F;
    printf("mboxd: Mbox Pro abierto, nodo 0x%04X\n", nodo);
    if (getenv("MBOX_SINDUENO")) printf("mboxd: PRUEBA sin registrarse como dueno\n");
    else ReclamarDueno(nodo);

    // --- memoria compartida ---------------------------------------------------
    shm_unlink(MBOX_SHM_NOMBRE);
    int fd = shm_open(MBOX_SHM_NOMBRE, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(fd, MBOX_BYTES_TOTAL) < 0) { perror("ftruncate"); return 1; }
    fchmod(fd, 0666);          // coreaudiod corre como _coreaudiod, no como tu
    void *sp = mmap(NULL, MBOX_BYTES_TOTAL, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (sp == MAP_FAILED) { perror("mmap"); return 1; }
    memset(sp, 0, MBOX_BYTES_TOTAL);
    MboxCab *cab = (MboxCab *)sp;
    gCabGlobal = cab; atexit(InvalidarCompartida);
    float *anCap = (float *)((char *)sp + MBOX_DESPL_CAPTURA);
    float *anRep = (float *)((char *)sp + MBOX_DESPL_REPRO);
    cab->canales = MBOX_CANALES; cab->tasa = MBOX_TASA;
    cab->marcosAnillo = MBOX_ANILLO; cab->periodo = MBOX_PERIODO;
    cab->desplCaptura = MBOX_DESPL_CAPTURA; cab->desplRepro = MBOX_DESPL_REPRO;
    cab->version = MBOX_VERSION;
    // El anillo de reproduccion nace "sin escribir" (NaN), no a cero: el margen
    // se mide buscando el primer NaN, y un cero inicial contaba como escrito.
    for (size_t i = 0; i < (size_t)MBOX_ANILLO * MBOX_CANALES; i++) anRep[i] = NAN;
    cab->magia = MBOX_MAGIA;   // lo ultimo: hasta aqui el plugin no lo da por bueno
    printf("mboxd: compartida %s, %.1f MB\n", MBOX_SHM_NOMBRE, MBOX_BYTES_TOTAL / 1048576.0);

    // --- router y reloj -------------------------------------------------------
    if (escopeta) { gTabla = kEscopeta; gNTabla = 41; }
    gSinRouter = getenv("MBOX_SINROUTER") && atoi(getenv("MBOX_SINROUTER"));
    if (gSinRouter) printf("mboxd: SIN TOCAR EL ROUTER (configuracion de fabrica)\n");
    gAbrirMezcla = getenv("MBOX_MEZCLA") && atoi(getenv("MBOX_MEZCLA"));
    gTap = getenv("MBOX_TAP") && atoi(getenv("MBOX_TAP"));
    UInt64 cmd = Sec(1), mix = Sec(2), rtr = Sec(4), cur = Sec(6), pico = Sec(3), appV = Sec(8);
    int pr = PrepararAparato(cmd, mix, rtr, cur);
    if (pr == -1) { fprintf(stderr, "mboxd: el reloj no se asento\n"); SoltarDueno(); shm_unlink(MBOX_SHM_NOMBRE); return 3; }
    if (pr == -2) { fprintf(stderr, "mboxd: el router no acepto la tabla\n"); SoltarDueno(); shm_unlink(MBOX_SHM_NOMBRE); return 3; }
    printf("mboxd: router vigente %u entradas; %u Hz\n", R(cur), R(G + 0x5C));

    // --- anillos DCL ----------------------------------------------------------
    UInt8 *bs = mmap(NULL, TOTAL_S, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
    UInt8 *br = mmap(NULL, NPKT_R * PKT_R, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
    if (bs == MAP_FAILED || br == MAP_FAILED) { perror("mmap dcl"); return 1; }
    memset(bs, 0, TOTAL_S); memset(br, 0, NPKT_R * PKT_R);
    UInt32 *hdr = (UInt32 *)(bs + HDROFF), *masc = (UInt32 *)(bs + HDROFF + 16);
    hdr[0] = 1u << 14; hdr[1] = 0; masc[0] = 0x0000C000u; masc[1] = 0;

    // Cabeceras CIP fijas. El relleno posterior solo toca la carga util, para no
    // romper la continuidad del DBC ni del SYT.
    size_t largo[NPKT_S]; int esDatos[NPKT_S]; unsigned marcoDe[NPKT_S];
    unsigned dbc = 0, nd = 0;
    for (int n = 0; n < NPKT_S; n++) {
        UInt8 *p = bs + (size_t)n * PKT_S;
        be32(p + 0, ((UInt32)sid << 24) | ((UInt32)DBS << 16) | dbc);
        if ((n % PORGRUPO) >= CONDATOS) {
            be32(p + 4, 0x90FF0000u | 0xFFFFu); largo[n] = 8; esDatos[n] = 0; marcoDe[n] = 0; continue;
        }
        unsigned t = RETARDO + 4096u * nd;
        be32(p + 4, 0x90020000u | (((t / 3072) & 0x0F) << 12) | (t % 3072));
        for (int k = 0; k < BLOQUES; k++) {
            UInt8 *d = p + 8 + (size_t)k * DBS * 4;
            for (int c = 0; c < 8; c++) d[c*4+0] = 0x40;     // etiqueta de audio
            be32(d + 8*4, 0x80000000u);                       // ranura MIDI
        }
        largo[n] = PKT_S; esDatos[n] = 1; marcoDe[n] = nd * BLOQUES;
        dbc = (dbc + BLOQUES) & 0xFF; nd++;
    }
    printf("mboxd: envio %u paquetes con datos, %u marcos por vuelta\n", nd, nd * BLOQUES);

    IOVirtualRange brS = { (IOVirtualAddress)bs, TOTAL_S }, brR = { (IOVirtualAddress)br, NPKT_R * PKT_R };
    IOFireWireLibNuDCLPoolRef poolS = (*D)->CreateNuDCLPool(D, NPKT_S + 8, CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    IOFireWireLibNuDCLPoolRef poolR = (*D)->CreateNuDCLPool(D, NPKT_R + 8, CFUUIDGetUUIDBytes(kIOFireWireNuDCLPoolInterfaceID));
    NuDCLRef pS = NULL, uS = NULL, pR = NULL, uR = NULL;
    for (int i = 0; i < NPKT_S; i++) {
        IOVirtualRange r = { (IOVirtualAddress)(bs + (size_t)i * PKT_S), largo[i] };
        NuDCLRef d = (*poolS)->AllocateSendPacket(poolS, NULL, 1, &r);
        (*poolS)->SetDCLUserHeaderPtr(d, hdr, masc);   // obligatorio: si no, panic
        if (!pS) pS = d; uS = d;
    }
    (*poolS)->SetDCLBranch(uS, pS);
    for (int i = 0; i < NPKT_R; i++) {
        IOVirtualRange r = { (IOVirtualAddress)(br + (size_t)i * PKT_R), PKT_R };
        NuDCLRef d = (*poolR)->AllocateReceivePacket(poolR, NULL, HDRBYTES, 1, &r);
        if (!pR) pR = d; uR = d;
    }
    (*poolR)->SetDCLBranch(uR, pR);

    IOFireWireLibLocalIsochPortRef locS = (*D)->CreateLocalIsochPortWithOptions(
        D, true, (*poolS)->GetProgram(poolS), 0,0,0, NULL,0, &brS,1,
        kFWIsochPortDefaultOptions, CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    IOFireWireLibLocalIsochPortRef locR = (*D)->CreateLocalIsochPortWithOptions(
        D, false, (*poolR)->GetProgram(poolR), 0,0,0, NULL,0, &brR,1,
        kFWIsochPortDefaultOptions, CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    IOFireWireLibRemoteIsochPortRef remS = (*D)->CreateRemoteIsochPort(D, false, CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    IOFireWireLibRemoteIsochPortRef remR = (*D)->CreateRemoteIsochPort(D, true,  CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    if (!locS || !locR || !remS || !remR) { fprintf(stderr, "mboxd: fallo creando puertos\n"); SoltarDueno(); shm_unlink(MBOX_SHM_NOMBRE); return 3; }
    (*remS)->SetGetSupportedHandler(remS,&hSup); (*remS)->SetAllocatePortHandler(remS,&hAllocRx);
    (*remS)->SetReleasePortHandler(remS,&hRelRx); (*remS)->SetStartHandler(remS,&hStart); (*remS)->SetStopHandler(remS,&hStopP);
    (*remR)->SetGetSupportedHandler(remR,&hSup); (*remR)->SetAllocatePortHandler(remR,&hAllocTx);
    (*remR)->SetReleasePortHandler(remR,&hRelTx); (*remR)->SetStartHandler(remR,&hStart); (*remR)->SetStopHandler(remR,&hStopP);

    IOFireWireLibIsochChannelRef canS = (*D)->CreateIsochChannel(D, true, PKT_S, kFWSpeed400MBit, CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    (*canS)->SetTalker(canS,(IOFireWireLibIsochPortRef)locS); (*canS)->AddListener(canS,(IOFireWireLibIsochPortRef)remS);
    IOFireWireLibIsochChannelRef canR = (*D)->CreateIsochChannel(D, true, PKT_R, kFWSpeed400MBit, CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    (*canR)->SetTalker(canR,(IOFireWireLibIsochPortRef)remR); (*canR)->AddListener(canR,(IOFireWireLibIsochPortRef)locR);
    (*D)->AddCallbackDispatcherToRunLoop(D, CFRunLoopGetCurrent());
    (*D)->AddIsochCallbackDispatcherToRunLoop(D, CFRunLoopGetCurrent());
    (*D)->TurnOnNotification(D);
    (*canS)->SetChannelForceStopHandler(canS,&hParo);
    (*canR)->SetChannelForceStopHandler(canR,&hParo);

    IOReturn r;
    if ((r = (*canR)->AllocateChannel(canR)) != kIOReturnSuccess) { fprintf(stderr, "AllocateChannel rx: 0x%08X\n", r); W(G + 0x50, 0); SoltarDueno(); shm_unlink(MBOX_SHM_NOMBRE); return 3; }
    if (!soloRx && (r = (*canS)->AllocateChannel(canS)) != kIOReturnSuccess) { fprintf(stderr, "AllocateChannel tx: 0x%08X\n", r); W(G + 0x50, 0); SoltarDueno(); shm_unlink(MBOX_SHM_NOMBRE); return 3; }
    if ((r = (*canR)->Start(canR)) != kIOReturnSuccess) { fprintf(stderr, "Start rx: 0x%08X\n", r); W(G + 0x50, 0); SoltarDueno(); shm_unlink(MBOX_SHM_NOMBRE); return 3; }
    unsigned cicloAntes = CicloAhora();
    if (!soloRx && (r = (*canS)->Start(canS)) != kIOReturnSuccess) { fprintf(stderr, "Start tx: 0x%08X\n", r); W(G + 0x50, 0); SoltarDueno(); shm_unlink(MBOX_SHM_NOMBRE); return 3; }
    if (soloRx) { W(G + 0x50, 1); printf("mboxd: SOLO RECEPCION\n"); }
    // Entrar en fase: invalidar todas las ranuras para que solo se consuman las
    // que el DMA escriba a partir de ahora.
    for (int i = 0; i < NPKT_R; i++) memset(br + (size_t)i * PKT_R, 0, 12);
    if (tonoFijo) {
        unsigned m = 0;
        for (int n = 0; n < NPKT_S; n++) {
            if (!esDatos[n]) continue;
            UInt8 *p = bs + (size_t)n * PKT_S;
            for (int k = 0; k < BLOQUES; k++) {
                int v = (int)(0x200000 * sin(2.0 * M_PI * (m % 48) / 48.0));   // -12 dBFS
                UInt8 *d = p + 8 + (size_t)k * DBS * 4;
                for (int c = 0; c < 8; c++) {
                    d[c*4+1] = (v >> 16) & 0xFF; d[c*4+2] = (v >> 8) & 0xFF; d[c*4+3] = v & 0xFF;
                }
                m++;
            }
        }
        printf("mboxd: TONO FIJO de 1 kHz en los %u paquetes con datos\n", nd);
    }
    int slot = 0, primera = 1;
    unsigned cicloCero = CicloAhora();      // el DMA manda el paquete i en C0+i

    // ANCLAR EL SYT AL RELOJ REAL DEL BUS.
    //
    // El SYT dice al aparato cuando presentar cada bloque, y hasta ahora lo
    // calculaba desde un cero arbitrario: el anillo se construye ANTES de
    // arrancar y el ciclo de arranque no se conoce hasta despues. Con un
    // desfase constante respecto al ciclo real, la reserva del aparato se va
    // vaciando o desbordando y acaba parandose. Medido: el tono volvia a -12,0
    // dBFS y se cortaba a los tres segundos.
    //
    // Ahora que cicloCero se conoce, se reescriben los SYT de los paquetes con
    // datos. El paquete de la posicion n sale en el ciclo cicloCero+n, y la
    // presentacion va RETARDO_CICLOS por delante. Cuadra solo: 6 paquetes con
    // datos por cada 8 ciclos, 6 x 4096 = 24576 ticks = 8 ciclos exactos.
    // El DMA arranca en algun ciclo entre cicloAntes y cicloCero; no se sabe
    // cual. Por eso la ancla se AUTOCALIBRA mas abajo mirando si el receptor de
    // la Mbox se engancha (EXT_STATUS bit 0x40). Era la loteria de "unas veces
    // entra y otras no".
    #define ANCLAR(ret) do { unsigned j_ = 0; \
        for (int n_ = 0; n_ < NPKT_S; n_++) { if (!esDatos[n_]) continue; \
            UInt64 t_ = (UInt64)((long long)cicloCero + (ret) + 64) * 3072ULL + 4096ULL * j_; \
            be32(bs + (size_t)n_ * PKT_S + 4, 0x90020000u | (UInt32)(((t_ / 3072) & 0x0F) << 12) | (UInt32)(t_ % 3072)); \
            j_++; } } while (0)
    // (+64 ciclos = 4 vueltas de 16: no cambia el SYT, solo evita negativos)
    ANCLAR(3);
    printf("mboxd: el arranque del DMA duro %u ciclos (%u -> %u); SYT anclado a +3\n",
           cicloCero - cicloAntes, cicloAntes, cicloCero);
    printf("mboxd: en marcha. ciclo cero %u. Ctrl-C para parar.\n", cicloCero);

    // Comprobacion de salud. El aparato entra a veces en un estado mudo en el
    // que acepta todo —ENABLE, canales, router— y no mueve audio ni hacia la
    // captura ni hacia la etapa analogica. Ningun registro lo delata y ningun
    // registro lo arregla: solo apagarlo y encenderlo. Mas vale avisar que medir
    // sobre un aparato asi, que fue lo que me costo la ultima tanda.
    {
        usleep(1500000);   // los flujos tardan en asentarse: medir antes daba falsa alarma
        for (int i = 0; i < NPKT_R; i++) memset(br + (size_t)i * PKT_R, 0, 12);
        float pk = 0; int sl = 0;
        struct timespec t0, tn;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        do {
            UInt8 *p = br + (size_t)sl * PKT_R;
            UInt32 c0 = ((UInt32)p[4]<<24)|((UInt32)p[5]<<16)|((UInt32)p[6]<<8)|p[7];
            UInt32 c1 = ((UInt32)p[8]<<24)|((UInt32)p[9]<<16)|((UInt32)p[10]<<8)|p[11];
            UInt32 ih = ((UInt32)p[3]<<24)|((UInt32)p[2]<<16)|((UInt32)p[1]<<8)|p[0];
            unsigned len = (ih >> 16) & 0xFFFF;
            if (((c0 >> 16) & 0xFF) == 9 && ((c1 >> 24) & 0x3F) == 0x10 && len > 8 && len <= PKT_R - 4) {
                unsigned bl = (len - 8) / 36;
                for (unsigned b = 0; b < bl; b++)
                    for (int c = 0; c < 8; c++) {
                        UInt8 *q = p + 12 + b*36 + c*4;
                        int v = ((int)q[1]<<16)|((int)q[2]<<8)|q[3];
                        if (v & 0x800000) v -= 0x1000000;
                        float a = (v < 0 ? -v : v) / 8388608.0f;
                        if (a > pk) pk = a;
                    }
                memset(p, 0, 12);
                sl = (sl + 1) % NPKT_R;
            } else { struct timespec d = {0, 50000}; nanosleep(&d, NULL); }
            clock_gettime(CLOCK_MONOTONIC, &tn);
        } while ((tn.tv_sec - t0.tv_sec) + (tn.tv_nsec - t0.tv_nsec)/1e9 < 2.0);
        if (pk > 0) printf("mboxd: aparato SANO (la captura trae %.1f dBFS)\n", 20*log10(pk));
        else {
            // Ya no es alarma: con las entradas sin nada conectado la captura es
            // cero exacto. Lo que dice si la Mbox esta bien es el enganche del
            // receptor (EXT_STATUS 0x40), que se comprueba justo despues.
            printf("mboxd: entradas en silencio (normal si no hay nada conectado)\n");
        }
        primera = 1; slot = 0;
    }
    if (!soloRx) {
        static const int pruebas[] = { 3, 1, -1, 5, -3, 8, -5, 12, -8, 16, 2, 0, 4, -2, 6, 10 };
        int enganchado = 0;
        // Tras despertar el Mac la Mbox tarda en aceptar el flujo: una vuelta
        // completa de pruebas no bastaba y el ayudante salia, lo que costaba un
        // segundo corte (y un segundo pausa-play en el navegador). Ahora se dan
        // hasta tres vueltas, con una pausa entre ellas, sin salir.
        for (int vuelta = 0; vuelta < 3 && !enganchado && !gParar; vuelta++) {
        if (vuelta) { fprintf(stderr, "mboxd: sin enganche; espero y repito las pruebas (vuelta %d)\n", vuelta + 1); sleep(2); }
        for (unsigned k = 0; k < sizeof pruebas / sizeof pruebas[0]; k++) {
            if (k || vuelta) { ANCLAR(pruebas[k]); }
            for (int i = 0; i < 6 && !enganchado; i++) {
                usleep(100000);
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
                if (R(G + 0x58) & 0x40) enganchado = 1;
            }
            printf("mboxd: SYT a %+d ciclos -> receptor de la Mbox %s\n", pruebas[k],
                   enganchado ? "ENGANCHADO" : "sin enganchar");
            if (enganchado) break;
        }
        }
        // Si no engancha, salir y que launchd relance. Medido el 9 oct al
        // despertar el Mac: reintentar en el MISMO proceso (tres vueltas) no
        // engancha nunca, y un proceso nuevo engancha a la primera.
        if (!enganchado) Fallo(3, "el receptor de la Mbox no engancha con ningun SYT");
        for (int i = 0; i < NPKT_R; i++) memset(br + (size_t)i * PKT_R, 0, 12);
        primera = 1; slot = 0;
    }

    // --- bucle ----------------------------------------------------------------
    static const unsigned short kVigila[] = { 0xB028, 0xB129, 0x2040, 0x2141, 0x2850, 0x2951 };
    enum { NVIGILA = 6 };
    unsigned picoRuta[NVIGILA] = { 0 }; unsigned picoK = 0;
    unsigned long sinEscribir = 0, resincros = 0, escriturasAntes = 0;
    long long margenMin = 1LL << 40; int segAlto = 0; unsigned long ajustes = 0;
    #define MARGEN_OBJ 256
    unsigned rafaga = 0, peorRafaga = 0;
    long long derMin = 1LL << 40, derMax = -(1LL << 40), distMin = 999;
    UInt64 capReal = 0, pubMarco = 0; int lineaLista = 0; long long kLinea = 0;
    unsigned long long cicTot = 0; unsigned cicPrev = 0; int cicIniciado = 0; UInt64 hCic0 = 0;
    double tpmBus = tpmGlobal; unsigned long saltosCiclo = 0; UInt32 volRueda = 0;
    unsigned dbcEsp = 0;
    unsigned rellenoPkt = 0;
    UInt64 marcoRepro = 0;
    unsigned long vueltas = 0;
    float picoEnviado = 0, picoCapturado = 0, picoToma = 0;
    struct timespec dormir = { 0, 10000 };
    time_t ultimoInforme = time(NULL);

    UInt64 tPrev = mach_absolute_time(); double peorPausa = 0, durEstado = 0;
    signal(SIGUSR1, alVolcar);
    // MBOX_QUIETO=1 (asi lo lanza launchd): el arranque queda en el registro,
    // pero la linea de estado de cada segundo no, que si no el registro crece
    // sin parar. Los avisos importantes van por stderr y si quedan.
    if (getenv("MBOX_QUIETO") && atoi(getenv("MBOX_QUIETO"))) {
        fprintf(stderr, "mboxd: en marcha (modo silencioso)\n");
        fflush(stdout); freopen("/dev/null", "w", stdout);
    }
    while (!gParar) {
        if (gVolcar == 1) { gGrabK = 0; gVolcar = 2; }
        if (gVolcar == 2 && gGrabK >= GRAB_N) {
            FILE *f = fopen("/tmp/mbox-enviado.raw", "wb"); fwrite(gGrab, 4, GRAB_N, f); fclose(f);
            gVolcar = 0; printf("\nmboxd: volcado lo enviado en /tmp/mbox-enviado.raw\n");
        }
        { UInt64 t = mach_absolute_time(); double ms = (double)(t - tPrev) / (tpmGlobal * 48.0);
          if (ms > peorPausa) peorPausa = ms; tPrev = t; }
        if ((++vueltas & 0xFF) == 0) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
            // Medidor del propio aparato: la seccion de picos lleva una entrada
            // por ruta, ruta en los 16 bits bajos y nivel en los altos. Se lee
            // una entrada por vez para no frenar el relleno del envio.
            // MBOX_RUEDA=1: vigilante. Recorre la seccion de aplicacion (20) y el
            // mezclador (289), una celda por vez, y avisa de cualquier cambio.
            if (gRueda == 2) {
                static UInt32 ant = 0xFFFFFFFF; static UInt64 t0r = 0;
                UInt32 nv = R(appV + 0x08);
                if (!t0r) t0r = mach_absolute_time();
                if (nv != ant) printf("\n  rueda t=%7.3f s  +0x08 = 0x%02X (%d)", (double)(mach_absolute_time() - t0r) / (tpmGlobal * 48000.0), nv, (int)(signed char)nv);
                ant = nv;
            } else if (gRueda) {
                static UInt32 prev[20 + 289]; static int lleno[20 + 289]; static int z = 0;
                UInt64 a = z < 20 ? appV + 4 * (UInt64)z : mix + 4 * (UInt64)(z - 20);
                UInt32 nv = R(a);
                if (lleno[z] && nv != prev[z]) {
                    if (z < 20) printf("\n  >>> CAMBIO aplicacion +0x%02X: 0x%X -> 0x%X", z * 4, prev[z], nv);
                    else { int q = z - 21; if (q < 0) printf("\n  >>> CAMBIO mezclador cabecera: 0x%X -> 0x%X", prev[z], nv);
                           else printf("\n  >>> CAMBIO mezclador salida %d <- entrada %d: 0x%X -> 0x%X", q / 18, q % 18, prev[z], nv); }
                }
                prev[z] = nv; lleno[z] = 1; z = (z + 1) % (20 + 289);
            }
            UInt32 v = gMedidor ? R(pico + 4 * (UInt64)picoK) : 0;
            unsigned ruta = v & 0xFFFF, niv = v >> 16;
            for (int q = 0; q < NVIGILA; q++)
                if (ruta == kVigila[q] && niv > picoRuta[q]) picoRuta[q] = niv;
            picoK = (picoK + 1) % NROUTER;
        }

        // 1. drenar lo recibido hacia el anillo de captura
        int hubo = 0;
        for (int tope = 0; tope < 64; tope++) {
            UInt8 *p = br + (size_t)slot * PKT_R;
            UInt32 c0 = ((UInt32)p[4]<<24)|((UInt32)p[5]<<16)|((UInt32)p[6]<<8)|p[7];
            UInt32 c1 = ((UInt32)p[8]<<24)|((UInt32)p[9]<<16)|((UInt32)p[10]<<8)|p[11];
            unsigned dbs = (c0 >> 16) & 0xFF, dbcR = c0 & 0xFF, fmt = (c1 >> 24) & 0x3F;
            UInt32 ih = ((UInt32)p[3]<<24)|((UInt32)p[2]<<16)|((UInt32)p[1]<<8)|p[0];
            unsigned len = (ih >> 16) & 0xFFFF;
            if (dbs != 9 || fmt != 0x10 || len < 8 || len > PKT_R - 4) break;
            // Solo se consume si el DMA YA escribio la ranura siguiente: si no,
            // puede estar escribiendo esta todavia. Medido con una rampa: cortes
            // de exactamente 6144 muestras (una vuelta del anillo de recepcion)
            // en la mitad final de algunos paquetes.
            {
                UInt8 *q = br + (size_t)((slot + 1) % NPKT_R) * PKT_R;
                UInt32 n0 = ((UInt32)q[4]<<24)|((UInt32)q[5]<<16)|((UInt32)q[6]<<8)|q[7];
                UInt32 n1 = ((UInt32)q[8]<<24)|((UInt32)q[9]<<16)|((UInt32)q[10]<<8)|q[11];
                if (((n0 >> 16) & 0xFF) != 9 || ((n1 >> 24) & 0x3F) != 0x10) break;
            }
            // La frescura la da la invalidacion, no el DBC: la cabecera se borra
            // al consumir, asi que una ranura valida es, por construccion, nueva.
            // Esperar a que el DBC case era un error: el contador es de 8 bits y
            // da la vuelta, asi que una vuelta distinta parece un salto atras y
            // el bucle se quedaba parado para siempre.
            if (primera) { dbcEsp = dbcR; primera = 0; }
            else if (dbcR != dbcEsp) cab->perdidos++;
            unsigned bl = (len - 8) / (dbs * 4);
            UInt8 *d = p + 12;
            if (bl) {   // SYT del aparato: tiempo de bus exacto de cada paquete
                unsigned sy = c1 & 0xFFFF;
                if (sy != 0xFFFF) {
                    long long tk = ((sy >> 12) & 0xF) * 3072 + (sy & 0xFFF);
                    if (gSytPrev >= 0) { gSytTicks += (tk - gSytPrev + 49152) % 49152; gSytMarcos += bl; }
                    gSytPrev = tk;
                }
            }
            UInt64 mc = capReal;
            for (unsigned b = 0; b < bl; b++) {
                float *o = anCap + ((size_t)((mc + b) & MBOX_MASCARA) * MBOX_CANALES);
                for (int c = 0; c < MBOX_CANALES; c++) {
                    UInt8 *q = d + (size_t)b * dbs * 4 + (size_t)c * 4;
                    int v = ((int)q[1] << 16) | ((int)q[2] << 8) | q[3];
                    if (v & 0x800000) v -= 0x1000000;
                    o[c] = (float)v * (1.0f / 8388608.0f);
                    { float a = o[c] < 0 ? -o[c] : o[c];
                      if (a > picoCapturado) picoCapturado = a;
                      if ((c == 0 || c == 1) && a > picoToma) picoToma = a; }
                }
            }
            capReal = mc + bl;
            rafaga += bl;
            // RELOJ PARA COREAUDIO. Antes se publicaba (capturados, hora de
            // llegada) en cada paquete. Pero la captura llega a rafagas de
            // hasta 6 ms, asi que ese reloj temblaba decenas de ms; CoreAudio
            // escribia en el anillo donde no tocaba: huecos (ceros) y zonas
            // escritas dos veces (saltos). Medido en lo ENVIADO: 10% de ceros.
            //
            // Ahora: una recta muestras->hora que sigue la ENVOLVENTE INFERIOR
            // de las llegadas (una rafaga solo puede llegar tarde, nunca antes),
            // y se publica en pasos exactos de MBOX_PERIODO, el periodo que el
            // plugin declara a CoreAudio.
            // RELOJ PARA COREAUDIO, desde el BUS.
            // La Mbox esta clavada al reloj del bus (0,0 ppm por sus SYT) y al
            // bus van exactamente 6 muestras por ciclo; el envio se rellena a
            // ese mismo ritmo. Asi que la recta es: muestra = 6 x ciclo + K,
            // con K la envolvente inferior de (capturadas - 6 x ciclo): una
            // rafaga de captura solo puede llegar tarde. Antes se sellaba cada
            // paquete con su hora de llegada y el reloj temblaba decenas de ms:
            // CoreAudio escribia huecos y repeticiones (10% de ceros, medido).
            if (bl) {
                long long kObs = (long long)capReal - 6LL * (long long)cicTot;
                if (!lineaLista || kObs < kLinea) { kLinea = kObs; }
                if (!lineaLista) { lineaLista = 1; pubMarco = (UInt64)(6LL * (long long)cicTot + kLinea); }
            }
            dbcEsp = (dbcR + bl) & 0xFF;
            memset(p, 0, 12);
            slot = (slot + 1) % NPKT_R;
            hubo = 1;
        }

        if (rafaga > peorRafaga) peorRafaga = rafaga;
        rafaga = 0;
        {   // ciclos de bus acumulados y su relacion con el reloj del Mac
            unsigned k = CicloAhora(); UInt64 hNow = mach_absolute_time();
            if (!cicIniciado) { cicPrev = k; cicIniciado = 1; hCic0 = hNow; }
            { unsigned dc = (k + 1024000u - cicPrev) % 1024000u;
              if (dc < 8000u) cicTot += dc; else saltosCiclo++;   // mas de 1 s en una vuelta: lectura mala
              cicPrev = k; }
            if (cicTot > 16000) tpmBus = (double)(hNow - hCic0) / (6.0 * (double)cicTot);
            if (lineaLista) {
                long long ahoraLinea = 6LL * (long long)cicTot + kLinea;
                if (ahoraLinea >= (long long)pubMarco + MBOX_PERIODO || !cab->anclaHost) {
                    if (cab->anclaHost) pubMarco += MBOX_PERIODO;
                    cab->anclaHost = (UInt64)((double)hNow - (double)(ahoraLinea - (long long)pubMarco) * tpmBus);
                    cab->marcosCapturados = pubMarco;
                }
            }
        }
        {   static UInt64 tR = 0; static UInt32 rAnt = 0xFFFFFFFF;
            UInt64 tn = mach_absolute_time();
            if (tn - tR > (UInt64)(tpmGlobal * 2400)) {        // 2400 marcos = 50 ms
                tR = tn;
                UInt32 rv = R(appV + 0x08) & 0xFF;
                if (rv != rAnt) { if (gRuedaSoft) AplicarRueda(mix, rv); rAnt = rv; volRueda = rv; }
            }
        }
        // MARGEN: cuantas muestras YA escritas por CoreAudio hay por delante de
        // la que toca leer. Es lo que separa sonar de leer huecos, y es lo que
        // fija la latencia: se gobierna para que ronde MARGEN_OBJ.
        {   static UInt64 tM = 0;
            UInt64 tn = mach_absolute_time();
            if (marcoRepro && tn - tM > (UInt64)(tpmGlobal * 960)) {   // cada 20 ms
                tM = tn;
                long long mg = 0;
                while (mg < 8192) {
                    float v = anRep[(size_t)((marcoRepro + mg) & MBOX_MASCARA) * MBOX_CANALES];
                    if (v != v) break;
                    mg++;
                }
                if (mg < margenMin) margenMin = mg;
            }
        }
        // 2. rellenar el envio por delante del DMA
        unsigned leyendo = (soloRx || tonoFijo) ? 0 : (CicloAhora() - cicloCero) % NPKT_S;
        if (soloRx || tonoFijo) goto saltarEnvio;
        
        for (int tope = 0; tope < NPKT_S; tope++) {
            unsigned dist = (rellenoPkt + NPKT_S - leyendo) % NPKT_S;
            if (dist >= ADELANTO) break;
            if (esDatos[rellenoPkt]) {
                // Si se ha ido la mano respecto al reloj de captura, volver a
                // engancharlo: los dos van al mismo reloj, el del aparato.
                // La muestra que toca a este paquete depende de CUANDO saldra,
                // no de cuando se rellena: sale dentro de 'dist' ciclos, y por
                // ciclo pasan 6 muestras. Sin contar eso, rellenar por delante
                // parecia una deriva y forzaba resincronizaciones (= saltos).
                long long ahoraLinea = lineaLista ? 6LL * (long long)cicTot + kLinea : (long long)capReal;
                long long objetivo = ahoraLinea + gDesfase + (long long)dist * 6;
                if (marcoRepro == 0) marcoRepro = (UInt64)objetivo;
                long long deriva = (long long)marcoRepro - objetivo;
                if (deriva < derMin) derMin = deriva;
                if (deriva > derMax) derMax = deriva;
                if ((long long)dist < distMin) distMin = dist;
                // La captura llega a rafagas de hasta 384 marcos (medido), asi que la
                // referencia tiembla eso. Solo se corrige una deriva de verdad.
                // Con la recta sacada del bus la deriva real varia ~60 muestras; el
                // umbral de 12000 era del reloj tembloroso. 512 recoloca la lectura
                // una vez al arrancar, cuando la envolvente se asienta.
                if (deriva > 512 || deriva < -512) { marcoRepro = (UInt64)objetivo; resincros++; }
                UInt8 *p = bs + (size_t)rellenoPkt * PKT_S;
                for (int k = 0; k < BLOQUES; k++) {
                    float *s = anRep + ((size_t)((marcoRepro + k) & MBOX_MASCARA) * MBOX_CANALES);
                    UInt8 *d = p + 8 + (size_t)k * DBS * 4;
                    // NaN = muestra ya consumida y no reescrita: CoreAudio no ha
                    // llegado aqui (o no esta reproduciendo). Silencio, nunca
                    // restos viejos.
                    if (s[0] != s[0]) sinEscribir++;
                    for (int c = 0; c < 8; c++) {
                        float x = s[c];
                        if (x != x) x = 0.0f;
                        if (c == 0 && gGrabK < GRAB_N) gGrab[gGrabK++] = x;
                        s[c] = NAN;
                        if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
                        float a = x < 0 ? -x : x;
                        if (a > picoEnviado) picoEnviado = a;
                        int v = (int)(x * 8388607.0f);
                        d[c*4+1] = (v >> 16) & 0xFF; d[c*4+2] = (v >> 8) & 0xFF; d[c*4+3] = v & 0xFF;
                    }
                }
                marcoRepro += BLOQUES;
                cab->marcosReproducidos = marcoRepro;
            }
            rellenoPkt = (rellenoPkt + 1) % NPKT_S;
        }

    saltarEnvio:
        cab->latido++;
        if (!hubo) nanosleep(&dormir, NULL);

        // Deteccion de parada. El aparato puede dejar de transmitir sin que el
        // sistema avise: no hay parada forzada ni error de bus. Si eso pasa hay
        // que dejar de publicar un ancla rancia, porque CoreAudio construye su
        // reloj con ella, y probar a rearmar.
        {
            static UInt64 ultimoDato = 0;
            UInt64 ahora = mach_absolute_time();
            if (hubo) ultimoDato = ahora;
            else if (ultimoDato) {
                double quieto = (double)(ahora - ultimoDato) / (48000.0 * tpmGlobal);
                if (quieto > 0.2 && cab->anclaHost) {
                    cab->anclaHost = 0;        // el plugin lo dara por muerto
                    fprintf(stderr, "\nmboxd: el flujo se paro\n");
                }
                // Rearmar en caliente no era fiable. Salir limpio y dejar que
                // launchd relance: el arranque completo si esta probado.
                if (quieto > 2.0) Fallo(3, "el flujo de la Mbox lleva 2 s parado");
            }
        }

        time_t ahora = time(NULL);
        if (ahora != ultimoInforme) {
            UInt64 tE0 = mach_absolute_time();
            ultimoInforme = ahora;
            if (getenv("MBOX_DIAG")) {
                printf("\n  diag: slot=%d vueltas=%lu  ENABLE=0x%X STATUS=0x%X TX=%d RX=%d\n",
                       slot, vueltas, R(G+0x50), R(G+0x54), (int)(SInt32)R(T+8), (int)(SInt32)R(X+8));
                for (int k = 0; k < 4; k++) {
                    UInt8 *q = br + (size_t)((slot + k) % NPKT_R) * PKT_R;
                    printf("        ranura %4d: ", (slot + k) % NPKT_R);
                    for (int z = 0; z < 12; z++) printf("%02X ", q[z]);
                    printf("\n");
                }
            }
            {   // Reloj del aparato frente al reloj del bus.
                static UInt64 c0 = 0; static unsigned k0 = 0; static unsigned long long ciclos = 0; static unsigned kPrev = 0;
                unsigned k = CicloAhora();
                if (!c0) { c0 = capReal; k0 = k; kPrev = k; }
                else {
                    ciclos += (k + 1024000u - kPrev) % 1024000u; kPrev = k;
                    double seg = ciclos / 8000.0, tasa = (capReal - c0) / seg;
                    static UInt64 hIni = 0; static unsigned long long cicIni = 0;
                    if (!hIni) { hIni = mach_absolute_time(); cicIni = ciclos; }
                    double segHost = (double)(mach_absolute_time() - hIni) / (tpmGlobal * 48000.0);
                    double cicPorSeg = segHost > 0 ? (ciclos - cicIni) / segHost : 0;
                    printf("\n    bus frente a Mac: %.3f ciclos por segundo del Mac (%+.1f ppm); muestras capturadas por segundo del Mac %.2f",
                           cicPorSeg, (cicPorSeg / 8000.0 - 1) * 1e6, segHost > 1 ? (capReal - c0) / ((double)(mach_absolute_time() - hIni) / (tpmGlobal * 48000.0) + seg - seg) : 0);
                    double tasaSyt = gSytTicks ? gSytMarcos / (gSytTicks / 24576000.0) : 0;
                    printf("\n    reloj: por SYT del aparato %.3f Hz (%+.1f ppm); por conteo %.1f en %.0f s",
                           tasaSyt, (tasaSyt / 48000.0 - 1) * 1e6, tasa, seg);
                }
            }
            printf("\n    rueda de monitor %u/255 -> %.1f dB", volRueda, volRueda ? -40.0 * pow(1.0 - (double)volRueda / 255.0, 1.5) : -999.0);
            // Los registros se releen cada 5 s: son lecturas sincronas y con el
            // relleno a solo 8 ms por delante del DMA no conviene frenar el bucle.
            static UInt32 regE[6]; static int cuentaE = 0;
            {   // una lectura por segundo, por turnos: seis de golpe tardaban 6,6 ms
                const UInt64 dir[6] = { G+0x50, G+0x54, G+0x58, X+8, T+8, cur };
                regE[cuentaE % 6] = R(dir[cuentaE % 6]); cuentaE++; }
            printf("\r  ENABLE=%X STATUS=%04X EXT=%08X RX=%d TX=%d VIG=%u | capt %llu  repr %llu  perd %llu   pico ENVIADO %6.1f dBFS   pico capt %6.1f  TOMA %6.1f  fallos E/S %lu/%lu   ",
                   regE[0], regE[1], regE[2], (int)(SInt32)regE[3], (int)(SInt32)regE[4], regE[5],
                   (unsigned long long)capReal,
                   (unsigned long long)cab->marcosReproducidos,
                   (unsigned long long)cab->perdidos,
                   picoEnviado > 0 ? 20*log10(picoEnviado) : -999.0,
                   picoCapturado > 0 ? 20*log10(picoCapturado) : -999.0,
                   picoToma > 0 ? 20*log10(picoToma) : -999.0,
                   gFallosLectura + gFallosEscritura, gLecturas);
            if (gFallosLectura > 20) {
                fprintf(stderr, "\n*** mboxd: el aparato dejo de contestar (%lu lecturas fallidas).\n"
                                "*** La pila FireWire del Mac puede estar colgada: mira\n"
                                "***   ioreg -c AppleFWOHCI -r -d1\n"
                                "*** y si sale 'inactive, busy', hay que REINICIAR EL MAC.\n", gFallosLectura);
                Fallo(3, "la Mbox dejo de contestar");
            }
            if (gMedidor) { UInt64 app = Sec(8);
              printf("\n    app: vol(+08)=%X modo=%X LED=%X DeviceMode=%X DAC1-6=%X %X %X %X %X %X",
                     R(app+0x08), R(app), R(app+0x14), R(app+0x10), R(app+0x28), R(app+0x2C), R(app+0x30), R(app+0x34), R(app+0x38), R(app+0x3C)); }
            if (getenv("MBOX_LED_CICLO")) {
                static int seg = 0, sel = 1;
                if (++seg % 10 == 0) {
                    sel = sel % 3 + 1;
                    W(Sec(8) + 0x14, 0x101u | ((UInt32)sel << 5));
                }
                printf("\n    >>> SELECCION DE PARLANTES %d (%s)", sel, sel == 1 ? "VERDE" : sel == 2 ? "ROJO" : "AMBAR");
            }
            {   // Si CoreAudio esta escribiendo y aun asi se leen huecos, leer mas atras.
                int activo = cab->pluginEscrituras != escriturasAntes;
                escriturasAntes = cab->pluginEscrituras;
                // Gobierno del margen: si se queda corto, retroceder; si sobra
                // durante 3 s seguidos, avanzar. Cada ajuste es un salto audible
                // una sola vez; despues queda quieto mientras no cambie el bufer.
                if (activo && margenMin < (1LL << 40)) {
                    long long cambio = 0;
                    if (margenMin < 96) { cambio = margenMin - MARGEN_OBJ; segAlto = 0; }
                    else if (margenMin > 3 * MARGEN_OBJ) { if (++segAlto >= 3) { cambio = margenMin - MARGEN_OBJ; segAlto = 0; } }
                    else segAlto = 0;
                    if (cambio > 0)   // lo que se salta queda marcado como consumido
                        for (long long q = 0; q < cambio; q++)
                            for (int c = 0; c < MBOX_CANALES; c++)
                                anRep[(size_t)((marcoRepro + q) & MBOX_MASCARA) * MBOX_CANALES + c] = NAN;
                    if (cambio) { marcoRepro += cambio; gDesfase += cambio; ajustes++; }
                }
                // Publicar para CoreAudio. Salida: lo que se lee por detras de la
                // linea mas ~5 ciclos de SYT y la conversion (estimado, 64).
                // Entrada: conversion y transporte (estimado, 64). Pendiente
                // medirlas con un cable de salida a entrada.
                { long long ls = -gDesfase + 64; cab->latSalida = (uint32_t)(ls < 64 ? 64 : ls); cab->latEntrada = 64; }
                printf("\n    LATENCIA de salida ~%.1f ms (desfase %lld) | margen minimo %lld muestras | ajustes %lu",
                       (-gDesfase + 40) / 48.0, gDesfase, margenMin < (1LL << 40) ? margenMin : -1, ajustes);
                margenMin = 1LL << 40;
                printf("\n    anillo: desfase %lld, muestras sin escribir %lu, resincronizaciones %lu, peor rafaga de captura %u marcos%s",
                       gDesfase, sinEscribir, resincros, peorRafaga, activo ? "" : "  (CoreAudio no escribe)");
                if (saltosCiclo) printf("  [lecturas de ciclo descartadas: %lu]", saltosCiclo);
                printf("\n    deriva min %lld max %lld, distancia minima al DMA %lld paquetes | peor pausa del bucle %.1f ms, linea de estado %.1f ms",
                       derMin, derMax, distMin, peorPausa, durEstado);
                peorPausa = 0;
                derMin = 1LL << 40; derMax = -(1LL << 40); distMin = 999;
                peorRafaga = 0;
                
                sinEscribir = 0; resincros = 0;
            }
            printf("\n    picos del aparato: ordenador->mezcla %04X %04X | mezcla->lineas1/2 %04X %04X | mezcla->auriculares %04X %04X\n",
                   picoRuta[0], picoRuta[1], picoRuta[2], picoRuta[3], picoRuta[4], picoRuta[5]);
            for (int q = 0; q < NVIGILA; q++) picoRuta[q] = 0;
            picoEnviado = 0; picoCapturado = 0; picoToma = 0;
            durEstado = (double)(mach_absolute_time() - tE0) / (tpmGlobal * 48.0);
            tPrev = mach_absolute_time();
        }
    }

    printf("\nmboxd: parando\n");
    cab->magia = 0; cab->anclaHost = 0;
    // ORDEN DE APAGADO. Importa, y lo tenia al reves.
    //
    // Los manejadores de liberacion de los puertos ponen los canales isocronos a
    // -1, y se ejecutan dentro de ReleaseChannel. Haciendolo antes de apagar
    // ENABLE, al aparato se le quita el canal por debajo con los flujos todavia
    // activos. El patron de todo el dia encaja: funciona recien encendido,
    // aguanta una o dos ejecuciones y se queda mudo, y entre medias siempre esta
    // esta secuencia.
    //
    // La forma correcta en DICE es al reves: primero ENABLE=0, que para los
    // flujos del aparato, y solo despues desmontar el lado del anfitrion.
    W(G + 0x50, 0);                      // 1. parar los flujos del aparato
    usleep(150000);
    if (!soloRx) (*canS)->Stop(canS);    // 2. parar el DMA del anfitrion
    (*canR)->Stop(canR); usleep(300000);
    if (!soloRx) (*canS)->ReleaseChannel(canS);   // 3. soltar canales (ponen -1)
    (*canR)->ReleaseChannel(canR); usleep(200000);
    W(X + 8, 0xFFFFFFFF); W(T + 8, 0xFFFFFFFF);   // 4. por si acaso
    W(G + 0x50, 0);
    SoltarDueno();
    shm_unlink(MBOX_SHM_NOMBRE);
    printf("mboxd: aparato en reposo, compartida liberada\n");
    return gCodigoSalida;
}
