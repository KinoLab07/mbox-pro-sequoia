#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 KinoLab07
# Quita todo lo que instala ./instalar.sh y deja la Mbox en reposo.
cd "$(dirname "$0")"
ETIQUETA="lab.remedia.mboxd"
[[ $EUID -eq 0 ]] && { print "No lo lances con sudo; ya pedira la contrasena."; exit 1; }

print "Parando el ayudante..."
launchctl bootout "gui/$(id -u)/$ETIQUETA" 2>/dev/null || true
if pgrep -x mboxd >/dev/null; then
  pkill -INT -x mboxd; sleep 3; pkill -9 -x mboxd 2>/dev/null
fi

print "Quitando los archivos (pide la contrasena de administrador)..."
sudo /bin/zsh -c "
  rm -rf '/Library/Audio/Plug-Ins/HAL/MboxPro.driver'
  rm -rf '/Library/Application Support/MboxPro'
  rm -f  '/Library/LaunchAgents/$ETIQUETA.plist'
  killall coreaudiod 2>/dev/null
" && print "Desinstalado. El registro queda en ~/Library/Logs/MboxPro.log (puedes borrarlo)."
