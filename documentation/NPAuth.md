# NPAuth ID tokens

Games call `sceNpAuthGetIdTokenA` / `sceNpAuthGetIdTokenV3` to get a signed token for the
signed-in user and pass it to their own backend. The backend checks the signature and reads
the account from the token without calling shadNet back. shadNet issues these tokens as RS256
JWTs signed with its own key.

Authorization codes (`sceNpAuthGetAuthorizationCode*`) are the other way for a backend to learn
who a player is. The backend redeems the code at `POST /2.0/oauth/token` and calls shadNet to
validate the access token, so it works with any backend that can be pointed at shadNet. An ID
token only works with a backend that trusts shadNet's key (see [Verifying a token](#verifying-a-token)).

---

## Configuration

| Setting | Default | Description |
|---|---|---|
| `NpAuthConfig` | empty | Path to the JSON policy. Empty turns ID tokens off |

Set it in `shadnet.cfg` and restart. A policy that fails to load is logged and leaves ID tokens
off; the rest of the server starts normally.

```json
{
  "issuer": "https://shadnet.example.net/2.0/oauth/token",
  "signing_key": "npauth-signing-key.pem",
  "token_lifetime": 3600,
  "clients": [
    {
      "client_id": "<client ID the game passes>",
      "client_secret_sha256": "<sha256 hex of the game's client secret>",
      "titles": ["CUSA00000"],
      "scopes": ["openid", "openid:acct_id_str", "openid:online_id"]
    }
  ]
}
```

| Field | Description |
|---|---|
| `issuer` | HTTPS URL copied to the `iss` claim. Use the public URL of the token endpoint |
| `signing_key` | Unencrypted RSA PEM, 2048 to 4096 bits, relative to the policy file |
| `token_lifetime` | Optional, seconds from 60 to 86400. Default 3600 |
| `clients` | Registered game clients |
| `client_id` | Client ID the game passes to NpAuth |
| `client_secret_sha256` | SHA-256 of the client secret, hex |
| `titles` | Optional. When set, the account's current title must be in the list |
| `scopes` | Optional. When set, requests may only ask for these scopes |

Create a key with
`openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out npauth-signing-key.pem`
and a secret hash with `printf '%s' '<secret>' | sha256sum`.

### Client secrets at the token endpoint

The registry also applies to the authorization code endpoints. A registered client that sends
the wrong secret to `POST /2.0/oauth/token` or `GET /2.0/oauth/token/<token>` gets
`invalid_client` (4102). Clients that are not registered are accepted there as before.

---

## Requesting a token (client protocol)

Command `GetIdToken` (43), sent by the emulator on the signed-in session.

```protobuf
message GetIdTokenRequest {
    string client_id     = 1;
    bytes  client_secret = 2;
    string scope         = 3; // space or comma separated
}

message GetIdTokenReply {
    string id_token          = 1;
    uint32 expires_in        = 2;
    string error             = 3;
    string error_description = 4;
    uint32 error_code        = 5;
}
```

| Reply | Meaning |
|---|---|
| `Unauthorized` | The session is not online yet or no longer allowed online |
| `Unsupported` | ID tokens are off on this server |
| `Malformed` | The request could not be decoded |
| `NoError` | `id_token` is set, or `error` / `error_code` say why it was refused |

The client returns `0x82E00000 | error_code` to the game when `error_code` is set.

| error | error_code | When |
|---|---|---|
| `invalid_request` | 4098 | `client_id`, `client_secret` or `scope` is empty |
| `invalid_request` | 4099 | A field is too long (client ID 128, secret 256, scope 1024 bytes) |
| `invalid_client` | 4161 | The client ID is not registered |
| `invalid_client` | 4102 | Wrong client secret |
| `invalid_client` | 4173 | The client is not registered for the account's current title |
| `invalid_scope` | 4153 | Unknown scope, or one the client may not request |
| `server_error` | 2 | Signing failed |

---

## Token

Header: `alg` `RS256`, `typ` `JWT`, `kid` (SHA-256 thumbprint of the public key).

Each scope adds the claim it is named after, so a token only carries what the game asked for.

| Scope | Claim | Value |
|---|---|---|
| `openid` | `iss`, `aud`, `iat`, `exp` | The four claims every OpenID Connect ID token has |
| `openid:iss` | `iss` | Configured issuer |
| `openid:aud` | `aud` | Client ID used for the request |
| `openid:iat` | `iat` | Issue time, Unix seconds |
| `openid:exp` | `exp` | Expiry, Unix seconds (`iat + token_lifetime`) |
| `openid:env_iss_id` | `env_iss_id` | `"256"` (string), the retail NP environment |
| `openid:acct_id` | `acct_id` | Account ID as a number. Kept for older games |
| `openid:acct_id_str` | `acct_id_str` | Account ID as a decimal string |
| `openid:online_id` | `online_id` | Online ID |
| `openid:ctry_code` | `ctry_code` | Country code, upper case (`US`) |
| `openid:lang` | `lang` | Language code (`en`) |
| `openid:comm_domain` | `comm_domain` | `a0` |
| `openid:dob` | `dob` | Date of birth, `YYYY-MM-DD` |
| `openid:age` | `age` | Age in years |
| `openid:duid` | `duid` | 64 hex characters, stable per account. Not a real console ID |

There is no `sub` claim. Country, language, date of birth and age are placeholders until
accounts store them; they match what the account and token validation endpoints report.

The signature is base64url encoded, as JWS requires.

---

## Verifying a token

`GET /.well-known/jwks.json` returns the public key (`kty`, `n`, `e`, `kid`, `alg`, `use`),
cacheable for five minutes.

- Take the key from the JWKS endpoint of a server you trust, never from the token.
- Require `RS256` and a known `kid`, then check the signature, `iss`, `aud` (your client ID) and
  `exp`. Allow about a minute of clock skew.
- Accept base64 as well as base64url for the signature if the same backend also has to accept
  tokens from other issuers.
- A token stays valid until it expires; signing out does not revoke it.

A backend that only trusts a key built into it will not accept shadNet's tokens. Point it at
shadNet's JWKS instead, or use authorization codes.

## Key rotation

One key signs at a time. Turn ID tokens off, wait for `token_lifetime` to pass, replace the key
and restart. Backends pick up the new key from the JWKS endpoint on the next unknown `kid`.
