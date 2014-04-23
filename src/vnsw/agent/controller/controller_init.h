/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef __VNSW_CONTROLLER_INIT_HPP__
#define __VNSW_CONTROLLER_INIT_HPP__

#include <sandesh/sandesh_trace.h>
#include <discovery_client.h>
#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>

class AgentXmppChannel;
class AgentDnsXmppChannel;
class AgentIfMapVmExport;
class BgpPeer;

struct CleanupTimer {
    CleanupTimer(Agent *agent, const std::string &timer_name, 
                 uint32_t default_stale_timer_interval);
    virtual ~CleanupTimer() { }

    void Start(AgentXmppChannel *agent_xmpp_channel);
    bool Cancel();
    void RescheduleTimer(AgentXmppChannel *agent_xmpp_channel);
    bool TimerExpiredCallback();

    virtual void TimerExpirationDone() { }
    virtual uint32_t GetTimerInterval() const = 0;
    virtual uint64_t GetTimerExtensionValue(AgentXmppChannel *ch) = 0;
    virtual uint32_t stale_timer_interval() {
        return stale_timer_interval_;
    }
    virtual void set_stale_timer_interval(uint32_t stale_timer_interval) {
        stale_timer_interval_ = stale_timer_interval;
    }

    Agent *agent_;
    Timer *cleanup_timer_;
    uint64_t extension_interval_;
    uint64_t last_restart_time_;
    AgentXmppChannel *agent_xmpp_channel_;
    bool running_;
    uint32_t stale_timer_interval_;
};

struct UnicastCleanupTimer : public CleanupTimer {
    static const uint32_t kUnicastStaleTimer = (2 * 60 * 1000); 
    UnicastCleanupTimer(Agent *agent)
        : CleanupTimer(agent, "Agent Unicast Stale cleanup timer", 
                       kUnicastStaleTimer) { };
    virtual ~UnicastCleanupTimer() { }

    virtual uint32_t GetTimerInterval() const {
        return kUnicastStaleTimer;}
    virtual void TimerExpirationDone();
    virtual uint64_t GetTimerExtensionValue(AgentXmppChannel *ch);
};

struct MulticastCleanupTimer : public CleanupTimer {
    static const uint32_t kMulticastStaleTimer = (5 * 60 * 1000); 
    MulticastCleanupTimer(Agent *agent) 
        : CleanupTimer(agent, "Agent Multicast Stale cleanup timer",
                       kMulticastStaleTimer) { }
    virtual ~MulticastCleanupTimer() { }

    virtual uint32_t GetTimerInterval() const {return kMulticastStaleTimer;}
    virtual void TimerExpirationDone();
    virtual uint64_t GetTimerExtensionValue(AgentXmppChannel *ch);

    uint32_t peer_sequence_;
};

struct ConfigCleanupTimer : public CleanupTimer {
    static const int timeout_ = (15 * 60 * 1000); // In milli seconds
    ConfigCleanupTimer(Agent *agent)
        : CleanupTimer(agent, "Agent Stale cleanup timer",
                       timeout_) { }
    virtual ~ConfigCleanupTimer() { }

    virtual uint32_t GetTimerInterval() const {return timeout_;}
    virtual void TimerExpirationDone();
    virtual uint64_t GetTimerExtensionValue(AgentXmppChannel *ch);
};

class VNController {
public:
    typedef boost::shared_ptr<BgpPeer> BgpPeerPtr; 
    static const uint64_t kInvalidPeerIdentifier = 0xFFFFFFFFFFFFFFFF;
    VNController(Agent *agent);
    ~VNController();
    void Connect();
    void DisConnect();

    void Cleanup();

    void XmppServerConnect();
    void DnsXmppServerConnect();

    void XmppServerDisConnect();
    void DnsXmppServerDisConnect();

    void ApplyDiscoveryXmppServices(std::vector<DSResponse> resp); 
    void ApplyDiscoveryDnsXmppServices(std::vector<DSResponse> resp); 

    //Multicast peer identifier
    void increment_multicast_peer_identifier() {multicast_peer_identifier_++;}
    uint64_t multicast_peer_identifier() {return multicast_peer_identifier_;}

    //Peer maintenace routines 
    uint8_t ActiveXmppConnectionCount();
    AgentXmppChannel *GetActiveXmppChannel();
    uint32_t DecommissionedPeerListSize() const {
        return decommissioned_peer_list_.size();
    }
    void AddToDecommissionedPeerList(boost::shared_ptr<BgpPeer> peer);

    //Unicast timer related routines
    void StartUnicastCleanupTimer(AgentXmppChannel *agent_xmpp_channel);
    bool UnicastCleanupTimerExpired();
    CleanupTimer &unicast_cleanup_timer(){return unicast_cleanup_timer_;}
    void ControllerPeerHeadlessAgentDelDone(BgpPeer *peer);

    //Multicast timer
    void StartMulticastCleanupTimer(AgentXmppChannel *agent_xmpp_channel);
    bool MulticastCleanupTimerExpired(uint64_t peer_sequence);
    CleanupTimer &multicast_cleanup_timer() {return multicast_cleanup_timer_;}

    AgentIfMapVmExport *agent_ifmap_vm_export() const {
        return agent_ifmap_vm_export_.get();}
    void StartConfigCleanupTimer(AgentXmppChannel *agent_xmpp_channel);
    CleanupTimer &config_cleanup_timer() {return config_cleanup_timer_;}

    // Clear of decommissioned peer listener id for vrf specified
    void DeleteVrfStateOfDecommisionedPeers(DBTablePartBase *partition, 
                                            DBEntryBase *e);
    Agent *agent() {return agent_;}

private:
    AgentXmppChannel *FindAgentXmppChannel(const std::string &server_ip);
    AgentDnsXmppChannel *FindAgentDnsXmppChannel(const std::string &server_ip);

    Agent *agent_;
    uint64_t multicast_peer_identifier_;
    std::list<boost::shared_ptr<BgpPeer> > decommissioned_peer_list_;
    boost::scoped_ptr<AgentIfMapVmExport> agent_ifmap_vm_export_;
    UnicastCleanupTimer unicast_cleanup_timer_;
    MulticastCleanupTimer multicast_cleanup_timer_;
    ConfigCleanupTimer config_cleanup_timer_;
};

extern SandeshTraceBufferPtr ControllerTraceBuf;

#define CONTROLLER_TRACE(obj, ...)\
do {\
    AgentXmpp##obj::TraceMsg(ControllerTraceBuf, __FILE__, __LINE__, __VA_ARGS__);\
} while(0);\

#endif
