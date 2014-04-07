/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <base/util.h>
#include <base/logging.h>
#include <net/bgp_af.h>
#include <sandesh/sandesh.h>
#include <sandesh/sandesh_types.h>
#include "cmn/agent_cmn.h"
#include "cmn/agent_stats.h"
#include "controller/controller_peer.h"
#include "controller/controller_ifmap.h"
#include "controller/controller_vrf_export.h"
#include "controller/controller_init.h"
#include "oper/vrf.h"
#include "oper/nexthop.h"
#include "oper/mirror_table.h"
#include "oper/multicast.h"
#include "oper/peer.h"
#include "oper/vxlan.h"
#include <pugixml/pugixml.hpp>
#include "xml/xml_pugi.h"
#include "xmpp/xmpp_init.h"
#include "xmpp_multicast_types.h"
#include "ifmap/ifmap_agent_table.h"
#include "controller/controller_types.h"
#include "net/tunnel_encap_type.h"
#include <assert.h>

using namespace boost::asio;
using namespace autogen;
 
AgentXmppChannel::AgentXmppChannel(Agent *agent, XmppChannel *channel, 
                                   std::string xmpp_server, 
                                   std::string label_range, uint8_t xs_idx) 
    : channel_(channel), xmpp_server_(xmpp_server), label_range_(label_range),
      xs_idx_(xs_idx), agent_(agent) {
    bgp_peer_id_.reset();
    channel_->RegisterReceive(xmps::BGP, 
                              boost::bind(&AgentXmppChannel::ReceiveInternal, 
                                          this, _1));
}

AgentXmppChannel::~AgentXmppChannel() {
    channel_->UnRegisterReceive(xmps::BGP);
}

std::string AgentXmppChannel::GetBgpPeerName() const {
    if (bgp_peer_id_.get() == NULL)
        return "No BGP peer";

    return bgp_peer_id_.get()->GetName();
}

void AgentXmppChannel::CreateBgpPeer() {
    //Ensure older bgp_peer_id in decommisioned list
    DBTableBase::ListenerId id = 
        agent_->GetVrfTable()->Register(boost::bind(&VrfExport::Notify,
                                       this, _1, _2)); 
    boost::system::error_code ec;
    const string &addr = agent_->GetXmppServer(xs_idx_);
    Ip4Address ip = Ip4Address::from_string(addr.c_str(), ec);
    assert(ec.value() == 0);
    bgp_peer_id_.reset(new BgpPeer(ip, addr, this, id));
}

void AgentXmppChannel::DeCommissionBgpPeer() {
    //Unregister is in  destructor of peer. Unregister shud happen
    //after dbstate for the id has happened w.r.t. this peer. If unregiter is 
    //done here, then there is a chance that it is reused and before state
    //is removed it is overwritten. Also it may happen that state delete may be
    //of somebody else.

    // Add the peer to global decommisioned list
    agent_->controller()->AddToControllerPeerList(bgp_peer_id_);
    //Reset channel BGP peer id
    bgp_peer_id_.reset();
}


bool AgentXmppChannel::SendUpdate(uint8_t *msg, size_t size) {

    if ((channel_ && (bgp_peer_id() != NULL) && 
        (channel_->GetPeerState() == xmps::READY))) {
        AgentStats::GetInstance()->incr_xmpp_out_msgs(xs_idx_);
	    return channel_->Send(msg, size, xmps::BGP,
			  boost::bind(&AgentXmppChannel::WriteReadyCb, this, _1));
    } else {
        return false; 
    }
}

void AgentXmppChannel::ReceiveEvpnUpdate(XmlPugi *pugi) {
    pugi::xml_node node = pugi->FindNode("items");
    pugi::xml_attribute attr = node.attribute("node");

    char *saveptr;
    strtok_r(const_cast<char *>(attr.value()), "/", &saveptr);
    strtok_r(NULL, "/", &saveptr);
    char *vrf_name =  strtok_r(NULL, "", &saveptr);
    const std::string vrf(vrf_name);
    Layer2AgentRouteTable *rt_table = 
        static_cast<Layer2AgentRouteTable *>
        (agent_->GetVrfTable()->GetLayer2RouteTable(vrf_name));

    pugi::xml_node node_check = pugi->FindNode("retract");
    if (!pugi->IsNull(node_check)) {
        for (node = node.first_child(); node; node = node.next_sibling()) {
            if (strcmp(node.name(), "retract") == 0)  {
                std::string id = node.first_attribute().value();
                CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                                 "EVPN Delete Node id:" + id);

                char *mac_str = 
                    strtok_r(const_cast<char *>(id.c_str()), "-", &saveptr);
                //char *mac_str = strtok_r(NULL, ",", &saveptr);
                struct ether_addr mac = *ether_aton(mac_str);;
                rt_table->DeleteReq(bgp_peer_id(), vrf_name, mac);
            }
        }
        return;
    }

    //Call Auto-generated Code to return struct
    auto_ptr<AutogenProperty> xparser(new AutogenProperty());
    if (EnetItemsType::XmlParseProperty(node, &xparser) == false) {
        CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                         "Xml Parsing for evpn Failed");
        return;
    }

    EnetItemsType *items;
    EnetItemType *item;

    items = (static_cast<EnetItemsType *>(xparser.get()));
    std::vector<EnetItemType>::iterator iter;
    for (vector<EnetItemType>::iterator iter =items->item.begin();
         iter != items->item.end(); iter++) {
        item = &*iter;
        if (item->entry.nlri.mac != "") {
            struct ether_addr mac = *ether_aton((item->entry.nlri.mac).c_str());
            AddEvpnRoute(vrf_name, mac, item);
        } else {
            CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                        "NLRI missing mac address for evpn, failed parsing");
        }
    }
}

static TunnelType::TypeBmap 
GetEnetTypeBitmap(const EnetTunnelEncapsulationListType &encap) {
    TunnelType::TypeBmap bmap = 0;
    for (EnetTunnelEncapsulationListType::const_iterator iter = encap.begin();
         iter != encap.end(); iter++) {
        TunnelEncapType::Encap encap = 
            TunnelEncapType::TunnelEncapFromString(*iter);
        if (encap == TunnelEncapType::MPLS_O_GRE)
            bmap |= (1 << TunnelType::MPLS_GRE);
        if (encap == TunnelEncapType::MPLS_O_UDP)
            bmap |= (1 << TunnelType::MPLS_UDP);
        if (encap == TunnelEncapType::VXLAN)
            bmap |= (1 << TunnelType::VXLAN);
    }
    return bmap;
}

static TunnelType::TypeBmap 
GetTypeBitmap(const TunnelEncapsulationListType &encap) {
    TunnelType::TypeBmap bmap = 0;
    for (TunnelEncapsulationListType::const_iterator iter = encap.begin();
         iter != encap.end(); iter++) {
        TunnelEncapType::Encap encap = 
            TunnelEncapType::TunnelEncapFromString(*iter);
        if (encap == TunnelEncapType::MPLS_O_GRE)
            bmap |= (1 << TunnelType::MPLS_GRE);
        if (encap == TunnelEncapType::MPLS_O_UDP)
            bmap |= (1 << TunnelType::MPLS_UDP);
    }
    return bmap;
}
static TunnelType::TypeBmap 
GetMcastTypeBitmap(const McastTunnelEncapsulationListType &encap) {
    TunnelType::TypeBmap bmap = 0;
    for (McastTunnelEncapsulationListType::const_iterator iter = encap.begin();
         iter != encap.end(); iter++) {
        TunnelEncapType::Encap encap = 
            TunnelEncapType::TunnelEncapFromString(*iter);
        if (encap == TunnelEncapType::MPLS_O_GRE)
            bmap |= (1 << TunnelType::MPLS_GRE);
        if (encap == TunnelEncapType::MPLS_O_UDP)
            bmap |= (1 << TunnelType::MPLS_UDP);
    }
    return bmap;
}

void AgentXmppChannel::ReceiveMulticastUpdate(XmlPugi *pugi) {

    pugi::xml_node node = pugi->FindNode("items");
    pugi::xml_attribute attr = node.attribute("node");

    char *saveptr;
    strtok_r(const_cast<char *>(attr.value()), "/", &saveptr);
    strtok_r(NULL, "/", &saveptr);
    char *vrf_name =  strtok_r(NULL, "", &saveptr);
    const std::string vrf(vrf_name);
    TunnelOlist olist;

    pugi::xml_node node_check = pugi->FindNode("retract");
    if (!pugi->IsNull(node_check)) {
        pugi->ReadNode("retract"); //sets the context
        std::string retract_id = pugi->ReadAttrib("id");
        if (bgp_peer_id() != agent_->GetControlNodeMulticastBuilder()->
                             bgp_peer_id()) {
            CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                       "Ignore retract request from non multicast tree "
                       "builder peer; Multicast Delete Node id:" + retract_id);
            return;
        }

        for (node = node.first_child(); node; node = node.next_sibling()) {
            if (strcmp(node.name(), "retract") == 0) { 
                std::string id = node.first_attribute().value();
                CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                                "Multicast Delete Node id:" + id);

                // Parse identifier to obtain group,source
                // <addr:VRF:Group,Source) 
                strtok_r(const_cast<char *>(id.c_str()), ":", &saveptr);
                strtok_r(NULL, ":", &saveptr);
                char *group = strtok_r(NULL, ",", &saveptr);
                char *source = strtok_r(NULL, "", &saveptr);
                if (group == NULL || source == NULL) {
                    CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name, 
                       "Error parsing multicast group address from retract id");
                    return;
                }

                boost::system::error_code ec;
                IpAddress g_addr =
                    IpAddress::from_string(group, ec);
                if (ec.value() != 0) {
                    CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name, 
                            "Error parsing multicast group address");
                    return;
                }

                IpAddress s_addr =
                    IpAddress::from_string(source, ec);
                if (ec.value() != 0) {
                    CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name, 
                            "Error parsing multicast source address");
                    return;
                }

                //Retract with invalid identifier
                MulticastHandler::ModifyFabricMembers(vrf, g_addr.to_v4(),
                        s_addr.to_v4(), 0, olist, 
                        VNController::kInvalidPeerIdentifier);
            }
        }
        return;
    }

    pugi::xml_node items_node = pugi->FindNode("item");
    if (!pugi->IsNull(items_node)) {
        pugi->ReadNode("item"); //sets the context
        std::string item_id = pugi->ReadAttrib("id");
        if (!(agent_->GetControlNodeMulticastBuilder()) || (bgp_peer_id() !=
            agent_->GetControlNodeMulticastBuilder()->bgp_peer_id())) {
            CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                             "Ignore request from non multicast tree "
                             "builder peer; Multicast Delete Node:" + item_id);
            return;
        }
    }

    //Call Auto-generated Code to return struct
    auto_ptr<AutogenProperty> xparser(new AutogenProperty());
    if (McastItemsType::XmlParseProperty(node, &xparser) == false) {
        CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name, 
                        "Xml Parsing for Multicast Message Failed");
        return;
    }

    McastItemsType *items;
    McastItemType *item;

    items = (static_cast<McastItemsType *>(xparser.get()));
    std::vector<McastItemType>::iterator items_iter;
    boost::system::error_code ec;
    for (items_iter = items->item.begin(); items_iter != items->item.end();  
            items_iter++) {

        item = &*items_iter;

        IpAddress g_addr =
            IpAddress::from_string(item->entry.nlri.group, ec);
        if (ec.value() != 0) {
            CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                             "Error parsing multicast group address");
            return;
        }

        IpAddress s_addr =
            IpAddress::from_string(item->entry.nlri.source, ec);
        if (ec.value() != 0) {
            CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                            "Error parsing multicast source address");
            return;
        }

        std::vector<McastNextHopType>::iterator iter;
        for (iter = item->entry.olist.next_hop.begin();
                iter != item->entry.olist.next_hop.end(); iter++) {

            McastNextHopType nh = *iter;
            IpAddress addr = IpAddress::from_string(nh.address, ec);
            if (ec.value() != 0) {
                CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                                 "Error parsing next-hop address");
                return;
            }

            int label;
            stringstream nh_label(nh.label);
            nh_label >> label;
            TunnelType::TypeBmap encap = 
                GetMcastTypeBitmap(nh.tunnel_encapsulation_list);
            olist.push_back(OlistTunnelEntry(label, addr.to_v4(), encap)); 
        }

        MulticastHandler::ModifyFabricMembers(vrf, g_addr.to_v4(),
                s_addr.to_v4(), item->entry.nlri.source_label,
                olist, agent_->controller()->multicast_peer_identifier());
    }
}

void AgentXmppChannel::AddEcmpRoute(string vrf_name, Ip4Address prefix_addr, 
                                    uint32_t prefix_len, ItemType *item) {
    Inet4UnicastAgentRouteTable *rt_table = 
        static_cast<Inet4UnicastAgentRouteTable *>
        (agent_->GetVrfTable()->GetInet4UnicastRouteTable
         (vrf_name));

    std::vector<ComponentNHData> comp_nh_list;
    for (uint32_t i = 0; i < item->entry.next_hops.next_hop.size(); i++) {
        std::string nexthop_addr = 
            item->entry.next_hops.next_hop[i].address;
        boost::system::error_code ec;
        IpAddress addr = IpAddress::from_string(nexthop_addr, ec);
        if (ec.value() != 0) {
            CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                             "Error parsing nexthop ip address");
            continue;
        }

        uint32_t label = item->entry.next_hops.next_hop[i].label;
        if (agent_->GetRouterId() == addr.to_v4()) {
            //Get local list of interface and append to the list
            MplsLabel *mpls = 
                agent_->GetMplsTable()->FindMplsLabel(label);
            if (mpls != NULL) {
                DBEntryBase::KeyPtr key = mpls->nexthop()->GetDBRequestKey();
                NextHopKey *nh_key = static_cast<NextHopKey *>(key.get());
                nh_key->SetPolicy(false);
                ComponentNHData nh_data(label, nh_key);
                comp_nh_list.push_back(nh_data);
            }
        } else {
            TunnelType::TypeBmap encap = GetTypeBitmap
                (item->entry.next_hops.next_hop[i].tunnel_encapsulation_list);
            ComponentNHData nh_data(label, agent_->GetDefaultVrf(),
                                    agent_->GetRouterId(), 
                                    addr.to_v4(), false, encap);
            comp_nh_list.push_back(nh_data);
        }
    }
    //ECMP create component NH
    rt_table->AddRemoteVmRouteReq(bgp_peer_id(), vrf_name,
                                  prefix_addr, prefix_len, comp_nh_list, -1,
                                  item->entry.virtual_network, 
                                  item->entry.security_group_list.security_group);
}

void AgentXmppChannel::AddRemoteEvpnRoute(string vrf_name, 
                                      struct ether_addr &mac, 
                                      EnetItemType *item) {
    boost::system::error_code ec; 
    string nexthop_addr = item->entry.next_hops.next_hop[0].address;
    uint32_t label = item->entry.next_hops.next_hop[0].label;
    TunnelType::TypeBmap encap = GetEnetTypeBitmap
        (item->entry.next_hops.next_hop[0].tunnel_encapsulation_list);
    IpAddress addr = IpAddress::from_string(nexthop_addr, ec);
    Layer2AgentRouteTable *rt_table = 
        static_cast<Layer2AgentRouteTable *>
        (agent_->GetVrfTable()->GetLayer2RouteTable(vrf_name));

    if (ec.value() != 0) {
        CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                         "Error parsing nexthop ip address");
        return;
    }

    stringstream str;
    str << (ether_ntoa ((struct ether_addr *)&mac)); 
    CONTROLLER_TRACE(RouteImport, GetBgpPeerName(), vrf_name, 
                     str.str(), 0, nexthop_addr, label, "");

    Ip4Address prefix_addr;
    int prefix_len;
    ec = Ip4PrefixParse(item->entry.nlri.address, &prefix_addr,
                        &prefix_len);
    if (ec.value() != 0) {
        CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                         "Error parsing route address");
        return;
    }
    if (agent_->GetRouterId() != addr.to_v4()) {
        CONTROLLER_TRACE(Trace, GetBgpPeerName(), nexthop_addr,
                         "add remote evpn route");
        rt_table->AddRemoteVmRouteReq(bgp_peer_id(), vrf_name, encap,
                                      addr.to_v4(), label, mac,
                                      prefix_addr, prefix_len);
        return;
    }

    const NextHop *nh = NULL;
    if (encap == (1 << TunnelType::VXLAN)) {
        VrfEntry *vrf = 
            agent_->GetVrfTable()->FindVrfFromName(vrf_name);
        Layer2RouteKey key(agent_->local_vm_peer(), 
                           vrf_name, mac);
        if (vrf != NULL) {
            Layer2RouteEntry *route = 
                static_cast<Layer2RouteEntry *>
                (static_cast<Layer2AgentRouteTable *>
                 (vrf->GetLayer2RouteTable())->FindActiveEntry(&key));
            if (route) {
                nh = route->GetActiveNextHop();
            } else {
                CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                                 "route not found, ignoring request");
            }
        } else {
                CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                                 "vrf not found, ignoring request");
        }
    } else {
        MplsLabel *mpls = 
            agent_->GetMplsTable()->FindMplsLabel(label);
        if (mpls != NULL) {
            nh = mpls->nexthop();
        }
    }
    if (nh != NULL) {
        switch(nh->GetType()) {
        case NextHop::INTERFACE: {
            const InterfaceNH *intf_nh = static_cast<const InterfaceNH *>(nh);
            if (encap == TunnelType::VxlanType()) {
                rt_table->AddLocalVmRouteReq(bgp_peer_id(), intf_nh->GetIfUuid(),
                                             "", vrf_name,
                                             MplsTable::kInvalidLabel,
                                             label, mac, prefix_addr,
                                             prefix_len);
            } else {
                rt_table->AddLocalVmRouteReq(bgp_peer_id(), intf_nh->GetIfUuid(),
                                             "", vrf_name, label,
                                             VxLanTable::kInvalidvxlan_id,
                                             mac, prefix_addr, prefix_len);
            }
            break;
            }
        default:
            CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                             "label points to invalid NH");
        }
    } else {
        CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                         "nexthop not found, ignoring request");
    }
}

void AgentXmppChannel::AddRemoteRoute(string vrf_name, Ip4Address prefix_addr, 
                                      uint32_t prefix_len, ItemType *item) {
    Inet4UnicastAgentRouteTable *rt_table = 
        static_cast<Inet4UnicastAgentRouteTable *>
        (agent_->GetVrfTable()->GetInet4UnicastRouteTable
         (vrf_name));

    boost::system::error_code ec; 
    string nexthop_addr = item->entry.next_hops.next_hop[0].address;
    uint32_t label = item->entry.next_hops.next_hop[0].label;
    IpAddress addr = IpAddress::from_string(nexthop_addr, ec);
    TunnelType::TypeBmap encap = GetTypeBitmap
        (item->entry.next_hops.next_hop[0].tunnel_encapsulation_list);

    if (ec.value() != 0) {
        CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                         "Error parsing nexthop ip address");
        return;
    }

    CONTROLLER_TRACE(RouteImport, GetBgpPeerName(), vrf_name, 
                     prefix_addr.to_string(), prefix_len, 
                     addr.to_v4().to_string(), label, 
                     item->entry.virtual_network);

    if (agent_->GetRouterId() != addr.to_v4()) {
        rt_table->AddRemoteVmRouteReq(bgp_peer_id(), vrf_name,
                                      prefix_addr, prefix_len, addr.to_v4(),
                                      encap, label, item->entry.virtual_network,
                                      item->entry.security_group_list.security_group);
        return;
    }

    MplsLabel *mpls = agent_->GetMplsTable()->FindMplsLabel(label);
    if (mpls != NULL) {
        const NextHop *nh = mpls->nexthop();
        switch(nh->GetType()) {
        case NextHop::INTERFACE: {
            const InterfaceNH *intf_nh = static_cast<const InterfaceNH *>(nh);
            const Interface *interface = intf_nh->GetInterface();
            if (interface == NULL) {
                break;
            }

            if (interface->type() == Interface::VM_INTERFACE) {
                rt_table->AddLocalVmRouteReq(bgp_peer_id(), vrf_name, prefix_addr,
                                             prefix_len, intf_nh->GetIfUuid(),
                                             item->entry.virtual_network, label,
                                             item->entry.security_group_list.security_group,
                                             false);
            } else if (interface->type() == Interface::INET) {
                rt_table->AddInetInterfaceRoute(bgp_peer_id(), vrf_name,
                                                 prefix_addr, prefix_len,
                                                 interface->name(),
                                                 label,
                                                 item->entry.virtual_network);
            } else {
                // Unsupported scenario
                CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                                 "MPLS label points to invalid interface type");
                 break;
            }

            break;
            }

        case NextHop::VLAN: {
            const VlanNH *vlan_nh = static_cast<const VlanNH *>(nh);
            rt_table->AddVlanNHRouteReq(bgp_peer_id(), vrf_name, prefix_addr,
                                        prefix_len, vlan_nh->GetIfUuid(),
                                        vlan_nh->GetVlanTag(), label,
                                        item->entry.virtual_network,
                                        item->entry.security_group_list.security_group);
            break;
            }
        case NextHop::COMPOSITE: {
            AddEcmpRoute(vrf_name, prefix_addr, prefix_len, item);
            break;
            }

        default:
            CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                             "MPLS label points to invalid NH");
        }
    }
}

void AgentXmppChannel::AddEvpnRoute(string vrf_name, 
                                   struct ether_addr &mac, 
                                   EnetItemType *item) {
    if (item->entry.next_hops.next_hop.size() > 1) {
        CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                         "Multiple NH in evpn not supported");
    } else {
        AddRemoteEvpnRoute(vrf_name, mac, item);
    }
}

void AgentXmppChannel::AddRoute(string vrf_name, Ip4Address prefix_addr, 
                                uint32_t prefix_len, ItemType *item) {
    if (item->entry.next_hops.next_hop.size() > 1) {
        AddEcmpRoute(vrf_name, prefix_addr, prefix_len, item);
    } else {
        AddRemoteRoute(vrf_name, prefix_addr, prefix_len, item);
    }
}

void AgentXmppChannel::ReceiveUpdate(const XmppStanza::XmppMessage *msg) {
    
    AgentStats::GetInstance()->incr_xmpp_in_msgs(xs_idx_);
    if (msg && msg->type == XmppStanza::MESSAGE_STANZA) {
      
        XmlBase *impl = msg->dom.get();
        XmlPugi *pugi = reinterpret_cast<XmlPugi *>(impl);        
        pugi::xml_node node = pugi->FindNode("items");
        pugi->ReadNode("items"); //sets the context
        std::string nodename = pugi->ReadAttrib("node");

        const char *af = NULL, *safi = NULL, *vrf_name;
        char *str = const_cast<char *>(nodename.c_str());
        char *saveptr;
        af = strtok_r(str, "/", &saveptr);
        safi = strtok_r(NULL, "/", &saveptr);
        vrf_name = saveptr;

        if (atoi(af) == BgpAf::IPv4 && atoi(safi) == BgpAf::Mcast) {
            ReceiveMulticastUpdate(pugi);
            return;
        }
        if (atoi(af) == BgpAf::L2Vpn && atoi(safi) == BgpAf::Enet) {
            ReceiveEvpnUpdate(pugi);
            return;
        }

        VrfKey vrf_key(vrf_name);
        VrfEntry *vrf = 
            static_cast<VrfEntry *>(agent_->GetVrfTable()->
                                    FindActiveEntry(&vrf_key));
        if (!vrf) {
            CONTROLLER_TRACE (Trace, GetBgpPeerName(), vrf_name,
                    "VRF not found");
            return;
        }

        Inet4UnicastAgentRouteTable *rt_table = 
            static_cast<Inet4UnicastAgentRouteTable *>
            (vrf->GetInet4UnicastRouteTable());
        if (!rt_table) {
            CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name, 
                             "VRF not found");
            return;
        }

        if (!pugi->IsNull(node)) {
  
            pugi::xml_node node_check = pugi->FindNode("retract");
            if (!pugi->IsNull(node_check)) {
                for (node = node.first_child(); node; node = node.next_sibling()) {
                    if (strcmp(node.name(), "retract") == 0)  {
                        std::string id = node.first_attribute().value();
                        CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                                        "Delete Node id:" + id);
                        boost::system::error_code ec;
                        Ip4Address prefix_addr;
                        int prefix_len;
                        ec = Ip4PrefixParse(id, &prefix_addr, &prefix_len);
                        if (ec.value() != 0) {
                            CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                                    "Error parsing prefix for delete");
                            return;
                        }
                        rt_table->DeleteReq(bgp_peer_id(), vrf_name,
                                prefix_addr, prefix_len);
                    }
                }
                return;
            }
           
            //Call Auto-generated Code to return struct
            auto_ptr<AutogenProperty> xparser(new AutogenProperty());
            if (ItemsType::XmlParseProperty(node, &xparser) == false) {
                CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                                 "Xml Parsing Failed");
                return;
            }
            ItemsType *items;
            ItemType *item;

            items = (static_cast<ItemsType *>(xparser.get()));
            for (vector<ItemType>::iterator iter =items->item.begin();
                                            iter != items->item.end();
                                            ++iter) {
                item = &*iter;
                boost::system::error_code ec;
                Ip4Address prefix_addr;
                int prefix_len;
                ec = Ip4PrefixParse(item->entry.nlri.address, &prefix_addr,
                                    &prefix_len);
                if (ec.value() != 0) {
                    CONTROLLER_TRACE(Trace, GetBgpPeerName(), vrf_name,
                            "Error parsing route address");
                    return;
                }
                AddRoute(vrf_name, prefix_addr, prefix_len, item);
            }
        }
    }
}

void AgentXmppChannel::ReceiveInternal(const XmppStanza::XmppMessage *msg) {
    ReceiveUpdate(msg);
}

std::string AgentXmppChannel::ToString() const {
    return channel_->ToString();
}

void AgentXmppChannel::WriteReadyCb(const boost::system::error_code &ec) {
}

void AgentXmppChannel::CleanStale(AgentXmppChannel *peer, bool config, 
                                  bool unicast, bool multicast) {
    Agent *agent = peer->agent();
    if (config) {
       //Start a timer to flush off all old configs
       agent->GetIfMapAgentStaleCleaner()->
           StaleCleanup(AgentIfMapXmppChannel::GetSeqNumber());
    }

    if (unicast) {
       // Start Cleanup Timers on stale bgp-peer's
       agent->controller()->StartUnicastCleanupTimer(); 
    }

    if (multicast) {
       // Start Cleanup Timers on stale bgp-peer's
       agent->controller()->StartMulticastCleanupTimer(agent->controller()->
                                                       multicast_peer_identifier());
    }
}

void AgentXmppChannel::UnicastPeerDown(AgentXmppChannel *peer, 
                                       BgpPeer *peer_id, bool all_peer_gone) {
    VNController *vn_controller = peer->agent()->controller();
    if (peer->agent()->headless_agent_mode() && all_peer_gone) {
        //Enqueue stale marking of unicast & l2 routes
        vn_controller->CancelTimer(vn_controller->unicast_cleanup_timer());
        peer_id->StalePeerRoutes();
        return;
    }

    //Enqueue delete of unicast routes
    peer_id->DelPeerRoutes(boost::bind(
                         &VNController::ControllerPeerHeadlessAgentDelDone, 
                         peer->agent()->controller(), peer_id));
}

void AgentXmppChannel::MulticastPeerDown(AgentXmppChannel *peer, 
                                         bool all_peer_gone) {
    VNController *vn_controller = peer->agent()->controller();
    if (all_peer_gone) {
        if (peer->agent()->headless_agent_mode()) {
            vn_controller->CancelTimer(vn_controller->multicast_cleanup_timer());
            return;
        }
    }

    AgentXmppChannel::CleanStale(peer, false, false, true);
}

void AgentXmppChannel::HandleAgentXmppClientChannelEvent(AgentXmppChannel *peer,
                                                         xmps::PeerState state) {
    Agent *agent = peer->agent();
    if (state == xmps::READY) {

        //Ignore duplicate ready messages
        if (peer->bgp_peer_id() != NULL)
            return;

        // Create a new BgpPeer every-time a channel is UP from DOWN state
        peer->CreateBgpPeer();
        agent->SetAgentXmppChannelSetupTime(UTCTimestampUsec(), peer->
                                            GetXmppServerIdx());
        // Switch-over Config Control-node
        if (agent->GetXmppCfgServer().empty()) {
            if (ControllerSendCfgSubscribe(peer)) {
                agent->SetXmppCfgServer(peer->GetXmppServer(), peer->
                                        GetXmppServerIdx());
                //Generate a new sequence number for the configuration
                AgentIfMapXmppChannel::NewSeqNumber();
                AgentIfMapVmExport::NotifyAll(peer);
            } 
        }


        // Switch-over Multicast Tree Builder
        AgentXmppChannel *agent_mcast_builder = 
            agent->GetControlNodeMulticastBuilder();
        bool multicast_builder_changed = false;

        if (agent_mcast_builder == NULL) {
            agent->controller()->increment_multicast_peer_identifier();
            agent->SetControlNodeMulticastBuilder(peer);
            multicast_builder_changed = true;
        } else if (agent_mcast_builder != peer) {
            boost::system::error_code ec;
            IpAddress ip1 = ip::address::from_string(peer->GetXmppServer(),ec);
            IpAddress ip2 = ip::address::from_string(agent_mcast_builder->
                                                     GetXmppServer(),ec);
            if (ip1.to_v4().to_ulong() < ip2.to_v4().to_ulong()) {
                // Walk route-tables and send dissociate to older peer
                // for subnet and broadcast routes
                agent_mcast_builder->bgp_peer_id()->
                    PeerNotifyMulticastRoutes(false); 
                // Reset Multicast Tree Builder
                agent->controller()->increment_multicast_peer_identifier();
                agent->SetControlNodeMulticastBuilder(peer);
                multicast_builder_changed = true;
            }
        } 

        // Walk route-tables and notify unicast routes
        // and notify subnet and broadcast if TreeBuilder  
        peer->bgp_peer_id()->PeerNotifyRoutes();

        //Cleanup stales if any
        AgentXmppChannel::CleanStale(peer, agent->headless_agent_mode(), 
                                     agent->headless_agent_mode(), 
                                     multicast_builder_changed);

        agent->stats()->incr_xmpp_reconnects(peer->GetXmppServerIdx());
        CONTROLLER_TRACE(Session, peer->GetXmppServer(), "READY", 
                         agent->GetControlNodeMulticastBuilder()->GetBgpPeerName(),
                         "Peer elected Multicast Tree Builder"); 
    } else {
        //Ignore duplicate not-ready messages
        if (peer->bgp_peer_id() == NULL)
            return;

        BgpPeer *decommissioned_peer_id = peer->bgp_peer_id();
        // Add BgpPeer to global decommissioned list
        peer->DeCommissionBgpPeer();
        if (agent->controller()->GetActiveXmppConnections() >= 1) {
            AgentXmppChannel::UnicastPeerDown(peer, decommissioned_peer_id, 
                                              false);
        } else {
            AgentXmppChannel::UnicastPeerDown(peer, decommissioned_peer_id, 
                                              true);
        }

        // Switch-over Config Control-node
        if (agent->GetXmppCfgServer().compare(peer->GetXmppServer()) == 0) {
            //send cfg subscribe to other peer if exists
            uint8_t o_idx = ((agent->GetXmppCfgServerIdx() == 0) ? 1: 0);
            agent->ResetXmppCfgServer();
            AgentXmppChannel *new_cfg_peer = agent->GetAgentXmppChannel(o_idx);
            AgentIfMapXmppChannel::NewSeqNumber();
            if (new_cfg_peer && ControllerSendCfgSubscribe(new_cfg_peer)) {
                agent->SetXmppCfgServer(new_cfg_peer->GetXmppServer(),
                                        new_cfg_peer->GetXmppServerIdx());
                AgentIfMapVmExport::NotifyAll(new_cfg_peer);
            } else {
                //All cfg peers are gone, in headless agent cancel cleanup timer
                if (agent->headless_agent_mode())
                    peer->agent()->GetIfMapAgentStaleCleaner()->CancelCleanup();
            }  

            //Start a timer to flush off all old configs, in non headless mode
            if (!agent->headless_agent_mode()) {
                AgentXmppChannel::CleanStale(peer, true, false, false);
            }
        }

        // Switch-over Multicast Tree Builder
        AgentXmppChannel *agent_mcast_builder = 
            agent->GetControlNodeMulticastBuilder();
        if (agent_mcast_builder == peer) {
            uint8_t o_idx = ((agent_mcast_builder->GetXmppServerIdx() == 0) 
                             ? 1: 0);
            AgentXmppChannel *new_mcast_builder = 
                agent->GetAgentXmppChannel(o_idx);

            if (new_mcast_builder && (new_mcast_builder->GetXmppChannel()->
                GetPeerState() == xmps::READY) && 
                (new_mcast_builder->bgp_peer_id() != NULL)) {
                agent->controller()->increment_multicast_peer_identifier();
                agent->SetControlNodeMulticastBuilder(new_mcast_builder);
                AgentXmppChannel::MulticastPeerDown(peer, false);

                //Advertise subnet and all broadcast routes to
                //the new multicast tree builder
                new_mcast_builder->bgp_peer_id()->PeerNotifyMulticastRoutes(true); 

                CONTROLLER_TRACE(Session, peer->GetXmppServer(), "NOT_READY",
                                 agent->GetControlNodeMulticastBuilder()->
                                 GetBgpPeerName(), 
                                 "Peer elected Multicast Tree Builder"); 
            } else {
                // Artificially increment peer identifier so that entries can 
                // be deleted
                agent->controller()->increment_multicast_peer_identifier();
                agent->SetControlNodeMulticastBuilder(NULL);
                AgentXmppChannel::MulticastPeerDown(peer, true);
                CONTROLLER_TRACE(Session, peer->GetXmppServer(), "NOT_READY", 
                                 "NULL", "No elected Multicast Tree Builder"); 
            }
        } 
    }
}

bool AgentXmppChannel::ControllerSendVmCfgSubscribe(AgentXmppChannel *peer, 
                         const boost::uuids::uuid &vm_id, 
                         bool subscribe) {
    uint8_t data_[4096];
    size_t datalen_;

    if (!peer) {
        return false;
    }      
       
    //Build the DOM tree
    auto_ptr<XmlBase> impl(XmppStanza::AllocXmppXmlImpl());
    XmlPugi *pugi = reinterpret_cast<XmlPugi *>(impl.get());

    pugi->AddNode("iq", "");
    pugi->AddAttribute("type", "set");
    pugi->AddAttribute("from", peer->channel_->FromString());
    std::string to(peer->channel_->ToString());
    to += "/";
    to += XmppInit::kConfigPeer; 
    pugi->AddAttribute("to", to);

    pugi->AddChildNode("pubsub", "");
    pugi->AddAttribute("xmlns", "http://jabber.org/protocol/pubsub");
    if (subscribe == true) {
        pugi->AddChildNode("subscribe", "");
    } else {
        pugi->AddChildNode("unsubscribe", "");
    }
    std::string vm("virtual-machine:");
    stringstream vmid;
    vmid << vm_id;
    vm += vmid.str();
    pugi->AddAttribute("node", vm);


    datalen_ = XmppProto::EncodeMessage(impl.get(), data_, sizeof(data_));
    CONTROLLER_TRACE(Trace, peer->GetBgpPeerName(), "",
              std::string(reinterpret_cast<const char *>(data_), datalen_));
    // send data
    return (peer->SendUpdate(data_,datalen_));
        
    return true;
}

bool AgentXmppChannel::ControllerSendCfgSubscribe(AgentXmppChannel *peer) {

    uint8_t data_[4096];
    size_t datalen_;

    if (!peer) {
        return false;
    }      
       
    //Build the DOM tree
    auto_ptr<XmlBase> impl(XmppStanza::AllocXmppXmlImpl());
    XmlPugi *pugi = reinterpret_cast<XmlPugi *>(impl.get());

    pugi->AddNode("iq", "");
    pugi->AddAttribute("type", "set");
    pugi->AddAttribute("from", peer->channel_->FromString());
    std::string to(peer->channel_->ToString());
    to += "/";
    to += XmppInit::kConfigPeer; 
    pugi->AddAttribute("to", to);

    pugi->AddChildNode("pubsub", "");
    pugi->AddAttribute("xmlns", "http://jabber.org/protocol/pubsub");
    pugi->AddChildNode("subscribe", "");
    string node("virtual-router:");
    node  = node + XmppInit::kFqnPrependAgentNodeJID  + peer->channel_->FromString();
    pugi->AddAttribute("node", node); 

    datalen_ = XmppProto::EncodeMessage(impl.get(), data_, sizeof(data_));
    CONTROLLER_TRACE(Trace, peer->GetBgpPeerName(), "",
            std::string(reinterpret_cast<const char *>(data_), datalen_));
    // send data
    return (peer->SendUpdate(data_,datalen_));
}

bool AgentXmppChannel::ControllerSendSubscribe(AgentXmppChannel *peer, 
                                               VrfEntry *vrf,
                                               bool subscribe) {
    static int req_id = 0;
    uint8_t data_[4096];
    size_t datalen_;

    if (!peer) {
        return false;
    }      
       
    //Build the DOM tree
    auto_ptr<XmlBase> impl(XmppStanza::AllocXmppXmlImpl());
    XmlPugi *pugi = reinterpret_cast<XmlPugi *>(impl.get());

    pugi->AddNode("iq", "");
    pugi->AddAttribute("type", "set");
    pugi->AddAttribute("from", peer->channel_->FromString());
    std::string to(peer->channel_->ToString());
    to += "/";
    to += XmppInit::kBgpPeer; 
    pugi->AddAttribute("to", to);

    stringstream request_id;
    request_id << "subscribe" << req_id++;
    pugi->AddAttribute("id", request_id.str()); 
    pugi->AddChildNode("pubsub", "");
    pugi->AddAttribute("xmlns", "http://jabber.org/protocol/pubsub");
    if (subscribe) {
        pugi->AddChildNode("subscribe", "");
    } else {
        pugi->AddChildNode("unsubscribe", "");
    }
    pugi->AddAttribute("node", vrf->GetName());
    pugi->AddChildNode("options", "" );
    stringstream vrf_id;
    vrf_id << vrf->vrf_id();
    pugi->AddChildNode("instance-id", vrf_id.str());

    datalen_ = XmppProto::EncodeMessage(impl.get(), data_, sizeof(data_));
    
    // send data
    return (peer->SendUpdate(data_,datalen_));
}

bool AgentXmppChannel::ControllerSendV4UnicastRoute(AgentXmppChannel *peer,
                                               AgentRoute *route, 
                                               std::string vn, 
                                               const SecurityGroupList *sg_list,
                                               uint32_t mpls_label,
                                               TunnelType::TypeBmap bmap,
                                               bool add_route) {

    static int id = 0;
    ItemType item;
    uint8_t data_[4096];
    size_t datalen_;
   
    if (!peer) return false;

    //Build the DOM tree
    auto_ptr<XmlBase> impl(XmppStanza::AllocXmppXmlImpl());
    XmlPugi *pugi = reinterpret_cast<XmlPugi *>(impl.get());

    item.entry.nlri.af = BgpAf::IPv4; 
    item.entry.nlri.safi = BgpAf::Unicast; 
    stringstream rstr;
    rstr << route->ToString();
    item.entry.nlri.address = rstr.str();

    string rtr(peer->agent()->GetRouterId().to_string());

    autogen::NextHopType nh;
    nh.af = BgpAf::IPv4;
    nh.address = rtr;
    nh.label = mpls_label;
    if (bmap & TunnelType::GREType()) {
        nh.tunnel_encapsulation_list.tunnel_encapsulation.push_back("gre");
    }
    if (bmap & TunnelType::UDPType()) {
        nh.tunnel_encapsulation_list.tunnel_encapsulation.push_back("udp");
    }
    item.entry.next_hops.next_hop.push_back(nh);

    if (sg_list && sg_list->size()) {
        item.entry.security_group_list.security_group = *sg_list;
    }

    item.entry.version = 1; //TODO
    item.entry.virtual_network = vn;
   
    pugi->AddNode("iq", "");
    pugi->AddAttribute("type", "set");
    
    pugi->AddAttribute("from", peer->channel_->FromString());
    std::string to(peer->channel_->ToString());
    to += "/";
    to += XmppInit::kBgpPeer; 
    pugi->AddAttribute("to", to);

    stringstream pubsub_id;
    pubsub_id << "pubsub" << id++;
    pugi->AddAttribute("id", pubsub_id.str()); 

    pugi->AddChildNode("pubsub", "");
    pugi->AddAttribute("xmlns", "http://jabber.org/protocol/pubsub");
    pugi->AddChildNode("publish", "");

    //Catering for inet4 and evpn unicast routes
    stringstream ss_node;
    ss_node << item.entry.nlri.af << "/" 
            << item.entry.nlri.safi << "/" 
            << route->vrf()->GetName() << "/" 
            << route->GetAddressString();
    std::string node_id(ss_node.str());
    pugi->AddAttribute("node", node_id);
    pugi->AddChildNode("item", "");

    pugi::xml_node node = pugi->FindNode("item");

    //Call Auto-generated Code to encode the struct
    item.Encode(&node);

    datalen_ = XmppProto::EncodeMessage(impl.get(), data_, sizeof(data_));
    // send data
    peer->SendUpdate(data_,datalen_);

    pugi->DeleteNode("pubsub");
    pugi->ReadNode("iq");

    stringstream collection_id;
    collection_id << "collection" << id++;
    pugi->ModifyAttribute("id", collection_id.str()); 
    pugi->AddChildNode("pubsub", "");
    pugi->AddAttribute("xmlns", "http://jabber.org/protocol/pubsub");
    pugi->AddChildNode("collection", "");

    pugi->AddAttribute("node", route->vrf()->GetName());
    if (add_route) {
        pugi->AddChildNode("associate", "");
    } else {
        pugi->AddChildNode("dissociate", "");
    }
    pugi->AddAttribute("node", node_id);

    datalen_ = XmppProto::EncodeMessage(impl.get(), data_, sizeof(data_));
    // send data
    return (peer->SendUpdate(data_,datalen_));
}

bool AgentXmppChannel::ControllerSendEvpnRoute(AgentXmppChannel *peer,
                                               AgentRoute *route, 
                                               std::string vn, 
                                               uint32_t label,
                                               uint32_t tunnel_bmap,
                                               bool add_route) {
    static int id = 0;
    EnetItemType item;
    uint8_t data_[4096];
    size_t datalen_;
   
    if (!peer) return false;

    //Build the DOM tree
    auto_ptr<XmlBase> impl(XmppStanza::AllocXmppXmlImpl());
    XmlPugi *pugi = reinterpret_cast<XmlPugi *>(impl.get());

    //TODO remove hardcoding
    item.entry.nlri.af = 25; 
    item.entry.nlri.safi = 242; 
    stringstream rstr;
    rstr << route->ToString();
    item.entry.nlri.mac = rstr.str();
    Layer2RouteEntry *l2_route = static_cast<Layer2RouteEntry *>(route);
    rstr.str("");
    rstr << l2_route->GetVmIpAddress().to_string() << "/" 
        << l2_route->GetVmIpPlen();
    item.entry.nlri.address = rstr.str();
    assert(item.entry.nlri.address != "0.0.0.0");

    string rtr(peer->agent()->GetRouterId().to_string());

    autogen::EnetNextHopType nh;
    nh.af = Address::INET;
    nh.address = rtr;
    nh.label = label;

    TunnelType::Type tunnel_type = TunnelType::ComputeType(tunnel_bmap);
    if (l2_route->GetActivePath()) {
        tunnel_type = l2_route->GetActivePath()->tunnel_type();
    }
    if (tunnel_type != TunnelType::VXLAN) {
        if (tunnel_bmap & TunnelType::GREType()) {
            nh.tunnel_encapsulation_list.tunnel_encapsulation.push_back("gre");
        }
        if (tunnel_bmap & TunnelType::UDPType()) {
            nh.tunnel_encapsulation_list.tunnel_encapsulation.push_back("udp");
        }
    } else {
        nh.tunnel_encapsulation_list.tunnel_encapsulation.push_back("vxlan");
    }

    item.entry.next_hops.next_hop.push_back(nh);

    //item.entry.version = 1; //TODO
    //item.entry.virtual_network = vn;
   
    pugi->AddNode("iq", "");
    pugi->AddAttribute("type", "set");
    
    pugi->AddAttribute("from", peer->channel_->FromString());
    std::string to(peer->channel_->ToString());
    to += "/";
    to += XmppInit::kBgpPeer; 
    pugi->AddAttribute("to", to);

    stringstream pubsub_id;
    pubsub_id << "pubsub" << id++;
    pugi->AddAttribute("id", pubsub_id.str()); 

    pugi->AddChildNode("pubsub", "");
    pugi->AddAttribute("xmlns", "http://jabber.org/protocol/pubsub");
    pugi->AddChildNode("publish", "");

    stringstream ss_node;
    ss_node << item.entry.nlri.af << "/" << item.entry.nlri.safi << "/" 
        << route->GetAddressString() << "," << item.entry.nlri.address; 
    std::string node_id(ss_node.str());
    pugi->AddAttribute("node", node_id);
    pugi->AddChildNode("item", "");

    pugi::xml_node node = pugi->FindNode("item");

    //Call Auto-generated Code to encode the struct
    item.Encode(&node);

    datalen_ = XmppProto::EncodeMessage(impl.get(), data_, sizeof(data_));
    // send data
    peer->SendUpdate(data_,datalen_);

    pugi->DeleteNode("pubsub");
    pugi->ReadNode("iq");

    stringstream collection_id;
    collection_id << "collection" << id++;
    pugi->ModifyAttribute("id", collection_id.str()); 
    pugi->AddChildNode("pubsub", "");
    pugi->AddAttribute("xmlns", "http://jabber.org/protocol/pubsub");
    pugi->AddChildNode("collection", "");

    pugi->AddAttribute("node", route->vrf()->GetName());
    if (add_route) {
        pugi->AddChildNode("associate", "");
    } else {
        pugi->AddChildNode("dissociate", "");
    }
    pugi->AddAttribute("node", node_id);

    datalen_ = XmppProto::EncodeMessage(impl.get(), data_, sizeof(data_));
    // send data
    return (peer->SendUpdate(data_,datalen_));
}

bool AgentXmppChannel::ControllerSendRoute(AgentXmppChannel *peer,
                                           AgentRoute *route, 
                                           std::string vn, 
                                           uint32_t label,
                                           TunnelType::TypeBmap bmap,
                                           const SecurityGroupList *sg_list,
                                           bool add_route,
                                           Agent::RouteTableType type)
{
    bool ret = false;
    if (type == Agent::INET4_UNICAST) {
        ret = AgentXmppChannel::ControllerSendV4UnicastRoute(peer, route, vn,
                                          sg_list, label, bmap, add_route);
    } 
    if (type == Agent::LAYER2) {
        ret = AgentXmppChannel::ControllerSendEvpnRoute(peer, route, vn, 
                                         label, bmap, add_route);
    } 
    return ret;
}

bool AgentXmppChannel::ControllerSendMcastRoute(AgentXmppChannel *peer,
                                                AgentRoute *route, 
                                                bool add_route) {

    static int id = 0;
    autogen::McastItemType item;
    uint8_t data_[4096];
    size_t datalen_;
   
    if (!peer) return false;
    if (add_route && (peer->agent()->GetControlNodeMulticastBuilder() != peer)) {
        CONTROLLER_TRACE(Trace, peer->GetBgpPeerName(),
                         route->vrf()->GetName(),
                         "Peer not elected Multicast Tree Builder");
        return false;
    }

    CONTROLLER_TRACE(McastSubscribe, peer->GetBgpPeerName(),
                     route->vrf()->GetName(), " ",
                     route->ToString());

    //Build the DOM tree
    auto_ptr<XmlBase> impl(XmppStanza::AllocXmppXmlImpl());
    XmlPugi *pugi = reinterpret_cast<XmlPugi *>(impl.get());

    item.entry.nlri.af = BgpAf::IPv4; 
    item.entry.nlri.safi = BgpAf::Mcast; 
    item.entry.nlri.group = route->GetAddressString();
    item.entry.nlri.source = "0.0.0.0";

    autogen::McastNextHopType item_nexthop;
    item_nexthop.af = BgpAf::IPv4;
    string rtr(peer->agent()->GetRouterId().to_string());
    item_nexthop.address = rtr;
    item_nexthop.label = peer->GetMcastLabelRange();
    item_nexthop.tunnel_encapsulation_list.tunnel_encapsulation.push_back("gre");
    item_nexthop.tunnel_encapsulation_list.tunnel_encapsulation.push_back("udp");
    item.entry.next_hops.next_hop.push_back(item_nexthop);

    //Build the pugi tree
    pugi->AddNode("iq", "");
    pugi->AddAttribute("type", "set");
    pugi->AddAttribute("from", peer->channel_->FromString());
    std::string to(peer->channel_->ToString());
    to += "/";
    to += XmppInit::kBgpPeer; 
    pugi->AddAttribute("to", to);

    std::string pubsub_id("pubsub_b");
    stringstream str_id;
    str_id << id;
    pubsub_id += str_id.str();
    pugi->AddAttribute("id", pubsub_id); 

    pugi->AddChildNode("pubsub", "");
    pugi->AddAttribute("xmlns", "http://jabber.org/protocol/pubsub");
    pugi->AddChildNode("publish", "");
    stringstream ss_node;
    ss_node << item.entry.nlri.af << "/" 
            << item.entry.nlri.safi << "/" 
            << route->vrf()->GetName() << "/" 
            << route->GetAddressString();
    std::string node_id(ss_node.str());
    pugi->AddAttribute("node", node_id);
    pugi->AddChildNode("item", "");

    pugi::xml_node node = pugi->FindNode("item");

    //Call Auto-generated Code to encode the struct
    item.Encode(&node);

    datalen_ = XmppProto::EncodeMessage(impl.get(), data_, sizeof(data_));
    // send data
    peer->SendUpdate(data_,datalen_);


    pugi->DeleteNode("pubsub");
    pugi->ReadNode("iq");

    stringstream collection_id;
    collection_id << "collection" << id++;
    pugi->ModifyAttribute("id", collection_id.str()); 
    pugi->AddChildNode("pubsub", "");
    pugi->AddAttribute("xmlns", "http://jabber.org/protocol/pubsub");
    pugi->AddChildNode("collection", "");

    pugi->AddAttribute("node", route->vrf()->GetName());
    if (add_route) {
        pugi->AddChildNode("associate", "");
    } else {
        pugi->AddChildNode("dissociate", "");
    }
    pugi->AddAttribute("node", node_id); 

    datalen_ = XmppProto::EncodeMessage(impl.get(), data_, sizeof(data_));
    // send data
    return (peer->SendUpdate(data_,datalen_));
}
