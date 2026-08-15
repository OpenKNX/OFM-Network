#pragma once

// Compiled in as soon as one of the two consumers is -- GroupMonitor (WebSocket) or
// MQTT (knx/tp/telegramm) -- and the device actually has a TP-UART to snoop.
// Everything that depends on the decoder keys off this one macro instead of
// repeating the condition.
#if (defined(OPENKNX_WEBMONITOR) || defined(OPENKNX_MQTT)) && (MASK_VERSION == 0x07B0 || MASK_VERSION == 0x091A) && (defined(KNX_IP_WIFI) || defined(KNX_IP_LAN))
#define OPENKNX_TELEGRAMJSON
#endif

#ifdef OPENKNX_TELEGRAMJSON

#include "OpenKNX/Format/JSON/Writer.h"

namespace TPUart
{
    class Frame;
}

namespace OpenKNX
{
    namespace Network
    {

        // Decodes a TP-UART frame into json (src/dst/ga/apci/len/hex/flags, plus a
        // value object when the GA table resolves a DPT for the destination address).
        // Shared by GroupMonitor (WebSocket) and MQTT::Module (knx/tp/telegramm).
        // Caller is responsible for the frame.isFrame() check and for reset()ing json.
        void buildTelegramJson(TPUart::Frame &frame, OpenKNX::Format::JSON::Writer &json);

    } // namespace Network
} // namespace OpenKNX

#endif
