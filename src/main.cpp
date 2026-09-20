// RHEA1-F4B: compact natural-language router over the validated E4 deterministic tool dispatcher.
//
// Production startup contract:
//   * Mount SD at 10 MHz.
//   * Validate only the small snapshot envelope.
//   * Keep Wi-Fi OFF.
//   * Present the local query shell immediately.
//   * Contact Azure ONLY when the user enters "update" or "update records".
//
// "diagnostics" runs the exhaustive E1/E2/E3 validation on demand.
// The trusted-TLS/hash/reread/recoverable-install update path is inherited
// from the previously validated private-device sync firmware.

#include <Arduino.h>

// FLORA1 stack-recovery: Arduino-ESP32 defaults loopTask to 8 KiB.
// TLS + snapshot/update paths have demonstrated a loopTask stack canary
// on the Cardputer ADV after the credential rebuild. Give loopTask 16 KiB.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <mbedtls/sha256.h>

#include "secrets.h"
#include "rhea_intent_model.h"
#include "flora_avatar_64.h"

#define SD_SPI_SCK_PIN   40
#define SD_SPI_MISO_PIN  39
#define SD_SPI_MOSI_PIN  14
#define SD_SPI_CS_PIN    12

const uint32_t SD_SPI_FREQUENCY_HZ = 10000000;

// FLORA1-DUAL-DISPLAY-MIRROR rev4
// External ILI9341 shares the SD SPI bus but has its own chip-select.
#define EXT_TFT_RST_PIN   3
#define EXT_TFT_CS_PIN    5
#define EXT_TFT_DC_PIN    6

constexpr uint32_t EXT_TFT_SPI_FREQUENCY_HZ = 10000000;
constexpr int EXT_TFT_WIDTH = 320;
constexpr int EXT_TFT_HEIGHT = 240;
constexpr int BUILTIN_MIRROR_WIDTH = 240;
constexpr int BUILTIN_MIRROR_HEIGHT = 135;
constexpr int EXT_TFT_MIRROR_X =
    (EXT_TFT_WIDTH - BUILTIN_MIRROR_WIDTH) / 2;
constexpr int EXT_TFT_MIRROR_Y =
    (EXT_TFT_HEIGHT - BUILTIN_MIRROR_HEIGHT) / 2;

Adafruit_ILI9341 externalTft(
    &SPI,
    EXT_TFT_DC_PIN,
    EXT_TFT_CS_PIN,
    EXT_TFT_RST_PIN
);

class FloraMirroredDisplay
{
public:
    // Keep history bounded in RAM. The visible viewport is identical on both
    // displays; the external panel simply centers the same 240x135 surface.
    static constexpr size_t CHAT_HISTORY_LINES = 64;
    static constexpr size_t CHAT_VISIBLE_LINES = 14;
    static constexpr size_t CHAT_COLUMNS = 38;

    enum class ChatTone : uint8_t
    {
        User,
        Flora,
        Text,
        Secondary,
        Error
    };

    struct ChatLine
    {
        String text;
        ChatTone tone = ChatTone::Text;
    };

    void enableExternal()
    {
        externalReady_ = true;
    }

    void showReady()
    {
        clearChat();

        pushWrappedLine(
            "F L O R A",
            ChatTone::Flora
        );
        pushWrappedLine(
            "Poppy's pocket sister",
            ChatTone::Flora
        );
        pushWrappedLine(
            "--------------------",
            ChatTone::Secondary
        );
        pushWrappedLine(
            "Hi! I'm Flora.",
            ChatTone::Text
        );
        pushWrappedLine(
            "Ask me about your collection.",
            ChatTone::Text
        );

        renderChat("");
    }

    void showInput(
        const String &input)
    {
        if (idleMode_)
        {
            return;
        }

        scrollOffsetFromBottom_ = 0;
        renderChat(input);
    }

    void scrollUp(
        const String &input)
    {
        if (idleMode_)
        {
            return;
        }

        const size_t maxOffset =
            chatCount_ > CHAT_VISIBLE_LINES
                ? chatCount_ - CHAT_VISIBLE_LINES
                : 0;

        if (scrollOffsetFromBottom_ < maxOffset)
        {
            ++scrollOffsetFromBottom_;
            renderChat(input);
        }
    }

    void scrollDown(
        const String &input)
    {
        if (idleMode_)
        {
            return;
        }

        if (scrollOffsetFromBottom_ > 0)
        {
            --scrollOffsetFromBottom_;
            renderChat(input);
        }
    }

    void beginChatTurn(
        const String &userText)
    {
        idleMode_ = false;
        scrollOffsetFromBottom_ = 0;
        capturing_ = true;
        captureLine_ = "";
        captureTone_ = ChatTone::Text;

        if (chatCount_ > 0)
        {
            pushLine(
                "",
                ChatTone::Text
            );
        }

        pushWrappedLine(
            String("You: ") + userText,
            ChatTone::User
        );

        pushLine(
            "Flora:",
            ChatTone::Flora
        );

        renderChat("");
    }

    void finishChatTurn()
    {
        if (!capturing_)
        {
            return;
        }

        flushCaptureLine();

        capturing_ = false;
        captureLine_ = "";

        renderChat("");
    }

    void showIdle(
        const uint16_t *frame)
    {
        idleMode_ = true;

        clearMirrorSurface();

        // Native 64x64 RGB565 avatar on the same 240x135 coordinates
        // on both displays.
        constexpr int avatarX = 88;
        constexpr int avatarY = 20;

        for (int row = 0;
             row < FLORA_AVATAR_64_HEIGHT;
             ++row)
        {
            for (int column = 0;
                 column < FLORA_AVATAR_64_WIDTH;
                 ++column)
            {
                const size_t index =
                    static_cast<size_t>(row) *
                        FLORA_AVATAR_64_WIDTH +
                    column;

                const uint16_t color =
                    pgm_read_word(
                        &frame[index]
                    );

                M5Cardputer.Display.drawPixel(
                    avatarX + column,
                    avatarY + row,
                    color
                );
            }
        }

        if (externalReady_)
        {
            externalTft.drawRGBBitmap(
                EXT_TFT_MIRROR_X + avatarX,
                EXT_TFT_MIRROR_Y + avatarY,
                frame,
                FLORA_AVATAR_64_WIDTH,
                FLORA_AVATAR_64_HEIGHT
            );
        }

        drawMirroredText(
            57,
            116,
            "Flora | offline & ready",
            ChatTone::Text
        );
    }

    void leaveIdleAndRestore(
        const String &input)
    {
        idleMode_ = false;
        scrollOffsetFromBottom_ = 0;
        renderChat(input);
    }

    // Compatibility surface for the existing UI functions. During command
    // execution, those functions are used only as a text source for the
    // rolling chat. Their frame-clearing/cursor operations must not mutate
    // either physical display.
    void setRotation(
        uint8_t rotation)
    {
        M5Cardputer.Display.setRotation(
            rotation
        );
    }

    void fillScreen(
        uint32_t)
    {
        // Intentionally ignored. renderChat()/showIdle() own full redraws.
    }

    void setCursor(
        int32_t,
        int32_t)
    {
        // Intentionally ignored by the rolling chat renderer.
    }

    void setTextColor(
        uint32_t foreground,
        uint32_t)
    {
        // Preserve logical intent without passing display-library color
        // encodings between LovyanGFX and Adafruit_GFX.
        if (foreground == GREEN)
        {
            captureTone_ =
                ChatTone::Flora;
        }
        else if (foreground == YELLOW)
        {
            captureTone_ =
                ChatTone::User;
        }
        else
        {
            captureTone_ =
                ChatTone::Text;
        }
    }

    void setTextColor(
        uint32_t foreground)
    {
        setTextColor(
            foreground,
            BLACK
        );
    }

    void setTextSize(
        uint8_t)
    {
        // Mirrored chat always uses size 1 so both displays have identical
        // line geometry.
    }

    void setTextWrap(
        bool)
    {
        // Wrapping is handled explicitly by pushWrappedLine().
    }

    void drawPixel(
        int32_t,
        int32_t,
        uint16_t)
    {
        // Avatar drawing is handled atomically by showIdle().
    }

    void mirrorRgb565Bitmap(
        int32_t,
        int32_t,
        const uint16_t *,
        int16_t,
        int16_t)
    {
        // Compatibility no-op. showIdle() owns mirrored avatar drawing.
    }

    template <typename T>
    size_t print(
        const T &value)
    {
        const String textValue(value);

        if (capturing_)
        {
            appendCaptureText(
                textValue
            );
        }

        return textValue.length();
    }

    size_t print(
        const String &value)
    {
        if (capturing_)
        {
            appendCaptureText(
                value
            );
        }

        return value.length();
    }

    size_t print(
        const char *value)
    {
        if (value == nullptr)
        {
            return 0;
        }

        const String textValue(value);

        if (capturing_)
        {
            appendCaptureText(
                textValue
            );
        }

        return textValue.length();
    }

    size_t print(
        double value,
        int digits)
    {
        const String textValue(
            value,
            digits
        );

        if (capturing_)
        {
            appendCaptureText(
                textValue
            );
        }

        return textValue.length();
    }

    size_t print(
        float value,
        int digits)
    {
        return print(
            static_cast<double>(value),
            digits
        );
    }

    size_t println()
    {
        if (capturing_)
        {
            flushCaptureLine();
            renderChat("");
        }

        return 1;
    }

    template <typename T>
    size_t println(
        const T &value)
    {
        const String textValue(value);

        if (capturing_)
        {
            appendCaptureText(
                textValue
            );
            flushCaptureLine();
            renderChat("");
        }

        return textValue.length() + 1;
    }

    size_t println(
        const String &value)
    {
        if (capturing_)
        {
            appendCaptureText(
                value
            );
            flushCaptureLine();
            renderChat("");
        }

        return value.length() + 1;
    }

    size_t println(
        const char *value)
    {
        if (value == nullptr)
        {
            return println();
        }

        const String textValue(value);

        if (capturing_)
        {
            appendCaptureText(
                textValue
            );
            flushCaptureLine();
            renderChat("");
        }

        return textValue.length() + 1;
    }

    size_t println(
        double value,
        int digits)
    {
        const String textValue(
            value,
            digits
        );

        if (capturing_)
        {
            appendCaptureText(
                textValue
            );
            flushCaptureLine();
            renderChat("");
        }

        return textValue.length() + 1;
    }

    size_t println(
        float value,
        int digits)
    {
        return println(
            static_cast<double>(value),
            digits
        );
    }

private:
    ChatLine chat_[CHAT_HISTORY_LINES];
    size_t chatStart_ = 0;
    size_t chatCount_ = 0;

    String captureLine_;
    ChatTone captureTone_ = ChatTone::Text;

    bool capturing_ = false;
    bool idleMode_ = false;
    bool externalReady_ = false;
    size_t scrollOffsetFromBottom_ = 0;

    void clearChat()
    {
        chatStart_ = 0;
        chatCount_ = 0;
        captureLine_ = "";
    }

    static uint32_t builtinColor(
        ChatTone tone)
    {
        // LovyanGFX expects RGB888-style values here. Do not reuse
        // Adafruit/RGB565 constants on the built-in display.
        switch (tone)
        {
            case ChatTone::User:
                return 0xFFFF00; // yellow

            case ChatTone::Flora:
                return 0x00FF00; // green

            case ChatTone::Secondary:
                return 0xC0C0C0; // light gray

            case ChatTone::Error:
                return 0xFFA500; // amber

            case ChatTone::Text:
            default:
                return 0xFFFFFF; // white
        }
    }

    static uint16_t externalColor(
        ChatTone tone)
    {
        switch (tone)
        {
            case ChatTone::User:
                return ILI9341_YELLOW;

            case ChatTone::Flora:
                return ILI9341_GREEN;

            case ChatTone::Secondary:
                return 0xC618;

            case ChatTone::Error:
                return 0xFD20;

            case ChatTone::Text:
            default:
                return ILI9341_WHITE;
        }
    }

    void clearMirrorSurface()
    {
        M5Cardputer.Display.fillScreen(
            BLACK
        );

        if (externalReady_)
        {
            // Clear the complete external panel so no stale pixels survive
            // outside the centered 240x135 mirrored viewport.
            externalTft.fillScreen(
                ILI9341_BLACK
            );
        }
    }

    void drawMirroredText(
        int16_t x,
        int16_t y,
        const String &value,
        ChatTone tone)
    {
        M5Cardputer.Display.setTextSize(1);
        M5Cardputer.Display.setTextWrap(false);
        M5Cardputer.Display.setCursor(
            x,
            y
        );
        M5Cardputer.Display.setTextColor(
            builtinColor(tone),
            0x000000
        );
        M5Cardputer.Display.print(
            value
        );

        if (externalReady_)
        {
            externalTft.setTextSize(1);
            externalTft.setTextWrap(false);
            externalTft.setCursor(
                EXT_TFT_MIRROR_X + x,
                EXT_TFT_MIRROR_Y + y
            );
            externalTft.setTextColor(
                externalColor(tone),
                ILI9341_BLACK
            );
            externalTft.print(
                value
            );
        }
    }

    bool shouldSuppressCapturedLine(
        const String &line) const
    {
        String normalized =
            line;

        normalized.trim();

        return
            normalized == "F L O R A" ||
            normalized ==
                "--------------------" ||
            normalized ==
                "Poppy's pocket sister" ||
            normalized ==
                "Hi! I'm Flora." ||
            normalized ==
                "Ask me about your collection." ||
            normalized ==
                "[ENTER] Ask me something else" ||
            normalized ==
                "[ENTER] New command";
    }

    void appendCaptureText(
        const String &value)
    {
        captureLine_ +=
            value;
    }

    void flushCaptureLine()
    {
        if (!capturing_)
        {
            captureLine_ = "";
            return;
        }

        if (!shouldSuppressCapturedLine(
                captureLine_))
        {
            pushWrappedLine(
                captureLine_,
                captureTone_
            );
        }

        captureLine_ = "";
    }

    void pushWrappedLine(
        const String &value,
        ChatTone tone)
    {
        String remaining =
            value;

        if (remaining.length() == 0)
        {
            pushLine(
                "",
                tone
            );
            return;
        }

        while (remaining.length() >
               CHAT_COLUMNS)
        {
            size_t split =
                CHAT_COLUMNS;

            for (size_t i =
                     CHAT_COLUMNS;
                 i > 0;
                 --i)
            {
                if (remaining[i] ==
                    ' ')
                {
                    split = i;
                    break;
                }
            }

            String line =
                remaining.substring(
                    0,
                    split
                );

            line.trim();

            pushLine(
                line,
                tone
            );

            remaining =
                remaining.substring(
                    split
                );

            remaining.trim();
        }

        pushLine(
            remaining,
            tone
        );
    }

    void pushLine(
        const String &value,
        ChatTone tone)
    {
        size_t target = 0;

        if (chatCount_ <
            CHAT_HISTORY_LINES)
        {
            target =
                (chatStart_ +
                 chatCount_) %
                CHAT_HISTORY_LINES;

            ++chatCount_;
        }
        else
        {
            target =
                chatStart_;

            chatStart_ =
                (chatStart_ + 1) %
                CHAT_HISTORY_LINES;
        }

        chat_[target].text =
            value;

        chat_[target].tone =
            tone;
    }

    void renderChat(
        const String &input)
    {
        if (idleMode_)
        {
            return;
        }

        clearMirrorSurface();

        const size_t visible =
            min(
                chatCount_,
                CHAT_VISIBLE_LINES
            );

        const size_t maxOffset =
            chatCount_ > visible
                ? chatCount_ - visible
                : 0;

        if (scrollOffsetFromBottom_ > maxOffset)
        {
            scrollOffsetFromBottom_ = maxOffset;
        }

        const size_t first =
            chatCount_ > visible
                ? chatCount_ - visible - scrollOffsetFromBottom_
                : 0;

        const size_t lastExclusive =
            min(
                chatCount_,
                first + visible
            );

        int16_t y = 4;

        for (size_t i = first;
             i < lastExclusive;
             ++i)
        {
            const size_t index =
                (chatStart_ + i) %
                CHAT_HISTORY_LINES;

            drawMirroredText(
                4,
                y,
                chat_[index].text,
                chat_[index].tone
            );

            y += 8;
        }

        if (scrollOffsetFromBottom_ > 0)
        {
            String scrollLabel =
                "^ history +" +
                String(scrollOffsetFromBottom_);

            drawMirroredText(
                146,
                124,
                scrollLabel,
                ChatTone::Secondary
            );
        }

        // Live input always occupies the bottom line on both screens.
        String prompt =
            "> " + input;

        if (prompt.length() >
            CHAT_COLUMNS)
        {
            prompt =
                prompt.substring(
                    prompt.length() -
                    CHAT_COLUMNS
                );
        }

        if (scrollOffsetFromBottom_ == 0)
        {
            drawMirroredText(
                4,
                124,
                prompt,
                ChatTone::User
            );
        }
    }
};

FloraMirroredDisplay floraDisplay;

void initializeExternalMirror()
{
    pinMode(
        SD_SPI_CS_PIN,
        OUTPUT
    );
    digitalWrite(
        SD_SPI_CS_PIN,
        HIGH
    );

    pinMode(
        EXT_TFT_CS_PIN,
        OUTPUT
    );
    digitalWrite(
        EXT_TFT_CS_PIN,
        HIGH
    );

    externalTft.begin(
        EXT_TFT_SPI_FREQUENCY_HZ
    );

    externalTft.setRotation(3);
    externalTft.fillScreen(
        ILI9341_BLACK
    );
    externalTft.setTextWrap(false);

    floraDisplay.enableExternal();
    floraDisplay.showReady();

    Serial.println(
        "External ILI9341 mirrored chat initialized."
    );
    Serial.println(
        "Both displays: identical 240x135 rolling chat / idle surface."
    );
}

static const char RHEA_TRUSTED_ROOTS[] PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICPzCCAcWgAwIBAgIQBVVWvPJepDU1w6QP1atFcjAKBggqhkjOPQQDAzBhMQsw
CQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3d3cu
ZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBHMzAe
Fw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVTMRUw
EwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5jb20x
IDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEczMHYwEAYHKoZIzj0CAQYF
K4EEACIDYgAE3afZu4q4C/sLfyHS8L6+c/MzXRq8NOrexpu80JX28MzQC7phW1FG
fp4tn+6OYwwX7Adw9c+ELkCDnOg/QW07rdOkFFk2eJ0DQ+4QE2xy3q6Ip6FrtUPO
Z9wj/wMco+I+o0IwQDAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAd
BgNVHQ4EFgQUs9tIpPmhxdiuNkHMEWNpYim8S8YwCgYIKoZIzj0EAwMDaAAwZQIx
AK288mw/EkrRLTnDCgmXc/SINoyIJ7vmiI1Qhadj+Z4y3maTD/HMsQmP3Wyr+mt/
oAIwOWZbwmSNuJ5Q3KjVSaLtx9zRSX8XAbjIho9OjIgrqJqpisXRAL34VOKa5Vt8
sycX
-----END CERTIFICATE-----
)PEM";


constexpr unsigned long CLOCK_SYNC_TIMEOUT_MS = 20000;
constexpr time_t MIN_VALID_EPOCH = 1767225600; // 2026-01-01 00:00:00 UTC

const char *RHEA_MANIFEST_HOST = "stct-prod-api.azurewebsites.net";
const uint16_t RHEA_MANIFEST_PORT = 443;
const char *RHEA_MANIFEST_PATH = "/api/v1/rhea/device/manifest";
const char *RHEA_SNAPSHOT_PATH = "/api/v1/rhea/device/snapshot";

const char *RHEA_SNAPSHOT_TEMP_PATH =
    "/RHEA/rhea_snapshot_candidate.tmp";
const char *RHEA_SNAPSHOT_BACKUP_PATH =
    "/RHEA/rhea_snapshot.bak";

constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr unsigned long HTTPS_READ_TIMEOUT_MS = 300000;
constexpr int MANIFEST_MAX_ATTEMPTS = 3;

struct RheaManifestInfo
{
    String format;
    int schemaVersion = 0;
    size_t recordCount = 0;
    size_t bytes = 0;
    String snapshotVersion;
    String sha256;
    String etag;
};

RheaManifestInfo productionManifest;


const char *RHEA_SNAPSHOT_ACTIVE_PATH =
    "/RHEA/rhea_snapshot.json";

constexpr size_t MAX_QUERY_LENGTH = 80;
constexpr size_t VALUE_ITEMIZE_PAGE_SIZE = 10;

enum class ValueGroupDomain
{
    Pops,
    Pins
};

String inputBuffer;
String serialInputBuffer;
bool previousKeyDown[4][14] = {};

// FLORA1-DYNAMIC-WIFI:
// Session-only alternate Wi-Fi credentials. They are never written to SD,
// NVS, diagnostics, or Serial. The compiled primary network remains the
// default/fallback network.
String sessionWifiSsid;
String sessionWifiPassword;
bool sessionWifiConfigured = false;



// FLORA1 final candidate: Flora persona + manual RFID lookup + accepted G1 value baseline.
static const size_t RHEA_HISTORY_CAPACITY = 5;
String commandHistory[RHEA_HISTORY_CAPACITY];
size_t commandHistoryCount = 0;
String lastUpdateState = "not run";

// RHEA1-G1 rev7 conversational value aggregation/itemization state.
String pendingValueGroupQuery;
ValueGroupDomain pendingValueGroupDomain = ValueGroupDomain::Pops;
bool pendingValueItemizationOffer = false;
bool valueItemizationActive = false;
size_t valueItemizationOffset = 0;

// FLORA1-CONVO1: lightweight conversational context. This is intentionally
// deterministic: Flora remembers the last resolved collection scope so short
// follow-ups such as "show me the top 4" can reuse it.
struct ConversationContext
{
    bool valid = false;
    ValueGroupDomain domain = ValueGroupDomain::Pops;
    String scope;
};

ConversationContext conversationContext;

constexpr size_t FLORA_MAX_RANK_RESULTS = 10;
const char *FLORA_ALIAS_PATH = "/RHEA/flora_aliases.tsv";

String friendlyScopeLabel(
    const String &scope);

void rememberConversationScope(
    const String &scope,
    ValueGroupDomain domain);

// RHEA1-F5B idle personality display.
static const unsigned long RHEA_IDLE_TIMEOUT_MS = 120000UL;
static const unsigned long RHEA_BLINK_CYCLE_MS = 4000UL;
static const unsigned long RHEA_BLINK_START_MS = 3200UL;
static const unsigned long RHEA_BLINK_DURATION_MS = 180UL;

unsigned long lastUserActivityMillis = 0;
unsigned long idleFaceStartedMillis = 0;
bool idleFaceActive = false;
bool idleBlinkFrame = false;

// FLORA1-IDLE-ACTIVITY-GUARD:
// Suppress idle-face activation while a submitted command is executing,
// then restart the idle timeout from response completion.
bool commandExecutionActive = false;

class CommandActivityScope
{
public:
    explicit CommandActivityScope(
        const String &userText)
    {
        commandExecutionActive = true;
        idleFaceActive = false;
        idleBlinkFrame = false;

        floraDisplay.beginChatTurn(
            userText
        );
    }

    ~CommandActivityScope()
    {
        floraDisplay.finishChatTurn();

        commandExecutionActive = false;
        lastUserActivityMillis = millis();
    }

    CommandActivityScope(
        const CommandActivityScope &) = delete;

    CommandActivityScope &operator=(
        const CommandActivityScope &) = delete;
};

enum class RecordReadState
{
    Record,
    EndOfArray,
    Malformed
};

struct EnvelopeInfo
{
    String format;
    int schemaVersion = 0;
    String snapshotVersion;
};

struct RecordInfo
{
    String title;
    String collectibleType;
    String id;
    String productType;
    String seriesName;
    int seriesNumber = 0;
    String affiliation;
    String variant;
    String subject;
    String distributor;

    // RHEA1-G1 private device projection fields.
    String upc;
    int64_t hobbyDbId = 0;
    bool hasCurrentValue = false;
    double currentValueUsd = 0.0;
    int inHandCopyCount = 0;
    int orderedCopyCount = 0;
    int sellableCopyCount = 0;
};

struct RfidInfo
{
    String printedLabel;
    String epc;
    String itemId;
    String title;
    String collectibleType;
    String physicalAssetId;
};


void printBoth(const String &text)
{
    floraDisplay.println(text);
    Serial.println(text);
}

String lowerCopy(String value)
{
    value.toLowerCase();
    return value;
}

bool deviceFunctionKeyConfigured()
{
    const String key =
        String(RHEA_DEVICE_FUNCTION_KEY);

    if (key.length() < 20)
    {
        return false;
    }

    if (key == "PASTE_ROTATED_RHEA_DEVICE_FUNCTION_KEY_HERE")
    {
        return false;
    }

    return true;
}

String toUpperHex(
    const unsigned char *bytes,
    size_t length)
{
    static const char hex[] = "0123456789ABCDEF";

    String result;
    result.reserve(length * 2);

    for (size_t i = 0; i < length; ++i)
    {
        result += hex[(bytes[i] >> 4) & 0x0F];
        result += hex[bytes[i] & 0x0F];
    }

    return result;
}


void rememberCommand(
    const String &command)
{
    String value =
        command;

    value.trim();

    if (value.length() == 0)
    {
        return;
    }

    const String lower =
        lowerCopy(value);

    if (lower == "history" ||
        lower == "!!")
    {
        return;
    }

    // Avoid duplicating an identical immediately-previous command.
    if (commandHistoryCount > 0 &&
        commandHistory[0] == value)
    {
        return;
    }

    const size_t moveCount =
        min(
            commandHistoryCount,
            RHEA_HISTORY_CAPACITY - 1
        );

    for (size_t i = moveCount;
         i > 0;
         --i)
    {
        commandHistory[i] =
            commandHistory[i - 1];
    }

    commandHistory[0] =
        value;

    if (commandHistoryCount <
        RHEA_HISTORY_CAPACITY)
    {
        ++commandHistoryCount;
    }
}

bool recallPreviousCommand(
    String &command)
{
    if (commandHistoryCount == 0)
    {
        return false;
    }

    command =
        commandHistory[0];

    return true;
}

void showCommandHistory()
{
    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextColor(WHITE, BLACK);
    display.setTextSize(1);
    display.setTextWrap(true);

    display.println("F L O R A");
    display.println("Recent commands");
    display.println("--------------------");

    Serial.println();
    Serial.println("=== RHEA1-F6 COMMAND HISTORY ===");

    if (commandHistoryCount == 0)
    {
        display.println("No history yet.");
        Serial.println("(empty)");
    }
    else
    {
        for (size_t i = 0;
             i < commandHistoryCount;
             ++i)
        {
            String shortValue =
                commandHistory[i];

            if (shortValue.length() > 31)
            {
                shortValue =
                    shortValue.substring(0, 28) +
                    "...";
            }

            display.print(i + 1);
            display.print(". ");
            display.println(shortValue);

            Serial.print(i + 1);
            Serial.print(". ");
            Serial.println(commandHistory[i]);
        }
    }

    Serial.println("===============================");

    display.println();
    display.println("!! repeats the last command");
    display.println();
    display.println("[ENTER] Ask me something else");
}

void drawPrompt();

void drawRheaIdleFace(
    bool blink)
{
    const uint16_t *frame =
        blink
            ? FLORA_AVATAR_64_BLINK
            : FLORA_AVATAR_64_NORMAL;

    floraDisplay.showIdle(
        frame
    );
}

void activateIdleFace()
{
    idleFaceActive =
        true;

    idleFaceStartedMillis =
        millis();

    idleBlinkFrame =
        false;

    drawRheaIdleFace(
        false
    );
}

void noteUserActivity()
{
    lastUserActivityMillis =
        millis();

    if (idleFaceActive)
    {
        idleFaceActive =
            false;

        idleBlinkFrame =
            false;

        floraDisplay.leaveIdleAndRestore(
            inputBuffer
        );
    }
}

void updateIdleFace()
{
    if (commandExecutionActive)
    {
        return;
    }

    const unsigned long now =
        millis();

    if (!idleFaceActive)
    {
        if (now - lastUserActivityMillis >=
            RHEA_IDLE_TIMEOUT_MS)
        {
            activateIdleFace();
        }

        return;
    }

    const unsigned long elapsed =
        now - idleFaceStartedMillis;

    const unsigned long phase =
        elapsed %
        RHEA_BLINK_CYCLE_MS;

    const bool shouldBlink =
        phase >= RHEA_BLINK_START_MS &&
        phase <
            (RHEA_BLINK_START_MS +
             RHEA_BLINK_DURATION_MS);

    if (shouldBlink !=
        idleBlinkFrame)
    {
        idleBlinkFrame =
            shouldBlink;

        drawRheaIdleFace(
            shouldBlink
        );
    }
}

void drawPrompt()
{
    floraDisplay.showInput(
        inputBuffer
    );
}

void showRecord(const RecordInfo &record)
{
    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextSize(1);
    display.setTextWrap(true);

    display.setTextColor(GREEN, BLACK);
    display.println("F L O R A");
    display.println("--------------------");
    display.setTextColor(WHITE, BLACK);
    display.println("Yep - found it.");
    display.println();

    display.println(record.title);

    if (record.productType.length() > 0)
    {
        display.print(record.productType);

        if (record.seriesNumber > 0)
        {
            display.print(" #");
            display.print(record.seriesNumber);
        }

        display.println();
    }
    else
    {
        display.println(record.collectibleType);
    }

    if (record.seriesName.length() > 0)
    {
        display.print("Series: ");
        display.println(record.seriesName);
    }

    if (record.variant.length() > 0)
    {
        display.print("Variant: ");
        display.println(record.variant);
    }

    if (record.hasCurrentValue)
    {
        display.print("Value: $");
        display.println(record.currentValueUsd, 2);
    }

    display.print("ID: ");
    display.println(record.id);

    display.println();
    display.println("[ENTER] Ask me something else");

    Serial.print("FLORA_SAYS Yep - found it: ");
    Serial.print(record.title);

    if (record.productType.length() > 0)
    {
        Serial.print(" | ");
        Serial.print(record.productType);

        if (record.seriesNumber > 0)
        {
            Serial.print(" #");
            Serial.print(record.seriesNumber);
        }
    }

    if (record.seriesName.length() > 0)
    {
        Serial.print(" | Series: ");
        Serial.print(record.seriesName);
    }

    if (record.hasCurrentValue)
    {
        Serial.print(" | Value: $");
        Serial.print(record.currentValueUsd, 2);
    }

    Serial.println();
}

void showError(
    const String &message)
{
    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextColor(WHITE, BLACK);
    display.setTextSize(1);
    display.setTextWrap(true);

    display.println("F L O R A");
    display.println("--------------------");
    display.println(message);
    display.println();
    display.println("[ENTER] Ask me something else");

    Serial.print("FLORA_SAYS ");
    Serial.println(message);
}

// Reads only the bounded envelope prefix, then synthesizes an empty records
// array so ArduinoJson can validate the header without loading the snapshot.
bool locateRecordsArray(
    File &file,
    EnvelopeInfo &envelope)
{
    String prefix;
    prefix.reserve(512);

    bool inString = false;
    bool escape = false;
    int objectDepth = 0;
    String currentString;
    bool currentStringIsTopLevelKey = false;
    bool expectingTopLevelKey = false;
    String lastTopLevelKey;

    while (file.available())
    {
        const int raw = file.read();

        if (raw < 0)
        {
            break;
        }

        const char c = static_cast<char>(raw);

        if (prefix.length() < 1024)
        {
            prefix += c;
        }
        else
        {
            Serial.println(
                "E1 FAILED: envelope prefix exceeded 1024 bytes"
            );
            return false;
        }

        if (inString)
        {
            if (escape)
            {
                escape = false;
                currentString += c;
                continue;
            }

            if (c == '\\')
            {
                escape = true;
                currentString += c;
                continue;
            }

            if (c == '"')
            {
                inString = false;

                if (currentStringIsTopLevelKey)
                {
                    lastTopLevelKey = currentString;
                    expectingTopLevelKey = false;
                }

                currentString = "";
                currentStringIsTopLevelKey = false;
                continue;
            }

            currentString += c;
            continue;
        }

        if (c == '"')
        {
            inString = true;
            currentString = "";
            currentStringIsTopLevelKey =
                (objectDepth == 1 && expectingTopLevelKey);
            continue;
        }

        if (c == '{')
        {
            ++objectDepth;

            if (objectDepth == 1)
            {
                expectingTopLevelKey = true;
            }

            continue;
        }

        if (c == '}')
        {
            --objectDepth;

            if (objectDepth < 0)
            {
                Serial.println(
                    "E1 FAILED: invalid envelope object depth"
                );
                return false;
            }

            continue;
        }

        if (objectDepth == 1 && c == ',')
        {
            expectingTopLevelKey = true;
            lastTopLevelKey = "";
            continue;
        }

        if (objectDepth == 1 &&
            c == ':' &&
            lastTopLevelKey == "records")
        {
            // Skip whitespace and require the actual array opener.
            while (file.available())
            {
                const int nextRaw = file.read();

                if (nextRaw < 0)
                {
                    break;
                }

                const char next =
                    static_cast<char>(nextRaw);

                if (next == ' ' ||
                    next == '\r' ||
                    next == '\n' ||
                    next == '\t')
                {
                    if (prefix.length() < 1024)
                    {
                        prefix += next;
                    }

                    continue;
                }

                if (next != '[')
                {
                    Serial.println(
                        "E1 FAILED: records property is not an array"
                    );
                    return false;
                }

                // prefix currently ends after the "records": separator.
                // Complete a tiny valid envelope with an empty records array.
                prefix += "[]}";

                StaticJsonDocument<512> header;

                const DeserializationError error =
                    deserializeJson(
                        header,
                        prefix
                    );

                if (error)
                {
                    Serial.print(
                        "E1 FAILED: envelope header parse: "
                    );
                    Serial.println(error.c_str());
                    return false;
                }

                envelope.format =
                    String(
                        (const char *)
                            (header["format"] | "")
                    );

                envelope.schemaVersion =
                    header["schemaVersion"] | 0;

                envelope.snapshotVersion =
                    String(
                        (const char *)
                            (header["snapshotVersion"] | "")
                    );

                if (envelope.format != "RHEA1")
                {
                    Serial.println(
                        "E1 FAILED: format is not RHEA1"
                    );
                    return false;
                }

                if (envelope.schemaVersion != 3)
                {
                    Serial.println(
                        "E1 FAILED: schemaVersion is not 3"
                    );
                    return false;
                }

                if (envelope.snapshotVersion.length() != 64)
                {
                    Serial.println(
                        "E1 FAILED: snapshotVersion length is not 64"
                    );
                    return false;
                }

                return true;
            }

            Serial.println(
                "E1 FAILED: records array opener missing"
            );
            return false;
        }
    }

    Serial.println(
        "E1 FAILED: top-level records property not found"
    );
    return false;
}

RecordReadState seekNextRecordObject(File &file)
{
    bool inString = false;
    bool escape = false;

    while (file.available())
    {
        const int raw = file.read();

        if (raw < 0)
        {
            break;
        }

        const char c = static_cast<char>(raw);

        if (inString)
        {
            if (escape)
            {
                escape = false;
                continue;
            }

            if (c == '\\')
            {
                escape = true;
                continue;
            }

            if (c == '"')
            {
                inString = false;
            }

            continue;
        }

        if (c == '"')
        {
            // Between array elements a string token is invalid for this schema.
            Serial.println(
                "E1 FAILED: unexpected string token between records"
            );
            return RecordReadState::Malformed;
        }

        if (c == '{')
        {
            file.seek(file.position() - 1);
            return RecordReadState::Record;
        }

        if (c == ']')
        {
            return RecordReadState::EndOfArray;
        }

        if (c == ',' ||
            c == ' ' ||
            c == '\r' ||
            c == '\n' ||
            c == '\t')
        {
            continue;
        }

        Serial.print(
            "E1 FAILED: unexpected token between records: "
        );
        Serial.println(c);

        return RecordReadState::Malformed;
    }

    Serial.println(
        "E1 FAILED: EOF reached before records array terminator"
    );
    return RecordReadState::Malformed;
}


bool locateRfidAssignmentsArray(
    File &file)
{
    static const char target[] =
        "\"rfidAssignments\"";

    size_t matched = 0;
    bool foundKey = false;

    file.seek(0);

    while (file.available())
    {
        const char c =
            static_cast<char>(
                file.read()
            );

        if (!foundKey)
        {
            if (c == target[matched])
            {
                ++matched;

                if (matched ==
                    sizeof(target) - 1)
                {
                    foundKey = true;
                }
            }
            else
            {
                matched =
                    c == target[0]
                        ? 1
                        : 0;
            }

            continue;
        }

        if (c == '[')
        {
            return true;
        }
    }

    return false;
}

RecordReadState readNextRfidRecord(
    File &file,
    RfidInfo &rfid)
{
    const RecordReadState next =
        seekNextRecordObject(file);

    if (next != RecordReadState::Record)
    {
        return next;
    }

    static StaticJsonDocument<192> filter;
    static StaticJsonDocument<768> json;

    filter.clear();
    json.clear();

    filter["printedLabel"] = true;
    filter["epc"] = true;
    filter["itemId"] = true;
    filter["title"] = true;
    filter["collectibleType"] = true;
    filter["physicalAssetId"] = true;

    const DeserializationError error =
        deserializeJson(
            json,
            file,
            DeserializationOption::Filter(filter)
        );

    if (error)
    {
        Serial.print(
            "FLORA1 RFID parse failed: "
        );
        Serial.println(error.c_str());
        return RecordReadState::Malformed;
    }

    rfid.printedLabel =
        String(
            (const char *)
                (json["printedLabel"] | "")
        );

    rfid.epc =
        String(
            (const char *)
                (json["epc"] | "")
        );

    rfid.itemId =
        String(
            (const char *)
                (json["itemId"] | "")
        );

    rfid.title =
        String(
            (const char *)
                (json["title"] | "")
        );

    rfid.collectibleType =
        String(
            (const char *)
                (json["collectibleType"] | "")
        );

    rfid.physicalAssetId =
        String(
            (const char *)
                (json["physicalAssetId"] | "")
        );

    if (rfid.printedLabel.length() == 0 ||
        rfid.epc.length() == 0 ||
        rfid.itemId.length() == 0)
    {
        Serial.println(
            "FLORA1 RFID record missing required identity."
        );
        return RecordReadState::Malformed;
    }

    return RecordReadState::Record;
}

bool isHex24(
    const String &value)
{
    if (value.length() != 24)
    {
        return false;
    }

    for (size_t i = 0;
         i < value.length();
         ++i)
    {
        if (!isHexadecimalDigit(value[i]))
        {
            return false;
        }
    }

    return true;
}

bool parseFriendlyRfid(
    const String &input,
    uint32_t &sequence)
{
    String value = input;
    value.trim();
    value.toUpperCase();

    if (!value.startsWith("VFC-") ||
        value.length() != 9)
    {
        return false;
    }

    for (size_t i = 4; i < 9; ++i)
    {
        if (!isDigit(value[i]))
        {
            return false;
        }
    }

    sequence =
        static_cast<uint32_t>(
            value.substring(4).toInt()
        );

    return
        sequence >= 1 &&
        sequence <= 10025;
}

bool parseEpcRfid(
    const String &input,
    uint32_t &sequence)
{
    String value = input;
    value.trim();
    value.toUpperCase();

    if (!isHex24(value) ||
        !value.startsWith("5646432D"))
    {
        return false;
    }

    const String suffix =
        value.substring(8);

    uint64_t parsed = 0;

    for (size_t i = 0;
         i < suffix.length();
         ++i)
    {
        const char c = suffix[i];
        uint8_t nibble = 0;

        if (c >= '0' && c <= '9')
        {
            nibble = c - '0';
        }
        else if (c >= 'A' && c <= 'F')
        {
            nibble = 10 + c - 'A';
        }
        else
        {
            return false;
        }

        parsed =
            (parsed << 4) |
            nibble;
    }

    if (parsed < 1 ||
        parsed > 10025)
    {
        return false;
    }

    sequence =
        static_cast<uint32_t>(parsed);

    return true;
}

String rfidLabelForSequence(
    uint32_t sequence)
{
    char buffer[10];

    snprintf(
        buffer,
        sizeof(buffer),
        "VFC-%05lu",
        static_cast<unsigned long>(
            sequence
        )
    );

    return String(buffer);
}

String rfidEpcForSequence(
    uint32_t sequence)
{
    char buffer[25];

    snprintf(
        buffer,
        sizeof(buffer),
        "5646432D%016llX",
        static_cast<unsigned long long>(
            sequence
        )
    );

    return String(buffer);
}

bool normalizeRfidIdentifier(
    const String &input,
    String &printedLabel,
    String &epc)
{
    uint32_t sequence = 0;

    if (parseFriendlyRfid(
            input,
            sequence) ||
        parseEpcRfid(
            input,
            sequence))
    {
        printedLabel =
            rfidLabelForSequence(
                sequence
            );

        epc =
            rfidEpcForSequence(
                sequence
            );

        return true;
    }

    return false;
}

bool toolFindByRfid(
    const String &input,
    RfidInfo &match,
    bool &validButUnassigned)
{
    validButUnassigned = false;

    String printedLabel;
    String epc;

    if (!normalizeRfidIdentifier(
            input,
            printedLabel,
            epc))
    {
        return false;
    }

    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        return false;
    }

    if (!locateRfidAssignmentsArray(
            file))
    {
        file.close();
        return false;
    }

    while (true)
    {
        RfidInfo current;

        const RecordReadState state =
            readNextRfidRecord(
                file,
                current
            );

        if (state ==
            RecordReadState::EndOfArray)
        {
            break;
        }

        if (state ==
            RecordReadState::Malformed)
        {
            file.close();
            return false;
        }

        String currentLabel =
            current.printedLabel;
        currentLabel.toUpperCase();

        String currentEpc =
            current.epc;
        currentEpc.toUpperCase();

        if (currentLabel ==
                printedLabel ||
            currentEpc == epc)
        {
            match = current;
            file.close();
            return true;
        }
    }

    file.close();
    validButUnassigned = true;
    return false;
}

void showRfidResult(
    const RfidInfo &rfid)
{
    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextColor(
        WHITE,
        BLACK
    );
    display.setTextSize(1);
    display.setTextWrap(true);

    display.println("F L O R A");
    display.println("--------------------");
    display.println(rfid.title);
    display.println(rfid.itemId);
    display.print("RFID: ");
    display.println(rfid.printedLabel);

    Serial.print(
        "RFID_RESPONSE {\"status\":\"assigned\",\"printedLabel\":\""
    );
    Serial.print(rfid.printedLabel);
    Serial.print("\",\"epc\":\"");
    Serial.print(rfid.epc);
    Serial.print("\",\"itemId\":\"");
    Serial.print(rfid.itemId);
    Serial.print("\",\"title\":\"");
    Serial.print(rfid.title);
    Serial.print("\",\"collectibleType\":\"");
    Serial.print(rfid.collectibleType);
    Serial.println("\"}");

    Serial.print("FLORA_SAYS ");
    Serial.print(rfid.printedLabel);
    Serial.print(" belongs to ");
    Serial.print(rfid.title);
    Serial.print(" | ");
    Serial.println(rfid.itemId);
}

RecordReadState readNextRecord(
    File &file,
    RecordInfo &record)
{
    const RecordReadState next =
        seekNextRecordObject(file);

    if (next != RecordReadState::Record)
    {
        return next;
    }

    // The filter includes Subject in addition to the G1 valuation/copy-count
    // fields. 256 bytes is too tight: when the filter overflows, keys added
    // later (including currentValueUsd/inHandCopyCount) are silently omitted,
    // which makes every value query appear to have no usable valuation data.
    static StaticJsonDocument<640> filter;
    static StaticJsonDocument<2176> json;

    filter.clear();
    json.clear();

    filter["title"] = true;
    filter["collectibleType"] = true;
    filter["id"] = true;
    filter["productType"] = true;
    filter["seriesName"] = true;
    filter["seriesNumber"] = true;
    filter["affiliation"] = true;
    filter["variant"] = true;
    filter["subject"] = true;
    filter["distributor"] = true;
    filter["upc"] = true;
    filter["hobbyDbId"] = true;
    filter["currentValueUsd"] = true;
    filter["inHandCopyCount"] = true;
    filter["orderedCopyCount"] = true;
    filter["sellableCopyCount"] = true;

    const DeserializationError error =
        deserializeJson(
            json,
            file,
            DeserializationOption::Filter(filter)
        );

    if (error)
    {
        Serial.print(
            "E1 FAILED: record JSON parse: "
        );
        Serial.println(error.c_str());
        return RecordReadState::Malformed;
    }

    record.title =
        String(
            (const char *)
                (json["title"] | "")
        );

    record.collectibleType =
        String(
            (const char *)
                (json["collectibleType"] | "")
        );

    record.id =
        String(
            (const char *)
                (json["id"] | "")
        );

    record.productType =
        String(
            (const char *)
                (json["productType"] | "")
        );

    record.seriesName =
        String(
            (const char *)
                (json["seriesName"] | "")
        );

    record.seriesNumber =
        json["seriesNumber"] | 0;

    record.affiliation =
        String(
            (const char *)
                (json["affiliation"] | "")
        );

    record.variant =
        String(
            (const char *)
                (json["variant"] | "")
        );

    record.subject =
        String(
            (const char *)
                (json["subject"] | "")
        );

    record.distributor =
        String(
            (const char *)
                (json["distributor"] | "")
        );

    record.upc =
        String(
            (const char *)
                (json["upc"] | "")
        );

    record.hobbyDbId =
        json["hobbyDbId"] | 0LL;

    record.hasCurrentValue =
        json.containsKey("currentValueUsd") &&
        !json["currentValueUsd"].isNull();

    record.currentValueUsd =
        record.hasCurrentValue
            ? json["currentValueUsd"].as<double>()
            : 0.0;

    record.inHandCopyCount =
        json["inHandCopyCount"] | 0;

    record.orderedCopyCount =
        json["orderedCopyCount"] | 0;

    record.sellableCopyCount =
        json["sellableCopyCount"] | 0;

    if (record.title.length() == 0)
    {
        Serial.println(
            "E1 FAILED: record has blank title"
        );
        return RecordReadState::Malformed;
    }

    if (record.collectibleType.length() == 0)
    {
        Serial.println(
            "E1 FAILED: record has blank collectibleType"
        );
        return RecordReadState::Malformed;
    }

    if (record.id.length() == 0)
    {
        Serial.print(
            "E1 FAILED: record has blank id; title="
        );
        Serial.println(record.title);
        return RecordReadState::Malformed;
    }

    return RecordReadState::Record;
}

bool validateProductionReader()
{
    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        Serial.println(
            "E1 FAILED: active snapshot missing"
        );
        return false;
    }

    Serial.println();
    Serial.println(
        "=== RHEA1-E2 PRODUCTION QUERY PRIMITIVE VALIDATION ==="
    );

    Serial.print(
        "Active snapshot bytes: "
    );
    Serial.println(file.size());

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return false;
    }

    Serial.print("format: ");
    Serial.println(envelope.format);

    Serial.print("schemaVersion: ");
    Serial.println(envelope.schemaVersion);

    Serial.print("snapshotVersion: ");
    Serial.println(envelope.snapshotVersion);

    size_t total = 0;
    size_t funko = 0;
    size_t thrilljoy = 0;
    size_t pins = 0;
    size_t trlIds = 0;
    size_t enpIds = 0;

    bool cleanEnd = false;

    while (true)
    {
        RecordInfo record;

        const size_t before =
            file.position();

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state == RecordReadState::EndOfArray)
        {
            cleanEnd = true;
            break;
        }

        if (state == RecordReadState::Malformed)
        {
            file.close();
            return false;
        }

        if (file.position() <= before)
        {
            Serial.println(
                "E1 FAILED: parser made no forward progress"
            );
            file.close();
            return false;
        }

        ++total;

        if (record.collectibleType == "Funko")
        {
            ++funko;
        }
        else if (record.collectibleType == "Thrilljoy")
        {
            ++thrilljoy;

            if (record.id.startsWith("TRL-"))
            {
                ++trlIds;
            }
        }
        else if (record.collectibleType == "Enamel Pins")
        {
            ++pins;

            if (record.id.startsWith("ENP-"))
            {
                ++enpIds;
            }
        }
        else
        {
            Serial.print(
                "E1 FAILED: unexpected collectibleType: "
            );
            Serial.println(record.collectibleType);
            file.close();
            return false;
        }

        if (total <= 3)
        {
            Serial.print("Sample record ");
            Serial.print(total);
            Serial.print(": ");
            Serial.print(record.id);
            Serial.print(" | ");
            Serial.print(record.collectibleType);
            Serial.print(" | ");
            Serial.println(record.title);
        }

        if (record.collectibleType == "Thrilljoy" &&
            thrilljoy <= 5)
        {
            Serial.print("Thrilljoy ID: ");
            Serial.print(record.id);
            Serial.print(" | ");
            Serial.println(record.title);
        }
    }

    file.close();

    Serial.println();
    Serial.print("Clean end-of-array: ");
    Serial.println(cleanEnd ? "PASS" : "FAIL");

    Serial.print("Records parsed: ");
    Serial.println(total);

    Serial.print("Funko: ");
    Serial.println(funko);

    Serial.print("Thrilljoy: ");
    Serial.println(thrilljoy);

    Serial.print("Enamel Pins: ");
    Serial.println(pins);

    Serial.print("Thrilljoy TRL-* IDs: ");
    Serial.println(trlIds);

    Serial.print("Pin ENP-* IDs: ");
    Serial.println(enpIds);

    const bool pass =
        cleanEnd &&
        total > 0 &&
        (funko + thrilljoy + pins) == total &&
        trlIds == thrilljoy &&
        enpIds == pins;

    Serial.println(
        pass
            ? "RHEA1-E1 SNAPSHOT READER VALIDATION: PASS"
            : "RHEA1-E1 SNAPSHOT READER VALIDATION: FAIL"
    );

    Serial.println(
        "================================================="
    );

    return pass;
}


bool findByExactId(
    const String &query,
    RecordInfo &match)
{
    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        Serial.println(
            "findByExactId FAILED: active snapshot missing"
        );
        return false;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return false;
    }

    const String needle =
        lowerCopy(query);

    while (true)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state == RecordReadState::EndOfArray)
        {
            break;
        }

        if (state == RecordReadState::Malformed)
        {
            file.close();
            return false;
        }

        if (lowerCopy(record.id) == needle)
        {
            match = record;
            file.close();
            return true;
        }
    }

    file.close();
    return false;
}

bool findByTitleSubstring(
    const String &query,
    RecordInfo &match)
{
    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        Serial.println(
            "findByTitleSubstring FAILED: active snapshot missing"
        );
        return false;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return false;
    }

    const String needle =
        lowerCopy(query);

    while (true)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state == RecordReadState::EndOfArray)
        {
            break;
        }

        if (state == RecordReadState::Malformed)
        {
            file.close();
            return false;
        }

        if (lowerCopy(record.title).indexOf(needle) >= 0)
        {
            match = record;
            file.close();
            return true;
        }
    }

    file.close();
    return false;
}

bool captureRepresentativeRecords(
    RecordInfo &funko,
    RecordInfo &thrilljoy,
    RecordInfo &pin)
{
    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        return false;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return false;
    }

    bool haveFunko = false;
    bool haveThrilljoy = false;
    bool havePin = false;

    while (true)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state == RecordReadState::EndOfArray)
        {
            break;
        }

        if (state == RecordReadState::Malformed)
        {
            file.close();
            return false;
        }

        if (!haveFunko &&
            record.collectibleType == "Funko")
        {
            funko = record;
            haveFunko = true;
        }
        else if (!haveThrilljoy &&
                 record.collectibleType == "Thrilljoy")
        {
            thrilljoy = record;
            haveThrilljoy = true;
        }
        else if (!havePin &&
                 record.collectibleType == "Enamel Pins")
        {
            pin = record;
            havePin = true;
        }

        if (haveFunko &&
            haveThrilljoy &&
            havePin)
        {
            file.close();
            return true;
        }
    }

    file.close();

    return false;
}

bool validateQueryPrimitives()
{
    Serial.println();
    Serial.println(
        "=== RHEA1-E2 QUERY PRIMITIVE SELF-TESTS ==="
    );

    RecordInfo funko;
    RecordInfo thrilljoy;
    RecordInfo pin;

    if (!captureRepresentativeRecords(
            funko,
            thrilljoy,
            pin))
    {
        Serial.println(
            "E2 FAILED: could not capture representatives for all domains"
        );
        return false;
    }

    RecordInfo found;

    if (!findByExactId(
            funko.id,
            found) ||
        found.id != funko.id)
    {
        Serial.println(
            "E2 FAILED: exact Funko ID lookup"
        );
        return false;
    }

    Serial.print("PASS exact Funko ID: ");
    Serial.println(funko.id);

    if (!findByExactId(
            lowerCopy(thrilljoy.id),
            found) ||
        found.id != thrilljoy.id)
    {
        Serial.println(
            "E2 FAILED: case-insensitive Thrilljoy ID lookup"
        );
        return false;
    }

    Serial.print(
        "PASS case-insensitive Thrilljoy ID: "
    );
    Serial.println(thrilljoy.id);

    if (!findByExactId(
            pin.id,
            found) ||
        found.id != pin.id)
    {
        Serial.println(
            "E2 FAILED: exact Enamel Pin ID lookup"
        );
        return false;
    }

    Serial.print("PASS exact Pin ID: ");
    Serial.println(pin.id);

    String titleNeedle =
        lowerCopy(funko.title);

    if (titleNeedle.length() > 8)
    {
        titleNeedle =
            titleNeedle.substring(0, 8);
    }

    if (!findByTitleSubstring(
            titleNeedle,
            found))
    {
        Serial.println(
            "E2 FAILED: case-insensitive title substring lookup"
        );
        return false;
    }

    Serial.print(
        "PASS case-insensitive title substring: "
    );
    Serial.println(titleNeedle);

    RecordInfo absent;

    if (findByExactId(
            "__RHEA_ID_THAT_MUST_NOT_EXIST__",
            absent))
    {
        Serial.println(
            "E2 FAILED: absent exact ID produced a match"
        );
        return false;
    }

    Serial.println(
        "PASS absent exact ID returns no result"
    );

    Serial.println(
        "RHEA1-E2 QUERY PRIMITIVE VALIDATION: PASS"
    );
    Serial.println(
        "========================================"
    );

    return true;
}


void showNetworkStatus(
    const String &line1,
    const String &line2 = "")
{
    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextColor(GREEN, BLACK);
    display.setTextSize(1);
    display.setTextWrap(true);

    display.println("F L O R A");
    display.println("Online sync test");
    display.println("--------------------");
    display.println(line1);

    if (line2.length() > 0)
    {
        display.println(line2);
    }
}

bool syncClockForTls()
{
    Serial.println("Synchronizing clock for TLS validation...");

    configTime(
        0,
        0,
        "time.cloudflare.com",
        "pool.ntp.org",
        "time.google.com"
    );

    const unsigned long started = millis();

    while (time(nullptr) < MIN_VALID_EPOCH)
    {
        if (millis() - started >= CLOCK_SYNC_TIMEOUT_MS)
        {
            Serial.println("Clock sync FAILED: timeout");
            return false;
        }

        delay(250);
    }

    const time_t now = time(nullptr);

    struct tm timeInfo;
    gmtime_r(&now, &timeInfo);

    char buffer[32];

    strftime(
        buffer,
        sizeof(buffer),
        "%Y-%m-%d %H:%M:%S UTC",
        &timeInfo
    );

    Serial.print("Clock synchronized: ");
    Serial.println(buffer);

    return true;
}

void waitForKeyboardRelease()
{
    const unsigned long started =
        millis();

    while (millis() - started < 1500UL)
    {
        M5Cardputer.update();

        if (M5Cardputer.Keyboard.keyList().size() == 0)
        {
            delay(60);
            return;
        }

        delay(10);
    }
}

void drawWifiEntryScreen(
    const String &title,
    const String &value,
    bool maskValue)
{
    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextColor(WHITE, BLACK);
    display.setTextSize(1);
    display.setTextWrap(true);

    display.println("F L O R A");
    display.println("Temporary Wi-Fi");
    display.println("--------------------");
    display.println(title);
    display.println();

    if (maskValue)
    {
        for (size_t i = 0;
             i < value.length();
             ++i)
        {
            display.print('*');
        }
    }
    else
    {
        display.print(value);
    }

    display.println();
    display.println();
    display.println("[ENTER] Continue");

    if (title == "SSID")
    {
        display.println("Empty SSID = cancel");
    }
}

bool readWifiField(
    const String &title,
    bool maskValue,
    size_t maxLength,
    String &value)
{
    bool localPrevious[4][14] = {};

    waitForKeyboardRelease();

    value = "";
    drawWifiEntryScreen(
        title,
        value,
        maskValue
    );

    while (true)
    {
        M5Cardputer.update();

        bool current[4][14] = {};

        const auto &keys =
            M5Cardputer.Keyboard.keyList();

        Keyboard_Class::KeysState state =
            M5Cardputer.Keyboard.keysState();

        bool changed = false;

        for (const auto &keyPos : keys)
        {
            if (keyPos.x < 0 ||
                keyPos.x >= 14 ||
                keyPos.y < 0 ||
                keyPos.y >= 4)
            {
                continue;
            }

            current[keyPos.y][keyPos.x] =
                true;

            if (localPrevious[keyPos.y][keyPos.x])
            {
                continue;
            }

            const KeyValue_t keyValue =
                M5Cardputer.Keyboard.getKeyValue(
                    keyPos
                );

            const uint8_t baseCode =
                keyValue.value_first;

            if (baseCode == KEY_FN ||
                baseCode == KEY_OPT ||
                baseCode == KEY_LEFT_CTRL ||
                baseCode == KEY_LEFT_SHIFT ||
                baseCode == KEY_LEFT_ALT)
            {
                continue;
            }

            if (state.fn)
            {
                continue;
            }

            if (baseCode == KEY_ENTER)
            {
                waitForKeyboardRelease();

                if (title == "SSID" &&
                    value.length() == 0)
                {
                    return false;
                }

                return true;
            }

            if (baseCode == KEY_BACKSPACE ||
                baseCode == KEY_DELETE)
            {
                if (value.length() > 0)
                {
                    value.remove(
                        value.length() - 1
                    );

                    changed = true;
                }

                continue;
            }

            const uint8_t resolvedCode =
                M5Cardputer.Keyboard.getKey(
                    keyPos
                );

            if (resolvedCode >= 32 &&
                resolvedCode <= 126 &&
                value.length() < maxLength)
            {
                value +=
                    static_cast<char>(
                        resolvedCode
                    );

                changed = true;
            }
        }

        for (int y = 0; y < 4; ++y)
        {
            for (int x = 0; x < 14; ++x)
            {
                localPrevious[y][x] =
                    current[y][x];
            }
        }

        if (changed)
        {
            drawWifiEntryScreen(
                title,
                value,
                maskValue
            );
        }

        delay(10);
    }
}

bool promptForTemporaryWifi()
{
    String ssid;
    String password;

    if (!readWifiField(
            "SSID",
            false,
            32,
            ssid))
    {
        Serial.println(
            "FLORA_WIFI temporary setup cancelled"
        );

        drawPrompt();
        return false;
    }

    if (!readWifiField(
            "Password",
            true,
            63,
            password))
    {
        drawPrompt();
        return false;
    }

    sessionWifiSsid =
        ssid;

    sessionWifiPassword =
        password;

    sessionWifiConfigured =
        true;

    // Never print the password.
    Serial.print(
        "FLORA_WIFI temporary SSID configured for session: "
    );
    Serial.println(
        sessionWifiSsid
    );

    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextColor(GREEN, BLACK);
    display.setTextSize(1);
    display.setTextWrap(true);
    display.println("F L O R A");
    display.println("--------------------");
    display.println("Temporary Wi-Fi saved");
    display.println("for this session.");
    display.println();
    display.print("SSID: ");
    display.println(sessionWifiSsid);
    display.println();
    display.println("Use: update records");

    delay(1200);
    return true;
}

void forgetTemporaryWifi()
{
    sessionWifiPassword = "";
    sessionWifiSsid = "";
    sessionWifiConfigured = false;

    Serial.println(
        "FLORA_WIFI temporary credentials cleared"
    );

    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextColor(GREEN, BLACK);
    display.setTextSize(1);
    display.setTextWrap(true);
    display.println("F L O R A");
    display.println("--------------------");
    display.println("Temporary Wi-Fi");
    display.println("cleared.");
    display.println();
    display.println("Primary network");
    display.println("remains available.");

    delay(1000);
}

bool tryWifiCredentials(
    const String &ssid,
    const String &password,
    const String &sourceLabel)
{
    if (ssid.length() == 0)
    {
        return false;
    }

    showNetworkStatus(
        "Connecting Wi-Fi...",
        sourceLabel
    );

    Serial.print(
        "Connecting Wi-Fi to SSID: "
    );
    Serial.println(ssid);

    Serial.print(
        "Wi-Fi credential source: "
    );
    Serial.println(sourceLabel);

    WiFi.disconnect(true);
    delay(150);
    WiFi.mode(WIFI_STA);

    WiFi.begin(
        ssid.c_str(),
        password.c_str()
    );

    const unsigned long started =
        millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - started >=
            WIFI_CONNECT_TIMEOUT_MS)
        {
            Serial.print(
                "Wi-Fi connect FAILED: "
            );
            Serial.println(
                sourceLabel
            );

            WiFi.disconnect(true);
            delay(100);
            return false;
        }

        delay(250);
    }

    Serial.println("Wi-Fi connected.");

    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    showNetworkStatus(
        "Wi-Fi connected",
        WiFi.localIP().toString()
    );

    return true;
}

bool connectWifi(
    bool allowInteractivePrompt = true)
{
    bool connected =
        false;

    // Once a temporary network has been entered, prefer it for the rest
    // of this boot/session. The compiled primary network remains fallback.
    if (sessionWifiConfigured)
    {
        connected =
            tryWifiCredentials(
                sessionWifiSsid,
                sessionWifiPassword,
                "temporary"
            );
    }

    if (!connected)
    {
        connected =
            tryWifiCredentials(
                String(RHEA_WIFI_SSID),
                String(RHEA_WIFI_PASSWORD),
                "primary"
            );
    }

    if (!connected &&
        allowInteractivePrompt)
    {
        showNetworkStatus(
            "Primary Wi-Fi unavailable",
            "Enter temporary network"
        );

        if (promptForTemporaryWifi())
        {
            connected =
                tryWifiCredentials(
                    sessionWifiSsid,
                    sessionWifiPassword,
                    "temporary"
                );
        }
    }

    if (!connected)
    {
        Serial.println(
            "Wi-Fi connect FAILED: no usable network"
        );

        showNetworkStatus(
            "Wi-Fi FAILED",
            "Continuing offline."
        );

        return false;
    }

    if (!syncClockForTls())
    {
        showNetworkStatus(
            "Clock sync FAILED",
            "TLS blocked."
        );

        return false;
    }

    delay(700);
    return true;
}

void printTlsLastError(
    WiFiClientSecure &client)
{
    char buffer[160];
    buffer[0] = '\0';

    const int errorCode =
        client.lastError(
            buffer,
            sizeof(buffer)
        );

    Serial.print("TLS lastError code: ");
    Serial.println(errorCode);

    if (buffer[0] != '\0')
    {
        Serial.print("TLS lastError text: ");
        Serial.println(buffer);
    }
}

bool waitForResponseData(
    WiFiClientSecure &client,
    unsigned long timeoutMs)
{
    const unsigned long started =
        millis();

    unsigned long lastProgressElapsed =
        0;

    while (!client.available())
    {
        if (!client.connected())
        {
            Serial.println(
                "HTTPS peer closed before response data arrived."
            );

            printTlsLastError(client);
            return false;
        }

        const unsigned long elapsed =
            millis() - started;

        if (elapsed >=
            lastProgressElapsed + 10000)
        {
            Serial.print(
                "Waiting for HTTP response... "
            );

            Serial.print(elapsed / 1000);
            Serial.println("s");

            lastProgressElapsed =
                elapsed;
        }

        if (elapsed >= timeoutMs)
        {
            Serial.println(
                "HTTPS response FAILED: 300s wait timeout"
            );

            return false;
        }

        delay(10);
    }

    return true;
}

bool readHttpResponse(
    WiFiClientSecure &client,
    int &statusCode,
    String &body,
    String &etag)
{
    if (!waitForResponseData(
            client,
            HTTPS_READ_TIMEOUT_MS))
    {
        return false;
    }

    String statusLine =
        client.readStringUntil('\n');

    statusLine.trim();

    if (statusLine.length() == 0)
    {
        Serial.println(
            "HTTPS response FAILED: empty status line"
        );

        printTlsLastError(client);
        return false;
    }

    Serial.print("HTTP status line: ");
    Serial.println(statusLine);

    const int firstSpace =
        statusLine.indexOf(' ');

    if (firstSpace < 0)
    {
        Serial.println(
            "HTTPS response FAILED: malformed status line"
        );

        return false;
    }

    const int secondSpace =
        statusLine.indexOf(
            ' ',
            firstSpace + 1
        );

    String statusText;

    if (secondSpace >= 0)
    {
        statusText =
            statusLine.substring(
                firstSpace + 1,
                secondSpace
            );
    }
    else
    {
        statusText =
            statusLine.substring(
                firstSpace + 1
            );
    }

    statusCode =
        statusText.toInt();

    int contentLength = -1;
    bool chunked = false;
    etag = "";

    while (true)
    {
        if (!waitForResponseData(
                client,
                HTTPS_READ_TIMEOUT_MS))
        {
            return false;
        }

        String headerLine =
            client.readStringUntil('\n');

        headerLine.trim();

        if (headerLine.length() == 0)
        {
            break;
        }

        String lowerHeader =
            headerLine;

        lowerHeader.toLowerCase();

        if (lowerHeader.startsWith(
                "content-length:"))
        {
            String value =
                headerLine.substring(
                    headerLine.indexOf(':') + 1
                );

            value.trim();
            contentLength =
                value.toInt();
        }
        else if (
            lowerHeader.startsWith(
                "transfer-encoding:") &&
            lowerHeader.indexOf("chunked") >= 0)
        {
            chunked = true;
        }
        else if (
            lowerHeader.startsWith(
                "etag:"))
        {
            etag =
                headerLine.substring(
                    headerLine.indexOf(':') + 1
                );

            etag.trim();
        }
    }

    body = "";

    if (chunked)
    {
        Serial.println(
            "HTTPS response uses chunked encoding."
        );

        while (true)
        {
            if (!waitForResponseData(
                    client,
                    HTTPS_READ_TIMEOUT_MS))
            {
                return false;
            }

            String chunkSizeLine =
                client.readStringUntil('\n');

            chunkSizeLine.trim();

            const long chunkSize =
                strtol(
                    chunkSizeLine.c_str(),
                    nullptr,
                    16
                );

            if (chunkSize < 0)
            {
                Serial.println(
                    "Chunked response FAILED: invalid chunk size"
                );

                return false;
            }

            if (chunkSize == 0)
            {
                return true;
            }

            long received = 0;

            while (received < chunkSize)
            {
                if (!waitForResponseData(
                        client,
                        HTTPS_READ_TIMEOUT_MS))
                {
                    return false;
                }

                while (
                    client.available() &&
                    received < chunkSize)
                {
                    const int value =
                        client.read();

                    if (value >= 0)
                    {
                        body +=
                            (char)value;

                        ++received;
                    }
                }
            }

            if (!waitForResponseData(
                    client,
                    HTTPS_READ_TIMEOUT_MS))
            {
                return false;
            }

            (void)client.read();
            (void)client.read();
        }
    }

    if (contentLength >= 0)
    {
        body.reserve(
            contentLength
        );

        while (
            (int)body.length() <
            contentLength)
        {
            if (!waitForResponseData(
                    client,
                    HTTPS_READ_TIMEOUT_MS))
            {
                return false;
            }

            while (
                client.available() &&
                (int)body.length() <
                    contentLength)
            {
                const int value =
                    client.read();

                if (value >= 0)
                {
                    body +=
                        (char)value;
                }
            }
        }

        return true;
    }

    Serial.println(
        "No Content-Length; reading until close."
    );

    unsigned long lastData =
        millis();

    while (
        client.connected() ||
        client.available())
    {
        if (client.available())
        {
            const int value =
                client.read();

            if (value >= 0)
            {
                body +=
                    (char)value;

                lastData =
                    millis();
            }
        }
        else
        {
            if (
                millis() - lastData >=
                HTTPS_READ_TIMEOUT_MS)
            {
                Serial.println(
                    "HTTPS body FAILED: read timeout"
                );

                return false;
            }

            delay(10);
        }
    }

    return body.length() > 0;
}

bool fetchManifestAttempt(
    String &payload,
    int &statusCode,
    String &etag)
{
    WiFiClientSecure client;

    client.setCACert(RHEA_TRUSTED_ROOTS);

    Serial.println(
        "TLS verification: DigiCert G2/G3 CA trust enabled"
    );
    client.setTimeout(
        HTTPS_READ_TIMEOUT_MS
    );

    Serial.print(
        "Connecting HTTPS to "
    );

    Serial.print(
        RHEA_MANIFEST_HOST
    );

    Serial.print(":");
    Serial.println(
        RHEA_MANIFEST_PORT
    );

    if (!client.connect(
            RHEA_MANIFEST_HOST,
            RHEA_MANIFEST_PORT))
    {
        Serial.println(
            "HTTPS connect FAILED"
        );

        printTlsLastError(client);
        return false;
    }

    Serial.println(
        "HTTPS connected."
    );

    const String request =
        String("GET ") +
        RHEA_MANIFEST_PATH +
        " HTTP/1.0\r\n" +
        "Host: " +
        RHEA_MANIFEST_HOST +
        "\r\n" +
        "Accept: application/json\r\n" +
        "Accept-Encoding: identity\r\n" +
        "User-Agent: Rhea-Cardputer/1\r\n" +
        "x-functions-key: " +
        String(RHEA_DEVICE_FUNCTION_KEY) +
        "\r\n" +
        "Connection: close\r\n" +
        "\r\n";

    Serial.print(
        "Sending manifest GET ("
    );

    Serial.print(
        request.length()
    );

    Serial.println(
        " bytes)..."
    );

    const size_t bytesWritten =
        client.write(
            (const uint8_t *)
                request.c_str(),
            request.length()
        );

    Serial.print(
        "HTTPS request bytes written: "
    );

    Serial.print(
        bytesWritten
    );

    Serial.print("/");
    Serial.println(
        request.length()
    );

    if (bytesWritten !=
        request.length())
    {
        Serial.println(
            "HTTPS request FAILED: short write"
        );

        printTlsLastError(client);
        client.stop();
        return false;
    }

    const bool responseOk =
        readHttpResponse(
            client,
            statusCode,
            payload,
            etag
        );

    client.stop();
    return responseOk;
}

void showManifest(
    const char *format,
    int schemaVersion,
    int recordCount,
    int bytes,
    const char *snapshotVersion,
    const char *sha256,
    const char *etag)
{
    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextColor(
        GREEN,
        BLACK
    );
    display.setTextSize(1);
    display.setTextWrap(true);

    display.println("F L O R A");
    display.println(
        "Production manifest"
    );
    display.println(
        "--------------------"
    );

    display.print("Format: ");
    display.println(format);

    display.print("Schema: ");
    display.println(schemaVersion);

    display.print("Records: ");
    display.println(recordCount);

    display.print("Bytes: ");
    display.println(bytes);

    display.println(
        "Snapshot version:"
    );
    display.println(
        snapshotVersion
    );

    display.println("SHA-256:");
    display.println(sha256);

    Serial.println();
    Serial.println(
        "=== RHEA PRODUCTION MANIFEST ==="
    );

    Serial.print("format: ");
    Serial.println(format);

    Serial.print(
        "schemaVersion: "
    );
    Serial.println(
        schemaVersion
    );

    Serial.print(
        "recordCount: "
    );
    Serial.println(
        recordCount
    );

    Serial.print("bytes: ");
    Serial.println(bytes);

    Serial.print(
        "snapshotVersion: "
    );
    Serial.println(
        snapshotVersion
    );

    Serial.print("sha256: ");
    Serial.println(sha256);

    Serial.print("etag: ");
    Serial.println(etag);

    Serial.println(
        "================================"
    );
}

bool fetchManifest()
{
    if (!connectWifi())
    {
        return false;
    }

    bool success = false;

    for (
        int attempt = 1;
        attempt <=
            MANIFEST_MAX_ATTEMPTS;
        ++attempt)
    {
        Serial.print(
            "Manifest attempt "
        );
        Serial.print(attempt);
        Serial.print("/");
        Serial.println(
            MANIFEST_MAX_ATTEMPTS
        );

        showNetworkStatus(
            "Fetching manifest...",
            "Attempt " +
                String(attempt)
        );

        String payload;
        int statusCode = 0;
        String etag;

        const bool responseOk =
            fetchManifestAttempt(
                payload,
                statusCode,
                etag
            );

        if (!responseOk)
        {
            Serial.println(
                "Manifest transport FAILED"
            );
        }
        else
        {
            Serial.print(
                "Manifest HTTP status: "
            );
            Serial.println(
                statusCode
            );

            if (statusCode == 200)
            {
                StaticJsonDocument<1024>
                    manifest;

                const
                    DeserializationError
                    error =
                        deserializeJson(
                            manifest,
                            payload
                        );

                if (error)
                {
                    Serial.print(
                        "Manifest JSON parse FAILED: "
                    );

                    Serial.println(
                        error.c_str()
                    );
                }
                else
                {
                    const char *format =
                        manifest["format"] |
                        "";

                    const int
                        schemaVersion =
                            manifest[
                                "schemaVersion"
                            ] |
                            0;

                    const int
                        recordCount =
                            manifest[
                                "recordCount"
                            ] |
                            0;

                    const int bytes =
                        manifest["bytes"] |
                        0;

                    const char *
                        snapshotVersion =
                            manifest[
                                "snapshotVersion"
                            ] |
                            "";

                    const char *sha256 =
                        manifest["sha256"] |
                        "";

                    if (
                        strcmp(
                            format,
                            "RHEA1") != 0 ||
                        schemaVersion != 3 ||
                        recordCount <= 0 ||
                        bytes <= 0 ||
                        strlen(
                            snapshotVersion
                        ) != 64 ||
                        strlen(sha256) !=
                            64)
                    {
                        Serial.println(
                            "Manifest contract FAILED"
                        );
                    }
                    else
                    {
                        productionManifest.format =
                            format;

                        productionManifest
                            .schemaVersion =
                                schemaVersion;

                        productionManifest
                            .recordCount =
                                (size_t)
                                    recordCount;

                        productionManifest
                            .bytes =
                                (size_t)bytes;

                        productionManifest
                            .snapshotVersion =
                                snapshotVersion;

                        productionManifest
                            .sha256 =
                                sha256;

                        productionManifest
                            .etag =
                                etag;

                        showManifest(
                            format,
                            schemaVersion,
                            recordCount,
                            bytes,
                            snapshotVersion,
                            sha256,
                            etag.c_str()
                        );

                        success = true;
                        break;
                    }
                }
            }
            else
            {
                Serial.print(
                    "Manifest HTTP error body: "
                );

                Serial.println(
                    payload
                );
            }
        }

        if (
            attempt <
            MANIFEST_MAX_ATTEMPTS)
        {
            delay(3000);
        }
    }

    if (!success)
    {
        showNetworkStatus(
            "Manifest FAILED",
            "Continuing offline."
        );
    }

    return success;
}

bool readResponseHeaders(
    WiFiClientSecure &client,
    int &statusCode,
    bool &chunked,
    long &contentLength,
    String &etag,
    String &contentSha256)
{
    if (!waitForResponseData(
            client,
            HTTPS_READ_TIMEOUT_MS))
    {
        return false;
    }

    String statusLine =
        client.readStringUntil('\n');

    statusLine.trim();

    Serial.print(
        "Snapshot HTTP status line: "
    );

    Serial.println(
        statusLine
    );

    const int firstSpace =
        statusLine.indexOf(' ');

    if (firstSpace < 0)
    {
        Serial.println(
            "Snapshot FAILED: malformed status line"
        );

        return false;
    }

    const int secondSpace =
        statusLine.indexOf(
            ' ',
            firstSpace + 1
        );

    String statusText;

    if (secondSpace >= 0)
    {
        statusText =
            statusLine.substring(
                firstSpace + 1,
                secondSpace
            );
    }
    else
    {
        statusText =
            statusLine.substring(
                firstSpace + 1
            );
    }

    statusCode =
        statusText.toInt();

    chunked = false;
    contentLength = -1;
    etag = "";
    contentSha256 = "";

    while (true)
    {
        if (!waitForResponseData(
                client,
                HTTPS_READ_TIMEOUT_MS))
        {
            return false;
        }

        String headerLine =
            client.readStringUntil('\n');

        headerLine.trim();

        if (
            headerLine.length() ==
            0)
        {
            break;
        }

        String lowerHeader =
            headerLine;

        lowerHeader.toLowerCase();

        if (lowerHeader.startsWith(
                "content-length:"))
        {
            String value =
                headerLine.substring(
                    headerLine.indexOf(
                        ':') +
                    1
                );

            value.trim();

            contentLength =
                value.toInt();
        }
        else if (
            lowerHeader.startsWith(
                "transfer-encoding:") &&
            lowerHeader.indexOf(
                "chunked") >= 0)
        {
            chunked = true;
        }
        else if (
            lowerHeader.startsWith(
                "etag:"))
        {
            etag =
                headerLine.substring(
                    headerLine.indexOf(
                        ':') +
                    1
                );

            etag.trim();
        }
        else if (
            lowerHeader.startsWith(
                "x-content-sha256:"))
        {
            contentSha256 =
                headerLine.substring(
                    headerLine.indexOf(
                        ':') +
                    1
                );

            contentSha256.trim();
        }
    }

    return true;
}

bool writeAndHash(
    File &file,
    mbedtls_sha256_context &sha,
    const uint8_t *buffer,
    size_t length,
    size_t &totalBytes)
{
    if (length == 0)
    {
        return true;
    }

    size_t offset = 0;
    unsigned long lastProgress =
        millis();

    while (offset < length)
    {
        const size_t remaining =
            length - offset;

        const size_t written =
            file.write(
                buffer + offset,
                remaining
            );

        if (written > 0)
        {
            if (
                mbedtls_sha256_update_ret(
                    &sha,
                    buffer + offset,
                    written) != 0)
            {
                Serial.println(
                    "Snapshot SHA-256 update FAILED"
                );

                return false;
            }

            offset += written;
            totalBytes += written;
            lastProgress = millis();
            continue;
        }

        if (
            millis() -
                lastProgress >=
            5000)
        {
            Serial.print(
                "Snapshot SD write FAILED: stalled after "
            );

            Serial.print(offset);
            Serial.print("/");
            Serial.print(length);
            Serial.println(
                " bytes"
            );

            return false;
        }

        delay(2);
    }

    return true;
}

bool readExactBodyBytes(
    WiFiClientSecure &client,
    File &file,
    mbedtls_sha256_context &sha,
    size_t expectedBytes,
    size_t &totalBytes)
{
    uint8_t buffer[512];
    unsigned long lastProgress =
        millis();

    while (
        totalBytes <
        expectedBytes)
    {
        if (!waitForResponseData(
                client,
                HTTPS_READ_TIMEOUT_MS))
        {
            return false;
        }

        size_t availableBytes =
            (size_t)
                client.available();

        size_t remaining =
            expectedBytes -
            totalBytes;

        size_t toRead =
            availableBytes;

        if (
            toRead >
            sizeof(buffer))
        {
            toRead =
                sizeof(buffer);
        }

        if (
            toRead >
            remaining)
        {
            toRead =
                remaining;
        }

        const int count =
            client.read(
                buffer,
                toRead
            );

        if (count <= 0)
        {
            continue;
        }

        if (!writeAndHash(
                file,
                sha,
                buffer,
                (size_t)count,
                totalBytes))
        {
            return false;
        }

        if (
            millis() -
                lastProgress >=
            2000)
        {
            Serial.print(
                "Snapshot progress: "
            );

            Serial.print(
                totalBytes
            );

            Serial.print("/");
            Serial.println(
                expectedBytes
            );

            lastProgress =
                millis();
        }
    }

    return true;
}

bool readChunkedSnapshotBody(
    WiFiClientSecure &client,
    File &file,
    mbedtls_sha256_context &sha,
    size_t expectedBytes,
    size_t &totalBytes)
{
    uint8_t buffer[512];

    while (true)
    {
        if (!waitForResponseData(
                client,
                HTTPS_READ_TIMEOUT_MS))
        {
            return false;
        }

        String chunkSizeLine =
            client.readStringUntil('\n');

        chunkSizeLine.trim();

        const int extensionIndex =
            chunkSizeLine.indexOf(
                ';'
            );

        if (extensionIndex >= 0)
        {
            chunkSizeLine =
                chunkSizeLine.substring(
                    0,
                    extensionIndex
                );

            chunkSizeLine.trim();
        }

        char *endPtr = nullptr;

        const unsigned long chunkSize =
            strtoul(
                chunkSizeLine.c_str(),
                &endPtr,
                16
            );

        if (
            endPtr ==
                chunkSizeLine.c_str() ||
            *endPtr != '\0')
        {
            Serial.println(
                "Snapshot FAILED: invalid chunk size"
            );

            return false;
        }

        if (chunkSize == 0)
        {
            return
                totalBytes ==
                expectedBytes;
        }

        if (
            totalBytes +
                chunkSize >
            expectedBytes)
        {
            Serial.println(
                "Snapshot FAILED: chunk exceeds manifest byte count"
            );

            return false;
        }

        size_t remainingInChunk =
            (size_t)chunkSize;

        while (
            remainingInChunk > 0)
        {
            if (!waitForResponseData(
                    client,
                    HTTPS_READ_TIMEOUT_MS))
            {
                return false;
            }

            size_t toRead =
                (size_t)
                    client.available();

            if (
                toRead >
                sizeof(buffer))
            {
                toRead =
                    sizeof(buffer);
            }

            if (
                toRead >
                remainingInChunk)
            {
                toRead =
                    remainingInChunk;
            }

            const int count =
                client.read(
                    buffer,
                    toRead
                );

            if (count <= 0)
            {
                continue;
            }

            if (!writeAndHash(
                    file,
                    sha,
                    buffer,
                    (size_t)count,
                    totalBytes))
            {
                return false;
            }

            remainingInChunk -=
                (size_t)count;
        }

        if (!waitForResponseData(
                client,
                HTTPS_READ_TIMEOUT_MS))
        {
            return false;
        }

        const int cr =
            client.read();

        if (!waitForResponseData(
                client,
                HTTPS_READ_TIMEOUT_MS))
        {
            return false;
        }

        const int lf =
            client.read();

        if (
            cr != '\r' ||
            lf != '\n')
        {
            Serial.println(
                "Snapshot FAILED: bad chunk terminator"
            );

            return false;
        }
    }
}

bool hashFileOnSd(
    const char *path,
    size_t &bytesRead,
    String &sha256)
{
    File file =
        SD.open(
            path,
            FILE_READ
        );

    if (!file)
    {
        Serial.print(
            "SD verify open FAILED: "
        );
        Serial.println(path);
        return false;
    }

    const size_t reportedSize =
        file.size();

    Serial.print(
        "SD file reported size: "
    );
    Serial.println(
        reportedSize
    );

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);

    if (
        mbedtls_sha256_starts_ret(
            &sha,
            0) != 0)
    {
        Serial.println(
            "SD verify SHA-256 init FAILED"
        );

        mbedtls_sha256_free(
            &sha
        );

        file.close();
        return false;
    }

    uint8_t buffer[512];
    bytesRead = 0;

    while (file.available())
    {
        const int count =
            file.read(
                buffer,
                sizeof(buffer)
            );

        if (count < 0)
        {
            Serial.println(
                "SD verify read FAILED"
            );

            mbedtls_sha256_free(
                &sha
            );

            file.close();
            return false;
        }

        if (count == 0)
        {
            break;
        }

        if (
            mbedtls_sha256_update_ret(
                &sha,
                buffer,
                (size_t)count) != 0)
        {
            Serial.println(
                "SD verify SHA-256 update FAILED"
            );

            mbedtls_sha256_free(
                &sha
            );

            file.close();
            return false;
        }

        bytesRead +=
            (size_t)count;
    }

    unsigned char digest[32];

    const int finishResult =
        mbedtls_sha256_finish_ret(
            &sha,
            digest
        );

    mbedtls_sha256_free(
        &sha
    );

    file.close();

    if (finishResult != 0)
    {
        Serial.println(
            "SD verify SHA-256 finish FAILED"
        );

        return false;
    }

    sha256 =
        toUpperHex(
            digest,
            sizeof(digest)
        );

    Serial.print(
        "SD file bytes reread: "
    );
    Serial.println(
        bytesRead
    );

    Serial.print(
        "SD file reread SHA-256: "
    );
    Serial.println(
        sha256
    );

    return true;
}

bool verifyFileOnSd(
    const char *path,
    size_t expectedBytes,
    const String &expectedHash)
{
    Serial.print(
        "Verifying SD file: "
    );
    Serial.println(path);

    size_t bytesRead = 0;
    String sha256;

    if (!hashFileOnSd(
            path,
            bytesRead,
            sha256))
    {
        return false;
    }

    if (
        bytesRead !=
        expectedBytes)
    {
        Serial.println(
            "SD verify FAILED: byte count mismatch"
        );

        return false;
    }

    if (
        !sha256.equalsIgnoreCase(
            expectedHash))
    {
        Serial.println(
            "SD verify FAILED: SHA-256 mismatch"
        );

        return false;
    }

    Serial.println(
        "SD file verification: PASS"
    );

    return true;
}

bool activeSnapshotIsCurrent()
{
    if (!SD.exists(
            RHEA_SNAPSHOT_ACTIVE_PATH))
    {
        Serial.println(
            "No installed production snapshot."
        );

        return false;
    }

    Serial.println();
    Serial.println(
        "=== CHECK INSTALLED PRODUCTION SNAPSHOT ==="
    );

    const bool current =
        verifyFileOnSd(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            productionManifest.bytes,
            productionManifest.sha256
        );

    if (current)
    {
        Serial.println(
            "Installed production snapshot is current."
        );
    }
    else
    {
        Serial.println(
            "Installed production snapshot differs from current manifest."
        );
    }

    return current;
}

bool installVerifiedCandidate()
{
    Serial.println();
    Serial.println(
        "=== RHEA PRODUCTION SNAPSHOT INSTALL ==="
    );

    if (!SD.exists(
            RHEA_SNAPSHOT_TEMP_PATH))
    {
        Serial.println(
            "INSTALL FAILED: verified candidate is missing"
        );

        return false;
    }

    if (!verifyFileOnSd(
            RHEA_SNAPSHOT_TEMP_PATH,
            productionManifest.bytes,
            productionManifest.sha256))
    {
        Serial.println(
            "INSTALL FAILED: candidate failed pre-install verification"
        );

        return false;
    }

    // A pre-existing backup means an earlier transaction may have
    // been interrupted. Preserve it and refuse to guess.
    if (SD.exists(
            RHEA_SNAPSHOT_BACKUP_PATH))
    {
        Serial.println(
            "INSTALL BLOCKED: backup file already exists"
        );

        Serial.print(
            "Preserved recovery file: "
        );

        Serial.println(
            RHEA_SNAPSHOT_BACKUP_PATH
        );

        Serial.println(
            "No production snapshot files were changed."
        );

        return false;
    }

    const bool hadActive =
        SD.exists(
            RHEA_SNAPSHOT_ACTIVE_PATH
        );

    size_t oldActiveBytes = 0;
    String oldActiveHash;

    if (hadActive)
    {
        Serial.println(
            "Capturing current active snapshot before swap..."
        );

        if (!hashFileOnSd(
                RHEA_SNAPSHOT_ACTIVE_PATH,
                oldActiveBytes,
                oldActiveHash))
        {
            Serial.println(
                "INSTALL FAILED: existing active snapshot could not be verified for rollback"
            );

            return false;
        }

        Serial.println(
            "Renaming active snapshot to backup..."
        );

        if (!SD.rename(
                RHEA_SNAPSHOT_ACTIVE_PATH,
                RHEA_SNAPSHOT_BACKUP_PATH))
        {
            Serial.println(
                "INSTALL FAILED: active-to-backup rename failed"
            );

            return false;
        }

        if (!SD.exists(
                RHEA_SNAPSHOT_BACKUP_PATH))
        {
            Serial.println(
                "INSTALL FAILED: backup not visible after rename"
            );

            return false;
        }
    }

    Serial.println(
        "Promoting verified candidate to active..."
    );

    if (!SD.rename(
            RHEA_SNAPSHOT_TEMP_PATH,
            RHEA_SNAPSHOT_ACTIVE_PATH))
    {
        Serial.println(
            "INSTALL FAILED: candidate-to-active rename failed"
        );

        if (hadActive &&
            SD.exists(
                RHEA_SNAPSHOT_BACKUP_PATH))
        {
            Serial.println(
                "Attempting rollback..."
            );

            if (SD.rename(
                    RHEA_SNAPSHOT_BACKUP_PATH,
                    RHEA_SNAPSHOT_ACTIVE_PATH))
            {
                Serial.println(
                    "ROLLBACK: active snapshot restored"
                );
            }
            else
            {
                Serial.println(
                    "ROLLBACK FAILED: backup preserved for manual recovery"
                );
            }
        }

        return false;
    }

    Serial.println(
        "Post-install verification..."
    );

    if (!verifyFileOnSd(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            productionManifest.bytes,
            productionManifest.sha256))
    {
        Serial.println(
            "INSTALL FAILED: new active snapshot did not verify"
        );

        if (SD.exists(
                RHEA_SNAPSHOT_ACTIVE_PATH))
        {
            if (!SD.remove(
                    RHEA_SNAPSHOT_ACTIVE_PATH))
            {
                Serial.println(
                    "ROLLBACK BLOCKED: failed to remove bad new active snapshot"
                );

                return false;
            }
        }

        if (hadActive)
        {
            Serial.println(
                "Restoring previous active snapshot..."
            );

            if (!SD.rename(
                    RHEA_SNAPSHOT_BACKUP_PATH,
                    RHEA_SNAPSHOT_ACTIVE_PATH))
            {
                Serial.println(
                    "ROLLBACK FAILED: backup preserved for manual recovery"
                );

                return false;
            }

            if (!verifyFileOnSd(
                    RHEA_SNAPSHOT_ACTIVE_PATH,
                    oldActiveBytes,
                    oldActiveHash))
            {
                Serial.println(
                    "ROLLBACK FAILED: restored snapshot verification failed"
                );

                return false;
            }

            Serial.println(
                "ROLLBACK: previous active snapshot restored and verified"
            );
        }

        return false;
    }

    if (hadActive &&
        SD.exists(
            RHEA_SNAPSHOT_BACKUP_PATH))
    {
        Serial.println(
            "New active verified. Removing old backup..."
        );

        if (!SD.remove(
                RHEA_SNAPSHOT_BACKUP_PATH))
        {
            Serial.println(
                "WARNING: new active is valid, but old backup could not be removed"
            );

            Serial.println(
                "Future installs will stop until the stale backup is reconciled."
            );
        }
    }

    Serial.println();
    Serial.println(
        "RHEA PRODUCTION SNAPSHOT INSTALL: PASS"
    );

    Serial.print(
        "Active path: "
    );
    Serial.println(
        RHEA_SNAPSHOT_ACTIVE_PATH
    );

    Serial.print(
        "Bytes: "
    );
    Serial.println(
        productionManifest.bytes
    );

    Serial.print(
        "SHA-256: "
    );
    Serial.println(
        productionManifest.sha256
    );

    Serial.println(
        "Installed snapshot is verified and authoritative."
    );

    Serial.println(
        "Current search UI still uses the five-record sample database."
    );

    Serial.println(
        "========================================"
    );

    return true;
}

bool downloadSnapshotCandidate()
{
    if (
        productionManifest.format !=
            "RHEA1" ||
        productionManifest
                .schemaVersion !=
            3 ||
        productionManifest.bytes ==
            0 ||
        productionManifest
                .sha256.length() !=
            64)
    {
        Serial.println(
            "Snapshot skipped: manifest is not valid."
        );

        return false;
    }

    showNetworkStatus(
        "Downloading snapshot...",
        "Candidate only"
    );

    if (
        SD.exists(
            RHEA_SNAPSHOT_TEMP_PATH))
    {
        if (!SD.remove(
                RHEA_SNAPSHOT_TEMP_PATH))
        {
            Serial.println(
                "Could not remove old snapshot candidate."
            );

            return false;
        }
    }

    File file =
        SD.open(
            RHEA_SNAPSHOT_TEMP_PATH,
            FILE_WRITE
        );

    if (!file)
    {
        Serial.println(
            "Snapshot candidate file open FAILED"
        );

        return false;
    }

    WiFiClientSecure client;

    client.setCACert(RHEA_TRUSTED_ROOTS);

    Serial.println(
        "TLS verification: DigiCert G2/G3 CA trust enabled"
    );

    client.setTimeout(
        HTTPS_READ_TIMEOUT_MS
    );

    Serial.println();
    Serial.println(
        "=== RHEA SNAPSHOT CANDIDATE DOWNLOAD ==="
    );

    Serial.print(
        "Expected bytes: "
    );

    Serial.println(
        productionManifest.bytes
    );

    Serial.print(
        "Expected SHA-256: "
    );

    Serial.println(
        productionManifest.sha256
    );

    Serial.print(
        "Connecting HTTPS to "
    );

    Serial.print(
        RHEA_MANIFEST_HOST
    );

    Serial.print(":");
    Serial.println(
        RHEA_MANIFEST_PORT
    );

    if (!client.connect(
            RHEA_MANIFEST_HOST,
            RHEA_MANIFEST_PORT))
    {
        Serial.println(
            "Snapshot HTTPS connect FAILED"
        );

        printTlsLastError(client);

        file.close();
        SD.remove(
            RHEA_SNAPSHOT_TEMP_PATH
        );

        return false;
    }

    const String request =
        String("GET ") +
        RHEA_SNAPSHOT_PATH +
        " HTTP/1.0\r\n" +
        "Host: " +
        RHEA_MANIFEST_HOST +
        "\r\n" +
        "Accept: application/json\r\n" +
        "Accept-Encoding: identity\r\n" +
        "User-Agent: Rhea-Cardputer/1\r\n" +
        "x-functions-key: " +
        String(RHEA_DEVICE_FUNCTION_KEY) +
        "\r\n" +
        "Connection: close\r\n" +
        "\r\n";

    const size_t bytesWritten =
        client.write(
            (const uint8_t *)
                request.c_str(),
            request.length()
        );

    Serial.print(
        "Snapshot request bytes written: "
    );

    Serial.print(
        bytesWritten
    );

    Serial.print("/");
    Serial.println(
        request.length()
    );

    if (
        bytesWritten !=
        request.length())
    {
        Serial.println(
            "Snapshot request FAILED: short write"
        );

        file.close();
        client.stop();

        SD.remove(
            RHEA_SNAPSHOT_TEMP_PATH
        );

        return false;
    }

    int statusCode = 0;
    bool chunked = false;
    long contentLength = -1;
    String etag;
    String contentSha256;

    if (!readResponseHeaders(
            client,
            statusCode,
            chunked,
            contentLength,
            etag,
            contentSha256))
    {
        file.close();
        client.stop();

        SD.remove(
            RHEA_SNAPSHOT_TEMP_PATH
        );

        return false;
    }

    Serial.print(
        "Snapshot HTTP status: "
    );
    Serial.println(
        statusCode
    );

    Serial.print(
        "Snapshot ETag: "
    );
    Serial.println(
        etag
    );

    Serial.print(
        "X-Content-SHA256: "
    );
    Serial.println(
        contentSha256
    );

    if (statusCode != 200)
    {
        Serial.println(
            "Snapshot FAILED: HTTP status not 200"
        );

        file.close();
        client.stop();

        SD.remove(
            RHEA_SNAPSHOT_TEMP_PATH
        );

        return false;
    }

    const String expectedEtag =
        String("\"") +
        productionManifest.sha256 +
        "\"";

    if (etag != expectedEtag)
    {
        Serial.println(
            "Snapshot FAILED: ETag mismatch"
        );

        file.close();
        client.stop();

        SD.remove(
            RHEA_SNAPSHOT_TEMP_PATH
        );

        return false;
    }

    if (
        contentSha256 !=
        productionManifest.sha256)
    {
        Serial.println(
            "Snapshot FAILED: X-Content-SHA256 mismatch"
        );

        file.close();
        client.stop();

        SD.remove(
            RHEA_SNAPSHOT_TEMP_PATH
        );

        return false;
    }

    if (
        contentLength >= 0 &&
        (size_t)contentLength !=
            productionManifest.bytes)
    {
        Serial.println(
            "Snapshot FAILED: Content-Length mismatch"
        );

        file.close();
        client.stop();

        SD.remove(
            RHEA_SNAPSHOT_TEMP_PATH
        );

        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);

    if (
        mbedtls_sha256_starts_ret(
            &sha,
            0) != 0)
    {
        Serial.println(
            "Snapshot SHA-256 init FAILED"
        );

        mbedtls_sha256_free(
            &sha
        );

        file.close();
        client.stop();

        SD.remove(
            RHEA_SNAPSHOT_TEMP_PATH
        );

        return false;
    }

    size_t totalBytes = 0;
    bool bodyOk = false;

    if (chunked)
    {
        Serial.println(
            "Snapshot transfer: chunked"
        );

        bodyOk =
            readChunkedSnapshotBody(
                client,
                file,
                sha,
                productionManifest.bytes,
                totalBytes
            );
    }
    else
    {
        if (
            contentLength >=
            0)
        {
            Serial.println(
                "Snapshot transfer: Content-Length"
            );
        }
        else
        {
            Serial.println(
                "Snapshot transfer: manifest byte framing"
            );
        }

        bodyOk =
            readExactBodyBytes(
                client,
                file,
                sha,
                productionManifest.bytes,
                totalBytes
            );
    }

    unsigned char digest[32];

    const int finishResult =
        mbedtls_sha256_finish_ret(
            &sha,
            digest
        );

    mbedtls_sha256_free(
        &sha
    );

    file.flush();
    file.close();
    client.stop();

    if (
        !bodyOk ||
        finishResult != 0)
    {
        Serial.println(
            "Snapshot FAILED during body/hash processing"
        );

        SD.remove(
            RHEA_SNAPSHOT_TEMP_PATH
        );

        return false;
    }

    const String actualSha256 =
        toUpperHex(
            digest,
            sizeof(digest)
        );

    Serial.print(
        "Downloaded bytes: "
    );
    Serial.println(
        totalBytes
    );

    Serial.print(
        "Calculated SHA-256: "
    );
    Serial.println(
        actualSha256
    );

    if (
        totalBytes !=
        productionManifest.bytes)
    {
        Serial.println(
            "Snapshot FAILED: downloaded byte count mismatch"
        );

        return false;
    }

    if (
        !actualSha256
            .equalsIgnoreCase(
                productionManifest.sha256))
    {
        Serial.println(
            "Snapshot FAILED: calculated SHA-256 mismatch"
        );

        return false;
    }

    if (!verifyFileOnSd(
            RHEA_SNAPSHOT_TEMP_PATH,
            productionManifest.bytes,
            productionManifest.sha256))
    {
        return false;
    }

    Serial.println();
    Serial.println(
        "RHEA SNAPSHOT CANDIDATE: PASS"
    );

    Serial.print(
        "Candidate path: "
    );
    Serial.println(
        RHEA_SNAPSHOT_TEMP_PATH
    );

    Serial.print(
        "Bytes: "
    );
    Serial.println(
        productionManifest.bytes
    );

    Serial.print(
        "SHA-256: "
    );
    Serial.println(
        productionManifest.sha256
    );

    Serial.println(
        "TLS VERIFIED. Candidate is authenticated and hash-valid."
    );

    Serial.println(
        "Candidate is ready for rename-based installation."
    );

    Serial.println(
        "========================================"
    );

    showNetworkStatus(
        "Snapshot verified",
        "NOT installed"
    );

    return true;
}




bool readActiveSnapshotSummary(
    EnvelopeInfo &envelope,
    size_t &bytes)
{
    bytes = 0;

    if (!SD.exists(
            RHEA_SNAPSHOT_ACTIVE_PATH))
    {
        return false;
    }

    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        return false;
    }

    bytes =
        static_cast<size_t>(
            file.size()
        );

    const bool ok =
        locateRecordsArray(
            file,
            envelope
        );

    file.close();
    return ok;
}





bool quickSnapshotReadyCheck()
{
    if (!SD.exists(
            RHEA_SNAPSHOT_ACTIVE_PATH))
    {
        Serial.println(
            "Startup: active snapshot is missing."
        );
        return false;
    }

    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        Serial.println(
            "Startup: active snapshot could not be opened."
        );
        return false;
    }

    EnvelopeInfo envelope;

    const bool ok =
        locateRecordsArray(
            file,
            envelope
        );

    file.close();

    if (!ok)
    {
        Serial.println(
            "Startup: active snapshot envelope validation failed."
        );
        return false;
    }

    Serial.print(
        "Startup snapshot: "
    );
    Serial.print(envelope.format);
    Serial.print(" schema ");
    Serial.print(envelope.schemaVersion);
    Serial.print(" version ");
    Serial.println(envelope.snapshotVersion);

    return true;
}


void showSyncPhase(
    int step,
    int total,
    const String &title,
    const String &detail = "")
{
    Serial.print("SYNC_PHASE ");
    Serial.print(step);
    Serial.print("/");
    Serial.print(total);
    Serial.print(" ");
    Serial.println(title);

    String phase =
        String(step) +
        "/" +
        String(total) +
        " " +
        title;

    showNetworkStatus(
        phase,
        detail
    );
}

bool performOnDemandUpdate()
{
    Serial.println();
    Serial.println(
        "=== FLORA ON-DEMAND RECORD UPDATE ==="
    );

    showSyncPhase(
        1,
        5,
        "Connecting",
        "Private sync"
    );

    lastUpdateState =
        "running";

    if (!deviceFunctionKeyConfigured())
    {
        Serial.println(
            "UPDATE FAILED: RHEA_DEVICE_FUNCTION_KEY is not configured."
        );

        lastUpdateState =
            "failed";

        showNetworkStatus(
            "Update failed",
            "Device key missing"
        );

        return false;
    }

    bool manifestOk = false;
    bool alreadyCurrent = false;
    bool snapshotReady = false;
    bool installOk = false;

    manifestOk =
        fetchManifest();

    if (manifestOk)
    {
        showSyncPhase(
            2,
            5,
            "Checking version",
            "Manifest verified"
        );
        if (SD.exists(
                RHEA_SNAPSHOT_BACKUP_PATH))
        {
            Serial.println(
                "UPDATE BLOCKED: recovery backup already exists."
            );
            Serial.print(
                "Preserved backup: "
            );
            Serial.println(
                RHEA_SNAPSHOT_BACKUP_PATH
            );
        }
        else
        {
            alreadyCurrent =
                activeSnapshotIsCurrent();

            if (!alreadyCurrent)
            {
                showSyncPhase(
                    3,
                    5,
                    "Downloading",
                    "Verified candidate"
                );

                snapshotReady =
                    downloadSnapshotCandidate();

                if (snapshotReady)
                {
                    showSyncPhase(
                        4,
                        5,
                        "Installing",
                        "Recoverable swap"
                    );

                    installOk =
                        installVerifiedCandidate();
                }
            }
        }
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    if (alreadyCurrent)
    {
        showNetworkStatus(
            "Records current",
            "No download needed"
        );

        lastUpdateState =
            "current";

        showSyncPhase(
            5,
            5,
            "Complete",
            "Already current"
        );

        Serial.println(
            "FLORA ON-DEMAND RECORD UPDATE: CURRENT"
        );
        return true;
    }

    if (installOk)
    {
        showNetworkStatus(
            "Records updated",
            "Verified private copy"
        );

        lastUpdateState =
            "updated";

        showSyncPhase(
            5,
            5,
            "Complete",
            "Records updated"
        );

        Serial.println(
            "FLORA ON-DEMAND RECORD UPDATE: PASS"
        );
        return true;
    }

    showNetworkStatus(
        "Update failed",
        "Existing snapshot kept"
    );

    lastUpdateState =
        "failed";

    Serial.println(
        "FLORA ON-DEMAND RECORD UPDATE: FAILED"
    );

    return false;
}

enum class ToolStatus
{
    Ok,
    NotFound,
    InvalidArgument,
    MalformedSnapshot
};

struct CountResult
{
    size_t total = 0;
    size_t funko = 0;
    size_t thrilljoy = 0;
    size_t pins = 0;
};

enum class ToolKind
{
    FindItem,
    CountItems,
    ListItems,
    UpdateRecords,
    Unknown
};

struct ToolRequest
{
    ToolKind kind = ToolKind::Unknown;
    String argument;
    size_t limit = 5;

    // RHEA1-F4J deterministic find filters. These refine a title lookup
    // without changing the high-level find_item tool contract.
    String findCollectibleType;
    String findProductTypePrefix;

    // RHEA1-F4C deterministic count filters.
    String countCollectibleType;
    String countProductTypePrefix;

    // Broad franchise scope can match either seriesName or affiliation.
    String countSeriesNamePrefix;
    String countAffiliationPrefix;

    // Enamel Pin subject scope (for example: Good Girl Art).
    String countSubjectPrefix;
};

String countScopeLabel(
    const ToolRequest &request);

void clearRecentRankContext();

// FLORA1-CONVO3: successful scoped count queries establish conversation
// context just like value/rank queries. This allows follow-ups such as
// "what are the top 4 most valuable ones?" to reuse Batman/Good Girl Art.
bool rememberConversationFromCountRequest(
    const ToolRequest &request)
{
    String scope;
    ValueGroupDomain domain =
        ValueGroupDomain::Pops;

    if (request.countSubjectPrefix.length() > 0)
    {
        scope =
            request.countSubjectPrefix;
        domain =
            ValueGroupDomain::Pins;
    }
    else if (request.countAffiliationPrefix.length() > 0)
    {
        scope =
            request.countAffiliationPrefix;

        domain =
            request.countCollectibleType ==
                "Enamel Pins"
                ? ValueGroupDomain::Pins
                : ValueGroupDomain::Pops;
    }
    else if (request.countSeriesNamePrefix.length() > 0)
    {
        scope =
            request.countSeriesNamePrefix;

        domain =
            request.countCollectibleType ==
                "Enamel Pins"
                ? ValueGroupDomain::Pins
                : ValueGroupDomain::Pops;
    }

    scope.trim();

    if (scope.length() < 2)
    {
        return false;
    }

    rememberConversationScope(
        scope,
        domain
    );

    clearRecentRankContext();

    Serial.print(
        "CONTEXT_SET source=count scope=["
    );
    Serial.print(scope);
    Serial.print("] domain=");
    Serial.println(
        domain == ValueGroupDomain::Pins
            ? "pins"
            : "pops"
    );

    return true;
}

bool recordMatchesFindFilter(
    const RecordInfo &record,
    const ToolRequest &request)
{
    if (request.findCollectibleType.length() > 0 &&
        lowerCopy(record.collectibleType) !=
            lowerCopy(request.findCollectibleType))
    {
        return false;
    }

    if (request.findProductTypePrefix.length() > 0)
    {
        const String productLower =
            lowerCopy(record.productType);

        const String prefixLower =
            lowerCopy(
                request.findProductTypePrefix
            );

        if (!productLower.startsWith(
                prefixLower))
        {
            return false;
        }
    }

    return true;
}

ToolStatus toolFindItem(
    const ToolRequest &request,
    RecordInfo &match)
{
    String normalized =
        request.argument;

    normalized.trim();

    if (normalized.length() < 2)
    {
        return ToolStatus::InvalidArgument;
    }

    // Deterministic precedence:
    // 1. exact ID (case-insensitive)
    // 2. title substring (case-insensitive)
    //
    // When natural language supplies a type qualifier such as "Pop", the
    // qualifier is applied to the matched record. This prevents a title-only
    // search from returning a different Batman product line.
    RecordInfo exact;

    if (findByExactId(
            normalized,
            exact))
    {
        if (recordMatchesFindFilter(
                exact,
                request))
        {
            match = exact;
            return ToolStatus::Ok;
        }

        return ToolStatus::NotFound;
    }

    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        return ToolStatus::MalformedSnapshot;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return ToolStatus::MalformedSnapshot;
    }

    const String needle =
        lowerCopy(normalized);

    while (true)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state ==
            RecordReadState::EndOfArray)
        {
            break;
        }

        if (state ==
            RecordReadState::Malformed)
        {
            file.close();
            return ToolStatus::MalformedSnapshot;
        }

        if (lowerCopy(record.title).indexOf(
                needle) < 0)
        {
            continue;
        }

        if (!recordMatchesFindFilter(
                record,
                request))
        {
            continue;
        }

        match = record;
        file.close();
        return ToolStatus::Ok;
    }

    file.close();
    return ToolStatus::NotFound;
}

// Compatibility overload for legacy deterministic callers and the explicit
// "find <query>" command. With no natural-language qualifier, behavior is
// intentionally identical to the pre-F4J title/ID lookup.
ToolStatus toolFindItem(
    const String &argument,
    RecordInfo &match)
{
    ToolRequest request;
    request.kind =
        ToolKind::FindItem;
    request.argument =
        argument;

    return toolFindItem(
        request,
        match
    );
}


ToolStatus toolFindCandidates(
    const ToolRequest &request,
    RecordInfo *results,
    size_t capacity,
    size_t &returned,
    size_t &totalMatches)
{
    returned = 0;
    totalMatches = 0;

    String normalized =
        request.argument;

    normalized.trim();

    if (normalized.length() < 2 ||
        results == nullptr ||
        capacity == 0)
    {
        return ToolStatus::InvalidArgument;
    }

    RecordInfo exact;

    if (findByExactId(
            normalized,
            exact))
    {
        if (!recordMatchesFindFilter(
                exact,
                request))
        {
            return ToolStatus::NotFound;
        }

        results[0] =
            exact;

        returned = 1;
        totalMatches = 1;

        return ToolStatus::Ok;
    }

    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        return ToolStatus::MalformedSnapshot;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return ToolStatus::MalformedSnapshot;
    }

    const String needle =
        lowerCopy(normalized);

    while (true)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state ==
            RecordReadState::EndOfArray)
        {
            break;
        }

        if (state ==
            RecordReadState::Malformed)
        {
            file.close();
            return ToolStatus::MalformedSnapshot;
        }

        if (lowerCopy(record.title).indexOf(
                needle) < 0)
        {
            continue;
        }

        if (!recordMatchesFindFilter(
                record,
                request))
        {
            continue;
        }

        ++totalMatches;

        if (returned < capacity)
        {
            results[returned] =
                record;

            ++returned;
        }
    }

    file.close();

    return totalMatches > 0
        ? ToolStatus::Ok
        : ToolStatus::NotFound;
}

void showFindAmbiguity(
    const ToolRequest &request,
    const RecordInfo *records,
    size_t returned,
    size_t totalMatches)
{
    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextColor(WHITE, BLACK);
    display.setTextSize(1);
    display.setTextWrap(true);

    display.println("F L O R A");
    display.println("--------------------");
    display.print("I found ");
    display.print(totalMatches);
    display.println(" matches.");
    display.println("Try a title/ID detail:");

    const size_t displayCount =
        min(
            returned,
            static_cast<size_t>(3)
        );

    for (size_t i = 0;
         i < displayCount;
         ++i)
    {
        display.print(i + 1);
        display.print(". ");
        display.print(records[i].title);

        if (records[i].productType.length() > 0)
        {
            display.print(" | ");
            display.print(records[i].productType);
        }

        if (records[i].seriesNumber > 0)
        {
            display.print(" #");
            display.print(records[i].seriesNumber);
        }

        display.println();
    }

    if (totalMatches > displayCount)
    {
        display.print("+ ");
        display.print(
            totalMatches -
            displayCount
        );
        display.println(" more");
    }

    display.println();
    display.println("[ENTER] Refine the search");

    Serial.print("FLORA_SAYS I found ");
    Serial.print(totalMatches);
    Serial.print(" matches for ");
    Serial.print(request.argument);
    Serial.println(". Please refine the search.");
}


bool isAllDigits(
    const String &value)
{
    if (value.length() == 0)
    {
        return false;
    }

    for (size_t i = 0;
         i < value.length();
         ++i)
    {
        if (!isDigit(
                value[i]))
        {
            return false;
        }
    }

    return true;
}

bool looksLikeUpc(
    String value)
{
    value.trim();

    return value.length() >= 8 &&
        value.length() <= 14 &&
        isAllDigits(value);
}

ToolStatus toolFindByUpc(
    const String &upc,
    RecordInfo *results,
    size_t capacity,
    size_t &returned,
    size_t &totalMatches)
{
    returned = 0;
    totalMatches = 0;

    String normalized =
        upc;
    normalized.trim();

    if (!looksLikeUpc(
            normalized) ||
        results == nullptr ||
        capacity == 0)
    {
        return ToolStatus::InvalidArgument;
    }

    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        return ToolStatus::MalformedSnapshot;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return ToolStatus::MalformedSnapshot;
    }

    while (true)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state ==
            RecordReadState::EndOfArray)
        {
            break;
        }

        if (state ==
            RecordReadState::Malformed)
        {
            file.close();
            return ToolStatus::MalformedSnapshot;
        }

        if (record.upc != normalized)
        {
            continue;
        }

        ++totalMatches;

        if (returned < capacity)
        {
            results[returned] =
                record;
            ++returned;
        }
    }

    file.close();

    return totalMatches > 0
        ? ToolStatus::Ok
        : ToolStatus::NotFound;
}

void showUpcMatches(
    const String &upc,
    const RecordInfo *records,
    size_t returned,
    size_t totalMatches)
{
    if (totalMatches == 1 &&
        returned == 1)
    {
        showRecord(
            records[0]
        );
        return;
    }

    ToolRequest request;
    request.kind =
        ToolKind::FindItem;
    request.argument =
        "UPC " +
        upc;

    showFindAmbiguity(
        request,
        records,
        returned,
        totalMatches
    );
}

bool valueScopeMatches(
    const RecordInfo &record,
    const String &scope)
{
    const String lowerScope =
        lowerCopy(scope);

    if (lowerScope.length() == 0 ||
        lowerScope == "collection" ||
        lowerScope == "item" ||
        lowerScope == "items")
    {
        return true;
    }

    if (lowerScope == "pop" ||
        lowerScope == "pops" ||
        lowerScope == "funko pop" ||
        lowerScope == "funko pops")
    {
        return lowerCopy(
                record.collectibleType) ==
                "funko" &&
            lowerCopy(
                record.productType)
                .startsWith("pop!");
    }

    if (lowerScope == "funko")
    {
        return lowerCopy(
            record.collectibleType) ==
            "funko";
    }

    if (lowerScope == "thrilljoy" ||
        lowerScope == "thrilljoys")
    {
        return lowerCopy(
            record.collectibleType) ==
            "thrilljoy";
    }

    if (lowerScope == "pin" ||
        lowerScope == "pins" ||
        lowerScope == "enamel pin" ||
        lowerScope == "enamel pins")
    {
        return lowerCopy(
            record.collectibleType) ==
            "enamel pins";
    }

    return
        lowerCopy(record.seriesName)
            .startsWith(lowerScope) ||
        lowerCopy(record.affiliation)
            .startsWith(lowerScope);
}

bool scanMostValuable(
    const String &scope,
    RecordInfo &best,
    size_t &valuedRecords)
{
    valuedRecords = 0;
    bool found = false;

    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        return false;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return false;
    }

    while (true)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state ==
            RecordReadState::EndOfArray)
        {
            break;
        }

        if (state ==
            RecordReadState::Malformed)
        {
            file.close();
            return false;
        }

        if (!record.hasCurrentValue ||
            !valueScopeMatches(
                record,
                scope))
        {
            continue;
        }

        ++valuedRecords;

        if (!found ||
            record.currentValueUsd >
                best.currentValueUsd)
        {
            best = record;
            found = true;
        }
    }

    file.close();
    return found;
}

bool scanCollectionValue(
    const String &scope,
    double &totalValue,
    size_t &valuedVariants,
    size_t &valuedCopies)
{
    totalValue = 0.0;
    valuedVariants = 0;
    valuedCopies = 0;

    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        return false;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return false;
    }

    while (true)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state ==
            RecordReadState::EndOfArray)
        {
            break;
        }

        if (state ==
            RecordReadState::Malformed)
        {
            file.close();
            return false;
        }

        if (!record.hasCurrentValue ||
            record.inHandCopyCount <= 0 ||
            !valueScopeMatches(
                record,
                scope))
        {
            continue;
        }

        ++valuedVariants;
        valuedCopies +=
            static_cast<size_t>(
                record.inHandCopyCount
            );

        totalValue +=
            record.currentValueUsd *
            static_cast<double>(
                record.inHandCopyCount
            );
    }

    file.close();
    return true;
}

String extractWorthQuery(
    const String &input)
{
    String value =
        input;
    value.trim();

    String lower =
        lowerCopy(value);

    static const char *const prefixes[] =
    {
        "what is my ",
        "what's my ",
        "what is ",
        "what's ",
        "how much is my ",
        "how much is ",
        "value "
    };

    for (const char *prefix : prefixes)
    {
        if (lower.startsWith(
                prefix))
        {
            value =
                value.substring(
                    strlen(prefix)
                );
            break;
        }
    }

    lower =
        lowerCopy(value);

    static const char *const suffixes[] =
    {
        " worth?",
        " worth",
        " valued at?",
        " valued at"
    };

    for (const char *suffix : suffixes)
    {
        if (lower.endsWith(
                suffix))
        {
            value.remove(
                value.length() -
                strlen(suffix)
            );
            break;
        }
    }

    value.trim();
    return value;
}

bool looksLikeWorthQuestion(
    const String &input)
{
    const String lower =
        lowerCopy(input);

    return lower.startsWith(
               "value ") ||
        lower.indexOf(
            " worth") >= 0 ||
        lower.indexOf(
            "valued at") >= 0;
}

void showValueForTitle(
    const String &query)
{
    ToolRequest request;
    request.kind =
        ToolKind::FindItem;
    request.argument =
        query;

    static RecordInfo matches[5];
    size_t returned = 0;
    size_t total = 0;

    const ToolStatus status =
        toolFindCandidates(
            request,
            matches,
            5,
            returned,
            total
        );

    if (status != ToolStatus::Ok ||
        returned == 0)
    {
        showError(
            "No matching item."
        );
        Serial.println(
            "VALUE_RESPONSE {\"status\":\"not_found\"}"
        );
        return;
    }

    if (total > 1)
    {
        showFindAmbiguity(
            request,
            matches,
            returned,
            total
        );
        return;
    }

    showRecord(
        matches[0]
    );

    Serial.print(
        "VALUE_RESPONSE {\"status\":\"ok\",\"id\":\""
    );
    Serial.print(matches[0].id);
    Serial.print(
        "\",\"hasValue\":"
    );
    Serial.print(
        matches[0].hasCurrentValue
            ? "true"
            : "false"
    );

    if (matches[0].hasCurrentValue)
    {
        Serial.print(
            ",\"currentValueUsd\":"
        );
        Serial.print(
            matches[0].currentValueUsd,
            2
        );
    }

    Serial.println("}");
}


String normalizedCommandText(
    const String &input)
{
    String value =
        input;

    value.trim();

    // PlatformIO/terminal input can occasionally leave a leading backslash
    // or the printable tail of an ANSI arrow-key sequence (e.g. "[C").
    // Strip only these bounded leading artifacts; do not normalize the
    // remainder of IDs/search text.
    bool changed = true;

    while (changed &&
           value.length() > 0)
    {
        changed = false;

        if (value.startsWith("\\") &&
            value.length() > 1 &&
            isAlpha(
                value[1]))
        {
            value.remove(0, 1);
            value.trim();
            changed = true;
            continue;
        }

        if (value.length() > 2 &&
            value[0] == '[' &&
            (value[1] == 'A' ||
             value[1] == 'B' ||
             value[1] == 'C' ||
             value[1] == 'D') &&
            isAlpha(
                value[2]))
        {
            value.remove(0, 2);
            value.trim();
            changed = true;
            continue;
        }
    }

    return value;
}

String normalizedCommandLower(
    const String &input)
{
    String value =
        lowerCopy(
            normalizedCommandText(
                input
            )
        );

    value.trim();

    while (value.length() > 0)
    {
        const char tail =
            value[
                value.length() - 1
            ];

        if (tail != '?' &&
            tail != '!' &&
            tail != '.')
        {
            break;
        }

        value.remove(
            value.length() - 1
        );
        value.trim();
    }

    return value;
}

bool recordMatchesValueGroup(
    const RecordInfo &record,
    const String &query,
    ValueGroupDomain domain)
{
    if (record.inHandCopyCount <= 0)
    {
        return false;
    }

    String needle =
        lowerCopy(query);
    needle.trim();

    if (needle.length() < 2)
    {
        return false;
    }

    if (domain == ValueGroupDomain::Pins)
    {
        if (lowerCopy(
                record.collectibleType) !=
                "enamel pins")
        {
            return false;
        }

        return
            lowerCopy(record.title)
                .indexOf(needle) >= 0 ||
            lowerCopy(record.subject)
                .indexOf(needle) >= 0 ||
            lowerCopy(record.distributor)
                .indexOf(needle) >= 0 ||
            lowerCopy(record.id)
                .indexOf(needle) >= 0;
    }

    if (lowerCopy(
            record.collectibleType) !=
            "funko")
    {
        return false;
    }

    if (!lowerCopy(
            record.productType)
            .startsWith("pop!"))
    {
        return false;
    }

    return
        lowerCopy(record.title)
            .indexOf(needle) >= 0 ||
        lowerCopy(record.seriesName)
            .indexOf(needle) >= 0 ||
        lowerCopy(record.affiliation)
            .indexOf(needle) >= 0 ||
        lowerCopy(record.id)
            .indexOf(needle) >= 0;
}

struct ValueGroupSummary
{
    size_t matchedVariants = 0;
    size_t matchedCopies = 0;
    size_t valuedVariants = 0;
    size_t valuedCopies = 0;
    double knownValueUsd = 0.0;
};

bool scanValueGroup(
    const String &query,
    ValueGroupDomain domain,
    ValueGroupSummary &summary)
{
    summary =
        ValueGroupSummary();

    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        return false;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return false;
    }

    while (true)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state ==
            RecordReadState::EndOfArray)
        {
            break;
        }

        if (state ==
            RecordReadState::Malformed)
        {
            file.close();
            return false;
        }

        if (!recordMatchesValueGroup(
                record,
                query,
                domain))
        {
            continue;
        }

        ++summary.matchedVariants;

        summary.matchedCopies +=
            static_cast<size_t>(
                record.inHandCopyCount
            );

        if (!record.hasCurrentValue)
        {
            continue;
        }

        ++summary.valuedVariants;

        summary.valuedCopies +=
            static_cast<size_t>(
                record.inHandCopyCount
            );

        summary.knownValueUsd +=
            record.currentValueUsd *
            static_cast<double>(
                record.inHandCopyCount
            );
    }

    file.close();
    return true;
}

void clearValueConversation()
{
    pendingValueGroupQuery = "";
    pendingValueGroupDomain = ValueGroupDomain::Pops;
    pendingValueItemizationOffer =
        false;
    valueItemizationActive =
        false;
    valueItemizationOffset = 0;
}

void showValueGroupSummary(
    const String &query,
    ValueGroupDomain domain)
{
    ValueGroupSummary summary;

    if (!scanValueGroup(
            query,
            domain,
            summary))
    {
        clearValueConversation();
        showError(
            "Value scan failed."
        );
        return;
    }

    if (summary.matchedVariants == 0)
    {
        clearValueConversation();
        showError(
            domain == ValueGroupDomain::Pins
                ? "I couldn't find matching Pins."
                : "I couldn't find matching Pops."
        );

        Serial.print(
            "VALUE_GROUP_RESPONSE {\"status\":\"not_found\",\"query\":\""
        );
        Serial.print(query);
        Serial.println("\"}");
        return;
    }

    clearRecentRankContext();

    rememberConversationScope(
        query,
        domain
    );

    const String displayQuery =
        friendlyScopeLabel(
            query
        );

    pendingValueGroupQuery =
        query;
    pendingValueGroupDomain =
        domain;
    pendingValueItemizationOffer =
        true;
    valueItemizationActive =
        false;
    valueItemizationOffset = 0;

    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextSize(1);
    display.setTextWrap(true);

    display.setTextColor(GREEN, BLACK);
    display.println("F L O R A");
    display.println("--------------------");
    display.setTextColor(WHITE, BLACK);
    display.print(displayQuery);
    display.println(
        domain == ValueGroupDomain::Pins
            ? " pins"
            : " Pops"
    );
    display.print("Known value: $");
    display.println(
        summary.knownValueUsd,
        2
    );
    display.print("Copies: ");
    display.println(
        summary.matchedCopies
    );
    display.print("Variants: ");
    display.println(
        summary.matchedVariants
    );

    if (summary.valuedVariants <
        summary.matchedVariants)
    {
        display.print("Unvalued: ");
        display.println(
            summary.matchedVariants -
            summary.valuedVariants
        );
    }

    display.println();
    display.println(
        "Itemize them? yes/no"
    );

    Serial.print(
        "VALUE_GROUP_RESPONSE {\"status\":\"ok\",\"query\":\""
    );
    Serial.print(query);
    Serial.print(
        "\",\"knownValueUsd\":"
    );
    Serial.print(
        summary.knownValueUsd,
        2
    );
    Serial.print(
        ",\"matchedVariants\":"
    );
    Serial.print(
        summary.matchedVariants
    );
    Serial.print(
        ",\"matchedCopies\":"
    );
    Serial.print(
        summary.matchedCopies
    );
    Serial.print(
        ",\"valuedVariants\":"
    );
    Serial.print(
        summary.valuedVariants
    );
    Serial.print(
        ",\"valuedCopies\":"
    );
    Serial.print(
        summary.valuedCopies
    );
    Serial.println("}");

    Serial.print(
        "FLORA_SAYS "
    );

    if (summary.valuedVariants ==
        summary.matchedVariants)
    {
        Serial.print("Your ");
        Serial.print(
            summary.matchedCopies
        );
        Serial.print(" ");
        Serial.print(displayQuery);
        Serial.print(
            domain == ValueGroupDomain::Pins
                ? " pin"
                : " Pop"
        );

        if (summary.matchedCopies != 1)
        {
            Serial.print("s");
        }

        Serial.print(
            " have a known value of $"
        );
        Serial.print(
            summary.knownValueUsd,
            2
        );
        Serial.print(
            ". All "
        );
        Serial.print(
            summary.matchedVariants
        );
        Serial.print(
            " variant"
        );

        if (summary.matchedVariants != 1)
        {
            Serial.print("s");
        }

        Serial.print(
            " currently have values."
        );
    }
    else
    {
        Serial.print(
            "I found values for "
        );
        Serial.print(
            summary.valuedVariants
        );
        Serial.print(" of your ");
        Serial.print(
            summary.matchedVariants
        );
        Serial.print(" ");
        Serial.print(displayQuery);
        Serial.print(
            domain == ValueGroupDomain::Pins
                ? " pin variant"
                : " Pop variant"
        );

        if (summary.matchedVariants != 1)
        {
            Serial.print("s");
        }

        Serial.print(
            ". Those known values total $"
        );
        Serial.print(
            summary.knownValueUsd,
            2
        );
        Serial.print(". ");
        Serial.print(
            summary.matchedVariants -
            summary.valuedVariants
        );
        Serial.print(
            " don't have a value yet."
        );
    }

    Serial.println(
        " Want me to itemize them?"
    );
}

bool showValueItemizationPage(
    const String &query,
    size_t offset,
    ValueGroupDomain domain)
{
    static RecordInfo page[
        VALUE_ITEMIZE_PAGE_SIZE
    ];

    size_t pageCount = 0;
    size_t totalMatches = 0;

    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        return false;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return false;
    }

    while (true)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state ==
            RecordReadState::EndOfArray)
        {
            break;
        }

        if (state ==
            RecordReadState::Malformed)
        {
            file.close();
            return false;
        }

        if (!recordMatchesValueGroup(
                record,
                query,
                domain))
        {
            continue;
        }

        if (totalMatches >= offset &&
            pageCount <
                VALUE_ITEMIZE_PAGE_SIZE)
        {
            page[pageCount] =
                record;
            ++pageCount;
        }

        ++totalMatches;
    }

    file.close();

    if (pageCount == 0)
    {
        clearValueConversation();
        return false;
    }

    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextColor(
        WHITE,
        BLACK
    );
    display.setTextSize(1);
    display.setTextWrap(true);

    display.println("F L O R A");
    display.print(
        friendlyScopeLabel(query)
    );
    display.println(
        domain == ValueGroupDomain::Pins
            ? " pins"
            : " Pops"
    );
    display.println("--------------------");

    for (size_t i = 0;
         i < pageCount;
         ++i)
    {
        const RecordInfo &record =
            page[i];

        display.print(
            offset + i + 1
        );
        display.print(". ");
        display.print(
            record.title
        );

        if (record.seriesNumber > 0)
        {
            display.print(" #");
            display.print(
                record.seriesNumber
            );
        }

        display.print(" ");

        if (record.hasCurrentValue)
        {
            display.print("$");
            display.print(
                record.currentValueUsd,
                2
            );

            if (record.inHandCopyCount > 1)
            {
                display.print(" x");
                display.print(
                    record.inHandCopyCount
                );
            }
        }
        else
        {
            display.print("no value");
        }

        display.println();

        Serial.print(
            "VALUE_ITEM "
        );
        Serial.print(
            offset + i + 1
        );
        Serial.print(" | ");
        Serial.print(
            record.title
        );
        Serial.print(" | ");
        Serial.print(
            record.id
        );
        Serial.print(
            " | copies="
        );
        Serial.print(
            record.inHandCopyCount
        );
        Serial.print(
            " | value="
        );

        if (record.hasCurrentValue)
        {
            Serial.print("$");
            Serial.print(
                record.currentValueUsd,
                2
            );
        }
        else
        {
            Serial.print("unvalued");
        }

        if (record.hasCurrentValue &&
            record.inHandCopyCount > 1)
        {
            Serial.print(
                " | subtotal=$"
            );
            Serial.print(
                record.currentValueUsd *
                static_cast<double>(
                    record.inHandCopyCount
                ),
                2
            );
        }

        Serial.println();
    }

    const size_t nextOffset =
        offset + pageCount;

    if (nextOffset < totalMatches)
    {
        valueItemizationActive =
            true;
        pendingValueItemizationOffer =
            false;
        valueItemizationOffset =
            nextOffset;

        display.println();
        display.print(nextOffset);
        display.print("/");
        display.print(totalMatches);
        display.println(" shown");
        display.println("Type more for next");

        Serial.print(
            "FLORA_SAYS Showing "
        );
        Serial.print(offset + 1);
        Serial.print("-");
        Serial.print(nextOffset);
        Serial.print(" of ");
        Serial.print(totalMatches);
        Serial.println(
            ". Type more for the next page."
        );
    }
    else
    {
        Serial.print(
            "FLORA_SAYS That's all "
        );
        Serial.print(totalMatches);
        Serial.println(
            domain == ValueGroupDomain::Pins
                ? " matching pins."
                : " matching Pops."
        );

        clearValueConversation();

        display.println();
        display.println("That's all.");
        display.println(
            "[ENTER] New command"
        );
    }

    return true;
}

ValueGroupDomain valueGroupDomainForInput(
    const String &input)
{
    const String lower =
        normalizedCommandLower(
            input);

    if (lower.indexOf(" enamel pin") >= 0 ||
        lower.indexOf(" pins") >= 0 ||
        lower.endsWith(" pin") ||
        lower.startsWith("pin ") ||
        lower.startsWith("pins "))
    {
        return ValueGroupDomain::Pins;
    }

    return ValueGroupDomain::Pops;
}

String extractValueGroupQuery(
    const String &input)
{
    String value =
        normalizedCommandLower(
            input);

    static const char *const prefixes[] =
    {
        "what is the value of all my ",
        "what's the value of all my ",
        "what is the value of my ",
        "what's the value of my ",
        "how much are all my ",
        "how much are my ",
        "what are all my ",
        "what are my ",
        "value of all my ",
        "value of my ",
        "value "
    };

    for (const char *prefix : prefixes)
    {
        if (value.startsWith(
                prefix))
        {
            value =
                value.substring(
                    strlen(prefix)
                );
            break;
        }
    }

    static const char *const valueSuffixes[] =
    {
        " valued at",
        " worth",
        " value"
    };

    for (const char *suffix : valueSuffixes)
    {
        if (value.endsWith(
                suffix))
        {
            value.remove(
                value.length() -
                strlen(suffix)
            );
            value.trim();
            break;
        }
    }

    static const char *const collectionSuffixes[] =
    {
        " enamel pins",
        " enamel pin",
        " funko pops",
        " funko pop",
        " pins",
        " pin",
        " pops",
        " pop"
    };

    for (const char *suffix : collectionSuffixes)
    {
        if (value.endsWith(
                suffix))
        {
            value.remove(
                value.length() -
                strlen(suffix)
            );
            break;
        }
    }

    value.trim();
    return value;
}

bool looksLikeValueGroupQuestion(
    const String &input)
{
    const String lower =
        normalizedCommandLower(
            input);

    const bool explicitCollectionWorth =
        (lower.startsWith("what are my ") ||
         lower.startsWith("what are all my ") ||
         lower.startsWith("how much are my ") ||
         lower.startsWith("how much are all my ")) &&
        (lower.endsWith(" pops worth") ||
         lower.endsWith(" funko pops worth") ||
         lower.endsWith(" pop worth") ||
         lower.endsWith(" funko pop worth") ||
         lower.endsWith(" pins worth") ||
         lower.endsWith(" enamel pins worth") ||
         lower.endsWith(" pin worth") ||
         lower.endsWith(" enamel pin worth"));

    // Compact collection shorthand, e.g. "Batman value".
    // A bare trailing "value" is treated as an aggregate scope query.
    const bool trailingValue =
        lower.length() > 6 &&
        lower.endsWith(" value");

    return
        lower.startsWith("value ") ||
        lower.startsWith(
            "value of my ") ||
        lower.startsWith(
            "value of all my ") ||
        lower.startsWith(
            "what is the value of my ") ||
        lower.startsWith(
            "what's the value of my ") ||
        lower.startsWith(
            "what is the value of all my ") ||
        lower.startsWith(
            "what's the value of all my ") ||
        explicitCollectionWorth ||
        trailingValue;
}


String friendlyScopeLabel(
    const String &scope)
{
    String result =
        lowerCopy(scope);
    result.trim();

    bool capitalizeNext = true;

    for (size_t i = 0;
         i < result.length();
         ++i)
    {
        const char c = result[i];

        if (isAlpha(c))
        {
            if (capitalizeNext)
            {
                result.setCharAt(
                    i,
                    static_cast<char>(
                        toupper(c)
                    )
                );
                capitalizeNext = false;
            }
        }
        else
        {
            capitalizeNext =
                c == ' ' ||
                c == '-' ||
                c == '/' ||
                c == '&';
        }
    }

    return result;
}

void rememberConversationScope(
    const String &scope,
    ValueGroupDomain domain)
{
    if (scope.length() < 2)
    {
        return;
    }

    conversationContext.valid = true;
    conversationContext.scope =
        lowerCopy(scope);
    conversationContext.scope.trim();
    conversationContext.domain =
        domain;
}

bool isWordBoundaryChar(
    char c)
{
    return
        !isAlpha(c) &&
        !isDigit(c);
}

String replaceAliasBounded(
    const String &input,
    const String &alias,
    const String &replacement)
{
    if (alias.length() == 0)
    {
        return input;
    }

    String source = input;
    String sourceLower =
        lowerCopy(source);
    const String aliasLower =
        lowerCopy(alias);

    String output;
    size_t cursor = 0;

    while (cursor <
           source.length())
    {
        const int found =
            sourceLower.indexOf(
                aliasLower,
                cursor
            );

        if (found < 0)
        {
            output +=
                source.substring(
                    cursor
                );
            break;
        }

        const size_t start =
            static_cast<size_t>(
                found
            );
        const size_t end =
            start +
            aliasLower.length();

        const bool leftOk =
            start == 0 ||
            isWordBoundaryChar(
                sourceLower[
                    start - 1
                ]
            );

        const bool rightOk =
            end >=
                sourceLower.length() ||
            isWordBoundaryChar(
                sourceLower[
                    end
                ]
            );

        output +=
            source.substring(
                cursor,
                start
            );

        if (leftOk &&
            rightOk)
        {
            output += replacement;
            cursor = end;
        }
        else
        {
            output +=
                source.substring(
                    start,
                    start + 1
                );
            cursor =
                start + 1;
        }
    }

    return output;
}

String applyLearnedAliases(
    const String &input)
{
    // The alias file is optional. Avoid asking the SD VFS to open a
    // nonexistent file because the ESP32 SD layer logs that as an error.
    if (!SD.exists(FLORA_ALIAS_PATH))
    {
        return input;
    }

    File file =
        SD.open(
            FLORA_ALIAS_PATH,
            FILE_READ
        );

    if (!file)
    {
        return input;
    }

    String result =
        input;

    while (file.available())
    {
        String line =
            file.readStringUntil(
                '\n'
            );
        line.trim();

        if (line.length() == 0)
        {
            continue;
        }

        const int separator =
            line.indexOf('\t');

        if (separator <= 0 ||
            separator >=
                static_cast<int>(
                    line.length() - 1
                ))
        {
            continue;
        }

        String alias =
            line.substring(
                0,
                separator
            );
        String replacement =
            line.substring(
                separator + 1
            );

        alias.trim();
        replacement.trim();

        result =
            replaceAliasBounded(
                result,
                alias,
                replacement
            );
    }

    file.close();
    return result;
}

bool saveLearnedAlias(
    const String &aliasInput,
    const String &replacementInput)
{
    String alias =
        lowerCopy(aliasInput);
    String replacement =
        lowerCopy(replacementInput);

    alias.trim();
    replacement.trim();

    if (alias.length() < 2 ||
        replacement.length() < 2 ||
        alias.indexOf('\t') >= 0 ||
        replacement.indexOf('\t') >= 0 ||
        alias.indexOf('\n') >= 0 ||
        replacement.indexOf('\n') >= 0)
    {
        return false;
    }

    // The snapshot normally lives under /RHEA, but do not assume the
    // directory exists on every SD card. Create it before the first teaching.
    if (!SD.exists("/RHEA"))
    {
        if (!SD.mkdir("/RHEA"))
        {
            return false;
        }
    }

    // The alias file itself is optional until the first teaching. Guard the
    // read so the ESP32 SD VFS does not log a missing-file error.
    if (SD.exists(FLORA_ALIAS_PATH))
    {
        File existing =
            SD.open(
                FLORA_ALIAS_PATH,
                FILE_READ
            );

        if (existing)
        {
            while (existing.available())
            {
                String line =
                    existing.readStringUntil(
                        '\n'
                    );
                line.trim();

                const int separator =
                    line.indexOf('\t');

                if (separator <= 0)
                {
                    continue;
                }

                String existingAlias =
                    line.substring(
                        0,
                        separator
                    );
                existingAlias.trim();

                if (lowerCopy(
                        existingAlias) ==
                    alias)
                {
                    existing.close();
                    return true;
                }
            }

            existing.close();
        }
    }

    File file =
        SD.open(
            FLORA_ALIAS_PATH,
            FILE_APPEND
        );

    if (!file)
    {
        return false;
    }

    file.print(alias);
    file.print('\t');
    file.println(replacement);
    file.close();

    return true;
}

bool parseTeachAlias(
    const String &input,
    String &alias,
    String &replacement)
{
    String lower =
        normalizedCommandLower(
            input
        );

    const String prefix =
        "when i say ";

    if (!lower.startsWith(
            prefix))
    {
        return false;
    }

    int separator =
        lower.indexOf(
            ", i mean "
        );
    size_t separatorLength = 9;

    if (separator < 0)
    {
        separator =
            lower.indexOf(
                " i mean "
            );
        separatorLength = 8;
    }

    if (separator < 0)
    {
        return false;
    }

    alias =
        lower.substring(
            prefix.length(),
            separator
        );

    replacement =
        lower.substring(
            separator +
            separatorLength
        );

    alias.trim();
    replacement.trim();

    return
        alias.length() >= 2 &&
        replacement.length() >= 2;
}

size_t extractRequestedLimit(
    const String &lower,
    size_t fallback = 4)
{
    for (size_t i = 0;
         i < lower.length();
         ++i)
    {
        if (!isDigit(
                lower[i]))
        {
            continue;
        }

        size_t j = i;

        while (j <
                   lower.length() &&
               isDigit(
                   lower[j]))
        {
            ++j;
        }

        const int value =
            lower.substring(
                i,
                j
            ).toInt();

        if (value > 0)
        {
            return min(
                static_cast<size_t>(
                    value
                ),
                FLORA_MAX_RANK_RESULTS
            );
        }
    }

    struct NumberWord
    {
        const char *word;
        size_t value;
    };

    static const NumberWord words[] =
    {
        { " one ", 1 },
        { " two ", 2 },
        { " three ", 3 },
        { " four ", 4 },
        { " five ", 5 },
        { " six ", 6 },
        { " seven ", 7 },
        { " eight ", 8 },
        { " nine ", 9 },
        { " ten ", 10 }
    };

    const String padded =
        " " + lower + " ";

    for (const auto &entry : words)
    {
        if (padded.indexOf(
                entry.word) >= 0)
        {
            return entry.value;
        }
    }

    return fallback;
}

bool looksLikeRankValueQuestion(
    const String &input)
{
    const String lower =
        normalizedCommandLower(
            input
        );

    return
        lower.indexOf(
            "most valuable") >= 0 ||
        lower.indexOf(
            "more valuable") >= 0 ||
        lower.indexOf(
            "least valuable") >= 0 ||
        lower.indexOf(
            "less valuable") >= 0 ||
        lower.indexOf(
            "highest value") >= 0 ||
        lower.indexOf(
            "lowest value") >= 0 ||
        lower.indexOf(
            "ranked by value") >= 0 ||
        lower.indexOf(
            "rank by value") >= 0 ||
        (lower.indexOf("top ") >= 0 &&
         (lower.indexOf("value") >= 0 ||
          lower.indexOf("valuable") >= 0));
}

ValueGroupDomain conversationalDomainForInput(
    const String &input,
    bool &explicitDomain)
{
    const String lower =
        normalizedCommandLower(
            input
        );

    if (lower.indexOf("pin") >= 0)
    {
        explicitDomain = true;
        return ValueGroupDomain::Pins;
    }

    if (lower.indexOf("pop") >= 0 ||
        lower.indexOf("funko") >= 0)
    {
        explicitDomain = true;
        return ValueGroupDomain::Pops;
    }

    explicitDomain = false;

    if (conversationContext.valid)
    {
        return conversationContext.domain;
    }

    return ValueGroupDomain::Pops;
}

String extractRankScope(
    const String &input)
{
    String value =
        normalizedCommandLower(
            input
        );

    // Prefer semantic ranking operators wherever they appear in the sentence.
    // This makes the parser tolerant of conversational lead-ins and harmless
    // typos such as "show mew" because the words before the operator are not
    // treated as part of the collection scope.
    static const char *const operators[] =
    {
        "most valuable",
        "more valuable",
        "least valuable",
        "less valuable",
        "highest value",
        "lowest value",
        "ranked by value"
    };

    bool usedOperator = false;

    for (const char *op : operators)
    {
        const int found =
            value.indexOf(op);

        if (found >= 0)
        {
            value =
                value.substring(
                    found +
                    strlen(op)
                );
            value.trim();
            usedOperator = true;
            break;
        }
    }

    // For forms such as "top 4 Batman Pops by value", there is no ranking
    // adjective before the scope. Strip everything through "top N" and keep
    // the remaining collection phrase.
    if (!usedOperator)
    {
        const int topIndex =
            value.indexOf("top ");

        if (topIndex >= 0)
        {
            size_t cursor =
                static_cast<size_t>(
                    topIndex + 4
                );

            while (cursor < value.length() &&
                   value[cursor] == ' ')
            {
                ++cursor;
            }

            while (cursor < value.length() &&
                   (isDigit(value[cursor]) ||
                    isAlpha(value[cursor])))
            {
                ++cursor;
            }

            while (cursor < value.length() &&
                   value[cursor] == ' ')
            {
                ++cursor;
            }

            value =
                value.substring(cursor);
            value.trim();
        }
    }

    if (value.endsWith(" by value"))
    {
        value.remove(
            value.length() -
            strlen(" by value")
        );
        value.trim();
    }

    // If no semantic operator was found, trim ordinary conversational lead-ins
    // that may remain after the top-N extraction.
    if (!usedOperator)
    {
        static const char *const prefixes[] =
        {
            "can you show me ",
            "could you show me ",
            "show me ",
            "give me ",
            "list ",
            "which are ",
            "what are "
        };

        for (const char *prefix : prefixes)
        {
            if (value.startsWith(prefix))
            {
                value =
                    value.substring(
                        strlen(prefix)
                    );
                value.trim();
                break;
            }
        }
    }

    if (value.startsWith("the "))
    {
        value = value.substring(4);
        value.trim();
    }

    if (value.startsWith("my "))
    {
        value = value.substring(3);
        value.trim();
    }

    static const char *const suffixes[] =
    {
        " enamel pins",
        " enamel pin",
        " funko pops",
        " funko pop",
        " pins",
        " pin",
        " pops",
        " pop"
    };

    for (const char *suffix : suffixes)
    {
        if (value.endsWith(suffix))
        {
            value.remove(
                value.length() -
                strlen(suffix)
            );
            value.trim();
            break;
        }
    }

    // Generic ranking phrases carry no new collection scope. Returning an
    // empty string tells the caller to reuse ConversationContext.
    static const char *const generic[] =
    {
        "",
        "ranked",
        "value",
        "valuable",
        "items",
        "item",
        "collection",
        "my collection",
        "the collection",

        // Referential words do not introduce a new collection scope.
        // They resolve against ConversationContext.
        "one",
        "ones",
        "the one",
        "the ones",
        "those",
        "those ones",
        "these",
        "these ones",
        "them",
        "they",
        "it",
        "that",
        "this"
    };

    for (const char *entry : generic)
    {
        if (value == entry)
        {
            return "";
        }
    }

    return value;
}

struct RankedValueItem
{
    RecordInfo record;
    double unitValue = 0.0;
};

// FLORA1-CONVO2: retain the actual records returned by the most recent
// value-ranking command. This gives pronouns such as "those" and "their"
// a concrete referent instead of falling back to the broader collection scope.
struct RecentRankContext
{
    bool valid = false;
    ValueGroupDomain domain = ValueGroupDomain::Pops;
    String scope;
    size_t count = 0;
    RecordInfo records[FLORA_MAX_RANK_RESULTS];
};

RecentRankContext recentRankContext;

void clearRecentRankContext()
{
    recentRankContext.valid = false;
    recentRankContext.scope = "";
    recentRankContext.count = 0;
}

bool scanRankedValueGroup(
    const String &query,
    ValueGroupDomain domain,
    size_t requestedLimit,
    bool descending,
    RankedValueItem *results,
    size_t &returned,
    size_t &matchedValued)
{
    returned = 0;
    matchedValued = 0;

    const size_t limit =
        min(
            requestedLimit,
            FLORA_MAX_RANK_RESULTS
        );

    if (limit == 0)
    {
        return false;
    }

    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        return false;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return false;
    }

    while (true)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state ==
            RecordReadState::EndOfArray)
        {
            break;
        }

        if (state ==
            RecordReadState::Malformed)
        {
            file.close();
            return false;
        }

        if (!recordMatchesValueGroup(
                record,
                query,
                domain) ||
            !record.hasCurrentValue)
        {
            continue;
        }

        ++matchedValued;

        RankedValueItem candidate;
        candidate.record = record;
        candidate.unitValue =
            record.currentValueUsd;

        size_t insertion = 0;

        while (insertion <
               returned)
        {
            const bool before =
                descending
                    ? candidate.unitValue >
                        results[insertion].unitValue
                    : candidate.unitValue <
                        results[insertion].unitValue;

            if (before)
            {
                break;
            }

            ++insertion;
        }

        if (returned < limit)
        {
            for (size_t move = returned;
                 move > insertion;
                 --move)
            {
                results[move] =
                    results[
                        move - 1
                    ];
            }

            results[insertion] =
                candidate;
            ++returned;
        }
        else if (insertion < limit)
        {
            for (size_t move =
                     limit - 1;
                 move > insertion;
                 --move)
            {
                results[move] =
                    results[
                        move - 1
                    ];
            }

            results[insertion] =
                candidate;
        }
    }

    file.close();
    return true;
}

void showRankedValueGroup(
    const String &query,
    ValueGroupDomain domain,
    size_t limit,
    bool descending,
    bool includeIds)
{
    static RankedValueItem results[
        FLORA_MAX_RANK_RESULTS
    ];

    size_t returned = 0;
    size_t matchedValued = 0;

    if (!scanRankedValueGroup(
            query,
            domain,
            limit,
            descending,
            results,
            returned,
            matchedValued))
    {
        showError(
            "I couldn't rank that collection."
        );
        return;
    }

    if (returned == 0)
    {
        showError(
            "I couldn't find valued matches."
        );
        return;
    }

    rememberConversationScope(
        query,
        domain
    );

    recentRankContext.valid = true;
    recentRankContext.domain = domain;
    recentRankContext.scope = lowerCopy(query);
    recentRankContext.scope.trim();
    recentRankContext.count = returned;

    for (size_t i = 0;
         i < returned;
         ++i)
    {
        recentRankContext.records[i] =
            results[i].record;
    }

    const String label =
        friendlyScopeLabel(
            query
        );

    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextSize(1);
    display.setTextWrap(true);
    display.setTextColor(GREEN, BLACK);
    display.println("F L O R A");
    display.println("--------------------");
    display.setTextColor(WHITE, BLACK);
    display.print(
        descending
            ? "Most valuable "
            : "Least valuable "
    );
    display.print(label);
    display.println(
        domain == ValueGroupDomain::Pins
            ? " pins"
            : " Pops"
    );

    for (size_t i = 0;
         i < returned;
         ++i)
    {
        const RecordInfo &record =
            results[i].record;

        display.print(i + 1);
        display.print(". ");
        display.print(record.title);

        if (includeIds)
        {
            display.println();
            display.print("   ");
            display.print(record.id);
            display.print(" | $");
            display.println(
                record.currentValueUsd,
                2
            );
        }
        else
        {
            if (record.seriesNumber > 0)
            {
                display.print(" #");
                display.print(
                    record.seriesNumber
                );
            }

            display.print(" $");
            display.println(
                record.currentValueUsd,
                2
            );
        }

        Serial.print(
            "VALUE_RANK_ITEM "
        );
        Serial.print(i + 1);
        Serial.print(" | ");
        Serial.print(record.title);
        Serial.print(" | ");
        Serial.print(record.id);
        Serial.print(" | $");
        Serial.println(
            record.currentValueUsd,
            2
        );
    }

    Serial.print(
        "VALUE_RANK_RESPONSE {\"status\":\"ok\",\"query\":\""
    );
    Serial.print(query);
    Serial.print(
        "\",\"direction\":\""
    );
    Serial.print(
        descending
            ? "desc"
            : "asc"
    );
    Serial.print(
        "\",\"returned\":"
    );
    Serial.print(returned);
    Serial.print(
        ",\"valuedMatches\":"
    );
    Serial.print(matchedValued);
    Serial.println("}");

    Serial.print("FLORA_SAYS Here ");
    Serial.print(
        returned == 1
            ? "is the "
            : "are the "
    );
    Serial.print(returned);
    Serial.print(
        descending
            ? " most valuable "
            : " least valuable "
    );
    Serial.print(label);
    Serial.print(
        domain == ValueGroupDomain::Pins
            ? " pin"
            : " Pop"
    );

    if (returned != 1)
    {
        Serial.print("s");
    }

    Serial.println(
        " I have values for:"
    );

    for (size_t i = 0;
         i < returned;
         ++i)
    {
        Serial.print("  ");
        Serial.print(i + 1);
        Serial.print(". ");
        Serial.print(
            results[i].record.title
        );

        if (includeIds)
        {
            Serial.print(" | ");
            Serial.print(
                results[i].record.id
            );
            Serial.print(" | $");
        }
        else
        {
            Serial.print(" - $");
        }

        Serial.println(
            results[i].record.currentValueUsd,
            2
        );
    }
}

bool refersToRecentRankSet(
    const String &input)
{
    const String lower =
        normalizedCommandLower(
            input
        );

    return
        lower.indexOf("those") >= 0 ||
        lower.indexOf("these") >= 0 ||
        lower.indexOf("them") >= 0 ||
        lower.indexOf("their") >= 0 ||
        lower.indexOf("the top") >= 0 ||
        lower.indexOf("top ") >= 0;
}

bool asksForIdFields(
    const String &input)
{
    const String lower =
        normalizedCommandLower(
            input
        );

    return
        lower.indexOf(" ids") >= 0 ||
        lower.endsWith("ids") ||
        lower.indexOf(" id ") >= 0 ||
        lower.endsWith(" id") ||
        lower.indexOf("unique id") >= 0 ||
        lower.indexOf("unique ids") >= 0;
}

bool isRecentRankIdFollowup(
    const String &input)
{
    if (!asksForIdFields(
            input))
    {
        return false;
    }

    const String lower =
        normalizedCommandLower(
            input
        );

    return
        refersToRecentRankSet(
            input
        ) ||
        lower == "show me the ids" ||
        lower == "what are the ids" ||
        lower == "show ids" ||
        lower == "ids";
}

void showRecentRankIds()
{
    if (!recentRankContext.valid ||
        recentRankContext.count == 0)
    {
        showError(
            "I don't have a recent ranked list to reference."
        );
        return;
    }

    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextSize(1);
    display.setTextWrap(true);
    display.setTextColor(GREEN, BLACK);
    display.println("F L O R A");
    display.println("--------------------");
    display.setTextColor(WHITE, BLACK);
    display.println("IDs from that list:");

    for (size_t i = 0;
         i < recentRankContext.count;
         ++i)
    {
        const RecordInfo &record =
            recentRankContext.records[i];

        display.print(i + 1);
        display.print(". ");
        display.println(record.title);
        display.print("   ");
        display.println(record.id);

        Serial.print(
            "RANK_ID_ITEM "
        );
        Serial.print(i + 1);
        Serial.print(" | ");
        Serial.print(record.title);
        Serial.print(" | ");
        Serial.println(record.id);
    }

    Serial.print(
        "FLORA_SAYS Here are the IDs for those "
    );
    Serial.print(
        recentRankContext.domain ==
            ValueGroupDomain::Pins
            ? "pins"
            : "Pops"
    );
    Serial.println(":");

    for (size_t i = 0;
         i < recentRankContext.count;
         ++i)
    {
        Serial.print("  ");
        Serial.print(i + 1);
        Serial.print(". ");
        Serial.print(
            recentRankContext.records[i].title
        );
        Serial.print(" - ");
        Serial.println(
            recentRankContext.records[i].id
        );
    }
}

void showRecentRankValueSummary()
{
    if (!recentRankContext.valid ||
        recentRankContext.count == 0)
    {
        showError(
            "I don't have a recent ranked list to reference."
        );
        return;
    }

    double total = 0.0;
    size_t copies = 0;

    for (size_t i = 0;
         i < recentRankContext.count;
         ++i)
    {
        const RecordInfo &record =
            recentRankContext.records[i];

        const size_t owned =
            record.inHandCopyCount > 0
                ? static_cast<size_t>(
                    record.inHandCopyCount
                )
                : 1;

        total +=
            record.currentValueUsd *
            static_cast<double>(owned);
        copies += owned;
    }

    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextSize(1);
    display.setTextWrap(true);
    display.setTextColor(GREEN, BLACK);
    display.println("F L O R A");
    display.println("--------------------");
    display.setTextColor(WHITE, BLACK);
    display.print("Those ");
    display.print(recentRankContext.count);
    display.print(
        recentRankContext.domain ==
            ValueGroupDomain::Pins
            ? " pins: $"
            : " Pops: $"
    );
    display.println(total, 2);

    Serial.print("FLORA_SAYS Those ");
    Serial.print(recentRankContext.count);
    Serial.print(
        recentRankContext.domain ==
            ValueGroupDomain::Pins
            ? " pins"
            : " Pops"
    );
    Serial.print(" have a known value of $");
    Serial.print(total, 2);
    Serial.print(" across ");
    Serial.print(copies);
    Serial.println(
        copies == 1
            ? " owned copy."
            : " owned copies."
    );
}

void showRecentRankCount()
{
    if (!recentRankContext.valid ||
        recentRankContext.count == 0)
    {
        showError(
            "I don't have a recent ranked list to reference."
        );
        return;
    }

    size_t copies = 0;

    for (size_t i = 0;
         i < recentRankContext.count;
         ++i)
    {
        const int owned =
            recentRankContext.records[i]
                .inHandCopyCount;

        copies +=
            static_cast<size_t>(
                owned > 0
                    ? owned
                    : 1
            );
    }

    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextSize(1);
    display.setTextWrap(true);
    display.setTextColor(GREEN, BLACK);
    display.println("F L O R A");
    display.println("--------------------");
    display.setTextColor(WHITE, BLACK);
    display.print("That ranked list has ");
    display.print(recentRankContext.count);
    display.println(
        recentRankContext.count == 1
            ? " variant."
            : " variants."
    );
    display.print("Owned copies: ");
    display.println(copies);

    Serial.print("FLORA_SAYS That ranked list contains ");
    Serial.print(recentRankContext.count);
    Serial.print(
        recentRankContext.count == 1
            ? " variant across "
            : " variants across "
    );
    Serial.print(copies);
    Serial.println(
        copies == 1
            ? " owned copy."
            : " owned copies."
    );
}

bool isContextValueItemizationFollowup(
    const String &input)
{
    const String lower =
        normalizedCommandLower(
            input
        );

    const bool asksValue =
        lower.indexOf("value") >= 0 ||
        lower.indexOf("worth") >= 0;

    const bool asksEach =
        lower.indexOf("each") >= 0 ||
        lower.indexOf("individual") >= 0 ||
        lower.indexOf("itemize") >= 0 ||
        lower.indexOf("itemise") >= 0;

    const bool refersBack =
        lower.indexOf("those") >= 0 ||
        lower.indexOf("these") >= 0 ||
        lower.indexOf("them") >= 0 ||
        lower.indexOf("they") >= 0;

    return asksValue && asksEach && refersBack;
}

void showRecentRankValues()
{
    if (!recentRankContext.valid ||
        recentRankContext.count == 0)
    {
        showError(
            "I don't have a recent ranked list to reference."
        );
        return;
    }

    auto &display =
        floraDisplay;

    display.setTextColor(
        WHITE,
        BLACK
    );

    for (size_t i = 0;
         i < recentRankContext.count;
         ++i)
    {
        const RecordInfo &record =
            recentRankContext.records[i];

        display.print(i + 1);
        display.print(". ");
        display.print(record.title);
        display.print(" | $");
        display.println(
            record.currentValueUsd,
            2
        );

        Serial.print("CONTEXT_VALUE_ITEM ");
        Serial.print(i + 1);
        Serial.print(" | ");
        Serial.print(record.title);
        Serial.print(" | ");
        Serial.print(record.id);
        Serial.print(" | $");
        Serial.println(
            record.currentValueUsd,
            2
        );
    }

    Serial.println(
        "FLORA_SAYS Here are the individual values from that ranked list."
    );
}

bool isContextValueFollowup(
    const String &input)
{
    const String lower =
        normalizedCommandLower(
            input
        );

    return
        lower == "what are they worth" ||
        lower == "what are those worth" ||
        lower == "what are these worth" ||
        lower == "what is that worth" ||
        lower == "what's that worth" ||
        lower == "what are they valued at" ||
        lower == "what are those valued at";
}

bool isContextCountFollowup(
    const String &input)
{
    const String lower =
        normalizedCommandLower(
            input
        );

    return
        lower == "how many of those do i have" ||
        lower == "how many of them do i have" ||
        lower == "how many of those" ||
        lower == "how many of them";
}

void showContextCount()
{
    if (!conversationContext.valid)
    {
        showError(
            "I don't have a collection scope to reuse yet."
        );
        return;
    }

    ValueGroupSummary summary;

    if (!scanValueGroup(
            conversationContext.scope,
            conversationContext.domain,
            summary))
    {
        showError(
            "I couldn't count that collection."
        );
        return;
    }

    const String label =
        friendlyScopeLabel(
            conversationContext.scope
        );

    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextSize(1);
    display.setTextWrap(true);
    display.setTextColor(GREEN, BLACK);
    display.println("F L O R A");
    display.println("--------------------");
    display.setTextColor(WHITE, BLACK);
    display.print(label);
    display.println(
        conversationContext.domain ==
            ValueGroupDomain::Pins
            ? " pins"
            : " Pops"
    );
    display.print("Copies: ");
    display.println(summary.matchedCopies);
    display.print("Variants: ");
    display.println(
        summary.matchedVariants
    );

    Serial.print("FLORA_SAYS You have ");
    Serial.print(summary.matchedCopies);
    Serial.print(" ");
    Serial.print(label);
    Serial.print(
        conversationContext.domain ==
            ValueGroupDomain::Pins
            ? " pin"
            : " Pop"
    );

    if (summary.matchedCopies != 1)
    {
        Serial.print("s");
    }

    Serial.print(" across ");
    Serial.print(
        summary.matchedVariants
    );
    Serial.println(" variants.");
}

bool recordMatchesCountFilter(
    const RecordInfo &record,
    const ToolRequest &request)
{
    if (request.countCollectibleType.length() > 0 &&
        lowerCopy(record.collectibleType) !=
            lowerCopy(request.countCollectibleType))
    {
        return false;
    }

    if (request.countSubjectPrefix.length() > 0)
    {
        const String subjectLower =
            lowerCopy(record.subject);

        const String subjectPrefixLower =
            lowerCopy(
                request.countSubjectPrefix
            );

        if (!subjectLower.startsWith(
                subjectPrefixLower))
        {
            return false;
        }
    }

    if (request.countProductTypePrefix.length() > 0)
    {
        const String productLower =
            lowerCopy(record.productType);

        const String prefixLower =
            lowerCopy(
                request.countProductTypePrefix
            );

        if (!productLower.startsWith(
                prefixLower))
        {
            return false;
        }
    }

    const bool hasSeriesScope =
        request.countSeriesNamePrefix.length() > 0;

    const bool hasAffiliationScope =
        request.countAffiliationPrefix.length() > 0;

    if (hasSeriesScope ||
        hasAffiliationScope)
    {
        bool franchiseMatch = false;

        if (hasSeriesScope)
        {
            const String seriesLower =
                lowerCopy(record.seriesName);

            const String seriesPrefixLower =
                lowerCopy(
                    request.countSeriesNamePrefix
                );

            franchiseMatch =
                seriesLower.startsWith(
                    seriesPrefixLower
                );
        }

        if (!franchiseMatch &&
            hasAffiliationScope)
        {
            const String affiliationLower =
                lowerCopy(record.affiliation);

            const String affiliationPrefixLower =
                lowerCopy(
                    request.countAffiliationPrefix
                );

            franchiseMatch =
                affiliationLower.startsWith(
                    affiliationPrefixLower
                );
        }

        if (!franchiseMatch)
        {
            return false;
        }
    }

    return true;
}

ToolStatus toolCountItemsFiltered(
    const ToolRequest &request,
    CountResult &result)
{
    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        return ToolStatus::MalformedSnapshot;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return ToolStatus::MalformedSnapshot;
    }

    while (true)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state ==
            RecordReadState::EndOfArray)
        {
            break;
        }

        if (state ==
            RecordReadState::Malformed)
        {
            file.close();
            return ToolStatus::MalformedSnapshot;
        }

        if (!recordMatchesCountFilter(
                record,
                request))
        {
            continue;
        }

        ++result.total;

        if (record.collectibleType == "Funko")
        {
            ++result.funko;
        }
        else if (record.collectibleType == "Thrilljoy")
        {
            ++result.thrilljoy;
        }
        else if (record.collectibleType == "Enamel Pins")
        {
            ++result.pins;
        }
        else
        {
            file.close();
            return ToolStatus::MalformedSnapshot;
        }
    }

    file.close();
    return ToolStatus::Ok;
}

ToolStatus toolCountItems(
    CountResult &result)
{
    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        return ToolStatus::MalformedSnapshot;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return ToolStatus::MalformedSnapshot;
    }

    while (true)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state == RecordReadState::EndOfArray)
        {
            break;
        }

        if (state == RecordReadState::Malformed)
        {
            file.close();
            return ToolStatus::MalformedSnapshot;
        }

        ++result.total;

        if (record.collectibleType == "Funko")
        {
            ++result.funko;
        }
        else if (record.collectibleType == "Thrilljoy")
        {
            ++result.thrilljoy;
        }
        else if (record.collectibleType == "Enamel Pins")
        {
            ++result.pins;
        }
        else
        {
            file.close();
            return ToolStatus::MalformedSnapshot;
        }
    }

    file.close();
    return ToolStatus::Ok;
}

ToolStatus toolListItems(
    const String &collectibleType,
    size_t limit,
    RecordInfo *results,
    size_t capacity,
    size_t &returned)
{
    returned = 0;

    if (limit == 0 ||
        results == nullptr ||
        capacity == 0)
    {
        return ToolStatus::InvalidArgument;
    }

    if (limit > capacity)
    {
        limit = capacity;
    }

    String typeNeedle = collectibleType;
    typeNeedle.trim();
    const String typeLower =
        lowerCopy(typeNeedle);

    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        return ToolStatus::MalformedSnapshot;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return ToolStatus::MalformedSnapshot;
    }

    while (returned < limit)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state == RecordReadState::EndOfArray)
        {
            break;
        }

        if (state == RecordReadState::Malformed)
        {
            file.close();
            return ToolStatus::MalformedSnapshot;
        }

        if (typeLower.length() == 0 ||
            lowerCopy(record.collectibleType) ==
                typeLower)
        {
            results[returned] = record;
            ++returned;
        }
    }

    file.close();

    return returned > 0
        ? ToolStatus::Ok
        : ToolStatus::NotFound;
}

bool validateDeterministicTools()
{
    Serial.println();
    Serial.println(
        "=== RHEA1-E3 DETERMINISTIC TOOL SELF-TESTS ==="
    );

    CountResult counts;

    if (toolCountItems(counts) != ToolStatus::Ok)
    {
        Serial.println(
            "E3 FAILED: toolCountItems"
        );
        return false;
    }

    if (counts.total == 0 ||
        (counts.funko +
         counts.thrilljoy +
         counts.pins) !=
            counts.total)
    {
        Serial.println(
            "E3 FAILED: toolCountItems returned inconsistent live counts"
        );
        return false;
    }

    Serial.print("PASS count_items total=");
    Serial.println(counts.total);

    RecordInfo found;

    if (toolFindItem(
            "trl-pix-05881-hro",
            found) != ToolStatus::Ok ||
        found.id != "TRL-PIX-05881-HRO")
    {
        Serial.println(
            "E3 FAILED: find_item exact ID precedence"
        );
        return false;
    }

    Serial.println(
        "PASS find_item exact ID precedence"
    );

    ToolRequest dexterRequest;
    dexterRequest.kind =
        ToolKind::FindItem;
    dexterRequest.argument =
        "dexter";

    static RecordInfo dexterMatches[5];
    size_t dexterReturned = 0;
    size_t dexterTotal = 0;

    const ToolStatus dexterStatus =
        toolFindCandidates(
            dexterRequest,
            dexterMatches,
            5,
            dexterReturned,
            dexterTotal
        );

    bool sawPopDexter = false;
    bool sawThrilljoyDexter = false;

    for (size_t i = 0;
         i < dexterReturned;
         ++i)
    {
        if (dexterMatches[i].id ==
            "POP-ANI-00731-4IP-VX")
        {
            sawPopDexter = true;
        }

        if (dexterMatches[i].id ==
            "TRL-PIX-05881-HRO")
        {
            sawThrilljoyDexter = true;
        }
    }

    if (dexterStatus != ToolStatus::Ok ||
        dexterTotal < 2 ||
        !sawPopDexter ||
        !sawThrilljoyDexter)
    {
        Serial.println(
            "E3 FAILED: ambiguous Dexter title lookup"
        );
        return false;
    }

    Serial.print(
        "PASS ambiguous Dexter title lookup matches="
    );
    Serial.println(dexterTotal);

    RecordInfo pins[3];
    size_t returned = 0;

    if (toolListItems(
            "Enamel Pins",
            3,
            pins,
            3,
            returned) != ToolStatus::Ok ||
        returned != 3)
    {
        Serial.println(
            "E3 FAILED: list_items Enamel Pins"
        );
        return false;
    }

    for (size_t i = 0; i < returned; ++i)
    {
        if (!pins[i].id.startsWith("ENP-"))
        {
            Serial.println(
                "E3 FAILED: list_items returned non-pin result"
            );
            return false;
        }
    }

    Serial.println(
        "PASS list_items Enamel Pins limit=3"
    );

    if (toolFindItem(
            "__RHEA_DOES_NOT_EXIST__",
            found) != ToolStatus::NotFound)
    {
        Serial.println(
            "E3 FAILED: find_item absent query"
        );
        return false;
    }

    Serial.println(
        "PASS find_item absent query => NotFound"
    );

    Serial.println(
        "RHEA1-E3 DETERMINISTIC TOOL VALIDATION: PASS"
    );
    Serial.println(
        "==========================================="
    );

    return true;
}

void showCountResult(
    const CountResult &counts)
{
    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextSize(1);
    display.setTextWrap(true);

    display.setTextColor(GREEN, BLACK);
    display.println("F L O R A");
    display.println("--------------------");
    display.setTextColor(WHITE, BLACK);
    display.print("You've got ");
    display.print(counts.total);
    display.println(" items.");

    display.print("Funko: ");
    display.println(counts.funko);
    display.print("Thrilljoy: ");
    display.println(counts.thrilljoy);
    display.print("Pins: ");
    display.println(counts.pins);

    display.println();
    display.println("[ENTER] Ask me something else");

    Serial.print("FLORA_SAYS You've got ");
    Serial.print(counts.total);
    Serial.println(" items.");
}


void showCountResult(
    const CountResult &counts,
    const ToolRequest &request)
{
    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextSize(1);
    display.setTextWrap(true);

    display.setTextColor(GREEN, BLACK);
    display.println("F L O R A");
    display.println("--------------------");
    display.setTextColor(WHITE, BLACK);

    const String scopeLabel =
        countScopeLabel(
            request
        );

    if (scopeLabel.length() > 0)
    {
        display.print("You've got ");
        display.print(counts.total);
        display.print(" ");
        display.print(scopeLabel);
        display.println(".");
    }
    else
    {
        display.print("You've got ");
        display.print(counts.total);
        display.println(" items.");
    }

    if (request.countCollectibleType.length() == 0 &&
        request.countProductTypePrefix.length() == 0 &&
        request.countSeriesNamePrefix.length() == 0 &&
        request.countAffiliationPrefix.length() == 0 &&
        request.countSubjectPrefix.length() == 0)
    {
        display.println();
        display.print("Funko: ");
        display.println(counts.funko);
        display.print("Thrilljoy: ");
        display.println(counts.thrilljoy);
        display.print("Pins: ");
        display.println(counts.pins);
    }

    display.println();
    display.println("[ENTER] Ask me something else");

    Serial.print("FLORA_SAYS You've got ");
    Serial.print(counts.total);

    if (scopeLabel.length() > 0)
    {
        Serial.print(" ");
        Serial.print(scopeLabel);
    }
    else
    {
        Serial.print(" items");
    }

    Serial.println(".");
}

void showListResult(
    const String &label,
    RecordInfo *records,
    size_t count)
{
    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextSize(1);
    display.setTextWrap(true);

    display.setTextColor(GREEN, BLACK);
    display.println("F L O R A");
    display.println("--------------------");
    display.setTextColor(WHITE, BLACK);
    display.print("Sure - here are ");
    display.print(count);
    display.print(" ");
    display.print(label);
    display.println(":");

    for (size_t i = 0; i < count; ++i)
    {
        display.print(i + 1);
        display.print(". ");
        display.println(records[i].title);
    }

    display.println();
    display.println("[ENTER] Ask me something else");

    Serial.print("FLORA_SAYS Sure - here are ");
    Serial.print(count);
    Serial.print(" ");
    Serial.print(label);
    Serial.println(".");
}

bool findProductionRecord(
    const String &query,
    RecordInfo &match)
{
    File file =
        SD.open(
            RHEA_SNAPSHOT_ACTIVE_PATH,
            FILE_READ
        );

    if (!file)
    {
        Serial.println(
            "Search FAILED: active snapshot missing"
        );
        return false;
    }

    EnvelopeInfo envelope;

    if (!locateRecordsArray(
            file,
            envelope))
    {
        file.close();
        return false;
    }

    const String needle =
        lowerCopy(query);

    size_t scanned = 0;

    while (true)
    {
        RecordInfo record;

        const RecordReadState state =
            readNextRecord(
                file,
                record
            );

        if (state == RecordReadState::EndOfArray)
        {
            break;
        }

        if (state == RecordReadState::Malformed)
        {
            file.close();
            Serial.println(
                "Search FAILED: malformed snapshot"
            );
            return false;
        }

        ++scanned;

        const String titleLower =
            lowerCopy(record.title);

        const String idLower =
            lowerCopy(record.id);

        const bool idMatch =
            idLower == needle ||
            idLower.indexOf(needle) >= 0;

        const bool titleMatch =
            titleLower.indexOf(needle) >= 0;

        if (idMatch || titleMatch)
        {
            match = record;
            file.close();

            Serial.print(
                "Search match after records scanned: "
            );
            Serial.println(scanned);

            return true;
        }
    }

    file.close();

    Serial.print(
        "Search complete. Records scanned: "
    );
    Serial.println(scanned);

    return false;
}


struct ToolResponse
{
    ToolStatus status = ToolStatus::InvalidArgument;
    RecordInfo record;
    RecordInfo findMatches[5];
    size_t findMatchCount = 0;
    size_t findTotalMatches = 0;
    bool findAmbiguous = false;
    CountResult counts;
    RecordInfo list[5];
    size_t listCount = 0;
    bool updateSucceeded = false;
};


// -----------------------------------------------------------------------------
// RHEA1-F4B: compact natural-language intent router.
//
// The classifier chooses ONLY the intent. All arguments are extracted with
// deterministic code below, and all collection access still goes through the
// E4 deterministic ToolRequest/executeToolRequest layer.
// -----------------------------------------------------------------------------

struct IntentClassification
{
    char label = 'X';
    float bestScore = 0.0f;
    float secondScore = 0.0f;
};

uint32_t rheaFnv1a(
    const String &value)
{
    uint32_t hash = 2166136261u;

    for (size_t i = 0;
         i < value.length();
         ++i)
    {
        hash ^=
            static_cast<uint8_t>(
                value[i]
            );

        hash *= 16777619u;
    }

    return hash;
}

String normalizeIntentText(
    const String &input)
{
    String output = " ";
    bool lastWasSpace = true;

    for (size_t i = 0;
         i < input.length();
         ++i)
    {
        char c =
            static_cast<char>(
                tolower(
                    static_cast<unsigned char>(
                        input[i]
                    )
                )
            );

        const bool keep =
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-';

        if (keep)
        {
            output += c;
            lastWasSpace = false;
        }
        else if (!lastWasSpace)
        {
            output += ' ';
            lastWasSpace = true;
        }
    }

    while (output.length() > 1 &&
           output.endsWith(" "))
    {
        output.remove(
            output.length() - 1
        );
    }

    output += ' ';
    return output;
}

void addIntentFeature(
    float *features,
    const String &key,
    float value)
{
    using namespace rhea_intent_model;

    const uint32_t index =
        rheaFnv1a(key) %
        static_cast<uint32_t>(
            kFeatureDim
        );

    features[index] += value;
}

IntentClassification classifyIntent(
    const String &input)
{
    using namespace rhea_intent_model;

    static float features[kFeatureDim];

    for (int i = 0;
         i < kFeatureDim;
         ++i)
    {
        features[i] = 0.0f;
    }

    const String normalized =
        normalizeIntentText(input);

    String words[24];
    size_t wordCount = 0;
    String current;

    for (size_t i = 0;
         i < normalized.length();
         ++i)
    {
        const char c =
            normalized[i];

        if (c == ' ')
        {
            if (current.length() > 0)
            {
                if (wordCount <
                    (sizeof(words) /
                     sizeof(words[0])))
                {
                    words[wordCount++] =
                        current;
                }

                current = "";
            }
        }
        else
        {
            current += c;
        }
    }

    for (size_t i = 0;
         i < wordCount;
         ++i)
    {
        addIntentFeature(
            features,
            "w:" + words[i],
            1.0f
        );
    }

    for (size_t i = 0;
         i + 1 < wordCount;
         ++i)
    {
        addIntentFeature(
            features,
            "b:" +
                words[i] +
                " " +
                words[i + 1],
            1.5f
        );
    }

    for (int n = 3;
         n <= 5;
         ++n)
    {
        if (normalized.length() <
            static_cast<size_t>(n))
        {
            continue;
        }

        for (size_t i = 0;
             i + static_cast<size_t>(n) <=
                 normalized.length();
             ++i)
        {
            addIntentFeature(
                features,
                "c" +
                    String(n) +
                    ":" +
                    normalized.substring(
                        i,
                        i + n
                    ),
                0.25f
            );
        }
    }

    double normSquared = 0.0;

    for (int i = 0;
         i < kFeatureDim;
         ++i)
    {
        normSquared +=
            static_cast<double>(
                features[i]
            ) *
            static_cast<double>(
                features[i]
            );
    }

    if (normSquared > 0.0)
    {
        const float inverseNorm =
            1.0f /
            static_cast<float>(
                sqrt(normSquared)
            );

        for (int i = 0;
             i < kFeatureDim;
             ++i)
        {
            features[i] *=
                inverseNorm;
        }
    }

    float scores[kClassCount] = {};

    int bestIndex = 0;
    int secondIndex = 0;

    for (int c = 0;
         c < kClassCount;
         ++c)
    {
        float score =
            kBias[c];

        for (int i = 0;
             i < kFeatureDim;
             ++i)
        {
            score +=
                kWeights[c][i] *
                features[i];
        }

        scores[c] = score;

        if (c == 0 ||
            score >
                scores[bestIndex])
        {
            secondIndex =
                bestIndex;
            bestIndex = c;
        }
        else if (c == 1 ||
                 secondIndex == bestIndex ||
                 score >
                    scores[secondIndex])
        {
            secondIndex = c;
        }
    }

    IntentClassification result;
    result.label =
        kLabels[bestIndex];
    result.bestScore =
        scores[bestIndex];
    result.secondScore =
        scores[secondIndex];

    return result;
}

bool containsAny(
    const String &lower,
    const char *const *needles,
    size_t count)
{
    for (size_t i = 0;
         i < count;
         ++i)
    {
        if (lower.indexOf(
                needles[i]) >= 0)
        {
            return true;
        }
    }

    return false;
}

bool hasExplicitUpdateCue(
    const String &lower)
{
    static const char *const cues[] =
    {
        "update",
        "sync",
        "refresh",
        "download",
        "pull ",
        "latest",
        "current records",
        "current data",
        "up to date"
    };

    return containsAny(
        lower,
        cues,
        sizeof(cues) /
        sizeof(cues[0])
    );
}

String canonicalListType(
    const String &lower)
{
    if (lower.indexOf("enamel") >= 0 ||
        lower.indexOf(" pin") >= 0 ||
        lower.startsWith("pin") ||
        lower.indexOf("pins") >= 0)
    {
        return "Enamel Pins";
    }

    if (lower.indexOf("thrilljoy") >= 0)
    {
        return "Thrilljoy";
    }

    if (lower.indexOf("funko") >= 0 ||
        lower.indexOf(" pop") >= 0 ||
        lower.startsWith("pop"))
    {
        return "Funko";
    }

    return "";
}

size_t extractListLimit(
    const String &lower)
{
    // Explicit digits take precedence.
    for (size_t i = 0;
         i < lower.length();
         ++i)
    {
        if (isDigit(lower[i]))
        {
            size_t j = i;

            while (j < lower.length() &&
                   isDigit(lower[j]))
            {
                ++j;
            }

            const int value =
                lower.substring(
                    i,
                    j
                ).toInt();

            if (value > 0)
            {
                return static_cast<size_t>(
                    min(value, 5)
                );
            }

            i = j;
        }
    }

    struct NumberWord
    {
        const char *word;
        size_t value;
    };

    static const NumberWord words[] =
    {
        { " one ", 1 },
        { " two ", 2 },
        { " three ", 3 },
        { " four ", 4 },
        { " five ", 5 }
    };

    const String padded =
        " " + lower + " ";

    for (const auto &entry : words)
    {
        if (padded.indexOf(
                entry.word) >= 0)
        {
            return entry.value;
        }
    }

    return 5;
}

String stripFindDecorators(
    const String &original)
{
    String value = original;
    value.trim();

    String lower =
        lowerCopy(value);

    static const char *const prefixes[] =
    {
        "can you show me my ",
        "could you show me my ",
        "show me my ",
        "do i have a ",
        "do i have an ",
        "do i own a ",
        "do i own an ",
        "can you find ",
        "could you find ",
        "please find ",
        "find item ",
        "find collectible ",
        "find pop ",
        "find pin ",
        "find thrilljoy ",
        "find ",
        "show me ",
        "show item ",
        "show collectible ",
        "show ",
        "look up ",
        "lookup ",
        "search my collection for ",
        "search for ",
        "search ",
        "open ",
        "get item ",
        "get ",
        "locate ",
        "pull up ",
        "do i have ",
        "where is ",
        "what do i have called ",
        "what do i have named "
    };

    for (const char *prefix : prefixes)
    {
        if (lower.startsWith(prefix))
        {
            value =
                value.substring(
                    strlen(prefix)
                );

            value.trim();
            lower =
                lowerCopy(value);
            break;
        }
    }

    static const char *const suffixes[] =
    {
        " for me please",
        " for me",
        " please",
        " in my collection",
        " from my collection",
        " in the collection",
        " from the collection"
    };

    for (const char *suffix : suffixes)
    {
        if (lower.endsWith(suffix))
        {
            value.remove(
                value.length() -
                strlen(suffix)
            );

            value.trim();
            lower =
                lowerCopy(value);
            break;
        }
    }

    static const char *const genericNounSuffixes[] =
    {
        " funko pop",
        " pop",
        " figure",
        " collectible",
        " item"
    };

    for (const char *suffix : genericNounSuffixes)
    {
        if (lower.endsWith(suffix))
        {
            value.remove(
                value.length() -
                strlen(suffix)
            );

            value.trim();
            break;
        }
    }

    return value;
}


bool looksLikeManyToken(
    const String &token)
{
    const String target = "many";

    const size_t aLength =
        token.length();

    const size_t bLength =
        target.length();

    if (aLength < 2 ||
        aLength > 6)
    {
        return false;
    }

    int previous[7] = {};
    int current[7] = {};

    for (size_t j = 0;
         j <= bLength;
         ++j)
    {
        previous[j] =
            static_cast<int>(j);
    }

    for (size_t i = 1;
         i <= aLength;
         ++i)
    {
        current[0] =
            static_cast<int>(i);

        for (size_t j = 1;
             j <= bLength;
             ++j)
        {
            const int substitutionCost =
                token[i - 1] ==
                    target[j - 1]
                    ? 0
                    : 1;

            const int deletion =
                previous[j] + 1;

            const int insertion =
                current[j - 1] + 1;

            const int substitution =
                previous[j - 1] +
                substitutionCost;

            current[j] =
                min(
                    deletion,
                    min(
                        insertion,
                        substitution
                    )
                );
        }

        for (size_t j = 0;
             j <= bLength;
             ++j)
        {
            previous[j] =
                current[j];
        }
    }

    return previous[bLength] <= 2;
}

String trimCountQuestionNoise(
    String value)
{
    value.trim();

    String lower =
        lowerCopy(value);

    // Strip terminal punctuation before suffix matching.
    while (value.endsWith("?") ||
           value.endsWith(".") ||
           value.endsWith("!"))
    {
        value.remove(value.length() - 1);
        value.trim();
    }

    lower = lowerCopy(value);

    // The classifier has already selected count_items. Normalize a mistyped
    // "how many" lead-in before deterministic scope extraction.
    if (lower.startsWith("how "))
    {
        const int firstSpace =
            lower.indexOf(' ');

        const int secondSpace =
            lower.indexOf(
                ' ',
                firstSpace + 1
            );

        if (firstSpace >= 0 &&
            secondSpace > firstSpace)
        {
            const String secondWord =
                lower.substring(
                    firstSpace + 1,
                    secondSpace
                );

            if (looksLikeManyToken(
                    secondWord))
            {
                value =
                    value.substring(
                        secondSpace + 1
                    );

                value.trim();

                lower =
                    lowerCopy(value);
            }
        }
    }

    static const char *const prefixes[] =
    {
        "how many ",
        "count my ",
        "count the ",
        "count ",
        "number of ",
        "what is the number of ",
        "what's the number of ",
        "what is my ",
        "what's my ",
        "total "
    };

    for (const char *prefix : prefixes)
    {
        if (lower.startsWith(prefix))
        {
            value =
                value.substring(
                    strlen(prefix)
                );
            value.trim();
            lower =
                lowerCopy(value);
            break;
        }
    }

    static const char *const suffixes[] =
    {
        " do i have",
        " do i own",
        " are in my collection",
        " are in the collection",
        " in my collection",
        " in the collection",
        " do we have",
        " do we own"
    };

    for (const char *suffix : suffixes)
    {
        if (lower.endsWith(suffix))
        {
            value.remove(
                value.length() -
                strlen(suffix)
            );
            value.trim();
            break;
        }
    }

    while (value.endsWith("?") ||
           value.endsWith(".") ||
           value.endsWith("!"))
    {
        value.remove(
            value.length() - 1
        );
        value.trim();
    }

    return value;
}

void extractCountFilters(
    const String &input,
    ToolRequest &request)
{
    String scope =
        trimCountQuestionNoise(
            input
        );

    String lower =
        lowerCopy(scope);

    // "pops" means Funko Pop-family product types. This intentionally uses
    // a prefix rather than exact equality so Pop! Vinyl, Deluxe, Rides, etc.
    // remain in scope.
    const int popsIndex =
        lower.indexOf(" pops");

    const bool endsWithPops =
        lower.endsWith("pops");

    if (popsIndex >= 0 ||
        endsWithPops ||
        lower == "pop" ||
        lower == "pops")
    {
        request.countCollectibleType =
            "Funko";
        request.countProductTypePrefix =
            "Pop!";

        int categoryIndex =
            popsIndex;

        if (categoryIndex < 0 &&
            endsWithPops)
        {
            categoryIndex =
                lower.length() - 4;
        }

        if (categoryIndex > 0)
        {
            String affiliation =
                scope.substring(
                    0,
                    categoryIndex
                );
            affiliation.trim();

            String affiliationLower =
                lowerCopy(affiliation);

            if (affiliationLower.startsWith("my "))
            {
                affiliation =
                    affiliation.substring(3);
                affiliation.trim();
            }

            if (affiliation.length() > 0 &&
                lowerCopy(affiliation) != "all")
            {
                request.countSeriesNamePrefix =
                    affiliation;

                request.countAffiliationPrefix =
                    affiliation;
            }
        }

        return;
    }

    // Enamel Pin subject queries: "Good Girl Art pins" means
    // collectibleType=Enamel Pins + subject=Good Girl Art.
    const int pinsIndex = lower.indexOf(" pins");
    const bool endsWithPins = lower.endsWith("pins");

    if (pinsIndex >= 0 || endsWithPins ||
        lower == "pin" || lower == "pins" ||
        lower.indexOf("enamel pin") >= 0)
    {
        request.countCollectibleType = "Enamel Pins";

        int categoryIndex = pinsIndex;
        if (categoryIndex < 0 && endsWithPins)
        {
            categoryIndex = lower.length() - 4;
        }

        if (categoryIndex > 0)
        {
            String subject = scope.substring(0, categoryIndex);
            subject.trim();

            String subjectLower = lowerCopy(subject);
            if (subjectLower.startsWith("my "))
            {
                subject = subject.substring(3);
                subject.trim();
            }

            const String normalizedSubject = lowerCopy(subject);
            if (subject.length() > 0 &&
                normalizedSubject != "all" &&
                normalizedSubject != "enamel")
            {
                request.countSubjectPrefix = subject;
            }
        }

        return;
    }

    static const char *const physicalNouns[] =
    {
        " figures",
        " figure",
        " collectibles",
        " collectible",
        " items",
        " item"
    };

    for (const char *noun : physicalNouns)
    {
        const int nounIndex =
            lower.indexOf(noun);

        if (nounIndex > 0)
        {
            String franchise =
                scope.substring(
                    0,
                    nounIndex
                );

            franchise.trim();

            String franchiseLower =
                lowerCopy(franchise);

            if (franchiseLower.startsWith("my "))
            {
                franchise =
                    franchise.substring(3);
                franchise.trim();
            }

            if (franchise.length() > 0 &&
                lowerCopy(franchise) != "all")
            {
                request.countSeriesNamePrefix =
                    franchise;
                request.countAffiliationPrefix =
                    franchise;
            }

            return;
        }
    }

    if (lower.indexOf("thrilljoy") >= 0)
    {
        request.countCollectibleType =
            "Thrilljoy";
        return;
    }

    if (lower.indexOf("funko") >= 0)
    {
        request.countCollectibleType =
            "Funko";
        return;
    }
}

String countScopeLabel(
    const ToolRequest &request)
{
    String label;

    if (request.countSubjectPrefix.length() > 0)
    {
        label = request.countSubjectPrefix;
        label += " Pins";
        return label;
    }

    if (request.countAffiliationPrefix.length() > 0)
    {
        label += request.countAffiliationPrefix;
    }

    if (request.countProductTypePrefix.length() > 0)
    {
        if (label.length() > 0) label += " ";
        if (request.countProductTypePrefix == "Pop!") label += "Pops";
        else label += request.countProductTypePrefix;
    }
    else if (request.countCollectibleType.length() > 0)
    {
        if (label.length() > 0) label += " ";
        if (request.countCollectibleType == "Enamel Pins") label += "Pins";
        else label += request.countCollectibleType;
    }

    return label;
}

void extractFindFilters(
    const String &input,
    ToolRequest &request)
{
    String lower =
        lowerCopy(input);

    lower.trim();

    // In normal collector language, singular/plural "Pop" means the Funko
    // Pop! product family, not merely any Funko item whose title matches.
    const bool asksForPop =
        lower == "pop" ||
        lower.endsWith(" pop") ||
        lower.endsWith(" pops") ||
        lower.indexOf(" pop ") >= 0 ||
        lower.indexOf(" pops ") >= 0 ||
        lower.indexOf("funko pop") >= 0;

    if (asksForPop)
    {
        request.findCollectibleType =
            "Funko";
        request.findProductTypePrefix =
            "Pop! Vinyl";
    }
}

bool hasUnsupportedCapabilityCue(
    const String &input)
{
    String lower =
        lowerCopy(input);

    lower.trim();

    // Current private Rhea snapshot intentionally does not contain physical
    // storage/location data. Questions that explicitly ask where an item is
    // must not be misrouted into count/find/list.
    static const char *const locationPrefixes[] =
    {
        "where is my ",
        "where's my ",
        "where is the ",
        "where's the ",
        "where did i put ",
        "what shelf is ",
        "which shelf is ",
        "what bin is ",
        "which bin is ",
        "what box is ",
        "which box is ",
        "what location is ",
        "which location is "
    };

    for (const char *prefix : locationPrefixes)
    {
        if (lower.startsWith(prefix))
        {
            return true;
        }
    }

    return false;
}

bool hasDeterministicFindCue(
    const String &input)
{
    String lower =
        lowerCopy(input);

    lower.trim();

    static const char *const prefixes[] =
    {
        "can you show me my ",
        "could you show me my ",
        "show me my ",
        "do i have a ",
        "do i have an ",
        "do i have ",
        "do i own a ",
        "do i own an ",
        "do i own ",
        "pull up "
    };

    for (const char *prefix : prefixes)
    {
        if (lower.startsWith(prefix))
        {
            return true;
        }
    }

    return false;
}

ToolRequest naturalLanguageToToolRequest(
    const String &input,
    IntentClassification *classificationOut = nullptr)
{
    ToolRequest request;

    const IntentClassification classification =
        classifyIntent(input);

    if (classificationOut != nullptr)
    {
        *classificationOut =
            classification;
    }

    const String lower =
        lowerCopy(input);

    if (hasUnsupportedCapabilityCue(input))
    {
        request.kind =
            ToolKind::Unknown;

        return request;
    }

    if (hasDeterministicFindCue(input))
    {
        const String query =
            stripFindDecorators(
                input
            );

        if (query.length() >= 2)
        {
            request.kind =
                ToolKind::FindItem;
            request.argument =
                query;

            extractFindFilters(
                input,
                request
            );
        }

        return request;
    }

    switch (classification.label)
    {
        case 'F':
        {
            const String query =
                stripFindDecorators(
                    input
                );

            if (query.length() >= 2)
            {
                request.kind =
                    ToolKind::FindItem;
                request.argument =
                    query;

                extractFindFilters(
                    input,
                    request
                );
            }

            break;
        }

        case 'C':
            request.kind =
                ToolKind::CountItems;

            extractCountFilters(
                input,
                request
            );
            break;

        case 'L':
            request.kind =
                ToolKind::ListItems;
            request.argument =
                canonicalListType(
                    lower
                );
            request.limit =
                extractListLimit(
                    lower
                );
            break;

        case 'U':
            // Updating records enables Wi-Fi and replaces the local snapshot.
            // Require an explicit deterministic update cue in addition to the
            // statistical classification so an ambiguous phrase cannot cause
            // a network write.
            if (hasExplicitUpdateCue(
                    lower))
            {
                request.kind =
                    ToolKind::UpdateRecords;
            }
            break;

        default:
            request.kind =
                ToolKind::Unknown;
            break;
    }

    return request;
}

String toolKindName(
    ToolKind kind)
{
    switch (kind)
    {
        case ToolKind::FindItem:
            return "find_item";

        case ToolKind::CountItems:
            return "count_items";

        case ToolKind::ListItems:
            return "list_items";

        case ToolKind::UpdateRecords:
            return "update_records";

        default:
            return "unknown";
    }
}

bool validateAdversarialRouting()
{
    struct ProbeCase
    {
        const char *text;
        ToolKind expectedKind;
    };

    static const ProbeCase cases[] =
    {
        { "tell me a joke", ToolKind::Unknown },
        { "what time is it", ToolKind::Unknown },
        { "battery status", ToolKind::Unknown },
        { "turn on wifi", ToolKind::Unknown },
        { "delete everything", ToolKind::Unknown },
        { "who is Batman", ToolKind::Unknown },
        { "find Batman", ToolKind::FindItem },
        { "show me Batman", ToolKind::FindItem },
        { "how many items are there", ToolKind::CountItems },
        { "list five Funko", ToolKind::ListItems },
        { "refresh the records", ToolKind::UpdateRecords },
        { "download the latest records", ToolKind::UpdateRecords }
    };

    Serial.println();
    Serial.println(
        "=== RHEA1-F4F ADVERSARIAL ROUTING PROBE ==="
    );

    size_t passed = 0;

    for (size_t i = 0;
         i < sizeof(cases) /
             sizeof(cases[0]);
         ++i)
    {
        IntentClassification classification;

        const ToolRequest request =
            naturalLanguageToToolRequest(
                cases[i].text,
                &classification
            );

        const bool ok =
            request.kind ==
                cases[i].expectedKind;

        Serial.print(
            ok
                ? "PASS "
                : "FAIL "
        );
        Serial.print("[");
        Serial.print(i + 1);
        Serial.print("] ");
        Serial.print(cases[i].text);
        Serial.print(" => ");
        Serial.print(
            toolKindName(
                request.kind
            )
        );
        Serial.print(" classifier=");
        Serial.print(
            classification.label
        );
        Serial.print(" margin=");
        Serial.println(
            classification.bestScore -
            classification.secondScore,
            4
        );

        if (ok)
        {
            ++passed;
        }
    }

    const size_t total =
        sizeof(cases) /
        sizeof(cases[0]);

    Serial.println();
    Serial.print("F4F adversarial probes: ");
    Serial.print(passed);
    Serial.print("/");
    Serial.println(total);

    return passed == total;
}

bool validateNaturalLanguageRouter()
{
    struct TestCase
    {
        const char *text;
        ToolKind expectedKind;
        const char *expectedArgument;
        size_t expectedLimit;
        const char *expectedCountType;
        const char *expectedCountProductPrefix;
        const char *expectedCountSeriesNamePrefix;
        const char *expectedCountAffiliationPrefix;
        const char *expectedFindType;
        const char *expectedFindProductPrefix;

        TestCase(
            const char *textValue,
            ToolKind kindValue,
            const char *argumentValue,
            size_t limitValue,
            const char *countTypeValue,
            const char *countProductPrefixValue,
            const char *countSeriesPrefixValue,
            const char *countAffiliationPrefixValue,
            const char *findTypeValue = "",
            const char *findProductPrefixValue = "")
            : text(textValue),
              expectedKind(kindValue),
              expectedArgument(argumentValue),
              expectedLimit(limitValue),
              expectedCountType(countTypeValue),
              expectedCountProductPrefix(countProductPrefixValue),
              expectedCountSeriesNamePrefix(countSeriesPrefixValue),
              expectedCountAffiliationPrefix(countAffiliationPrefixValue),
              expectedFindType(findTypeValue),
              expectedFindProductPrefix(findProductPrefixValue)
        {
        }
    };

    static const TestCase cases[] =
    {
        {
            "Find Dexter",
            ToolKind::FindItem,
            "Dexter",
            5,
            "",
            "",
            "",
            ""
        },
        {
            "Show me TRL-PIX-05881-HRO",
            ToolKind::FindItem,
            "TRL-PIX-05881-HRO",
            5,
            "",
            "",
            "",
            ""
        },
        {
            "can you find Wakko",
            ToolKind::FindItem,
            "Wakko",
            5,
            "",
            "",
            "",
            ""
        },
        {
            "do i have a batman pop",
            ToolKind::FindItem,
            "batman",
            5,
            "",
            "",
            "",
            "",
            "Funko",
            "Pop! Vinyl"
        },
        {
            "pull up dexter for me",
            ToolKind::FindItem,
            "dexter",
            5,
            "",
            "",
            "",
            ""
        },
        {
            "can you show me my wakko",
            ToolKind::FindItem,
            "wakko",
            5,
            "",
            "",
            "",
            ""
        },
        {
            "How many items do I own?",
            ToolKind::CountItems,
            "",
            5,
            "",
            "",
            "",
            ""
        },
        {
            "Count my collection",
            ToolKind::CountItems,
            "",
            5,
            "",
            "",
            "",
            ""
        },
        {
            "How many Marvel Pops do I have?",
            ToolKind::CountItems,
            "",
            5,
            "Funko",
            "Pop!",
            "Marvel",
            "Marvel"
        },
        {
            "How many Marvel figures do I own?",
            ToolKind::CountItems,
            "",
            5,
            "",
            "",
            "Marvel",
            "Marvel"
        },
        {
            "How many Star Wars pops do I own?",
            ToolKind::CountItems,
            "",
            5,
            "Funko",
            "Pop!",
            "Star Wars",
            "Star Wars"
        },
        {
            "how nmany star wars pops do i have",
            ToolKind::CountItems,
            "",
            5,
            "Funko",
            "Pop!",
            "star wars",
            "star wars"
        },
        {
            "how mny star wars pops do i have",
            ToolKind::CountItems,
            "",
            5,
            "Funko",
            "Pop!",
            "star wars",
            "star wars"
        },
        {
            "how manyy marvel pops do i have",
            ToolKind::CountItems,
            "",
            5,
            "Funko",
            "Pop!",
            "marvel",
            "marvel"
        },
        {
            "how mnary marvel pops do i own",
            ToolKind::CountItems,
            "",
            5,
            "Funko",
            "Pop!",
            "marvel",
            "marvel"
        },
        {
            "List my enamel pins",
            ToolKind::ListItems,
            "Enamel Pins",
            5,
            "",
            "",
            "",
            ""
        },
        {
            "Show five Thrilljoy items",
            ToolKind::ListItems,
            "Thrilljoy",
            5,
            "",
            "",
            "",
            ""
        },
        {
            "show me 3 Funko",
            ToolKind::ListItems,
            "Funko",
            3,
            "",
            "",
            "",
            ""
        },
        {
            "Update records",
            ToolKind::UpdateRecords,
            "",
            5,
            "",
            "",
            "",
            ""
        },
        {
            "Sync the collection",
            ToolKind::UpdateRecords,
            "",
            5,
            "",
            "",
            "",
            ""
        },
        {
            "where is my batman",
            ToolKind::Unknown,
            "",
            5,
            "",
            "",
            "",
            ""
        },
        {
            "what shelf is batman on",
            ToolKind::Unknown,
            "",
            5,
            "",
            "",
            "",
            ""
        },
        {
            "hello there",
            ToolKind::Unknown,
            "",
            5,
            "",
            "",
            "",
            ""
        },
        {
            "battery status",
            ToolKind::Unknown,
            "",
            5,
            "",
            "",
            "",
            ""
        }
    };

    Serial.println();
    Serial.println(
        "=== RHEA1-F4K NATURAL-LANGUAGE ROUTER SELF-TEST ==="
    );

    size_t passed = 0;

    for (size_t i = 0;
         i < sizeof(cases) /
             sizeof(cases[0]);
         ++i)
    {
        IntentClassification classification;

        const ToolRequest request =
            naturalLanguageToToolRequest(
                cases[i].text,
                &classification
            );

        const bool ok =
            request.kind ==
                cases[i].expectedKind &&
            request.argument ==
                cases[i].expectedArgument &&
            request.limit ==
                cases[i].expectedLimit &&
            request.countCollectibleType ==
                cases[i].expectedCountType &&
            request.countProductTypePrefix ==
                cases[i].expectedCountProductPrefix &&
            request.countSeriesNamePrefix ==
                cases[i].expectedCountSeriesNamePrefix &&
            request.countAffiliationPrefix ==
                cases[i].expectedCountAffiliationPrefix &&
            request.findCollectibleType ==
                cases[i].expectedFindType &&
            request.findProductTypePrefix ==
                cases[i].expectedFindProductPrefix;

        Serial.print(
            ok
                ? "PASS "
                : "FAIL "
        );

        Serial.print("[");
        Serial.print(i + 1);
        Serial.print("] ");
        Serial.print(cases[i].text);
        Serial.print(" => ");
        Serial.print(
            toolKindName(
                request.kind
            )
        );

        if (request.argument.length() > 0)
        {
            Serial.print(" arg=[");
            Serial.print(
                request.argument
            );
            Serial.print("]");
        }

        if (request.kind ==
                ToolKind::FindItem &&
            (request.findCollectibleType.length() > 0 ||
             request.findProductTypePrefix.length() > 0))
        {
            Serial.print(" filter=[");

            if (request.findCollectibleType.length() > 0)
            {
                Serial.print(
                    request.findCollectibleType
                );
            }

            if (request.findProductTypePrefix.length() > 0)
            {
                if (request.findCollectibleType.length() > 0)
                {
                    Serial.print(" / ");
                }

                Serial.print(
                    request.findProductTypePrefix
                );
            }

            Serial.print("]");
        }

        if (request.kind ==
            ToolKind::ListItems)
        {
            Serial.print(" limit=");
            Serial.print(
                request.limit
            );
        }

        if (request.kind ==
            ToolKind::CountItems)
        {
            const String scopeLabel =
                countScopeLabel(
                    request
                );

            if (scopeLabel.length() > 0)
            {
                Serial.print(" scope=[");
                Serial.print(scopeLabel);
                Serial.print("]");
            }
        }

        Serial.print(" classifier=");
        Serial.print(
            classification.label
        );

        Serial.print(" margin=");
        Serial.println(
            classification.bestScore -
            classification.secondScore,
            4
        );

        if (ok)
        {
            ++passed;
        }
    }

    const size_t total =
        sizeof(cases) /
        sizeof(cases[0]);

    Serial.println();
    Serial.print("F4K router tests: ");
    Serial.print(passed);
    Serial.print("/");
    Serial.println(total);

    const bool allPassed =
        passed == total;

    Serial.println(
        allPassed
            ? "RHEA1-F4K NATURAL-LANGUAGE ROUTER: PASS"
            : "RHEA1-F4K NATURAL-LANGUAGE ROUTER: FAIL"
    );

    return allPassed;
}


void executeToolRequestInto(
    const ToolRequest &request,
    ToolResponse &response)
{
    // Reset scalar state used to interpret the active union-like response.
    // Record/String buffers are deliberately reused to avoid large temporary
    // objects on the ESP32 loopTask stack.
    response.status =
        ToolStatus::InvalidArgument;
    response.findMatchCount = 0;
    response.findTotalMatches = 0;
    response.findAmbiguous = false;
    response.listCount = 0;
    response.updateSucceeded = false;

    switch (request.kind)
    {
        case ToolKind::FindItem:
            response.status =
                toolFindCandidates(
                    request,
                    response.findMatches,
                    5,
                    response.findMatchCount,
                    response.findTotalMatches
                );

            if (response.status ==
                    ToolStatus::Ok &&
                response.findMatchCount > 0)
            {
                response.record =
                    response.findMatches[0];

                response.findAmbiguous =
                    response.findTotalMatches > 1;
            }

            break;

        case ToolKind::CountItems:
            if (request.countCollectibleType.length() > 0 ||
                request.countProductTypePrefix.length() > 0 ||
                request.countSeriesNamePrefix.length() > 0 ||
                request.countAffiliationPrefix.length() > 0)
            {
                response.status =
                    toolCountItemsFiltered(
                        request,
                        response.counts
                    );
            }
            else
            {
                response.status =
                    toolCountItems(
                        response.counts
                    );
            }
            break;

        case ToolKind::ListItems:
            response.status =
                toolListItems(
                    request.argument,
                    request.limit,
                    response.list,
                    5,
                    response.listCount
                );
            break;

        case ToolKind::UpdateRecords:
            response.updateSucceeded =
                performOnDemandUpdate();

            response.status =
                response.updateSucceeded
                    ? ToolStatus::Ok
                    : ToolStatus::MalformedSnapshot;
            break;

        default:
            response.status =
                ToolStatus::InvalidArgument;
            break;
    }
}

ToolResponse executeToolRequest(
    const ToolRequest &request)
{
    ToolResponse response;

    executeToolRequestInto(
        request,
        response
    );

    return response;
}

String toolStatusName(
    ToolStatus status)
{
    switch (status)
    {
        case ToolStatus::Ok:
            return "ok";

        case ToolStatus::NotFound:
            return "not_found";

        case ToolStatus::InvalidArgument:
            return "invalid_argument";

        case ToolStatus::MalformedSnapshot:
            return "snapshot_error";
    }

    return "unknown";
}

void printToolResponseJson(
    const ToolRequest &request,
    const ToolResponse &response)
{
    StaticJsonDocument<2048> doc;

    doc["status"] =
        toolStatusName(
            response.status
        );

    switch (request.kind)
    {
        case ToolKind::FindItem:
            doc["tool"] = "find_item";

            if (request.findCollectibleType.length() > 0)
            {
                doc["collectibleTypeFilter"] =
                    request.findCollectibleType;
            }

            if (request.findProductTypePrefix.length() > 0)
            {
                doc["productTypePrefixFilter"] =
                    request.findProductTypePrefix;
            }

            if (response.status == ToolStatus::Ok)
            {
                doc["matchCount"] =
                    response.findTotalMatches;

                doc["ambiguous"] =
                    response.findAmbiguous;

                if (response.findAmbiguous)
                {
                    JsonArray matches =
                        doc.createNestedArray("matches");

                    for (size_t i = 0;
                         i < response.findMatchCount;
                         ++i)
                    {
                        JsonObject match =
                            matches.createNestedObject();

                        match["id"] =
                            response.findMatches[i].id;

                        match["title"] =
                            response.findMatches[i].title;

                        match["collectibleType"] =
                            response.findMatches[i].collectibleType;

                        if (response.findMatches[i].productType.length() > 0)
                        {
                            match["productType"] =
                                response.findMatches[i].productType;
                        }

                        if (response.findMatches[i].seriesName.length() > 0)
                        {
                            match["seriesName"] =
                                response.findMatches[i].seriesName;
                        }

                        if (response.findMatches[i].seriesNumber > 0)
                        {
                            match["seriesNumber"] =
                                response.findMatches[i].seriesNumber;
                        }
                    }
                }

                JsonObject item =
                    doc.createNestedObject("item");

                item["id"] =
                    response.record.id;

                item["title"] =
                    response.record.title;

                item["collectibleType"] =
                    response.record.collectibleType;

                if (response.record.productType.length() > 0)
                {
                    item["productType"] =
                        response.record.productType;
                }

                if (response.record.seriesName.length() > 0)
                {
                    item["seriesName"] =
                        response.record.seriesName;
                }

                if (response.record.seriesNumber > 0)
                {
                    item["seriesNumber"] =
                        response.record.seriesNumber;
                }

                if (response.record.variant.length() > 0)
                {
                    item["variant"] =
                        response.record.variant;
                }
            }

            break;

        case ToolKind::CountItems:
            doc["tool"] = "count_items";

            if (request.countCollectibleType.length() > 0)
            {
                doc["collectibleType"] =
                    request.countCollectibleType;
            }

            if (request.countProductTypePrefix.length() > 0)
            {
                doc["productTypePrefix"] =
                    request.countProductTypePrefix;
            }

            if (request.countSeriesNamePrefix.length() > 0)
            {
                doc["seriesNamePrefix"] =
                    request.countSeriesNamePrefix;
            }

            if (request.countAffiliationPrefix.length() > 0)
            {
                doc["affiliationPrefix"] =
                    request.countAffiliationPrefix;
            }

            if (response.status == ToolStatus::Ok)
            {
                JsonObject counts =
                    doc.createNestedObject("counts");

                counts["total"] =
                    response.counts.total;

                counts["funko"] =
                    response.counts.funko;

                counts["thrilljoy"] =
                    response.counts.thrilljoy;

                counts["enamelPins"] =
                    response.counts.pins;
            }

            break;

        case ToolKind::ListItems:
        {
            doc["tool"] = "list_items";
            doc["collectibleType"] =
                request.argument;

            JsonArray items =
                doc.createNestedArray("items");

            for (size_t i = 0;
                 i < response.listCount;
                 ++i)
            {
                JsonObject item =
                    items.createNestedObject();

                item["id"] =
                    response.list[i].id;

                item["title"] =
                    response.list[i].title;

                item["collectibleType"] =
                    response.list[i].collectibleType;
            }

            break;
        }

        case ToolKind::UpdateRecords:
            doc["tool"] = "update_records";
            doc["updated"] =
                response.updateSucceeded;
            break;

        default:
            doc["tool"] = "unknown";
            break;
    }

    Serial.print("TOOL_RESPONSE ");
    serializeJson(doc, Serial);
    Serial.println();
}


// RHEA1-F6-rev2: status/diagnostics live here so all tool types and
// deterministic validation functions are already declared/defined.

bool showRheaStatus()
{
    EnvelopeInfo envelope;
    size_t snapshotBytes = 0;

    const bool snapshotOk =
        readActiveSnapshotSummary(
            envelope,
            snapshotBytes
        );

    CountResult counts;

    const ToolStatus countStatus =
        snapshotOk
            ? toolCountItems(counts)
            : ToolStatus::MalformedSnapshot;

    const bool countOk =
        countStatus == ToolStatus::Ok;

    const bool backupPresent =
        SD.exists(
            RHEA_SNAPSHOT_BACKUP_PATH
        );

    const bool candidatePresent =
        SD.exists(
            RHEA_SNAPSHOT_TEMP_PATH
        );

    const bool wifiConnected =
        WiFi.status() == WL_CONNECTED;

    const uint32_t freeHeap =
        ESP.getFreeHeap();

    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextColor(
        snapshotOk && countOk
            ? WHITE
            : RED,
        BLACK
    );
    display.setTextSize(1);
    display.setTextWrap(true);

    display.println("F L O R A");
    display.println("Status");
    display.println("--------------------");

    if (snapshotOk)
    {
        display.print("Records: ");
        display.println(counts.total);

        display.print("Schema: ");
        display.println(envelope.schemaVersion);

        display.print("Version: ");
        display.println(
            envelope.snapshotVersion.substring(
                0,
                min(
                    static_cast<size_t>(12),
                    envelope.snapshotVersion.length()
                )
            )
        );
    }
    else
    {
        display.println("Snapshot: ERROR");
    }

    display.print("Wi-Fi: ");
    display.println(
        wifiConnected
            ? "connected"
            : "offline"
    );

    display.print("Heap: ");
    display.print(freeHeap / 1024);
    display.println(" KB");

    display.print("Last update: ");
    display.println(lastUpdateState);

    if (backupPresent)
    {
        display.println("WARN: backup present");
    }

    if (candidatePresent)
    {
        display.println("WARN: candidate present");
    }

    display.println();
    display.println("[ENTER] Ask me something else");

    static StaticJsonDocument<768> doc;
    doc.clear();
    doc["snapshotOk"] = snapshotOk;
    doc["recordsOk"] = countOk;
    doc["recordCount"] = counts.total;
    doc["snapshotBytes"] = snapshotBytes;
    doc["schemaVersion"] = envelope.schemaVersion;
    doc["snapshotVersion"] = envelope.snapshotVersion;
    doc["wifi"] =
        wifiConnected
            ? "connected"
            : "offline";
    doc["freeHeap"] = freeHeap;
    doc["backupPresent"] = backupPresent;
    doc["candidatePresent"] = candidatePresent;
    doc["lastUpdate"] = lastUpdateState;

    Serial.print("FLORA_STATUS ");
    serializeJson(doc, Serial);
    Serial.println();

    Serial.print("FLORA_SAYS I'm ");
    Serial.print(
        snapshotOk && countOk
            ? "healthy"
            : "not fully healthy"
    );
    Serial.print(" with ");
    Serial.print(counts.total);
    Serial.println(" local records.");

    return snapshotOk && countOk;
}

bool runF6Diagnostics()
{
    Serial.println();
    Serial.println(
        "=== FLORA1 HANDHELD DIAGNOSTICS ==="
    );

    const bool e1 =
        validateProductionReader();

    const bool e2 =
        e1 &&
        validateQueryPrimitives();

    const bool e3 =
        e2 &&
        validateDeterministicTools();

    EnvelopeInfo envelope;
    size_t bytes = 0;

    const bool quickSnapshot =
        readActiveSnapshotSummary(
            envelope,
            bytes
        );

    const bool keyConfigured =
        deviceFunctionKeyConfigured();

    const bool backupPresent =
        SD.exists(
            RHEA_SNAPSHOT_BACKUP_PATH
        );

    const bool candidatePresent =
        SD.exists(
            RHEA_SNAPSHOT_TEMP_PATH
        );

    Serial.print("F6 snapshot quick-check: ");
    Serial.println(
        quickSnapshot
            ? "PASS"
            : "FAIL"
    );

    Serial.print("F6 device key configured: ");
    Serial.println(
        keyConfigured
            ? "YES"
            : "NO"
    );

    Serial.print("F6 free heap: ");
    Serial.println(
        ESP.getFreeHeap()
    );

    Serial.print("F6 Wi-Fi state: ");
    Serial.println(
        WiFi.status() == WL_CONNECTED
            ? "CONNECTED"
            : "OFFLINE"
    );

    Serial.print("F6 recovery backup present: ");
    Serial.println(
        backupPresent
            ? "YES"
            : "NO"
    );

    Serial.print("F6 candidate present: ");
    Serial.println(
        candidatePresent
            ? "YES"
            : "NO"
    );

    const bool passed =
        e1 &&
        e2 &&
        e3 &&
        quickSnapshot;

    Serial.println(
        passed
            ? "FLORA1 HANDHELD DIAGNOSTICS: PASS"
            : "FLORA1 HANDHELD DIAGNOSTICS: FAIL"
    );
    Serial.println(
        "======================================"
    );

    auto &display =
        floraDisplay;

    display.fillScreen(BLACK);
    display.setCursor(4, 4);
    display.setTextColor(
        passed
            ? WHITE
            : RED,
        BLACK
    );
    display.setTextSize(1);
    display.setTextWrap(true);

    display.println("F L O R A");
    display.println("Diagnostics");
    display.println("--------------------");
    display.print("Reader: ");
    display.println(e1 ? "PASS" : "FAIL");
    display.print("Queries: ");
    display.println(e2 ? "PASS" : "FAIL");
    display.print("Tools: ");
    display.println(e3 ? "PASS" : "FAIL");
    display.print("Snapshot: ");
    display.println(
        quickSnapshot
            ? "PASS"
            : "FAIL"
    );
    display.print("Device key: ");
    display.println(
        keyConfigured
            ? "configured"
            : "missing"
    );

    if (backupPresent)
    {
        display.println("WARN: backup present");
    }

    if (candidatePresent)
    {
        display.println("WARN: candidate present");
    }

    display.println();
    display.println("[ENTER] Ask me something else");

    return passed;
}

bool validateF6Features()
{
    Serial.println();
    Serial.println(
        "=== FLORA1 HANDHELD USABILITY SELF-TEST ==="
    );

    EnvelopeInfo envelope;
    size_t bytes = 0;

    if (!readActiveSnapshotSummary(
            envelope,
            bytes))
    {
        Serial.println(
            "F6 FAILED: status snapshot summary"
        );
        return false;
    }

    Serial.println(
        "PASS status snapshot summary"
    );

    ToolRequest dexterRequest;
    dexterRequest.kind =
        ToolKind::FindItem;
    dexterRequest.argument =
        "Dexter";

    RecordInfo dexterMatches[5];
    size_t dexterReturned = 0;
    size_t dexterTotal = 0;

    const ToolStatus dexterStatus =
        toolFindCandidates(
            dexterRequest,
            dexterMatches,
            5,
            dexterReturned,
            dexterTotal
        );

    if (dexterStatus != ToolStatus::Ok ||
        dexterTotal < 2)
    {
        Serial.println(
            "F6 FAILED: ambiguous find probe"
        );
        return false;
    }

    Serial.print(
        "PASS ambiguous find probe matches="
    );
    Serial.println(dexterTotal);

    String savedHistory[RHEA_HISTORY_CAPACITY];
    const size_t savedCount =
        commandHistoryCount;

    for (size_t i = 0;
         i < RHEA_HISTORY_CAPACITY;
         ++i)
    {
        savedHistory[i] =
            commandHistory[i];
    }

    commandHistoryCount = 0;

    for (size_t i = 0;
         i < RHEA_HISTORY_CAPACITY;
         ++i)
    {
        commandHistory[i] = "";
    }

    rememberCommand("status");
    rememberCommand("count my collection");

    String recalled;

    const bool historyOk =
        recallPreviousCommand(
            recalled
        ) &&
        recalled ==
            "count my collection";

    for (size_t i = 0;
         i < RHEA_HISTORY_CAPACITY;
         ++i)
    {
        commandHistory[i] =
            savedHistory[i];
    }

    commandHistoryCount =
        savedCount;

    if (!historyOk)
    {
        Serial.println(
            "F6 FAILED: command history"
        );
        return false;
    }

    Serial.println(
        "PASS command history"
    );

    Serial.println(
        "FLORA1 HANDHELD USABILITY SELF-TEST: PASS"
    );
    Serial.println(
        "========================================="
    );

    return true;
}

bool validateDispatcherCount()
{
    CountResult expected;

    if (toolCountItems(expected) !=
            ToolStatus::Ok ||
        expected.total == 0)
    {
        Serial.println(
            "E4 FAILED: baseline count_items"
        );
        return false;
    }

    ToolRequest request;
    request.kind =
        ToolKind::CountItems;

    static ToolResponse response;

    executeToolRequestInto(
        request,
        response
    );

    if (response.status != ToolStatus::Ok ||
        response.counts.total != expected.total ||
        response.counts.funko != expected.funko ||
        response.counts.thrilljoy != expected.thrilljoy ||
        response.counts.pins != expected.pins)
    {
        Serial.println(
            "E4 FAILED: count_items dispatch"
        );
        return false;
    }

    Serial.print(
        "PASS dispatcher count_items total="
    );
    Serial.println(
        response.counts.total
    );

    return true;
}

bool validateDispatcherFind()
{
    ToolRequest request;
    request.kind =
        ToolKind::FindItem;
    request.argument =
        "trl-pix-05881-hro";

    static ToolResponse response;

    executeToolRequestInto(
        request,
        response
    );

    if (response.status != ToolStatus::Ok ||
        response.record.id !=
            "TRL-PIX-05881-HRO" ||
        response.findTotalMatches != 1 ||
        response.findAmbiguous)
    {
        Serial.println(
            "E4 FAILED: exact find_item dispatch"
        );
        return false;
    }

    Serial.println(
        "PASS dispatcher exact find_item"
    );

    return true;
}

bool validateDispatcherList()
{
    ToolRequest request;
    request.kind =
        ToolKind::ListItems;
    request.argument =
        "Enamel Pins";
    request.limit = 2;

    static ToolResponse response;

    executeToolRequestInto(
        request,
        response
    );

    if (response.status != ToolStatus::Ok ||
        response.listCount != 2)
    {
        Serial.println(
            "E4 FAILED: list_items dispatch"
        );
        return false;
    }

    Serial.println(
        "PASS dispatcher list_items"
    );

    return true;
}

bool validateDispatcherNotFound()
{
    ToolRequest request;
    request.kind =
        ToolKind::FindItem;
    request.argument =
        "__RHEA_MUST_NOT_EXIST__";

    static ToolResponse response;

    executeToolRequestInto(
        request,
        response
    );

    if (response.status !=
        ToolStatus::NotFound)
    {
        Serial.println(
            "E4 FAILED: NotFound dispatch"
        );
        return false;
    }

    Serial.println(
        "PASS dispatcher deterministic NotFound"
    );

    return true;
}

bool validateToolDispatcher()
{
    Serial.println();
    Serial.println(
        "=== RHEA1-E4 TOOL DISPATCHER SELF-TESTS ==="
    );

    if (!validateDispatcherCount())
    {
        return false;
    }

    if (!validateDispatcherFind())
    {
        return false;
    }

    if (!validateDispatcherList())
    {
        return false;
    }

    if (!validateDispatcherNotFound())
    {
        return false;
    }

    Serial.println(
        "RHEA1-E4 TOOL DISPATCHER VALIDATION: PASS"
    );
    Serial.println(
        "========================================"
    );

    return true;
}

void runSearch()
{
    inputBuffer.trim();

    if (inputBuffer.length() == 0)
    {
        drawPrompt();
        return;
    }

    // Keep the idle avatar from replacing in-progress/result UI.
    // The full 120-second idle countdown starts when this command exits.
    CommandActivityScope commandActivityScope(
        inputBuffer
    );

    String lower =
        lowerCopy(inputBuffer);

    String commandText =
        normalizedCommandText(
            inputBuffer
        );

    const String aliasExpanded =
        applyLearnedAliases(
            commandText
        );

    if (aliasExpanded !=
        commandText)
    {
        Serial.print(
            "FLORA_LEARNED_ALIAS ["
        );
        Serial.print(commandText);
        Serial.print("] -> [");
        Serial.print(aliasExpanded);
        Serial.println("]");

        commandText =
            aliasExpanded;
    }

    lower =
        lowerCopy(commandText);

    const String commandLower =
        normalizedCommandLower(
            commandText
        );

    if (commandText != inputBuffer)
    {
        Serial.print(
            "COMMAND_NORMALIZED ["
        );
        Serial.print(inputBuffer);
        Serial.print("] -> [");
        Serial.print(commandText);
        Serial.println("]");
    }

    if (lower == "!!")
    {
        String recalled;

        if (!recallPreviousCommand(
                recalled))
        {
            Serial.println(
                "RHEA_HISTORY no previous command"
            );

            auto &display =
        floraDisplay;

            display.fillScreen(BLACK);
            display.setCursor(4, 4);
            display.setTextColor(WHITE, BLACK);
            display.setTextSize(1);
            display.setTextWrap(true);
            display.println("F L O R A");
            display.println("--------------------");
            display.println("No previous command yet.");
            display.println();
            display.println("[ENTER] Ask me something else");

            inputBuffer = "";
            return;
        }

        Serial.print("RHEA_HISTORY repeat: ");
        Serial.println(recalled);

        inputBuffer =
            recalled;

        lower =
            lowerCopy(inputBuffer);
    }

    if (lower != "history")
    {
        rememberCommand(
            inputBuffer
        );
    }

    Serial.print("Command: ");
    Serial.println(inputBuffer);

    String learnedAlias;
    String learnedReplacement;

    if (parseTeachAlias(
            inputBuffer,
            learnedAlias,
            learnedReplacement))
    {
        if (saveLearnedAlias(
                learnedAlias,
                learnedReplacement))
        {
            Serial.print(
                "FLORA_SAYS Got it. When you say \""
            );
            Serial.print(learnedAlias);
            Serial.print(
                "\", I'll treat it as \""
            );
            Serial.print(
                learnedReplacement
            );
            Serial.println("\".");

            auto &display =
        floraDisplay;

            display.fillScreen(BLACK);
            display.setCursor(4, 4);
            display.setTextSize(1);
            display.setTextWrap(true);
            display.setTextColor(
                GREEN,
                BLACK
            );
            display.println("F L O R A");
            display.println("--------------------");
            display.setTextColor(
                WHITE,
                BLACK
            );
            display.println("Learned:");
            display.print(learnedAlias);
            display.print(" = ");
            display.println(
                learnedReplacement
            );
        }
        else
        {
            showError(
                "I couldn't save that alias."
            );
        }

        inputBuffer = "";
        return;
    }

    if (pendingValueItemizationOffer)
    {
        if (commandLower == "yes" ||
            commandLower == "y" ||
            commandLower == "sure" ||
            commandLower == "itemize" ||
            commandLower == "itemize them" ||
            commandLower == "show them" ||
            commandLower == "show me")
        {
            const String query =
                pendingValueGroupQuery;

            pendingValueItemizationOffer =
                false;
            valueItemizationActive =
                true;
            valueItemizationOffset = 0;

            if (!showValueItemizationPage(
                    query,
                    0,
                    pendingValueGroupDomain))
            {
                showError(
                    "I couldn't itemize that value set."
                );
            }

            inputBuffer = "";
            return;
        }

        if (commandLower == "no" ||
            commandLower == "n" ||
            commandLower == "nope")
        {
            clearValueConversation();

            Serial.println(
                "FLORA_SAYS Okay - keeping it at the summary."
            );

            drawPrompt();
            inputBuffer = "";
            return;
        }

        // A new substantive command cancels the pending yes/no question.
        clearValueConversation();
    }

    if (valueItemizationActive)
    {
        if (commandLower == "more" ||
            commandLower == "next" ||
            commandLower == "next page")
        {
            const String query =
                pendingValueGroupQuery;

            if (!showValueItemizationPage(
                    query,
                    valueItemizationOffset,
                    pendingValueGroupDomain))
            {
                showError(
                    "I couldn't load the next value page."
                );
            }

            inputBuffer = "";
            return;
        }

        if (commandLower == "stop" ||
            commandLower == "done" ||
            commandLower == "cancel")
        {
            clearValueConversation();
            drawPrompt();
            inputBuffer = "";
            return;
        }

        clearValueConversation();
    }

    // FLORA1: manually entered RFID identifiers bypass the NL classifier.
    String rfidCandidate =
        commandText;

    if (commandLower.startsWith(
            "rfid "))
    {
        rfidCandidate =
            commandText.substring(5);
        rfidCandidate.trim();
    }

    String normalizedRfidLabel;
    String normalizedRfidEpc;

    if (normalizeRfidIdentifier(
            rfidCandidate,
            normalizedRfidLabel,
            normalizedRfidEpc))
    {
        static RfidInfo rfidMatch;
        bool validButUnassigned =
            false;

        if (toolFindByRfid(
                rfidCandidate,
                rfidMatch,
                validButUnassigned))
        {
            showRfidResult(
                rfidMatch
            );
        }
        else if (validButUnassigned)
        {
            Serial.print(
                "RFID_RESPONSE {\"status\":\"unassigned\",\"printedLabel\":\""
            );
            Serial.print(
                normalizedRfidLabel
            );
            Serial.println("\"}");

            Serial.print(
                "FLORA_SAYS "
            );
            Serial.print(
                normalizedRfidLabel
            );
            Serial.println(
                " is a valid tag, but it isn't assigned to an owned item."
            );
        }
        else
        {
            showError(
                "RFID lookup failed."
            );
        }

        inputBuffer = "";
        return;
    }

    // RHEA1-G1: barcode/UPC input bypasses the natural-language classifier.
    String upcCandidate =
        commandText;

    if (lower.startsWith("upc "))
    {
        upcCandidate =
            commandText.substring(4);
        upcCandidate.trim();
    }

    if (looksLikeUpc(
            upcCandidate))
    {
        static RecordInfo upcMatches[5];
        size_t returned = 0;
        size_t total = 0;

        const ToolStatus upcStatus =
            toolFindByUpc(
                upcCandidate,
                upcMatches,
                5,
                returned,
                total
            );

        Serial.print(
            "UPC_RESPONSE {\"upc\":\""
        );
        Serial.print(upcCandidate);
        Serial.print(
            "\",\"matches\":"
        );
        Serial.print(total);
        Serial.println("}");

        if (upcStatus ==
                ToolStatus::Ok)
        {
            showUpcMatches(
                upcCandidate,
                upcMatches,
                returned,
                total
            );
        }
        else
        {
            showError(
                "UPC not found."
            );
        }

        inputBuffer = "";
        return;
    }

    // FLORA1-CONVO1: deterministic conversational layer runs before the
    // older exact-phrase and statistical classifier paths.
    if (looksLikeRankValueQuestion(
            commandText))
    {
        bool explicitDomain = false;
        const ValueGroupDomain domain =
            conversationalDomainForInput(
                commandText,
                explicitDomain
            );

        String scope =
            extractRankScope(
                commandText
            );

        if (scope.length() < 2 &&
            conversationContext.valid)
        {
            scope =
                conversationContext.scope;

            Serial.print(
                "CONTEXT_REUSE source=rank scope=["
            );
            Serial.print(scope);
            Serial.print("] domain=");
            Serial.println(
                conversationContext.domain ==
                    ValueGroupDomain::Pins
                    ? "pins"
                    : "pops"
            );
        }

        if (scope.length() < 2)
        {
            showError(
                "Which collection should I rank?"
            );
        }
        else
        {
            const bool descending =
                commandLower.indexOf(
                    "least valuable") < 0 &&
                commandLower.indexOf(
                    "less valuable") < 0 &&
                commandLower.indexOf(
                    "lowest value") < 0;

            const bool includeIds =
                asksForIdFields(
                    commandText
                );

            showRankedValueGroup(
                scope,
                domain,
                extractRequestedLimit(
                    commandLower,
                    4
                ),
                descending,
                includeIds
            );
        }

        inputBuffer = "";
        return;
    }

    // FLORA1-CONVO2: resolve pronouns against the concrete result set from
    // the most recent ranking before attempting a new intent classification.
    if (isRecentRankIdFollowup(
            commandText))
    {
        showRecentRankIds();
        inputBuffer = "";
        return;
    }

    if (isContextValueItemizationFollowup(
            commandText))
    {
        if (recentRankContext.valid &&
            refersToRecentRankSet(
                commandText))
        {
            showRecentRankValues();
        }
        else if (!conversationContext.valid)
        {
            showError(
                "I don't have a collection scope to reuse yet."
            );
        }
        else
        {
            pendingValueGroupQuery =
                conversationContext.scope;
            pendingValueGroupDomain =
                conversationContext.domain;
            pendingValueItemizationOffer =
                false;
            valueItemizationActive =
                true;
            valueItemizationOffset = 0;

            if (!showValueItemizationPage(
                    conversationContext.scope,
                    0,
                    conversationContext.domain))
            {
                showError(
                    "I couldn't itemize that value set."
                );
            }
        }

        inputBuffer = "";
        return;
    }

    if (isContextValueFollowup(
            commandText))
    {
        if (recentRankContext.valid &&
            refersToRecentRankSet(
                commandText))
        {
            showRecentRankValueSummary();
        }
        else if (!conversationContext.valid)
        {
            showError(
                "I don't have a collection scope to reuse yet."
            );
        }
        else
        {
            showValueGroupSummary(
                conversationContext.scope,
                conversationContext.domain
            );
        }

        inputBuffer = "";
        return;
    }

    if (isContextCountFollowup(
            commandText))
    {
        if (recentRankContext.valid &&
            refersToRecentRankSet(
                commandText))
        {
            showRecentRankCount();
        }
        else
        {
            showContextCount();
        }

        inputBuffer = "";
        return;
    }

    if (commandLower == "most valuable item" ||
        commandLower == "what is my most valuable item" ||
        commandLower == "what's my most valuable item" ||
        commandLower == "most valuable pop" ||
        commandLower == "what is my most valuable pop" ||
        commandLower == "what's my most valuable pop")
    {
        const String scope =
            commandLower.indexOf("pop") >= 0
                ? "pops"
                : "collection";

        RecordInfo best;
        size_t valuedRecords = 0;

        if (scanMostValuable(
                scope,
                best,
                valuedRecords))
        {
            showRecord(best);

            Serial.print(
                "VALUE_TOP_RESPONSE {\"scope\":\""
            );
            Serial.print(scope);
            Serial.print(
                "\",\"valuedRecords\":"
            );
            Serial.print(valuedRecords);
            Serial.print(
                ",\"id\":\""
            );
            Serial.print(best.id);
            Serial.print(
                "\",\"currentValueUsd\":"
            );
            Serial.print(
                best.currentValueUsd,
                2
            );
            Serial.println("}");
        }
        else
        {
            showError(
                "No value data found."
            );
        }

        inputBuffer = "";
        return;
    }

    if (commandLower == "what is my collection worth" ||
        commandLower == "what's my collection worth" ||
        commandLower == "collection value" ||
        commandLower == "what are my pops worth" ||
        commandLower == "pop collection value")
    {
        const String scope =
            commandLower.indexOf("pop") >= 0
                ? "pops"
                : "collection";

        double totalValue = 0.0;
        size_t valuedVariants = 0;
        size_t valuedCopies = 0;

        if (scanCollectionValue(
                scope,
                totalValue,
                valuedVariants,
                valuedCopies))
        {
            auto &display =
        floraDisplay;

            display.fillScreen(BLACK);
            display.setCursor(4, 4);
            display.setTextSize(1);
            display.setTextWrap(true);

            display.setTextColor(GREEN, BLACK);
            display.println("F L O R A");
            display.println("--------------------");
            display.setTextColor(WHITE, BLACK);
            display.print("Known value: $");
            display.println(
                totalValue,
                2
            );
            display.print("Valued copies: ");
            display.println(
                valuedCopies
            );
            display.print("Valued variants: ");
            display.println(
                valuedVariants
            );
            display.println();
            display.println(
                "Unvalued items are excluded."
            );
            display.println();
            display.println(
                "[ENTER] Ask me something else"
            );

            Serial.print(
                "VALUE_TOTAL_RESPONSE {\"scope\":\""
            );
            Serial.print(scope);
            Serial.print(
                "\",\"knownValueUsd\":"
            );
            Serial.print(
                totalValue,
                2
            );
            Serial.print(
                ",\"valuedVariants\":"
            );
            Serial.print(
                valuedVariants
            );
            Serial.print(
                ",\"valuedCopies\":"
            );
            Serial.print(
                valuedCopies
            );
            Serial.println("}");
        }
        else
        {
            showError(
                "Value scan failed."
            );
        }

        inputBuffer = "";
        return;
    }

    if (looksLikeValueGroupQuestion(
            commandText))
    {
        const ValueGroupDomain valueDomain =
            valueGroupDomainForInput(
                commandText
            );

        const String valueQuery =
            extractValueGroupQuery(
                commandText
            );

        if (valueQuery.length() < 2)
        {
            showError(
                valueDomain == ValueGroupDomain::Pins
                    ? "Tell me which Pins to value."
                    : "Tell me which Pops to value."
            );
        }
        else
        {
            showValueGroupSummary(
                valueQuery,
                valueDomain
            );
        }

        inputBuffer = "";
        return;
    }

    if (looksLikeWorthQuestion(
            commandText))
    {
        const String valueQuery =
            extractWorthQuery(
                commandText
            );

        showValueForTitle(
            valueQuery
        );

        inputBuffer = "";
        return;
    }

    if (commandLower == "status")
    {
        showRheaStatus();
        inputBuffer = "";
        return;
    }

    if (commandLower == "history")
    {
        showCommandHistory();
        inputBuffer = "";
        return;
    }

    if (commandLower == "rfidtest")
    {
        Serial.println();
        Serial.println(
            "=== FLORA1 RFID SELF-TEST ==="
        );

        uint32_t sequence = 0;

        const bool friendlyPass =
            parseFriendlyRfid(
                "VFC-05001",
                sequence
            ) &&
            sequence == 5001;

        const bool epcPass =
            parseEpcRfid(
                "5646432D0000000000001389",
                sequence
            ) &&
            sequence == 5001;

        String label;
        String epc;

        const bool roundTripPass =
            normalizeRfidIdentifier(
                "VFC-05001",
                label,
                epc
            ) &&
            label == "VFC-05001" &&
            epc ==
                "5646432D0000000000001389";

        File file =
            SD.open(
                RHEA_SNAPSHOT_ACTIVE_PATH,
                FILE_READ
            );

        bool arrayPass = false;
        size_t assignedCount = 0;

        if (file)
        {
            arrayPass =
                locateRfidAssignmentsArray(
                    file
                );

            if (arrayPass)
            {
                while (true)
                {
                    RfidInfo rfid;

                    const RecordReadState state =
                        readNextRfidRecord(
                            file,
                            rfid
                        );

                    if (state ==
                        RecordReadState::EndOfArray)
                    {
                        break;
                    }

                    if (state ==
                        RecordReadState::Malformed)
                    {
                        arrayPass = false;
                        break;
                    }

                    ++assignedCount;
                }
            }

            file.close();
        }

        Serial.print("RFID friendly parse: ");
        Serial.println(friendlyPass ? "PASS" : "FAIL");
        Serial.print("RFID EPC parse: ");
        Serial.println(epcPass ? "PASS" : "FAIL");
        Serial.print("RFID round-trip: ");
        Serial.println(roundTripPass ? "PASS" : "FAIL");
        Serial.print("RFID snapshot index: ");
        Serial.println(arrayPass ? "PASS" : "FAIL");
        Serial.print("Assigned RFID records: ");
        Serial.println(assignedCount);

        const bool pass =
            friendlyPass &&
            epcPass &&
            roundTripPass &&
            arrayPass;

        Serial.print("FLORA1 RFID SELF-TEST: ");
        Serial.println(pass ? "PASS" : "FAIL");

        inputBuffer = "";
        return;
    }

    if (commandLower == "convotest")
    {
        struct Probe
        {
            const char *text;
            bool rankExpected;
            size_t limitExpected;
            const char *scopeExpected;
        };

        static const Probe probes[] =
        {
            {
                "can you show me the top 4 most valuable batman pops?",
                true,
                4,
                "batman"
            },
            {
                "what are my top 4 more valuable batman pops",
                true,
                4,
                "batman"
            },
            {
                "can you show mew the top 4 most valuable batman pops",
                true,
                4,
                "batman"
            },
            {
                "what are the top 4 ranked by value",
                true,
                4,
                ""
            },
            {
                "what are the top 4 most valuable ones?",
                true,
                4,
                ""
            },
            {
                "what are the titles and IDs of my top 4 most valuable Spider-Man pops?",
                true,
                4,
                "spider-man"
            },
            {
                "show me the top 3 least valuable good girl art pins",
                true,
                3,
                "good girl art"
            },
            {
                "batman value",
                false,
                4,
                "batman value"
            }
        };

        bool passed = true;

        for (const auto &probe : probes)
        {
            const bool isRank =
                looksLikeRankValueQuestion(
                    probe.text
                );

            const size_t limit =
                extractRequestedLimit(
                    normalizedCommandLower(
                        probe.text
                    ),
                    4
                );

            const String scope =
                extractRankScope(
                    probe.text
                );

            const bool ok =
                isRank ==
                    probe.rankExpected &&
                limit ==
                    probe.limitExpected &&
                scope ==
                    probe.scopeExpected;

            Serial.print(
                ok ? "PASS " : "FAIL "
            );
            Serial.println(probe.text);

            if (!ok)
            {
                passed = false;
            }
        }

        // Stateful regression: a scoped count must establish context, and a
        // referential ranking phrase must carry no replacement scope.
        ToolRequest contextProbe;
        contextProbe.kind =
            ToolKind::CountItems;
        contextProbe.countCollectibleType =
            "Funko";
        contextProbe.countProductTypePrefix =
            "Pop!";
        contextProbe.countSeriesNamePrefix =
            "batman";
        contextProbe.countAffiliationPrefix =
            "batman";

        const ConversationContext savedContext =
            conversationContext;

        conversationContext =
            ConversationContext();

        const bool remembered =
            rememberConversationFromCountRequest(
                contextProbe
            );

        const String referentialScope =
            extractRankScope(
                "what are the top 4 most valuable ones?"
            );

        const bool contextSequenceOk =
            remembered &&
            conversationContext.valid &&
            conversationContext.scope ==
                "batman" &&
            conversationContext.domain ==
                ValueGroupDomain::Pops &&
            referentialScope.length() == 0;

        const String explicitSwitchScope =
            extractRankScope(
                "what are the titles and IDs of my top 4 most valuable Spider-Man pops?"
            );

        const bool contextSwitchOk =
            explicitSwitchScope ==
                "spider-man";

        const bool eachValuesFollowupOk =
            isContextValueItemizationFollowup(
                "what are the values of each of those?"
            );

        Serial.print(
            contextSequenceOk
                ? "PASS "
                : "FAIL "
        );
        Serial.println(
            "count Batman -> top 4 most valuable ones"
        );

        if (!contextSequenceOk)
        {
            passed = false;
        }

        Serial.print(
            contextSwitchOk
                ? "PASS "
                : "FAIL "
        );
        Serial.println(
            "Batman context -> explicit Spider-Man rank + IDs"
        );

        if (!contextSwitchOk)
        {
            passed = false;
        }

        Serial.print(
            eachValuesFollowupOk
                ? "PASS "
                : "FAIL "
        );
        Serial.println(
            "count scope -> values of each of those"
        );

        if (!eachValuesFollowupOk)
        {
            passed = false;
        }

        conversationContext =
            savedContext;

        Serial.println(
            passed
                ? "FLORA1 CONVERSATIONAL ROUTER: PASS"
                : "FLORA1 CONVERSATIONAL ROUTER: FAIL"
        );

        inputBuffer = "";
        return;
    }

    if (commandLower == "g1test")
    {
        Serial.println();
        Serial.println(
            "=== FLORA1 DATA/VALUE SELF-TEST ==="
        );

        EnvelopeInfo envelope;
        size_t bytes = 0;

        const bool snapshotOk =
            readActiveSnapshotSummary(
                envelope,
                bytes
            );

        const bool schemaOk =
            snapshotOk &&
            envelope.schemaVersion == 3;

        Serial.print("FLORA1 schema v3: ");
        Serial.println(
            schemaOk
                ? "PASS"
                : "FAIL"
        );

        double totalValue = 0.0;
        size_t valuedVariants = 0;
        size_t valuedCopies = 0;

        const bool valueScanOk =
            schemaOk &&
            scanCollectionValue(
                "collection",
                totalValue,
                valuedVariants,
                valuedCopies
            );

        Serial.print(
            "G1 value scan: "
        );
        Serial.println(
            valueScanOk
                ? "PASS"
                : "FAIL"
        );

        Serial.print(
            "G1 valued variants: "
        );
        Serial.println(
            valuedVariants
        );

        Serial.print(
            "G1 valued copies: "
        );
        Serial.println(
            valuedCopies
        );

        RecordInfo topPop;
        size_t valuedPops = 0;

        const bool topPopOk =
            schemaOk &&
            scanMostValuable(
                "pops",
                topPop,
                valuedPops
            );

        Serial.print(
            "G1 top Pop scan: "
        );
        Serial.println(
            topPopOk
                ? "PASS"
                : "FAIL"
        );

        const bool passed =
            schemaOk &&
            valueScanOk &&
            topPopOk;

        Serial.println(
            passed
                ? "FLORA1 DATA/VALUE SELF-TEST: PASS"
                : "FLORA1 DATA/VALUE SELF-TEST: FAIL"
        );
        Serial.println(
            "==================================="
        );

        auto &display =
        floraDisplay;

        display.fillScreen(BLACK);
        display.setCursor(4, 4);
        display.setTextColor(
            passed
                ? WHITE
                : RED,
            BLACK
        );
        display.setTextSize(1);
        display.setTextWrap(true);
        display.println("F L O R A");
        display.println("--------------------");
        display.println(
            passed
                ? "G1 data/value: PASS"
                : "G1 data/value: FAIL"
        );
        display.print("Valued variants: ");
        display.println(valuedVariants);
        display.print("Valued copies: ");
        display.println(valuedCopies);
        display.println();
        display.println(
            "[ENTER] New command"
        );

        inputBuffer = "";
        return;
    }

    if (commandLower == "f6test")
    {
        const bool passed =
            validateF6Features();

        auto &display =
        floraDisplay;

        display.fillScreen(BLACK);
        display.setCursor(4, 4);
        display.setTextColor(
            passed
                ? WHITE
                : RED,
            BLACK
        );
        display.setTextSize(1);
        display.setTextWrap(true);

        display.println("F L O R A");
        display.println("--------------------");
        display.println(
            passed
                ? "F6 usability: PASS"
                : "F6 usability: FAIL"
        );
        display.println();
        display.println("See Serial output.");
        display.println();
        display.println("[ENTER] New command");

        inputBuffer = "";
        return;
    }

    if (lower == "idletest")
    {
        inputBuffer = "";
        activateIdleFace();

        Serial.println(
            "RHEA1-F5B IDLE FACE: PASS"
        );

        return;
    }

    if (lower == "personatest")
    {
        auto &display =
        floraDisplay;

        display.fillScreen(BLACK);
        display.setCursor(4, 4);
        display.setTextColor(GREEN, BLACK);
        display.setTextSize(1);
        display.setTextWrap(true);

        display.println("F L O R A");
        display.println("Poppy's pocket sister");
        display.println("--------------------");
        display.println("Hi! I'm Flora.");
        display.println();
        display.println("Cute, concise,");
        display.println("collection-savvy,");
        display.println("and offline-first.");
        display.println();
        display.println("[ENTER] Ask me something");

        Serial.println(
            "RHEA1-F5A PERSONA PRESENTATION: PASS"
        );

        inputBuffer = "";
        return;
    }

    if (commandLower == "tooltest")
    {
        const bool passed =
            validateToolDispatcher();

        auto &display =
        floraDisplay;

        display.fillScreen(BLACK);
        display.setCursor(4, 4);
        display.setTextColor(GREEN, BLACK);
        display.setTextSize(1);
        display.setTextWrap(true);

        display.println("F L O R A");
        display.println("--------------------");
        display.println(
            passed
                ? "E4 dispatcher: PASS"
                : "E4 dispatcher: FAIL"
        );
        display.println();
        display.println("[ENTER] New command");

        inputBuffer = "";
        return;
    }

    if (commandLower == "probtest")
    {
        const bool passed =
            validateAdversarialRouting();

        auto &display =
        floraDisplay;

        display.fillScreen(BLACK);
        display.setCursor(4, 4);
        display.setTextColor(
            passed
                ? GREEN
                : RED,
            BLACK
        );
        display.setTextSize(1);
        display.setTextWrap(true);

        display.println("F L O R A");
        display.println("--------------------");
        display.println(
            passed
                ? "F4F probe: PASS"
                : "F4F probe: REVIEW"
        );
        display.println();
        display.println("See Serial output.");
        display.println();
        display.println("[ENTER] New command");

        inputBuffer = "";
        return;
    }

    if (lower.startsWith("probe "))
    {
        String probeText =
            inputBuffer.substring(6);

        probeText.trim();

        IntentClassification classification;

        const ToolRequest request =
            naturalLanguageToToolRequest(
                probeText,
                &classification
            );

        Serial.println();
        Serial.println(
            "=== RHEA1-F4F NON-EXECUTING NL PROBE ==="
        );
        Serial.print("Input: ");
        Serial.println(probeText);
        Serial.print("Classifier: ");
        Serial.println(classification.label);
        Serial.print("Best score: ");
        Serial.println(
            classification.bestScore,
            4
        );
        Serial.print("Margin: ");
        Serial.println(
            classification.bestScore -
            classification.secondScore,
            4
        );
        Serial.print("Tool: ");
        Serial.println(
            toolKindName(
                request.kind
            )
        );

        if (request.argument.length() > 0)
        {
            Serial.print("Argument: ");
            Serial.println(
                request.argument
            );
        }

        if (request.kind ==
            ToolKind::ListItems)
        {
            Serial.print("Limit: ");
            Serial.println(
                request.limit
            );
        }

        if (request.kind ==
            ToolKind::CountItems)
        {
            const String scopeLabel =
                countScopeLabel(
                    request
                );

            if (scopeLabel.length() > 0)
            {
                Serial.print("Scope: ");
                Serial.println(scopeLabel);
            }
        }

        Serial.println(
            "EXECUTION: BLOCKED (probe mode)"
        );
        Serial.println(
            "========================================"
        );

        auto &display =
        floraDisplay;

        display.fillScreen(BLACK);
        display.setCursor(4, 4);
        display.setTextColor(GREEN, BLACK);
        display.setTextSize(1);
        display.setTextWrap(true);

        display.println("F L O R A");
        display.println("--------------------");
        display.println("Probe only");
        display.print("Class: ");
        display.println(
            classification.label
        );
        display.print("Tool: ");
        display.println(
            toolKindName(
                request.kind
            )
        );
        display.print("Margin: ");
        display.println(
            classification.bestScore -
            classification.secondScore,
            3
        );
        display.println();
        display.println("Nothing executed.");
        display.println();
        display.println("[ENTER] New command");

        inputBuffer = "";
        return;
    }

    if (lower == "nltest")
    {
        const bool passed =
            validateNaturalLanguageRouter();

        auto &display =
        floraDisplay;

        display.fillScreen(BLACK);
        display.setCursor(4, 4);
        display.setTextColor(GREEN, BLACK);
        display.setTextSize(1);
        display.setTextWrap(true);

        display.println("F L O R A");
        display.println("--------------------");
        display.println(
            passed
                ? "F4K router: PASS"
                : "F4K router: FAIL"
        );
        display.println();
        display.println("[ENTER] New command");

        inputBuffer = "";
        return;
    }

    if (lower.startsWith("tool "))
    {
        String remainder =
            inputBuffer.substring(5);

        remainder.trim();

        String remainderLower =
            lowerCopy(remainder);

        ToolRequest request;

        if (remainderLower == "count_items")
        {
            request.kind =
                ToolKind::CountItems;
        }
        else if (remainderLower == "update_records")
        {
            request.kind =
                ToolKind::UpdateRecords;
        }
        else if (remainderLower.startsWith("find_item "))
        {
            request.kind =
                ToolKind::FindItem;
            request.argument =
                remainder.substring(10);
            request.argument.trim();
        }
        else if (remainderLower.startsWith("list_items "))
        {
            request.kind =
                ToolKind::ListItems;

            String arg =
                remainder.substring(11);
            arg.trim();

            int lastSpace =
                arg.lastIndexOf(' ');

            if (lastSpace > 0)
            {
                const String possibleLimit =
                    arg.substring(lastSpace + 1);

                bool numeric = true;

                for (size_t i = 0;
                     i < possibleLimit.length();
                     ++i)
                {
                    if (!isDigit(
                            possibleLimit[i]))
                    {
                        numeric = false;
                        break;
                    }
                }

                if (numeric &&
                    possibleLimit.length() > 0)
                {
                    const int parsed =
                        possibleLimit.toInt();

                    if (parsed > 0)
                    {
                        request.limit =
                            static_cast<size_t>(
                                min(parsed, 5)
                            );

                        arg =
                            arg.substring(
                                0,
                                lastSpace
                            );

                        arg.trim();
                    }
                }
            }

            const String typeLower =
                lowerCopy(arg);

            if (typeLower == "pins" ||
                typeLower == "pin")
            {
                arg = "Enamel Pins";
            }
            else if (typeLower == "funko")
            {
                arg = "Funko";
            }
            else if (typeLower == "thrilljoy")
            {
                arg = "Thrilljoy";
            }

            request.argument = arg;
        }

        const ToolResponse response =
            executeToolRequest(
                request
            );

        printToolResponseJson(
            request,
            response
        );

        if (request.kind == ToolKind::FindItem &&
            response.status == ToolStatus::Ok &&
            response.findAmbiguous)
        {
            showFindAmbiguity(
                request,
                response.findMatches,
                response.findMatchCount,
                response.findTotalMatches
            );
        }
        else if (request.kind == ToolKind::FindItem &&
                 response.status == ToolStatus::Ok)
        {
            showRecord(
                response.record
            );
        }
        else if (request.kind == ToolKind::CountItems &&
                 response.status == ToolStatus::Ok)
        {
            rememberConversationFromCountRequest(
                request
            );

            showCountResult(
                response.counts,
                request
            );
        }
        else if (request.kind == ToolKind::ListItems &&
                 response.status == ToolStatus::Ok)
        {
            showListResult(
                request.argument,
                const_cast<RecordInfo *>(
                    response.list
                ),
                response.listCount
            );
        }
        else
        {
            auto &display =
        floraDisplay;

            display.fillScreen(BLACK);
            display.setCursor(4, 4);
            display.setTextColor(GREEN, BLACK);
            display.setTextSize(1);
            display.setTextWrap(true);

            display.println("F L O R A");
            display.println("--------------------");
            display.print("Tool status: ");
            display.println(
                toolStatusName(
                    response.status
                )
            );
            display.println();
            display.println("[ENTER] New command");
        }

        inputBuffer = "";
        return;
    }

    if (commandLower == "wifi")
    {
        promptForTemporaryWifi();

        inputBuffer = "";
        return;
    }

    if (commandLower == "wifi forget")
    {
        forgetTemporaryWifi();

        inputBuffer = "";
        return;
    }

    if (lower == "update" ||
        lower == "update records")
    {
        const bool updated =
            performOnDemandUpdate();

        delay(
            updated
                ? 1800
                : 3500
        );

        inputBuffer = "";
        drawPrompt();
        return;
    }

    if (commandLower == "diagnostics")
    {
        runF6Diagnostics();

        inputBuffer = "";
        return;
    }

    if (lower == "count" ||
        lower == "count_items")
    {
        CountResult counts;

        const ToolStatus status =
            toolCountItems(counts);

        if (status == ToolStatus::Ok)
        {
            showCountResult(counts);
        }
        else
        {
            printBoth(
                "count_items FAILED"
            );
        }

        inputBuffer = "";
        return;
    }

    // Legacy explicit list command. Only intercept the old strict syntax:
    //   list pins
    //   list funko
    //   list thrilljoy
    //
    // Natural-language phrases such as "list my enamel pins" must fall
    // through to the F4B classifier instead of being treated literally as
    // collectible type "my enamel pins".
    if (lower.startsWith("list "))
    {
        String legacyType =
            inputBuffer.substring(5);

        legacyType.trim();

        const String legacyTypeLower =
            lowerCopy(legacyType);

        bool isLegacyListCommand = false;

        if (legacyTypeLower == "pins" ||
            legacyTypeLower == "pin" ||
            legacyTypeLower == "enamel pins")
        {
            legacyType = "Enamel Pins";
            isLegacyListCommand = true;
        }
        else if (legacyTypeLower == "thrilljoy")
        {
            legacyType = "Thrilljoy";
            isLegacyListCommand = true;
        }
        else if (legacyTypeLower == "funko")
        {
            legacyType = "Funko";
            isLegacyListCommand = true;
        }

        if (isLegacyListCommand)
        {
            RecordInfo results[5];
            size_t returned = 0;

            const ToolStatus status =
                toolListItems(
                    legacyType,
                    5,
                    results,
                    5,
                    returned
                );

            if (status == ToolStatus::Ok)
            {
                showListResult(
                    legacyType,
                    results,
                    returned
                );
            }
            else
            {
                auto &display =
        floraDisplay;

                display.fillScreen(BLACK);
                display.setCursor(4, 4);
                display.setTextColor(GREEN, BLACK);
                display.setTextSize(1);
                display.setTextWrap(true);

                display.println("F L O R A");
                display.println("--------------------");
                display.println("No list results.");
                display.print("Type: ");
                display.println(legacyType);
                display.println();
                display.println("[ENTER] New command");
            }

            inputBuffer = "";
            return;
        }

        // Otherwise continue into the natural-language routing path below.
    }

    // Preserve the explicit "find <query>" command, now with F6 ambiguity
    // handling instead of silently choosing the first title match.
    if (lower.startsWith("find "))
    {
        String query =
            inputBuffer.substring(5);

        query.trim();

        ToolRequest request;
        request.kind =
            ToolKind::FindItem;
        request.argument =
            query;

        const ToolResponse response =
            executeToolRequest(
                request
            );

        printToolResponseJson(
            request,
            response
        );

        if (response.status ==
                ToolStatus::Ok &&
            response.findAmbiguous)
        {
            showFindAmbiguity(
                request,
                response.findMatches,
                response.findMatchCount,
                response.findTotalMatches
            );
        }
        else if (response.status ==
                     ToolStatus::Ok)
        {
            showRecord(
                response.record
            );
        }
        else
        {
            auto &display =
        floraDisplay;

            display.fillScreen(BLACK);
            display.setCursor(4, 4);
            display.setTextColor(WHITE, BLACK);
            display.setTextSize(1);
            display.setTextWrap(true);

            display.println("F L O R A");
            display.println("--------------------");

            if (response.status ==
                ToolStatus::InvalidArgument)
            {
                display.println("Query too short.");
            }
            else if (response.status ==
                     ToolStatus::NotFound)
            {
                display.println("No match found.");
            }
            else
            {
                display.println("Snapshot read error.");
            }

            display.println();
            display.print("Query: ");
            display.println(query);
            display.println();
            display.println("[ENTER] New command");
        }

        inputBuffer = "";
        return;
    }

    if (commandLower.startsWith(
            "rfid "))
    {
        Serial.println(
            "RFID_RESPONSE {\"status\":\"invalid\"}"
        );
        Serial.println(
            "FLORA_SAYS That doesn't look like a valid VFC tag or 96-bit EPC."
        );
        inputBuffer = "";
        return;
    }

    // RHEA1-F4B natural-language path.
    IntentClassification classification;

    const ToolRequest request =
        naturalLanguageToToolRequest(
            inputBuffer,
            &classification
        );

    Serial.print("NL_INTENT label=");
    Serial.print(classification.label);
    Serial.print(" best=");
    Serial.print(
        classification.bestScore,
        4
    );
    Serial.print(" margin=");
    Serial.print(
        classification.bestScore -
        classification.secondScore,
        4
    );
    Serial.print(" tool=");
    Serial.print(
        toolKindName(
            request.kind
        )
    );

    if (request.argument.length() > 0)
    {
        Serial.print(" arg=[");
        Serial.print(
            request.argument
        );
        Serial.print("]");
    }

    if (request.kind ==
        ToolKind::ListItems)
    {
        Serial.print(" limit=");
        Serial.print(
            request.limit
        );
    }

    if (request.kind ==
        ToolKind::CountItems)
    {
        const String scopeLabel =
            countScopeLabel(
                request
            );

        if (scopeLabel.length() > 0)
        {
            Serial.print(" scope=[");
            Serial.print(scopeLabel);
            Serial.print("]");
        }
    }

    Serial.println();

    if (request.kind ==
        ToolKind::Unknown)
    {
        auto &display =
        floraDisplay;

        display.fillScreen(BLACK);
        display.setCursor(4, 4);
        display.setTextColor(GREEN, BLACK);
        display.setTextSize(1);
        display.setTextWrap(true);

        display.println("F L O R A");
        display.println("--------------------");
        display.println("Hmm - I can't do that");
        display.println("offline yet.");
        display.println();
        display.println("I can find, count,");
        display.println("list, update records, or wifi.");
        display.println();
        display.println("[ENTER] Ask me something else");

        Serial.println(
            "FLORA_SAYS Hmm - I can't do that offline yet. I can find, count, list, or update records."
        );

        inputBuffer = "";
        return;
    }

    const ToolResponse response =
        executeToolRequest(
            request
        );

    printToolResponseJson(
        request,
        response
    );

    if (request.kind ==
            ToolKind::FindItem &&
        response.status ==
            ToolStatus::Ok &&
        response.findAmbiguous)
    {
        showFindAmbiguity(
            request,
            response.findMatches,
            response.findMatchCount,
            response.findTotalMatches
        );
    }
    else if (request.kind ==
                 ToolKind::FindItem &&
             response.status ==
                 ToolStatus::Ok)
    {
        showRecord(
            response.record
        );
    }
    else if (request.kind ==
                 ToolKind::CountItems &&
             response.status ==
                 ToolStatus::Ok)
    {
        rememberConversationFromCountRequest(
            request
        );

        showCountResult(
            response.counts,
            request
        );
    }
    else if (request.kind ==
                 ToolKind::ListItems &&
             response.status ==
                 ToolStatus::Ok)
    {
        showListResult(
            request.argument,
            const_cast<RecordInfo *>(
                response.list
            ),
            response.listCount
        );
    }
    else if (request.kind ==
                 ToolKind::UpdateRecords)
    {
        delay(
            response.updateSucceeded
                ? 1800
                : 3500
        );

        drawPrompt();
    }
    else
    {
        auto &display =
        floraDisplay;

        display.fillScreen(BLACK);
        display.setCursor(4, 4);
        display.setTextColor(GREEN, BLACK);
        display.setTextSize(1);
        display.setTextWrap(true);

        display.println("F L O R A");
        display.println("--------------------");
        display.print("Tool status: ");
        display.println(
            toolStatusName(
                response.status
            )
        );

        if (request.argument.length() > 0)
        {
            display.println();
            display.print("Argument: ");
            display.println(
                request.argument
            );
        }

        display.println();
        display.println("[ENTER] New command");
    }

    inputBuffer = "";
}

void processSerialInput()
{
    while (Serial.available() > 0)
    {
        const int raw = Serial.read();

        if (raw < 0)
        {
            return;
        }

        const char c =
            static_cast<char>(raw);

        noteUserActivity();

        if (c == '\r')
        {
            continue;
        }

        if (c == '\n')
        {
            serialInputBuffer.trim();

            if (serialInputBuffer.length() > 0)
            {
                inputBuffer =
                    serialInputBuffer;

                serialInputBuffer = "";

                runSearch();
            }
            else
            {
                serialInputBuffer = "";
            }

            continue;
        }

        if (c == 8 ||
            c == 127)
        {
            if (serialInputBuffer.length() > 0)
            {
                serialInputBuffer.remove(
                    serialInputBuffer.length() - 1
                );
            }

            continue;
        }

        if (c >= 32 &&
            c <= 126 &&
            serialInputBuffer.length() <
                MAX_QUERY_LENGTH)
        {
            serialInputBuffer += c;
        }
    }
}

void processKeyboardEdges()
{
    bool currentKeyDown[4][14] = {};

    const auto &keys =
        M5Cardputer.Keyboard.keyList();

    Keyboard_Class::KeysState state =
        M5Cardputer.Keyboard.keysState();

    bool inputChanged = false;
    bool submitted = false;

    for (const auto &keyPos : keys)
    {
        if (keyPos.x < 0 ||
            keyPos.x >= 14 ||
            keyPos.y < 0 ||
            keyPos.y >= 4)
        {
            continue;
        }

        currentKeyDown[keyPos.y][keyPos.x] =
            true;

        if (previousKeyDown[keyPos.y][keyPos.x])
        {
            continue;
        }

        noteUserActivity();

        const KeyValue_t keyValue =
            M5Cardputer.Keyboard.getKeyValue(
                keyPos
            );

        const uint8_t baseCode =
            keyValue.value_first;

        if (baseCode == KEY_FN ||
            baseCode == KEY_OPT ||
            baseCode == KEY_LEFT_CTRL ||
            baseCode == KEY_LEFT_SHIFT ||
            baseCode == KEY_LEFT_ALT)
        {
            continue;
        }

        // Cardputer Fn layer: Fn+; = Up, Fn+. = Down.
        if (state.fn &&
            baseCode == ';')
        {
            floraDisplay.scrollUp(
                inputBuffer
            );
            continue;
        }

        if (state.fn &&
            baseCode == '.')
        {
            floraDisplay.scrollDown(
                inputBuffer
            );
            continue;
        }

        if (state.fn)
        {
            continue;
        }

        if (baseCode == KEY_ENTER)
        {
            if (!submitted)
            {
                Serial.println();
                runSearch();
                submitted = true;
            }

            continue;
        }

        if (baseCode == KEY_BACKSPACE ||
            baseCode == KEY_DELETE)
        {
            if (inputBuffer.length() > 0)
            {
                inputBuffer.remove(
                    inputBuffer.length() - 1
                );

                inputChanged = true;
            }

            continue;
        }

        const uint8_t resolvedCode =
            M5Cardputer.Keyboard.getKey(
                keyPos
            );

        if (resolvedCode >= 32 &&
            resolvedCode <= 126)
        {
            if (inputBuffer.length() <
                MAX_QUERY_LENGTH)
            {
                inputBuffer +=
                    static_cast<char>(
                        resolvedCode
                    );

                inputChanged = true;
            }
        }
    }

    for (int y = 0; y < 4; ++y)
    {
        for (int x = 0; x < 14; ++x)
        {
            previousKeyDown[y][x] =
                currentKeyDown[y][x];
        }
    }

    if (inputChanged && !submitted)
    {
        drawPrompt();
    }
}

void setup()
{
    Serial.begin(115200);
    delay(350);

    auto cfg = M5.config();
    M5Cardputer.begin(cfg);

    SPI.begin(
        SD_SPI_SCK_PIN,
        SD_SPI_MISO_PIN,
        SD_SPI_MOSI_PIN,
        SD_SPI_CS_PIN
    );

    initializeExternalMirror();

    auto &display =
        floraDisplay;

    display.setRotation(1);
    display.fillScreen(BLACK);
    display.setTextColor(GREEN, BLACK);
    display.setTextSize(1);
    display.setTextWrap(true);
    display.setCursor(4, 4);

    printBoth("R H E A");
    printBoth("RHEA1-F4B natural-language router");
    printBoth("--------------------");

    Serial.print("SD SPI frequency: ");
    Serial.println(SD_SPI_FREQUENCY_HZ);

    if (!SD.begin(
            SD_SPI_CS_PIN,
            SPI,
            SD_SPI_FREQUENCY_HZ))
    {
        printBoth("SD mount FAILED");
        return;
    }

    printBoth("SD mount OK");

    // Production startup rule:
    // Never enable Wi-Fi and never contact Azure automatically.
    WiFi.mode(WIFI_OFF);

    if (!quickSnapshotReadyCheck())
    {
        printBoth("Snapshot unavailable.");
        printBoth("Use: update");
        inputBuffer = "";
        drawPrompt();
        return;
    }

    Serial.println();
    Serial.println(
        "RHEA STARTUP READY: offline snapshot available."
    );
    Serial.println(
        "No network request was made at startup."
    );
    Serial.println(
        "Commands: natural language | probe <text> | probtest | nltest | update"
    );

    lastUserActivityMillis =
        millis();

    drawPrompt();
}

void loop()
{
    processSerialInput();
    M5Cardputer.update();
    processKeyboardEdges();
    updateIdleFace();
    delay(5);
}
