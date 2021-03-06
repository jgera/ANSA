// Copyright (C) 2013 Brno University of Technology (http://nes.fit.vutbr.cz/ansa)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
/**
 * @file AnsaIPv4.h
 * @date 21.10.2011
 * @author Veronika Rybova, Vladimir Vesely (mailto:ivesely@fit.vutbr.cz)
 * @brief IPv4 implementation with changes for multicast
 * @details File contains extension of class IP, which can work also with multicast data and multicast
 */

#ifndef __INET_ANSAIPV4_H
#define __INET_ANSAIPV4_H

#include "INETDefs.h"

#ifdef WITH_MANET
#include "ControlManetRouting_m.h"
#endif

#include "ARPPacket_m.h"
#include "IPv4.h"
#include "PimSplitter.h"
#include "AnsaInterfaceEntry.h"
#include "AnsaRoutingTable.h"
#include "AnsaRoutingTableAccess.h"
#include "PimInterfaceTable.h"
#include "PIMPacket_m.h"
#include "pimSM.h"

/*
 * Migration towards ANSAINET2.2
enum AnsaIPProtocolId
{
    IP_PROT_PIM = 103
};
*/
/**
 * @brief Class is extension of the IP protocol implementation for multicast.
 * @details It extends class IP mainly by methods processing multicast stream.
 */
class INET_API AnsaIPv4 : public IPv4
{
    private:
        AnsaRoutingTable            *rt;
        PimInterfaceTable           *pimIft;        /**< Pointer to table of PIM interfaces. */

    protected:
        virtual void handlePacketFromNetwork(IPv4Datagram *datagram, InterfaceEntry *fromIE);
        virtual void routeMulticastPacket(IPv4Datagram *datagram, InterfaceEntry *destIE, InterfaceEntry *fromIE);
        virtual void routePimSM (AnsaIPv4MulticastRoute *route, AnsaIPv4MulticastRoute *routeG, IPv4Datagram *datagram, IPv4ControlInfo *ctrl);
        virtual void routePimDM (AnsaIPv4MulticastRoute *route, IPv4Datagram *datagram, IPv4ControlInfo *ctrl);
        virtual IPv4Datagram *encapsulate(cPacket *transportPacket, IPv4ControlInfo *controlInfo);
        virtual void routeUnicastPacket(IPv4Datagram *datagram, InterfaceEntry *destIE, IPv4Address destNextHopAddr);
        virtual void handleMessageFromHL(cPacket *msg);
        virtual void reassembleAndDeliver(IPv4Datagram *datagram);
        virtual InterfaceEntry *determineOutgoingInterfaceForMulticastDatagram(IPv4Datagram *datagram, InterfaceEntry *multicastIFOption);

        virtual void fragmentAndSend(IPv4Datagram *datagram, InterfaceEntry *ie, IPv4Address nextHopAddr, int vforwarder);
        virtual void sendDatagramToOutput(IPv4Datagram *datagram, InterfaceEntry *ie, IPv4Address nextHopAddr, int vforwarderId);
        int getVirtualForwarderId(InterfaceEntry *ie, MACAddress addr);
    public:
        AnsaIPv4() {}

    protected:
      virtual int numInitStages() const  {return 5;}
      virtual void initialize(int stage);
};


#endif
