/*
 * clientserver.h
 *
 *  Created on: 17.12.2013
 *      Author: dplasa
 */

#ifndef CLIENTSERVER_H_
#define CLIENTSERVER_H_

#include <netinet/in.h>

// that is our 4-byte signature value, "clo3"
#define CLOOP3_NET_SIGNATURE (('c'<<24)|('l'<<16)|('o'<<8)|'3')

// currently we support protocol_version 3...3
#define PROTOCOL_VERSION_MIN 3
#define PROTOCOL_VERSION_CURRENT 3
#define PROTOCOL_VERSION_MAX PROTOCOL_VERSION_CURRENT

// Data structure that is used for discovery of remote nodes
struct __attribute__ ((__packed__)) client2server_v3_discovery
{
	// all entries in network byte order before sending it over the net!
	uint32_t signature;
	uint32_t protocolVersion;
	uint32_t suggestedConnections;
	client2server_v3_discovery()
	{
		signature = htonl(CLOOP3_NET_SIGNATURE);
		protocolVersion = htonl(PROTOCOL_VERSION_MIN);
		suggestedConnections = -1;
	}
};

struct  __attribute__ ((__packed__)) NetRequestV3
{
	// all entries in network byte order before sending it over the net!
	uint32_t signature;			// CLOOP3_NET_SIGNATURE
	uint32_t protocolVersion;
	uint32_t workMode;			// mode_COMPRESS or mode_UNCOMPRESS
	uint32_t dataSize;			// size of following data block
	uint32_t strategy;	 		// mode_COMPRESS: cs_NONE, cs_BESTSIZE, cs_BESTTIME or cs_SMART
								// mode_UNCOMPRESS: don't care
	uint32_t compressorMask;	// mode_COMPRESS: bit-OR as defined in cloop.h
								// mode_UNCOMPRESS: don't care
	int32_t level;				// mode_COMPRESS: suggested compression level
								// mode_UNCOMPRESS: don't care
	NetRequestV3()
	{
		signature = htonl(CLOOP3_NET_SIGNATURE);
		protocolVersion = htonl(PROTOCOL_VERSION_MIN);
		workMode = 0; // mode_UNSPECIFIED
		dataSize = strategy = compressorMask = 0;
		level = -1;	// dont't care, use defaults
	}

	void ntoh()
	{
		protocolVersion = ntohl(protocolVersion);
		workMode = ntohl(workMode);
		dataSize = ntohl(dataSize);
		strategy = ntohl(strategy);
		compressorMask = ntohl(compressorMask );
		level = ntohl(level);
	}
};

struct  __attribute__ ((__packed__)) NetReplyV3
{
	// all entries in network byte order before sending it over the net!
	uint32_t protocolVersion;	// should be the same as in the request
	uint32_t result;			// C_OK, C_INCOMPRESSIBLE or C_ERROR
	uint32_t dataSize;			// size of following data block
	uint32_t compressor;		// mode_COMPRESS: used compressor as defined in cloop.h
								// mode_UNCOMPRESS: don't care
	NetReplyV3()
	{
		protocolVersion = htonl(PROTOCOL_VERSION_MAX);
		result = dataSize = compressor = 0;
	}
};

struct node_address
{
	int af;	// what address type AF_INET or AF_INET6
	union {
		struct in6_addr ip6;
		struct in_addr ip4;
	} address;
	int prefixLen;
};

#endif /* CLIENTSERVER_H_ */
