# AWX Credentials Configuration
# Sensitive values should be passed via environment variables

import os

# Admin user (created on first startup)
ADMIN_USER = os.environ.get('AWX_ADMIN_USER', 'admin')
ADMIN_PASSWORD = os.environ.get('AWX_ADMIN_PASSWORD', 'password')
ADMIN_EMAIL = os.environ.get('AWX_ADMIN_EMAIL', 'admin@example.com')

# Secret key for cryptographic signing
SECRET_KEY = os.environ.get('AWX_SECRET_KEY', 'awxsecret')

# Social auth (optional - configure for LDAP/SAML/OAuth)
# SOCIAL_AUTH_SAML_ENABLED_IDPS = {}
# AUTH_LDAP_SERVER_URI = 'ldap://ldap.example.com'
