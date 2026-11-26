#!/usr/bin/env bash
# An Entra access token from GitHub Actions' OIDC token, with no Azure CLI: the federated
# credential of AZURE_CLIENT_ID (in AZURE_TENANT_ID) trusts this repo, so the OIDC token is the
# client assertion. Needs `permissions: id-token: write`.
#
#   entra_token.sh <scope>     an access token, e.g. for https://storage.azure.com/.default
#   entra_token.sh --oidc      the GitHub OIDC token itself (for AZURE_FEDERATED_TOKEN_FILE)
set -euo pipefail

oidc=$(curl -sSf -H "Authorization: bearer $ACTIONS_ID_TOKEN_REQUEST_TOKEN" \
  "$ACTIONS_ID_TOKEN_REQUEST_URL&audience=api://AzureADTokenExchange" |
  python3 -c 'import json, sys; print(json.load(sys.stdin)["value"])')

if [ "$1" = "--oidc" ]; then
  printf '%s' "$oidc"
  exit 0
fi

curl -sSf "https://login.microsoftonline.com/$AZURE_TENANT_ID/oauth2/v2.0/token" \
  --data-urlencode "client_id=$AZURE_CLIENT_ID" \
  --data-urlencode "scope=$1" \
  --data-urlencode "grant_type=client_credentials" \
  --data-urlencode "client_assertion_type=urn:ietf:params:oauth:client-assertion-type:jwt-bearer" \
  --data-urlencode "client_assertion=$oidc" |
  python3 -c 'import json, sys; print(json.load(sys.stdin)["access_token"])'
