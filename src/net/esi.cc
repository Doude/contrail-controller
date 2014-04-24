/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "net/esi.h"

#include "base/parse_object.h"
#include "net/address.h"

using namespace std;

static uint8_t max_esi_bytes[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

EthernetSegmentId EthernetSegmentId::null_esi;
EthernetSegmentId EthernetSegmentId::max_esi(max_esi_bytes);

EthernetSegmentId::EthernetSegmentId() {
    memset(data_, 0, kSize);
}

EthernetSegmentId::EthernetSegmentId(const uint8_t *data) {
    memcpy(data_, data, kSize);
}

string EthernetSegmentId::ToString() const {
    if (CompareTo(null_esi) == 0)
        return "null-esi";
    if (CompareTo(max_esi) == 0)
        return "max-esi";

    switch (Type()) {
    case AS_BASED: {
        char temp[64];
        uint32_t asn = get_value(data_ + 1, 4);
        uint32_t value = get_value(data_ + 5, 4);
        snprintf(temp, sizeof(temp), "%u:%u", asn, value);
        return temp;
        break;
    }
    case IP_BASED: {
        Ip4Address ip(get_value(data_ + 1, 4));
        uint32_t value = get_value(data_ + 5, 4);
        char temp[64];
        snprintf(temp, sizeof(temp), ":%u", value);
        return ip.to_string() + temp;
        break;
    }
    case MAC_BASED:
    case STP_BASED:
    case LACP_BASED:
    case CONFIGURED:
    default: {
        char temp[64];
        snprintf(temp, sizeof(temp),
            "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
            data_[0], data_[1], data_[2], data_[3], data_[4],
            data_[5], data_[6], data_[7], data_[8], data_[9]);
        return temp;
        break;
    }
    }

    return "bad_esi";
}

int EthernetSegmentId::CompareTo(const EthernetSegmentId &rhs) const {
    return memcmp(data_, rhs.data_, kSize);
}
