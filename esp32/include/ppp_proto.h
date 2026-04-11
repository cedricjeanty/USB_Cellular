#pragma once
// Minimal PPP protocol implementation — HDLC framing, LCP, IPCP
// Used by SimModem to act as a PPP peer (like the real SIM7600).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// PPP protocol IDs
#define PPP_LCP    0xC021
#define PPP_IPCP   0x8021
#define PPP_IP     0x0021

// LCP/IPCP code values
#define PPP_CONF_REQ    1
#define PPP_CONF_ACK    2
#define PPP_CONF_NAK    3
#define PPP_CONF_REJ    4
#define PPP_TERM_REQ    5
#define PPP_TERM_ACK    6
#define PPP_ECHO_REQ    9
#define PPP_ECHO_REP   10

// IPCP option types
#define IPCP_OPT_IP_ADDR    3
#define IPCP_OPT_DNS1      129
#define IPCP_OPT_DNS2      131

// ── CRC-16 (PPP FCS) ───────────────────────────────────────────────────────

static const uint16_t ppp_fcs_table[256] = {
    0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
    0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
    0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
    0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
    0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
    0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
    0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
    0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
    0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
    0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
    0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
    0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
    0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
    0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
    0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
    0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
    0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
    0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
    0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
    0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
    0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
    0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
    0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
    0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
    0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
    0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
    0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
    0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
    0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
    0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
    0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
    0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78,
};

inline uint16_t ppp_fcs(const uint8_t* data, size_t len) {
    uint16_t fcs = 0xFFFF;
    for (size_t i = 0; i < len; i++)
        fcs = (fcs >> 8) ^ ppp_fcs_table[(fcs ^ data[i]) & 0xFF];
    return fcs ^ 0xFFFF;
}

// ── HDLC frame builder ──────────────────────────────────────────────────────

// Build a PPP frame: 7E FF 03 [protocol] [payload] [FCS] 7E
// With byte stuffing (0x7D escape for 0x7E, 0x7D, <0x20)
inline std::vector<uint8_t> ppp_build_frame(uint16_t protocol, const uint8_t* payload, size_t len) {
    // Raw frame (before stuffing): FF 03 protocol(2) payload FCS(2)
    std::vector<uint8_t> raw;
    raw.push_back(0xFF);  // address
    raw.push_back(0x03);  // control
    raw.push_back((protocol >> 8) & 0xFF);
    raw.push_back(protocol & 0xFF);
    raw.insert(raw.end(), payload, payload + len);

    // Compute FCS over raw (address + control + protocol + payload)
    uint16_t fcs = ppp_fcs(raw.data(), raw.size());
    raw.push_back(fcs & 0xFF);
    raw.push_back((fcs >> 8) & 0xFF);

    // Byte-stuff and frame
    std::vector<uint8_t> frame;
    frame.push_back(0x7E);
    for (uint8_t b : raw) {
        if (b == 0x7E || b == 0x7D || b < 0x20) {
            frame.push_back(0x7D);
            frame.push_back(b ^ 0x20);
        } else {
            frame.push_back(b);
        }
    }
    frame.push_back(0x7E);
    return frame;
}

// Parse a PPP frame: remove delimiters, unstuff, verify FCS.
// Returns true if valid frame. Sets protocol and payload.
inline bool ppp_parse_frame(const uint8_t* data, size_t len,
                             uint16_t* protocol, std::vector<uint8_t>* payload) {
    if (len < 2 || data[0] != 0x7E) return false;

    // Find end delimiter
    size_t end = 0;
    for (size_t i = 1; i < len; i++) {
        if (data[i] == 0x7E) { end = i; break; }
    }
    if (end < 6) return false;  // too short

    // Unstuff
    std::vector<uint8_t> raw;
    for (size_t i = 1; i < end; i++) {
        if (data[i] == 0x7D && i + 1 < end) {
            raw.push_back(data[++i] ^ 0x20);
        } else {
            raw.push_back(data[i]);
        }
    }

    if (raw.size() < 6) return false;  // addr(1) + ctrl(1) + proto(2) + fcs(2)

    // Verify FCS (over everything except FCS itself)
    uint16_t fcs = ppp_fcs(raw.data(), raw.size() - 2);
    uint16_t expected = raw[raw.size()-2] | (raw[raw.size()-1] << 8);
    if (fcs != expected) return false;

    // Extract protocol (skip FF 03 address/control)
    size_t offset = 0;
    if (raw[0] == 0xFF && raw[1] == 0x03) offset = 2;

    *protocol = (raw[offset] << 8) | raw[offset + 1];
    payload->assign(raw.begin() + offset + 2, raw.end() - 2);
    return true;
}

// ── LCP/IPCP packet builder ─────────────────────────────────────────────────

inline std::vector<uint8_t> ppp_build_conf_ack(uint8_t id, const uint8_t* options, size_t optLen) {
    std::vector<uint8_t> pkt;
    pkt.push_back(PPP_CONF_ACK);
    pkt.push_back(id);
    uint16_t length = 4 + optLen;
    pkt.push_back((length >> 8) & 0xFF);
    pkt.push_back(length & 0xFF);
    pkt.insert(pkt.end(), options, options + optLen);
    return pkt;
}

inline std::vector<uint8_t> ppp_build_conf_req(uint8_t id, const uint8_t* options, size_t optLen) {
    std::vector<uint8_t> pkt;
    pkt.push_back(PPP_CONF_REQ);
    pkt.push_back(id);
    uint16_t length = 4 + optLen;
    pkt.push_back((length >> 8) & 0xFF);
    pkt.push_back(length & 0xFF);
    pkt.insert(pkt.end(), options, options + optLen);
    return pkt;
}

inline std::vector<uint8_t> ppp_build_echo_reply(uint8_t id, uint32_t magic) {
    std::vector<uint8_t> pkt;
    pkt.push_back(PPP_ECHO_REP);
    pkt.push_back(id);
    pkt.push_back(0x00); pkt.push_back(0x08);  // length = 8
    pkt.push_back((magic >> 24) & 0xFF);
    pkt.push_back((magic >> 16) & 0xFF);
    pkt.push_back((magic >> 8) & 0xFF);
    pkt.push_back(magic & 0xFF);
    return pkt;
}

// ── URC parser (extracted from modemTask data pump) ─────────────────────────

struct UrcState {
    char buf[128];
    int len = 0;
    int rssi = 99;
    int regStat = 1;
    bool regLost = false;

    void reset() { len = 0; rssi = 99; regStat = 1; regLost = false; }

    // Feed a byte from the UART stream. Returns true if a URC was parsed.
    bool feed(uint8_t byte) {
        if (byte == 0x7E || byte == '\0') {
            return processAccumulated();
        } else if (byte >= 0x20 && byte < 0x7F && len < (int)sizeof(buf) - 1) {
            buf[len++] = byte;
        } else if (byte == '\r' || byte == '\n') {
            return processAccumulated();
        }
        return false;
    }

private:
    bool processAccumulated() {
        bool parsed = false;
        if (len > 2) {
            buf[len] = '\0';
            char* csq = strstr(buf, "+CSQ:");
            if (csq) {
                int r = 99, b = 99;
                sscanf(csq, "+CSQ: %d,%d", &r, &b);
                if (r != 99) rssi = r;
                parsed = true;
            }
            char* creg = strstr(buf, "+CREG:");
            if (creg) {
                int s = 0;
                sscanf(creg, "+CREG: %d", &s);
                regStat = s;
                if (s != 1 && s != 5) regLost = true;
                parsed = true;
            }
        }
        len = 0;
        return parsed;
    }
};
