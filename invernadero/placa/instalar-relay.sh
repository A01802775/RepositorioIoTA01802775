#!/bin/bash
# Instala el repetidor MQTT como servicio (arranca solo con la placa).
# Uso (en la placa):  sudo bash ~/relay/instalar-relay.sh
set -e
cd "$(dirname "$0")"

if ! command -v socat >/dev/null; then
    echo "Falta socat, instalando..."
    apt-get install -y socat
fi

install -m 755 mqtt-relay-destino /usr/local/bin/mqtt-relay-destino
install -m 644 mqtt-relay.service /etc/systemd/system/mqtt-relay.service
systemctl daemon-reload
systemctl enable --now mqtt-relay.service
systemctl restart mqtt-relay.service
sleep 1

echo "------------------------------------------------------------"
echo "Servicio: $(systemctl is-active mqtt-relay.service)"
LEASES=/var/lib/NetworkManager/dnsmasq-usb0.leases
if [ -r "$LEASES" ]; then
    echo "Compu detectada por USB: $(sort -n "$LEASES" | tail -n 1 | awk '{print $3}')"
else
    echo "Aviso: no existe $LEASES; se usará la tabla de vecinos (ip neigh)"
fi
if timeout 5 bash -c '</dev/tcp/127.0.0.1/1883' 2>/dev/null; then
    echo "RELAY_OK: el repetidor escucha en el puerto 1883"
else
    echo "RELAY_FALLA: revisa con  journalctl -u mqtt-relay -n 30"
fi
