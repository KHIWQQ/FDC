#!/usr/bin/env bash
# ============================================================
#  make-certs.sh — ออกใบรับรอง mTLS สำหรับ dev/นำร่อง (self-signed CA)
#  งานจริงระดับประเทศ: ใช้ CA ของทหาร/PKI องค์กรแทน อย่าใช้สคริปต์นี้ใน production
#
#  สร้าง:  ca, broker, ingest, และใบของ ศอย. ตามอาร์กิวเมนต์
#  ใช้:    ./make-certs.sh BN1-A/FDC01 BN1-A/FDC02 ...
#         (CN ของ ศอย. ต้องเป็น "<unit>/<node>" ให้ตรง ACL pattern %u)
# ============================================================
set -euo pipefail
cd "$(dirname "$0")"
CERTS=./certs
DAYS=825
mkdir -p "$CERTS"

gen_ca() {
  [ -f "$CERTS/ca.key" ] && { echo "ca มีอยู่แล้ว — ข้าม"; return; }
  openssl genrsa -out "$CERTS/ca.key" 4096
  openssl req -x509 -new -nodes -key "$CERTS/ca.key" -sha256 -days 3650 \
    -subj "/O=FDC/CN=FDC-Central-CA" -out "$CERTS/ca.crt"
  echo "✔ สร้าง CA"
}

# gen_cert <name> <CN> [serverAuth]
gen_cert() {
  local name="$1" cn="$2" server="${3:-}"
  openssl genrsa -out "$CERTS/$name.key" 2048
  openssl req -new -key "$CERTS/$name.key" -subj "/O=FDC/CN=$cn" \
    -out "$CERTS/$name.csr"
  local ext="$CERTS/$name.ext"
  if [ -n "$server" ]; then
    printf "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:%s,DNS:localhost\n" "$cn" > "$ext"
  else
    printf "extendedKeyUsage=clientAuth\n" > "$ext"
  fi
  openssl x509 -req -in "$CERTS/$name.csr" -CA "$CERTS/ca.crt" -CAkey "$CERTS/ca.key" \
    -CAcreateserial -days "$DAYS" -sha256 -extfile "$ext" -out "$CERTS/$name.crt"
  rm -f "$CERTS/$name.csr" "$ext"
  chmod 600 "$CERTS/$name.key"
  echo "✔ สร้าง $name (CN=$cn)"
}

gen_ca
gen_cert broker "${BROKER_CN:-localhost}" server   # CN = ชื่อ host ของ broker (ตรวจ hostname)
gen_cert ingest "central-ingest"                    # client ของศูนย์

for cn in "$@"; do
  # ชื่อไฟล์: แทน / ด้วย _  เช่น BN1-A/FDC01 → BN1-A_FDC01
  gen_cert "${cn//\//_}" "$cn"
done

echo ""
echo "เสร็จ → $CERTS/"
echo " - broker.crt/key  → mosquitto"
echo " - ingest.crt/key  → ingest service"
echo " - <unit>_<node>.crt/key → ติดตั้งที่ ศอย. นั้น (วางใน fdc/certs/ แล้วชี้ใน config.yaml)"
