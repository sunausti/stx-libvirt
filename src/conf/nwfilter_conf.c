/*
 * nwfilter_conf.c: network filter XML processing
 *                  (derived from storage_conf.c)
 *
 * Copyright (C) 2006-2010 Red Hat, Inc.
 * Copyright (C) 2006-2008 Daniel P. Berrange
 *
 * Copyright (C) 2010 IBM Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: Stefan Berger <stefanb@us.ibm.com>
 */

#include <config.h>

#include <fcntl.h>
#include <dirent.h>
#include <net/ethernet.h>

#include "internal.h"

#include "uuid.h"
#include "memory.h"
#include "virterror_internal.h"
#include "datatypes.h"
#include "nwfilter_params.h"
#include "nwfilter_conf.h"
#include "domain_conf.h"
#include "nwfilter/nwfilter_gentech_driver.h"


#define VIR_FROM_THIS VIR_FROM_NWFILTER

#define virNWFilterError(conn, code, fmt...)                             \
            virReportErrorHelper(conn, VIR_FROM_NWFILTER, code, __FILE__,\
                                  __FUNCTION__, __LINE__, fmt)

VIR_ENUM_IMPL(virNWFilterRuleAction, VIR_NWFILTER_RULE_ACTION_LAST,
              "drop",
              "accept");

VIR_ENUM_IMPL(virNWFilterJumpTarget, VIR_NWFILTER_RULE_ACTION_LAST,
              "DROP",
              "ACCEPT");

VIR_ENUM_IMPL(virNWFilterRuleDirection, VIR_NWFILTER_RULE_DIRECTION_LAST,
              "in",
              "out",
              "inout");

VIR_ENUM_IMPL(virNWFilterChainPolicy, VIR_NWFILTER_CHAIN_POLICY_LAST,
              "ACCEPT",
              "DROP");

VIR_ENUM_IMPL(virNWFilterEbtablesTable, VIR_NWFILTER_EBTABLES_TABLE_LAST,
              "filter",
              "nat",
              "broute");

VIR_ENUM_IMPL(virNWFilterChainSuffix, VIR_NWFILTER_CHAINSUFFIX_LAST,
              "root",
              "arp",
              "ipv4");


/*
 * a map entry for a simple static int-to-string map
 */
struct int_map {
    int32_t attr;
    const char *val;
};


/*
 * only one filter update allowed
 */
static virMutex updateMutex;

static void
virNWFilterLockFilterUpdates(void) {
    virMutexLock(&updateMutex);
}

static void
virNWFilterUnlockFilterUpdates(void) {
    virMutexUnlock(&updateMutex);
}


/*
 * attribute names for the rules XML
 */
static const char srcmacaddr_str[]   = "srcmacaddr";
static const char srcmacmask_str[]   = "srcmacmask";
static const char dstmacaddr_str[]   = "dstmacaddr";
static const char dstmacmask_str[]   = "dstmacmask";
static const char arpsrcmacaddr_str[]= "arpsrcmacaddr";
static const char arpdstmacaddr_str[]= "arpdstmacaddr";
static const char arpsrcipaddr_str[] = "arpsrcipaddr";
static const char arpdstipaddr_str[] = "arpdstipaddr";
static const char srcipaddr_str[]    = "srcipaddr";
static const char srcipmask_str[]    = "srcipmask";
static const char dstipaddr_str[]    = "dstipaddr";
static const char dstipmask_str[]    = "dstipmask";
static const char srcportstart_str[] = "srcportstart";
static const char srcportend_str[]   = "srcportend";
static const char dstportstart_str[] = "dstportstart";
static const char dstportend_str[]   = "dstportend";
static const char dscp_str[]         = "dscp";

#define SRCMACADDR    srcmacaddr_str
#define SRCMACMASK    srcmacmask_str
#define DSTMACADDR    dstmacaddr_str
#define DSTMACMASK    dstmacmask_str
#define ARPSRCMACADDR arpsrcmacaddr_str
#define ARPDSTMACADDR arpdstmacaddr_str
#define ARPSRCIPADDR  arpsrcipaddr_str
#define ARPDSTIPADDR  arpdstipaddr_str
#define SRCIPADDR     srcipaddr_str
#define SRCIPMASK     srcipmask_str
#define DSTIPADDR     dstipaddr_str
#define DSTIPMASK     dstipmask_str
#define SRCPORTSTART  srcportstart_str
#define SRCPORTEND    srcportend_str
#define DSTPORTSTART  dstportstart_str
#define DSTPORTEND    dstportend_str
#define DSCP          dscp_str


/**
 * intMapGetByInt:
 * @intmap: Pointer to int-to-string map
 * @attr: The attribute to look up
 * @res: Pointer to string pointer for result
 *
 * Returns 1 if value was found with result returned, 0 otherwise.
 *
 * lookup a map entry given the integer.
 */
static bool
intMapGetByInt(const struct int_map *intmap, int32_t attr, const char **res)
{
    int i = 0;
    bool found = 0;
    while (intmap[i].val && !found) {
        if (intmap[i].attr == attr) {
            *res = intmap[i].val;
            found = 1;
        }
        i++;
    }
    return found;
}


/**
 * intMapGetByString:
 * @intmap: Pointer to int-to-string map
 * @str: Pointer to string for which to find the entry
 * @casecmp : Whether to ignore case when doing string matching
 * @result: Pointer to int for result
 *
 * Returns 0 if no entry was found, 1 otherwise.
 *
 * Do a lookup in the map trying to find an integer key using the string
 * value. Returns 1 if entry was found with result returned, 0 otherwise.
 */
static bool
intMapGetByString(const struct int_map *intmap, const char *str, int casecmp,
                  int32_t *result)
{
    int i = 0;
    bool found = 0;
    while (intmap[i].val && !found) {
        if ( (casecmp && STRCASEEQ(intmap[i].val, str)) ||
                         STREQ    (intmap[i].val, str)    ) {
            *result = intmap[i].attr;
            found = 1;
        }
        i++;
    }
    return found;
}


void
virNWFilterRuleDefFree(virNWFilterRuleDefPtr def) {
    int i;
    if (!def)
        return;

    for (i = 0; i < def->nvars; i++)
        VIR_FREE(def->vars[i]);

    VIR_FREE(def->vars);

    VIR_FREE(def);
}


static void
virNWFilterIncludeDefFree(virNWFilterIncludeDefPtr inc) {
    if (!inc)
        return;
    virNWFilterHashTableFree(inc->params);
    VIR_FREE(inc->filterref);
    VIR_FREE(inc);
}


static void
virNWFilterEntryFree(virNWFilterEntryPtr entry) {
    if (!entry)
        return;

    virNWFilterRuleDefFree(entry->rule);
    virNWFilterIncludeDefFree(entry->include);
    VIR_FREE(entry);
}


void
virNWFilterDefFree(virNWFilterDefPtr def) {
    int i;
    if (!def)
        return;

    VIR_FREE(def->name);

    for (i = 0; i < def->nentries; i++)
        virNWFilterEntryFree(def->filterEntries[i]);

    VIR_FREE(def->filterEntries);

    VIR_FREE(def);
}


void
virNWFilterPoolObjFree(virNWFilterPoolObjPtr obj) {
    if (!obj)
        return;

    virNWFilterDefFree(obj->def);
    virNWFilterDefFree(obj->newDef);

    VIR_FREE(obj->configFile);

    virMutexDestroy(&obj->lock);

    VIR_FREE(obj);
}


void
virNWFilterPoolObjListFree(virNWFilterPoolObjListPtr pools)
{
    unsigned int i;
    for (i = 0 ; i < pools->count ; i++)
        virNWFilterPoolObjFree(pools->objs[i]);
    VIR_FREE(pools->objs);
    pools->count = 0;
}


static int
virNWFilterRuleDefAddVar(virConnectPtr conn ATTRIBUTE_UNUSED,
                         virNWFilterRuleDefPtr nwf,
                         nwItemDesc *item,
                         const char *var)
{
    int i = 0;

    if (nwf->vars) {
        for (i = 0; i < nwf->nvars; i++)
            if (STREQ(nwf->vars[i], var)) {
                item->var = nwf->vars[i];
                return 0;
            }
    }

    if (VIR_REALLOC_N(nwf->vars, nwf->nvars+1) < 0) {
        virReportOOMError();
        return 1;
    }

    nwf->vars[nwf->nvars] = strdup(var);

    if (!nwf->vars[nwf->nvars]) {
        virReportOOMError();
        return 1;
    }

    item->var = nwf->vars[nwf->nvars++];

    return 0;
}


void
virNWFilterPoolObjRemove(virNWFilterPoolObjListPtr pools,
                         virNWFilterPoolObjPtr pool)
{
    unsigned int i;

    virNWFilterPoolObjUnlock(pool);

    for (i = 0 ; i < pools->count ; i++) {
        virNWFilterPoolObjLock(pools->objs[i]);
        if (pools->objs[i] == pool) {
            virNWFilterPoolObjUnlock(pools->objs[i]);
            virNWFilterPoolObjFree(pools->objs[i]);

            if (i < (pools->count - 1))
                memmove(pools->objs + i, pools->objs + i + 1,
                        sizeof(*(pools->objs)) * (pools->count - (i + 1)));

            if (VIR_REALLOC_N(pools->objs, pools->count - 1) < 0) {
                ; /* Failure to reduce memory allocation isn't fatal */
            }
            pools->count--;

            break;
        }
        virNWFilterPoolObjUnlock(pools->objs[i]);
    }
}



typedef bool (*valueValidator)(enum attrDatatype datatype, void *valptr,
                               virNWFilterRuleDefPtr nwf);
typedef bool (*valueFormatter)(virBufferPtr buf,
                               virNWFilterRuleDefPtr nwf);

typedef struct _virXMLAttr2Struct virXMLAttr2Struct;
struct _virXMLAttr2Struct
{
    const char *name;           // attribute name
    enum attrDatatype datatype;
    int dataIdx;		// offset of the hasXYZ boolean
    valueValidator validator;   // beyond-standard checkers
    valueFormatter formatter;   // beyond-standard formatter
};


static const struct int_map macProtoMap[] = {
    {
      .attr = ETHERTYPE_ARP,
      .val  = "arp",
    }, {
      .attr = ETHERTYPE_IP,
      .val  = "ipv4",
    }, {
      .val  = NULL,
    }
};


static bool
checkMacProtocolID(enum attrDatatype datatype, void *value,
                   virNWFilterRuleDefPtr nwf ATTRIBUTE_UNUSED)
{
    int32_t res = -1;
    const char *str;

    if (datatype == DATATYPE_STRING) {
        if (intMapGetByString(macProtoMap, (char *)value, 1, &res) == 0)
            res = -1;
    } else if (datatype == DATATYPE_UINT16) {
        if (intMapGetByInt(macProtoMap,
                           (int32_t)*(uint16_t *)value, &str) == 0)
            res = -1;
    }

    if (res != -1) {
        nwf->p.ethHdrFilter.dataProtocolID.u.u16 = res;
        return 1;
    }

    return 0;
}


static bool
macProtocolIDFormatter(virBufferPtr buf,
                       virNWFilterRuleDefPtr nwf)
{
    const char *str = NULL;

    if (intMapGetByInt(macProtoMap,
                       nwf->p.ethHdrFilter.dataProtocolID.u.u16,
                       &str)) {
        virBufferVSprintf(buf, "%s", str);
        return 1;
    }
    return 0;
}


/* generic function to check for a valid (ipv4,ipv6, mac) mask
 * A mask is valid of there is a sequence of 1's followed by a sequence
 * of 0s or only 1s or only 0s
 */
static bool
checkValidMask(unsigned char *data, int len)
{
    uint32_t idx = 0;
    uint8_t mask = 0x80;
    int checkones = 1;

    while ((idx >> 3) < len) {
        if (checkones) {
            if (!(data[idx>>3] & mask))
                checkones = 0;
        } else {
            if ((data[idx>>3] & mask))
                return 0;
        }

        idx++;
        mask >>= 1;
        if (!mask)
            mask = 0x80;
    }
    return 1;
}


/* check for a valid IPv4 mask */
static bool
checkIPv4Mask(enum attrDatatype datatype ATTRIBUTE_UNUSED, void *maskptr,
              virNWFilterRuleDefPtr nwf ATTRIBUTE_UNUSED)
{
    return checkValidMask(maskptr, 4);
}


static bool
checkMACMask(enum attrDatatype datatype ATTRIBUTE_UNUSED,
             void *macMask,
             virNWFilterRuleDefPtr nwf ATTRIBUTE_UNUSED)
{
    return checkValidMask((unsigned char *)macMask, 6);
}


static int getMaskNumBits(const unsigned char *mask, int len) {
     int i = 0;
     while (i < (len << 3)) {
         if (!(mask[i>>3] & (0x80 >> (i & 3))))
             break;
         i++;
     }
     return i;
}

/*
 * supported arp opcode -- see 'ebtables -h arp' for the naming
 */
static const struct int_map arpOpcodeMap[] = {
    {
        .attr = 1,
        .val = "Request",
    } , {
        .attr = 2,
        .val = "Reply",
    } , {
        .attr = 3,
        .val = "Request_Reverse",
    } , {
        .attr = 4,
        .val = "Reply_Reverse",
    } , {
        .attr = 5,
        .val = "DRARP_Request",
    } , {
        .attr = 6,
        .val = "DRARP_Reply",
    } , {
        .attr = 7,
        .val = "DRARP_Error",
    } , {
        .attr = 8,
        .val = "InARP_Request",
    } , {
        .attr = 9,
        .val = "ARP_NAK",
    } , {
        .val = NULL,
    }
};


static bool
arpOpcodeValidator(enum attrDatatype datatype,
                   void *value,
                   virNWFilterRuleDefPtr nwf)
{
    int32_t res = -1;
    const char *str;

    if (datatype == DATATYPE_STRING) {
        if (intMapGetByString(arpOpcodeMap, (char *)value, 1, &res) == 0)
            res = -1;
    } else if (datatype == DATATYPE_UINT16) {
        if (intMapGetByInt(arpOpcodeMap,
                           (uint32_t)*(uint16_t *)value, &str) == 0)
            res = -1;
    }

    if (res != -1) {
        nwf->p.arpHdrFilter.dataOpcode.u.u16 = res;
        nwf->p.arpHdrFilter.dataOpcode.datatype = DATATYPE_UINT16;
        return 1;
    }
    return 0;
}


static bool
arpOpcodeFormatter(virBufferPtr buf,
                   virNWFilterRuleDefPtr nwf)
{
    const char *str = NULL;

    if (intMapGetByInt(arpOpcodeMap,
                       nwf->p.arpHdrFilter.dataOpcode.u.u16,
                       &str)) {
        virBufferVSprintf(buf, "%s", str);
        return 1;
    }
    return 0;
}


static const struct int_map ipProtoMap[] = {
    {
        .attr = IPPROTO_TCP,
        .val  = "tcp",
    } , {
        .attr = IPPROTO_UDP,
        .val  = "udp",
    } , {
        .attr = IPPROTO_ICMP,
        .val  = "icmp",
    } , {
        .attr = IPPROTO_IGMP,
        .val  = "igmp",
#ifdef IPPROTO_SCTP
    } , {
        .attr = IPPROTO_SCTP,
        .val  = "sctp",
#endif
    } , {
        .val = NULL,
    }
};


static bool checkIPProtocolID(enum attrDatatype datatype,
                              void *value,
                              virNWFilterRuleDefPtr nwf)
{
    int32_t res = -1;
    const char *str;

    if (datatype == DATATYPE_STRING) {
        if (intMapGetByString(ipProtoMap, (char *)value, 1, &res) == 0)
            res = -1;
    } else if (datatype == DATATYPE_UINT8) {
        // may just accept what user provides and not test...
        if (intMapGetByInt(ipProtoMap,
                           (uint32_t)*(uint16_t *)value, &str) == 0)
            res = -1;
    }

    if (res != -1) {
        nwf->p.ipHdrFilter.ipHdr.dataProtocolID.u.u8 = res;
        nwf->p.ipHdrFilter.ipHdr.dataProtocolID.datatype = DATATYPE_UINT8;
        return 1;
    }
    return 0;
}


static bool
formatIPProtocolID(virBufferPtr buf,
                   virNWFilterRuleDefPtr nwf)
{
    const char *str = NULL;

    if (intMapGetByInt(ipProtoMap,
                       nwf->p.ipHdrFilter.ipHdr.dataProtocolID.u.u8,
                       &str)) {
        virBufferVSprintf(buf, "%s", str);
        return 1;
    }
    return 0;
}


static bool
dscpValidator(enum attrDatatype datatype ATTRIBUTE_UNUSED, void *val,
              virNWFilterRuleDefPtr nwf ATTRIBUTE_UNUSED)
{
    uint8_t dscp = *(uint16_t *)val;
    if (dscp > 63)
        return 0;
    return 1;
}

#define COMMON_MAC_PROPS(STRUCT) \
    {\
        .name = SRCMACADDR,\
        .datatype = DATATYPE_MACADDR,\
        .dataIdx = offsetof(virNWFilterRuleDef,p.STRUCT.ethHdr.dataSrcMACAddr),\
    },\
    {\
        .name = SRCMACMASK,\
        .datatype = DATATYPE_MACMASK,\
        .dataIdx = offsetof(virNWFilterRuleDef, p.STRUCT.ethHdr.dataSrcMACMask),\
    },\
    {\
        .name = DSTMACADDR,\
        .datatype = DATATYPE_MACADDR,\
        .dataIdx = offsetof(virNWFilterRuleDef, p.STRUCT.ethHdr.dataDstMACAddr),\
    },\
    {\
        .name = DSTMACMASK,\
        .datatype = DATATYPE_MACMASK,\
        .dataIdx = offsetof(virNWFilterRuleDef, p.STRUCT.ethHdr.dataDstMACMask),\
    }


static const virXMLAttr2Struct macAttributes[] = {
    COMMON_MAC_PROPS(ethHdrFilter),
    {
        .name = "protocolid",
        .datatype = DATATYPE_UINT16 | DATATYPE_STRING,
        .dataIdx = offsetof(virNWFilterRuleDef, p.ethHdrFilter.dataProtocolID),
        .validator= checkMacProtocolID,
        .formatter= macProtocolIDFormatter,
    },
    {
        .name = NULL,
    }
};

static const virXMLAttr2Struct arpAttributes[] = {
    COMMON_MAC_PROPS(arpHdrFilter),
    {
        .name = "hwtype",
        .datatype = DATATYPE_UINT16,
        .dataIdx = offsetof(virNWFilterRuleDef, p.arpHdrFilter.dataHWType),
    }, {
        .name = "protocoltype",
        .datatype = DATATYPE_UINT16,
        .dataIdx = offsetof(virNWFilterRuleDef, p.arpHdrFilter.dataProtocolType),
    }, {
        .name = "opcode",
        .datatype = DATATYPE_UINT16 | DATATYPE_STRING,
        .dataIdx = offsetof(virNWFilterRuleDef, p.arpHdrFilter.dataOpcode),
        .validator= arpOpcodeValidator,
        .formatter= arpOpcodeFormatter,
    }, {
        .name = ARPSRCMACADDR,
        .datatype = DATATYPE_MACADDR,
        .dataIdx = offsetof(virNWFilterRuleDef, p.arpHdrFilter.dataARPSrcMACAddr),
    }, {
        .name = ARPDSTMACADDR,
        .datatype = DATATYPE_MACADDR,
        .dataIdx = offsetof(virNWFilterRuleDef, p.arpHdrFilter.dataARPDstMACAddr),
    }, {
        .name = ARPSRCIPADDR,
        .datatype = DATATYPE_IPADDR,
        .dataIdx = offsetof(virNWFilterRuleDef, p.arpHdrFilter.dataARPSrcIPAddr),
    }, {
        .name = ARPDSTIPADDR,
        .datatype = DATATYPE_IPADDR,
        .dataIdx = offsetof(virNWFilterRuleDef, p.arpHdrFilter.dataARPDstIPAddr),
    },
    {
        .name = NULL,
    }
};

static const virXMLAttr2Struct ipAttributes[] = {
    COMMON_MAC_PROPS(ipHdrFilter),
    {
        .name = "version",
        .datatype = DATATYPE_UINT8,
        .dataIdx = offsetof(virNWFilterRuleDef, p.ipHdrFilter.ipHdr.dataIPVersion),
    },
    {
        .name = SRCIPADDR,
        .datatype = DATATYPE_IPADDR,
        .dataIdx = offsetof(virNWFilterRuleDef, p.ipHdrFilter.ipHdr.dataSrcIPAddr),
    },
    {
        .name = DSTIPADDR,
        .datatype = DATATYPE_IPADDR,
        .dataIdx = offsetof(virNWFilterRuleDef, p.ipHdrFilter.ipHdr.dataDstIPAddr),
    },
    {
        .name = SRCIPMASK,
        .datatype = DATATYPE_IPMASK,
        .dataIdx = offsetof(virNWFilterRuleDef, p.ipHdrFilter.ipHdr.dataSrcIPMask),
    },
    {
        .name = DSTIPMASK,
        .datatype = DATATYPE_IPMASK,
        .dataIdx = offsetof(virNWFilterRuleDef, p.ipHdrFilter.ipHdr.dataDstIPMask),
    },
    {
        .name = "protocol",
        .datatype = DATATYPE_STRING,
        .dataIdx = offsetof(virNWFilterRuleDef, p.ipHdrFilter.ipHdr.dataProtocolID),
        .validator= checkIPProtocolID,
        .formatter= formatIPProtocolID,
    },
    {
        .name = SRCPORTSTART,
        .datatype = DATATYPE_UINT16,
        .dataIdx = offsetof(virNWFilterRuleDef, p.ipHdrFilter.portData.dataSrcPortStart),
    },
    {
        .name = SRCPORTEND,
        .datatype = DATATYPE_UINT16,
        .dataIdx = offsetof(virNWFilterRuleDef, p.ipHdrFilter.portData.dataSrcPortEnd),
    },
    {
        .name = DSTPORTSTART,
        .datatype = DATATYPE_UINT16,
        .dataIdx = offsetof(virNWFilterRuleDef, p.ipHdrFilter.portData.dataDstPortStart),
    },
    {
        .name = DSTPORTEND,
        .datatype = DATATYPE_UINT16,
        .dataIdx = offsetof(virNWFilterRuleDef, p.ipHdrFilter.portData.dataDstPortEnd),
    },
    {
        .name = DSCP,
        .datatype = DATATYPE_UINT8,
        .dataIdx = offsetof(virNWFilterRuleDef, p.ipHdrFilter.ipHdr.dataDSCP),
        .validator = dscpValidator,
    },
    {
        .name = NULL,
    }
};


typedef struct _virAttributes virAttributes;
struct _virAttributes {
    const char *id;
    const virXMLAttr2Struct *att;
    enum virNWFilterRuleProtocolType prtclType;
};


static const virAttributes virAttr[] = {
    {
        .id = "arp",
        .att = arpAttributes,
        .prtclType = VIR_NWFILTER_RULE_PROTOCOL_ARP,
    }, {
        .id = "mac",
        .att = macAttributes,
        .prtclType = VIR_NWFILTER_RULE_PROTOCOL_MAC,
    }, {
        .id = "ip",
        .att = ipAttributes,
        .prtclType = VIR_NWFILTER_RULE_PROTOCOL_IP,
    }, {
        .id = NULL,
    }
};


static bool
virNWMACAddressParser(const char *input,
                      nwMACAddressPtr output)
{
    if (virParseMacAddr(input, &output->addr[0]) == 0)
        return 1;
    return 0;
}


static bool
virNWIPv4AddressParser(const char *input,
                       nwIPAddressPtr output)
{
    int i;
    char *endptr;
    const char *n = input;
    long int d;

    for (i = 0; i < 4; i++) {
        d = strtol(n, &endptr, 10);
        if (d < 0 || d > 255            ||
            (endptr - n        > 3    ) ||
            (i <= 2 && *endptr != '.' ) ||
            (i == 3 && *endptr != '\0'))
            return 0;
        output->addr.ipv4Addr[i] = (unsigned char)d;
        n = endptr + 1;
    }
    return 1;
}


static int
virNWFilterRuleDetailsParse(virConnectPtr conn ATTRIBUTE_UNUSED,
                            xmlNodePtr node,
                            virNWFilterRuleDefPtr nwf,
                            const virXMLAttr2Struct *att)
{
    int rc = 0;
    int idx = 0;
    char *prop;
    int found = 0;
    enum attrDatatype datatype, att_datatypes;
    enum virNWFilterEntryItemFlags *flags ,match_flag = 0, flags_set = 0;
    nwItemDesc *item;
    int int_val;
    void *data_ptr, *storage_ptr;
    valueValidator validator;
    char *match = virXMLPropString(node, "match");
    nwIPAddress ipaddr;

    if (match && STREQ(match, "no"))
        match_flag = NWFILTER_ENTRY_ITEM_FLAG_IS_NEG;
    VIR_FREE(match);
    match = NULL;

    while (att[idx].name != NULL && rc == 0) {
        prop = virXMLPropString(node, att[idx].name);

        item = (nwItemDesc *)((char *)nwf + att[idx].dataIdx);
        flags = &item->flags;
        flags_set = match_flag;

        if (prop) {
            found = 0;

            validator = NULL;

            if (STRPREFIX(prop, "$")) {
                flags_set |= NWFILTER_ENTRY_ITEM_FLAG_HAS_VAR;
                storage_ptr = NULL;

                if (virNWFilterRuleDefAddVar(conn,
                                             nwf,
                                             item,
                                             &prop[1]))
                    rc = -1;
                found = 1;
            }

            datatype = 1;

            att_datatypes = att[idx].datatype;

            while (datatype <= DATATYPE_LAST && found == 0 && rc == 0) {
                if ((att_datatypes & datatype)) {

                    att_datatypes ^= datatype;

                    validator = att[idx].validator;

                    switch (datatype) {

                        case DATATYPE_UINT8:
                            storage_ptr = &item->u.u8;
                            if (sscanf(prop, "%d", &int_val) == 1) {
                                if (int_val >= 0 && int_val <= 0xff) {
                                    if (!validator)
                                        *(uint8_t *)storage_ptr = int_val;
                                    found = 1;
                                    data_ptr = &int_val;
                                } else
                                    rc = -1;
                            } else
                                rc = -1;
                        break;

                        case DATATYPE_UINT16:
                            storage_ptr = &item->u.u16;
                            if (sscanf(prop, "%d", &int_val) == 1) {
                                if (int_val >= 0 && int_val <= 0xffff) {
                                    if (!validator)
                                        *(uint16_t *)storage_ptr = int_val;
                                    found = 1;
                                    data_ptr = &int_val;
                                } else
                                    rc = -1;
                            } else
                                rc = -1;
                        break;

                        case DATATYPE_IPADDR:
                            storage_ptr = &item->u.ipaddr;
                            if (!virNWIPv4AddressParser(prop,
                                       (nwIPAddressPtr)storage_ptr)) {
                                rc = -1;
                            }
                            found = 1;
                        break;

                        case DATATYPE_IPMASK:
                            storage_ptr = &item->u.u8;
                            if (!virNWIPv4AddressParser(prop, &ipaddr)) {
                                if (sscanf(prop, "%d", &int_val) == 1) {
                                    if (int_val >= 0 && int_val <= 32) {
                                        if (!validator)
                                            *(uint8_t *)storage_ptr =
                                                   (uint8_t)int_val;
                                        found = 1;
                                        data_ptr = &int_val;
                                    } else
                                        rc = -1;
                                } else
                                    rc = -1;
                            } else {
                                if (checkIPv4Mask(datatype,
                                                  ipaddr.addr.ipv4Addr, nwf))
                                    *(uint8_t *)storage_ptr =
                                       getMaskNumBits(ipaddr.addr.ipv4Addr,
                                                 sizeof(ipaddr.addr.ipv4Addr));
                                else
                                    rc = -1;
                                found = 1;
                            }
                        break;

                        case DATATYPE_MACADDR:
                            storage_ptr = &item->u.macaddr;
                            if (!virNWMACAddressParser(prop,
                                        (nwMACAddressPtr)storage_ptr)) {
                                rc = -1;
                            }
                            found = 1;
                        break;

                        case DATATYPE_MACMASK:
                            validator = checkMACMask;
                            storage_ptr = &item->u.macaddr;
                            if (!virNWMACAddressParser(prop,
                                        (nwMACAddressPtr)storage_ptr)) {
                                rc = -1;
                            }
                            data_ptr = storage_ptr;
                            found = 1;
                        break;

                        case DATATYPE_STRING:
                            if (!validator) {
                                // not supported
                                rc = -1;
                                break;
                            }
                            data_ptr = prop;
                            found = 1;
                        break;

                        case DATATYPE_LAST:
                        default:
                        break;
                    }
                }

                if (rc != 0 && att_datatypes != 0) {
                    rc = 0;
                    found = 0;
                }

                datatype <<= 1;
            } /* while */

            if (found == 1 && rc == 0) {
                *flags = NWFILTER_ENTRY_ITEM_FLAG_EXISTS | flags_set;
                item->datatype = datatype >> 1;
                if (validator) {
                    if (!validator(datatype >> 1, data_ptr, nwf)) {
                        rc = -1;
                        *flags = 0;
                    }
                }
            }

            if (!found || rc) {
                virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                                       _("%s has illegal value %s"),
                                       att[idx].name, prop);
                rc = -1;
            }
            VIR_FREE(prop);
        }
        idx++;
    }

    return rc;
}




static virNWFilterIncludeDefPtr
virNWFilterIncludeParse(virConnectPtr conn,
                        xmlNodePtr cur)
{
    virNWFilterIncludeDefPtr ret;

    if (VIR_ALLOC(ret) < 0) {
        virReportOOMError();
        return NULL;
    }

    ret->filterref = virXMLPropString(cur, "filter");
    if (!ret->filterref) {
        virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                               "%s",
                               _("rule node requires action attribute"));
        goto err_exit;
    }

    ret->params = virNWFilterParseParamAttributes(cur);
    if (!ret->params)
        goto err_exit;

cleanup:
    return ret;

err_exit:
    virNWFilterIncludeDefFree(ret);
    ret = NULL;
    goto cleanup;
}


static void
virNWFilterRuleDefFixup(virNWFilterRuleDefPtr rule)
{
#define COPY_NEG_SIGN(A, B) \
    (A).flags = ((A).flags & ~NWFILTER_ENTRY_ITEM_FLAG_IS_NEG) | \
                ((B).flags &  NWFILTER_ENTRY_ITEM_FLAG_IS_NEG);

    switch (rule->prtclType) {
    case VIR_NWFILTER_RULE_PROTOCOL_MAC:
        COPY_NEG_SIGN(rule->p.ethHdrFilter.ethHdr.dataSrcMACMask,
                      rule->p.ethHdrFilter.ethHdr.dataSrcMACAddr);
        COPY_NEG_SIGN(rule->p.ethHdrFilter.ethHdr.dataDstMACMask,
                      rule->p.ethHdrFilter.ethHdr.dataDstMACAddr);
    break;

    case VIR_NWFILTER_RULE_PROTOCOL_IP:
        COPY_NEG_SIGN(rule->p.ipHdrFilter.ipHdr.dataSrcIPMask,
                      rule->p.ipHdrFilter.ipHdr.dataSrcIPAddr);
        COPY_NEG_SIGN(rule->p.ipHdrFilter.ipHdr.dataDstIPMask,
                      rule->p.ipHdrFilter.ipHdr.dataDstIPAddr);
    break;

    case VIR_NWFILTER_RULE_PROTOCOL_ARP:
    case VIR_NWFILTER_RULE_PROTOCOL_NONE:
    break;
    }

#undef COPY_NEG_SIGN
}


static virNWFilterRuleDefPtr
virNWFilterRuleParse(virConnectPtr conn,
                     xmlNodePtr node)
{
    char *action;
    char *direction;
    char *prio;
    int found;
    int found_i;
    unsigned int priority;

    xmlNodePtr cur;
    virNWFilterRuleDefPtr ret;

    if (VIR_ALLOC(ret) < 0) {
        virReportOOMError();
        return NULL;
    }

    action    = virXMLPropString(node, "action");
    direction = virXMLPropString(node, "direction");
    prio      = virXMLPropString(node, "priority");

    if (!action) {
        virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                               "%s",
                               _("rule node requires action attribute"));
        goto err_exit;
    }

    if ((ret->action = virNWFilterRuleActionTypeFromString(action)) < 0) {
        virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                               "%s",
                               _("unknown rule action attribute value"));
        goto err_exit;
    }

    if (!direction) {
        virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                               "%s",
                               _("rule node requires direction attribute"));
        goto err_exit;
    }

    if ((ret->tt = virNWFilterRuleDirectionTypeFromString(direction)) < 0) {
        virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                               "%s",
                               _("unknown rule direction attribute value"));
        goto err_exit;
    }

    ret->priority = MAX_RULE_PRIORITY / 2;

    if (prio) {
        if (sscanf(prio, "%d", (int *)&priority) == 1) {
            if ((int)priority >= 0 && priority <= MAX_RULE_PRIORITY)
                ret->priority = priority;
        }
    }

    cur = node->children;

    found = 0;

    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            int i = 0;
            while (1) {
                if (found)
                    i = found_i;

                if (xmlStrEqual(cur->name, BAD_CAST virAttr[i].id)) {

                    found_i = i;
                    found = 1;
                    ret->prtclType = virAttr[i].prtclType;

                    if (virNWFilterRuleDetailsParse(conn,
                                                    cur,
                                                    ret,
                                                    virAttr[i].att) < 0) {
                        /* we ignore malformed rules
                           goto err_exit;
                        */
                    }
                    break;
                }
                if (!found) {
                    i++;
                    if (!virAttr[i].id)
                        break;
                } else
                   break;
            }
        }

        cur = cur->next;
    }

    virNWFilterRuleDefFixup(ret);

cleanup:
    VIR_FREE(prio);
    VIR_FREE(action);
    VIR_FREE(direction);

    return ret;

err_exit:
    virNWFilterRuleDefFree(ret);
    ret = NULL;
    goto cleanup;
}


static virNWFilterDefPtr
virNWFilterDefParseXML(virConnectPtr conn,
                       xmlXPathContextPtr ctxt) {
    virNWFilterDefPtr ret;
    xmlNodePtr curr = ctxt->node;
    char *uuid = NULL;
    char *chain = NULL;
    virNWFilterEntryPtr entry;

    if (VIR_ALLOC(ret) < 0) {
        virReportOOMError();
        return NULL;
    }

    ret->name = virXPathString("string(./@name)", ctxt);
    if (!ret->name) {
        virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                               "%s", _("filter has no name"));
        goto cleanup;
    }

    ret->chainsuffix = VIR_NWFILTER_CHAINSUFFIX_ROOT;
    chain = virXPathString("string(./@chain)", ctxt);
    if (chain) {
        if ((ret->chainsuffix =
             virNWFilterChainSuffixTypeFromString(chain)) < 0) {
            virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                                   _("unknown chain suffix '%s'"), chain);
            goto cleanup;
        }
    }

    uuid = virXPathString("string(./uuid)", ctxt);
    if (uuid == NULL) {
        if (virUUIDGenerate(ret->uuid) < 0) {
            virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                                  "%s", _("unable to generate uuid"));
            goto cleanup;
        }
    } else {
        if (virUUIDParse(uuid, ret->uuid) < 0) {
            virNWFilterReportError(conn, VIR_ERR_XML_ERROR,
                                  "%s", _("malformed uuid element"));
            goto cleanup;
        }
        VIR_FREE(uuid);
    }

    curr = curr->children;

    while (curr != NULL) {
        if (curr->type == XML_ELEMENT_NODE) {
            if (VIR_ALLOC(entry) < 0) {
                virReportOOMError();
                goto cleanup;
            }

            /* ignore malformed rule and include elements */
            if (xmlStrEqual(curr->name, BAD_CAST "rule"))
                entry->rule = virNWFilterRuleParse(conn, curr);
            else if (xmlStrEqual(curr->name, BAD_CAST "filterref"))
                entry->include = virNWFilterIncludeParse(conn, curr);

            if (entry->rule || entry->include) {
                if (VIR_REALLOC_N(ret->filterEntries, ret->nentries+1) < 0) {
                    VIR_FREE(entry);
                    virReportOOMError();
                    goto cleanup;
                }
                ret->filterEntries[ret->nentries++] = entry;
            } else
                VIR_FREE(entry);
        }
        curr = curr->next;
    }

    VIR_FREE(chain);

    return ret;

 cleanup:
    VIR_FREE(chain);
    VIR_FREE(uuid);
    return NULL;
}


/* Called from SAX on parsing errors in the XML. */
static void
catchXMLError (void *ctx, const char *msg ATTRIBUTE_UNUSED, ...)
{
    xmlParserCtxtPtr ctxt = (xmlParserCtxtPtr) ctx;

    if (ctxt) {
        virConnectPtr conn = ctxt->_private;

        if (conn &&
            conn->err.code == VIR_ERR_NONE &&
            ctxt->lastError.level == XML_ERR_FATAL &&
            ctxt->lastError.message != NULL) {
            virNWFilterReportError(conn, VIR_ERR_XML_DETAIL,
                                   _("at line %d: %s"),
                                   ctxt->lastError.line,
                                   ctxt->lastError.message);
        }
    }
}


virNWFilterDefPtr
virNWFilterDefParseNode(virConnectPtr conn,
                        xmlDocPtr xml,
                        xmlNodePtr root) {
    xmlXPathContextPtr ctxt = NULL;
    virNWFilterDefPtr def = NULL;

    if (STRNEQ((const char *)root->name, "filter")) {
        virNWFilterReportError(conn, VIR_ERR_XML_ERROR,
                               "%s",
                               _("unknown root element for nw filter pool"));
        goto cleanup;
    }

    ctxt = xmlXPathNewContext(xml);
    if (ctxt == NULL) {
        virReportOOMError();
        goto cleanup;
    }

    ctxt->node = root;
    def = virNWFilterDefParseXML(conn, ctxt);

cleanup:
    xmlXPathFreeContext(ctxt);
    return def;
}


static virNWFilterDefPtr
virNWFilterDefParse(virConnectPtr conn,
                     const char *xmlStr,
                     const char *filename) {
    virNWFilterDefPtr ret = NULL;
    xmlParserCtxtPtr pctxt;
    xmlDocPtr xml = NULL;
    xmlNodePtr node = NULL;

    /* Set up a parser context so we can catch the details of XML errors. */
    pctxt = xmlNewParserCtxt ();
    if (!pctxt || !pctxt->sax)
        goto cleanup;
    pctxt->sax->error = catchXMLError;
    pctxt->_private = conn;

    if (conn) virResetError (&conn->err);
    if (filename) {
        xml = xmlCtxtReadFile (pctxt, filename, NULL,
                               XML_PARSE_NOENT | XML_PARSE_NONET |
                               XML_PARSE_NOWARNING);
    } else {
        xml = xmlCtxtReadDoc (pctxt, BAD_CAST xmlStr,
                              "nwfilter.xml", NULL,
                              XML_PARSE_NOENT | XML_PARSE_NONET |
                              XML_PARSE_NOWARNING);
    }

    if (!xml) {
        if (conn && conn->err.code == VIR_ERR_NONE)
              virNWFilterReportError(conn, VIR_ERR_XML_ERROR,
                                     "%s",_("failed to parse xml document"));
        goto cleanup;
    }

    node = xmlDocGetRootElement(xml);
    if (node == NULL) {
        virNWFilterReportError(conn, VIR_ERR_XML_ERROR,
                               "%s", _("missing root element"));
        goto cleanup;
    }

    ret = virNWFilterDefParseNode(conn, xml, node);

    xmlFreeParserCtxt (pctxt);
    xmlFreeDoc(xml);

    return ret;

 cleanup:
    xmlFreeParserCtxt (pctxt);
    xmlFreeDoc(xml);
    return NULL;
}


virNWFilterDefPtr
virNWFilterDefParseString(virConnectPtr conn,
                             const char *xmlStr)
{
    return virNWFilterDefParse(conn, xmlStr, NULL);
}


virNWFilterDefPtr
virNWFilterDefParseFile(virConnectPtr conn,
                        const char *filename)
{
    return virNWFilterDefParse(conn, NULL, filename);
}


virNWFilterPoolObjPtr
virNWFilterPoolObjFindByUUID(virNWFilterPoolObjListPtr pools,
                            const unsigned char *uuid)
{
    unsigned int i;

    for (i = 0 ; i < pools->count ; i++) {
        virNWFilterPoolObjLock(pools->objs[i]);
        if (!memcmp(pools->objs[i]->def->uuid, uuid, VIR_UUID_BUFLEN))
            return pools->objs[i];
        virNWFilterPoolObjUnlock(pools->objs[i]);
    }

    return NULL;
}


virNWFilterPoolObjPtr
virNWFilterPoolObjFindByName(virNWFilterPoolObjListPtr pools,
                            const char *name)
{
    unsigned int i;

    for (i = 0 ; i < pools->count ; i++) {
        virNWFilterPoolObjLock(pools->objs[i]);
        if (STREQ(pools->objs[i]->def->name, name))
            return pools->objs[i];
        virNWFilterPoolObjUnlock(pools->objs[i]);
    }

    return NULL;
}


int virNWFilterSaveXML(virConnectPtr conn,
                       const char *configDir,
                       virNWFilterDefPtr def,
                       const char *xml)
{
    char *configFile = NULL;
    int fd = -1, ret = -1;
    size_t towrite;
    int err;

    if ((configFile = virNWFilterConfigFile(conn, configDir, def->name)) == NULL)
        goto cleanup;

    if ((err = virFileMakePath(configDir))) {
        virReportSystemError(err,
                             _("cannot create config directory '%s'"),
                             configDir);
        goto cleanup;
    }

    if ((fd = open(configFile,
                   O_WRONLY | O_CREAT | O_TRUNC,
                   S_IRUSR | S_IWUSR )) < 0) {
        virReportSystemError(errno,
                             _("cannot create config file '%s'"),
                             configFile);
        goto cleanup;
    }

    towrite = strlen(xml);
    if (safewrite(fd, xml, towrite) < 0) {
        virReportSystemError(errno,
                             _("cannot write config file '%s'"),
                             configFile);
        goto cleanup;
    }

    if (close(fd) < 0) {
        virReportSystemError(errno,
                             _("cannot save config file '%s'"),
                             configFile);
        goto cleanup;
    }

    ret = 0;

 cleanup:
    if (fd != -1)
        close(fd);

    VIR_FREE(configFile);

    return ret;
}


int virNWFilterSaveConfig(virConnectPtr conn,
                          const char *configDir,
                          virNWFilterDefPtr def)
{
    int ret = -1;
    char *xml;

    if (!(xml = virNWFilterDefFormat(conn, def)))
        goto cleanup;

    if (virNWFilterSaveXML(conn, configDir, def, xml))
        goto cleanup;

    ret = 0;
cleanup:
    VIR_FREE(xml);
    return ret;
}


static int
_virNWFilterDefLoopDetect(virConnectPtr conn,
                          virNWFilterPoolObjListPtr pools,
                          virNWFilterDefPtr def,
                          const char *filtername)
{
    int rc = 0;
    int i;
    virNWFilterEntryPtr entry;
    virNWFilterPoolObjPtr obj;

    if (!def)
        return 0;

    for (i = 0; i < def->nentries; i++) {
        entry = def->filterEntries[i];
        if (entry->include) {

            if (STREQ(filtername, entry->include->filterref)) {
                rc = 1;
                break;
            }

            obj = virNWFilterPoolObjFindByName(pools,
                                               entry->include->filterref);
            if (obj) {
                rc = _virNWFilterDefLoopDetect(conn,
                                               pools,
                                               obj->def, filtername);

                virNWFilterPoolObjUnlock(obj);
                if (rc)
                   break;
            }
        }
    }

    return rc;
}


/*
 * virNWFilterDefLoopDetect:
 * @conn: pointer to virConnect object
 * @pools : the pools to search
 * @def : the filter definiton that may add a loop and is to be tested
 *
 * Detect a loop introduced through the filters being able to
 * reference each other.
 *
 * Returns 0 in case no loop was detected, 1 otherwise.
 */
static int
virNWFilterDefLoopDetect(virConnectPtr conn,
                         virNWFilterPoolObjListPtr pools,
                         virNWFilterDefPtr def)
{
    return _virNWFilterDefLoopDetect(conn, pools, def, def->name);
}

int nCallbackDriver;
#define MAX_CALLBACK_DRIVER 10
static virNWFilterCallbackDriverPtr callbackDrvArray[MAX_CALLBACK_DRIVER];

void
virNWFilterRegisterCallbackDriver(virNWFilterCallbackDriverPtr cbd)
{
    if (nCallbackDriver < MAX_CALLBACK_DRIVER) {
        callbackDrvArray[nCallbackDriver++] = cbd;
    }
}


enum UpdateStep {
    STEP_APPLY_NEW,
    STEP_TEAR_NEW,
    STEP_TEAR_OLD,
};

struct cbStruct {
    virConnectPtr conn;
    enum UpdateStep step;
    int err;
};

static void
virNWFilterDomainFWUpdateCB(void *payload,
                            const char *name ATTRIBUTE_UNUSED,
                            void *data)
{
    virDomainObjPtr obj = payload;
    virDomainDefPtr vm = obj->def;
    struct cbStruct *cb = data;
    int i;

    virDomainObjLock(obj);

    if (virDomainObjIsActive(obj)) {
        for (i = 0; i < vm->nnets; i++) {
            virDomainNetDefPtr net = vm->nets[i];
            if ((net->filter) && (net->ifname)) {
                switch (cb->step) {
                case STEP_APPLY_NEW:
                    cb->err = virNWFilterUpdateInstantiateFilter(cb->conn,
                                                                 net);
                    break;

                case STEP_TEAR_NEW:
                    cb->err = virNWFilterRollbackUpdateFilter(cb->conn, net);
                    break;

                case STEP_TEAR_OLD:
                    cb->err = virNWFilterTearOldFilter(cb->conn, net);
                    break;
                }
                if (cb->err)
                    break;
            }
        }
    }

    virDomainObjUnlock(obj);
}


static int
virNWFilterTriggerVMFilterRebuild(virConnectPtr conn)
{
    int i;
    int err;
    struct cbStruct cb = {
        .conn = conn,
        .err = 0,
        .step = STEP_APPLY_NEW,
    };

    for (i = 0; i < nCallbackDriver; i++) {
        callbackDrvArray[i]->vmFilterRebuild(conn,
                                             virNWFilterDomainFWUpdateCB,
                                             &cb);
    }

    err = cb.err;

    if (err) {
        cb.step = STEP_TEAR_NEW; // rollback
        cb.err = 0;

        for (i = 0; i < nCallbackDriver; i++)
            callbackDrvArray[i]->vmFilterRebuild(conn,
                                                 virNWFilterDomainFWUpdateCB,
                                                 &cb);
    } else {
        cb.step = STEP_TEAR_OLD; // switch over

        for (i = 0; i < nCallbackDriver; i++)
            callbackDrvArray[i]->vmFilterRebuild(conn,
                                                 virNWFilterDomainFWUpdateCB,
                                                 &cb);
    }

    return err;
}


int
virNWFilterTestUnassignDef(virConnectPtr conn,
                           virNWFilterPoolObjPtr pool)
{
    int rc = 0;

    virNWFilterLockFilterUpdates();

    pool->wantRemoved = 1;
    // trigger the update on VMs referencing the filter
    if (virNWFilterTriggerVMFilterRebuild(conn))
        rc = 1;

    pool->wantRemoved = 0;
    virNWFilterUnlockFilterUpdates();
    return rc;
}


virNWFilterPoolObjPtr
virNWFilterPoolObjAssignDef(virConnectPtr conn,
                           virNWFilterPoolObjListPtr pools,
                           virNWFilterDefPtr def)
{
    virNWFilterPoolObjPtr pool;

    if (virNWFilterDefLoopDetect(conn, pools, def)) {
        virNWFilterReportError(conn, VIR_ERR_INVALID_NWFILTER,
                              "%s", _("filter would introduce loop"));
        return NULL;
    }

    if ((pool = virNWFilterPoolObjFindByName(pools, def->name))) {
        virNWFilterLockFilterUpdates();
        pool->newDef = def;
        // trigger the update on VMs referencing the filter
        if (virNWFilterTriggerVMFilterRebuild(conn)) {
            pool->newDef = NULL;
            virNWFilterUnlockFilterUpdates();
            virNWFilterPoolObjUnlock(pool);
            return NULL;
        }

        virNWFilterDefFree(pool->def);
        pool->def = def;
        pool->newDef = NULL;
        virNWFilterUnlockFilterUpdates();
        return pool;
    }

    if (VIR_ALLOC(pool) < 0) {
        virReportOOMError();
        return NULL;
    }

    if (virMutexInitRecursive(&pool->lock) < 0) {
        virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("cannot initialize mutex"));
        VIR_FREE(pool);
        return NULL;
    }
    virNWFilterPoolObjLock(pool);
    pool->active = 0;
    pool->def = def;

    if (VIR_REALLOC_N(pools->objs, pools->count+1) < 0) {
        pool->def = NULL;
        virNWFilterPoolObjUnlock(pool);
        virNWFilterPoolObjFree(pool);
        virReportOOMError();
        return NULL;
    }
    pools->objs[pools->count++] = pool;

    return pool;
}


static virNWFilterPoolObjPtr
virNWFilterPoolObjLoad(virConnectPtr conn,
                      virNWFilterPoolObjListPtr pools,
                      const char *file,
                      const char *path)
{
    virNWFilterDefPtr def;
    virNWFilterPoolObjPtr pool;

    if (!(def = virNWFilterDefParseFile(conn, path))) {
        return NULL;
    }

    if (!virFileMatchesNameSuffix(file, def->name, ".xml")) {
        virNWFilterError(conn, VIR_ERR_INVALID_NWFILTER,
            "NWFilter pool config filename '%s' does not match pool name '%s'",
                      path, def->name);
        virNWFilterDefFree(def);
        return NULL;
    }

    if (!(pool = virNWFilterPoolObjAssignDef(conn, pools, def))) {
        virNWFilterDefFree(def);
        return NULL;
    }

    pool->configFile = strdup(path);
    if (pool->configFile == NULL) {
        virReportOOMError();
        virNWFilterDefFree(def);
        return NULL;
    }

    return pool;
}


int
virNWFilterPoolLoadAllConfigs(virConnectPtr conn,
                             virNWFilterPoolObjListPtr pools,
                             const char *configDir)
{
    DIR *dir;
    struct dirent *entry;

    if (!(dir = opendir(configDir))) {
        if (errno == ENOENT) {
            return 0;
        }
        virReportSystemError(errno, _("Failed to open dir '%s'"),
                             configDir);
        return -1;
    }

    while ((entry = readdir(dir))) {
        char path[PATH_MAX];
        virNWFilterPoolObjPtr pool;

        if (entry->d_name[0] == '.')
            continue;

        if (!virFileHasSuffix(entry->d_name, ".xml"))
            continue;

        if (virFileBuildPath(configDir, entry->d_name,
                             NULL, path, PATH_MAX) < 0) {
            virNWFilterError(conn, VIR_ERR_INTERNAL_ERROR,
                            "Config filename '%s/%s' is too long",
                            configDir, entry->d_name);
            continue;
        }

        pool = virNWFilterPoolObjLoad(conn, pools, entry->d_name, path);
        if (pool)
            virNWFilterPoolObjUnlock(pool);
    }

    closedir(dir);

    return 0;
}


int
virNWFilterPoolObjSaveDef(virConnectPtr conn,
                         virNWFilterDriverStatePtr driver,
                         virNWFilterPoolObjPtr pool,
                         virNWFilterDefPtr def)
{
    char *xml;
    int fd = -1, ret = -1;
    ssize_t towrite;

    if (!pool->configFile) {
        int err;
        char path[PATH_MAX];

        if ((err = virFileMakePath(driver->configDir))) {
            virReportSystemError(err,
                                 _("cannot create config directory %s"),
                                 driver->configDir);
            return -1;
        }

        if (virFileBuildPath(driver->configDir, def->name, ".xml",
                             path, sizeof(path)) < 0) {
            virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                                  "%s", _("cannot construct config file path"));
            return -1;
        }
        if (!(pool->configFile = strdup(path))) {
            virReportOOMError();
            return -1;
        }
    }

    if (!(xml = virNWFilterDefFormat(conn, def))) {
        virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                              "%s", _("failed to generate XML"));
        return -1;
    }

    if ((fd = open(pool->configFile,
                   O_WRONLY | O_CREAT | O_TRUNC,
                   S_IRUSR | S_IWUSR )) < 0) {
        virReportSystemError(errno,
                             _("cannot create config file %s"),
                             pool->configFile);
        goto cleanup;
    }

    towrite = strlen(xml);
    if (safewrite(fd, xml, towrite) != towrite) {
        virReportSystemError(errno,
                             _("cannot write config file %s"),
                             pool->configFile);
        goto cleanup;
    }

    if (close(fd) < 0) {
        virReportSystemError(errno,
                             _("cannot save config file %s"),
                             pool->configFile);
        goto cleanup;
    }

    ret = 0;

 cleanup:
    if (fd != -1)
        close(fd);

    VIR_FREE(xml);

    return ret;
}


int
virNWFilterPoolObjDeleteDef(virConnectPtr conn,
                           virNWFilterPoolObjPtr pool)
{
    if (!pool->configFile) {
        virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                              _("no config file for %s"), pool->def->name);
        return -1;
    }

    if (unlink(pool->configFile) < 0) {
        virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                              _("cannot remove config for %s"),
                              pool->def->name);
        return -1;
    }

    return 0;
}


static void
virNWIPAddressFormat(virBufferPtr buf, nwIPAddressPtr ipaddr)
{
    if (!ipaddr->isIPv6) {
        virBufferVSprintf(buf, "%d.%d.%d.%d",
                          ipaddr->addr.ipv4Addr[0],
                          ipaddr->addr.ipv4Addr[1],
                          ipaddr->addr.ipv4Addr[2],
                          ipaddr->addr.ipv4Addr[3]);
    } else {
        virBufferAddLit(buf, "MISSING IPv6 ADDRESS FORMATTER");
    }
}


static void
virNWFilterRuleDefDetailsFormat(virConnectPtr conn,
                                virBufferPtr buf,
                                const char *type,
                                const virXMLAttr2Struct *att,
                                virNWFilterRuleDefPtr def)
{
    int i, j;
    bool typeShown = 0;
    bool neverShown = 1;
    enum match {
        MATCH_NONE = 0,
        MATCH_YES,
        MATCH_NO
    } matchShown = MATCH_NONE;
    nwItemDesc *item;

    while (att[i].name) {
        item = (nwItemDesc *)((char *)def + att[i].dataIdx);
        enum virNWFilterEntryItemFlags flags = item->flags;
        void *storage_ptr;
        if ((flags & NWFILTER_ENTRY_ITEM_FLAG_EXISTS)) {
            if (!typeShown) {
                virBufferVSprintf(buf, "    <%s", type);
                typeShown = 1;
                neverShown = 0;
            }

            if ((flags & NWFILTER_ENTRY_ITEM_FLAG_IS_NEG)) {
                if (matchShown == MATCH_NONE) {
                    virBufferAddLit(buf, " match='no'");
                    matchShown = MATCH_NO;
                } else if (matchShown == MATCH_YES) {
                    virBufferAddLit(buf, "/>\n");
                    typeShown = 0;
                    matchShown = MATCH_NONE;
                    continue;
                }
            } else {
                if (matchShown == MATCH_NO) {
                    virBufferAddLit(buf, "/>\n");
                    typeShown = 0;
                    matchShown = MATCH_NONE;
                    continue;
                }
                matchShown = MATCH_YES;
            }

            virBufferVSprintf(buf, " %s='",
                              att[i].name);
            if (att[i].formatter) {
               if (!att[i].formatter(buf, def)) {
                  virNWFilterReportError(conn, VIR_ERR_INTERNAL_ERROR,
                                         _("formatter for %s %s reported error"),
                                         type,
                                         att[i].name);
                   goto err_exit;
               }
            } else if ((flags & NWFILTER_ENTRY_ITEM_FLAG_HAS_VAR)) {
                virBufferVSprintf(buf, "$%s", item->var);
            } else {
               switch (att[i].datatype) {

               case DATATYPE_IPMASK:
                   // display all masks in CIDR format
               case DATATYPE_UINT8:
                   storage_ptr = &item->u.u8;
                   virBufferVSprintf(buf, "%d", *(uint8_t *)storage_ptr);
               break;

               case DATATYPE_UINT16:
                   storage_ptr = &item->u.u16;
                   virBufferVSprintf(buf, "%d", *(uint16_t *)storage_ptr);
               break;

               case DATATYPE_IPADDR:
                   storage_ptr = &item->u.ipaddr;
                   virNWIPAddressFormat(buf,
                                        (nwIPAddressPtr)storage_ptr);
               break;

               case DATATYPE_MACMASK:
               case DATATYPE_MACADDR:
                   storage_ptr = &item->u.macaddr;
                   for (j = 0; j < 6; j++)
                       virBufferVSprintf(buf, "%02x%s",
                                      ((nwMACAddressPtr)storage_ptr)->addr[j],
                                      (j < 5) ? ":" : "");
               break;

               case DATATYPE_STRING:
               default:
                   virBufferVSprintf(buf,
                                     "UNSUPPORTED DATATYPE 0x%02x\n",
                                     att[i].datatype);
               }
            }
            virBufferAddLit(buf, "'");
        }
        i++;
    }
    if (typeShown)
       virBufferAddLit(buf, "/>\n");

    if (neverShown)
       virBufferVSprintf(buf,
                         "    <%s/>\n", type);

err_exit:
    return;
}


static char *
virNWFilterRuleDefFormat(virConnectPtr conn,
                         virNWFilterRuleDefPtr def)
{
    int i;
    virBuffer buf  = VIR_BUFFER_INITIALIZER;
    virBuffer buf2 = VIR_BUFFER_INITIALIZER;
    char *data;

    virBufferVSprintf(&buf, "  <rule action='%s' direction='%s' priority='%d'",
                      virNWFilterRuleActionTypeToString(def->action),
                      virNWFilterRuleDirectionTypeToString(def->tt),
                      def->priority);

    i = 0;
    while (virAttr[i].id) {
        if (virAttr[i].prtclType == def->prtclType) {
            virNWFilterRuleDefDetailsFormat(conn,
                                            &buf2,
                                            virAttr[i].id,
                                            virAttr[i].att,
                                            def);
            break;
        }
        i++;
    }

    if (virBufferError(&buf2))
        goto no_memory;

    data = virBufferContentAndReset(&buf2);

    if (data) {
        virBufferAddLit(&buf, ">\n");
        virBufferVSprintf(&buf, "%s  </rule>\n", data);
        VIR_FREE(data);
    } else
        virBufferAddLit(&buf, "/>\n");

    if (virBufferError(&buf))
        goto no_memory;

    return virBufferContentAndReset(&buf);

no_memory:
    virReportOOMError();
    virBufferFreeAndReset(&buf);
    virBufferFreeAndReset(&buf2);

    return NULL;
}


static char *
virNWFilterIncludeDefFormat(virNWFilterIncludeDefPtr inc)
{
    char *attrs;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    virBufferVSprintf(&buf,"  <filterref filter='%s'",
                      inc->filterref);

    attrs = virNWFilterFormatParamAttributes(inc->params, "    ");

    if (!attrs || strlen(attrs) <= 1)
        virBufferAddLit(&buf, "/>\n");
    else
        virBufferVSprintf(&buf, ">\n%s  </filterref>\n", attrs);

    if (virBufferError(&buf)) {
        virReportOOMError();
        virBufferFreeAndReset(&buf);
        return NULL;
    }

    return virBufferContentAndReset(&buf);
}


static char *
virNWFilterEntryFormat(virConnectPtr conn,
                       virNWFilterEntryPtr entry)
{
    if (entry->rule)
        return virNWFilterRuleDefFormat(conn, entry->rule);
    return virNWFilterIncludeDefFormat(entry->include);
}


char *
virNWFilterDefFormat(virConnectPtr conn,
                     virNWFilterDefPtr def)
{
    virBuffer buf = VIR_BUFFER_INITIALIZER;
    char uuid[VIR_UUID_STRING_BUFLEN];
    int i;
    char *xml;

    virBufferVSprintf(&buf, "<filter name='%s' chain='%s'",
                      def->name,
                      virNWFilterChainSuffixTypeToString(def->chainsuffix));
    virBufferAddLit(&buf, ">\n");

    virUUIDFormat(def->uuid, uuid);
    virBufferVSprintf(&buf,"  <uuid>%s</uuid>\n", uuid);

    for (i = 0; i < def->nentries; i++) {
        xml = virNWFilterEntryFormat(conn, def->filterEntries[i]);
        if (!xml)
            goto err_exit;
        virBufferVSprintf(&buf, "%s", xml);
        VIR_FREE(xml);
    }

    virBufferAddLit(&buf, "</filter>\n");

    if (virBufferError(&buf))
        goto no_memory;

    return virBufferContentAndReset(&buf);

 no_memory:
    virReportOOMError();

 err_exit:
    virBufferFreeAndReset(&buf);
    return NULL;
}


char *virNWFilterConfigFile(virConnectPtr conn ATTRIBUTE_UNUSED,
                            const char *dir,
                            const char *name)
{
    char *ret = NULL;

    if (virAsprintf(&ret, "%s/%s.xml", dir, name) < 0) {
        virReportOOMError();
        return NULL;
    }

    return ret;
}


int virNWFilterConfLayerInit(void)
{
    if (virMutexInit(&updateMutex))
        return 1;

    if (virNWFilterParamConfLayerInit())
        return 1;

    return 0;
}


void virNWFilterConfLayerShutdown(void)
{
    virNWFilterParamConfLayerShutdown();
}


void virNWFilterPoolObjLock(virNWFilterPoolObjPtr obj)
{
    virMutexLock(&obj->lock);
}

void virNWFilterPoolObjUnlock(virNWFilterPoolObjPtr obj)
{
    virMutexUnlock(&obj->lock);
}
