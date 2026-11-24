"""Extract the WeCom trust anchor from the local Mozilla CA bundle."""
from pathlib import Path
import certifi
from cryptography import x509

bundle = Path(certifi.where()).read_text(encoding="ascii")
certs = []
for part in bundle.split("-----END CERTIFICATE-----"):
    if "-----BEGIN CERTIFICATE-----" not in part:
        continue
    pem = part[part.index("-----BEGIN CERTIFICATE-----"):] + "-----END CERTIFICATE-----\n"
    if "CN=DigiCert Global Root G2," in x509.load_pem_x509_certificate(pem.encode()).subject.rfc4514_string():
        certs.append(pem)
assert len(certs) == 1
Path("include/wecom_ca.h").write_text(
    '#pragma once\n// DigiCert Global Root G2, Mozilla CA store.\n'
    'static const char WECOM_CA[] PROGMEM = R"PEM(' + certs[0] + ')PEM";\n',
    encoding="ascii",
)
