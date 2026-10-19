#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Envía un programa a la PS3 real.
#   PS3_IP=x.x.x.x tools/deploy-ps3.sh ps3load build/salsads-empty/salsads-empty.self
#   PS3_IP=x.x.x.x tools/deploy-ps3.sh ftp build/salsads-empty/salsads-empty.pkg
#
# ps3load: la PS3 debe estar corriendo la app ps3load (escucha en TCP 4299);
#          el programa se ejecuta en cuanto llega.
# ftp:     sube a /dev_hdd0/packages/ (si es .pkg, para instalarlo desde
#          Package Manager) o a /dev_hdd0/salsads/, vía el FTP de webMAN MOD.
# PS3_IP también puede ir en tools/ps3.conf (ignorado por git): PS3_IP=...
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ -f "$ROOT/tools/ps3.conf" ] && . "$ROOT/tools/ps3.conf"
: "${PS3_IP:?definir PS3_IP (o tools/ps3.conf)}"

[ $# -eq 2 ] || { sed -n '2,12p' "$0"; exit 1; }
METHOD=$1
FILE=$2
[ -f "$FILE" ] || { echo "no existe $FILE" >&2; exit 1; }

case "$METHOD" in
ps3load)
	# ps3load (host) viene con PSL1GHT dentro de la imagen.
	REL=$(cd "$(dirname "$FILE")" && pwd)/$(basename "$FILE")
	REL=${REL#"$ROOT"/}
	"$ROOT/tools/build.sh" --sh "PS3LOAD=tcp:$PS3_IP ps3load '$REL'"
	;;
ftp)
	case "$FILE" in
	*.pkg) DEST=/dev_hdd0/packages/ ;;
	*) DEST=/dev_hdd0/salsads/ ;;
	esac
	curl --ftp-create-dirs -T "$FILE" "ftp://$PS3_IP$DEST"
	echo "subido a $DEST$(basename "$FILE")"
	;;
*)
	echo "método desconocido: $METHOD (ps3load | ftp)" >&2
	exit 1
	;;
esac
