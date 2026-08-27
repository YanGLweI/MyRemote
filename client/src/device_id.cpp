#include "device_id.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <cstdio>
#include <vector>

namespace device {

namespace {

uint64_t fnv1a(const std::string& input) {
    uint64_t hash = 1469598103934665603ULL;
    for (char c : input) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string primary_mac_address() {
    ULONG size = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                        GAA_FLAG_SKIP_DNS_SERVER,
                         nullptr, nullptr, &size);
    if (size == 0) {
        return {};
    }

    std::vector<uint8_t> buffer(size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                            GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, addresses, &size) != ERROR_SUCCESS) {
        return {};
    }

    for (IP_ADAPTER_ADDRESSES* adapter = addresses; adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->PhysicalAddressLength == 0 ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        std::string mac;
        char byte_str[4];
        for (DWORD i = 0; i < adapter->PhysicalAddressLength; ++i) {
            snprintf(byte_str, sizeof(byte_str), "%02x", adapter->PhysicalAddress[i]);
            mac += byte_str;
        }
        return mac;
    }
    return {};
}

std::string computer_name() {
    char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = sizeof(name);
    if (!GetComputerNameA(name, &size)) {
        return "unknown-host";
    }
    return name;
}

}  // namespace

std::string default_device_name() {
    return computer_name();
}

std::string make_device_id() {
    std::string material = primary_mac_address() + "|" + computer_name();
    char id[20];
    snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(fnv1a(material)));
    return id;
}

}  // namespace device
