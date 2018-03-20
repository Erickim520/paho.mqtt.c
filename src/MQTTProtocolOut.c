/*******************************************************************************
 * Copyright (c) 2009, 2018 IBM Corp.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v1.0 which accompany this distribution. 
 *
 * The Eclipse Public License is available at 
 *    http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at 
 *   http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Ian Craggs - initial API and implementation and/or initial documentation
 *    Ian Craggs, Allan Stockdill-Mander - SSL updates
 *    Ian Craggs - fix for buffer overflow in addressPort bug #433290
 *    Ian Craggs - MQTT 3.1.1 support
 *    Rong Xiang, Ian Craggs - C++ compatibility
 *    Ian Craggs - fix for bug 479376
 *    Ian Craggs - SNI support
 *    Ian Craggs - fix for issue #164
 *    Ian Craggs - fix for issue #179
 *    Ian Craggs - MQTT 5.0 support
 *******************************************************************************/

/**
 * @file
 * \brief Functions dealing with the MQTT protocol exchanges
 *
 * Some other related functions are in the MQTTProtocolClient module
 */

#include <stdlib.h>
#include <string.h>

#include "MQTTProtocolOut.h"
#include "StackTrace.h"
#include "Heap.h"

extern ClientStates* bstate;



/**
 * Separates an address:port into two separate values
 * @param uri the input string - hostname:port
 * @param port the returned port integer
 * @return the address string
 */
char* MQTTProtocol_addressPort(const char* uri, int* port)
{
	char* colon_pos = strrchr(uri, ':'); /* reverse find to allow for ':' in IPv6 addresses */
	char* buf = (char*)uri;
	size_t len;

	FUNC_ENTRY;
	if (uri[0] == '[')
	{  /* ip v6 */
		if (colon_pos < strrchr(uri, ']'))
			colon_pos = NULL;  /* means it was an IPv6 separator, not for host:port */
	}

	if (colon_pos) /* have to strip off the port */
	{
		size_t addr_len = colon_pos - uri;
		buf = malloc(addr_len + 1);
		*port = atoi(colon_pos + 1);
		MQTTStrncpy(buf, uri, addr_len+1);
	}
	else
		*port = DEFAULT_PORT;

	len = strlen(buf);
	if (buf[len - 1] == ']')
	{
		if (buf == (char*)uri)
		{
			buf = malloc(len);  /* we are stripping off the final ], so length is 1 shorter */
			MQTTStrncpy(buf, uri, len);
		}
		else
			buf[len - 1] = '\0';
	}
	FUNC_EXIT;
	return buf;
}


/**
 * MQTT outgoing connect processing for a client
 * @param ip_address the TCP address:port to connect to
 * @param aClient a structure with all MQTT data needed
 * @param int ssl
 * @param int MQTTVersion the MQTT version to connect with (3 or 4)
 * @return return code
 */
#if defined(OPENSSL)
int MQTTProtocol_connect(const char* ip_address, Clients* aClient, int ssl, int MQTTVersion,
		MQTTProperties* connectProperties, MQTTProperties* willProperties)
#else
int MQTTProtocol_connect(const char* ip_address, Clients* aClient, int MQTTVersion,
		MQTTProperties* connectProperties, MQTTProperties* willProperties)
#endif
{
	int rc, port;
	char* addr;

	FUNC_ENTRY;
	aClient->good = 1;

	addr = MQTTProtocol_addressPort(ip_address, &port);
	rc = Socket_new(addr, port, &(aClient->net.socket));
	if (rc == EINPROGRESS || rc == EWOULDBLOCK)
		aClient->connect_state = 1; /* TCP connect called - wait for connect completion */
	else if (rc == 0)
	{	/* TCP connect completed. If SSL, send SSL connect */
#if defined(OPENSSL)
		if (ssl)
		{
			if (SSLSocket_setSocketForSSL(&aClient->net, aClient->sslopts, addr) == 1)
			{
				rc = SSLSocket_connect(aClient->net.ssl, aClient->net.socket,
						addr, aClient->sslopts->verify);
				if (rc == TCPSOCKET_INTERRUPTED)
					aClient->connect_state = 2; /* SSL connect called - wait for completion */
			}
			else
				rc = SOCKET_ERROR;
		}
#endif
		
		if (rc == 0)
		{
			/* Now send the MQTT connect packet */
			if ((rc = MQTTPacket_send_connect(aClient, MQTTVersion, connectProperties, willProperties)) == 0)
				aClient->connect_state = 3; /* MQTT Connect sent - wait for CONNACK */ 
			else
				aClient->connect_state = 0;
		}
	}
	if (addr != ip_address)
		free(addr);

	FUNC_EXIT_RC(rc);
	return rc;
}


/**
 * Process an incoming pingresp packet for a socket
 * @param pack pointer to the publish packet
 * @param sock the socket on which the packet was received
 * @return completion code
 */
int MQTTProtocol_handlePingresps(void* pack, int sock)
{
	Clients* client = NULL;
	int rc = TCPSOCKET_COMPLETE;

	FUNC_ENTRY;
	client = (Clients*)(ListFindItem(bstate->clients, &sock, clientSocketCompare)->content);
	Log(LOG_PROTOCOL, 21, NULL, sock, client->clientID);
	client->ping_outstanding = 0;
	FUNC_EXIT_RC(rc);
	return rc;
}


/**
 * MQTT outgoing subscribe processing for a client
 * @param client the client structure
 * @param topics list of topics
 * @param qoss corresponding list of QoSs
 * @param opts MQTT 5.0 subscribe options
 * @param props MQTT 5.0 subscribe properties
 * @return completion code
 */
int MQTTProtocol_subscribe(Clients* client, List* topics, List* qoss, int msgID,
		MQTTSubscribe_options* opts, MQTTProperties* props)
{
	int rc = 0;

	FUNC_ENTRY;
	/* we should stack this up for retry processing too */
	rc = MQTTPacket_send_subscribe(topics, qoss, opts, props, msgID, 0, client);
	FUNC_EXIT_RC(rc);
	return rc;
}


/**
 * Process an incoming suback packet for a socket
 * @param pack pointer to the publish packet
 * @param sock the socket on which the packet was received
 * @return completion code
 */
int MQTTProtocol_handleSubacks(void* pack, int sock)
{
	Suback* suback = (Suback*)pack;
	Clients* client = NULL;
	int rc = TCPSOCKET_COMPLETE;

	FUNC_ENTRY;
	client = (Clients*)(ListFindItem(bstate->clients, &sock, clientSocketCompare)->content);
	Log(LOG_PROTOCOL, 23, NULL, sock, client->clientID, suback->msgId);
	MQTTPacket_freeSuback(suback);
	FUNC_EXIT_RC(rc);
	return rc;
}


/**
 * MQTT outgoing unsubscribe processing for a client
 * @param client the client structure
 * @param topics list of topics
 * @return completion code
 */
int MQTTProtocol_unsubscribe(Clients* client, List* topics, int msgID)
{
	int rc = 0;

	FUNC_ENTRY;
	/* we should stack this up for retry processing too? */
	rc = MQTTPacket_send_unsubscribe(topics, msgID, 0, &client->net, client->clientID);
	FUNC_EXIT_RC(rc);
	return rc;
}


/**
 * Process an incoming unsuback packet for a socket
 * @param pack pointer to the publish packet
 * @param sock the socket on which the packet was received
 * @return completion code
 */
int MQTTProtocol_handleUnsubacks(void* pack, int sock)
{
	Unsuback* unsuback = (Unsuback*)pack;
	Clients* client = NULL;
	int rc = TCPSOCKET_COMPLETE;

	FUNC_ENTRY;
	client = (Clients*)(ListFindItem(bstate->clients, &sock, clientSocketCompare)->content);
	Log(LOG_PROTOCOL, 24, NULL, sock, client->clientID, unsuback->msgId);
	free(unsuback);
	FUNC_EXIT_RC(rc);
	return rc;
}

