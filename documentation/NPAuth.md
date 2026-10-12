# NPAuth ID tokens

Issues the ID tokens games request with `sceNpAuthGetIdTokenA/V3`, as short-lived JWTs their own backends can verify.

---

## Overview

Games call `sceNpAuthGetIdTokenA/V3` to get a token they forward to their own backend. shadNet issues that token as a short-lived RS256 JWT for the signed-in account. The backend checks it against shadNet's public key and never sees the account bearer.

The console's own request to Sony is not public. The endpoint follows Sony's auth server conventions, and the claims a game reads from the token payload are reproduced; tokens are signed with shadNet's own key.

---

## Configuration

| Setting | Default | Description |
|---|---|---|
| `NpAuthConfig` | empty | Path to the JSON policy file. Empty disables issuance |

Set in `shadnet.cfg` next to the executable and restart. The policy is loaded at startup; an invalid policy stops the WebAPI listener.

```json
{
  "issuer": "https://shadnet.example.net",
  "signing_key": "npauth-signing-key.pem",
  "applications": [
    {
      "client_id": "<client id used by the game>",
      "client_secret_sha256": "<sha256 hex of the game's client secret>",
      "titles": ["CUSA13795"],
      "scopes": ["openid", "openid:duid"],
      "versions": ["legacy", "v3"]
    }
  ]
}
```

| Field | Description |
|---|---|
| `issuer` | HTTPS URL, copied to `iss` |
| `signing_key` | Unencrypted RSA 2048-4096 PEM, relative to the policy file |
| `titles` | Must match the title of the account's current session |
| `scopes` | Allowed scope names, `openid` required |
| `versions` | `legacy` (GetIdTokenA) and/or `v3` (GetIdTokenV3) |

`.npauth/` is ignored by git for local policies. Example key: `openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out npauth-signing-key.pem`.

---

## Endpoints

The request carries the game's client secret, so it goes to the token endpoint. NpManager forwards the secret to the system auth service only for ID tokens; authorization codes, which use the authorize endpoint, are requested without it.

Path, encoding and error codes follow Sony's PS4-era auth API as seen in public captures. [oauth2-psn](https://github.com/larabros/oauth2-psn/blob/master/DOCS.md) shows `/2.0/oauth/authorize` and a form-encoded `POST /2.0/oauth/token` with `client_id` and `client_secret` in the body. [psn-php#166](https://github.com/Tustin/psn-php/issues/166) shows that endpoint rejecting a Sony-specific grant (`sso_cookie`) with error 4151, "Client does not have grant type", the same code and message as in CTR's error table.

How the console presents the user's session is not public; shadNet uses OAuth 2.0 Token Exchange ([RFC 8693](https://www.rfc-editor.org/rfc/rfc8693)) with the account token from the shadNet login.

### POST /2.0/oauth/token

`Content-Type: application/x-www-form-urlencoded`

| Parameter | Value |
|---|---|
| `grant_type` | `urn:ietf:params:oauth:grant-type:token-exchange` |
| `subject_token` | Account token from the shadNet login |
| `subject_token_type` | `urn:ietf:params:oauth:token-type:access_token` |
| `requested_token_type` | Optional, `urn:ietf:params:oauth:token-type:id_token` |
| `scope` | Space separated; commas are also accepted since some games use them |
| `version` | `3` for GetIdTokenV3, omitted for GetIdTokenA |
| `client_id`, `client_secret` | Or HTTP Basic (`client_secret_basic`), not both |

Each parameter at most once, no query string. Limits: client ID 128 bytes, secret 256, scope 1024, body 8192. The account must have an online session.

```json
{
  "access_token": "<id token>",
  "issued_token_type": "urn:ietf:params:oauth:token-type:id_token",
  "token_type": "N_A",
  "expires_in": 120,
  "scope": "openid openid:duid"
}
```

Sent with `Cache-Control: no-store`.

### GET /.well-known/jwks.json

Public key set (`kty`, `n`, `e`, `kid`, `alg`, `use`), `Cache-Control: public, max-age=300`. No authentication.

### GET /.well-known/oauth-authorization-server

[RFC 8414](https://www.rfc-editor.org/rfc/rfc8414) metadata: `issuer`, `token_endpoint`, `jwks_uri`, supported grant and client authentication methods. URLs are built from `issuer`, so it should be the public base URL of the WebAPI.

---

## Token

Header: `alg` RS256, `typ` JWT, `kid` (SHA-256 of the public key).

| Claim | Notes |
|---|---|
| `iss` | Configured issuer |
| `sub` | Account ID, decimal string |
| `aud` | Client ID |
| `iat`, `exp` | Unix seconds, `exp = iat + 120` |
| `jti` | Random, unique per token |
| `env_iss_id` | `256` (retail NP environment) |
| `online_id` | Current online ID |
| `np_title_id` | Title of the account's session |
| `scope` | Granted scopes, space separated |
| `duid` | Only with `openid:duid`: 64 hex chars, stable per account. Not a real device ID |

Crash Team Racing Nitro-Fueled (CUSA13795) decodes the payload and reads `env_iss_id` as an integer, `duid` as exactly 64 characters (the last 32 as hex) and `iat`/`exp` as integers.

---

## Errors

OAuth error body ([RFC 6749 §5.2](https://www.rfc-editor.org/rfc/rfc6749#section-5.2)) plus Sony's numeric code: `{"error": "...", "error_description": "...", "error_code": n}`. `error_code` is only set where the game knows Sony's code; the emulator returns it to the game as `0x82E00000 | error_code`.

| HTTP | error | error_description | error_code |
|---|---|---|---|
| 400 | `invalid_request` | Mandatory parameter is missing | 4098 |
| 400 | `invalid_request` | Parameter is malformed | 4099 |
| 400 | `invalid_request` | Query parameters not allowed for POST | 4130 |
| 400 | `unsupported_grant_type` | Unsupported grant type | 4152 |
| 400 | `invalid_scope` | Invalid scope | 4153 |
| 400 | `invalid_grant` | Subject token is not valid / no online session | |
| 400 | `unauthorized_client` | Client is not registered for this title / version | |
| 401 | `invalid_client` | Bad client credentials | 4102 |
| 401 | `invalid_client` | The client was not found | 4161 |
| 500 | `server_error` | An unknown error has occurred | 2 |
| 503 | `temporarily_unavailable` | Service dependency is temporarily unavailable | 9001 |

`invalid_client` with HTTP Basic also sends `WWW-Authenticate: Basic`.

---

## Verifying a token

- Get keys from `jwks_uri` (configured, or from the metadata of a trusted issuer), never from the token. Cache them and refetch on an unknown `kid`.
- Require RS256 and a known `kid`, check the signature, then `iss`, `aud`, `exp`, `iat` and the expected title and scopes.
- Allow some clock leeway (around 60 s) if the backend runs on another machine.
- The token can be reused until it expires. Logout stops new tokens but does not revoke issued ones.

---

## Key rotation

One signing key is active. Stop issuance, wait for the token lifetime, replace the key and restart. Backends pick up the new key from the JWKS endpoint.
