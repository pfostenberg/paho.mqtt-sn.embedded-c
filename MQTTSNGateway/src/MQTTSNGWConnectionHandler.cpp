/**************************************************************************************
 * Copyright (c) 2016, 2020 Tomoaki Yamaguchi and others
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
 *    Tomoaki Yamaguchi - initial API and implementation and/or initial documentation
 **************************************************************************************/

#include "MQTTSNGWConnectionHandler.h"
#include "MQTTSNGateway.h"
#include "MQTTSNGWPacket.h"
#include "MQTTGWPacket.h"
#include <string.h>

using namespace std;
using namespace MQTTSNGW;

/*=====================================
 Class MQTTSNConnectionHandler
 =====================================*/
MQTTSNConnectionHandler::MQTTSNConnectionHandler(Gateway* gateway)
{
    _gateway = gateway;
}

MQTTSNConnectionHandler::~MQTTSNConnectionHandler()
{

}

/*
 *  ADVERTISE
 */
void MQTTSNConnectionHandler::sendADVERTISE()
{
    MQTTSNPacket* adv = new MQTTSNPacket();
    adv->setADVERTISE(_gateway->getGWParams()->gatewayId, _gateway->getGWParams()->keepAlive);
    Event* ev1 = new Event();
    ev1->setBrodcastEvent(adv);  //broadcast
    _gateway->getClientSendQue()->post(ev1);
}

/*
 *  SEARCHGW
 */
void MQTTSNConnectionHandler::handleSearchgw(MQTTSNPacket* packet)
{
    if (packet->getType() == MQTTSN_SEARCHGW)
    {
        MQTTSNPacket* gwinfo = new MQTTSNPacket();
        gwinfo->setGWINFO(_gateway->getGWParams()->gatewayId);
        Event* ev1 = new Event();
        ev1->setBrodcastEvent(gwinfo);
        _gateway->getClientSendQue()->post(ev1);
    }
}

/*
 *  CONNECT
 */
void MQTTSNConnectionHandler::handleConnect(Client* client, MQTTSNPacket* packet)
{
    MQTTSNPacket_connectData data;
    if (packet->getCONNECT(&data) == 0)
    {
        return;
    }

    /* return CONNACK when the client is sleeping */
    if (client->isSleep() || client->isAwake())
    {
        MQTTSNPacket* packet = new MQTTSNPacket();
        packet->setCONNACK(MQTTSN_RC_ACCEPTED);
        Event* ev = new Event();
        ev->setClientSendEvent(client, packet);
        _gateway->getClientSendQue()->post(ev);

        sendStoredPublish(client);
        return;
    }

    //* clear ConnectData of Client */
    Connect* connectData = client->getConnectData();
    memset(connectData, 0, sizeof(Connect));
    if (!client->isAdapter())
    {
        client->disconnected();
    }

    Topics* topics = client->getTopics();

    /* CONNECT was not sent yet. prepare Connect data */
    connectData->header.bits.type = CONNECT;
    connectData->clientID = client->getClientId();
    connectData->version = _gateway->getGWParams()->mqttVersion;
    connectData->keepAliveTimer = data.duration;
    DEBUGLOG("XXX Duration1  = %d\n", data.duration);
    connectData->flags.bits.will = data.willFlag;
	
#ifdef CLIENTID2UNPW
    connectData->flags.bits.username = 1;
	connectData->flags.bits.password = 1;
#else	

    if ((const char*) _gateway->getGWParams()->loginId != nullptr)
    {
        connectData->flags.bits.username = 1;
    }

    if ((const char*) _gateway->getGWParams()->password != 0)
    {
        connectData->flags.bits.password = 1;
    }
#endif

    client->setSessionStatus(false);
    if (data.cleansession)
    {
        connectData->flags.bits.cleanstart = 1;
        /* reset the table of msgNo and TopicId pare */
        client->clearWaitedPubTopicId();
        client->clearWaitedSubTopicId();

        /* renew the TopicList */
        if (topics)
        {
            topics->eraseNormal();
            ;
        }
        client->setSessionStatus(true);
    }

    if (data.willFlag)
    {
        /* create & send WILLTOPICREQ message to the client */
        MQTTSNPacket* reqTopic = new MQTTSNPacket();
        reqTopic->setWILLTOPICREQ();
        Event* evwr = new Event();
        evwr->setClientSendEvent(client, reqTopic);

        /* Send WILLTOPICREQ to the client */
        _gateway->getClientSendQue()->post(evwr);
    }
    else
    {
        /* CONNECT message was not qued in.
         * create CONNECT message & send it to the broker */
        MQTTGWPacket* mqMsg = new MQTTGWPacket();
	unsigned char* login = (unsigned char*)( _gateway->getGWParams()->loginId);
	unsigned char* passw = (unsigned char*)( _gateway->getGWParams()->password);

        /* 
         use 
         ./build.sh udp -DCLIENTID2UNPW 
         to make this work. Alias CLIENT To UName PassWord
        */
	#ifdef CLIENTID2UNPW
        /*
         MQTT-SN lacks of user / password by default.
         in some use cases with IMEI it can be good if you can restrict the gateway to only the topic the "clientID" has intests in
         Create a user with IMEI(15 char) and a password (8char)
         give the user rights for his TOPIC/IMEI/whatever
         below we separate the clientID into user + password for the MQTT call ( makes only sense if transparent gateway!)
         with mqtt-sn-pub -i imei+pw you can now write to topic where the user has rights
         see more in https://support.ram.m2m.telekom.com/apps/docs/device-sdk/mqtt-de/index.html
        */

        char cidLogin[32];
        char cidPassw[32];  // the pointer must be valid during the connect call!
        int imeiLen = 15;
        int pwLen=8;

        int cidLength = strlen(connectData->clientID);
        if (cidLength == (imeiLen+pwLen))
        {
                for (int i=0; i<imeiLen; i++)
		{
			cidLogin[i]=connectData->clientID[i];
                }
                cidLogin[imeiLen]=0; // terminate
	        for (int i=0; i < pwLen; i++)
		{
                        cidPassw[i]=connectData->clientID[i+imeiLen];
                }
                cidPassw[pwLen]=0;  // terminate

		login = (unsigned char*)cidLogin;
		passw = (unsigned char*)cidPassw;

              	WRITELOG("MQTTSNConnectionHandler::handleConnect clientID 2 username password %s %s\n", login, passw);
        }
        #endif	
        mqMsg->setCONNECT(client->getConnectData(), login, passw);
        Event* ev1 = new Event();
        ev1->setBrokerSendEvent(client, mqMsg);
        _gateway->getBrokerSendQue()->post(ev1);
    }
}

/*
 *  WILLTOPIC
 */
void MQTTSNConnectionHandler::handleWilltopic(Client* client, MQTTSNPacket* packet)
{
    int willQos;
    uint8_t willRetain;
    MQTTSNString willTopic = MQTTSNString_initializer;

    if (packet->getWILLTOPIC(&willQos, &willRetain, &willTopic) == 0)
    {
        return;
    }
    client->setWillTopic(willTopic);
    Connect* connectData = client->getConnectData();

    /* add the connectData for MQTT CONNECT message */
    connectData->willTopic = client->getWillTopic();
    connectData->flags.bits.willQoS = willQos;
    connectData->flags.bits.willRetain = willRetain;

    /* Send WILLMSGREQ to the client */
    client->setWaitWillMsgFlg(true);
    MQTTSNPacket* reqMsg = new MQTTSNPacket();
    reqMsg->setWILLMSGREQ();
    Event* evt = new Event();
    evt->setClientSendEvent(client, reqMsg);
    _gateway->getClientSendQue()->post(evt);
}

/*
 *  WILLMSG
 */
void MQTTSNConnectionHandler::handleWillmsg(Client* client, MQTTSNPacket* packet)
{
    if (!client->isWaitWillMsg())
    {
        DEBUGLOG("     MQTTSNConnectionHandler::handleWillmsg  WaitWillMsgFlg is off.\n");
        return;
    }

    MQTTSNString willmsg = MQTTSNString_initializer;
    Connect* connectData = client->getConnectData();

    if (client->isConnectSendable())
    {
        /* save WillMsg in the client */
        if (packet->getWILLMSG(&willmsg) == 0)
        {
            return;
        }
        client->setWillMsg(willmsg);

        /* create CONNECT message */
        MQTTGWPacket* mqttPacket = new MQTTGWPacket();
        connectData->willMsg = client->getWillMsg();
        mqttPacket->setCONNECT(connectData, (unsigned char*) _gateway->getGWParams()->loginId,
                (unsigned char*) _gateway->getGWParams()->password);

        /* Send CONNECT to the broker */
        Event* evt = new Event();
        evt->setBrokerSendEvent(client, mqttPacket);
        client->setWaitWillMsgFlg(false);
        _gateway->getBrokerSendQue()->post(evt);
    }
}

/*
 *  DISCONNECT
 */
void MQTTSNConnectionHandler::handleDisconnect(Client* client, MQTTSNPacket* packet)
{
    uint16_t duration = 0;

    if (packet->getDISCONNECT(&duration) != 0)
    {
        if (duration == 0)
        {
            MQTTGWPacket* mqMsg = new MQTTGWPacket();
            mqMsg->setHeader(DISCONNECT);
            Event* ev = new Event();
            ev->setBrokerSendEvent(client, mqMsg);
            _gateway->getBrokerSendQue()->post(ev);
        }
    }

    MQTTSNPacket* snMsg = new MQTTSNPacket();
    snMsg->setDISCONNECT(0);
    Event* evt = new Event();
    evt->setClientSendEvent(client, snMsg);
    _gateway->getClientSendQue()->post(evt);
}

/*
 *  WILLTOPICUPD
 */
void MQTTSNConnectionHandler::handleWilltopicupd(Client* client, MQTTSNPacket* packet)
{
    /* send NOT_SUPPORTED responce to the client */
    MQTTSNPacket* respMsg = new MQTTSNPacket();
    respMsg->setWILLTOPICRESP(MQTTSN_RC_NOT_SUPPORTED);
    Event* evt = new Event();
    evt->setClientSendEvent(client, respMsg);
    _gateway->getClientSendQue()->post(evt);
}

/*
 *  WILLMSGUPD
 */
void MQTTSNConnectionHandler::handleWillmsgupd(Client* client, MQTTSNPacket* packet)
{
    /* send NOT_SUPPORTED responce to the client */
    MQTTSNPacket* respMsg = new MQTTSNPacket();
    respMsg->setWILLMSGRESP(MQTTSN_RC_NOT_SUPPORTED);
    Event* evt = new Event();
    evt->setClientSendEvent(client, respMsg);
    _gateway->getClientSendQue()->post(evt);
}

/*
 *  PINGREQ
 */
void MQTTSNConnectionHandler::handlePingreq(Client* client, MQTTSNPacket* packet)
{
    if ((client->isSleep() || client->isAwake()) && client->getClientSleepPacket())
    {
        sendStoredPublish(client);
        client->holdPingRequest();
    }
    else
    {
        /* send PINGREQ to the broker */
        client->resetPingRequest();
        MQTTGWPacket* pingreq = new MQTTGWPacket();
        pingreq->setHeader(PINGREQ);
        Event* evt = new Event();
        evt->setBrokerSendEvent(client, pingreq);
        _gateway->getBrokerSendQue()->post(evt);
    }
}

void MQTTSNConnectionHandler::sendStoredPublish(Client* client)
{
    MQTTGWPacket* msg = nullptr;

    while ((msg = client->getClientSleepPacket()) != nullptr)
    {
        // ToDo:  This version can't re-send PUBLISH when PUBACK is not returned.
        client->deleteFirstClientSleepPacket(); // pop the que to delete element.

        Event* ev = new Event();
        ev->setBrokerRecvEvent(client, msg);
        _gateway->getPacketEventQue()->post(ev);
    }
}
