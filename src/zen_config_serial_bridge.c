// SPDX-License-Identifier: GPL-2.0-or-later
//
// USB-serial config bridge for the ZEN configurator companion app. Exposes
// pointer/scroll runtime settings (zen_pointer_settings) and direct keymap
// binding edits (plain ZMK core keymap/behavior APIs -- not ZMK Studio's own
// RPC/protobuf transport) over the same UART used by the studio-rpc-usb-uart
// snippet. See include/zen_config_wire.h for the framing/command reference.
//
// CENTRAL/STANDALONE ONLY -- CMakeLists.txt gates this file on CONFIG_ZMK_USB.
//
// Why: building it on a split peripheral stopped that half ever coming back
// from deep sleep. Bisected against main with a shortened
// CONFIG_ZMK_IDLE_SLEEP_TIMEOUT: main recovered, main + this repo's Kconfig
// changes recovered, this branch minus this one file recovered, this branch
// with it did not. Only reproducible when *both* halves were asleep at once;
// with the central awake the peripheral came back fine, which is why it
// looked like an overnight-only problem.
//
// The asymmetry is that CONFIG_ZMK_USB is on for the central and off for the
// peripheral (zmk/app/CMakeLists.txt builds the USB/HID/endpoint layer for
// central + standalone only). The studio-rpc-usb-uart snippet still brings up
// the raw USB device stack and a CDC-ACM port on the peripheral, so this
// bridge had a device to poll there -- one ZMK itself never enables or
// manages. Polling an unowned USB device from a plain thread, across a
// zmk_pm_suspend_devices()/sys_poweroff() cycle, is not something to paper
// over, so the bridge simply isn't built where ZMK doesn't own USB.
//
// Cost: no GET_STATUS/pointer settings over USB on the peripheral. That was
// genuinely useful for diagnosis once, but not worth a half that stops
// waking up.

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <zmk/battery.h>
#include <zmk/behavior.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/central.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/bluetooth/peripheral.h>
#endif

#include "zen_config_codec.h"
#include "zen_config_wire.h"
#include "zen_pointer_settings.h"

LOG_MODULE_REGISTER(zen_config_serial_bridge, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_CHOSEN(zmk_studio_rpc_uart)
#define ZEN_CONFIG_SERIAL_NODE DT_CHOSEN(zmk_studio_rpc_uart)
#elif DT_HAS_CHOSEN(zephyr_shell_uart)
#define ZEN_CONFIG_SERIAL_NODE DT_CHOSEN(zephyr_shell_uart)
#elif DT_HAS_CHOSEN(zephyr_console)
#define ZEN_CONFIG_SERIAL_NODE DT_CHOSEN(zephyr_console)
#endif

#ifdef ZEN_CONFIG_SERIAL_NODE
static const struct device *const zen_config_serial = DEVICE_DT_GET(ZEN_CONFIG_SERIAL_NODE);
#else
static const struct device *const zen_config_serial;
#endif

#define ZEN_CONFIG_MAX_REQUEST_PAYLOAD                                                            \
    MAX(ZEN_CONFIG_WIRE_POINTER_PAYLOAD_SIZE, 1u + 2u + ZEN_CONFIG_WIRE_BINDING_SIZE)
/* SET_BINDING is comfortably the largest request, so the BLE commands do not
 * move the number above -- assert it rather than trusting that to stay true. */
BUILD_ASSERT(ZEN_CONFIG_MAX_REQUEST_PAYLOAD >= 2u, "request buffer too small for BLE_ACTION");
#define ZEN_CONFIG_MAX_RESPONSE_PAYLOAD (1u + (size_t)ZMK_KEYMAP_LEN * ZEN_CONFIG_WIRE_BINDING_SIZE)

static uint8_t rx_buffer[ZEN_CONFIG_WIRE_HEADER_SIZE + ZEN_CONFIG_MAX_REQUEST_PAYLOAD];
static size_t rx_pos;

static uint8_t tx_buffer[ZEN_CONFIG_WIRE_HEADER_SIZE + ZEN_CONFIG_MAX_RESPONSE_PAYLOAD];

static void reset_rx_buffer(void) { rx_pos = 0; }

static void slide_rx_buffer(void) {
    if (rx_pos == 0) {
        return;
    }

    memmove(rx_buffer, &rx_buffer[1], rx_pos - 1);
    rx_pos--;
}

static bool rx_magic_matches(void) {
    return rx_pos >= 4 && sys_get_le32(&rx_buffer[0]) == ZEN_CONFIG_WIRE_MAGIC;
}

static size_t expected_request_payload_len(uint8_t command) {
    switch (command) {
    case ZEN_CONFIG_CMD_GET_POINTER:
    case ZEN_CONFIG_CMD_SAVE_POINTER:
    case ZEN_CONFIG_CMD_RESET_POINTER:
    case ZEN_CONFIG_CMD_GET_KEYMAP_META:
    case ZEN_CONFIG_CMD_SAVE_KEYMAP:
    case ZEN_CONFIG_CMD_DISCARD_KEYMAP:
    case ZEN_CONFIG_CMD_RESET_KEYMAP:
        return 0;
    case ZEN_CONFIG_CMD_SET_POINTER:
        return ZEN_CONFIG_WIRE_POINTER_PAYLOAD_SIZE;
    case ZEN_CONFIG_CMD_GET_AUTO_MOUSE:
        return 0;
    case ZEN_CONFIG_CMD_SET_AUTO_MOUSE:
        return ZEN_CONFIG_WIRE_AUTO_MOUSE_PAYLOAD_SIZE;
    case ZEN_CONFIG_CMD_GET_LAYER:
        return 1;
    case ZEN_CONFIG_CMD_SET_BINDING:
        return 1 + 2 + ZEN_CONFIG_WIRE_BINDING_SIZE;
    case ZEN_CONFIG_CMD_GET_STATUS:
    case ZEN_CONFIG_CMD_GET_BLE_PROFILES:
        return 0;
    case ZEN_CONFIG_CMD_BLE_ACTION:
        return 2; /* action, profile index */
    default:
        return SIZE_MAX;
    }
}

static size_t expected_frame_size(void) {
    if (rx_pos < ZEN_CONFIG_WIRE_HEADER_SIZE) {
        return 0;
    }

    uint8_t version = rx_buffer[4];
    uint8_t command = rx_buffer[5];
    uint16_t payload_len = sys_get_le16(&rx_buffer[6]);

    if (version != ZEN_CONFIG_WIRE_VERSION) {
        return SIZE_MAX;
    }

    size_t expected = expected_request_payload_len(command);
    if (expected == SIZE_MAX || payload_len != expected) {
        return SIZE_MAX;
    }

    return ZEN_CONFIG_WIRE_HEADER_SIZE + payload_len;
}

static void send_response(uint8_t command, uint8_t status, const uint8_t *payload,
                          size_t payload_len) {
    if (!zen_config_serial || !device_is_ready(zen_config_serial)) {
        return;
    }

    size_t total_payload_len = 1 + payload_len;

    sys_put_le32(ZEN_CONFIG_WIRE_MAGIC, &tx_buffer[0]);
    tx_buffer[4] = ZEN_CONFIG_WIRE_VERSION;
    tx_buffer[5] = command | ZEN_CONFIG_RESPONSE_BIT;
    sys_put_le16((uint16_t)total_payload_len, &tx_buffer[6]);
    tx_buffer[ZEN_CONFIG_WIRE_HEADER_SIZE] = status;

    if (payload && payload_len > 0) {
        memcpy(&tx_buffer[ZEN_CONFIG_WIRE_HEADER_SIZE + 1], payload, payload_len);
    }

    size_t frame_size = ZEN_CONFIG_WIRE_HEADER_SIZE + total_payload_len;
    for (size_t i = 0; i < frame_size; i++) {
        uart_poll_out(zen_config_serial, tx_buffer[i]);
    }
}

static void handle_get_pointer(void) {
    uint8_t payload[ZEN_CONFIG_WIRE_POINTER_PAYLOAD_SIZE];
    zen_config_encode_pointer(zen_pointer_settings_payload(), payload);
    send_response(ZEN_CONFIG_CMD_GET_POINTER, ZEN_CONFIG_STATUS_OK, payload, sizeof(payload));
}

static void handle_set_pointer(const uint8_t *payload) {
    /* Start from what the keyboard already holds, the same way
     * handle_set_auto_mouse does: this command carries only part of a shared
     * record, and the rest has to survive untouched. */
    struct zen_pointer_settings_payload settings;
    zen_pointer_settings_copy_payload(&settings);

    if (zen_config_decode_pointer(payload, &settings) < 0) {
        send_response(ZEN_CONFIG_CMD_SET_POINTER, ZEN_CONFIG_STATUS_ERR_INVALID_PAYLOAD, NULL, 0);
        return;
    }

    zen_pointer_settings_apply_payload(&settings, false);
    send_response(ZEN_CONFIG_CMD_SET_POINTER, ZEN_CONFIG_STATUS_OK, NULL, 0);
}

static void handle_get_auto_mouse(void) {
    uint8_t payload[ZEN_CONFIG_WIRE_AUTO_MOUSE_PAYLOAD_SIZE];
    zen_config_encode_auto_mouse(zen_pointer_settings_payload(), payload);
    send_response(ZEN_CONFIG_CMD_GET_AUTO_MOUSE, ZEN_CONFIG_STATUS_OK, payload, sizeof(payload));
}

static void handle_set_auto_mouse(const uint8_t *payload) {
    /* Start from what the keyboard already holds and change only the
     * auto-mouse fields -- this command shares its settings record with the
     * pointer settings, and must not disturb them. */
    struct zen_pointer_settings_payload settings;
    zen_pointer_settings_copy_payload(&settings);

    if (zen_config_decode_auto_mouse(payload, &settings) < 0 ||
        zen_pointer_settings_apply_payload(&settings, false) < 0) {
        send_response(ZEN_CONFIG_CMD_SET_AUTO_MOUSE, ZEN_CONFIG_STATUS_ERR_INVALID_PAYLOAD, NULL,
                      0);
        return;
    }

    send_response(ZEN_CONFIG_CMD_SET_AUTO_MOUSE, ZEN_CONFIG_STATUS_OK, NULL, 0);
}

static void handle_save_pointer(void) {
    struct zen_pointer_settings_payload current;
    zen_pointer_settings_copy_payload(&current);
    int rc = zen_pointer_settings_apply_payload(&current, true);
    send_response(ZEN_CONFIG_CMD_SAVE_POINTER, rc == 0 ? ZEN_CONFIG_STATUS_OK : ZEN_CONFIG_STATUS_ERR_GENERIC,
                  NULL, 0);
}

static void handle_reset_pointer(void) {
    int rc = zen_pointer_settings_reset_to_defaults();
    if (rc != 0) {
        send_response(ZEN_CONFIG_CMD_RESET_POINTER, ZEN_CONFIG_STATUS_ERR_GENERIC, NULL, 0);
        return;
    }

    uint8_t payload[ZEN_CONFIG_WIRE_POINTER_PAYLOAD_SIZE];
    zen_config_encode_pointer(zen_pointer_settings_payload(), payload);
    send_response(ZEN_CONFIG_CMD_RESET_POINTER, ZEN_CONFIG_STATUS_OK, payload, sizeof(payload));
}

/* The Bluetooth profiles ("チャンネル" in the Configurator) this keyboard can
 * pair a host to.
 *
 * A split peripheral has none of its own -- it is bonded to the central, not
 * to any host -- so both handlers below refuse there rather than reporting the
 * central's profile table as if it were the peripheral's.
 */
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define ZEN_CONFIG_HAS_BLE_PROFILES 0
#else
#define ZEN_CONFIG_HAS_BLE_PROFILES 1
#endif

#if ZEN_CONFIG_HAS_BLE_PROFILES

static uint8_t ble_profile_count(void) {
    return MIN((uint8_t)ZMK_BLE_PROFILE_COUNT, (uint8_t)ZEN_CONFIG_WIRE_BLE_PROFILES_MAX);
}

static void encode_ble_profiles(uint8_t *out) {
    const uint8_t count = ble_profile_count();

    memset(out, 0, ZEN_CONFIG_WIRE_BLE_PROFILES_PAYLOAD_SIZE);
    out[0] = count;

    const int active = zmk_ble_active_profile_index();
    out[1] = (active >= 0 && active < count) ? (uint8_t)active : 0xFF;

    for (uint8_t i = 0; i < count; i++) {
        uint8_t flags = 0;
        /* "open" is ZMK's word for a profile with no bond in it yet. */
        if (!zmk_ble_profile_is_open(i)) {
            flags |= ZEN_CONFIG_WIRE_BLE_PROFILE_BONDED;
        }
        if (zmk_ble_profile_is_connected(i)) {
            flags |= ZEN_CONFIG_WIRE_BLE_PROFILE_CONNECTED;
        }
        out[2 + i] = flags;
    }
}

static void send_ble_profiles(uint8_t command) {
    uint8_t payload[ZEN_CONFIG_WIRE_BLE_PROFILES_PAYLOAD_SIZE];
    encode_ble_profiles(payload);
    send_response(command, ZEN_CONFIG_STATUS_OK, payload, sizeof(payload));
}

/* Forget one profile's bond.
 *
 * ZMK only exposes clearing the *active* profile (zmk_ble_clear_bonds), so
 * select the target, clear it, and select back. Going through the public API
 * this way keeps the profile bookkeeping and the settings write in ZMK's
 * hands; reaching for bt_unpair() directly would unpair the device but leave
 * ZMK still believing the slot is occupied.
 *
 * Deliberately NOT zmk_ble_unpair_all(): it is declared in zmk/ble.h but has
 * no definition in v0.3, and the name suggests a blanket bt_unpair() that
 * would take the split peripheral's bond with it. zmk_ble_clear_all_bonds()
 * walks profiles[] only, which is the table that excludes the peripheral.
 */
static int clear_ble_profile(uint8_t index) {
    if (index >= ble_profile_count()) {
        return -ERANGE;
    }

    const int previous = zmk_ble_active_profile_index();

    int rc = zmk_ble_prof_select(index);
    if (rc < 0) {
        return rc;
    }

    zmk_ble_clear_bonds();

    /* Cleared the one that was in use: leave it selected and open, which is
     * what the keyboard does for &bt BT_CLR and puts it straight into pairing. */
    if (previous >= 0 && previous != index) {
        zmk_ble_prof_select((uint8_t)previous);
    }

    return 0;
}

static void handle_get_ble_profiles(void) { send_ble_profiles(ZEN_CONFIG_CMD_GET_BLE_PROFILES); }

static void handle_ble_action(const uint8_t *payload) {
    const uint8_t action = payload[0];
    const uint8_t index = payload[1];

    if (action != ZEN_CONFIG_BLE_ACTION_CLEAR_ALL && index >= ble_profile_count()) {
        send_response(ZEN_CONFIG_CMD_BLE_ACTION, ZEN_CONFIG_STATUS_ERR_INVALID_LOCATION, NULL, 0);
        return;
    }

    int rc;
    switch (action) {
    case ZEN_CONFIG_BLE_ACTION_SELECT:
        /* Selecting a profile with no bond in it is how pairing starts: ZMK's
         * update_advertising() sees an open profile and advertises. */
        rc = zmk_ble_prof_select(index);
        break;
    case ZEN_CONFIG_BLE_ACTION_DISCONNECT:
        rc = zmk_ble_prof_disconnect(index);
        /* Nothing was connected -- the end state the caller asked for. */
        if (rc == -ENODEV) {
            rc = 0;
        }
        break;
    case ZEN_CONFIG_BLE_ACTION_CLEAR:
        rc = clear_ble_profile(index);
        break;
    case ZEN_CONFIG_BLE_ACTION_CLEAR_ALL:
        zmk_ble_clear_all_bonds();
        rc = 0;
        break;
    default:
        send_response(ZEN_CONFIG_CMD_BLE_ACTION, ZEN_CONFIG_STATUS_ERR_INVALID_PAYLOAD, NULL, 0);
        return;
    }

    if (rc < 0) {
        send_response(ZEN_CONFIG_CMD_BLE_ACTION, ZEN_CONFIG_STATUS_ERR_GENERIC, NULL, 0);
        return;
    }

    send_ble_profiles(ZEN_CONFIG_CMD_BLE_ACTION);
}

#else /* split peripheral */

static void handle_get_ble_profiles(void) {
    send_response(ZEN_CONFIG_CMD_GET_BLE_PROFILES, ZEN_CONFIG_STATUS_ERR_NOT_SUPPORTED, NULL, 0);
}

static void handle_ble_action(const uint8_t *payload) {
    ARG_UNUSED(payload);
    send_response(ZEN_CONFIG_CMD_BLE_ACTION, ZEN_CONFIG_STATUS_ERR_NOT_SUPPORTED, NULL, 0);
}

#endif /* ZEN_CONFIG_HAS_BLE_PROFILES */

/* Battery/connection status. Works on any role -- unlike the keymap
 * handlers below, zmk_battery_state_of_charge()/zmk_ble_*() are compiled in
 * regardless of split role. */
static void handle_get_status(void) {
    uint8_t payload[ZEN_CONFIG_WIRE_STATUS_PAYLOAD_SIZE];

    payload[0] = zmk_battery_state_of_charge();

    payload[1] = 0;
    payload[2] = 0xFF;
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    /* zmk_split_central_get_peripheral_battery_level() only range-checks and
     * always succeeds -- it hands back a cache (central.c's
     * peripheral_battery_levels[]) that starts at 0 and is only written when
     * the peripheral actually reports over the split link. So a 0 here means
     * "the peripheral has never reported", not "0% charge", and reporting it
     * as a real reading makes a disconnected half look like a flat battery.
     * Treat 0 as unknown; a genuinely-0% peripheral is already dead anyway
     * (CONFIG_ZMK_NON_LIPO_MIN_MV cuts out well above that). */
    uint8_t peripheral_level;
    if (zmk_split_central_get_peripheral_battery_level(0, &peripheral_level) == 0 &&
        peripheral_level > 0) {
        payload[1] = 1;
        payload[2] = peripheral_level;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    payload[3] = 1; /* central */
#elif IS_ENABLED(CONFIG_ZMK_SPLIT)
    payload[3] = 2; /* peripheral */
#else
    payload[3] = 0; /* standalone */
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    payload[4] = zmk_split_bt_peripheral_is_connected() ? 1 : 0;
    payload[5] = 0xFF;
    payload[6] = 0xFF;
#else
    payload[4] = zmk_ble_active_profile_is_connected() ? 1 : 0;
    payload[5] = (uint8_t)zmk_ble_active_profile_index();
    payload[6] = (uint8_t)zmk_endpoints_selected().transport;
#endif

    send_response(ZEN_CONFIG_CMD_GET_STATUS, ZEN_CONFIG_STATUS_OK, payload, sizeof(payload));
}

/* src/keymap.c (and the zmk_keymap_* functions it defines) is only compiled
 * into non-split and split-central builds -- see the matching condition in
 * zmk/app/CMakeLists.txt. On a split peripheral (e.g. zen_left) those symbols
 * don't exist at link time, so the handlers below have to be swapped out for
 * stubs on that side rather than merely skipped at runtime. */
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static void handle_get_keymap_meta(void) {
    /* payload[0] = layer_count, payload[1..2] = key_position_count (LE u16),
     * payload[3..] = one layer_id byte per layer, in display order. */
    uint8_t payload[3 + ZMK_KEYMAP_LAYERS_LEN];
    uint8_t layer_count = 0;

    for (zmk_keymap_layer_index_t index = 0; index < ZMK_KEYMAP_LAYERS_LEN; index++) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(index);
        if (id == UINT8_MAX) {
            break;
        }
        payload[3 + layer_count] = id;
        layer_count++;
    }

    payload[0] = layer_count;
    sys_put_le16((uint16_t)ZMK_KEYMAP_LEN, &payload[1]);

    send_response(ZEN_CONFIG_CMD_GET_KEYMAP_META, ZEN_CONFIG_STATUS_OK, payload,
                  3 + layer_count);
}

static void handle_get_layer(const uint8_t *payload) {
    zmk_keymap_layer_id_t layer_id = payload[0];

    static uint8_t response[(size_t)ZMK_KEYMAP_LEN * ZEN_CONFIG_WIRE_BINDING_SIZE];
    memset(response, 0, sizeof(response));

    for (int idx = 0; idx < ZMK_KEYMAP_LEN; idx++) {
        const struct zmk_behavior_binding *binding = zmk_keymap_get_layer_binding_at_idx(layer_id, idx);
        uint8_t *entry = &response[idx * ZEN_CONFIG_WIRE_BINDING_SIZE];

        if (binding && binding->behavior_dev) {
            size_t name_len = strlen(binding->behavior_dev);
            if (name_len >= ZEN_CONFIG_WIRE_BEHAVIOR_NAME_LEN) {
                name_len = ZEN_CONFIG_WIRE_BEHAVIOR_NAME_LEN - 1;
            }
            memcpy(entry, binding->behavior_dev, name_len);
            entry[name_len] = '\0';
            sys_put_le32(binding->param1, &entry[ZEN_CONFIG_WIRE_BEHAVIOR_NAME_LEN]);
            sys_put_le32(binding->param2, &entry[ZEN_CONFIG_WIRE_BEHAVIOR_NAME_LEN + 4]);
        }
    }

    send_response(ZEN_CONFIG_CMD_GET_LAYER, ZEN_CONFIG_STATUS_OK, response, sizeof(response));
}

static void handle_set_binding(const uint8_t *payload) {
    zmk_keymap_layer_id_t layer_id = payload[0];
    uint16_t key_position = sys_get_le16(&payload[1]);
    const uint8_t *binding_entry = &payload[3];

    char name[ZEN_CONFIG_WIRE_BEHAVIOR_NAME_LEN];
    memcpy(name, binding_entry, sizeof(name));
    name[sizeof(name) - 1] = '\0';

    const struct device *behavior_dev = zmk_behavior_get_binding(name);
    if (!behavior_dev) {
        send_response(ZEN_CONFIG_CMD_SET_BINDING, ZEN_CONFIG_STATUS_ERR_UNKNOWN_BEHAVIOR, NULL, 0);
        return;
    }

    struct zmk_behavior_binding binding = {
        .behavior_dev = behavior_dev->name,
        .param1 = sys_get_le32(&binding_entry[ZEN_CONFIG_WIRE_BEHAVIOR_NAME_LEN]),
        .param2 = sys_get_le32(&binding_entry[ZEN_CONFIG_WIRE_BEHAVIOR_NAME_LEN + 4]),
    };

    if (zmk_behavior_validate_binding(&binding) < 0) {
        send_response(ZEN_CONFIG_CMD_SET_BINDING, ZEN_CONFIG_STATUS_ERR_INVALID_PAYLOAD, NULL, 0);
        return;
    }

    int rc = zmk_keymap_set_layer_binding_at_idx(layer_id, key_position, binding);
    if (rc < 0) {
        uint8_t status = (rc == -EINVAL) ? ZEN_CONFIG_STATUS_ERR_INVALID_LOCATION
                                         : ZEN_CONFIG_STATUS_ERR_GENERIC;
        send_response(ZEN_CONFIG_CMD_SET_BINDING, status, NULL, 0);
        return;
    }

    send_response(ZEN_CONFIG_CMD_SET_BINDING, ZEN_CONFIG_STATUS_OK, NULL, 0);
}

static void handle_save_keymap(void) {
    int rc = zmk_keymap_save_changes();
    uint8_t status = rc == 0 ? ZEN_CONFIG_STATUS_OK
                    : rc == -ENOTSUP ? ZEN_CONFIG_STATUS_ERR_NOT_SUPPORTED
                                     : ZEN_CONFIG_STATUS_ERR_GENERIC;
    send_response(ZEN_CONFIG_CMD_SAVE_KEYMAP, status, NULL, 0);
}

static void handle_discard_keymap(void) {
    int rc = zmk_keymap_discard_changes();
    uint8_t status = rc == 0 ? ZEN_CONFIG_STATUS_OK
                    : rc == -ENOTSUP ? ZEN_CONFIG_STATUS_ERR_NOT_SUPPORTED
                                     : ZEN_CONFIG_STATUS_ERR_GENERIC;
    send_response(ZEN_CONFIG_CMD_DISCARD_KEYMAP, status, NULL, 0);
}

static void handle_reset_keymap(void) {
    /* Deletes every saved binding, layer name and layer order, then reloads
     * the stock keymap -- ZMK does the whole job in one call. */
    int rc = zmk_keymap_reset_settings();
    uint8_t status = rc == 0 ? ZEN_CONFIG_STATUS_OK
                    : rc == -ENOTSUP ? ZEN_CONFIG_STATUS_ERR_NOT_SUPPORTED
                                     : ZEN_CONFIG_STATUS_ERR_GENERIC;
    send_response(ZEN_CONFIG_CMD_RESET_KEYMAP, status, NULL, 0);
}

#else /* split peripheral: no local keymap to read or edit */

static void handle_get_keymap_meta(void) {
    send_response(ZEN_CONFIG_CMD_GET_KEYMAP_META, ZEN_CONFIG_STATUS_ERR_NOT_SUPPORTED, NULL, 0);
}

static void handle_get_layer(const uint8_t *payload) {
    ARG_UNUSED(payload);
    send_response(ZEN_CONFIG_CMD_GET_LAYER, ZEN_CONFIG_STATUS_ERR_NOT_SUPPORTED, NULL, 0);
}

static void handle_set_binding(const uint8_t *payload) {
    ARG_UNUSED(payload);
    send_response(ZEN_CONFIG_CMD_SET_BINDING, ZEN_CONFIG_STATUS_ERR_NOT_SUPPORTED, NULL, 0);
}

static void handle_save_keymap(void) {
    send_response(ZEN_CONFIG_CMD_SAVE_KEYMAP, ZEN_CONFIG_STATUS_ERR_NOT_SUPPORTED, NULL, 0);
}

static void handle_discard_keymap(void) {
    send_response(ZEN_CONFIG_CMD_DISCARD_KEYMAP, ZEN_CONFIG_STATUS_ERR_NOT_SUPPORTED, NULL, 0);
}

static void handle_reset_keymap(void) {
    send_response(ZEN_CONFIG_CMD_RESET_KEYMAP, ZEN_CONFIG_STATUS_ERR_NOT_SUPPORTED, NULL, 0);
}

#endif /* !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */

static void handle_command(uint8_t command, const uint8_t *payload) {
    switch (command) {
    case ZEN_CONFIG_CMD_GET_POINTER:
        handle_get_pointer();
        break;
    case ZEN_CONFIG_CMD_SET_POINTER:
        handle_set_pointer(payload);
        break;
    case ZEN_CONFIG_CMD_SAVE_POINTER:
        handle_save_pointer();
        break;
    case ZEN_CONFIG_CMD_RESET_POINTER:
        handle_reset_pointer();
        break;
    case ZEN_CONFIG_CMD_GET_AUTO_MOUSE:
        handle_get_auto_mouse();
        break;
    case ZEN_CONFIG_CMD_SET_AUTO_MOUSE:
        handle_set_auto_mouse(payload);
        break;
    case ZEN_CONFIG_CMD_GET_STATUS:
        handle_get_status();
        break;
    case ZEN_CONFIG_CMD_GET_BLE_PROFILES:
        handle_get_ble_profiles();
        break;
    case ZEN_CONFIG_CMD_BLE_ACTION:
        handle_ble_action(payload);
        break;
    case ZEN_CONFIG_CMD_GET_KEYMAP_META:
        handle_get_keymap_meta();
        break;
    case ZEN_CONFIG_CMD_GET_LAYER:
        handle_get_layer(payload);
        break;
    case ZEN_CONFIG_CMD_SET_BINDING:
        handle_set_binding(payload);
        break;
    case ZEN_CONFIG_CMD_SAVE_KEYMAP:
        handle_save_keymap();
        break;
    case ZEN_CONFIG_CMD_DISCARD_KEYMAP:
        handle_discard_keymap();
        break;
    case ZEN_CONFIG_CMD_RESET_KEYMAP:
        handle_reset_keymap();
        break;
    default:
        send_response(command, ZEN_CONFIG_STATUS_ERR_UNKNOWN_COMMAND, NULL, 0);
        break;
    }
}

static void ingest_byte(uint8_t byte) {
    if (rx_pos >= ARRAY_SIZE(rx_buffer)) {
        reset_rx_buffer();
    }

    rx_buffer[rx_pos++] = byte;

    while (rx_pos >= 4 && !rx_magic_matches()) {
        slide_rx_buffer();
    }

    size_t frame_size = expected_frame_size();
    if (frame_size == SIZE_MAX) {
        LOG_WRN("Rejected malformed config bridge frame");
        slide_rx_buffer();
        return;
    }

    if (frame_size > 0 && rx_pos == frame_size) {
        uint8_t command = rx_buffer[5];
        handle_command(command, &rx_buffer[ZEN_CONFIG_WIRE_HEADER_SIZE]);
        reset_rx_buffer();
    }
}

/* DTR tells us whether a host actually has the CDC-ACM port open, as opposed
 * to it merely being enumerated. Back off to an idle poll rate when nothing
 * is attached instead of spinning every 250us for the board's whole runtime. */
static bool zen_config_host_attached(void) {
    int dtr = 0;
    return uart_line_ctrl_get(zen_config_serial, UART_LINE_CTRL_DTR, &dtr) == 0 && dtr;
}

static void zen_config_serial_bridge_thread(void) {
    if (!zen_config_serial) {
        LOG_WRN("No shell/console UART chosen for config serial bridge");
        return;
    }

    if (!device_is_ready(zen_config_serial)) {
        LOG_WRN("Config serial bridge UART is not ready");
        return;
    }

    LOG_INF("Config serial bridge listening on chosen UART");

    while (true) {
        if (!zen_config_host_attached()) {
            reset_rx_buffer();
            k_sleep(K_MSEC(200));
            continue;
        }

        uint8_t byte = 0;
        bool read_any = false;

        while (uart_poll_in(zen_config_serial, &byte) == 0) {
            ingest_byte(byte);
            read_any = true;
        }

        if (!read_any) {
            k_sleep(K_USEC(250));
        }
    }
}

K_THREAD_DEFINE(zen_config_serial_bridge_tid, 2048, zen_config_serial_bridge_thread, NULL, NULL,
                NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 800);
