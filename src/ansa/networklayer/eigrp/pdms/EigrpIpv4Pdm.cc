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
// 

#include <algorithm>

#include "IPv4ControlInfo.h"
#include "deviceConfigurator.h"
#include "AnsaIPv4Route.h"

#include "EigrpIpv4Pdm.h"

/**
 * Shortcuts:
 * TT ... Topology Table
 * RT ... Routing Table
 * NT ... Neighbor Table
 */

#define EIGRP_DEBUG

Define_Module(EigrpIpv4Pdm);

namespace eigrp
{

// User message codes
enum UserMsgCodes
{
  M_OK = 0,                             // no message
  M_UPDATE_SEND = EIGRP_UPDATE_MSG,     // send Update message
  M_REQUEST_SEND = EIGRP_REQUEST_MSG,   // send Request message
  M_QUERY_SEND = EIGRP_QUERY_MSG,       // send Query message
  M_REPLY_SEND = EIGRP_REPLY_MSG,       // send Query message
  M_HELLO_SEND = EIGRP_HELLO_MSG,       // send Hello message
  M_DISABLED_ON_IF,                     // EIGRP is disabled on interface
  M_NEIGH_BAD_AS,                       // neighbor has bad AS number
  M_NEIGH_BAD_KVALUES,                  // neighbor has bad K-values
  M_NEIGH_BAD_SUBNET,                   // neighbor isn't on common subnet
  M_SIAQUERY_SEND = EIGRP_SIAQUERY_MSG, // send SIA Query message
  M_SIAREPLY_SEND = EIGRP_SIAREPLY_MSG, // send SIA Reply message
};

// User messages
const char *UserMsgs[] =
{
  // M_OK
  "OK",
  // M_UPDATE_SEND
  "Update",
  // M_REQUEST_SEND
  "Request",
  // M_QUERY_SEND
  "Query",
  // M_REPLY_SEND
  "Reply",
  // M_HELLO_SEND
  "Hello",
  // M_DISABLED_ON_IF
  "EIGRP process isn't enabled on interface",
  // M_NEIGH_BAD_AS
  "AS number is different",
  // M_NEIGH_BAD_KVALUES
  "K-value mismatch",
  // M_NEIGH_BAD_SUBNET
  "Not on the common subnet",
  // M_SIAQUERY_SEND
  "Send SIA Query message",
  // M_SIAREPLY_SEND
  "Send SIA Reply message",
};

}; // end of namespace eigrp

/**
 * Output operator for IPv4Network class.
 */
std::ostream& operator<<(std::ostream& os, const EigrpNetwork& network)
{
    os << "Address:" << network.getAddress() << " Mask:" << network.getMask();
    return os;
}

std::ostream& operator<<(std::ostream& os, const EigrpKValues& kval)
{
    os << "K1:" << kval.K1 << " K2:" << kval.K2 << " K3:" << kval.K3;
    os << "K4:" << kval.K4 << " K5:" << kval.K5 << " K6:" << kval.K6;
    return os;
}

bool operator==(const EigrpKValues& k1, const EigrpKValues& k2)
{
    return k1.K1 == k2.K1 && k1.K2 == k2.K2 &&
            k1.K3 == k2.K3 && k1.K4 == k2.K4 &&
            k1.K5 == k2.K5 && k1.K6 == k2.K6;
}


EigrpIpv4Pdm::EigrpIpv4Pdm() : EIGRP_IPV4_MULT(IPv4Address(224, 0, 0, 10))
{
    SPLITTER_OUTGW = "splitterOut";
    RTP_OUTGW = "rtpOut";

    KVALUES_MAX.K1 = KVALUES_MAX.K2 = KVALUES_MAX.K3 = KVALUES_MAX.K4 = KVALUES_MAX.K5 = 255;
    KVALUES_MAX.K6 = 0;

    asNum = -1;
    maximumPath = 4;
    variance = 1;

    kValues.K1 = kValues.K3 = 1;
    kValues.K2 = kValues.K4 = kValues.K5 = kValues.K6 = 0;
}

EigrpIpv4Pdm::~EigrpIpv4Pdm()
{
    delete this->routingForNetworks;
    delete this->eigrpIftDisabled;
    delete this->eigrpDual;
    delete this->eigrpMetric;
}

void EigrpIpv4Pdm::initialize(int stage)
{
    // in stage 0 interfaces are registrated
    // in stage 2 interfaces and routing table
    // in stage 3
    if (stage == 3)
    {
        this->eigrpIft = ModuleAccess<EigrpInterfaceTable>("eigrpInterfaceTable").get();
        this->eigrpNt = ModuleAccess<EigrpIpv4NeighborTable>("eigrpIpv4NeighborTable").get();
        this->eigrpTt = ModuleAccess<EigrpIpv4TopologyTable>("eigrpIpv4TopologyTable").get();
        this->rt = AnsaRoutingTableAccess().get();
        //this->eigrpDual = ModuleAccess<EigrpDual>("eigrpDual").get();

        this->ift = InterfaceTableAccess().get();
        this->nb = NotificationBoardAccess().get();

        this->eigrpIftDisabled = new EigrpDisabledInterfaces();
        this->routingForNetworks = new EigrpNetworkTable();
        this->eigrpDual = new EigrpDual(this);
        this->eigrpMetric = new EigrpMetricHelper();

        // Load configuration of EIGRP
        DeviceConfigurator *conf = ModuleAccess<DeviceConfigurator>("deviceConfigurator").get();
        conf->loadEigrpIPv4Config(this);

        WATCH_PTRVECTOR(*routingForNetworks->getAllNetworks());
        WATCH(asNum);
        WATCH(kValues);
        WATCH(maximumPath);
        WATCH(variance);

        // Subscribe for changes in the device
        nb->subscribe(this, NF_INTERFACE_STATE_CHANGED);
        nb->subscribe(this, NF_INTERFACE_CONFIG_CHANGED);
        nb->subscribe(this, NF_IPv4_ROUTE_DELETED);
    }
}


void EigrpIpv4Pdm::receiveChangeNotification(int category, const cObject *details)
{
    // ignore notifications during initialization
    if (simulation.getContextType() == CTX_INITIALIZE)
        return;

    Enter_Method_Silent();
    printNotificationBanner(category, details);

    if (category == NF_INTERFACE_STATE_CHANGED)
    {
        InterfaceEntry *iface = check_and_cast<InterfaceEntry*>(details);
        processIfaceStateChange(iface);
    }
    else if (category == NF_INTERFACE_CONFIG_CHANGED)
    {
        InterfaceEntry *iface = check_and_cast<InterfaceEntry*>(details);
        EigrpInterface *eigrpIface = getInterfaceById(iface->getInterfaceId());
        double ifParam;

        if (eigrpIface == NULL)
            return;

        ifParam = iface->getBandwidth();
        if (ifParam != eigrpIface->getBandwidth())
        { // Bandwidth
            eigrpIface->setBandwidth(ifParam);
            if (eigrpIface->isEnabled())
                processIfaceConfigChange(eigrpIface);
        }
        ifParam = iface->getDelay();
        if (ifParam != eigrpIface->getDelay())
        { // Delay
            eigrpIface->setDelay(ifParam);
            if (eigrpIface->isEnabled())
                processIfaceConfigChange(eigrpIface);
        }
        // TODO reagovat na zmenu load, reliability (vyvolani upraveni cest podminit k-hodnotami) a mtu (zmena pacing time)
    }
}

void EigrpIpv4Pdm::handleMessage(cMessage *msg)
{
    if (msg->isSelfMessage())
    { // Timer
        this->processTimer(msg);
    }
    else
    { // EIGRP message
        if (dynamic_cast<EigrpMessage *>(msg))
        {
            processMsgFromNetwork(msg);
            delete msg->removeControlInfo();
            delete msg;
            msg = NULL;
        }
        else if (dynamic_cast<EigrpMsgReq *>(msg))
        {
            processMsgFromRtp(msg);
            delete msg;
            msg = NULL;
        }
    }
}

void EigrpIpv4Pdm::processMsgFromNetwork(cMessage *msg)
{
    // Get source IP address and interface ID
    IPv4ControlInfo *ctrlInfo =check_and_cast<IPv4ControlInfo *>(msg->getControlInfo());
    IPv4Address srcAddr = ctrlInfo->getSrcAddr();
    int ifaceId = ctrlInfo->getInterfaceId();
    cMessage *msgDup = NULL;

    // Find neighbor if exists
    EigrpNeighbor<IPv4Address> *neigh;
    if ((neigh = eigrpNt->findNeighbor(srcAddr)) != NULL)
    { // Reset hold timer
        resetTimer(neigh->getHoldTimer(), neigh->getHoldInt());
    }

    // Send message to RTP (message must be duplicated)
    msgDup = msg->dup();
    msgDup->setControlInfo(ctrlInfo->dup());
    send(msgDup, RTP_OUTGW);

    if (dynamic_cast<EigrpIpv4Ack*>(msg) && neigh != NULL)
    {
        printRecvMsg(check_and_cast<EigrpIpv4Ack*>(msg), srcAddr, ifaceId);
        if (neigh->isStateUp() == false)
        {
            // If neighbor is "pending", then change its state to "up"
            neigh->setStateUp(true);
            // Send all EIGRP paths from routing table to sender
            sendAllEigrpPaths(eigrpIft->findInterfaceById(ifaceId), neigh);
        }
    }
    else if (dynamic_cast<EigrpIpv4Hello*>(msg))
    {
        processHelloMsg(msg, srcAddr, ifaceId, neigh);
    }
    else if (dynamic_cast<EigrpIpv4Update*>(msg) && neigh != NULL)
    {
        processUpdateMsg(msg, srcAddr, ifaceId, neigh);
    }
    else if (dynamic_cast<EigrpIpv4Query*>(msg) && neigh != NULL)
    {
        processQueryMsg(msg, srcAddr, ifaceId, neigh);
    }
    else if (dynamic_cast<EigrpIpv4Reply*>(msg) && neigh != NULL)
    {
        processReplyMsg(msg, srcAddr, ifaceId, neigh);
    }
    else if (neigh != NULL)
    {
        EV << "EIGRP: Received message of unknown type, skipped" << endl;
    }
    else
    {
        EV << "EIGRP: Received message from "<< srcAddr <<" that is not neighbor, skipped" << endl;
    }
}

// TODO zlepsit rozhrani mezi PDM a RTP (neprehledne, nejednotne - ack zprava je NULL, neusporne - mnoho duplikaci)
void EigrpIpv4Pdm::processMsgFromRtp(cMessage *msg)
{
    EigrpMsgReq *msgReq = check_and_cast<EigrpMsgReq *>(msg);
    EigrpMessage *eigrpMsg = NULL;
    EigrpIpv4Message *eigrpMsgRt = NULL;
    int routeCnt = msgReq->getRoutesArraySize();
    IPv4Address destAddress;
    int destIface = msgReq->getDestInterface();
    EigrpInterface *eigrpIface = eigrpIft->findInterfaceById(destIface);

    if (!getDestIpAddress(msgReq->getDestNeighbor(), &destAddress) || eigrpIface == NULL)
    { // Discard message request
        return;
    }

    // Create EIGRP message
    switch (msgReq->getOpcode())
    {
    case EIGRP_HELLO_MSG:
        if (msgReq->getGoodbyeMsg() == true)        // Goodbye message
            eigrpMsg = createHelloMsg(eigrpIface->getHoldInt(), this->KVALUES_MAX, destAddress, msgReq);
        else if (msgReq->getAckNumber() > 0)    // Ack message
            eigrpMsg = createAckMsg(destAddress, msgReq);
        else // Hello message
            eigrpMsg = createHelloMsg(eigrpIface->getHoldInt(), this->kValues, destAddress, msgReq);
        break;

    case EIGRP_UPDATE_MSG:
        eigrpMsgRt = createUpdateMsg(destAddress, msgReq);
        // Add route TLV
        if (routeCnt > 0) addRoutesToMsg(eigrpMsgRt, msgReq);

        eigrpMsg = eigrpMsgRt;
        break;

    case EIGRP_QUERY_MSG:
        eigrpMsgRt = createQueryMsg(destAddress, msgReq);
        // Add route TLV
        if (routeCnt > 0) addRoutesToMsg(eigrpMsgRt, msgReq);

        eigrpMsg = eigrpMsgRt;
        break;

    case EIGRP_REPLY_MSG:
        eigrpMsgRt = createReplyMsg(destAddress, msgReq);
        // Add route TLV
        if (routeCnt > 0) addRoutesToMsg(eigrpMsgRt, msgReq);

        eigrpMsg = eigrpMsgRt;
        break;

    default:
        ASSERT(false);
        return;
        break;
    }

    // Print message information
    printSentMsg(eigrpMsg, routeCnt, destAddress, msgReq);

    // Send message to network
    send(eigrpMsg, SPLITTER_OUTGW);
}

bool EigrpIpv4Pdm::getDestIpAddress(int destNeigh, IPv4Address *resultAddress)
{
    EigrpNeighbor<IPv4Address> *neigh = NULL;

    if (destNeigh == EigrpNeighbor<IPv4Address>::UNSPEC_ID)
        resultAddress->set(EIGRP_IPV4_MULT.getInt());
    else
    {
        if ((neigh = eigrpNt->findNeighborById(destNeigh)) == NULL)
            return false;
        resultAddress->set(neigh->getIPAddress().getInt());
    }

    return true;
}

void EigrpIpv4Pdm::printSentMsg(EigrpMessage *msg, int routeCnt, IPv4Address& destAddress, EigrpMsgReq *msgReq)
{
    int type = msg->getOpcode();

    ev << "EIGRP: send " << eigrp::UserMsgs[type];
    if (type == EIGRP_HELLO_MSG)
    {
        if (msg->getAckNum() > 0)
            ev << " (ack) ";
        else if (msgReq->getGoodbyeMsg() == true)
            ev << " (goodbye) ";
    }

    ev << " message to " << destAddress << " on IF " << msgReq->getDestInterface();

    // Print flags
    ev << ", flags: ";
    if (msg->getInit()) ev << "init";
    else if (msg->getEot()) ev << "eot";
    else if (msg->getCr()) ev << "cr";
    else if (msg->getRs()) ev << "rs";

    if (type != EIGRP_HELLO_MSG) ev << ", route count: " << routeCnt;
    ev << ", seq num:" << msg->getSeqNum();
    ev << ", ack num:" << msg->getAckNum();
    ev << endl;
}

void EigrpIpv4Pdm::printRecvMsg(EigrpMessage *msg, IPv4Address& addr, int ifaceId)
{
    EV << "EIGRP: received " << eigrp::UserMsgs[msg->getOpcode()];
    if (msg->getOpcode() == EIGRP_HELLO_MSG && msg->getAckNum() > 0)
        EV << " (ack) ";
    EV << " message from " << addr << " on IF " << ifaceId;

    EV << ", flags: ";
    if (msg->getInit()) EV << "init";
    else if (msg->getEot()) EV << "eot";
    else if (msg->getCr()) EV << "cr";
    else if (msg->getRs()) EV << "rs";

    EV << ", seq num:" << msg->getSeqNum();
    EV << ", ack num:" << msg->getAckNum();
    EV << endl;
}

void EigrpIpv4Pdm::processTimer(cMessage *msg)
{
    EigrpTimer *timer = check_and_cast<EigrpTimer*>(msg);
    EigrpInterface *eigrpIface = NULL;
    InterfaceEntry *iface = NULL;
    IPv4Address ifMask;
    EigrpNeighbor<IPv4Address> *neigh = NULL;
    cObject *contextBasePtr = NULL;
    EigrpMsgReq *msgReq = NULL;
    int ifaceId = -1;

    switch (timer->getTimerKind())
    {
    case EIGRP_HELLO_TIMER:
        // get interface that belongs to timer
        contextBasePtr = (cObject*)timer->getContextPointer();
        eigrpIface = check_and_cast<EigrpInterface *>(contextBasePtr);

        // schedule Hello timer
        scheduleAt(simTime() + eigrpIface->getHelloInt() /*- uniform(0, 0.4)*/, timer);

        // send Hello message
        msgReq = createMsgReq(EIGRP_HELLO_MSG, EigrpNeighbor<IPv4Address>::UNSPEC_ID, eigrpIface->getInterfaceId());
        send(msgReq, RTP_OUTGW);
        break;

    case EIGRP_HOLD_TIMER:
        // get neighbor from context
        contextBasePtr = (cObject*)timer->getContextPointer();
        neigh = check_and_cast<EigrpNeighbor<IPv4Address> *>(contextBasePtr);
        ifaceId = neigh->getIfaceId();

        // remove neighbor
        EV << "Neighbor " << neigh->getIPAddress() <<" is down, holding time expired" << endl;
        iface = ift->getInterfaceById(ifaceId);
        ifMask = iface->ipv4Data()->getNetmask();
        removeNeighbor(neigh, ifMask);
        neigh = NULL;

        // Send goodbye and restart Hello timer
        eigrpIface = eigrpIft->findInterfaceById(ifaceId);
        resetTimer(eigrpIface->getHelloTimer(), eigrpIface->getHelloInt());
        msgReq = createMsgReq(EIGRP_HELLO_MSG, EigrpNeighbor<IPv4Address>::UNSPEC_ID, ifaceId);
        msgReq->setGoodbyeMsg(true);
        send(msgReq, RTP_OUTGW);
        break;

    default:
        EV << "Timer with unknown kind was skipped" << endl;
        delete timer;
        timer = NULL;
        break;
    }
}

void EigrpIpv4Pdm::processHelloMsg(cMessage *msg, IPv4Address& srcAddress, int ifaceId, EigrpNeighbor<IPv4Address> *neigh)
{
    InterfaceEntry *iface = NULL;
    IPv4Address ifMask;
    EigrpHello *hello = check_and_cast<EigrpHello *>(msg);
    EigrpTlvParameter tlvParam = hello->getParameterTlv();

    printRecvMsg(hello, srcAddress, ifaceId);

    if (tlvParam.kValues == KVALUES_MAX)
    { // Received Goodbye message, remove neighbor
        EV << "Interface goodbye received" << endl;
        if (neigh != NULL)
        {
            EV << "EIGRP neighbor " << srcAddress << " is down, interface goodbye received" << endl;
            iface = ift->getInterfaceById(ifaceId);
            ifMask = iface->ipv4Data()->getNetmask();
            removeNeighbor(neigh, ifMask);
        }
        return;
    }

    if (neigh == NULL)
    { // New neighbor
        processNewNeighbor(ifaceId, srcAddress, hello);
    }
    else
    { // neighbor exists, its state is "up" or "pending"
        // Check K-values
        if (!(tlvParam.kValues == this->kValues))
        { // Not satisfied
            EV << "EIGRP neighbor " << srcAddress << " is down, " << eigrp::UserMsgs[eigrp::M_NEIGH_BAD_KVALUES] << endl;
            iface = ift->getInterfaceById(ifaceId);
            ifMask = iface->ipv4Data()->getNetmask();
            removeNeighbor(neigh, ifMask);

            // send Goodbye message and reset Hello timer
            EigrpInterface *iface = this->eigrpIft->findInterfaceById(ifaceId);
            resetTimer(iface->getHelloTimer(), iface->getHelloInt());
            EigrpMsgReq *msgReq = createMsgReq(EIGRP_HELLO_MSG, EigrpNeighbor<IPv4Address>::UNSPEC_ID, ifaceId);
            msgReq->setGoodbyeMsg(true);
            send(msgReq, RTP_OUTGW);

            return;
        }

        // Save Hold interval
        if (tlvParam.holdTimer != neigh->getHoldInt())
            neigh->setHoldInt(tlvParam.holdTimer);
    }
}

void EigrpIpv4Pdm::processUpdateMsg(cMessage *msg, IPv4Address& srcAddress, int ifaceId, EigrpNeighbor<IPv4Address> *neigh)
{
    EigrpIpv4Update *update = check_and_cast<EigrpIpv4Update *>(msg);
    EigrpInterface *eigrpIface = eigrpIft->findInterfaceById(ifaceId);
    EigrpRouteSource<IPv4Address> *src;

    printRecvMsg(update, srcAddress, ifaceId);

    if (neigh->isStateUp() == false && update->getAckNum() != 0)
    { // First ack from neighbor
        // If neighbor is "pending", then change its state to "up"
        neigh->setStateUp(true);
        // Send all EIGRP paths from routing table to sender
        sendAllEigrpPaths(eigrpIface, neigh);
    }

    if (update->getInit())
    { // Request to send all paths from routing table
        if (neigh->isStateUp() == true)
        {
            sendAllEigrpPaths(eigrpIface, neigh);
        }
    }
    else if (neigh->isStateUp())
    { // Neighbor is "up", forward message to DUAL
        int cnt = update->getInterRoutesArraySize();
#ifdef EIGRP_DEBUG
        ev << "     Route count:" << cnt << endl;
#endif

        for (int i = 0; i < cnt; i++)
        {
            src = processInterRoute(update->getInterRoutes(i), srcAddress, neigh->getNeighborId(), eigrpIface);
            // Notify DUAL about event
            eigrpDual->processEvent(EigrpDual::RECV_UPDATE, src, neigh->getNeighborId());
        }
        flushMsgRequests();
    }
    // else ignore message
}

void EigrpIpv4Pdm::processQueryMsg(cMessage *msg, IPv4Address& srcAddress, int ifaceId, EigrpNeighbor<IPv4Address> *neigh)
{
    EigrpIpv4Query *query = check_and_cast<EigrpIpv4Query *>(msg);
    EigrpInterface *eigrpIface = eigrpIft->findInterfaceById(ifaceId);
    EigrpRouteSource<IPv4Address> *src;

    printRecvMsg(query, srcAddress, ifaceId);

    int cnt = query->getInterRoutesArraySize();
#ifdef EIGRP_DEBUG
    ev << "     Route count:" << cnt << endl;
#endif

    for (int i = 0; i < cnt; i++)
    {
        src = processInterRoute(query->getInterRoutes(i), srcAddress, neigh->getNeighborId(), eigrpIface);
        eigrpDual->processEvent(EigrpDual::RECV_QUERY, src, neigh->getNeighborId());
    }
    flushMsgRequests();
}

void EigrpIpv4Pdm::processReplyMsg(cMessage *msg, IPv4Address& srcAddress, int ifaceId, EigrpNeighbor<IPv4Address> *neigh)
{
    EigrpIpv4Reply *reply = check_and_cast<EigrpIpv4Reply *>(msg);
    EigrpInterface *eigrpIface = eigrpIft->findInterfaceById(ifaceId);
    EigrpRouteSource<IPv4Address> *src;

    printRecvMsg(reply, srcAddress, ifaceId);

    int cnt = reply->getInterRoutesArraySize();
#ifdef EIGRP_DEBUG
    ev << "     Route count:" << cnt << endl;
#endif

    for (int i = 0; i < cnt; i++)
    {
        src = processInterRoute(reply->getInterRoutes(i), srcAddress, neigh->getNeighborId(), eigrpIface);
        eigrpDual->processEvent(EigrpDual::RECV_REPLY, src, neigh->getNeighborId());
    }
    flushMsgRequests();
}

/**
 * @param neigh Neighbor which is next hop for a route in TLV.
 */
EigrpRouteSource<IPv4Address> *EigrpIpv4Pdm::processInterRoute(EigrpIpv4Internal& tlv, IPv4Address& srcAddr,
        int sourceNeighId, EigrpInterface *eigrpIface)
{
    int ifaceId = eigrpIface->getInterfaceId();
    bool isSourceNew = false;
    IPv4Address nextHop = getNextHopAddr(tlv.nextHop, srcAddr);
    EigrpNeighbor<IPv4Address> *nextHopNeigh = eigrpNt->findNeighbor(nextHop);
    EigrpRouteSource<IPv4Address> *src;
    EigrpMetricPar newParam, oldParam;
    EigrpMetricPar ifParam = eigrpMetric->getParam(eigrpIface);

    // Find route or create one (route source is identified by ID of the next hop - not by ID of sender)
    src = eigrpTt->findOrCreateRoute(tlv.destAddress, tlv.destMask, ifaceId, nextHopNeigh->getNeighborId(), &isSourceNew);

    // Get new metric parameters
    newParam = eigrpMetric->adjustParam(ifParam, tlv.metric);
    // Get old metric parameters for comparison
    oldParam = src->getMetricParams();

    // Compute reported distance
    uint32_t metric = eigrpMetric->computeMetric(tlv.metric, this->kValues);
    src->setRdParams(tlv.metric);
    src->setRd(metric);

    if (isSourceNew || !eigrpMetric->compareParamters(newParam, oldParam, this->kValues))
    {
        // Set source of route
        if (isSourceNew)
            src->setNextHop(nextHop);
        src->setMetricParams(newParam);
        // Compute metric
        metric = eigrpMetric->computeMetric(newParam, this->kValues);
        src->setMetric(metric);
    }

    return src;
}

EigrpTimer *EigrpIpv4Pdm::createTimer(char timerKind, void *context)
{
    EigrpTimer *timer = new EigrpTimer();
    timer->setTimerKind(timerKind);
    timer->setContextPointer(context);

    return timer;
}

inline void EigrpIpv4Pdm::resetTimer(EigrpTimer *timer, int interval)
{
    cancelEvent(timer);
    // Uniform je zde, protoze bez nej nefunguje simulace jak by mela
    // (pri ustaveni sousedstvi se detekuji spatne K-hodnoty (coz je spravne),
    // odesle se Goodbye (spravne), ale hned po ni take Hello (spatne, goodbye by
    // mela resetovat Hello timer, ale asi to nestihne).
    //      UZ TO NEDELA - NEVIM PROC
    scheduleAt(simTime() + interval /*- uniform(0,0.4)*/, timer);
}

EigrpIpv4Hello *EigrpIpv4Pdm::createHelloMsg(int holdInt, EigrpKValues kValues, IPv4Address& destAddress, EigrpMsgReq *msgReq)
{
    EigrpIpv4Hello *msg = new EigrpIpv4Hello("EIGRPHello");
    EigrpTlvParameter paramTlv;

    addMessageHeader(msg, EIGRP_HELLO_MSG, msgReq);
    addCtrInfo(msg, msgReq->getDestInterface(), destAddress);

    // add parameter type TLV
    paramTlv.holdTimer = holdInt;
    paramTlv.kValues = kValues;
    msg->setParameterTlv(paramTlv);

    return msg;
}

EigrpIpv4Ack *EigrpIpv4Pdm::createAckMsg(IPv4Address& destAddress, EigrpMsgReq *msgReq)
{
    EigrpIpv4Ack *msg = new EigrpIpv4Ack("EIGRPAck");
    addMessageHeader(msg, EIGRP_HELLO_MSG, msgReq);
    msg->setAckNum(msgReq->getAckNumber());
    addCtrInfo(msg, msgReq->getDestInterface(), destAddress);
    return msg;
}

EigrpIpv4Update *EigrpIpv4Pdm::createUpdateMsg(IPv4Address& destAddress, EigrpMsgReq *msgReq)
{
    EigrpIpv4Update *msg = new EigrpIpv4Update("EIGRPUpdate");
    addMessageHeader(msg, EIGRP_UPDATE_MSG, msgReq);
    msg->setAckNum(msgReq->getAckNumber());
    msg->setSeqNum(msgReq->getSeqNumber());
    addCtrInfo(msg, msgReq->getDestInterface(), destAddress);

    return msg;
}

EigrpIpv4Query *EigrpIpv4Pdm::createQueryMsg(IPv4Address& destAddress, EigrpMsgReq *msgReq)
{
    EigrpIpv4Query *msg = new EigrpIpv4Query("EIGRPQuery");
    addMessageHeader(msg, EIGRP_QUERY_MSG, msgReq);
    msg->setAckNum(msgReq->getAckNumber());
    msg->setSeqNum(msgReq->getSeqNumber());
    addCtrInfo(msg, msgReq->getDestInterface(), destAddress);
    return msg;
}

EigrpIpv4Reply *EigrpIpv4Pdm::createReplyMsg(IPv4Address& destAddress, EigrpMsgReq *msgReq)
{
    EigrpIpv4Reply *msg = new EigrpIpv4Reply("EIGRPReply");
    addMessageHeader(msg, EIGRP_REPLY_MSG, msgReq);
    msg->setAckNum(msgReq->getAckNumber());
    msg->setSeqNum(msgReq->getSeqNumber());
    addCtrInfo(msg, msgReq->getDestInterface(), destAddress);
    return msg;
}

void EigrpIpv4Pdm::addMessageHeader(EigrpMessage *msg, int opcode, EigrpMsgReq *msgReq)
{
    msg->setOpcode(opcode);
    msg->setAsNum(this->asNum);
    msg->setInit(msgReq->getInit());
    msg->setEot(msgReq->getEot());
    msg->setRs(msgReq->getRs());
    msg->setCr(msgReq->getCr());
}

void EigrpIpv4Pdm::addCtrInfo(EigrpMessage *msg, int ifaceId,
        const IPv4Address &destAddress)
{
    IPv4ControlInfo *ctrl = new IPv4ControlInfo();
    ctrl->setDestAddr(destAddress);
    ctrl->setProtocol(88);
    ctrl->setTimeToLive(1);
    ctrl->setInterfaceId(ifaceId);

    msg->setControlInfo(ctrl);
}

void EigrpIpv4Pdm::createRouteTlv(EigrpIpv4Internal *routeTlv, EigrpRoute<IPv4Address> *route, bool unreachable)
{
    routeTlv->destAddress = route->getRouteAddress();
    routeTlv->destMask = route->getRouteMask();
    routeTlv->nextHop = IPv4Address::UNSPECIFIED_ADDRESS;
    routeTlv->metric = route->getRdPar();
    if (unreachable)
        routeTlv->metric.delay = UINT32_MAX;
}

/**
 * Add routes to the message
 */
void EigrpIpv4Pdm::addRoutesToMsg(EigrpIpv4Message *msg, const EigrpMsgReq *msgReq)
{
    // Add routes to the message
    int reqCnt = msgReq->getRoutesArraySize();
    EigrpRouteSource<IPv4Address> *source = NULL;

    msg->setInterRoutesArraySize(reqCnt);
    for (int i = 0; i < reqCnt; i++)
    {
        EigrpMsgRoute req = msgReq->getRoutes(i);
        source = eigrpTt->findRouteById(req.sourceId);
        EigrpIpv4Internal routeTlv;
        createRouteTlv(&routeTlv, source->getRouteInfo(), req.unreachable);
        msg->setInterRoutes(i, routeTlv);
    }
}

EigrpMsgReq *EigrpIpv4Pdm::createMsgReq(HeaderOpcode msgType, int destNeighbor, int destIface)
{
    EigrpMsgReq *msgReq = new EigrpMsgReq(eigrp::UserMsgs[msgType]);

    msgReq->setOpcode(msgType);
    msgReq->setDestNeighbor(destNeighbor);
    msgReq->setDestInterface(destIface);

    return msgReq;
}

/**
 * In the RT may not be all EIGRP routes due to administrative distance of route.
 * Send all path from EIGRP TT.
 */
void EigrpIpv4Pdm::sendAllEigrpPaths(EigrpInterface *eigrpIface, EigrpNeighbor<IPv4Address> *neigh)
{
    int routeCnt = eigrpTt->getNumRouteInfo();
    int addedRoutes = 0;    // Number of routes in message
    EigrpRoute<IPv4Address> *route;
    EigrpRouteSource<IPv4Address> *source;
    EigrpMsgReq *msgReq = createMsgReq(EIGRP_UPDATE_MSG, neigh->getNeighborId(), neigh->getIfaceId());

    msgReq->setRoutesArraySize(routeCnt);

    // TODO vsechny cesty nemusi byt v 1 zprave, ale je to nejak rozdeleny
#ifdef EIGRP_DEBUG
    ev << "Send all routes from TT:" << endl;
#endif

    for (int i = 0; i < routeCnt; i++)
    {
        route = eigrpTt->getRouteInfo(i);
        if (route->isActive())
            continue;

        if ((source = route->getSuccessor()) != NULL)
        {
            if (applySplitHorizon(eigrpIface, source))
            { // Apply Split Horizon rule
                continue;
            }

            EigrpMsgRoute routeReq;
            routeReq.sourceId = source->getSourceId();
            msgReq->setRoutes(addedRoutes /* not variable i */, routeReq);
            addedRoutes++;
#ifdef EIGRP_DEBUG
            ev << "     route:" << route->getRouteAddress() << "/" << route->getRouteMask().getNetmaskLength() << endl;
#endif
        }
    }

    if (addedRoutes < routeCnt) // reduce size of array
        msgReq->setRoutesArraySize(addedRoutes);

    msgReq->setEot(true);

    send(msgReq, RTP_OUTGW);
}

/**
 * Creates relationship with neighbor.
 *
 * @param srcAddress address of the neighbor
 * @param ifaceId ID of interface where the neighbor is connected
 * @param helloMe
 */
void EigrpIpv4Pdm::processNewNeighbor(int ifaceId, IPv4Address &srcAddress, EigrpHello *rcvMsg)
{
    EigrpMsgReq *msgReqUpdate = NULL, *msgReqHello = NULL;
    EigrpNeighbor<IPv4Address> *neigh;
    EigrpInterface *iface = eigrpIft->findInterfaceById(ifaceId);
    EigrpTlvParameter paramTlv = rcvMsg->getParameterTlv();
    int ecode;      // Code of user message

    // Check rules for establishing neighborship
    if ((ecode = checkNeighborshipRules(ifaceId, rcvMsg->getAsNum(), srcAddress,
            paramTlv.kValues)) != eigrp::M_OK)
    {
        EV << "EIGRP can't create neighborship with " << srcAddress << ", " << eigrp::UserMsgs[ecode] << endl;

        if (ecode == eigrp::M_NEIGH_BAD_KVALUES)
        { // Send Goodbye message and reset Hello timer
            resetTimer(iface->getHelloTimer(), iface->getHelloInt());
            msgReqHello = createMsgReq(EIGRP_HELLO_MSG, EigrpNeighbor<IPv4Address>::UNSPEC_ID, ifaceId);
            msgReqHello->setGoodbyeMsg(true);
            send(msgReqHello, RTP_OUTGW);
        }
        return;
    }

    EV << "Neighbor " << srcAddress << " is up, new adjacency" << endl;

    neigh = createNeighbor(iface, srcAddress, paramTlv.holdTimer);

    // Reply with Hello message and reset Hello timer
    resetTimer(iface->getHelloTimer(), iface->getHelloInt());
    msgReqHello = createMsgReq(EIGRP_HELLO_MSG, EigrpNeighbor<IPv4Address>::UNSPEC_ID, ifaceId);
    send(msgReqHello, RTP_OUTGW);

    // Send Update with INIT flag
    msgReqUpdate = createMsgReq(EIGRP_UPDATE_MSG, neigh->getNeighborId(), neigh->getIfaceId());
    msgReqUpdate->setInit(true);
    send(msgReqUpdate, RTP_OUTGW);
}

/**
 * @param ifaceId ID of interface where the neighbor is connected
 *
 * @return returns code from enumeration eigrp::UserMsgCodes
 */
int EigrpIpv4Pdm::checkNeighborshipRules(int ifaceId, int neighAsNum,
        IPv4Address &neighAddr, const EigrpKValues &neighKValues)
{
    IPv4Address ifaceAddr, ifaceMask;

    if (this->eigrpIft->findInterfaceById(ifaceId) == NULL)
    { // EIGRP must be enabled on interface
        return eigrp::M_DISABLED_ON_IF;
    }
    else if (this->asNum != neighAsNum)
    { // AS nubers must be equal
        // TODO az bude AsSplitter funkcni, tak zpravy s neznamym AS cislem
        // bude zahazovat. Takze tato kontrola je tady zbytecna.
        return eigrp::M_NEIGH_BAD_AS;
    }
    else if (!(this->kValues == neighKValues))
    { // K-values must be same
        return eigrp::M_NEIGH_BAD_KVALUES;
    }
    else
    {
        // get IP address of interface and mask
        InterfaceEntry *iface = ift->getInterfaceById(ifaceId);
        ifaceAddr.set(iface->ipv4Data()->getIPAddress().getInt());
        ifaceMask.set(iface->ipv4Data()->getNetmask().getInt());

        // check, if the neighbor is on same subnet
        if (!ifaceAddr.maskedAddrAreEqual(ifaceAddr, neighAddr, ifaceMask))
        {
            return eigrp::M_NEIGH_BAD_SUBNET;
        }
    }

    return eigrp::M_OK;
}

EigrpNeighbor<IPv4Address> *EigrpIpv4Pdm::createNeighbor(EigrpInterface *eigrpIface, IPv4Address& address, uint16_t holdInt)
{
    // Create record in the neighbor table
    EigrpNeighbor<IPv4Address> *neigh = new EigrpNeighbor<IPv4Address>(eigrpIface->getInterfaceId(), address);
    EigrpTimer *holdt = createTimer(EIGRP_HOLD_TIMER, neigh);
    neigh->setHoldTimer(holdt);
    neigh->setHoldInt(holdInt);
    eigrpNt->addNeighbor(neigh);
    // Start Hold timer
    scheduleAt(simTime() + holdInt, holdt);

    // Increase number of neighbors on interface
    eigrpIface->incNumOfNeighbors();

    return neigh;
}

/**
 * Removes neighbor from neighbor table and delete it.
 */
void EigrpIpv4Pdm::removeNeighbor(EigrpNeighbor<IPv4Address> *neigh, IPv4Address& neighMask)
{
    EigrpRouteSource<IPv4Address> *source = NULL;
    EigrpRoute<IPv4Address> *route = NULL;
    EigrpInterface *eigrpIface = eigrpIft->findInterfaceById(neigh->getIfaceId());

    // Remove neighbor from NT
    this->eigrpNt->removeNeighbor(neigh);
    eigrpIface->decNumOfNeighbors();

    // Process routes that go via this neighbor
    int routeCount = eigrpTt->getNumRouteInfo();
    int nextHopId = neigh->getNeighborId();
    int ifaceId = neigh->getIfaceId();
    int routeId;

    for (int i = 0; i < routeCount; i++)
    {
        route = eigrpTt->getRouteInfo(i);
        routeId = route->getRouteId();
        source = eigrpTt->findRouteByNextHop(routeId, nextHopId);

        if (source != NULL)
        {
            // Notify DUAL about event for the route
            eigrpDual->processEvent(EigrpDual::NEIGHBOR_DOWN, source, nextHopId);
        }
        else if (route->isActive())
        { // Create dummy source for active route (instead of Reply)
            source = new EigrpRouteSource<IPv4Address>(ifaceId, nextHopId, routeId, route);
            eigrpTt->addRoute(source);
            // Notify DUAL about event for the route
            eigrpDual->processEvent(EigrpDual::NEIGHBOR_DOWN, source, nextHopId);
        }
    }

    // Do not flush messages in transmit queue

    delete neigh;
}

EigrpIpv4Pdm::RouteVector::iterator EigrpIpv4Pdm::findRouteInRT(EigrpRouteSource<IPv4Address> *source, EigrpRoute<IPv4Address> *route)
{
    // kdyby nekdo pomoci prepareForAddRoute smazal moji cestu, tak
    // neprijmu udalost informujici o smazani (pouziva se deleteRouteSilent). Takze bych mohl mit vector s odkazy na
    // kusy pameti, ktere mi nepatri -> problem.

    RouteVector::iterator it;

    IPv4Route *rtEntry;
    IPv4Address routeAddr = route->getRouteAddress();
    IPv4Address routeMask = route->getRouteMask();
    IPv4Address nextHop = source->getNextHop();

    for (it = eigrpRtRoutes.begin(); it != eigrpRtRoutes.end(); it++)
    {
        rtEntry = *it;
        if (rtEntry->getDestination() == routeAddr && rtEntry->getNetmask() == routeMask && rtEntry->getGateway() == nextHop)
            return it;
    }

    return it;
}

ANSAIPv4Route *EigrpIpv4Pdm::createRTRoute(EigrpRouteSource<IPv4Address> *successor)
{
    ANSAIPv4Route *rtEntry = new ANSAIPv4Route();
    int ifaceId = successor->getIfaceId();
    EigrpRoute<IPv4Address> *route = successor->getRouteInfo();

    rtEntry->setDestination(route->getRouteAddress());
    rtEntry->setNetmask(route->getRouteMask());
    rtEntry->setInterface(this->ift->getInterfaceById(ifaceId));
    rtEntry->setGateway(successor->getNextHop());
    rtEntry->setMetric(successor->getMetric());

    if (successor->isInternal())
    {
        // Set any source except IFACENETMASK and MANUAL
        rtEntry->setSource(IPv4Route::ZEBRA);
        // Set right protocol source
        rtEntry->setRoutingProtocolSource(ANSAIPv4Route::pEIGRP);
        rtEntry->setAdminDist(ANSAIPv4Route::dEIGRPInternal);
    }

    return rtEntry;
}

bool EigrpIpv4Pdm::createOrUpdateRouteInRT(EigrpRoute<IPv4Address> *route, EigrpRouteSource<IPv4Address> *source, bool *checkRTSafe, bool *isRTSafe)
{
    ANSAIPv4Route *rtEntry;
    RouteVector::iterator rtIt;
    bool routeAdded = false;

    if (source->isSuccessor() && source->getMetric() != route->getDij() && (rtIt = findRouteInRT(source, route)) != this->eigrpRtRoutes.end())
    { // Update route in RT
        EV << "EIGRP: Update EIGRP route " << route->getRouteAddress() << " via " << source->getNextHop() << " in RT" << endl;
        (*rtIt)->setMetric(route->getDij());
        *checkRTSafe = false;       // In RT is at least one EIGRP route
    }
    else if (*isRTSafe)
    { // Add route to RT
        rtEntry = createRTRoute(source);

        if (*checkRTSafe)
        { // For the first time check if it is safe to add route
            (*isRTSafe) = rt->prepareForAddRoute(rtEntry);
            *checkRTSafe = false;
        }

        if (*isRTSafe)
        {
            EV << "EIGRP: Add EIGRP route " << route->getRouteAddress() << " via " << source->getNextHop() << " to RT" << endl;
            this->eigrpRtRoutes.push_back(rtEntry);
            rt->addRoute(rtEntry);
            routeAdded = true;
        }
        else
        { // Route can not be added to RT
            delete rtEntry;
#ifdef EIGRP_DEBUG
            EV << "DUAL: route " << route->getRouteAddress() << " can not be added to RT" << endl;
#endif
        }
    }

    return routeAdded;
}

/**
 * Record request to send message to all neighbors
 * @param srcNeighbor ID of neighbor that sends Query message. Neighbor is successor for the route.
 * Should be set only if DUAL sends Query message.
 */
void EigrpIpv4Pdm::msgToAllIfaces(HeaderOpcode msgType, EigrpRouteSource<IPv4Address> *source, bool updateFromS)
{
    EigrpInterface *eigrpIface;
    int ifCount = eigrpIft->getNumInterfaces();

    for (int i = 0; i < ifCount; i++)
    {
        eigrpIface = eigrpIft->getInterface(i);
        msgToIface(msgType, source, updateFromS, eigrpIface);
    }
}

void EigrpIpv4Pdm::msgToIface(HeaderOpcode msgType, EigrpRouteSource<IPv4Address> *source, bool updateFromS, EigrpInterface *eigrpIface, bool forcePoisonRev)
{
    EigrpNeighbor<IPv4Address> *neigh = NULL;
    EigrpMsgRoute msgRt;
    int ifaceId, destNeigh = 0;
    int numOfNeigh;

    if ((numOfNeigh = eigrpIface->getNumOfNeighbors()) == 0)
        return;

    if (applySplitHorizon(eigrpIface, source))
    {
        if (updateFromS || forcePoisonRev)
        { // Apply Poison Reverse instead of Split Horizon
            msgRt.unreachable = true;
        }
        else // Apply Split Horizon rule - do not send message to the interface
            return;
    }

    msgRt.sourceId = source->getSourceId();
    msgRt.invalid = false;

    // Get destination interface ID and destination neighbor ID
    ifaceId = eigrpIface->getInterfaceId();
    if (numOfNeigh > 1 && ift->getInterfaceById(ifaceId)->isMulticast())
    { // Multicast
        destNeigh = IEigrpPdm::UNSPEC_RECEIVER;
    }
    else
    { // Unicast
        neigh = eigrpNt->getFirstNeighborOnIf(ifaceId);
        destNeigh = neigh->getNeighborId();
    }

    pushMsgRouteToQueue(msgType, ifaceId, destNeigh, msgRt);
}

/**
 * Insert route into the queue.
 */
EigrpMsgReq *EigrpIpv4Pdm::pushMsgRouteToQueue(HeaderOpcode msgType, int ifaceId, int neighId, const EigrpMsgRoute& msgRt)
{
    EigrpMsgReq *request = NULL;
    RequestVector::iterator it;

    // Find or create message
    for (it = reqQueue.begin(); it != reqQueue.end(); it++)
    {
        if ((*it)->getDestInterface() == ifaceId && (*it)->getDestNeighbor() == neighId && (*it)->getOpcode() == msgType)
        {
            request = *it;
            break;
        }
    }
    if (request == NULL)
    {
        request = createMsgReq(msgType, neighId, ifaceId);
        request->setRoutesArraySize(1);
        request->setRoutes(0, msgRt);
        reqQueue.push_back(request);
    }
    else
    {
        int rtSize = request->getRoutesArraySize();
        request->setRoutesArraySize(rtSize + 1);
        request->setRoutes(rtSize, msgRt);
    }

    return request;
}

/*
 * void EigrpIpv4Pdm::addRoutesToMsg(EigrpIpv4Message *msg, const EigrpMsgReq *msgReq)
{
    // Add routes to the message
    int reqCnt = requests.size();
    EigrpMsgRequest *req = NULL;
    EigrpRouteSource<IPv4Address> *source = NULL;

    msg->setInterRoutesArraySize(reqCnt);
    for (int i = 0; i < reqCnt; i++)
    {
        req = requests[i];
        source = eigrpTt->findRouteById(req->sourceId);
        EigrpIpv4Internal routeTlv;
        createRouteTlv(&routeTlv, source->getRouteInfo(), req->unreachable);
        msg->setInterRoutes(i, routeTlv);
    }
}
 */

void EigrpIpv4Pdm::flushMsgRequests()
{
    RequestVector::iterator it;
    IPv4Address destAddress;

    for (it = reqQueue.begin(); it != reqQueue.end(); it++)
    {
        // Check if interface exists
        if (eigrpIft->findInterfaceById((*it)->getDestInterface()) == NULL)
            delete *it; // Discard request
        else
            send(*it, RTP_OUTGW);
    }

    reqQueue.clear();
}

/*bool EigrpIpv4Pdm::getRoutesFromRequests(RequestVector *msgReqs)
{
    bool emptyQueue = true;
    EigrpMsgRequest *request = NULL;
    EigrpMsgRequest *reqToSend = NULL;
    RequestVector::iterator it;

    for (it = transmitionQueue.begin(); it != transmitionQueue.end(); it++)
    {
        if ((*it)->invalid)
            continue;

        request = *it;
        if (reqToSend == NULL)
        {
            emptyQueue = false;
            reqToSend = request;
        }

        if (reqToSend != NULL)
        {
            if (reqToSend->type == request->type && reqToSend->destInterface == request->destInterface &&
                    reqToSend->destNeighbor == request->destNeighbor)
            {
                request->invalid = true;
                msgReqs->push_back(request);
            }
        }
    }

    return emptyQueue;
}*/

/**
 * Returns EIGRP interface (enabled or disabled) or NULL.
 */
EigrpInterface *EigrpIpv4Pdm::getInterfaceById(int ifaceId)
{
    EigrpInterface *iface;

    if ((iface = eigrpIft->findInterfaceById(ifaceId)) != NULL)
        return iface;
    else
        return eigrpIftDisabled->findInterface(ifaceId);
}

void EigrpIpv4Pdm::processIfaceStateChange(InterfaceEntry *iface)
{
    EigrpInterface *eigrpIface;
    int ifaceId = iface->getInterfaceId();
    // Get address and mask of interface
    IPv4Address ifMask = iface->ipv4Data()->getNetmask();
    IPv4Address ifAddress = iface->ipv4Data()->getIPAddress().doAnd(ifMask);
    int networkId;

    if (iface->isUp())
    { // an interface go up
        if (routingForNetworks->isInterfaceIncluded(ifAddress, ifMask, &networkId))
        { // Interface is included in EIGRP
            if ((eigrpIface = getInterfaceById(ifaceId)) == NULL)
            { // Create EIGRP interface
                eigrpIface = new EigrpInterface(iface, networkId, false);
            }
            if (!eigrpIface->isEnabled())
                enableInterface(eigrpIface, ifAddress, ifMask, networkId);
        }
    }
    else if (!iface->isDown() || !iface->hasCarrier())
    { // an interface go dowdn
        eigrpIface = this->eigrpIft->findInterfaceById(ifaceId);

        if (eigrpIface != NULL && eigrpIface->isEnabled())
        {
            disableInterface(eigrpIface, ifAddress, ifMask);
        }
    }
}

void EigrpIpv4Pdm::processIfaceConfigChange(EigrpInterface *eigrpIface)
{
    EigrpRouteSource<IPv4Address> *source;
    int routeCount = eigrpTt->getNumRoutes();
    int ifaceId = eigrpIface->getInterfaceId();
    EigrpMetricPar ifParam = eigrpMetric->getParam(eigrpIface);
    EigrpMetricPar newParam;
    uint32_t metric;

    // Update routes through the interface
    for (int i = 0; i < routeCount; i++)
    {
        source = eigrpTt->getRoute(i);
        if (source->getIfaceId() == ifaceId)
        {
            // Update metric of source
            if (source->getNexthopId() == EigrpNeighbor<IPv4Address>::UNSPEC_ID)
            { // connected route
                source->setMetricParams(ifParam);
                metric = eigrpMetric->computeMetric(ifParam, this->kValues);
                source->setMetric(metric);
            }
            else
            {
                newParam = eigrpMetric->adjustParam(ifParam, source->getRdParams());
                source->setMetricParams(newParam);
                metric = eigrpMetric->computeMetric(newParam, this->kValues);
                source->setMetric(metric);
            }

            // Notify DUAL about event
            eigrpDual->processEvent(EigrpDual::RECV_UPDATE, source, source->getNexthopId());
        }
    }

    flushMsgRequests();
}

void EigrpIpv4Pdm::disableInterface(EigrpInterface *eigrpIface, IPv4Address& ifAddress, IPv4Address& ifMask)
{
    EigrpTimer* hellot;
    EigrpNeighbor<IPv4Address> *neigh;
    EigrpRouteSource<IPv4Address> *source;
    int neighCount;
    int ifaceId = eigrpIface->getInterfaceId();;

    EV << "EIGRP disabled on interface " << ifaceId << endl;

    // Move interface to EIGRP interface table
    eigrpIft->removeInterface(eigrpIface);
    eigrpIftDisabled->addInterface(eigrpIface);

    eigrpIface->setEnabling(false);

    // stop hello timer
    hellot = eigrpIface->getHelloTimer();
    cancelEvent(hellot);

    // First process route on the interface
    IPv4Address ifNetwork = ifAddress.doAnd(ifMask);
    source = eigrpTt->findRoute(ifNetwork, ifMask, EigrpNeighbor<IPv4Address>::UNSPEC_ID);
    // Notify DUAL about event
    eigrpDual->processEvent(EigrpDual::INTERFACE_DOWN, source, EigrpNeighbor<IPv4Address>::UNSPEC_ID);

    // Delete all neighbors on the interface
    neighCount = eigrpNt->getNumNeighbors();
    for (int i = 0; i < neighCount; i++)
    {
        neigh = eigrpNt->getNeighbor(i);
        if (neigh->getIfaceId() == ifaceId)
        {
            removeNeighbor(neigh, ifMask);
        }
    }

    flushMsgRequests();
}

void EigrpIpv4Pdm::enableInterface(EigrpInterface *eigrpIface, IPv4Address& ifAddress, IPv4Address& ifMask, int networkId)
{
    EigrpTimer* hellot;
    int ifaceId = eigrpIface->getInterfaceId();
    EigrpRouteSource<IPv4Address> *src;
    EigrpMetricPar metricPar;
    bool isSourceNew;

    EV << "EIGRP enabled on interface " << ifaceId << endl;

    // Move interface to EIGRP interface table
    eigrpIftDisabled->removeInterface(eigrpIface);
    eigrpIft->addInterface(eigrpIface);

    eigrpIface->setEnabling(true);
    eigrpIface->setNetworkId(networkId);

    // Start Hello timer on interface
    if ((hellot = eigrpIface->getHelloTimer()) == NULL)
    {
        hellot = createTimer(EIGRP_HELLO_TIMER, eigrpIface);
        eigrpIface->setHelloTimerPtr(hellot);
    }
    scheduleAt(simTime() + uniform(0, 5), hellot);

    // Create route
    src = eigrpTt->findOrCreateRoute(ifAddress, ifMask, ifaceId, EigrpNeighbor<IPv4Address>::UNSPEC_ID, &isSourceNew);

    // Compute metric
    metricPar = eigrpMetric->getParam(eigrpIface);
    src->setMetricParams(metricPar);
    uint32_t metric = eigrpMetric->computeMetric(metricPar, this->kValues);
    src->setMetric(metric);

    // Notify DUAL about event
    eigrpDual->processEvent(EigrpDual::INTERFACE_UP, src, EigrpNeighbor<IPv4Address>::UNSPEC_ID);
    flushMsgRequests();
}

EigrpInterface *EigrpIpv4Pdm::addInterfaceToEigrp(int ifaceId, int networkId, bool enabled)
{
    InterfaceEntry *iface = ift->getInterfaceById(ifaceId);
    IPv4Address ifAddress, ifMask;
    // create EIGRP interface and hello timer
    EigrpInterface *eigrpIface = new EigrpInterface(iface, networkId, enabled);

    if (enabled)
    {
        // Get address and mask of interface
        ifMask = iface->ipv4Data()->getNetmask();
        ifAddress = iface->ipv4Data()->getIPAddress().doAnd(ifMask);

        enableInterface(eigrpIface, ifAddress, ifMask, networkId);
    }
    else
    {
        eigrpIftDisabled->addInterface(eigrpIface);
    }

    return eigrpIface;
}

//
// interface IEigrpModule
//

EigrpNetwork *EigrpIpv4Pdm::addNetwork(IPv4Address address, IPv4Address mask)
{
    // TODO duplicity control
    return routingForNetworks->addNetwork(address, mask);
}

void EigrpIpv4Pdm::setHelloInt(int interval, int ifaceId)
{
    EigrpInterface *iface = getInterfaceById(ifaceId);
    if (iface == NULL)
        iface = addInterfaceToEigrp(ifaceId, EigrpNetworkTable::UNSPEC_NETID, false);
    iface->setHelloInt(interval);
}

void EigrpIpv4Pdm::setHoldInt(int interval, int ifaceId)
{
    EigrpInterface *iface = getInterfaceById(ifaceId);
    if (iface == NULL)
        iface = addInterfaceToEigrp(ifaceId, EigrpNetworkTable::UNSPEC_NETID, false);
    iface->setHoldInt(interval);
}

void EigrpIpv4Pdm::setSplitHorizon(bool shenabled, int ifaceId)
{
    EigrpInterface *iface = getInterfaceById(ifaceId);
    if (iface == NULL)
        iface = addInterfaceToEigrp(ifaceId, EigrpNetworkTable::UNSPEC_NETID, false);
    iface->setSplitHorizon(shenabled);
}

//
// interface IEigrpPdm
//

/**
 * @param destiation If dest is 0.0.0.0, then send message to all neighbors.
 */
void EigrpIpv4Pdm::sendUpdate(int destNeighbor, EigrpRoute<IPv4Address> *route, EigrpRouteSource<IPv4Address> *source)
{
    ev << "DUAL: send Update message about " << route->getRouteAddress() << " to all neighbors" << endl;
    msgToAllIfaces(EIGRP_UPDATE_MSG, source, false /* Only when sending Query message */);
    //}
}

/**
 * @param destNeighbor If parameter is 0 then sends message to all neighbors.
 * @param updateFromS If input event of DUAL is Update from successor.
 */
void EigrpIpv4Pdm::sendQuery(int destNeighbor, EigrpRoute<IPv4Address> *route, EigrpRouteSource<IPv4Address> *source, bool updateFromS)
{
    ev << "DUAL: send Query message about " << route->getRouteAddress() << " to all neighbors" << endl;
    msgToAllIfaces(EIGRP_QUERY_MSG, source, updateFromS);
}

void EigrpIpv4Pdm::sendReply(EigrpRoute<IPv4Address> *route, int destNeighbor, EigrpRouteSource<IPv4Address> *source, bool isUnreachable)
{
    EigrpMsgRoute msgRt;
    EigrpNeighbor<IPv4Address> *neigh = eigrpNt->findNeighborById(destNeighbor);

    ev << "DUAL: send Reply message about " << route->getRouteAddress() << endl;
    if (neigh == NULL)
        return;

    msgRt.invalid = false;
    msgRt.sourceId = source->getSourceId();
    msgRt.unreachable = isUnreachable;

    if (eigrpIft->findInterfaceById(neigh->getIfaceId())->isSplitHorizonEn())
    { // Poison Reverse is enabled when Split Horizon is enabled
        // Note: destNeighbor is also neighbor that sent Query
        if (destNeighbor == source->getNexthopId())
        {// Apply Poison Reverse
            msgRt.unreachable = true;
        }
    }

    pushMsgRouteToQueue(EIGRP_REPLY_MSG, neigh->getIfaceId(), neigh->getNeighborId(), msgRt);
}

/*bool EigrpIpv4Pdm::addRouteToRT(EigrpRouteSource<IPv4Address> *successor)
{
    ANSAIPv4Route *rtEntry = new ANSAIPv4Route();
    int ifaceId = successor->getIfaceId();
    EigrpRoute<IPv4Address> *route = successor->getRouteInfo();

    rtEntry->setDestination(route->getRouteAddress());
    rtEntry->setNetmask(route->getRouteMask());
    rtEntry->setInterface(this->ift->getInterfaceById(ifaceId));
    rtEntry->setGateway(successor->getNextHop());
    rtEntry->setMetric(successor->getMetric());

    if (successor->isInternal())
    {
        // Set any source except IFACENETMASK and MANUAL
        rtEntry->setSource(IPv4Route::ZEBRA);
        // Set right protocol source
        rtEntry->setRoutingProtocolSource(ANSAIPv4Route::pEIGRP);
        rtEntry->setAdminDist(ANSAIPv4Route::dEIGRPInternal);
    }

    if (rt->prepareForAddRoute(rtEntry))
    {
        EV << "Add EIGRP route " << route->getRouteAddress() << " to RT" << endl;
        this->eigrpRtRoutes.push_back(rtEntry);
        rt->addRoute(rtEntry);

        return true;
    }
    else
    {
        delete rtEntry;
        EV << "EIGRP route " << route->getRouteAddress() << " can not be added to RT" << endl;

        return false;
    }
}*/

/*bool EigrpIpv4Pdm::updateRouteInRT(EigrpRouteSource<IPv4Address> *source)
{
    RouteVector::iterator it;
    EigrpRoute<IPv4Address> *route = source->getRouteInfo();

    if ((it = findRouteInRT(source, route)) != eigrpRtRoutes.end())
    {
        EV << "DUAL: update route " << route->getRouteAddress() << " in RT" << endl;
        (*it)->setMetric(route->getDij());
        return true;
    }
    else
    {
#ifdef EIGRP_DEBUG
        // For example there is connected route in RT with lower AD
        EV << "DUAL: route "<< route->getRouteAddress() << " can not be updated, not found in RT" << endl;
#endif
        return false;
    }
}*/

void EigrpIpv4Pdm::removeRouteFromRT(EigrpRouteSource<IPv4Address> *source)
{
    EigrpRoute<IPv4Address> *route = source->getRouteInfo();
    RouteVector::iterator it;
    IPv4Route *rtEntry;

    if ((it = findRouteInRT(source, route)) != eigrpRtRoutes.end())
    {
        EV << "DUAL: delete route " << route->getRouteAddress() << " via " << source->getNextHop() << " from RT" << endl;
        rtEntry = *it;      // Must be here
        this->eigrpRtRoutes.erase(it);
        rt->deleteRoute(rtEntry);
    }
    else
    {
#ifdef EIGRP_DEBUG
        EV << "DUAL: route "<< route->getRouteAddress() << " can not be removed, not found in RT" << endl;
#endif
    }
}

void EigrpIpv4Pdm::removeSourceFromTT(EigrpRouteSource<IPv4Address> *source)
{
    EV << "EIGRP remove route " << source->getRouteInfo()->getRouteAddress();
    EV << " via " << source->getNextHop() << " from TT" << endl;

    eigrpTt->removeRoute(source);
    if (source->getRouteInfo()->getRefCnt() == 1)
        eigrpTt->removeRouteInfo(source->getRouteInfo());
    delete source;
}

EigrpRouteSource<IPv4Address> *EigrpIpv4Pdm::updateRoute(EigrpRoute<IPv4Address> *route, uint32_t dmin)
{
    int routeNum = eigrpTt->getNumRoutes();
    int routeId = route->getRouteId();
    int pathsInRT = 0;
    bool isRTSafe = true;
    bool checkRTSafety = true;      // Check if it is safe add EIGRP route to RT
    bool routeAdded;
    uint32_t routeFd = route->getFd();
    EigrpRouteSource<IPv4Address> *source = NULL;
    EigrpRouteSource<IPv4Address> *bestSuccessor = NULL;
    EigrpInterface *eigrpIface = NULL;

    if (dmin < routeFd)
        route->setFd(dmin);
#ifdef EIGRP_DEBUG
    ev << "EIGRP: Search successor for route " << route->getRouteAddress() << ", FD is " << route->getFd() << endl;
#endif
    for (int i = 0; i < routeNum; i++)
    {
        source = eigrpTt->getRoute(i);

        if (source->getRouteId() != routeId)
            continue;

        if (source->getRd() < routeFd && source->getMetric() <= dmin*this->variance && pathsInRT < this->maximumPath)
        {
#ifdef EIGRP_DEBUG
            ev << "     successor " << source->getNextHop() << " (" << source->getMetric() << "/" << source->getRd() << ")" << endl;
#endif
            pathsInRT++;

            // Actualize Dij and RDij of route
            if (source->getMetric() == dmin && bestSuccessor == NULL)
            {
                route->setDij(dmin);
                route->setRdPar(source->getMetricParams());
                route->setSuccessor(source);
                bestSuccessor = source;
            }

            if (source->getNexthopId() != EigrpNeighbor<IPv4Address>::UNSPEC_ID)    // Do not add connected route to RT
            {
                routeAdded = createOrUpdateRouteInRT(route, source, &checkRTSafety, &isRTSafe);
                if (routeAdded)
                { // Apply Poison Reverse
                    eigrpIface = eigrpIft->findInterfaceById(source->getIfaceId());

                    if (eigrpIface->isSplitHorizonEn())
                    { // Split Horizon and Poison Reverse is enabled
                        // Send update with infinite metric to the interface (Poison Reverse)
                        ev << "DUAL: send Update message about " << route->getRouteAddress() << " with unreachable metric to interface " << eigrpIface->getInterfaceId() << endl;
                        msgToIface(EIGRP_UPDATE_MSG, source, false, eigrpIface, true);

                        // Do not flush request in transmit queue
                    }
                }
            }

            source->setSuccessor(true);
        }
        else if (source->isSuccessor())
        { // Remove old successor from RT
            source->setSuccessor(false);
            if (route->getSuccessor() == source)
                route->setSuccessor(NULL);
            removeRouteFromRT(source);
        }
    }

    eigrpTt->purge(routeId);

    return bestSuccessor;
}

/**
 * @param updateFromS If input event of DUAL was Update from successor.
 */
int EigrpIpv4Pdm::setReplyStatusTable(EigrpRoute<IPv4Address> *route, EigrpRouteSource<IPv4Address> *source, bool updateFromS)
{
    int neighCount = eigrpNt->getNumNeighbors();
    EigrpNeighbor<IPv4Address> *neigh;
    EigrpInterface *eigrpIface = NULL;

    for (int i = 0; i < neighCount; i++)
    {
        neigh = eigrpNt->getNeighbor(i);
        eigrpIface = eigrpIft->findInterfaceById(neigh->getIfaceId());

        if (applySplitHorizon(eigrpIface, source))
        {
            if (updateFromS)
            { // Apply Poison Reverse instead of Split Horizon
                route->setReplyStatus(neigh->getNeighborId());
            }
        } else
            route->setReplyStatus(neigh->getNeighborId());
    }

    return route->getReplyStatusSum();
}

bool EigrpIpv4Pdm::hasNeighbor(EigrpRouteSource<IPv4Address> *source)
{
    int ifaceCnt = this->eigrpIft->getNumInterfaces();
    EigrpInterface *eigrpIface;

    for (int i = 0; i < ifaceCnt; i++)
    {
        eigrpIface = eigrpIft->getInterface(i);

        // Apply Split Horizon rule only for Update message
        if (!applySplitHorizon(eigrpIface, source) && eigrpIface->getNumOfNeighbors() > 0)
        { // There is at least one neighbor
            return true;
        }
    }

    return false;
}



