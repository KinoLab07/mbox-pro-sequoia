#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 KinoLab07
# Diagnostico: dice que pieza falla. Solo lee, no cambia nada.
ETIQUETA="lab.remedia.mboxd"
ok()  { print "  [ok]  $1"; }
no()  { print "  [!!]  $1"; }

print "== Piezas instaladas"
[[ -f /Library/Audio/Plug-Ins/HAL/MboxPro.driver/Contents/MacOS/MboxPro ]] && ok "plugin del HAL" || no "falta el plugin del HAL: ./instalar.sh"
[[ -x "/Library/Application Support/MboxPro/mboxd" ]] && ok "ayudante" || no "falta el ayudante: ./instalar.sh"
[[ -f "/Library/LaunchAgents/$ETIQUETA.plist" ]] && ok "agente de inicio" || no "falta el agente: ./instalar.sh"

print "\n== Ayudante"
if pgrep -x mboxd >/dev/null; then ok "corriendo (pid $(pgrep -x mboxd))"
else no "no esta corriendo"; fi
launchctl print "gui/$(id -u)/$ETIQUETA" 2>/dev/null | grep -E "state =|runs =|last exit code" | sed 's/^\s*/        /'

print "\n== FireWire"
if ! ioreg -r -c AppleFWOHCI -d1 2>/dev/null | grep -q AppleFWOHCI; then
  no "no hay pila FireWire (macOS 26+ la retiro, o falta el adaptador)"
else
  colgado=0
  for c in AppleFWOHCI IOFireWireController; do
    l=$(ioreg -c $c -r -d1 2>/dev/null | grep -m1 "+-o")
    echo "$l" | grep -q inactive && { no "$c COLGADO: hay que reiniciar el Mac"; colgado=1; }
  done
  [[ $colgado -eq 0 ]] && ok "pila FireWire sana"
  if ioreg -r -c IOFireWireUnit -l 2>/dev/null | grep -q '"Vendor_ID" = 41086'; then ok "Mbox Pro conectada y encendida"
  else no "no veo la Mbox Pro: esta encendida y con el cable puesto?"; fi
fi

print "\n== CoreAudio"
if system_profiler SPAudioDataType 2>/dev/null | grep -q "Mbox Pro (FireWire)"; then
  ok "el aparato \"Mbox Pro (FireWire)\" esta publicado"
  system_profiler SPAudioDataType 2>/dev/null | grep -A3 "Mbox Pro (FireWire):" | grep -i default | sed 's/^\s*/        /'
else no "CoreAudio no publica el aparato: reinstala o reinicia el Mac"; fi

print "\n== Ultimas lineas del registro (~/Library/Logs/MboxPro.log)"
tail -n 20 "$HOME/Library/Logs/MboxPro.log" 2>/dev/null | sed 's/^/   /' || print "   (sin registro)"
print "\nSi no suena: comprueba la rueda de monitor (volumen general). Tras dormir el Mac, recarga la pestaña del navegador."
