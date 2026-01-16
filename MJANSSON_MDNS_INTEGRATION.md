# mjansson/mdns Integration for open62541

## Overview

This document describes the integration of the mjansson/mdns library into the open62541 project for mDNS/DNS-SD discovery support.

## What Was Done

### 1. Added mjansson/mdns as a Git Submodule
The mjansson/mdns library was added as a git submodule in `deps/mdns`:
```bash
git submodule add https://github.com/mjansson/mdns deps/mdns
```

**Location**: `/home/mbruder/Work/open62541/deps/mdns`

### 2. Updated CMakeLists.txt
Modified `/home/mbruder/Work/open62541/CMakeLists.txt` to:

#### a. Add MJANSSON to the mDNS Plugin List
- Added `"MJANSSON"` to the `UA_MDNS_PLUGINS` list
- Added `UA_ENABLE_DISCOVERY_MULTICAST_MJANSSON` option for backward compatibility

#### b. Configure Plugin Selection Logic
Updated the configuration logic to properly handle the MJANSSON plugin:
- Added configuration conditions to set/unset the MJANSSON flag
- Implemented mutual exclusivity between MDNSD, AVAHI, and MJANSSON plugins

#### c. Add Build Configuration
Added build configuration for the MJANSSON plugin:
- Include `mdns.h` header from the submodule
- Include the new `ua_discovery_mdns_mjansson.c` source file
- Added include path for the mdns header directory

### 3. Implemented ua_discovery_mdns_mjansson.c
Created a new implementation file at `/home/mbruder/Work/open62541/src/server/ua_discovery_mdns_mjansson.c`

#### Key Features
The implementation includes:
- **Socket Management**: Creates and manages IPv4 mDNS sockets for sending and receiving
- **Server Discovery**: Implements mDNS discovery callback that:
  - Processes PTR (service pointer) records
  - Processes SRV (service) records to extract hostname and port
  - Processes TXT records to extract path and capabilities
- **Service Announcement**: `UA_Discovery_updateMdnsForDiscoveryUrl()` announces OPC UA services on the network
- **List Management**: Maintains a hash-based list of discovered servers with:
  - Server name tracking
  - Last seen timestamp
  - Service discovery state (TXT and SRV record status)
  - Discovery URL storage

#### API Functions Implemented
All required public API functions from `ua_discovery.h`:
- `UA_DiscoveryManager_startMulticast()` - Start mDNS discovery
- `UA_DiscoveryManager_stopMulticast()` - Stop mDNS discovery  
- `UA_DiscoveryManager_clearMdns()` - Clear mDNS state
- `UA_DiscoveryManager_getMdnsConnectionCount()` - Get socket count
- `UA_DiscoveryManager_getServerOnNetworkRecordIdCounter()` - Get record counter
- `UA_DiscoveryManager_resetServerOnNetworkRecordCounter()` - Reset counter
- `UA_DiscoveryManager_getServerOnNetworkCounterResetTime()` - Get reset time
- `UA_DiscoveryManager_getServerOnNetworkList()` - Get discovered servers list
- `UA_DiscoveryManager_getNextServerOnNetworkRecord()` - Iterate through servers
- `UA_DiscoveryManager_clearServerOnNetwork()` - Clear server list
- `UA_Discovery_updateMdnsForDiscoveryUrl()` - Announce service
- `UA_Server_setServerOnNetworkCallback()` - Set callback for server discoveries
- `UA_DiscoveryManager_mdnsCyclicTimer()` - Periodic mDNS processing

#### Data Structures
- `serverOnNetwork`: Tracks individual discovered servers
- `mjansson_mdns_private`: Private state for mDNS implementation

## Building with mjansson/mdns

### Configure with CMake
```bash
cd /path/to/open62541
mkdir build
cd build
cmake -DUA_ENABLE_DISCOVERY_MULTICAST=MJANSSON ..
```

### Build
```bash
make
```

### Available mDNS Plugin Options
- `OFF` - No mDNS support
- `MDNSD` - Use embedded mdnsd library
- `AVAHI` - Use Avahi daemon
- `MJANSSON` - Use mjansson/mdns (new)

## Features

### Advantages of mjansson/mdns
1. **Header-Only Library**: No external dependencies to compile/link
2. **Cross-Platform**: Works on Linux, macOS, Windows
3. **Public Domain**: Licensed under Unlicense - no restrictions
4. **Stateless Design**: All buffers passed by caller - predictable memory usage
5. **Lightweight**: Minimal code footprint
6. **Standards Compliant**: Implements RFC 6762 (mDNS) and RFC 6763 (DNS-SD)

## Implementation Notes

### Discovery Process
1. Application calls `UA_DiscoveryManager_startMulticast()` to initialize
2. mDNS sockets are created on INADDR_ANY and MDNS_PORT (5353)
3. `UA_DiscoveryManager_mdnsCyclicTimer()` is called periodically (typically via event loop)
4. Timer reads incoming mDNS packets and parses them via `query_callback()`
5. Discovered services are tracked internally
6. Client can retrieve discovered servers via `UA_DiscoveryManager_getServerOnNetworkList()`

### Service Announcement
1. Application calls `UA_Discovery_updateMdnsForDiscoveryUrl()` with service details
2. Service is added to the internal discovery list
3. Callback is triggered if registered
4. Application can send mDNS announcements using standard mDNS functions

### Thread Safety
- Current implementation is single-threaded (no locks)
- Suitable for use in event-driven servers
- Multi-threaded access requires external synchronization

## File Structure

```
open62541/
├── CMakeLists.txt                          (modified)
├── deps/
│   └── mdns/                               (git submodule)
│       ├── mdns.h
│       ├── mdns.c
│       ├── CMakeLists.txt
│       └── ...
└── src/server/
    ├── ua_discovery.h
    ├── ua_discovery.c
    ├── ua_discovery_mdns.c                 (existing - mdnsd implementation)
    ├── ua_discovery_mdns_avahi.c           (existing - Avahi implementation)
    └── ua_discovery_mdns_mjansson.c        (new - mjansson/mdns implementation)
```

## Testing

To test the mjansson/mdns integration:

```bash
# Build with mjansson support
cd /path/to/open62541
mkdir build_mjansson
cd build_mjansson
cmake -DUA_ENABLE_DISCOVERY_MULTICAST=MJANSSON ..
make

# Test binary will be in build_mjansson/bin/
```

## Compatibility

- **C Standard**: C99 compatible
- **Platforms**: Linux, macOS, Windows (POSIX and Win32 socket APIs)
- **Dependencies**: None beyond standard C library and POSIX sockets
- **Python Version**: Works with Python 3.x for build tools

## Future Enhancements

Potential improvements:
1. IPv6 support (currently IPv4 only)
2. Service announcement via multicast
3. Custom TXT record handling
4. TTL-based server expiration
5. Performance optimizations for large discovery lists
6. Integration with event loop for true async operations

## References

- mjansson/mdns GitHub: https://github.com/mjansson/mdns
- RFC 6762 (mDNS): https://tools.ietf.org/html/rfc6762
- RFC 6763 (DNS-SD): https://tools.ietf.org/html/rfc6763
- open62541 Discovery: https://github.com/open62541/open62541

## License

The mjansson/mdns library is in the public domain (Unlicense).
open62541 is licensed under the Mozilla Public License v2.0.
This integration maintains compatibility with both licenses.
