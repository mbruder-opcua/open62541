/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2024 (c) Fraunhofer IOSB (using mjansson/mdns library)
 */

#include "ua_discovery.h"
#include "ua_server_internal.h"
#include <stdlib.h>
#include "mdns.h"

#ifdef UA_ENABLE_DISCOVERY_MULTICAST_MJANSSON

#ifdef UA_ARCHITECTURE_WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <unistd.h>
#endif

#define SERVER_ON_NETWORK_HASH_SIZE 1000

typedef struct serverOnNetwork {
    LIST_ENTRY(serverOnNetwork) pointers;
    UA_ServerOnNetwork serverOnNetwork;
    UA_DateTime created;
    UA_DateTime lastSeen;
    UA_Boolean txtSet;
    UA_Boolean srvSet;
    char* pathTmp;
} serverOnNetwork;

typedef struct serverOnNetwork_hash_entry {
    serverOnNetwork *entry;
    struct serverOnNetwork_hash_entry* next;
} serverOnNetwork_hash_entry;

typedef struct mjansson_mdns_private {
    int sendSocket;
    int recvSocket;
    /* hash mapping domain name to serverOnNetwork list entry */
    struct serverOnNetwork_hash_entry* serverOnNetworkHash[SERVER_ON_NETWORK_HASH_SIZE];
    LIST_HEAD(, serverOnNetwork) serverOnNetwork;
    /* Name of server itself. Used to detect if received mDNS
     * message was from itself */
    UA_String selfMdnsRecord;
    UA_UInt32 serverOnNetworkRecordIdCounter;
    UA_DateTime serverOnNetworkRecordIdLastReset;
} mjansson_mdns_private;

static mjansson_mdns_private mdnsPrivateData = {
    .sendSocket = -1,
    .recvSocket = -1,
    .serverOnNetworkRecordIdCounter = 0
};

/* Helper function to parse service domain names */
static mdns_string_t
mdns_string_from_ua_string(const UA_String* ua_str, char* buffer, size_t capacity) {
    mdns_string_t str = {0, 0};
    if (ua_str && ua_str->data && ua_str->length > 0) {
        size_t len = ua_str->length < capacity ? ua_str->length : capacity - 1;
        memcpy(buffer, ua_str->data, len);
        buffer[len] = '\0';
        str.str = buffer;
        str.length = len;
    }
    return str;
}

static UA_StatusCode
UA_DiscoveryManager_addEntryToServersOnNetwork(UA_DiscoveryManager *dm,
                                               const char *fqdnMdnsRecord,
                                               UA_String serverName,
                                               struct serverOnNetwork **addedEntry);

static struct serverOnNetwork *
mdns_record_add_or_get(UA_DiscoveryManager *dm, const char *record,
                       UA_String serverName, UA_Boolean createNew) {
    UA_UInt32 hashIdx = UA_ByteString_hash(0, (const UA_Byte*)record,
                                           strlen(record)) % SERVER_ON_NETWORK_HASH_SIZE;
    struct serverOnNetwork_hash_entry *hash_entry = mdnsPrivateData.serverOnNetworkHash[hashIdx];

    while(hash_entry) {
        size_t maxLen = serverName.length;
        if(maxLen > hash_entry->entry->serverOnNetwork.serverName.length)
            maxLen = hash_entry->entry->serverOnNetwork.serverName.length;

        if(strncmp((char*)hash_entry->entry->serverOnNetwork.serverName.data,
                   (char*)serverName.data, maxLen) == 0)
            return hash_entry->entry;
        hash_entry = hash_entry->next;
    }

    if(!createNew)
        return NULL;

    struct serverOnNetwork *listEntry;
    UA_StatusCode res =
        UA_DiscoveryManager_addEntryToServersOnNetwork(dm, record, serverName, &listEntry);
    if(res != UA_STATUSCODE_GOOD)
        return NULL;

    return listEntry;
}

static UA_StatusCode
UA_DiscoveryManager_addEntryToServersOnNetwork(UA_DiscoveryManager *dm,
                                               const char *fqdnMdnsRecord,
                                               UA_String serverName,
                                               struct serverOnNetwork **addedEntry) {
    struct serverOnNetwork *entry =
            mdns_record_add_or_get(dm, fqdnMdnsRecord, serverName, false);
    if(entry) {
        if(addedEntry != NULL)
            *addedEntry = entry;
        return UA_STATUSCODE_BADALREADYEXISTS;
    }

    UA_LOG_DEBUG(dm->sc.server->config.logging, UA_LOGCATEGORY_SERVER,
                 "Multicast DNS: adding entry for domain: %s", fqdnMdnsRecord);

    struct serverOnNetwork *listEntry = (serverOnNetwork*)
            UA_malloc(sizeof(struct serverOnNetwork));
    if(!listEntry)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    UA_EventLoop *el = dm->sc.server->config.eventLoop;
    listEntry->created = el->dateTime_now(el);
    listEntry->pathTmp = NULL;
    listEntry->txtSet = false;
    listEntry->srvSet = false;
    UA_ServerOnNetwork_init(&listEntry->serverOnNetwork);
    listEntry->serverOnNetwork.recordId = mdnsPrivateData.serverOnNetworkRecordIdCounter;
    UA_StatusCode res = UA_String_copy(&serverName, &listEntry->serverOnNetwork.serverName);
    if(res != UA_STATUSCODE_GOOD) {
        UA_free(listEntry);
        return res;
    }
    mdnsPrivateData.serverOnNetworkRecordIdCounter++;
    if(mdnsPrivateData.serverOnNetworkRecordIdCounter == 0)
        mdnsPrivateData.serverOnNetworkRecordIdLastReset = el->dateTime_now(el);
    listEntry->lastSeen = el->dateTime_nowMonotonic(el);

    /* add to hash */
    UA_UInt32 hashIdx = UA_ByteString_hash(0, (const UA_Byte*)fqdnMdnsRecord,
                                           strlen(fqdnMdnsRecord)) % SERVER_ON_NETWORK_HASH_SIZE;
    struct serverOnNetwork_hash_entry *newHashEntry = (struct serverOnNetwork_hash_entry*)
            UA_malloc(sizeof(struct serverOnNetwork_hash_entry));
    if(!newHashEntry) {
        UA_ServerOnNetwork_clear(&listEntry->serverOnNetwork);
        UA_free(listEntry);
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }
    newHashEntry->next = mdnsPrivateData.serverOnNetworkHash[hashIdx];
    mdnsPrivateData.serverOnNetworkHash[hashIdx] = newHashEntry;
    newHashEntry->entry = listEntry;

    LIST_INSERT_HEAD(&mdnsPrivateData.serverOnNetwork, listEntry, pointers);
    if(addedEntry != NULL)
        *addedEntry = listEntry;

    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
UA_DiscoveryManager_removeEntryFromServersOnNetwork(UA_DiscoveryManager *dm,
                                                    const char *fqdnMdnsRecord,
                                                    UA_String serverName) {
    UA_LOG_DEBUG(dm->sc.server->config.logging, UA_LOGCATEGORY_SERVER,
                 "Multicast DNS: removing entry for domain: %s", fqdnMdnsRecord);

    struct serverOnNetwork *entry =
            mdns_record_add_or_get(dm, fqdnMdnsRecord, serverName, false);
    if(!entry)
        return UA_STATUSCODE_BADNOTHINGTODO;

    UA_String recordStr;
    recordStr.data = (UA_Byte*)(uintptr_t)fqdnMdnsRecord;
    recordStr.length = strlen(fqdnMdnsRecord);

    /* remove from hash */
    UA_UInt32 hashIdx = UA_ByteString_hash(0, (const UA_Byte*)recordStr.data,
                                           recordStr.length) % SERVER_ON_NETWORK_HASH_SIZE;
    struct serverOnNetwork_hash_entry *hash_entry = mdnsPrivateData.serverOnNetworkHash[hashIdx];
    struct serverOnNetwork_hash_entry *prevEntry = hash_entry;
    while(hash_entry) {
        if(hash_entry->entry == entry) {
            if(hash_entry == mdnsPrivateData.serverOnNetworkHash[hashIdx])
                mdnsPrivateData.serverOnNetworkHash[hashIdx] = hash_entry->next;
            else
                prevEntry->next = hash_entry->next;
            break;
        }
        prevEntry = hash_entry;
        hash_entry = hash_entry->next;
    }
    UA_free(hash_entry);

    if(dm->serverOnNetworkCallback &&
       entry->srvSet) {
        entry->serverOnNetwork.recordId = 0;
        dm->serverOnNetworkCallback(dm, &entry->serverOnNetwork,
                                    dm->serverOnNetworkCallbackData);
    }

    /* Remove from list */
    LIST_REMOVE(entry, pointers);
    UA_ServerOnNetwork_clear(&entry->serverOnNetwork);
    if(entry->pathTmp) {
        UA_free(entry->pathTmp);
        entry->pathTmp = NULL;
    }
    UA_free(entry);
    return UA_STATUSCODE_GOOD;
}

static int
query_callback(int sock, const struct sockaddr* from, size_t addrlen,
               mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
               uint16_t rclass, uint32_t ttl, const void* data, size_t size,
               size_t name_offset, size_t name_length, size_t record_offset,
               size_t record_length, void* user_data) {
    (void)sock;
    (void)from;
    (void)addrlen;
    (void)query_id;
    (void)rclass;
    (void)ttl;
    
    UA_DiscoveryManager *dm = (UA_DiscoveryManager*)user_data;
    if (!dm)
        return 0;

    char namebuffer[256] = {0};
    char entrybuffer[256] = {0};
    mdns_string_t fromname, hostname, service;

    if (rtype == MDNS_RECORDTYPE_PTR) {
        /* A PTR response record */
        if (entry != MDNS_ENTRYTYPE_ANSWER)
            return 0;

        fromname = mdns_string_extract(data, size, &name_offset, namebuffer, sizeof(namebuffer));
        
        /* Check if this is an OPC UA service */
        if (strstr(fromname.str, "_opcua-tcp") == NULL)
            return 0;

        service = mdns_record_parse_ptr(data, size, record_offset, record_length,
                                       entrybuffer, sizeof(entrybuffer));
        
        if (service.length > 0) {
            UA_String serverName;
            serverName.data = (UA_Byte*)service.str;
            serverName.length = service.length;
            
            struct serverOnNetwork *listEntry;
            UA_StatusCode res = UA_DiscoveryManager_addEntryToServersOnNetwork(
                dm, service.str, serverName, &listEntry);
            if (res == UA_STATUSCODE_GOOD && listEntry) {
                listEntry->txtSet = false;
                listEntry->srvSet = false;
            }
        }
    } else if (rtype == MDNS_RECORDTYPE_SRV) {
        /* A SRV response record */
        if (entry != MDNS_ENTRYTYPE_ANSWER)
            return 0;

        fromname = mdns_string_extract(data, size, &name_offset, namebuffer, sizeof(namebuffer));
        struct mdns_record_srv_t srv = mdns_record_parse_srv(data, size, record_offset,
                                                             record_length, entrybuffer,
                                                             sizeof(entrybuffer));

        if (srv.name.length > 0) {
            UA_String serverName;
            serverName.data = (UA_Byte*)fromname.str;
            serverName.length = fromname.length;

            char uriBuf[256];
            int uriLen = snprintf(uriBuf, sizeof(uriBuf), "opc.tcp://%.*s:%d",
                                 (int)srv.name.length, srv.name.str, srv.port);
            if (uriLen > 0 && uriLen < (int)sizeof(uriBuf)) {
                UA_String discoveryUrl;
                discoveryUrl.data = (UA_Byte*)uriBuf;
                discoveryUrl.length = uriLen;

                struct serverOnNetwork *listEntry;
                UA_StatusCode res = UA_DiscoveryManager_addEntryToServersOnNetwork(
                    dm, fromname.str, serverName, &listEntry);
                if (res == UA_STATUSCODE_GOOD && listEntry) {
                    UA_String_copy(&discoveryUrl, &listEntry->serverOnNetwork.discoveryUrl);
                    listEntry->srvSet = true;
                    listEntry->lastSeen = dm->sc.server->config.eventLoop->dateTime_nowMonotonic(
                        dm->sc.server->config.eventLoop);

                    if (listEntry->srvSet && dm->serverOnNetworkCallback) {
                        dm->serverOnNetworkCallback(dm, &listEntry->serverOnNetwork,
                                                   dm->serverOnNetworkCallbackData);
                    }
                }
            }
        }
    } else if (rtype == MDNS_RECORDTYPE_TXT) {
        /* A TXT response record */
        if (entry != MDNS_ENTRYTYPE_ANSWER)
            return 0;

        fromname = mdns_string_extract(data, size, &name_offset, namebuffer, sizeof(namebuffer));
        
        /* Parse TXT records (key-value pairs) */
        size_t offset = record_offset;
        mdns_record_txt_t txt[32];
        size_t parsed = mdns_record_parse_txt(data, size, offset, txt, 32);

        for (size_t i = 0; i < parsed; ++i) {
            if (txt[i].key.length > 0 && strncmp(txt[i].key.str, "path", txt[i].key.length) == 0) {
                if (txt[i].value.length > 0) {
                    UA_String serverName;
                    serverName.data = (UA_Byte*)fromname.str;
                    serverName.length = fromname.length;

                    struct serverOnNetwork *listEntry;
                    UA_StatusCode res = UA_DiscoveryManager_addEntryToServersOnNetwork(
                        dm, fromname.str, serverName, &listEntry);
                    if (res == UA_STATUSCODE_GOOD && listEntry) {
                        if (listEntry->pathTmp)
                            UA_free(listEntry->pathTmp);
                        listEntry->pathTmp = (char*)UA_malloc(txt[i].value.length + 1);
                        if (listEntry->pathTmp) {
                            memcpy(listEntry->pathTmp, txt[i].value.str, txt[i].value.length);
                            listEntry->pathTmp[txt[i].value.length] = '\0';
                        }
                        listEntry->txtSet = true;
                    }
                }
            }
        }
    }

    return 0;
}

UA_StatusCode
UA_DiscoveryManager_clearServerOnNetwork(UA_DiscoveryManager *dm) {
    if(!dm)
        return UA_STATUSCODE_BADARGUMENTSMISSING;

    serverOnNetwork *son, *son_tmp;
    LIST_FOREACH_SAFE(son, &mdnsPrivateData.serverOnNetwork, pointers, son_tmp) {
        UA_String recordStr;
        recordStr.data = son->serverOnNetwork.serverName.data;
        recordStr.length = son->serverOnNetwork.serverName.length;
        UA_DiscoveryManager_removeEntryFromServersOnNetwork(dm, (const char*)recordStr.data, recordStr);
    }

    UA_String_clear(&mdnsPrivateData.selfMdnsRecord);

    for(size_t i = 0; i < SERVER_ON_NETWORK_HASH_SIZE; i++) {
        struct serverOnNetwork_hash_entry *hash_entry = mdnsPrivateData.serverOnNetworkHash[i];
        while(hash_entry) {
            struct serverOnNetwork_hash_entry *next = hash_entry->next;
            UA_free(hash_entry);
            hash_entry = next;
        }
        mdnsPrivateData.serverOnNetworkHash[i] = NULL;
    }

    return UA_STATUSCODE_GOOD;
}

UA_ServerOnNetwork*
UA_DiscoveryManager_getServerOnNetworkList(UA_DiscoveryManager *dm) {
    (void)dm;
    serverOnNetwork* entry = LIST_FIRST(&mdnsPrivateData.serverOnNetwork);
    return entry ? &entry->serverOnNetwork : NULL;
}

UA_ServerOnNetwork*
UA_DiscoveryManager_getNextServerOnNetworkRecord(UA_DiscoveryManager *dm,
                                   UA_ServerOnNetwork *current) {
    (void)dm;
    serverOnNetwork *entry = NULL;
    LIST_FOREACH(entry, &mdnsPrivateData.serverOnNetwork, pointers) {
        if(&entry->serverOnNetwork == current) {
            serverOnNetwork *next = LIST_NEXT(entry, pointers);
            return next ? &next->serverOnNetwork : NULL;
        }
    }
    return NULL;
}

UA_UInt32
UA_DiscoveryManager_getServerOnNetworkRecordIdCounter(UA_DiscoveryManager *dm) {
    if(!dm)
        return 0;
    return mdnsPrivateData.serverOnNetworkRecordIdCounter;
}

UA_StatusCode
UA_DiscoveryManager_resetServerOnNetworkRecordCounter(UA_DiscoveryManager *dm) {
    if(!dm)
        return UA_STATUSCODE_BADARGUMENTSMISSING;
    mdnsPrivateData.serverOnNetworkRecordIdCounter = 0;
    mdnsPrivateData.serverOnNetworkRecordIdLastReset = dm->sc.server->config.eventLoop->dateTime_now(
        dm->sc.server->config.eventLoop);
    return UA_STATUSCODE_GOOD;
}

UA_DateTime
UA_DiscoveryManager_getServerOnNetworkCounterResetTime(UA_DiscoveryManager *dm) {
    if(!dm)
        return 0;
    return mdnsPrivateData.serverOnNetworkRecordIdLastReset;
}

void
UA_DiscoveryManager_startMulticast(UA_DiscoveryManager *dm) {
    UA_LOG_INFO(dm->sc.server->config.logging, UA_LOGCATEGORY_DISCOVERY,
                "Multicast DNS: starting mDNS discovery");

    /* Open sockets for mDNS service discovery and response */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(MDNS_PORT);

#ifdef SO_REUSEADDR
    int reuse = 1;
    setsockopt(mdnsPrivateData.sendSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#endif

    /* Create send socket */
    mdnsPrivateData.sendSocket = mdns_socket_open_ipv4(NULL);
    if (mdnsPrivateData.sendSocket < 0) {
        UA_LOG_ERROR(dm->sc.server->config.logging, UA_LOGCATEGORY_DISCOVERY,
                    "Multicast DNS: failed to open send socket");
        return;
    }

    /* Create receive socket */
    mdnsPrivateData.recvSocket = mdns_socket_open_ipv4(&addr);
    if (mdnsPrivateData.recvSocket < 0) {
        UA_LOG_ERROR(dm->sc.server->config.logging, UA_LOGCATEGORY_DISCOVERY,
                    "Multicast DNS: failed to open receive socket");
        mdns_socket_close(mdnsPrivateData.sendSocket);
        mdnsPrivateData.sendSocket = -1;
        return;
    }

    UA_LOG_INFO(dm->sc.server->config.logging, UA_LOGCATEGORY_DISCOVERY,
                "Multicast DNS: sockets opened successfully");
}

void
UA_DiscoveryManager_stopMulticast(UA_DiscoveryManager *dm) {
    UA_LOG_INFO(dm->sc.server->config.logging, UA_LOGCATEGORY_DISCOVERY,
                "Multicast DNS: stopping mDNS discovery");

    if (mdnsPrivateData.sendSocket >= 0) {
        mdns_socket_close(mdnsPrivateData.sendSocket);
        mdnsPrivateData.sendSocket = -1;
    }
    if (mdnsPrivateData.recvSocket >= 0) {
        mdns_socket_close(mdnsPrivateData.recvSocket);
        mdnsPrivateData.recvSocket = -1;
    }
}

void
UA_DiscoveryManager_clearMdns(UA_DiscoveryManager *dm) {
    UA_DiscoveryManager_clearServerOnNetwork(dm);
}

UA_UInt32
UA_DiscoveryManager_getMdnsConnectionCount(void) {
    return (mdnsPrivateData.sendSocket >= 0) + (mdnsPrivateData.recvSocket >= 0);
}

void
UA_DiscoveryManager_mdnsCyclicTimer(UA_Server *server, void *data) {
    UA_DiscoveryManager *dm = (UA_DiscoveryManager*)data;
    if (!dm || mdnsPrivateData.recvSocket < 0)
        return;

    size_t capacity = 2048;
    uint8_t buffer[2048];
    
    /* Listen for incoming mDNS messages */
    mdns_socket_listen(mdnsPrivateData.recvSocket, buffer, capacity, query_callback, dm);
}

void
UA_Discovery_updateMdnsForDiscoveryUrl(UA_DiscoveryManager *dm, const UA_String serverName,
                                       const UA_MdnsDiscoveryConfiguration *mdnsConfig,
                                       const UA_String discoveryUrl, UA_Boolean isOnline,
                                       UA_Boolean updateTxt) {
    (void)mdnsConfig;
    (void)updateTxt;
    
    if (!dm || !isOnline)
        return;

    /* Parse the discovery URL to extract hostname and port */
    UA_String hostname = UA_STRING_NULL;
    UA_UInt16 port = 4840;
    UA_String path = UA_STRING_NULL;
    
    UA_StatusCode retval = UA_parseEndpointUrl(&discoveryUrl, &hostname, &port, &path);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_LOG_WARNING(dm->sc.server->config.logging, UA_LOGCATEGORY_DISCOVERY,
                      "Multicast DNS: invalid discovery URL");
        return;
    }

    if (hostname.length == 0) {
        UA_LOG_WARNING(dm->sc.server->config.logging, UA_LOGCATEGORY_DISCOVERY,
                      "Multicast DNS: empty hostname in discovery URL");
        return;
    }

    /* Create service domain name */
    char serviceDomainBuf[256];
    char hostnameBuf[256];
    char pathBuf[256];
    
    if (hostname.length < sizeof(hostnameBuf)) {
        memcpy(hostnameBuf, hostname.data, hostname.length);
        hostnameBuf[hostname.length] = '\0';
    } else {
        return;
    }

    if (path.length > 0 && path.length < sizeof(pathBuf)) {
        memcpy(pathBuf, path.data, path.length);
        pathBuf[path.length] = '\0';
    } else {
        pathBuf[0] = '/';
        pathBuf[1] = '\0';
    }

    int len = snprintf(serviceDomainBuf, sizeof(serviceDomainBuf), 
                      "%.*s-%s._opcua-tcp._tcp.local.",
                      (int)serverName.length, (const char*)serverName.data, hostnameBuf);
    
    if (len > 0 && len < (int)sizeof(serviceDomainBuf)) {
        UA_String serviceName;
        serviceName.data = (UA_Byte*)serviceDomainBuf;
        serviceName.length = len;

        UA_LOG_INFO(dm->sc.server->config.logging, UA_LOGCATEGORY_DISCOVERY,
                   "Multicast DNS: announcing service %.*s", (int)serviceName.length,
                   (const char*)serviceName.data);

        /* Add entry to server list */
        struct serverOnNetwork *listEntry;
        UA_StatusCode res = UA_DiscoveryManager_addEntryToServersOnNetwork(
            dm, serviceDomainBuf, serviceName, &listEntry);
        if (res == UA_STATUSCODE_GOOD && listEntry) {
            UA_String_copy(&discoveryUrl, &listEntry->serverOnNetwork.discoveryUrl);
            listEntry->srvSet = true;
            listEntry->txtSet = true;

            if (dm->serverOnNetworkCallback) {
                dm->serverOnNetworkCallback(dm, &listEntry->serverOnNetwork,
                                           dm->serverOnNetworkCallbackData);
            }
        }
    }
}

void
UA_Server_setServerOnNetworkCallback(UA_Server *server,
                                     UA_Server_serverOnNetworkCallback cb,
                                     void* data) {
    lockServer(server);
    UA_DiscoveryManager *dm = (UA_DiscoveryManager*)
        getServerComponentByName(server, UA_STRING("discovery"));
    if(dm) {
        dm->serverOnNetworkCallback = cb;
        dm->serverOnNetworkCallbackData = data;
    }
    unlockServer(server);
}

#endif /* UA_ENABLE_DISCOVERY_MULTICAST_MJANSSON */
