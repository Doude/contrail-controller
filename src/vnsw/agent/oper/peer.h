/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef vnsw_agent_peer_h_
#define vnsw_agent_peer_h_

#include <string>
#include <map>
#include <tbb/mutex.h>
#include <db/db_table_walker.h>
#include <net/address.h>

#define LOCAL_PEER_NAME "Local"
#define LOCAL_VM_PEER_NAME "Local_Vm"
#define LOCAL_VM_PORT_PEER_NAME "Local_Vm_Port"
#define NOVA_PEER_NAME "Nova"
#define LINKLOCAL_PEER_NAME "LinkLocal"

class AgentXmppChannel;
class ControllerRouteWalker;
class VrfTable;

class Peer {
public:
    typedef boost::function<void()> DelPeerDone;
    typedef std::map<std::string, Peer *> PeerMap;
    typedef std::pair<std::string, Peer *> PeerPair;
    enum Type {
        ECMP_PEER,
        BGP_PEER,
        LOCAL_PEER,  // higher priority for local peer
        LOCAL_VM_PEER,
        LOCAL_VM_PORT_PEER,
        LINKLOCAL_PEER,
        NOVA_PEER
    };

    Peer(Agent *agent, Type type, const std::string &name);
    virtual ~Peer();

    // Table Walkers
    void DelPeerRoutes(DelPeerDone cb);
    void PeerNotifyRoutes();
    void PeerNotifyMulticastRoutes(bool associate);
    void StalePeerRoutes();

    bool IsLess(const Peer *rhs) const {
        if  (type_ != rhs->type_) {
            return type_ < rhs->type_;
        }

        return Compare(rhs);
    }
    virtual bool Compare(const Peer *rhs) const {return false;}

    const std::string &GetName() const { return name_; }
    const Type GetType() const { return type_; }
    ControllerRouteWalker *route_walker() const {
        return route_walker_.get(); }
    bool is_disconnect_walk() const {return is_disconnect_walk_;}
    void set_is_disconnect_walk(bool is_disconnect_walk) {
        is_disconnect_walk_ = is_disconnect_walk;
    }
    Agent *agent() const {return agent_;}

private:
    Type type_;
    std::string name_;
    Agent *agent_;
    boost::scoped_ptr<ControllerRouteWalker> route_walker_;
    tbb::atomic<bool> is_disconnect_walk_;
    DISALLOW_COPY_AND_ASSIGN(Peer);
};

// Peer used for BGP paths
class BgpPeer : public Peer {
public:
    BgpPeer(Agent *agent, const Ip4Address &server_ip, const std::string &name,
            AgentXmppChannel *bgp_xmpp_peer, DBTableBase::ListenerId id) : 
        Peer(agent, Peer::BGP_PEER, name), server_ip_(server_ip), id_(id),
        bgp_xmpp_peer_(bgp_xmpp_peer) {
    }
    virtual ~BgpPeer();

    bool Compare(const Peer *rhs) const {
        const BgpPeer *bgp = static_cast<const BgpPeer *>(rhs);
        return server_ip_ < bgp->server_ip_;
    }

    void SetVrfListenerId(DBTableBase::ListenerId id) { id_ = id; }
    DBTableBase::ListenerId GetVrfExportListenerId() { return id_; } 
    AgentXmppChannel *GetBgpXmppPeer() { return bgp_xmpp_peer_; }    
private: 
    Ip4Address server_ip_;
    DBTableBase::ListenerId id_;
    AgentXmppChannel *bgp_xmpp_peer_;
    DISALLOW_COPY_AND_ASSIGN(BgpPeer);
};

// Peer for local-vm-port paths. There can be multiple VMs with same IP.
// They are all added as different path. ECMP path will consolidate all 
// local-vm-port paths
class LocalVmPortPeer : public Peer {
public:
    LocalVmPortPeer(const std::string &name, uint64_t handle) :
        Peer(Agent::GetInstance(), Peer::LOCAL_VM_PORT_PEER, name), handle_(handle) {
    }

    virtual ~LocalVmPortPeer() { }

    bool Compare(const Peer *rhs) const {
        const LocalVmPortPeer *local = 
            static_cast<const LocalVmPortPeer *>(rhs);
        return handle_ < local->handle_;
    }

private:
    uint64_t handle_;
    DISALLOW_COPY_AND_ASSIGN(LocalVmPortPeer);
};

// ECMP peer
class EcmpPeer : public Peer {
public:
    EcmpPeer() : Peer(Agent::GetInstance(), Peer::ECMP_PEER, "ECMP") { }
    virtual ~EcmpPeer() { }

    bool Compare(const Peer *rhs) const { return false; }
private:
    DISALLOW_COPY_AND_ASSIGN(EcmpPeer);
};
#endif // vnsw_agent_peer_h_
