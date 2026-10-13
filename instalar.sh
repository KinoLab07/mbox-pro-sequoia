#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 KinoLab07
# Instala el driver de la Avid Mbox Pro (3a generacion, FireWire) para macOS
# Sequoia y anteriores con pila FireWire, SIN desactivar SIP y sin cuenta de
# desarrollador de Apple.
#
#   ./instalar.sh
#
# Compila aqui mismo (hacen falta las herramientas de linea de comandos de
# Apple), y pide la contrasena de administrador para copiar tres piezas:
#
#   /Library/Audio/Plug-Ins/HAL/MboxPro.driver      el aparato de audio de macOS
#   /Library/Application Support/MboxPro/mboxd       el ayudante que habla FireWire
#   /Library/LaunchAgents/lab.remedia.mboxd.plist    lo arranca al iniciar sesion
#
# Para quitarlo todo: ./desinstalar.sh
set -e
cd "$(dirname "$0")"
AQUI="$PWD"

ROJO=$'\e[31m'; VERDE=$'\e[32m'; NEGRITA=$'\e[1m'; NORMAL=$'\e[0m'
paso() { print "\n${NEGRITA}== $1${NORMAL}"; }
mal()  { print "${ROJO}ERROR: $1${NORMAL}"; exit 1; }
bien() { print "${VERDE}   $1${NORMAL}"; }

HAL="/Library/Audio/Plug-Ins/HAL"
APOYO="/Library/Application Support/MboxPro"
AGENTE="/Library/LaunchAgents/lab.remedia.mboxd.plist"
ETIQUETA="lab.remedia.mboxd"

[[ $EUID -eq 0 ]] && mal "no lo lances con sudo; ya pedira la contrasena cuando haga falta."

# ---------------------------------------------------------------------------
paso "1. Comprobando el Mac"
print "   macOS $(sw_vers -productVersion) ($(uname -m))"
if ! ioreg -r -c AppleFWOHCI -d1 2>/dev/null | grep -q AppleFWOHCI; then
  mal "este Mac no tiene pila FireWire activa.
   - Si es macOS 26 Tahoe o posterior: Apple la retiro; hace falta el driver ASFireWire.
   - Si no: conecta el adaptador FireWire (Thunderbolt) y vuelve a intentarlo."
fi
bien "pila FireWire presente"

if ! xcode-select -p >/dev/null 2>&1 || ! command -v clang >/dev/null 2>&1; then
  print "   Faltan las herramientas de linea de comandos de Apple. Abro su instalador;"
  print "   cuando termine, vuelve a lanzar ./instalar.sh"
  xcode-select --install 2>/dev/null || true
  exit 1
fi
bien "herramientas de compilacion: $(clang --version | head -1)"

# ---------------------------------------------------------------------------
paso "2. Compilando"
OBRA="$(mktemp -d /tmp/mboxpro-obra.XXXXXX)"
trap 'rm -rf "$OBRA"' EXIT

clang -O2 -o "$OBRA/mboxd" fuente/mboxd.c \
      -framework IOKit -framework CoreFoundation 2> "$OBRA/avisos-mboxd.txt" \
  || { cat "$OBRA/avisos-mboxd.txt"; mal "no compilo el ayudante"; }
codesign --force --sign - "$OBRA/mboxd" 2>/dev/null
bien "ayudante (mboxd)"

B="$OBRA/MboxPro.driver"
mkdir -p "$B/Contents/MacOS"
clang -O2 -bundle -o "$B/Contents/MacOS/MboxPro" fuente/MboxProHAL.c \
      -framework CoreFoundation -framework CoreAudio -framework CoreServices 2> "$OBRA/avisos-hal.txt" \
  || { cat "$OBRA/avisos-hal.txt"; mal "no compilo el plugin del HAL"; }
cat > "$B/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>        <string>English</string>
    <key>CFBundleExecutable</key>               <string>MboxPro</string>
    <key>CFBundleIdentifier</key>               <string>lab.remedia.mboxpro.hal</string>
    <key>CFBundleName</key>                     <string>MboxPro</string>
    <key>CFBundlePackageType</key>              <string>BNDL</string>
    <key>CFBundleShortVersionString</key>       <string>1.0</string>
    <key>CFBundleVersion</key>                  <string>1</string>
    <key>CFPlugInFactories</key>
    <dict>
        <key>ACFA171D-14F2-40DF-9C71-48505D9EE1B4</key>
        <string>MboxProCrear</string>
    </dict>
    <key>CFPlugInTypes</key>
    <dict>
        <key>443ABAB8-E7B3-491A-B985-BEB9187030DB</key>
        <array><string>ACFA171D-14F2-40DF-9C71-48505D9EE1B4</string></array>
    </dict>
</dict>
</plist>
PLIST
# coreaudiod admite plugins con firma ad hoc (trae clear-library-validation).
codesign --force --sign - "$B" 2>/dev/null
bien "plugin del HAL (MboxPro.driver)"

# El agente: arranca el ayudante al iniciar sesion y lo relanza si sale (Mbox
# apagada, reinicio del bus, el Mac que se duerme...). El registro va a
# ~/Library/Logs/MboxPro.log.
cat > "$OBRA/agente.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>                 <string>$ETIQUETA</string>
    <key>ProgramArguments</key>
    <array>
        <string>/bin/zsh</string>
        <string>-c</string>
        <string>mkdir -p "\$HOME/Library/Logs"; exec "$APOYO/mboxd" &gt;&gt; "\$HOME/Library/Logs/MboxPro.log" 2&gt;&amp;1</string>
    </array>
    <key>EnvironmentVariables</key>
    <dict>
        <key>MBOX_QUIETO</key>       <string>1</string>
    </dict>
    <key>RunAtLoad</key>             <true/>
    <key>KeepAlive</key>             <true/>
    <key>ThrottleInterval</key>      <integer>5</integer>
    <key>ProcessType</key>           <string>Interactive</string>
    <key>LimitLoadToSessionType</key><string>Aqua</string>
</dict>
</plist>
PLIST
plutil -lint "$OBRA/agente.plist" >/dev/null || mal "el agente salio mal formado"
bien "agente de inicio de sesion"

# ---------------------------------------------------------------------------
paso "3. Parando una version anterior, si la hay"
launchctl bootout "gui/$(id -u)/$ETIQUETA" 2>/dev/null && bien "agente anterior parado" || true
if pgrep -x mboxd >/dev/null; then
  pkill -INT -x mboxd || true; sleep 3; pkill -9 -x mboxd 2>/dev/null || true
  bien "ayudante anterior parado"
fi

# ---------------------------------------------------------------------------
paso "4. Instalando (pide la contrasena de administrador)"
sudo /bin/zsh -c "
  set -e
  mkdir -p '$HAL' '$APOYO'
  rm -rf '$HAL/MboxPro.driver'
  cp -R '$B' '$HAL/'
  chown -R root:wheel '$HAL/MboxPro.driver'
  cp '$OBRA/mboxd' '$APOYO/mboxd'
  chown root:wheel '$APOYO/mboxd'; chmod 755 '$APOYO/mboxd'
  cp '$OBRA/agente.plist' '$AGENTE'
  chown root:wheel '$AGENTE'; chmod 644 '$AGENTE'
  killall coreaudiod 2>/dev/null || true
" || mal "la instalacion con sudo fallo"
bien "plugin en $HAL/MboxPro.driver"
bien "ayudante en $APOYO/mboxd"
bien "agente en $AGENTE"
bien "audio del Mac reiniciado"

# ---------------------------------------------------------------------------
paso "5. Arrancando el ayudante"
sleep 3
launchctl bootstrap "gui/$(id -u)" "$AGENTE" 2>/dev/null || launchctl kickstart -k "gui/$(id -u)/$ETIQUETA"
sleep 12
if pgrep -x mboxd >/dev/null; then bien "ayudante en marcha"; else print "${ROJO}   el ayudante no esta corriendo; mira ./estado.sh${NORMAL}"; fi
print "\n   Ultimas lineas del registro (~/Library/Logs/MboxPro.log):"
tail -n 12 "$HOME/Library/Logs/MboxPro.log" 2>/dev/null | sed 's/^/     /'

print "\n${NEGRITA}Listo.${NORMAL}"
print "  - Elige \"Mbox Pro (FireWire)\" como salida en Ajustes del Sistema > Sonido."
print "  - La rueda de monitor es el volumen general: si no suena, comprueba que no este abajo."
print "  - Diagnostico: ./estado.sh     Desinstalar: ./desinstalar.sh"
