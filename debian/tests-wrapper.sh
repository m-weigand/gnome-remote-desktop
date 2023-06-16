#!/bin/sh
# Keep this in sync with
# https://gitlab.gnome.org/GNOME/gnome-remote-desktop/-/blob/master/.gitlab-ci/run-tests.sh
set -ex

trap '{ kill -9 $pipewire_pids 2>/dev/null || true; }' EXIT

export HOME="$(mktemp -d --tmpdir grd-home-XXXXXX)"

pipewire &
pipewire_pids=$!
sleep 1

wireplumber &
pipewire_pids="$pipewire_pids $!"
sleep 1

openssl req -new -newkey rsa:4096 -days 720 -nodes -x509 \
            -subj "/C=DE/ST=NONE/L=NONE/O=GNOME/CN=gnome.org" \
            -keyout tls.key -out tls.crt
gsettings set org.gnome.desktop.remote-desktop.rdp tls-cert $(realpath tls.crt)
gsettings set org.gnome.desktop.remote-desktop.rdp tls-key $(realpath tls.key)
gsettings set org.gnome.desktop.remote-desktop.rdp screen-share-mode extend
gsettings set org.gnome.desktop.remote-desktop.rdp enable true

gsettings set org.gnome.desktop.remote-desktop.vnc enable true

$@
