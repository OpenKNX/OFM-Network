#include "OpenKNX/Network/TelegramJson.h"
#ifdef OPENKNX_TELEGRAMJSON

#include "OpenKNX.h"
#include "OpenKNX/Network/Module.h"
#include "OpenKNX/Charset.hpp"

#include <knx/dptconvert.h> // KNX_Decode_Value, Dpt, KNXValue (bereits gelinkt)

#include <cstdio>
#include <cstring>
#include <string>

namespace OpenKNX
{
    namespace Network
    {
        // Unpadded 1.2.3 / 1/2/3 -- frame.human*() pads to 01/01/003 and returns std::string by value.
        static void formatAddr(char *out, size_t outLen, uint16_t addr, bool group)
        {
            if (group)
                snprintf(out, outLen, "%u/%u/%u", (addr >> 11) & 0x1F, (addr >> 8) & 0x07, addr & 0xFF);
            else
                snprintf(out, outLen, "%u.%u.%u", (addr >> 12) & 0x0F, (addr >> 8) & 0x0F, addr & 0xFF);
        }

        static void toHex(const char *d, uint16_t from, uint16_t count, uint16_t total,
                          char *out, size_t outLen)
        {
            out[0] = '\0';
            int n = 0;
            for (uint16_t i = 0; i < count && (from + i) < total && n < (int)outLen - 2; i++)
                n += snprintf(out + n, outLen - n, "%02X", (uint8_t)d[from + i]);
        }

        // out stays empty for an unknown or unsupported DPT -- caller then shows the raw bytes.
        static void decodeValue(uint8_t dpt, uint8_t sub, const char *d, uint16_t meta,
                                uint8_t len, uint16_t total, char *out, size_t outLen)
        {
            out[0] = '\0';

            uint8_t buf[16] = {};
            size_t vlen;
            if (len <= 1)
            {
                // <=6-bit value lives in the APCI low octet (DPT 1/2/3)
                if (meta < 1 || meta > total) return;
                buf[0] = (uint8_t)d[meta - 1] & 0x3F;
                vlen = 1;
            }
            else
            {
                // apduSize() comes from the length field and can exceed the buffer on a mangled frame
                vlen = (size_t)(len - 1);
                if (vlen > sizeof(buf)) vlen = sizeof(buf);
                if (meta >= total) return;
                const size_t avail = (size_t)(total - meta);
                if (vlen > avail) vlen = avail;
                if (vlen == 0) return;
                for (size_t i = 0; i < vlen; i++)
                    buf[i] = (uint8_t)d[meta + i];
            }

            KNXValue v("");
            Dpt type(dpt, sub);
            if (!KNX_Decode_Value(buf, vlen, type, v)) return;

            switch (dpt)
            {
                case 1: snprintf(out, outLen, "%u", (bool)v ? 1u : 0u); break;
                case 5:
                    if (sub == 1) // 5.001: Stack liefert roh 0–255 → Prozent
                        snprintf(out, outLen, "%u%%", (unsigned)(((uint16_t)(uint8_t)v * 100 + 127) / 255));
                    else
                        snprintf(out, outLen, "%u", (unsigned)(uint8_t)v);
                    break;
                case 6: snprintf(out, outLen, "%d", (int)(int8_t)v); break;
                case 7: snprintf(out, outLen, "%u", (unsigned)(uint16_t)v); break;
                case 8: snprintf(out, outLen, "%d", (int)(int16_t)v); break;
                case 9:
                case 14: snprintf(out, outLen, "%.2f", (double)v); break;
                case 12: snprintf(out, outLen, "%lu", (unsigned long)(uint32_t)v); break;
                case 13: snprintf(out, outLen, "%ld", (long)(int32_t)v); break;
                case 16: snprintf(out, outLen, "%s", (const char *)v); break;
                case 17:
                case 18: snprintf(out, outLen, "%u", (unsigned)(uint8_t)v); break;
                default: out[0] = '\0'; break;
            }
        }

        void buildTelegramJson(TPUart::Frame &frame, OpenKNX::Format::JSON::Writer &json)
        {
            const char *d = frame.data();
            const uint16_t total = frame.size();
            const uint8_t meta = frame.metadataSize(); // 8 (standard) / 9 (extended), CRC included
            const bool ga = frame.isGroupAddress();
            const uint8_t len = frame.apduSize();      // payload bytes from meta-1 on

            // ASCII placeholder, not an em dash -- would clash with the Latin-15->UTF-8 conversion below
            const char *apci = "-";
            bool hasValue = false; // only Write/Response carry one
            if (ga && total >= meta)
            {
                const uint16_t a = (((uint8_t)d[meta - 2] & 0x03) << 8) | (uint8_t)d[meta - 1];
                switch (a & 0x03C0)
                {
                    case 0x000: apci = "Read"; break;
                    case 0x040: apci = "Response"; hasValue = true; break;
                    case 0x080: apci = "Write"; hasValue = true; break;
                    default: apci = "other"; break;
                }
            }

            const uint16_t src = frame.source();
            const uint16_t dst = frame.destination();
            char dptStr[12] = {};
            char val[40] = {};
            if (ga && hasValue)
            {
                uint8_t dptMain, dptSub;
                if (openknxNetwork.gatable.getDpt(dst, dptMain, dptSub) && dptSub > 0)
                {
                    snprintf(dptStr, sizeof(dptStr), "%u.%03u", dptMain, dptSub);
                    decodeValue(dptMain, dptSub, d, meta, len, total, val, sizeof(val));
                }
            }

            char srcText[12], dstText[12];
            formatAddr(srcText, sizeof(srcText), src, false);
            formatAddr(dstText, sizeof(dstText), dst, ga);

            // One buffer for both hex fields -- field() copies, so it can be refilled for raw below
            char hex[560];
            const uint16_t start = (meta >= 1) ? (meta - 1) : 0;
            toHex(d, start, len, total, hex, sizeof(hex)); // APDU, from the APCI octet on

            // DPT16 arrives as ISO-8859-15 from the bus, WS and MQTT want UTF-8
            std::string valUtf8;
            if (val[0])
            {
                valUtf8.reserve(sizeof(val) * 2);
                OpenKNX::Charset::encodeUtf8(valUtf8, val, strlen(val));
            }

            json.beginObject()
                .key("src")
                .beginObject()
                .field("addr", (uint32_t)src)
                .field("text", srcText)
                .endObject()
                .key("dst")
                .beginObject()
                .field("addr", (uint32_t)dst)
                .field("text", dstText)
                .endObject()
                .field("ga", ga)
                .field("apci", apci)
                .field("len", (uint32_t)len)
                .field("hex", hex);

            // Full frame -- control byte, addresses, CRC included -- for anyone who
            // wants to decode it themselves rather than parse hex's APCI-tangled first byte
            toHex(d, 0, total, total, hex, sizeof(hex));
            json.field("raw", hex);

            // Without a DPT the payload cannot be interpreted, so the block is left out entirely
            if (dptStr[0])
                json.key("value")
                    .beginObject()
                    .field("dpt", dptStr)
                    .field("text", valUtf8)
                    .endObject();

            json.key("flags")
                .beginObject()
                .field("transmitted", frame.isTransmitted())
                .field("addressed", frame.isAddressed())
                .field("invalid", frame.isInvalid())
                .field("extended", frame.isExtended())
                .field("repeated", frame.isRepeated())
                .field("filtered", frame.isFiltered())
                .field("busy", frame.isBusy())
                .field("nack", frame.isNack())
                .field("ack", frame.isAck())
                .endObject()
                .endObject();
        }

    } // namespace Network
} // namespace OpenKNX

#endif
