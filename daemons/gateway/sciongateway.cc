#include <stdint.h>
#include <syslog.h>
#include <errno.h>
#include <map>
#include <string>
using namespace std;


#include "clicknetxia.h"
#include "Xsocket.h"
#include "Xsecurity.h"
#include "dagaddr.hpp"
#include "Xkeys.h"
#include "scion.h"

#define DEFAULT_NAME "host0"
#define APPNAME "sciongateway0"

#define RV_MAX_DATA_PACKET_SIZE 16384
#define RV_MAX_CONTROL_PACKET_SIZE 1024

map<std::string, std::string> name_to_dag_db_table; // map name to dag

char *hostname = NULL;
char *ident = NULL;
char *datasid = NULL;
char *controlsid = NULL;
map<string, string> HIDtoAD;
map<string, double> HIDtoTimestamp;
map<string, double>::iterator HIDtoTimestampIterator;

void help(const char *name)
{
	printf("\nusage: %s [-l level] [-v] [-d SID -c SID][-h hostname]\n", name);
	printf("where:\n");
	printf(" -c SID      : SID for rendezvous control plane.\n");
	printf(" -d SID      : SID for rendezvous data plane.\n");
	printf(" -l level    : syslog logging level 0 = LOG_EMERG ... 7 = LOG_DEBUG (default=3:LOG_ERR)\n");
	printf(" -v          : log to the console as well as syslog\n");
	printf(" -h hostname : click device name (default=host0)\n");
	printf("\n");
	exit(0);
}

// Allocates memory and creates a new string containing SID in that space
// Caller must free the returned pointer if not NULL
char *createRandomSIDifNeeded(char *sidptr)
{
	char *sid;
	// In future we can check if keys matching the SID exist
	if(sidptr != NULL) {
		return sidptr;
	}
	syslog(LOG_INFO, "No SID provided. Creating a new one");
	int sidlen = strlen("SID:") + XIA_SHA_DIGEST_STR_LEN;
	sid = (char *)calloc(sidlen, 1);
	if(!sid) {
		syslog(LOG_ERR, "ERROR allocating memory for SID\n");
		return NULL;
	}
	if(XmakeNewSID(sid, sidlen)) {
		syslog(LOG_ERR, "ERROR autogenerating SID\n");
		return NULL;
	}
	syslog(LOG_DEBUG, "New service ID:%s: created", sid);
	return sid;
}

#define MAX_RV_DAG_SIZE 2048

void config(int argc, char** argv)
{
	int c;
	unsigned level = 3;
	int verbose = 0;

	opterr = 0;

	while ((c = getopt(argc, argv, "h:l:d:c:v")) != -1) {
		switch (c) {
			case 'h':
				hostname = strdup(optarg);
				break;
			case 'l':
				level = MIN(atoi(optarg), LOG_DEBUG);
				break;
			case 'd':
				datasid = strdup(optarg);
				break;
			case 'c':
				controlsid = strdup(optarg);
				break;
			case 'v':
				verbose = LOG_PERROR;
				break;
			case '?':
			default:
				// Help Me!
				help(basename(argv[0]));
				break;
		}
	}

	if (!hostname)
		hostname = strdup(DEFAULT_NAME);

	// Read Data plane SID from resolv.conf, if needed
	if(datasid == NULL) {
		char data_plane_DAG[MAX_RV_DAG_SIZE];
		//if(XreadRVServerAddr(data_plane_DAG, MAX_RV_DAG_SIZE) == 0) {
                if(XreadScionSID(data_plane_DAG, MAX_RV_DAG_SIZE) == 0) {
			char *sid = strstr(data_plane_DAG, "SID:");
			datasid = (char *)calloc(strlen(sid) + 1, 1);
			if(!datasid) {
				syslog(LOG_ERR, "ERROR allocating memory for SID\n");
			} else {
				strcpy(datasid, sid);
				syslog(LOG_INFO, "Data plane: %s", datasid);
			}
		}
	}

	// Read Control plane SID from resolv.conf, if needed
	if(controlsid == NULL) {
		char control_plane_DAG[MAX_RV_DAG_SIZE];
		if(XreadRVServerControlAddr(control_plane_DAG, MAX_RV_DAG_SIZE) == 0) {
			char *sid = strstr(control_plane_DAG, "SID:");
			controlsid = (char *)calloc(strlen(sid) + 1, 1);
			if(!controlsid) {
				syslog(LOG_ERR, "ERROR allocating memory for SID\n");
			} else {
				strcpy(controlsid, sid);
				syslog(LOG_INFO, "Control plane: %s", controlsid);
			}
		}
	}

	// Create random SIDs if not provided in resolv.conf or command line
	datasid = createRandomSIDifNeeded(datasid);
	controlsid = createRandomSIDifNeeded(controlsid);

        syslog(LOG_DEBUG, "DEBUG: create SID is done");
	// Terminate, if we don't have SIDs for control and data plane
	if(datasid == NULL || controlsid == NULL) {
		syslog(LOG_ERR, "ERROR: Unable to generate new service IDs");
		exit(1);
	}

	// load the config setting for this hostname
	set_conf("xsockconf.ini", hostname);

	// note: ident must exist for the life of the app
	ident = (char *)calloc(strlen (APPNAME) + 4, 1);
	sprintf(ident, "%s", APPNAME);
	openlog(ident, LOG_CONS|LOG_NDELAY|verbose, LOG_LOCAL4);
	setlogmask(LOG_UPTO(level));
}

int getServerSocket(char *sid, int type)
{
	int sockfd = Xsocket(AF_XIA, type, 0);
	if (sockfd < 0) {
   		syslog(LOG_ALERT, "Unable to create a socket for %s", sid);
   		return -1;
	}

	struct addrinfo *ai;
	if (Xgetaddrinfo(NULL, sid, NULL, &ai) != 0) {
   		syslog(LOG_ALERT, "unable to get local address");
		return -2;
	}

	sockaddr_x *sa = (sockaddr_x*)ai->ai_addr;
	Graph g(sa);
	syslog(LOG_INFO, "binding to local DAG: %s", g.dag_string().c_str());

	// Data plane socket binding to the SID
	if (Xbind(sockfd, (struct sockaddr*)sa, sizeof(sockaddr_x)) < 0) {
  		syslog(LOG_ALERT, "unable to bind to local DAG : %s", g.dag_string().c_str());
   		return -3;
	}
	return sockfd;
}

void print_packet_contents(char *packet, int len)
{
  syslog(LOG_INFO, "print pakcet len %d", len);
	int hex_string_len = (len*2) + 1;
	char hex_string[hex_string_len];
    int i;
    uint8_t* data = (uint8_t*)packet;
    bzero(hex_string, hex_string_len);
    for(i=0;i<len;i++) {
        sprintf(&hex_string[2*i], "%02x", (unsigned int)data[i]);
    }
    hex_string[hex_string_len-1] = '\0';
	syslog(LOG_INFO, "Packet contents|%s|", hex_string);
}

int print_packet_header(click_xia *xiah)
{
	syslog(LOG_INFO, "======= RAW PACKET HEADER ========");
	syslog(LOG_INFO, "ver:%d", xiah->ver);
	syslog(LOG_INFO, "nxt:%d", xiah->nxt);
	syslog(LOG_INFO, "plen:%d, %d", ntohs(xiah->plen));
	syslog(LOG_INFO, "hlim:%d", xiah->hlim);
	syslog(LOG_INFO, "dnode:%d", xiah->dnode);
	syslog(LOG_INFO, "snode:%d", xiah->snode);
	syslog(LOG_INFO, "last:%d", xiah->last);
	int total_nodes = xiah->dnode + xiah->snode;
	for(int i=0;i<total_nodes;i++) {
		uint8_t id[20];
		char hex_string[41];
		bzero(hex_string, 41);
		memcpy(id, xiah->node[i].xid.id, 20);
		for(int j=0;j<20;j++) {
			sprintf(&hex_string[2*j], "%02x", (unsigned int)id[j]);
		}
		char type[10];
		bzero(type, 10);
		switch (htonl(xiah->node[i].xid.type)) {
			case CLICK_XIA_XID_TYPE_AD:
				strcpy(type, "AD");
				break;
			case CLICK_XIA_XID_TYPE_HID:
				strcpy(type, "HID");
				break;
			case CLICK_XIA_XID_TYPE_SID:
				strcpy(type, "SID");
				break;
			case CLICK_XIA_XID_TYPE_CID:
				strcpy(type, "CID");
				break;
			case CLICK_XIA_XID_TYPE_IP:
				strcpy(type, "4ID");
				break;
		        case CLICK_XIA_XID_TYPE_SCIONID:
				strcpy(type, "SCION");
				break;
			default:
				sprintf(type, "%d", xiah->node[i].xid.type);
		};
		syslog(LOG_INFO, "%s:%s", type, hex_string);
	}
        
        return total_nodes;
}

#ifdef SCION_PACKET

int my_isd = 0; // ISD ID of this router
int my_ad = 0;  // AD ID of this router
int neighbor_isd = 1; // ISD ID of a neighbor AD
int neighbor_ad = 1; // AD ID of a neighbor AD

#define MAX_DPDK_PORT 16
uint32_t interface_ip[MAX_DPDK_PORT]; // current router IP address (TODO: IPv6)

#define INGRESS_IF(HOF)                                                        \
  (ntohl((HOF)->ingress_egress_if) >>                                          \
   (12 +                                                                       \
    8)) // 12bit is  egress if and 8 bit gap between uint32 and 24bit field
#define EGRESS_IF(HOF) ((ntohl((HOF)->ingress_egress_if) >> 8) & 0x000fff)
#define IN_EGRESS_IF(EN, IN) (((IN)&0x000fff) | (((EN)<<12)&0xfff000))



void build_addr_hdr(SCIONCommonHeader *sch, SCIONAddr *src, SCIONAddr *dst)
{
  int src_len = 16;
  int dst_len = 16;
  int pad = (SCION_ADDR_PAD - ((src_len + dst_len) % 8)) % 8;
  uint8_t *ptr = (uint8_t *)sch + sizeof(*sch);
  *(uint32_t *)ptr = htonl(src->isd_ad);
  ptr += 4;
  //memcpy(ptr, src->host_addr, src_len);
  ptr += src_len;
  *(uint32_t *)ptr = htonl(dst->isd_ad);
  ptr += 4;
  //memcpy(ptr, dst->host_addr, dst_len);
  sch->headerLen += src_len + dst_len + 8 + pad;
  sch->totalLen = htons(sch->headerLen);
}

void build_cmn_hdr(SCIONCommonHeader *sch, int src_type, int dst_type, int next_hdr)
{
  uint16_t vsd = 0;
  vsd |= src_type << 6;
  vsd |= dst_type;
  sch->versionSrcDst = htons(vsd);
  sch->nextHeader = next_hdr;
  sch->headerLen = sizeof(*sch);
  sch->currentIOF = 0;
  sch->currentOF = 0;
  sch->totalLen = htons(sch->headerLen);
  return;
}

size_t add_scion_header(SCIONCommonHeader* sch, uint8_t* payload, uint16_t payload_len) {
  uint8_t *ptr;
  uint8_t path_length;
  uint8_t header_length;
  SCIONAddr src_addr, dst_addr;
  
  //data packet uses IPV4 although XIA-SCION does not use it at all.
  //it is used to distinglish data or control packet
  build_cmn_hdr(sch, ADDR_IPV4_TYPE, ADDR_IPV4_TYPE, L4_UDP);  

  //Fill in SCION addresses - 
  // we need to isd and ad information for routing.
  // but we do not need the address info
  src_addr.isd_ad = ISD_AD(my_isd, my_ad);
  //*(uint32_t *)(src_addr.host_addr) = interface_ip[0];
  dst_addr.isd_ad = ISD_AD(neighbor_isd, neighbor_ad);
  //*(uint32_t *)(dst_addr.host_addr) = interface_ip[0];
  build_addr_hdr(sch, &src_addr, &dst_addr);


  /* construct path info */
  //path information - gateway should call path server to get the path info
  InfoOpaqueField up_inf = {IOF_CORE, htonl(1111), htons(1), 2};
  HopOpaqueField up_hops = {HOP_NORMAL_OF, 111, IN_EGRESS_IF(12, 45), htonl(0x010203)};
  HopOpaqueField up_hops_next = {HOP_NORMAL_OF, 111, IN_EGRESS_IF(78, 98), htonl(0x010203)};
  InfoOpaqueField core_inf = {IOF_CORE, htonl(2222), htons(1), 2};
  HopOpaqueField core_hops = {HOP_NORMAL_OF, 111, htonl(IN_EGRESS_IF(11, 22)), htonl(0x010203)};
  HopOpaqueField core_hops_next = {HOP_NORMAL_OF, 111, htonl(IN_EGRESS_IF(33, 44)), htonl(0x010203)};
  InfoOpaqueField dw_inf = {IOF_CORE, htonl(3333), htons(1), 2};
  HopOpaqueField dw_hops = {HOP_NORMAL_OF, 111, htonl(IN_EGRESS_IF(12, 45)), htonl(0x010203)};
  HopOpaqueField dw_hops_next = {HOP_NORMAL_OF, 111, htonl(IN_EGRESS_IF(78, 78)), htonl(0x010203)};  


  ptr = (uint8_t *)sch + sizeof(SCIONCommonHeader) + sizeof(SCIONAddr) + sizeof(SCIONAddr);

  sch->currentOF = sizeof(SCIONCommonHeader) + sizeof(SCIONAddr) + sizeof(SCIONAddr) + sizeof(up_inf);
  
  syslog(LOG_INFO, "scion currentOF is %d", sch->currentOF);

  size_t offset = 0;
  //add path info to SCION header
  memcpy(ptr, (void*)&up_inf, sizeof(up_inf));
  offset += sizeof(up_inf);
  memcpy(ptr + offset, (void*)&up_hops, sizeof(up_hops));
  offset += sizeof(up_hops);
  memcpy(ptr + offset, (void*)&up_hops_next, sizeof(up_hops_next));
  offset += sizeof(up_hops_next);
  memcpy(ptr + offset, (void*)&core_inf, sizeof(core_inf));
  offset += sizeof(core_inf);
  memcpy(ptr + offset, (void*)&core_hops, sizeof(core_hops));
  offset += sizeof(core_hops);
  memcpy(ptr + offset, (void*)&core_hops_next, sizeof(core_hops_next));
  offset += sizeof(core_hops_next);
  memcpy(ptr + offset, (void*)&dw_inf, sizeof(dw_inf));
  offset += sizeof(dw_inf);
  memcpy(ptr + offset, (void*)&dw_hops, sizeof(dw_hops));
  offset += sizeof(dw_hops);
  memcpy(ptr + offset, (void*)&dw_hops_next, sizeof(dw_hops_next));
  offset += sizeof(dw_hops_next);
  ptr += offset;
  path_length = offset;
  *(size_t *)ptr = htons(path_length); /*path length*/
  path_length += 4;

  header_length = sizeof(SCIONCommonHeader) +
    sizeof(SCIONAddr) + sizeof(SCIONAddr) + path_length;
  sch->headerLen = header_length;

  printf("path length is %d bytes\n", (int)path_length);
  
  memcpy(((uint8_t*)sch +  header_length), payload, payload_len);

  sch->totalLen = htons(header_length + payload_len);
  
    
  syslog(LOG_INFO, "after modification scion currentOF is %d", sch->currentOF);

  return header_length;
}

void print_scion_header(SCIONCommonHeader *sch){
    syslog(LOG_INFO, "print scion common header:");
    syslog(LOG_INFO, "versionSrcDst : %d", ntohs(sch->versionSrcDst));
    syslog(LOG_INFO, "totalLen: %d", ntohs(sch->totalLen));
    syslog(LOG_INFO, "currentIOF: %d", sch->currentIOF);
    syslog(LOG_INFO, "currentOF: %d", sch->currentOF);
    syslog(LOG_INFO, "nextHeader: %d", sch->nextHeader);
    syslog(LOG_INFO, "headerLen: %d", sch->headerLen);
    return;
}

size_t add_scion_ext_header(click_xia_ext* xia_ext_header, uint8_t* packet_payload, uint16_t payload_len){
  SCIONCommonHeader *scion_common_header = (SCIONCommonHeader*)((uint8_t*)xia_ext_header + 2);
  size_t scion_len = add_scion_header(scion_common_header, packet_payload, payload_len);
  xia_ext_header->hlen = (uint8_t)scion_len + 2; /*hlen is uint8 now*/ 
  xia_ext_header->nxt = CLICK_XIA_NXT_NO;
  syslog(LOG_INFO, "scion ext header length %d", xia_ext_header->hlen);
  print_scion_header((SCIONCommonHeader*)((uint8_t*)xia_ext_header + 2));
  print_packet_contents((char*)xia_ext_header, xia_ext_header->hlen); 
  return xia_ext_header->hlen;
}

//
//add scion header as xia extension header
//
void add_scion_info(click_xia *xiah, int xia_hdr_len)
{
  uint8_t packet_data[RV_MAX_DATA_PACKET_SIZE];
  uint8_t* original_data;
  //int total_nodes = xiah->dnode + xiah->snode;
  uint16_t original_payload_len = ntohs(xiah->plen);
  short data_len = 0;
  uint8_t nxt_hdr_type = xiah->nxt;
  uint8_t* packet = reinterpret_cast<uint8_t *>(xiah);
  //uint16_t xia_hdr_len =  sizeof(struct click_xia) + total_nodes*sizeof(struct click_xia_xid_node);
  uint16_t hdr_len = xia_hdr_len;
  struct click_xia_ext* xia_ext_hdr = reinterpret_cast<struct click_xia_ext *>(packet + hdr_len);
  
  if(nxt_hdr_type != CLICK_XIA_NXT_NO){
    syslog(LOG_INFO, "next header type %d, xia hdr len %d ", (int)nxt_hdr_type, hdr_len);
    nxt_hdr_type = xia_ext_hdr->nxt;

    while(nxt_hdr_type != CLICK_XIA_NXT_NO){
      syslog(LOG_INFO, "next header type %d, hdr len %d, xia hdr len %d ", (int)nxt_hdr_type, (int)xia_ext_hdr->hlen, hdr_len);
      hdr_len += xia_ext_hdr->hlen;
      xia_ext_hdr = reinterpret_cast<struct click_xia_ext *>(packet + hdr_len);
      nxt_hdr_type = xia_ext_hdr->nxt;
    }

    hdr_len += xia_ext_hdr->hlen;
  }

  original_data = packet + hdr_len;
  data_len = xia_hdr_len + original_payload_len - hdr_len; 

  syslog(LOG_INFO, "next header type %d, hdr len %d, data len %d, original payload len %d", (int)nxt_hdr_type, hdr_len, (int)data_len, original_payload_len);

  assert(data_len >= 0);

  memcpy(packet_data, original_data, data_len);
  // add SCION header
  memset(packet + hdr_len, 0, data_len);

  xia_ext_hdr->nxt = CLICK_XIA_NXT_SCION;
  
  //now add/configure SCION ext header
  xia_ext_hdr = reinterpret_cast<struct click_xia_ext *>(packet + hdr_len);
  xia_ext_hdr->nxt = CLICK_XIA_NXT_NO;

  syslog(LOG_INFO, "add scion hdr: scion header offset is %d", hdr_len);
  size_t scion_hdr_len = add_scion_ext_header(xia_ext_hdr, packet_data, data_len);

  xiah->plen = htons(original_payload_len + (uint16_t)scion_hdr_len);

  syslog(LOG_INFO, "add scion hdr: final payload len is %d, scion hdr len is %d", (int)ntohs(xiah->plen), (int)scion_hdr_len);

  return; 
}


//
//add scion header as xia extension header
//
void add_scion_info_before_xtransport_hdr(click_xia *xiah, int xia_hdr_len)
{
  uint8_t packet_data[RV_MAX_DATA_PACKET_SIZE];
  //int total_nodes = xiah->dnode + xiah->snode;
  uint16_t original_payload_len = ntohs(xiah->plen);
  uint8_t nxt_hdr_type = xiah->nxt;
  //uint16_t xia_hdr_len =  sizeof(struct click_xia) + total_nodes*sizeof(struct click_xia_xid_node);
  uint16_t hdr_len = xia_hdr_len;
  uint8_t* packet = reinterpret_cast<uint8_t *>(xiah);
  struct click_xia_ext* xia_ext_hdr = reinterpret_cast<struct click_xia_ext *>(packet + hdr_len);  

  memcpy(packet_data, packet + hdr_len, original_payload_len);
  memset(packet + hdr_len, 0, original_payload_len); 
  
  xiah->nxt = CLICK_XIA_NXT_SCION;
  
  xia_ext_hdr = reinterpret_cast<struct click_xia_ext *>(packet + hdr_len);
  syslog(LOG_INFO, "add scion hdr: scion header offset is %d, original payload len is %d", 
             hdr_len, original_payload_len);
  size_t scion_hdr_len = add_scion_ext_header(xia_ext_hdr, packet_data, original_payload_len);

  xia_ext_hdr->nxt = nxt_hdr_type;

  xiah->plen = htons(original_payload_len + scion_hdr_len);

  syslog(LOG_INFO, "add scion hdr: final payload len is %d, scion hdr len is %d", (int)ntohs(xiah->plen), (int)scion_hdr_len);

  return; 
}


//
//add scion header as xia extension header
//
void add_scion_info_in_scion_sid(click_xia *xiah, int xia_hdr_len)
{
  uint8_t* scion_sid = xiah->node[0].xid.id;
  scion_sid[0] = 0; // 0: router 0; 1: router 1
  return; 
}

#endif

void process_data(int datasock)
{
	int packetlen;
	char packet[RV_MAX_DATA_PACKET_SIZE];
	sockaddr_x rdag;
	socklen_t rdaglen = sizeof(rdag);
	bzero(packet, RV_MAX_DATA_PACKET_SIZE);

	syslog(LOG_INFO, "gateway: Reading data packet");
	int retval = Xrecvfrom(datasock, packet, RV_MAX_DATA_PACKET_SIZE, 0, (struct sockaddr *)&rdag, &rdaglen);
	if(retval < 0) {
		syslog(LOG_WARNING, "WARN: No data(%s)", strerror(errno));
		return;
	}
	packetlen = retval;
	Graph g(&rdag);
	syslog(LOG_INFO, "gateway: Packet of size:%d received from %s:", retval, g.dag_string().c_str());
	click_xia *xiah = reinterpret_cast<struct click_xia *>(packet);
	int total_nodes = print_packet_header(xiah);
        int xia_header_size = sizeof(struct click_xia) + total_nodes * sizeof(struct click_xia_xid_node);
        syslog(LOG_INFO, "\ngateway: XIA header contents, size %d\n", xia_header_size);
        print_packet_contents(packet, xia_header_size);

        syslog(LOG_INFO, "\ngateway: contents after XIA header, size %d\n", retval - xia_header_size);
        print_packet_contents(packet + xia_header_size, retval - xia_header_size);
         
        //add scion infor as extension header here
        //add_scion_info(xiah, xia_header_size);  //add scion hdr after all headers
	add_scion_info_before_xtransport_hdr(xiah, xia_header_size);
#if 1
        //add_scion_info_in_scion_sid(xiah, xia_header_size); // for debugging
	xiah->node[0].xid.type = htonl(CLICK_XIA_XID_TYPE_SCIONID);
	syslog(LOG_INFO, "gateway: change type to SCION");
#endif
          
	xiah->last = -1;
	syslog(LOG_INFO, "gateway: Updated AD and last pointer in header");
	print_packet_header(xiah);
	print_packet_contents(packet, retval + 127);
	syslog(LOG_INFO, "gateway: Sending the packet out on the network");
	Xsend(datasock, packet, packetlen, 0);
}

#define MAX_XID_STR_SIZE 64
#define MAX_HID_DAG_STR_SIZE 256

void process_control_message(int controlsock)
{
	char packet[RV_MAX_CONTROL_PACKET_SIZE];
	sockaddr_x ddag;
	socklen_t ddaglen = sizeof(ddag);
	bzero(packet, RV_MAX_CONTROL_PACKET_SIZE);

	syslog(LOG_INFO, "gateway: Reading control packet");
	int retval = Xrecvfrom(controlsock, packet, RV_MAX_CONTROL_PACKET_SIZE, 0, (struct sockaddr *)&ddag, &ddaglen);
	if(retval < 0) {
		syslog(LOG_WARNING, "WARN: No control message(%s)", strerror(errno));
		return;
	}
	syslog(LOG_INFO, "Control packet of size:%d received", retval);

	// Extract control message, signature and pubkey of sender
	XIASecurityBuffer signedMsg(packet, retval);
	uint16_t controlMsgLength = signedMsg.peekUnpackLength();
	char controlMsgBuffer[controlMsgLength];
	signedMsg.unpack(controlMsgBuffer, &controlMsgLength);

	// The control message
	XIASecurityBuffer controlMsg(controlMsgBuffer, controlMsgLength);

	// The signature
	uint16_t signatureLength = signedMsg.peekUnpackLength();
	char signature[signatureLength];
	signedMsg.unpack(signature, &signatureLength);

	// The sender's public key
	uint16_t pubkeyLength = signedMsg.peekUnpackLength();
	char pubkey[pubkeyLength+1];
	bzero(pubkey, pubkeyLength+1);
	signedMsg.unpack(pubkey, &pubkeyLength);

	// Extract HID, DAG & timestamp from the control message
	uint16_t hidLength = controlMsg.peekUnpackLength();
	char hid[hidLength+1];
	bzero(hid, hidLength+1);
	controlMsg.unpack(hid, &hidLength);

	uint16_t dagLength = controlMsg.peekUnpackLength();
	char dag[dagLength+1];
	bzero(dag, dagLength+1);
	controlMsg.unpack(dag, &dagLength);

	uint16_t timestampLength = controlMsg.peekUnpackLength();
	assert(timestampLength == (uint16_t) sizeof(double));
	double timestamp;
	controlMsg.unpack((char *)&timestamp, &timestampLength);

	// Verify hash(pubkey) matches HID
	if(!xs_pubkeyMatchesXID(pubkey, hid)) {
		syslog(LOG_ERR, "ERROR: Public key does not match HID of control message sender");
		return;
	}

	// Verify signature using pubkey
	if(!xs_isValidSignature((const unsigned char *)controlMsgBuffer, controlMsgLength, (unsigned char *)signature, signatureLength, pubkey, pubkeyLength)) {
		syslog(LOG_ERR, "ERROR: Invalid signature");
		return;
	}

	// Verify that the timestamp is newer than seen before
	HIDtoTimestampIterator = HIDtoTimestamp.find(hid);
	if(HIDtoTimestampIterator == HIDtoTimestamp.end()) {
		// New host, create an entry and record initial timestamp
		HIDtoTimestamp[hid] = timestamp;
		syslog(LOG_INFO, "New timestamp %f for HID|%s|", timestamp, hid);
	} else {
		// Verify the last timestamp is older than the one is this message
		if(HIDtoTimestamp[hid] < timestamp) {
			HIDtoTimestamp[hid] = timestamp;
			syslog(LOG_INFO, "Updated timestamp %f for HID|%s|", timestamp, hid);
		} else {
			syslog(LOG_ERR, "ERROR: Timestamp previous:%f now:%f", HIDtoTimestamp[hid], timestamp);
			return;
		}
	}

	// Extract AD from DAG
	int ADstringLength = strlen("AD:") + 40 + 1;
	char ADstring[ADstringLength];
	bzero(ADstring, ADstringLength);
	strncpy(ADstring, strstr(dag, "AD:"), ADstringLength-1);
	HIDtoAD[hid] = ADstring;
	syslog(LOG_INFO, "gateway: Added %s:%s to table", hid, ADstring);

	// Registration message
	// Extract HID, newAD, timestamp, Signature, Pubkey
	// Verify HID not already in table
	// Verify HID matches Pubkey
	// Verify Signature(HID, newAD, timestamp) using Pubkey
	// Set lastTimestamp for this HID
	//
	//
	// UpdateAD message
	// Extract HID, newAD, timestamp, Signature, Pubkey
	// Verify HID is in table
	// Verify HID matches Pubkey
	// Verify Signature(HID, newAD, timestamp) using Pubkey
	// Verify timestamp > lastTimestamp
	//
	// Heartbeat message?
}

//void print_scion_header(SCIONHeader *scionh)
//{
//	syslog(LOG_INFO, "======= SCION HEADER ========");
//}


int main(int argc, char *argv[]) {
	// Parse command-line arguments
	config(argc, argv);
	syslog(LOG_NOTICE, "%s started on %s", APPNAME, hostname);

	// Data plane socket used to rendezvous clients with services
	int datasock = getServerSocket(datasid, SOCK_RAW);
	int controlsock = getServerSocket(controlsid, SOCK_DGRAM);
	if(datasock < 0 || controlsock < 0) {
		syslog(LOG_ERR, "ERROR creating a server socket");
		return 1;
	}

	// Main loop checks data and control sockets for activity
	while(1) {
		fd_set read_fds;
		FD_ZERO(&read_fds);
		FD_SET(datasock, &read_fds);
		FD_SET(controlsock, &read_fds);

		struct timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 1000;

		int retval = Xselect(controlsock+1, &read_fds, NULL, NULL, &timeout);
		if(retval == -1) {
			syslog(LOG_ERR, "ERROR waiting for data to arrive. Exiting");
			return 2;
		}

                //syslog(LOG_DEBUG, "Gateway loop");

		if(retval == 0) {
			// No data on control/data sockets, loop again
			// This is the place to add any actions between loop iterations
			continue;
		}

		// Check for control messages first
		if(FD_ISSET(controlsock, &read_fds)) {
			process_control_message(controlsock);
		}

		// Handle data packets
		if(FD_ISSET(datasock, &read_fds)) {
			process_data(datasock);
		}
	}

	return 0;

}
