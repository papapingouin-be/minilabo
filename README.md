# minilabo

Low tech elec labo firmware for the ESP8266 based MiniLabBox. The web
interface is served over HTTPS using an embedded self-signed
certificate; connect to `https://<node-id>.local` (or the device IP) and
accept the certificate warning in your browser. A plain HTTP endpoint on
port 80 remains available for legacy clients while HTTPS is preferred.
