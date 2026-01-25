#!/bin/bash
#
# Generate Self-Signed SSL Certificates for AWX
#
# For development/testing only. In production, use certificates
# from a trusted CA (Let's Encrypt, internal PKI, etc.)
#
# Usage:
#   ./generate-ssl-certs.sh [hostname]
#
# Example:
#   ./generate-ssl-certs.sh awx.example.com
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SSL_DIR="${SCRIPT_DIR}/../nginx/ssl"
HOSTNAME="${1:-localhost}"

# Certificate validity period
DAYS_VALID=365

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}Generating SSL certificates for AWX${NC}"
echo "Hostname: ${HOSTNAME}"
echo ""

# Create SSL directory
mkdir -p "${SSL_DIR}"

# Generate private key
echo "Generating private key..."
openssl genrsa -out "${SSL_DIR}/awx.key" 4096

# Create certificate signing request config
cat > "${SSL_DIR}/openssl.cnf" << EOF
[req]
default_bits = 4096
prompt = no
default_md = sha256
distinguished_name = dn
req_extensions = req_ext
x509_extensions = v3_req

[dn]
C = US
ST = California
L = San Francisco
O = AWX Development
OU = IT
CN = ${HOSTNAME}

[req_ext]
subjectAltName = @alt_names

[v3_req]
subjectAltName = @alt_names
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth

[alt_names]
DNS.1 = ${HOSTNAME}
DNS.2 = localhost
DNS.3 = awx-web
DNS.4 = awx-nginx
IP.1 = 127.0.0.1
EOF

# Generate self-signed certificate
echo "Generating self-signed certificate..."
openssl req \
    -new \
    -x509 \
    -key "${SSL_DIR}/awx.key" \
    -out "${SSL_DIR}/awx.crt" \
    -days ${DAYS_VALID} \
    -config "${SSL_DIR}/openssl.cnf"

# Set permissions
chmod 600 "${SSL_DIR}/awx.key"
chmod 644 "${SSL_DIR}/awx.crt"

# Display certificate info
echo ""
echo -e "${GREEN}Certificate generated successfully!${NC}"
echo ""
echo "Files created:"
echo "  Private Key: ${SSL_DIR}/awx.key"
echo "  Certificate: ${SSL_DIR}/awx.crt"
echo ""
echo "Certificate details:"
openssl x509 -in "${SSL_DIR}/awx.crt" -noout -subject -dates -issuer
echo ""
echo -e "${YELLOW}NOTE: This is a self-signed certificate for development only.${NC}"
echo -e "${YELLOW}For production, obtain certificates from a trusted CA.${NC}"
echo ""
echo "To trust this certificate on macOS:"
echo "  sudo security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain ${SSL_DIR}/awx.crt"
echo ""
echo "To trust on Linux (Debian/Ubuntu):"
echo "  sudo cp ${SSL_DIR}/awx.crt /usr/local/share/ca-certificates/awx.crt"
echo "  sudo update-ca-certificates"
