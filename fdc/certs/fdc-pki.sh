
#!/usr/bin/env bash
# ============================================================
#  fdc-pki.sh — PKI ของระบบ Artillery FCS (military grade)
# ------------------------------------------------------------
#  อัลกอริทึม: ECDSA P-384 + SHA-384  → ตรงมาตรฐาน NSA CNSA Suite
#  (Commercial National Security Algorithm) สำหรับข้อมูลชั้นความลับ
#
#  ออก/จัดการใบรับรอง mTLS ทั้งระบบ:
#    - CA กลาง (root of trust) — เก็บกุญแจลับไว้ใน _ca/private/ (chmod 600)
#    - ใบรับรอง broker ส่วนกลาง (serverAuth + SAN กัน MITM)
#    - ใบรับรอง ศอย. แต่ละ node (clientAuth → mutual TLS)
#    - เพิกถอน (revoke) + CRL  ← สำคัญมากในสนาม: เครื่องหาย/ถูกยึด ยกเลิกได้ทันที
#
#  ใช้งาน:
#    ./fdc-pki.sh init                       สร้าง CA ครั้งแรก
#    ./fdc-pki.sh server <broker-hostname>   ออกใบรับรองให้ broker
#    ./fdc-pki.sh node <unit> <node>         ออกใบรับรองให้ ศอย.  (เช่น: node BN1-A FDC01)
#    ./fdc-pki.sh revoke <name>              เพิกถอนใบรับรอง
#    ./fdc-pki.sh crl                        สร้าง/อัปเดต CRL
#    ./fdc-pki.sh list                       แสดงใบรับรองที่ออกแล้ว
#    ./fdc-pki.sh verify <name>              ตรวจสอบใบรับรองกับ CA
#    ./fdc-pki.sh hmac                        สุ่มคีย์ HMAC สำหรับคำสั่งยิง downlink
# ============================================================
set -euo pipefail

# --- พารามิเตอร์ความปลอดภัย (CNSA Suite) ---
CURVE="secp384r1"        # = NIST P-384
MD="sha384"
CA_DAYS=3650             # CA อายุ 10 ปี
LEAF_DAYS=397            # ใบลูกอายุ ~13 เดือน (บังคับต่ออายุบ่อย = ปลอดภัยกว่า)
CRL_DAYS=30              # CRL ต้องรีเฟรชทุก 30 วัน

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CA="$ROOT/_ca"
CNF="$CA/openssl.cnf"
ORG="Royal Thai Armed Forces"        # แก้ให้ตรงหน่วยจริง
OU="Artillery FCS"
COUNTRY="TH"

c_green(){ printf '\033[32m%s\033[0m\n' "$*"; }
c_red(){ printf '\033[31m%s\033[0m\n' "$*" >&2; }
die(){ c_red "ERROR: $*"; exit 1; }
need_ca(){ [[ -f "$CA/private/ca.key" ]] || die "ยังไม่มี CA — รัน './fdc-pki.sh init' ก่อน"; }

# ------------------------------------------------------------
#  เขียนไฟล์ config ของ OpenSSL CA (ฝัง extension v3 ครบ)
# ------------------------------------------------------------
write_cnf() {
  cat > "$CNF" <<EOF
# สร้างอัตโนมัติโดย fdc-pki.sh — อย่าแก้มือ
[ ca ]
default_ca = CA_default

[ CA_default ]
dir               = $CA
database          = \$dir/index.txt
serial            = \$dir/serial
new_certs_dir     = \$dir/newcerts
certificate       = \$dir/ca.crt
private_key       = \$dir/private/ca.key
crlnumber         = \$dir/crlnumber
crl               = \$dir/crl.pem
default_md        = $MD
default_days      = $LEAF_DAYS
default_crl_days  = $CRL_DAYS
policy            = policy_loose
unique_subject    = no
copy_extensions   = none

[ policy_loose ]
countryName            = optional
stateOrProvinceName    = optional
organizationName       = optional
organizationalUnitName = optional
commonName             = supplied
emailAddress           = optional

[ req ]
default_bits        = 384
distinguished_name  = req_dn
string_mask         = utf8only
prompt              = no

[ req_dn ]
C  = $COUNTRY
O  = $ORG
OU = $OU
CN = placeholder

[ v3_ca ]
# Root CA — เซ็นได้แค่ใบรับรองและ CRL, ห้ามมี CA ลูกต่อ (pathlen:0)
subjectKeyIdentifier   = hash
authorityKeyIdentifier = keyid:always,issuer
basicConstraints       = critical, CA:TRUE, pathlen:0
keyUsage               = critical, keyCertSign, cRLSign

[ crl_ext ]
authorityKeyIdentifier = keyid:always,issuer:always
EOF
}

# extension ของใบลูก เขียนเป็นไฟล์ชั่วคราวต่อใบ (หลีกเลี่ยง ENV ใน static config)
# $1 = serverAuth|clientAuth   $2 = subjectAltName value
write_leaf_ext() {
  local eku="$1" san="$2"
  cat > "$CA/leaf.ext" <<EOF
basicConstraints       = critical, CA:FALSE
keyUsage               = critical, digitalSignature
extendedKeyUsage       = $eku
subjectKeyIdentifier   = hash
authorityKeyIdentifier = keyid,issuer
subjectAltName         = $san
EOF
  echo "$CA/leaf.ext"
}

# ------------------------------------------------------------
#  init — สร้าง CA
# ------------------------------------------------------------
cmd_init() {
  [[ -f "$CA/private/ca.key" ]] && die "มี CA อยู่แล้วที่ $CA (ลบทิ้งเองถ้าจะสร้างใหม่ — ระวัง!)"
  mkdir -p "$CA/private" "$CA/newcerts"
  chmod 700 "$CA/private"
  touch "$CA/index.txt"
  [[ -f "$CA/serial" ]]    || echo 1000 > "$CA/serial"
  [[ -f "$CA/crlnumber" ]] || echo 1000 > "$CA/crlnumber"
  write_cnf

  c_green "→ สร้างกุญแจ CA (ECDSA P-384)…"
  openssl ecparam -name "$CURVE" -genkey -noout -out "$CA/private/ca.key"
  chmod 600 "$CA/private/ca.key"

  c_green "→ เซ็นใบรับรอง CA (self-signed, อายุ $CA_DAYS วัน)…"
  openssl req -x509 -new -key "$CA/private/ca.key" -"$MD" -days "$CA_DAYS" \
    -config "$CNF" -extensions v3_ca \
    -subj "/C=$COUNTRY/O=$ORG/OU=$OU/CN=FDC Root CA" \
    -out "$CA/ca.crt"

  cp "$CA/ca.crt" "$ROOT/ca.crt"     # publish ให้ทุก node/broker ใช้ตรวจ
  cmd_crl
  c_green "✔ CA พร้อม → $ROOT/ca.crt (แจกไฟล์นี้ให้ทุกเครื่องได้)"
  c_red   "⚠ ปกป้อง $CA/private/ca.key ให้ดีที่สุด — ใครได้ไฟล์นี้ปลอมบัตรได้ทั้งระบบ"
}

# ------------------------------------------------------------
#  ออกใบรับรองลูก (ใช้ร่วมกัน server/client)
# ------------------------------------------------------------
issue() {
  local name="$1" eku="$2" cn="$3" san="$4"
  need_ca
  local key="$ROOT/$name.key" csr="$CA/$name.csr" crt="$ROOT/$name.crt"
  local extf; extf="$(write_leaf_ext "$eku" "$san")"

  c_green "→ สร้างกุญแจ $name (ECDSA P-384)…"
  openssl ecparam -name "$CURVE" -genkey -noout -out "$key"
  chmod 600 "$key"

  openssl req -new -key "$key" -"$MD" -config "$CNF" \
    -subj "/C=$COUNTRY/O=$ORG/OU=$OU/CN=$cn" -out "$csr"

  c_green "→ CA เซ็นใบรับรอง $name (อายุ $LEAF_DAYS วัน)…"
  openssl ca -batch -config "$CNF" -extfile "$extf" \
    -days "$LEAF_DAYS" -notext -md "$MD" -in "$csr" -out "$crt"
  rm -f "$csr" "$extf"

  # bundle (cert + ca) เผื่อ client บางตัวต้องการ chain ครบ
  cat "$crt" "$ROOT/ca.crt" > "$ROOT/$name.bundle.pem"
  c_green "✔ ออกแล้ว: $crt  +  $key"
}

cmd_server() {
  local host="${1:-}"; [[ -n "$host" ]] || die "ใช้: ./fdc-pki.sh server <broker-hostname>"
  # SAN ต้องมีชื่อที่ client จะใช้ต่อจริง (กัน MITM ด้วย check_hostname)
  issue "broker" "serverAuth" "$host" "DNS:$host"
  c_green "  ตั้ง broker.host ใน config.yaml = $host (ต้องตรง SAN)"
}

cmd_node() {
  local unit="${1:-}" node="${2:-}"
  [[ -n "$unit" && -n "$node" ]] || die "ใช้: ./fdc-pki.sh node <unit> <node>  (เช่น node BN1-A FDC01)"
  # CN เลี่ยง '/' (openssl -subj ใช้เป็นตัวคั่น) — identity ลำดับชั้นเต็มอยู่ใน SAN URI
  local cn="$unit-$node"
  # ใส่ identity ลง SAN ด้วย เผื่อ broker ทำ ACL ตามชื่อ node
  issue "$(echo "$node" | tr 'A-Z' 'a-z')" "clientAuth" "$cn" "DNS:$node,URI:fdc://$unit/$node"
  c_green "  → ใน config.yaml ตั้ง tls.client_cert/client_key ให้ชี้ไฟล์ node นี้"
}

# ------------------------------------------------------------
#  revoke + CRL
# ------------------------------------------------------------
cmd_revoke() {
  local name="${1:-}"; [[ -n "$name" ]] || die "ใช้: ./fdc-pki.sh revoke <name>"
  need_ca
  [[ -f "$ROOT/$name.crt" ]] || die "ไม่พบ $ROOT/$name.crt"
  c_red "→ เพิกถอน $name …"
  openssl ca -config "$CNF" -revoke "$ROOT/$name.crt" -crl_reason keyCompromise
  cmd_crl
  c_green "✔ เพิกถอนแล้ว + อัปเดต CRL — อย่าลืมเอา crl.pem ไปติดตั้งที่ broker"
}

cmd_crl() {
  need_ca
  openssl ca -config "$CNF" -gencrl -out "$CA/crl.pem"
  cp "$CA/crl.pem" "$ROOT/crl.pem"
  c_green "✔ CRL → $ROOT/crl.pem (มีผล $CRL_DAYS วัน ต้องรีเฟรชก่อนหมดอายุ)"
}

cmd_list() {
  need_ca
  echo "สถานะ | หมดอายุ        | serial | subject"
  echo "------+----------------+--------+---------------------------"
  awk -F'\t' '{printf "%-5s | %-14s | %-6s | %s\n",$1,$2,$4,$6}' "$CA/index.txt" || true
}

cmd_verify() {
  local name="${1:-}"; [[ -n "$name" ]] || die "ใช้: ./fdc-pki.sh verify <name>"
  need_ca
  openssl verify -CAfile "$ROOT/ca.crt" -crl_check -CRLfile "$ROOT/crl.pem" "$ROOT/$name.crt" \
    2>/dev/null || openssl verify -CAfile "$ROOT/ca.crt" "$ROOT/$name.crt"
  echo "--- รายละเอียด ---"
  openssl x509 -in "$ROOT/$name.crt" -noout -subject -issuer -dates -ext subjectAltName,extendedKeyUsage
}

cmd_hmac() {
  local key; key="$(openssl rand -hex 32)"   # 256-bit
  local envf="$ROOT/cmd_hmac.env"
  umask 077
  printf 'export FDC_CMD_HMAC_KEY=%s\n' "$key" > "$envf"
  chmod 600 "$envf"
  c_green "✔ สุ่มคีย์ HMAC (256-bit) → $envf  (chmod 600)"
  echo "  โหลดด้วย:  source $envf"
  c_red   "⚠ คีย์นี้ต้องเหมือนกันทั้งฝั่งศูนย์และ ศอย. — ส่งผ่านช่องทางปลอดภัยเท่านั้น"
}

# ------------------------------------------------------------
case "${1:-}" in
  init)    cmd_init ;;
  server)  shift; cmd_server "$@" ;;
  node)    shift; cmd_node "$@" ;;
  revoke)  shift; cmd_revoke "$@" ;;
  crl)     cmd_crl ;;
  list)    cmd_list ;;
  verify)  shift; cmd_verify "$@" ;;
  hmac)    cmd_hmac ;;
  *) cat <<EOF
fdc-pki.sh — PKI ระบบ Artillery FCS (ECDSA P-384 / CNSA Suite)

  init                      สร้าง CA ครั้งแรก
  server <hostname>         ออกใบรับรอง broker ส่วนกลาง
  node <unit> <node>        ออกใบรับรอง ศอย.   (เช่น: node BN1-A FDC01)
  revoke <name>             เพิกถอนใบรับรอง
  crl                       สร้าง/อัปเดต CRL
  list                      แสดงใบรับรองที่ออกแล้ว
  verify <name>             ตรวจสอบใบรับรอง
  hmac                      สุ่มคีย์ HMAC สำหรับคำสั่งยิง downlink
EOF
  ;;
esac
