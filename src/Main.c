// Copyright (c) 2015-2019 The HomeKit ADK Contributors
//
// Licensed under the Apache License, Version 2.0 (the “License”);
// you may not use this file except in compliance with the License.
// See [CONTRIBUTORS.md] for the list of HomeKit ADK project authors.

#include "App.h"
#include "DB.h"

#include "HAP.h"
#include "HAPPlatform+Init.h"
#include "HAPPlatformAccessorySetup+Init.h"
#include "HAPPlatformAccessorySetupDisplay+Init.h"
#if BLE
#include "HAPPlatformBLEPeripheralManager+Init.h"
#endif
#include "HAPPlatformKeyValueStore+Init.h"
#if IP
#include "HAPPlatformServiceDiscovery+Init.h"
#include "HAPPlatformTCPStreamManager+Init.h"
#endif

#include "HAP+Internal.h"

#include "mgos.h"
#include "mgos_hap.h"

#include "qrcode.h"
#include "math.h"
#include "mgos_http_server.h"

static bool requestedFactoryReset = false;
static bool clearPairings = false;

#define MAX_NUM_SESSIONS 9

#define PREFERRED_ADVERTISING_INTERVAL (HAPBLEAdvertisingIntervalCreateFromMilliseconds(417.5f))

/**
 * Global platform objects.
 * Only tracks objects that will be released in DeinitializePlatform.
 */
static struct {
    HAPPlatformKeyValueStore keyValueStore;
    HAPAccessoryServerOptions hapAccessoryServerOptions;
    HAPPlatform hapPlatform;
    HAPAccessoryServerCallbacks hapAccessoryServerCallbacks;

#if IP
    HAPPlatformTCPStreamManager tcpStreamManager;
#endif

} platform;

/**
 * HomeKit accessory server that hosts the accessory.
 */
static HAPAccessoryServerRef accessoryServer;

void HandleUpdatedState(HAPAccessoryServerRef* _Nonnull server, void* _Nullable context);

/**
 * Functions provided by App.c for each accessory application.
 */
extern void AppRelease(void);
extern void AppCreate(HAPAccessoryServerRef* server, HAPPlatformKeyValueStoreRef keyValueStore);
extern void AppInitialize(
        HAPAccessoryServerOptions* hapAccessoryServerOptions,
        HAPPlatform* hapPlatform,
        HAPAccessoryServerCallbacks* hapAccessoryServerCallbacks);
extern void AppDeinitialize();
extern void AppAccessoryServerStart(void);
extern void AccessoryServerHandleUpdatedState(HAPAccessoryServerRef* server, void* _Nullable context);

static void timer_cb(void* arg) {
    static bool s_tick_tock = false;
    LOG(LL_INFO,
        ("%s uptime: %.2lf, RAM: %lu, %lu free",
         (s_tick_tock ? "Tick" : "Tock"),
         mgos_uptime(),
         (unsigned long) mgos_get_heap_size(),
         (unsigned long) mgos_get_free_heap_size()));
    s_tick_tock = !s_tick_tock;
    (void) arg;
}

uint64_t base36ToLong(const char* base36String) {

    int len = strlen(base36String);

    uint64_t result = 0;

    int i = 0;
    while (len--) {
        char* character = (char*) base36String + len;

        int value;
        if (*character > 0x39) {
            value = *character - 'A' + 10;
        } else {
            value = *character - '0';
        }

        result += value * pow(36, i++);
    }

    return result;
}

/// Extracts and returns the setup code from the setup payload
/// @param setupPayload A 20 character long HAP setupPayload URI
uint64_t codeFromSetupPayload(const char* setupPayload) {
    if ((strlen(setupPayload)) == 20 && (strncmp(setupPayload, "X-HM://", 7) == 0)) {

        char setupCode[10] = { 0 }; // 9 chars + terminator */

        // extract setupCode and flags
        const char* payload = setupPayload + 7;

        for (int i = 0; i < 9; i++) {
            setupCode[i] = *payload++;
        }

        uint64_t code = base36ToLong(setupCode) & 0x7ffffff;

        return code;
    }

    return 0;
}

/// Outputs a SVG path from qrcode data
/// @param connection the opened mongoose connection
/// @param qrcode QRCode representation
/// @param x_offset The X offset of the left upper edge of the qrcode path
/// @param y_offset The y offset of the left upper edge of the qrcode path
/// @param width The width of the qrcode representation to be drawn
void qrcodeSVGPath(struct mg_connection* connection, QRCode qrcode, float x_offset, float y_offset, float width) {

    mg_printf(connection, "<path d=\"");

    int border = 1;

    float offsetX = x_offset;
    float offsetY = y_offset;

    float scale = width / (qrcode.size + 2 * border);

    offsetX = offsetX + (border * scale);
    offsetY = offsetY + (border * scale);

    for (uint8_t y = 0; y < qrcode.size; y++) {
        // Each horizontal module
        for (uint8_t x = 0; x < qrcode.size; x++) {
            if (qrcode_getModule(&qrcode, x, y)) {
                mg_printf(
                        connection,
                        "M%.3f,%.3fh%.3fv%.3fh-%.3fz",
                        offsetX + scale * x,
                        offsetY + scale * y,
                        scale,
                        scale,
                        scale);
            }
        }
    }

    mg_printf(connection, "\"/>");
    return;
}

/// creates a HomeKit badge for pairing from setupPayload URI
/// @param setupPayload A 20 character long HAP setupPayload URI
void SVGBadgeFromSetupPayload(const char* setupPayload, struct mg_connection* connection) {

    // check for valid setupPayload URI
    if ((strlen(setupPayload)) == 20 && (strncmp(setupPayload, "X-HM://", 7) == 0)) {

        uint64_t code = codeFromSetupPayload(setupPayload);
        if (code > 99999999) {
            LOG(LL_ERROR, ("%s: Code exceeds the limits of a valid setup code.", __func__));
            return;
        }

        mg_printf(connection, "<?xml version=\"1.0\" encoding=\"utf-8\"?>");
        mg_printf(
                connection,
                "<svg version=\"1.1\" id=\"homekit-badge\" xmlns=\"http://www.w3.org/2000/svg\" "
                "xmlns:xlink=\"http://www.w3.org/1999/xlink\" x=\"0px\" y=\"0px\" viewBox=\"0 0 180 250\" "
                "style=\"enable-background:new 0 0 180 250;\" xml:space=\"preserve\">");
        mg_printf(
                connection,
                "<style "
                "type=\"text/"
                "css\">.st0{fill:#FFFFFF;stroke:#221E1F;stroke-width:5;}.st1{fill:#221E1F;stroke:#221E1F}.st2{fill:#"
                "FFFFFF;}</style>");
        mg_printf(
                connection,
                "<g><rect x=\"2.5\" y=\"2.5\" width=\"180\" height=\"245\" rx=\"20\" ry=\"20\" class=\"st0\" />");
        mg_printf(
                connection,
                "<g><path id=\"_Compound_Path_1_1_\" class=\"st1\" "
                "d=\"M69.5,31l-6.6-5.3v-9.4c0-0.7-0.3-0.9-0.8-0.9h-4.2c-0.6,0-0.9,0.1-0.9,0.9v4.8l0,0L41,8.5c-0.4-0.4-"
                "0.9-0.6-1.4-0.6c-0.5,0-1,0.2-1.4,0.6L9.7,31c-1,0.8-0.7,1.9,0.4,1.9h5.3v28.6c0,1.9,0.7,2.6,2.5,2.6h43."
                "4c1.8,0,2.5-0.7,2.5-2.6V32.9h5.3C70.2,32.9,70.5,31.8,69.5,31z "
                "M60.3,58.1c0.2,1.1-0.6,2.1-1.6,2.2c-0.1,0-0.3,0-0.4,0H20.9c-1.1,0.1-2-0.7-2.1-1.8c0-0.1,0-0.3,0-0."
                "4V30.4c0-1.3,0.5-2.5,1.5-3.3l18-14c0.3-0.3,0.8-0.5,1.3-0.5c0.5,0,0.9,0.2,1.3,0.5l18,14c1,0.8,1.5,2,1."
                "5,3.3V58.1z\"/>");
        mg_printf(
                connection,
                "<path id=\"_Compound_Path_2_1_\" class=\"st1\" "
                "d=\"M53.1,30.4l-12.6-10c-0.3-0.2-0.6-0.4-1-0.4c-0.4,0-0.7,0.1-1,0.4L26,30.4c-0.7,0.5-1.1,1.4-1,2.3v19."
                "9c-0.1,0.8,0.5,1.5,1.3,1.6c0.1,0,0.2,0,0.3,0h26c0.8,0.1,1.5-0.5,1.6-1.3c0-0.1,0-0.2,0-0.3V32.8C54.3,"
                "31.9,53.9,31,53.1,30.4z "
                "M50.6,49.2c0.1,0.6-0.3,1.2-1,1.3c-0.1,0-0.2,0-0.3,0H29.8c-0.6,0.1-1.2-0.4-1.3-1.1c0-0.1,0-0.2,0-0."
                "3V34.1c-0.1-0.7,0.2-1.4,0.7-1.8l9.5-7.5c0.2-0.2,0.5-0.3,0.8-0.3c0.3,0,0.6,0.1,0.8,0.3c0.3,0.2,9,7.1,9."
                "4,7.5c0.6,0.4,0.8,1.1,0.7,1.8V49.2z\"/>");
        mg_printf(
                connection,
                "<path id=\"_Compound_Path_3_1_\" class=\"st1\" "
                "d=\"M40.1,31.3c-0.2-0.1-0.3-0.2-0.5-0.2c-0.2,0-0.4,0.1-0.5,0.2c-0.2,0.1-4.8,3.6-5,3.8c-0.4,0.3-0.6,0."
                "8-0.6,1.2v8.5c0,0.7,0.4,0.8,0.8,0.8h10.5c0.5,0,0.8-0.2,0.8-0.8v-8.5c0-0.5-0.2-0.9-0.6-1.2C44.9,34.9,"
                "40.3,31.4,40.1,31.3z "
                "M42.1,41.7c0,0.3-0.1,0.4-0.3,0.4h-4.3c-0.2,0-0.3-0.1-0.3-0.4v-4c0-0.2,0.1-0.4,0.2-0.5l2-1.6c0.1-0.1,0."
                "1-0.1,0.2-0.1c0.1,0,0.2,0,0.2,0.1l2,1.6c0.2,0.1,0.2,0.3,0.2,0.5L42.1,41.7z\"/></g>");

        char codeString[8 + 1] = { 0 };
        snprintf(codeString, sizeof(codeString), "%llu", code);

        // split the string in halfs for badge
        char codeFirstHalf[5] = { 0 };  // 4 chars + \0 terminator
        char codeSecondHalf[5] = { 0 }; // same here ...

        strncpy(codeFirstHalf, codeString, 4);
        strncpy(codeSecondHalf, codeString + 4, 4);

        mg_printf(
                connection,
                "<text x=\"75.5\" y=\"29.5\" font-family=\"SF Mono, Menlo, monospace\" font-weight=\"bold\" "
                "letter-spacing=\"8\" font-size=\"28\" class=\"st1\"><tspan x=\"75.5\" y=\"29.5\">%s</tspan><tspan "
                "x=\"75.5\" y=\"52\">%s</tspan></text>",
                codeFirstHalf,
                codeSecondHalf);
        mg_printf(connection, "<rect x=\"10\" y=\"74\" class=\"st2\" width=\"165\" height=\"165\"/>");

        QRCode qrcode;

        uint8_t qrcodeData[qrcode_getBufferSize(3)];
        qrcode_initText(&qrcode, qrcodeData, 3, 2, setupPayload);

        qrcodeSVGPath(connection, qrcode, 10.0, 74.0, 165.0);

        mg_printf(connection, "</g></svg>");
    }

    return;
}

static void HttpSetupHandler(struct mg_connection* c, int ev, void* p, void* user_data) {

    (void) p;

    if (ev != MG_EV_HTTP_REQUEST)
        return;

    if (platform.hapPlatform.setupDisplay->setupPayloadIsSet) {
        // get payload
        const char* payload = platform.hapPlatform.setupDisplay->setupPayload.stringValue;

        // we`ll be sending a svg image
        mg_send_response_line(c, 200, "Content-Type: image/svg+xml\r\n");

        SVGBadgeFromSetupPayload(payload, c);

    } else {
        mg_send_response_line(c, 200, "Content-Type: text/text\r\n");
        mg_printf(c, "%s\r\n", "No setup payload is set. Already paired?");
    }

    c->flags |= MG_F_SEND_AND_CLOSE;
    (void) user_data;
}

static const HAPLogObject logObject = { .subsystem = kHAP_LogSubsystem, .category = "QRCode" };

static void UpdateSetupPayloadEventHandler(int ev, void* ev_data, void* userdata) {

    struct mgos_hap_display_update_setup_payload_arg* arg = (struct mgos_hap_display_update_setup_payload_arg*) ev_data;

    if (arg->setupCode) {
        HAPLogInfo(&logObject, "##### Setup code for display: %s", arg->setupCode->stringValue);
        HAPRawBufferCopyBytes(
                &arg->setupDisplay->setupCode, HAPNonnull(arg->setupCode), sizeof arg->setupDisplay->setupCode);
        arg->setupDisplay->setupCodeIsSet = true;
    } else {
        HAPLogInfo(&logObject, "##### Setup code for display invalidated.");
        HAPRawBufferZero(&arg->setupDisplay->setupCode, sizeof arg->setupDisplay->setupCode);
        arg->setupDisplay->setupCodeIsSet = false;
    }
    if (arg->setupPayload) {
        HAPLogInfo(&logObject, "##### Setup payload for QR code display: %s", arg->setupPayload->stringValue);
        HAPRawBufferCopyBytes(
                &arg->setupDisplay->setupPayload,
                HAPNonnull(arg->setupPayload),
                sizeof arg->setupDisplay->setupPayload);
        arg->setupDisplay->setupPayloadIsSet = true;
    } else {
        HAPRawBufferZero(&arg->setupDisplay->setupPayload, sizeof arg->setupDisplay->setupPayload);
        arg->setupDisplay->setupPayloadIsSet = false;
    }
}

/**
 * Initialize global platform objects.
 */
static void InitializePlatform() {
    // Key-value store.
    HAPPlatformKeyValueStoreCreate(
            &platform.keyValueStore, &(const HAPPlatformKeyValueStoreOptions) { .fileName = "kv.json" });
    platform.hapPlatform.keyValueStore = &platform.keyValueStore;

    // Accessory setup manager. Depends on key-value store.
    static HAPPlatformAccessorySetup accessorySetup;
    HAPPlatformAccessorySetupCreate(&accessorySetup, &(const HAPPlatformAccessorySetupOptions) {});
    platform.hapPlatform.accessorySetup = &accessorySetup;

    // Display
    static HAPPlatformAccessorySetupDisplay setupDisplay;
    HAPPlatformAccessorySetupDisplayCreate(&setupDisplay);
    platform.hapPlatform.setupDisplay = &setupDisplay;

#if IP
    // TCP stream manager.
    HAPPlatformTCPStreamManagerCreate(
            &platform.tcpStreamManager,
            &(const HAPPlatformTCPStreamManagerOptions) { .port = kHAPNetworkPort_Any, // Listen on unused port number
                                                                                       // from the ephemeral port range.
                                                          .maxConcurrentTCPStreams = MAX_NUM_SESSIONS });

    // Service discovery.
    static HAPPlatformServiceDiscovery serviceDiscovery;
    HAPPlatformServiceDiscoveryCreate(&serviceDiscovery, &(const HAPPlatformServiceDiscoveryOptions) {});
    platform.hapPlatform.ip.serviceDiscovery = &serviceDiscovery;
#endif

#if BLE
    // BLE peripheral manager. Depends on key-value store.
    static HAPPlatformBLEPeripheralManagerOptions blePMOptions = { 0 };
    blePMOptions.keyValueStore = &platform.keyValueStore;

    static HAPPlatformBLEPeripheralManager blePeripheralManager;
    HAPPlatformBLEPeripheralManagerCreate(&blePeripheralManager, &blePMOptions);
    platform.hapPlatform.ble.blePeripheralManager = &blePeripheralManager;
#endif

    // Run loop.
    // HAPPlatformRunLoopCreate(&(const HAPPlatformRunLoopOptions) {
    // .keyValueStore = &platform.keyValueStore });

    platform.hapAccessoryServerOptions.maxPairings = kHAPPairingStorage_MinElements;

    platform.hapAccessoryServerCallbacks.handleUpdatedState = HandleUpdatedState;

    mgos_set_timer(1000, MGOS_TIMER_REPEAT, timer_cb, NULL);

    mgos_event_add_handler(MGOS_HAP_EV_DISPLAY_UPDATE_SETUP_PAYLOAD, UpdateSetupPayloadEventHandler, NULL);
}

/**
 * Deinitialize global platform objects.
 */
void DeinitializePlatform() {
#if IP
    // TCP stream manager.
    HAPPlatformTCPStreamManagerRelease(&platform.tcpStreamManager);
#endif

    AppDeinitialize();

    // Run loop.
    // HAPPlatformRunLoopRelease();
}

/**
 * Restore platform specific factory settings.
 */
void RestorePlatformFactorySettings(void) {
}

/**
 * Either simply passes State handling to app, or processes Factory Reset
 */
void HandleUpdatedState(HAPAccessoryServerRef* _Nonnull server, void* _Nullable context) {
    if (HAPAccessoryServerGetState(server) == kHAPAccessoryServerState_Idle && requestedFactoryReset) {
        HAPPrecondition(server);

        HAPError err;

        HAPLogInfo(&kHAPLog_Default, "A factory reset has been requested.");

        // Purge app state.
        err = HAPPlatformKeyValueStorePurgeDomain(&platform.keyValueStore, ((HAPPlatformKeyValueStoreDomain) 0x00));
        if (err) {
            HAPAssert(err == kHAPError_Unknown);
            HAPFatalError();
        }

        // Reset HomeKit state.
        err = HAPRestoreFactorySettings(&platform.keyValueStore);
        if (err) {
            HAPAssert(err == kHAPError_Unknown);
            HAPFatalError();
        }

        // Restore platform specific factory settings.
        RestorePlatformFactorySettings();

        // De-initialize App.
        AppRelease();

        requestedFactoryReset = false;

        // Re-initialize App.
        AppCreate(server, &platform.keyValueStore);

        // Restart accessory server.
        AppAccessoryServerStart();
        return;
    } else if (HAPAccessoryServerGetState(server) == kHAPAccessoryServerState_Idle && clearPairings) {
        HAPError err;
        err = HAPRemoveAllPairings(&platform.keyValueStore);
        if (err) {
            HAPAssert(err == kHAPError_Unknown);
            HAPFatalError();
        }
        AppAccessoryServerStart();
    } else {
        AccessoryServerHandleUpdatedState(server, context);
    }
}

#if IP
static void InitializeIP() {
    // Prepare accessory server storage.
    static HAPIPSession ipSessions[MAX_NUM_SESSIONS];
    static uint8_t ipScratchBuffer[1536];
    static HAPIPAccessoryServerStorage ipAccessoryServerStorage = {
        .sessions = ipSessions,
        .numSessions = HAPArrayCount(ipSessions),
        .scratchBuffer = { .bytes = ipScratchBuffer, .numBytes = sizeof ipScratchBuffer }
    };

    platform.hapAccessoryServerOptions.ip.transport = &kHAPAccessoryServerTransport_IP;
    platform.hapAccessoryServerOptions.ip.accessoryServerStorage = &ipAccessoryServerStorage;

    platform.hapPlatform.ip.tcpStreamManager = &platform.tcpStreamManager;

    mgos_register_http_endpoint("/homekit/pairing", HttpSetupHandler, NULL);
}
#endif

#if BLE
static void InitializeBLE() {
    static HAPBLEGATTTableElementRef gattTableElements[kAttributeCount];
    static HAPBLESessionCacheElementRef sessionCacheElements[kHAPBLESessionCache_MinElements];
    static HAPSessionRef session;
    static uint8_t procedureBytes[3072];
    static HAPBLEProcedureRef procedures[1];

    static HAPBLEAccessoryServerStorage bleAccessoryServerStorage = {
        .gattTableElements = gattTableElements,
        .numGATTTableElements = HAPArrayCount(gattTableElements),
        .sessionCacheElements = sessionCacheElements,
        .numSessionCacheElements = HAPArrayCount(sessionCacheElements),
        .session = &session,
        .procedures = procedures,
        .numProcedures = HAPArrayCount(procedures),
        .procedureBuffer = { .bytes = procedureBytes, .numBytes = sizeof procedureBytes }
    };

    platform.hapAccessoryServerOptions.ble.transport = &kHAPAccessoryServerTransport_BLE;
    platform.hapAccessoryServerOptions.ble.accessoryServerStorage = &bleAccessoryServerStorage;
    platform.hapAccessoryServerOptions.ble.preferredAdvertisingInterval = PREFERRED_ADVERTISING_INTERVAL;
    platform.hapAccessoryServerOptions.ble.preferredNotificationDuration = kHAPBLENotification_MinDuration;
}
#endif

enum mgos_app_init_result mgos_app_init(void) {
    HAPAssert(HAPGetCompatibilityVersion() == HAP_COMPATIBILITY_VERSION);

    // Initialize global platform objects.
    InitializePlatform();

#if IP
    InitializeIP();
#endif

#if BLE
    InitializeBLE();
#endif

    // Perform Application-specific initalizations such as setting up callbacks
    // and configure any additional unique platform dependencies
    AppInitialize(&platform.hapAccessoryServerOptions, &platform.hapPlatform, &platform.hapAccessoryServerCallbacks);

    // Initialize accessory server.
    HAPAccessoryServerCreate(
            &accessoryServer,
            &platform.hapAccessoryServerOptions,
            &platform.hapPlatform,
            &platform.hapAccessoryServerCallbacks,
            /* context: */ NULL);

    // Create app object.
    AppCreate(&accessoryServer, &platform.keyValueStore);

    // Start accessory server for App.
    AppAccessoryServerStart();

    mgos_hap_add_rpc_service(&accessoryServer, AppGetAccessoryInfo());

    return MGOS_APP_INIT_SUCCESS;
}
