#include "ra_setup.h"

#include <stdio.h>
#include <string.h>

#include "ra_server.h"
#include "rc_client.h"
#include "rc_error.h"

typedef struct {
  int result;
  char error[192];
  char username[RA_VAULT_MAX_USERNAME];
  char token[RA_VAULT_MAX_TOKEN];
} setup_login_state_t;

static ra_setup_transport_t g_setup_transport;

void ra_setup_set_transport(ra_setup_transport_t transport) {
  g_setup_transport = transport;
}

static uint32_t setup_read_memory(uint32_t address, uint8_t *buffer,
                uint32_t num_bytes, rc_client_t *client) {
  (void)address;
  (void)buffer;
  (void)num_bytes;
  (void)client;
  return 0;
}

static void setup_copy(char *destination, size_t capacity, const char *source) {
  if (destination == NULL || capacity == 0)
    return;
  snprintf(destination, capacity, "%s", source != NULL ? source : "");
}

static void setup_login_complete(int result, const char *error_message,
                rc_client_t *client, void *userdata) {
  setup_login_state_t *state = (setup_login_state_t*)userdata;
  const rc_client_user_t *user;

  state->result = result;
  if (error_message != NULL)
    setup_copy(state->error, sizeof(state->error), error_message);
  if (result != RC_OK)
    return;

  user = rc_client_get_user_info(client);
  if (user == NULL || user->username == NULL || user->token == NULL) {
    state->result = RC_INVALID_STATE;
    setup_copy(state->error, sizeof(state->error),
        "login succeeded but user/token was missing");
    return;
  }
  setup_copy(state->username, sizeof(state->username), user->username);
  setup_copy(state->token, sizeof(state->token), user->token);
}

int ra_setup_login_to_vault(const char *username, const char *password,
              const uint8_t device_id[RA_VAULT_DEVICE_ID_SIZE],
              const char *vault_path, char *canonical_username,
              size_t canonical_capacity, char *error, size_t error_capacity) {
  setup_login_state_t state;
  ra_vault_credentials_t credentials;
  rc_client_t *client;
  int result;

  if (error != NULL && error_capacity > 0)
    error[0] = '\0';
  if (username == NULL || username[0] == '\0' ||
      password == NULL || password[0] == '\0' ||
      device_id == NULL || vault_path == NULL || vault_path[0] == '\0') {
    setup_copy(error, error_capacity, "username and password are required");
    return -1;
  }

  memset(&state, 0, sizeof(state));
  state.result = RC_INVALID_STATE;
  if (g_setup_transport != NULL)
    ra_server_set_transport(g_setup_transport);
  else
    ra_server_set_transport(ra_http_post);

  client = rc_client_create(setup_read_memory, ra_server_call);
  if (client == NULL) {
    setup_copy(error, error_capacity, "could not create the RA client");
    return -1;
  }

  rc_client_begin_login_with_password(client, username, password,
                    setup_login_complete, &state);
  if (state.result != RC_OK) {
    setup_copy(error, error_capacity,
        state.error[0] != '\0' ? state.error : "login failed");
    rc_client_destroy(client);
    return -1;
  }

  memset(&credentials, 0, sizeof(credentials));
  setup_copy(credentials.username, sizeof(credentials.username), state.username);
  setup_copy(credentials.token, sizeof(credentials.token), state.token);
  result = ra_vault_save(vault_path, device_id, &credentials);
  if (result != 0) {
    setup_copy(error, error_capacity, "could not save the encrypted vault");
    ra_vault_clear(&credentials);
    rc_client_destroy(client);
    return -1;
  }

  if (canonical_username != NULL && canonical_capacity > 0)
    setup_copy(canonical_username, canonical_capacity, state.username);
  ra_vault_clear(&credentials);
  rc_client_destroy(client);
  return 0;
}
