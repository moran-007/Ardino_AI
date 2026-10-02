#pragma once

// RSA-2048 public key used to verify OTA manifests. The matching private key
// is generated once under ota_release_private/ and must never be copied to a
// device or server checkout that does not need to publish releases.
static const char OTA_FIRMWARE_SIGNING_PUBLIC_KEY[] PROGMEM = R"KEY(
-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAkCKOVczeYvK8XYOA/Cy2
TY7CufA8kl/LQRPCcGxHDmJeqIPU1mOrtuQ8H4ncVkCzQW/fJtp1WfKWWK2cHe7A
OrxnORVUNnKW7y8IFqg2imIHiqljopdN55VW+hJugIfVSiMIym4mQ8PsTbAembtF
gx0EvPFrw5aRmYMsbqpLhb6n2vnBZjivHg5TJT7FFGto0Gj4WuDCzdhNWy0DjMlv
yPBo97LHRuTYsp/Uu3O1Ap6lnhMZftDK2AuDRgVxSSNM9to2hfRyFuJb7PnECfS1
Ji1OYBpG39U0vvPr854qTZ5L7Y9gAUgfau9YXsnVMA5HU1Ga8ae+/Vu+aBcaGvey
YwIDAQAB
-----END PUBLIC KEY-----
)KEY";

